#include "framework.h"
#include "Evidence.h"
#include "ActivityStore.h"
#include "Attention.h"
#include "EntityKey.h"
#include "EntityStore.h"
#include "Json.h"
#include "sqlite3.h"

#include <objbase.h>
#include <bcrypt.h>
#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>
#pragma comment(lib, "bcrypt.lib")

namespace
{
    struct Statement
    {
        sqlite3_stmt* p = nullptr;
        Statement(sqlite3* db, const char* sql)
        {
            if (sqlite3_prepare_v2(db, sql, -1, &p, nullptr) != SQLITE_OK)
                throw std::runtime_error(sqlite3_errmsg(db));
        }
        ~Statement()
        {
            sqlite3_finalize(p);
        }
        operator sqlite3_stmt*() const
        {
            return p;
        }
    };
    void Text(sqlite3_stmt* s, int i, const std::string& v)
    {
        sqlite3_bind_text(s, i, v.c_str(), (int)v.size(), SQLITE_TRANSIENT);
    }
    std::string Text(sqlite3_stmt* s, int i)
    {
        auto p = sqlite3_column_text(s, i);
        return p ? reinterpret_cast<const char*>(p) : "";
    }
    void Done(sqlite3* db, sqlite3_stmt* s)
    {
        if (!s || sqlite3_step(s) != SQLITE_DONE)
            throw std::runtime_error(sqlite3_errmsg(db));
    }
    void Exec(sqlite3* db, const std::string& sql)
    {
        if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr) != SQLITE_OK)
            throw std::runtime_error(sqlite3_errmsg(db));
    }
    int64_t Scalar(sqlite3* db, const char* sql)
    {
        Statement s(db, sql);
        return sqlite3_step(s) == SQLITE_ROW ? sqlite3_column_int64(s, 0) : 0;
    }
    int64_t ClipMs(int64_t total, int64_t start, int64_t end, int64_t a, int64_t b)
    {
        if (total < 0)
            return -1;
        if (end <= start || b <= a)
            return 0;
        const double rate = (std::min)(1.0, double(total) / double(end - start));
        return (int64_t)std::floor((b - start) * rate + 1e-8) - (int64_t)std::floor((a - start) * rate + 1e-8);
    }
} // namespace

namespace Evidence
{
    std::string Fingerprint(const std::string& value)
    {
        BCRYPT_ALG_HANDLE algorithm = nullptr;
        BCRYPT_HASH_HANDLE hash = nullptr;
        bool ok = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0;
        if (ok)
            ok = BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) >= 0;
        if (ok)
            ok = BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(value.data())), (ULONG)value.size(),
                                0) >= 0;
        unsigned char digest[32]{};
        if (ok)
            ok = BCryptFinishHash(hash, digest, sizeof(digest), 0) >= 0;
        if (hash)
            BCryptDestroyHash(hash);
        if (algorithm)
            BCryptCloseAlgorithmProvider(algorithm, 0);
        if (!ok)
            throw std::runtime_error("cannot fingerprint observation identity");
        static const char hex[] = "0123456789abcdef";
        std::string result;
        for (auto byte : digest)
        {
            result += hex[byte >> 4];
            result += hex[byte & 15];
        }
        return result;
    }

    int64_t NowMs()
    {
        return StoreUtil::NowMs();
    }

    std::string NewId(const char* prefix)
    {
        GUID id{};
        if (FAILED(CoCreateGuid(&id)))
            throw std::runtime_error("cannot allocate evidence identity");
        wchar_t value[40]{};
        StringFromGUID2(id, value, 40);
        return std::string(prefix) + ":" + WarpJson::WideToUtf8(value);
    }

    const std::string& RuntimeId()
    {
        static const std::string id = NewId("run");
        return id;
    }

    std::string FileSha256(const std::wstring& path)
    {
        HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                  FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return {};
        BCRYPT_ALG_HANDLE algorithm = nullptr;
        BCRYPT_HASH_HANDLE hash = nullptr;
        bool ok = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0;
        if (ok)
            ok = BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) >= 0;
        std::vector<unsigned char> buffer(65536);
        while (ok)
        {
            DWORD count = 0;
            if (!ReadFile(file, buffer.data(), (DWORD)buffer.size(), &count, nullptr))
            {
                ok = false;
                break;
            }
            if (!count)
                break;
            ok = BCryptHashData(hash, buffer.data(), count, 0) >= 0;
        }
        unsigned char digest[32]{};
        if (ok)
            ok = BCryptFinishHash(hash, digest, sizeof(digest), 0) >= 0;
        if (hash)
            BCryptDestroyHash(hash);
        if (algorithm)
            BCryptCloseAlgorithmProvider(algorithm, 0);
        CloseHandle(file);
        if (!ok)
            return {};
        static const char hex[] = "0123456789abcdef";
        std::string out;
        for (auto byte : digest)
        {
            out += hex[byte >> 4];
            out += hex[byte & 15];
        }
        return out;
    }

    void Prepare(ObservationEvidence& e)
    {
        if (e.id.empty())
            e.id = e.episodeId.empty() ? NewId("event") : e.episodeId + ":" + std::to_string(e.revision);
        if (e.sourceInstance.empty())
            e.sourceInstance = RuntimeId();
        if (!e.receivedMs)
            e.receivedMs = NowMs();
        if (!e.observedMs)
            e.observedMs = e.receivedMs;
        e.supportCount = (std::max)(1, (std::min)(1000000, e.supportCount));
        if (e.replayed)
        {
            e.actorMethod = "unknown";
            e.actorQuality = -1;
            e.attributionAgeMs = -1;
        }
    }

    void Write(WarpJson::Writer& w, const ObservationEvidence& e)
    {
        w.BeginObject()
            .KV("event_id", e.id)
            .KV("available", !e.id.empty())
            .KV("source_instance", e.sourceInstance)
            .KV("observed_ms", e.observedMs)
            .KV("received_ms", e.receivedMs)
            .KV("timestamp_kind", e.timestampKind)
            .KV("replayed", e.replayed)
            .KV("confirmation", e.confirmation)
            .KV("actor_method", e.actorMethod)
            .KV("attribution_age_ms", e.attributionAgeMs)
            .KV("context_age_ms", e.contextAgeMs)
            .KV("resolution_observed_ms", e.resolutionObservedMs)
            .KV("resolution_valid_until_ms", e.resolutionValidUntilMs)
            .KV("process_instance", e.processInstance)
            .KV("foreground_instance", e.foregroundInstance)
            .KV("session_id", e.sessionId)
            .KV("resolution", e.resolution)
            .KV("redaction", e.redaction)
            .KV("rule_version", e.id.empty() ? "legacy_unknown" : RuleVersion)
            .KV("support_count", e.supportCount);
        w.Key("observed_utc_offset_minutes");
        if (e.utcOffsetMinutes == INT32_MIN)
            w.Null();
        else
            w.Int(e.utcOffsetMinutes);
        auto quality = [&](const char* key, double value) {
            w.Key(key);
            if (value < 0 || !std::isfinite(value))
                w.Null();
            else
                w.Double((std::min)(1.0, value));
        };
        quality("source_quality", e.sourceQuality);
        quality("actor_quality", e.actorQuality);
        quality("identity_quality", e.identityQuality);
        if (!e.volumeId.empty())
            w.KV("volume_id", e.volumeId);
        if (!e.fileId.empty())
            w.KV("file_id", e.fileId);
        if (!e.journalId.empty())
            w.KV("journal_id", e.journalId);
        if (!e.sourceRecordId.empty())
            w.KV("source_record_id", e.sourceRecordId);
        if (!e.originApp.empty())
            w.KV("origin_app", e.originApp);
        if (!e.cloudUrl.empty())
            w.KV("cloud_url", e.cloudUrl);
        if (!e.artifactId.empty())
            w.KV("artifact_id", e.artifactId).KV("identity_basis", e.identityBasis);
        if (!e.windowInstance.empty())
            w.KV("window_instance", e.windowInstance);
        if (!e.episodeId.empty())
            w.KV("episode_id", e.episodeId)
                .KV("revision", e.revision)
                .KV("open", e.open)
                .KV("boundary_reason", e.boundaryReason);
        w.EndObject();
    }

    std::string Json(const ObservationEvidence& e)
    {
        std::string out;
        WarpJson::Writer w(out);
        Write(w, e);
        return out;
    }

    ObservationEvidence Parse(const std::string& json)
    {
        ObservationEvidence e;
        auto p = WarpJson::Parse(json);
        if (!p.ok)
            return e;
        const auto& v = p.value;
        e.id = v.get("event_id").asString();
        e.sourceInstance = v.get("source_instance").asString();
        e.observedMs = v.get("observed_ms").asInt64();
        e.receivedMs = v.get("received_ms").asInt64();
        e.timestampKind = v.get("timestamp_kind").asString("unknown");
        e.replayed = v.get("replayed").asBool();
        e.confirmation = v.get("confirmation").asString("unknown");
        e.actorMethod = v.get("actor_method").asString("unknown");
        e.attributionAgeMs = v.get("attribution_age_ms").asInt64(-1);
        e.contextAgeMs = v.get("context_age_ms").asInt64(-1);
        e.resolutionObservedMs = v.get("resolution_observed_ms").asInt64();
        e.resolutionValidUntilMs = v.get("resolution_valid_until_ms").asInt64();
        e.processInstance = v.get("process_instance").asString();
        e.foregroundInstance = v.get("foreground_instance").asString();
        e.sessionId = (int)v.get("session_id").asInt64(-1);
        e.resolution = v.get("resolution").asString("unknown");
        e.redaction = v.get("redaction").asString("none");
        e.supportCount = (int)v.get("support_count").asInt64(1);
        e.utcOffsetMinutes = (int)v.get("observed_utc_offset_minutes").asInt64(INT32_MIN);
        e.sourceQuality = v.get("source_quality").asDouble(-1);
        e.actorQuality = v.get("actor_quality").asDouble(-1);
        e.identityQuality = v.get("identity_quality").asDouble(-1);
        e.volumeId = v.get("volume_id").asString();
        e.fileId = v.get("file_id").asString();
        e.journalId = v.get("journal_id").asString();
        e.sourceRecordId = v.get("source_record_id").asString();
        e.originApp = v.get("origin_app").asString();
        e.cloudUrl = v.get("cloud_url").asString();
        e.artifactId = v.get("artifact_id").asString();
        e.identityBasis = v.get("identity_basis").asString();
        e.episodeId = v.get("episode_id").asString();
        e.windowInstance = v.get("window_instance").asString();
        e.revision = (int)v.get("revision").asInt64();
        e.open = v.get("open").asBool();
        e.boundaryReason = v.get("boundary_reason").asString();
        return e;
    }

    void Write(WarpJson::Writer& w, const EpisodeRecord& e)
    {
        w.BeginObject()
            .KV("episode_id", e.id)
            .KV("revision", e.revision)
            .KV("kind", e.kind)
            .KV("entity_key", e.entityKey)
            .KV("app_key", e.appKey)
            .KV("display", e.display)
            .KV("title", e.title)
            .KV("start_ms", e.startMs)
            .KV("end_ms", e.endMs)
            .KV("open", e.open)
            .KV("window_instance", e.windowInstance)
            .KV("process_instance", e.processInstance)
            .KV("resolution", e.resolution)
            .KV("state", e.state)
            .KV("boundary_reason", e.boundaryReason)
            .KV("exposure_ms", e.exposureMs)
            .KV("unknown_ms", e.unknownMs);
        auto nullable = [&](const char* key, int64_t value) {
            w.Key(key);
            if (value < 0)
                w.Null();
            else
                w.Int(value);
        };
        nullable("active_ms", e.activeMs);
        nullable("passive_ms", e.passiveMs);
        nullable("idle_ms", e.idleMs);
        w.Key("provenance");
        Write(w, e.evidence);
        w.EndObject();
    }

    void Write(WarpJson::Writer& w, const SourceCoverage& c)
    {
        w.BeginObject()
            .KV("source", c.source)
            .KV("source_instance", c.instance)
            .KV("state", c.state)
            .KV("reason", c.reason)
            .KV("start_ms", c.startMs)
            .KV("end_ms", c.endMs)
            .KV("dropped", c.dropped)
            .EndObject();
    }
} // namespace Evidence

bool ActivityStore::MigrateV4()
{
    if (Scalar(m_wdb, "PRAGMA user_version") >= 4)
        return true;
    try
    {
        Exec(m_wdb, "BEGIN IMMEDIATE");
        Exec(m_wdb,
             "CREATE TABLE IF NOT EXISTS observation (id TEXT PRIMARY KEY,kind TEXT NOT NULL,row_id INTEGER NOT NULL,"
             "event_ms INTEGER NOT NULL,received_ms INTEGER NOT NULL,payload TEXT NOT NULL,change_seq INTEGER NOT NULL,"
             "UNIQUE(kind,row_id)) WITHOUT ROWID;"
             "CREATE INDEX IF NOT EXISTS idx_observation_time ON observation(kind,event_ms);"
             "CREATE TABLE IF NOT EXISTS activity_episode (episode_id TEXT NOT NULL,revision INTEGER NOT NULL,kind "
             "TEXT NOT NULL,"
             "start_ms INTEGER NOT NULL,end_ms INTEGER NOT NULL,entity_key TEXT NOT NULL,app_key TEXT NOT NULL,"
             "display TEXT NOT NULL,title TEXT NOT NULL,exposure_ms INTEGER NOT NULL,active_ms INTEGER,passive_ms "
             "INTEGER,idle_ms INTEGER,"
             "unknown_ms INTEGER NOT NULL,state TEXT NOT NULL,raw_id INTEGER NOT NULL,received_ms INTEGER NOT "
             "NULL,payload TEXT NOT NULL,"
             "PRIMARY KEY(episode_id,revision)) WITHOUT ROWID;"
             "CREATE INDEX IF NOT EXISTS idx_episode_interval ON activity_episode(end_ms,start_ms);"
             "CREATE INDEX IF NOT EXISTS idx_episode_raw ON activity_episode(kind,raw_id);"
             "CREATE TABLE IF NOT EXISTS source_coverage (source TEXT NOT NULL,instance TEXT NOT NULL,start_ms INTEGER "
             "NOT NULL,"
             "end_ms INTEGER NOT NULL,state TEXT NOT NULL,reason TEXT NOT NULL,dropped INTEGER NOT NULL,received_ms "
             "INTEGER NOT NULL,"
             "PRIMARY KEY(source,instance,start_ms)) WITHOUT ROWID;"
             "CREATE INDEX IF NOT EXISTS idx_coverage_interval ON source_coverage(end_ms,start_ms);"
             "CREATE TABLE IF NOT EXISTS artifact_alias (path_key TEXT NOT NULL,from_ms INTEGER NOT NULL,to_ms INTEGER "
             "NOT NULL DEFAULT 0,"
             "artifact_id TEXT NOT NULL,physical_id TEXT NOT NULL,basis TEXT NOT NULL,evidence_id TEXT NOT NULL,"
             "PRIMARY KEY(path_key,from_ms)) WITHOUT ROWID;"
             "CREATE INDEX IF NOT EXISTS idx_artifact_id ON artifact_alias(artifact_id);"
             "CREATE INDEX IF NOT EXISTS idx_artifact_physical ON artifact_alias(physical_id,to_ms);"
             "CREATE INDEX IF NOT EXISTS idx_artifact_evidence ON artifact_alias(evidence_id);"
             "CREATE TABLE IF NOT EXISTS privacy_scope(kind INTEGER NOT NULL,scope_hash TEXT NOT NULL,from_ms INTEGER "
             "NOT NULL,"
             "to_ms INTEGER NOT NULL,PRIMARY KEY(kind,scope_hash)) WITHOUT ROWID;");
        const char* tables[] = {"file_activity", "app_launch_activity", "browsing_activity", "app_focus_activity",
                                "presence_interval"};
        const char* kinds[] = {"file", "launch", "browse", "focus", "presence"};
        for (int i = 0; i < 5; ++i)
        {
            const std::string kind = kinds[i], table = tables[i];
            Exec(m_wdb, "CREATE TRIGGER IF NOT EXISTS evidence_delete_" + kind + " AFTER DELETE ON " + table +
                            " BEGIN DELETE FROM activity_episode WHERE kind='" + kind +
                            "' AND raw_id=old.id;"
                            "DELETE FROM artifact_alias WHERE evidence_id IN (SELECT id FROM observation WHERE kind='" +
                            kind +
                            "' AND row_id=old.id);"
                            "DELETE FROM observation WHERE kind='" +
                            kind + "' AND row_id=old.id; END;");
        }
        Exec(m_wdb, "INSERT OR IGNORE INTO meta(key,value) VALUES('evidence_available_from','" +
                        std::to_string(Evidence::NowMs()) + "'); PRAGMA user_version=4; COMMIT;");
        return true;
    }
    catch (...)
    {
        sqlite3_exec(m_wdb, "ROLLBACK", nullptr, nullptr, nullptr);
        return false;
    }
}

bool ActivityStore::EvidenceExists(WriterContext& w, const ObservationEvidence& e)
{
    if (e.id.empty())
        return false;
    auto st = w.Stmt("SELECT 1 FROM observation WHERE id=?1");
    Text(st, 1, e.id);
    const bool exists = sqlite3_step(st) == SQLITE_ROW;
    sqlite3_reset(st);
    return exists;
}

void ActivityStore::PersistEvidence(WriterContext& w, const char* kind, int64_t rowId, int64_t eventMs,
                                    const ObservationEvidence& e)
{
    auto st =
        w.Stmt("INSERT INTO observation(id,kind,row_id,event_ms,received_ms,payload,change_seq) VALUES(?,?,?,?,?,?,?)");
    Text(st, 1, e.id);
    Text(st, 2, kind);
    sqlite3_bind_int64(st, 3, rowId);
    sqlite3_bind_int64(st, 4, eventMs);
    sqlite3_bind_int64(st, 5, e.receivedMs);
    Text(st, 6, Evidence::Json(e));
    sqlite3_bind_int64(st, 7, w.NextChangeSeq());
    Done(w.db, st);
}

void ActivityStore::PersistEpisode(WriterContext& w, const EpisodeRecord& r, int64_t rowId)
{
    if (r.id.empty() || r.endMs < r.startMs)
        return;
    auto st =
        w.Stmt("INSERT OR IGNORE INTO activity_episode(episode_id,revision,kind,start_ms,end_ms,entity_key,app_key,"
               "display,title,exposure_ms,active_ms,passive_ms,idle_ms,unknown_ms,state,raw_id,received_ms,payload)"
               "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
    Text(st, 1, r.id);
    sqlite3_bind_int(st, 2, r.revision);
    Text(st, 3, r.kind);
    sqlite3_bind_int64(st, 4, r.startMs);
    sqlite3_bind_int64(st, 5, r.endMs);
    Text(st, 6, r.entityKey);
    Text(st, 7, r.appKey);
    Text(st, 8, StoreUtil::W2U(r.display));
    Text(st, 9, StoreUtil::W2U(r.title));
    sqlite3_bind_int64(st, 10, r.exposureMs);
    const int64_t values[] = {r.activeMs, r.passiveMs, r.idleMs};
    for (int i = 0; i < 3; ++i)
    {
        if (values[i] < 0)
            sqlite3_bind_null(st, 11 + i);
        else
            sqlite3_bind_int64(st, 11 + i, values[i]);
    }
    sqlite3_bind_int64(st, 14, r.unknownMs);
    Text(st, 15, r.state);
    sqlite3_bind_int64(st, 16, rowId);
    sqlite3_bind_int64(st, 17, r.evidence.receivedMs);
    Text(st, 18, Evidence::Json(r.evidence));
    Done(w.db, st);
    if (sqlite3_changes(w.db))
        w.NextChangeSeq();
}

bool ActivityStore::RecordPageEpisode(EpisodeRecord r)
{
    if (r.id.empty() || r.endMs < r.startMs || r.revision < 0)
        return false;
    r.exposureMs = (std::max)(int64_t(0), (std::min)(r.endMs - r.startMs, r.exposureMs));
    int64_t remaining = r.exposureMs;
    for (auto component : {&r.activeMs, &r.passiveMs, &r.idleMs})
        if (*component >= 0)
        {
            *component = (std::min)(remaining, *component);
            remaining -= *component;
        }
    r.unknownMs = remaining;
    if (r.evidence.redaction != "none")
    {
        r.entityKey.clear();
        r.display.clear();
        r.title.clear();
        r.resolution = "withheld";
    }
    r.kind = "page";
    Evidence::Prepare(r.evidence);
    r.evidence.episodeId = r.id;
    r.evidence.revision = r.revision;
    r.evidence.open = r.open;
    r.evidence.boundaryReason = r.boundaryReason;
    r.evidence.windowInstance = r.windowInstance;
    r.evidence.resolution = r.resolution;
    return EnqueueRecord([this, r = std::move(r)](WriterContext& w) {
        Statement existing(w.db, "SELECT MAX(end_ms),MAX(revision) FROM activity_episode WHERE episode_id=?1");
        Text(existing, 1, r.id);
        if (sqlite3_step(existing) == SQLITE_ROW && sqlite3_column_type(existing, 0) != SQLITE_NULL &&
            (sqlite3_column_int(existing, 1) >= r.revision || sqlite3_column_int64(existing, 0) > r.startMs))
            return;
        RecordIds ids;
        ids.key = r.entityKey;
        const auto scheme = r.display.find(L"://");
        if (scheme != std::wstring::npos)
        {
            const auto end = r.display.find_first_of(L"/?#", scheme + 3);
            ids.host = EntityKey::Key(r.display.substr(scheme + 3, end == std::wstring::npos ? end : end - scheme - 3));
        }
        auto parts = UnpurgedParts(r.startMs, r.endMs, ids);
        for (const auto& part : parts)
        {
            EpisodeRecord p = r;
            p.startMs = part.first;
            p.endMs = part.second;
            if (parts.size() != 1 || p.startMs != r.startMs || p.endMs != r.endMs)
                p.id += ":retained:" + std::to_string(p.startMs);
            p.exposureMs = ClipMs(r.exposureMs, r.startMs, r.endMs, p.startMs, p.endMs);
            p.activeMs = ClipMs(r.activeMs, r.startMs, r.endMs, p.startMs, p.endMs);
            p.passiveMs = ClipMs(r.passiveMs, r.startMs, r.endMs, p.startMs, p.endMs);
            p.idleMs = ClipMs(r.idleMs, r.startMs, r.endMs, p.startMs, p.endMs);
            p.unknownMs = ClipMs(r.unknownMs, r.startMs, r.endMs, p.startMs, p.endMs);
            PersistEpisode(w, p);
        }
    });
}

bool ActivityStore::RecordCoverage(SourceCoverage r)
{
    if (r.endMs <= r.startMs || r.source.empty())
        return false;
    if (r.instance.empty())
        r.instance = Evidence::RuntimeId();
    if (!r.receivedMs)
        r.receivedMs = Evidence::NowMs();
    return Enqueue(
        [this, r = std::move(r)](WriterContext& w) {
            for (const auto& part : UnpurgedParts(r.startMs, r.endMs, RecordIds{}))
            {
                auto st = w.Stmt("INSERT OR IGNORE INTO source_coverage VALUES(?,?,?,?,?,?,?,?)");
                Text(st, 1, r.source);
                Text(st, 2, r.instance);
                sqlite3_bind_int64(st, 3, part.first);
                sqlite3_bind_int64(st, 4, part.second);
                Text(st, 5, r.state);
                Text(st, 6, r.reason);
                sqlite3_bind_int64(st, 7, (int64_t)r.dropped);
                sqlite3_bind_int64(st, 8, r.receivedMs);
                Done(w.db, st);
                if (sqlite3_changes(w.db))
                    w.NextChangeSeq();
            }
        },
        true);
}

void ActivityStore::ResolveArtifact(WriterContext& w, FileEventRecord& r)
{
    if (r.isDir || EntityStore::IsTransientFileName(r.path))
        return;
    const std::string key = EntityKey::FileKey(r.path), old = EntityKey::FileKey(r.oldPath);
    const int64_t when = r.eventMs;
    auto& e = r.ctx.evidence;
    const std::string physical = e.volumeId.empty() || e.fileId.empty() ? "" : e.volumeId + ":" + e.fileId;
    auto find = [&](const std::string& path) {
        ArtifactAlias a;
        Statement st(w.db, "SELECT artifact_id,physical_id,from_ms,to_ms FROM artifact_alias "
                           "WHERE path_key=?1 AND from_ms<=?2 AND (to_ms=0 OR to_ms>?2) ORDER BY from_ms DESC LIMIT 1");
        Text(st, 1, path);
        sqlite3_bind_int64(st, 2, when);
        if (sqlite3_step(st) == SQLITE_ROW)
        {
            a.artifactId = Text(st, 0);
            a.physicalId = Text(st, 1);
            a.fromMs = sqlite3_column_int64(st, 2);
            a.toMs = sqlite3_column_int64(st, 3);
        }
        return a;
    };
    const ArtifactAlias target = find(key);
    ArtifactAlias identity = target;
    if (r.action == L"CREATE" && (physical.empty() || target.physicalId != physical))
        identity = ArtifactAlias{};
    std::string basis = physical.empty() ? "path_fallback" : "volume_file_id";
    if (r.action == L"RENAME" && !old.empty() && !EntityStore::IsTransientFileName(r.oldPath))
    {
        const auto previous = find(old);
        if (!previous.artifactId.empty() &&
            (physical.empty() || previous.physicalId.empty() || previous.physicalId == physical))
        {
            identity = previous;
            basis = "explicit_rename";
        }
        Statement close(w.db, "UPDATE artifact_alias SET to_ms=?2 WHERE path_key=?1 AND to_ms=0 AND from_ms<=?2");
        Text(close, 1, old);
        sqlite3_bind_int64(close, 2, when);
        Done(w.db, close);
    }
    else if (!target.artifactId.empty() && !physical.empty() && !target.physicalId.empty() &&
             physical != target.physicalId)
    {
        if (r.action == L"MODIFY" || (r.action == L"RENAME" && EntityStore::IsTransientFileName(r.oldPath)))
            basis = "replacement_at_path";
        else
            identity = ArtifactAlias{}; // a new create/copy is not evidence of logical continuity
    }
    if (identity.artifactId.empty())
        identity.artifactId = Evidence::NewId("artifact");
    e.artifactId = identity.artifactId;
    e.identityBasis = basis;
    if (r.action == L"DELETE")
    {
        Statement close(w.db, "UPDATE artifact_alias SET to_ms=?2 WHERE path_key=?1 AND to_ms=0 AND from_ms<=?2");
        Text(close, 1, key);
        sqlite3_bind_int64(close, 2, when);
        Done(w.db, close);
        return;
    }
    if (target.artifactId == identity.artifactId && target.physicalId == physical)
    {
        Statement refresh(w.db, "UPDATE artifact_alias SET evidence_id=?3 WHERE path_key=?1 AND from_ms=?2");
        Text(refresh, 1, key);
        sqlite3_bind_int64(refresh, 2, target.fromMs);
        Text(refresh, 3, e.id);
        Done(w.db, refresh);
        return;
    }
    Statement close(w.db, "UPDATE artifact_alias SET to_ms=?2 WHERE path_key=?1 AND to_ms=0 AND from_ms<=?2");
    Text(close, 1, key);
    sqlite3_bind_int64(close, 2, when);
    Done(w.db, close);
    Statement add(w.db, "INSERT OR IGNORE INTO artifact_alias VALUES(?,?,0,?,?,?,?)");
    Text(add, 1, key);
    sqlite3_bind_int64(add, 2, when);
    Text(add, 3, e.artifactId);
    Text(add, 4, physical);
    Text(add, 5, basis);
    Text(add, 6, e.id);
    Done(w.db, add);
}

std::vector<ArtifactAlias> ActivityStore::ResolveFileIdentity(const std::string& key, int64_t atMs, int limit)
{
    std::vector<ArtifactAlias> out;
    auto lease = AcquireReader();
    if (!lease)
        return out;
    Statement st(lease.get(),
                 "SELECT path_key,from_ms,to_ms,artifact_id,physical_id,basis,evidence_id FROM artifact_alias "
                 "WHERE artifact_id IN (SELECT artifact_id FROM artifact_alias WHERE path_key=?1 AND from_ms<=?2 "
                 "AND (to_ms=0 OR to_ms>?2)) AND from_ms<=?2 ORDER BY from_ms,path_key LIMIT ?3");
    Text(st, 1, key);
    sqlite3_bind_int64(st, 2, atMs);
    sqlite3_bind_int(st, 3, (std::max)(1, (std::min)(500, limit)));
    while (sqlite3_step(st) == SQLITE_ROW)
    {
        ArtifactAlias a;
        a.pathKey = Text(st, 0);
        a.fromMs = sqlite3_column_int64(st, 1);
        a.toMs = sqlite3_column_int64(st, 2);
        a.artifactId = Text(st, 3);
        a.physicalId = Text(st, 4);
        a.basis = Text(st, 5);
        a.evidenceId = Text(st, 6);
        out.push_back(std::move(a));
    }
    return out;
}

void ActivityStore::PruneEvidence(WriterContext& w)
{
    const int64_t cut = w.nowMs - int64_t(m_rawRetentionDays.load()) * 86400000;
    for (const char* table : {"activity_episode", "source_coverage"})
    {
        std::string sql = std::string("DELETE FROM ") + table + " WHERE end_ms<?1";
        Statement st(w.db, sql.c_str());
        sqlite3_bind_int64(st, 1, cut);
        Done(w.db, st);
    }
}

void ActivityStore::PurgeEvidence(WriterContext& w, const PurgeSpec& spec)
{
    // A privacy edit can change only provenance/episodes, without an entity tombstone.
    // It must still invalidate evidence references keyed by the committed watermark.
    w.NextChangeSeq();
    if (!spec.entityKey.empty())
    {
        Statement st(w.db, "DELETE FROM activity_episode WHERE entity_key=?1 OR app_key=?1");
        Text(st, 1, spec.entityKey);
        Done(w.db, st);
    }
    if (!spec.pathPrefix.empty())
    {
        std::string key = EntityKey::FileKey(spec.pathPrefix);
        if (!key.empty() && key.back() != '\\')
            key += '\\';
        for (const char* sql : {"DELETE FROM artifact_alias WHERE substr(path_key,1,length(?1))=?1",
                                "DELETE FROM activity_episode WHERE substr(entity_key,1,length(?1))=?1 OR "
                                "substr(app_key,1,length(?1))=?1"})
        {
            Statement st(w.db, sql);
            Text(st, 1, key);
            Done(w.db, st);
        }
    }
    if (!spec.domain.empty())
    {
        const auto host = EntityKey::Key(spec.domain);
        auto matchesHost = [&host](const std::string& url) {
            const auto scheme = url.find("://");
            if (scheme == std::string::npos)
                return false;
            const auto end = url.find_first_of("/?#", scheme + 3);
            auto site = EntityKey::Key(
                StoreUtil::U2W(url.substr(scheme + 3, end == std::string::npos ? end : end - scheme - 3).c_str()));
            const auto port = site.rfind(':');
            if (port != std::string::npos && site.find(']') == std::string::npos)
                site.erase(port);
            return site == host ||
                   (site.size() > host.size() && site.compare(site.size() - host.size(), host.size(), host) == 0 &&
                    site[site.size() - host.size() - 1] == '.');
        };
        Statement origins(
            w.db,
            "SELECT f.id,f.origin_url,o.id,o.payload FROM file_activity f LEFT JOIN observation o "
            "ON o.kind='file' AND o.row_id=f.id WHERE f.origin_url IS NOT NULL OR o.payload LIKE '%\"cloud_url\"%'");
        while (sqlite3_step(origins) == SQLITE_ROW)
        {
            auto provenance = Evidence::Parse(Text(origins, 3));
            if (matchesHost(Text(origins, 1)))
            {
                Statement clear(w.db, "UPDATE file_activity SET origin_url=NULL WHERE id=?1");
                sqlite3_bind_int64(clear, 1, sqlite3_column_int64(origins, 0));
                Done(w.db, clear);
            }
            if (matchesHost(provenance.cloudUrl))
            {
                provenance.cloudUrl.clear();
                Statement clear(w.db, "UPDATE observation SET payload=?2 WHERE id=?1");
                Text(clear, 1, Text(origins, 2));
                Text(clear, 2, Evidence::Json(provenance));
                Done(w.db, clear);
            }
        }
        // Check the host, not arbitrary title or opaque-ID substrings.
        Statement rows(w.db, "SELECT episode_id,revision,display FROM activity_episode WHERE kind='page'");
        std::vector<std::pair<std::string, int>> remove;
        while (sqlite3_step(rows) == SQLITE_ROW)
        {
            const auto url = Text(rows, 2);
            const auto scheme = url.find("://");
            if (scheme == std::string::npos)
                continue;
            const auto end = url.find_first_of("/?#", scheme + 3);
            const auto site = EntityKey::Key(
                StoreUtil::U2W(url.substr(scheme + 3, end == std::string::npos ? end : end - scheme - 3).c_str()));
            if (matchesHost(url) ||
                (site.size() > host.size() && site.compare(site.size() - host.size(), host.size(), host) == 0 &&
                 site[site.size() - host.size() - 1] == '.'))
                remove.emplace_back(Text(rows, 0), sqlite3_column_int(rows, 1));
        }
        for (const auto& id : remove)
        {
            Statement st(w.db, "DELETE FROM activity_episode WHERE episode_id=?1 AND revision=?2");
            Text(st, 1, id.first);
            sqlite3_bind_int(st, 2, id.second);
            Done(w.db, st);
        }
    }
    if (spec.startUtc > 0 || spec.endUtc > 0)
    {
        const int64_t start = spec.startUtc * 1000, end = spec.endUtc > 0 ? spec.endUtc * 1000 + 999 : w.nowMs;
        for (const char* table : {"activity_episode", "source_coverage"})
        {
            const std::string sql = std::string("DELETE FROM ") + table + " WHERE end_ms>=?1 AND start_ms<=?2";
            Statement st(w.db, sql.c_str());
            sqlite3_bind_int64(st, 1, start);
            sqlite3_bind_int64(st, 2, end);
            Done(w.db, st);
        }
        Statement aliases(w.db, "DELETE FROM artifact_alias WHERE from_ms<=?2 AND (to_ms=0 OR to_ms>=?1)");
        sqlite3_bind_int64(aliases, 1, start);
        sqlite3_bind_int64(aliases, 2, end);
        Done(w.db, aliases);
    }
}

ActivityStore::EvidenceSet ActivityStore::ReadEvidence(int64_t startMs, int64_t endMs, int64_t asOfMs, int limitPerType)
{
    EvidenceSet result;
    result.startMs = startMs;
    result.endMs = endMs;
    result.asOfMs = asOfMs > 0 ? asOfMs : Evidence::NowMs();
    if (endMs <= startMs || endMs > result.asOfMs || startMs < 0)
        return result;
    limitPerType = (std::max)(1, (std::min)(20000, limitPerType));
    auto lease = AcquireReader();
    if (!lease)
        return result;
    sqlite3* db = lease.get();
    Exec(db, "BEGIN");
    try
    {
        result.watermark = Scalar(db, "SELECT CAST(value AS INTEGER) FROM meta WHERE key='change_seq_hw'");
        result.availableStartMs =
            (std::max)(Scalar(db, "SELECT CAST(value AS INTEGER) FROM meta WHERE key='evidence_available_from'"),
                       Evidence::NowMs() - int64_t(m_rawRetentionDays.load()) * 86400000);
        result.files = QueryFiles(startMs / 1000, (endMs + 999) / 1000, limitPerType + 1, 0, 0, db, result.asOfMs);
#ifdef WARP_UNIT_TESTS
        if (m_evidenceReadBarrier)
            m_evidenceReadBarrier();
#endif
        result.launches =
            QueryLaunches(startMs / 1000, (endMs + 999) / 1000, limitPerType + 1, 0, 0, db, result.asOfMs);
        result.browsing =
            QueryBrowsing(startMs / 1000, (endMs + 999) / 1000, limitPerType + 1, 0, 0, db, result.asOfMs);
        result.focus = QueryFocus(startMs / 1000, (endMs + 999) / 1000, limitPerType + 1, 0, 0, db, result.asOfMs);
        auto points = [&](auto& rows) {
            if ((int)rows.size() > limitPerType)
            {
                result.truncated = true;
                rows.resize(limitPerType);
            }
            rows.erase(std::remove_if(rows.begin(), rows.end(),
                                      [&](const auto& row) { return row.eventMs < startMs || row.eventMs >= endMs; }),
                       rows.end());
        };
        points(result.files);
        points(result.launches);
        points(result.browsing);
        if ((int)result.focus.size() > limitPerType)
        {
            result.truncated = true;
            result.focus.resize(limitPerType);
        }
        result.focus.erase(
            std::remove_if(result.focus.begin(), result.focus.end(),
                           [&](const auto& row) { return row.endMs <= startMs || row.startMs >= endMs; }),
            result.focus.end());
        Statement ep(
            db, "SELECT episode_id,revision,kind,start_ms,end_ms,entity_key,app_key,display,title,exposure_ms,"
                "active_ms,passive_ms,idle_ms,unknown_ms,state,received_ms,payload FROM activity_episode "
                "WHERE end_ms>?1 AND start_ms<?2 AND received_ms<=?3 ORDER BY start_ms,episode_id,revision LIMIT ?4");
        sqlite3_bind_int64(ep, 1, startMs);
        sqlite3_bind_int64(ep, 2, endMs);
        sqlite3_bind_int64(ep, 3, result.asOfMs);
        sqlite3_bind_int(ep, 4, limitPerType + 1);
        while (sqlite3_step(ep) == SQLITE_ROW)
        {
            if ((int)result.episodes.size() == limitPerType)
            {
                result.truncated = true;
                break;
            }
            EpisodeRecord e;
            e.id = Text(ep, 0);
            e.revision = sqlite3_column_int(ep, 1);
            e.kind = Text(ep, 2);
            const int64_t a = sqlite3_column_int64(ep, 3), b = sqlite3_column_int64(ep, 4);
            e.startMs = (std::max)(a, startMs);
            e.endMs = (std::min)(b, endMs);
            e.entityKey = Text(ep, 5);
            e.appKey = Text(ep, 6);
            e.display = StoreUtil::U2W(Text(ep, 7).c_str());
            e.title = StoreUtil::U2W(Text(ep, 8).c_str());
            e.exposureMs = ClipMs(sqlite3_column_int64(ep, 9), a, b, e.startMs, e.endMs);
            auto nullable = [&](int i) {
                return sqlite3_column_type(ep, i) == SQLITE_NULL
                           ? int64_t(-1)
                           : ClipMs(sqlite3_column_int64(ep, i), a, b, e.startMs, e.endMs);
            };
            e.activeMs = nullable(10);
            e.passiveMs = nullable(11);
            e.idleMs = nullable(12);
            e.unknownMs = nullable(13);
            e.state = Text(ep, 14);
            e.receivedMs = sqlite3_column_int64(ep, 15);
            e.evidence = Evidence::Parse(Text(ep, 16));
            e.open = e.evidence.open;
            e.resolution = e.evidence.resolution;
            e.boundaryReason = e.evidence.boundaryReason;
            e.windowInstance = e.evidence.windowInstance;
            e.processInstance = e.evidence.processInstance;
            result.episodes.push_back(std::move(e));
        }
        Statement coverage(
            db, "SELECT source,instance,start_ms,end_ms,state,reason,dropped,received_ms FROM source_coverage "
                "WHERE end_ms>?1 AND start_ms<?2 AND received_ms<=?3 ORDER BY source,start_ms LIMIT 20001");
        sqlite3_bind_int64(coverage, 1, startMs);
        sqlite3_bind_int64(coverage, 2, endMs);
        sqlite3_bind_int64(coverage, 3, result.asOfMs);
        while (sqlite3_step(coverage) == SQLITE_ROW)
        {
            if (result.coverage.size() == 20000)
            {
                result.truncated = true;
                break;
            }
            SourceCoverage c;
            c.source = Text(coverage, 0);
            c.instance = Text(coverage, 1);
            c.startMs = (std::max)(startMs, sqlite3_column_int64(coverage, 2));
            c.endMs = (std::min)(endMs, sqlite3_column_int64(coverage, 3));
            c.state = Text(coverage, 4);
            c.reason = Text(coverage, 5);
            c.dropped = (uint64_t)sqlite3_column_int64(coverage, 6);
            c.receivedMs = sqlite3_column_int64(coverage, 7);
            result.coverage.push_back(std::move(c));
        }
        Statement redacted(db, "SELECT 1 FROM privacy_scope WHERE from_ms<?2 AND to_ms>=?1 LIMIT 1");
        sqlite3_bind_int64(redacted, 1, startMs);
        sqlite3_bind_int64(redacted, 2, endMs);
        result.privacyRedacted = sqlite3_step(redacted) == SQLITE_ROW;
        result.partial = result.truncated || startMs < result.availableStartMs || result.privacyRedacted;
        auto unknownProvenance = [](const auto& rows) {
            return std::any_of(rows.begin(), rows.end(),
                               [](const auto& row) { return row.evidence.id.empty() || row.evidence.receivedMs <= 0; });
        };
        if (unknownProvenance(result.files) || unknownProvenance(result.launches) ||
            unknownProvenance(result.browsing) || unknownProvenance(result.focus))
            result.partial = true;
        for (const auto& f : result.focus)
            if (f.evidence.redaction != "none")
                result.partial = true;
        for (const char* source : {"files", "launch", "browser", "focus", "presence", "storage"})
        {
            int64_t covered = startMs;
            for (const auto& c : result.coverage)
            {
                if (c.source != source)
                    continue;
                if (c.startMs > covered || c.state != "healthy" || c.dropped)
                    result.partial = true;
                covered = (std::max)(covered, c.endMs);
            }
            if (covered < endMs)
                result.partial = true;
        }
        result.available = true;
        Exec(db, "COMMIT");
    }
    catch (...)
    {
        sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
        throw;
    }
    return result;
}

std::string ActivityStore::EvidenceJson(const EvidenceSet& e, int limitPerType)
{
    std::string out;
    WarpJson::Writer w(out);
    const size_t limit = (size_t)(std::max)(0, (std::min)(20000, limitPerType));
    const bool projectionTruncated =
        limit && (e.files.size() > limit || e.launches.size() > limit || e.browsing.size() > limit ||
                  e.focus.size() > limit || e.episodes.size() > limit || e.coverage.size() > limit);
    w.BeginObject()
        .KV("schema", "warp.context-evidence.v1")
        .KV("available", e.available)
        .KV("start_ms", e.startMs)
        .KV("end_ms", e.endMs)
        .KV("interval_semantics", "[start,end)")
        .KV("as_of_ms", e.asOfMs)
        .KV("input_watermark", e.watermark)
        .KV("available_start_ms", e.availableStartMs)
        .KV("partial", e.partial)
        .KV("privacy_redacted", e.privacyRedacted)
        .KV("truncated", e.truncated)
        .KV("projection_truncated", projectionTruncated)
        .KV("rule_version", Evidence::RuleVersion)
        .KV("projection", limit ? "bounded_evidence" : "metadata_only")
        .Key("coverage_summary")
        .BeginArray();
    for (const char* source : {"files", "launch", "browser", "focus", "presence", "storage"})
    {
        std::vector<Attention::Span> spans;
        std::set<std::string> states;
        uint64_t dropped = 0;
        for (const auto& c : e.coverage)
            if (c.source == source)
            {
                spans.push_back({c.startMs, c.endMs, 0, 0});
                states.insert(c.state);
                dropped += c.dropped;
            }
        int64_t covered = 0;
        for (const auto& span : Attention::Union(spans))
            covered += span.end - span.start;
        w.BeginObject()
            .KV("source", source)
            .KV("observed_ms", covered)
            .KV("unknown_ms", (std::max)(int64_t(0), e.endMs - e.startMs - covered))
            .KV("dropped", dropped)
            .Key("states")
            .BeginArray();
        for (const auto& state : states)
            w.String(state);
        w.EndArray().EndObject();
    }
    w.EndArray().Key("observations").BeginArray();
    auto finish = [&](const ObservationEvidence& p) {
        w.Key("provenance");
        Evidence::Write(w, p);
        w.EndObject();
    };
    for (size_t i = 0; i < e.files.size() && i < limit; ++i)
    {
        const auto& f = e.files[i];
        w.BeginObject()
            .KV("ref", "file:" + std::to_string(f.id))
            .KV("kind", "file")
            .KV("event_ms", f.eventMs)
            .KV("entity_key", f.entityKey)
            .KV("path", f.path)
            .KV("old_path", f.oldPath)
            .KV("action", f.action)
            .KV("source", f.source)
            .KV("origin_url", f.originUrl)
            .KV("confidence", f.confidence);
        finish(f.evidence);
    }
    for (size_t i = 0; i < e.launches.size() && i < limit; ++i)
    {
        const auto& a = e.launches[i];
        w.BeginObject()
            .KV("ref", "launch:" + std::to_string(a.id))
            .KV("kind", "launch")
            .KV("event_ms", a.eventMs)
            .KV("entity_key", StoreUtil::AppEntityKey(a.exePath, a.aumid))
            .KV("app", a.exeName)
            .KV("action", "launch_observed");
        finish(a.evidence);
    }
    for (size_t i = 0; i < e.browsing.size() && i < limit; ++i)
    {
        const auto& b = e.browsing[i];
        w.BeginObject()
            .KV("ref", "browse:" + std::to_string(b.id))
            .KV("kind", "browse")
            .KV("event_ms", b.eventMs)
            .KV("entity_key", b.entityKey)
            .KV("title", b.title)
            .KV("url", b.url)
            .KV("action", "page_observed");
        finish(b.evidence);
    }
    for (size_t i = 0; i < e.focus.size() && i < limit; ++i)
    {
        const auto& f = e.focus[i];
        const int64_t a = (std::max)(f.startMs, e.startMs), b = (std::min)(f.endMs, e.endMs);
        w.BeginObject()
            .KV("ref", "focus:" + std::to_string(f.id))
            .KV("kind", "focus")
            .KV("entity_key", f.docKey.empty() ? StoreUtil::AppEntityKey(f.exePath, f.aumid) : f.docKey)
            .KV("title", f.windowTitle)
            .KV("start_ms", a)
            .KV("end_ms", b)
            .KV("action", "foreground")
            .KV("active_secs", Attention::Clip(f.activeSecs, f.startMs, f.endMs, a, b))
            .KV("passive_secs", Attention::Clip(f.passiveSecs, f.startMs, f.endMs, a, b))
            .KV("attention_status", f.attentionStatus);
        finish(f.evidence);
    }
    w.EndArray().Key("episodes").BeginArray();
    for (size_t i = 0; i < e.episodes.size() && i < limit; ++i)
        Evidence::Write(w, e.episodes[i]);
    w.EndArray().Key("source_coverage").BeginArray();
    for (size_t i = 0; i < e.coverage.size() && i < limit; ++i)
        Evidence::Write(w, e.coverage[i]);
    w.EndArray()
        .KV("missing_coverage_semantics", "Uncovered time is unknown, not inactivity.")
        .KV("open_episode_semantics", "Only checkpointed time is durable; no extrapolation after a crash.")
        .EndObject();
    return out;
}

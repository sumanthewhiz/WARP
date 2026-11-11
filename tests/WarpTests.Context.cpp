#include "WarpTests.h"
#include "ActivityStore.h"
#include "Attention.h"
#include "ContextInference.h"
#include "DocResolver.h"
#include "EntityKey.h"
#include "EntityStore.h"
#include "FileCapture.h"
#include "HistoryEngine.h"
#include "Json.h"
#include "PrintMonitor.h"
#include "UrlCanonicalizer.h"
#include "sqlite3.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <set>
#include <stdexcept>

struct ContextInferenceTestAccess
{
    static void Attach(ContextInference& c, ActivityStore& store, EntityStore& entities, HistoryEngine& history)
    {
        c.Init(L"");
        c.m_store = &store;
        c.SetEntityStore(&entities);
        c.SetHistoryEngine(&history);
        c.SetPresenceProvider([] { return std::string("active"); });
    }
    static ContextSnapshot Compose(ContextInference& c, int64_t end, int64_t seconds = 1800)
    {
        return c.ComposeSnapshot(seconds, false, end);
    }
    static std::string Json(const ContextSnapshot& s) { return ContextInference::SnapshotToJsonObject(s, "all", false); }
};

namespace
{
    struct Fixture
    {
        wt::TempDir dir;
        ActivityStore store;
        EntityStore entities{ &store };
        HistoryEngine history{ &store };
        ContextInference context;
        int64_t now = wt::NowUtc();
        Fixture()
        {
            store.AddSink(&entities);
            store.AddSink(&history);
            if (!store.Open(dir.File(L"quality.db"))) throw std::runtime_error("cannot open fixture database");
            ContextInferenceTestAccess::Attach(context, store, entities, history);
        }
        ~Fixture() { context.Stop(); store.Close(); }
        void Flush()
        {
            if (!store.RunSync([](WriterContext&) {})) throw std::runtime_error("fixture writer barrier failed");
        }
        ContextSnapshot Snapshot(int64_t window = 1800) { Flush(); return ContextInferenceTestAccess::Compose(context, now, window); }
        FocusEventRecord Focus(int active, int passive = 0)
        {
            FocusEventRecord r;
            r.startMs = (now - 660) * 1000;
            r.endMs = (now - 60) * 1000;
            r.exeName = L"WINWORD.EXE";
            r.exePath = L"C:\\WarpQuality\\WINWORD.EXE";
            r.windowTitle = L"Alpha proposal.docx - Word";
            r.docDisplay = L"C:\\WarpQuality\\Alpha proposal.docx";
            r.docKey = EntityKey::FileKey(r.docDisplay);
            r.docType = "file";
            r.docResolution = "filename_candidate";
            r.activeSecs = active;
            r.passiveSecs = passive;
            r.idleSecs = active < 0 ? 0 : 600 - active - passive;
            r.ctx.confidence = 1;
            return r;
        }
    };

    std::wstring Wide(const WarpJson::Value& v, const char* field)
    {
        return EntityKey::FromUtf8(v.get(field).asString());
    }

    int64_t RawCount(ActivityStore& store, const std::string& type)
    {
        const char* table = type == "file" ? "file_activity" : type == "browse" ? "browsing_activity" :
            type == "launch" ? "app_launch_activity" : type == "focus" ? "app_focus_activity" :
            type == "presence" ? "presence_interval" : type == "print" ? "print_activity" : nullptr;
        if (!table) return 0;
        auto reader = store.AcquireReader();
        if (!reader) throw std::runtime_error("cannot read replay counts");
        sqlite3_stmt* st = nullptr;
        const std::string sql = std::string("SELECT COUNT(*) FROM ") + table;
        if (sqlite3_prepare_v2(reader.get(), sql.c_str(), -1, &st, nullptr) != SQLITE_OK)
            throw std::runtime_error("cannot prepare replay count");
        const int64_t count = sqlite3_step(st) == SQLITE_ROW ? sqlite3_column_int64(st, 0) : -1;
        sqlite3_finalize(st);
        if (count < 0) throw std::runtime_error("cannot count replay rows");
        return count;
    }

    void MakeLegacyV2(const std::wstring& path, const char* extra)
    {
        sqlite3* db = nullptr;
        if (sqlite3_open16(path.c_str(), &db) != SQLITE_OK) throw std::runtime_error("cannot open legacy fixture");
        const char* sql =
            "DROP TABLE session_attention; DROP TABLE entity_alias;"
            "ALTER TABLE app_focus_activity DROP COLUMN passive_secs;"
            "ALTER TABLE app_focus_activity DROP COLUMN unknown_secs;"
            "ALTER TABLE app_focus_activity DROP COLUMN doc_resolution;"
            "ALTER TABLE work_session DROP COLUMN accounting_version;"
            "PRAGMA user_version=2;";
        const bool ok = sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK &&
                        sqlite3_exec(db, extra, nullptr, nullptr, nullptr) == SQLITE_OK;
        sqlite3_close(db);
        if (!ok) throw std::runtime_error("could not construct legacy-v2 fixture");
    }
}

TEST(Context, MeasuredZeroNeverBecomesFullEngagement)
{
    Fixture f;
    REQUIRE(f.store.RecordFocus(f.Focus(0)));
    const auto s = f.Snapshot();
    CHECK_EQ(s.focusSeconds, 0);
    CHECK_EQ(s.exposureSeconds, 600);
    CHECK_EQ(s.idleSeconds, 600);
    CHECK_EQ(s.unknownSeconds, 0);
    REQUIRE(s.entities.size() == 1);
    CHECK_EQ(s.entities[0].activeSecs, 0);
    CHECK_EQ(s.entities[0].role, std::string("observed"));
    CHECK(s.confidence < 0.5);
}

TEST(Context, MissingAttentionAndPassiveMediaAreNotActiveTime)
{
    Fixture unknown;
    REQUIRE(unknown.store.RecordFocus(unknown.Focus(-1)));
    auto u = unknown.Snapshot();
    CHECK_EQ(u.focusSeconds, 0);
    CHECK_EQ(u.unknownSeconds, 600);
    REQUIRE(u.entities.size() == 1);
    CHECK_EQ(u.entities[0].role, std::string("attention_unknown"));

    Fixture passive;
    const auto row = passive.Focus(0, 600);
    REQUIRE(passive.store.RecordFocus(row));
    auto p = passive.Snapshot();
    CHECK_EQ(p.focusSeconds, 0);
    CHECK_EQ(p.passiveSeconds, 600);
    CHECK_EQ(p.idleSeconds, 0);
    CHECK_EQ(p.unknownSeconds, 0);
    CHECK(passive.entities.Lookup({ row.docKey })[0].entityKey.empty());
}

TEST(Context, ClippedWindowsConserveMeasuredTotals)
{
    Fixture f;
    auto r = f.Focus(7);
    r.startMs = (f.now - 1200) * 1000;
    r.endMs = f.now * 1000;
    r.idleSecs = 1193;
    REQUIRE(f.store.RecordFocus(r));
    f.Flush();
    int total = 0;
    for (int i = 1; i <= 4; ++i)
        total += ContextInferenceTestAccess::Compose(f.context, f.now - 1200 + i * 300, 300).focusSeconds;
    CHECK_EQ(total, 7);
    CHECK_EQ(f.Snapshot(1800).focusSeconds, 7);
    CHECK_EQ(f.Snapshot(7200).focusSeconds, 7);
    CHECK_EQ(f.Snapshot(86400).focusSeconds, 7);
}

TEST(Context, PackagedAppFocusKeepsItsAumid)
{
    Fixture f;
    auto r = f.Focus(600);
    r.docKey.clear(); r.docDisplay.clear(); r.docType.clear();
    r.aumid = L"Contoso.Editor_package!App";
    REQUIRE(f.store.RecordFocus(r));
    const auto s = f.Snapshot();
    REQUIRE(s.entities.size() == 1);
    CHECK_EQ(s.entities[0].entityKey, EntityKey::AppKey(r.aumid));
    CHECK(!f.entities.Lookup({ s.entities[0].entityKey })[0].entityKey.empty());
    const auto rows = f.store.QueryFocus(0, f.now);
    REQUIRE(rows.size() == 1);
    CHECK_EQ(rows[0].aumid, r.aumid);
}

TEST(Context, BrowserOnlyEvidenceSurvivesWithoutFocus)
{
    Fixture f;
    BrowseEventRecord r;
    r.eventMs = (f.now - 60) * 1000;
    r.url = L"https://example.org/Alpha";
    r.title = L"Alpha architecture reference";
    r.browser = L"Microsoft Edge";
    REQUIRE(f.store.RecordBrowse(r));
    const auto s = f.Snapshot();
    CHECK_EQ(s.focusSeconds, 0);
    CHECK_EQ(s.exposureSeconds, 0);
    CHECK(!s.summaryWebsites.empty());
    REQUIRE(s.entities.size() == 1);
    CHECK_EQ(s.entities[0].entityKey, EntityKey::UrlKey(r.url));
    CHECK_EQ(s.entities[0].action, std::string("page_observed"));
    CHECK(s.summary[0].find("no recent") == std::string::npos);
}

TEST(Context, UnresolvedBrowserDoesNotBorrowAnotherWindowsPage)
{
    Fixture f;
    auto focus = f.Focus(600);
    focus.exeName = L"msedge.exe"; focus.exePath = L"C:\\WarpQuality\\msedge.exe";
    focus.windowTitle = L"Unknown page - Microsoft Edge";
    focus.docKey.clear(); focus.docType.clear(); focus.docDisplay.clear();
    focus.docResolution = "unresolved";
    REQUIRE(f.store.RecordFocus(focus));
    BrowseEventRecord browse;
    browse.eventMs = (f.now - 30) * 1000;
    browse.url = L"https://example.org/OtherWindow";
    browse.title = L"Unrelated page";
    browse.browser = L"Microsoft Edge";
    REQUIRE(f.store.RecordBrowse(browse));
    const auto s = f.Snapshot();
    REQUIRE(s.entities.size() == 1);
    CHECK_EQ(s.entities[0].entityKey, EntityKey::AppKey(focus.exePath));
    CHECK(s.entities[0].title.find("Unrelated") == std::string::npos);
}

TEST(Context, IdleFocusDoesNotHideExplicitSparseObservations)
{
    Fixture f;
    auto focus = f.Focus(0);
    REQUIRE(f.store.RecordFocus(focus));
    FileEventRecord file;
    file.eventMs = (f.now - 30) * 1000;
    file.action = L"OPEN"; file.source = L"recent"; file.path = focus.docDisplay;
    REQUIRE(f.store.RecordFile(file));
    const auto s = f.Snapshot();
    REQUIRE(s.entities.size() == 1);
    CHECK_EQ(s.entities[0].action, std::string("open"));
    CHECK_EQ(s.entities[0].lastActiveTs, int64_t(0));
    CHECK_EQ(s.entities[0].lastObservedTs, f.now - 30);
    CHECK_EQ(s.entities[0].exposureSecs, 600);
    CHECK_EQ(s.entities[0].activeSecs, 0);
}
TEST(Context, FileOpenDoesNotClaimEditing)
{
    Fixture f;
    FileEventRecord r;
    r.eventMs = (f.now - 60) * 1000;
    r.action = L"OPEN"; r.path = L"C:\\WarpQuality\\Read-only proposal.pdf"; r.source = L"recent";
    REQUIRE(f.store.RecordFile(r));
    const auto s = f.Snapshot();
    CHECK_EQ(s.focusSeconds, 0);
    REQUIRE(s.entities.size() == 1);
    CHECK_EQ(s.entities[0].action, std::string("open"));
    CHECK_EQ(s.entities[0].entityKey, EntityKey::FileKey(r.path));
    REQUIRE(s.summaryFiles.size() == 1);
    CHECK_EQ(s.summaryFiles[0], s.summary[0]);
    CHECK(s.summary[0].find("Opened") != std::string::npos);
    CHECK(s.summary[0].find("Editing") == std::string::npos);
}

TEST(Context, AppOnlyEvidenceHasNoSyntheticDwell)
{
    Fixture f;
    LaunchEventRecord r;
    r.eventMs = (f.now - 60) * 1000;
    r.exeName = L"notepad.exe"; r.exePath = L"C:\\WarpQuality\\notepad.exe";
    r.aumid = L"Contoso.Notes_package!App";
    REQUIRE(f.store.RecordLaunch(r));
    const auto s = f.Snapshot();
    CHECK_EQ(s.focusSeconds, 0);
    REQUIRE(s.entities.size() == 1);
    CHECK_EQ(s.entities[0].entityKey, EntityKey::AppKey(r.aumid));
    CHECK_EQ(s.entities[0].action, std::string("launch"));
    CHECK(!s.summaryApps.empty());
}

TEST(Context, PersistedFacetsMatchAfterComposerAndStoreRestart)
{
    Fixture f;
    REQUIRE(f.store.RecordFocus(f.Focus(600)));
    f.Flush();
    f.context.RunOnce();
    const auto before = WarpJson::Parse(f.context.GetRecentContexts(10, "files", 86400, false));
    REQUIRE(before.ok && before.value["returned"].asInt64() > 0);
    f.store.Close();
    REQUIRE(f.store.Open(f.dir.File(L"quality.db")));
    ContextInference restarted;
    restarted.SetHistoryEngine(&f.history);
    for (const std::string category : { "all", "files", "websites", "apps", "documents" })
    {
        const auto after = WarpJson::Parse(restarted.GetRecentContexts(10, category, 86400, false));
        REQUIRE(after.ok);
        CHECK_EQ(after.value["returned"].asInt64(), before.value["returned"].asInt64());
        CHECK_EQ(after.value["from_persisted"].asInt64(), after.value["returned"].asInt64());
        const auto& row = after.value["recent_contexts"][0];
        CHECK(!row.has("summary_embedding"));
        if (category != "all") CHECK(!row.has("summary_files"));
        if (category == "files" || category == "documents")
            CHECK_EQ(WarpJson::Serialize(row["summary"]), WarpJson::Serialize(before.value["recent_contexts"][0]["summary"]));
    }
}

TEST(History, FocusPresenceUnionSurvivesOrderReplayAndRestart)
{
    for (bool presenceFirst : { false, true })
    {
        Fixture f;
        const auto r = f.Focus(600);
        PresenceRecord p{ r.startMs, r.endMs, "active" };
        if (presenceFirst) REQUIRE(f.store.RecordPresence(p));
        REQUIRE(f.store.RecordFocus(r));
        if (!presenceFirst) REQUIRE(f.store.RecordPresence(p));
        f.Flush();
        auto sessions = f.history.GetSessions(0, 0, 10);
        REQUIRE(sessions.size() == 1);
        CHECK_EQ(sessions[0].activeSecs, 600);
        f.store.Close();
        REQUIRE(f.store.Open(f.dir.File(L"quality.db")));
        REQUIRE(f.store.RecordPresence(p));
        f.Flush();
        sessions = f.history.GetSessions(0, 0, 10);
        REQUIRE(sessions.size() == 1);
        CHECK_EQ(sessions[0].activeSecs, 600);
        CHECK_EQ(sessions[0].passiveSecs, 0);
    }
}

TEST(History, PassiveUnionDoesNotOverlapActiveOrCreditGaps)
{
    Fixture f;
    const auto r = f.Focus(0, 600);
    REQUIRE(f.store.RecordFocus(r));
    REQUIRE(f.store.RecordPresence({ r.startMs + 100000, r.startMs + 400000, "active" }));
    REQUIRE(f.store.RecordPresence({ r.endMs + 100000, r.endMs + 200000, "active" }));
    f.Flush();
    const auto sessions = f.history.GetSessions(0, 0, 10);
    REQUIRE(sessions.size() == 1);
    CHECK_EQ(sessions[0].activeSecs, 400);
    CHECK_EQ(sessions[0].passiveSecs, 300);
    CHECK_EQ(sessions[0].endTs - sessions[0].startTs, 800);
}

TEST(History, PurgedAttentionCannotReappearOnLaterOverlap)
{
    Fixture f;
    const auto r = f.Focus(600);
    REQUIRE(f.store.RecordFocus(r));
    f.Flush();
    ActivityStore::PurgeSpec purge;
    purge.startUtc = r.startMs / 1000 + 200;
    purge.endUtc = r.startMs / 1000 + 300;
    int64_t removed = 0;
    REQUIRE(f.store.Purge(purge, removed));
    auto sessions = f.history.GetSessions(0, 0, 10);
    int before = 0;
    for (const auto& s : sessions) before += s.activeSecs;
    REQUIRE(f.store.RecordPresence({ r.startMs, r.endMs, "active" }));
    f.Flush();
    sessions = f.history.GetSessions(0, 0, 10);
    int after = 0;
    for (const auto& s : sessions) after += s.activeSecs;
    CHECK(after <= before + 1);
    CHECK(after < 500);
    REQUIRE(f.store.ClearAll());
    REQUIRE(f.store.RecordPresence({ (f.now + 10) * 1000, (f.now + 20) * 1000, "active" }));
    f.Flush();
    sessions = f.history.GetSessions(0, 0, 10);
    REQUIRE(sessions.size() == 1);
    CHECK_EQ(sessions[0].activeSecs, 10);
}

TEST(Store, V2WebAliasesAreExplicitAndBecomeAmbiguous)
{
    Fixture f;
    BrowseEventRecord row;
    row.eventMs = (f.now - 60) * 1000;
    row.browser = L"Edge"; row.url = L"https://example.org/PlanA"; row.title = L"Alpha plan";
    REQUIRE(f.store.RecordBrowse(row));
    for (const wchar_t* name : { L"C:\\WarpQuality\\a.txt", L"C:\\WarpQuality\\b.txt" })
    {
        FileEventRecord file;
        file.action = L"OPEN"; file.source = L"recent"; file.path = name;
        REQUIRE(f.store.RecordFile(file));
    }
    f.Flush();
    f.store.Close();
    MakeLegacyV2(f.dir.File(L"quality.db"),
        "UPDATE browsing_activity SET entity_key=lower(entity_key);"
        "UPDATE inference SET entity_key=lower(entity_key);"
        "UPDATE entity_day SET entity_key=lower(entity_key);"
        "INSERT INTO tombstone VALUES(123,'c:\\warpquality\\old-deleted.txt','file',1,'deleted');");
    ActivityStore restored;
    EntityStore entities(&restored);
    restored.AddSink(&entities);
    REQUIRE(restored.Open(f.dir.File(L"quality.db")));
    CHECK_EQ(restored.GetStats().integrity, std::string("ok"));
    const std::string legacy = "https://example.org/plana", canonical = "https://example.org/PlanA";
    CHECK(entities.Lookup({ legacy })[0].entityKey.empty()); // an unseen case variant is not the legacy alias
    const auto alias = entities.Lookup({ legacy }, 0, true);
    CHECK_EQ(alias[0].entityKey, canonical);
    CHECK(restored.ResetSeq() > 0);
    REQUIRE(restored.ScheduleMaintenance()); // a pruned old tombstone must not invalidate bootstrap page two
    REQUIRE(restored.RunSync([](WriterContext&) {}));
    int64_t cursor = 0;
    size_t upserts = 0;
    for (int page = 0; page < 10; ++page)
    {
        const auto changes = entities.GetChanges(cursor, 1);
        REQUIRE(!changes.reset);
        upserts += changes.upserts.size();
        cursor = changes.nextSeq;
        if (!changes.more) break;
    }
    CHECK_EQ(upserts, size_t(3));
    CHECK(cursor >= restored.ResetSeq());
    row.url = L"https://example.org/pLANA";
    REQUIRE(restored.RecordBrowse(row));
    REQUIRE(restored.RunSync([](WriterContext&) {}));
    CHECK(entities.Lookup({ legacy }, 0, true)[0].entityKey.empty());
    CHECK(!entities.Lookup({ canonical })[0].entityKey.empty());
    CHECK(!entities.Lookup({ "https://example.org/pLANA" })[0].entityKey.empty());
    restored.Close();
}

TEST(Store, V2CaseCollisionsRebuildOnlyRetainedEvidence)
{
    Fixture f;
    BrowseEventRecord row;
    row.eventMs = (f.now - 180) * 1000;
    row.browser = L"Edge"; row.url = L"https://example.org/PlanA"; row.title = L"Uppercase plan";
    REQUIRE(f.store.RecordBrowse(row));
    row.eventMs += 120000;
    row.url = L"https://example.org/plana"; row.title = L"Lowercase plan";
    REQUIRE(f.store.RecordBrowse(row));
    f.Flush();
    f.store.Close();
    MakeLegacyV2(f.dir.File(L"quality.db"),
        "UPDATE browsing_activity SET entity_key=lower(entity_key);"
        "DELETE FROM inference WHERE entity_key='https://example.org/PlanA';"
        "UPDATE inference SET open_count_total=77 WHERE entity_key='https://example.org/plana';"
        "DELETE FROM entity_day;");
    ActivityStore restored;
    EntityStore entities(&restored);
    restored.AddSink(&entities);
    REQUIRE(restored.Open(f.dir.File(L"quality.db")));
    CHECK_EQ(restored.GetStats().integrity, std::string("ok"));
    auto scores = entities.Lookup({ "https://example.org/PlanA", "https://example.org/plana" });
    REQUIRE(scores.size() == 2 && !scores[0].entityKey.empty() && !scores[1].entityKey.empty());
    CHECK_NEAR(scores[0].openCountTotal, 1, 1e-9);
    CHECK_NEAR(scores[1].openCountTotal, 1, 1e-9);
    CHECK(entities.Lookup({ "https://example.org/plana" }, 0, true)[0].entityKey.empty());
    CHECK_EQ(restored.QueryBrowsing(0, f.now).size(), size_t(2));
    restored.Close();
}

TEST(Store, CaseSensitivePurgeRemovesOnlyMatchingSnapshots)
{
    Fixture f;
    REQUIRE(f.store.RunSync([&](WriterContext& w) {
        for (const char* key : { "gdoc:AbC123", "gdoc:abc123" })
        {
            std::string json;
            WarpJson::Writer writer(json);
            writer.BeginObject().KV("timestamp", f.now).Key("current_entities").BeginArray()
                .BeginObject().KV("entity_key", key).EndObject().EndArray().EndObject();
            auto* st = w.Stmt("INSERT INTO context_snapshot(ts,window_secs,json) VALUES(?1,900,?2);");
            sqlite3_bind_int64(st, 1, f.now);
            sqlite3_bind_text(st, 2, json.c_str(), -1, SQLITE_TRANSIENT);
            REQUIRE(sqlite3_step(st) == SQLITE_DONE);
        }
    }));
    ActivityStore::PurgeSpec spec;
    spec.entityKey = "gdoc:AbC123";
    int64_t deleted = 0;
    REQUIRE(f.store.Purge(spec, deleted));
    const auto snapshots = f.history.ContextSnapshotsJson(10, 0);
    REQUIRE(snapshots.size() == 1);
    CHECK(snapshots[0].find("gdoc:abc123") != std::string::npos);
}

TEST(Store, EntityPurgeStillRemovesFileReferencesInsideProse)
{
    Fixture f;
    REQUIRE(f.store.RunSync([&](WriterContext& w) {
        std::string json;
        WarpJson::Writer writer(json);
        writer.BeginObject().KV("timestamp", f.now).Key("summary").BeginArray()
            .String("Foreground observed: C:\\WarpQuality\\Private.docx - Word").EndArray().EndObject();
        auto* st = w.Stmt("INSERT INTO context_snapshot(ts,window_secs,json) VALUES(?1,900,?2);");
        sqlite3_bind_int64(st, 1, f.now);
        sqlite3_bind_text(st, 2, json.c_str(), -1, SQLITE_TRANSIENT);
        REQUIRE(sqlite3_step(st) == SQLITE_DONE);
    }));
    ActivityStore::PurgeSpec spec;
    spec.entityKey = "c:\\warpquality\\private.docx";
    int64_t deleted = 0;
    REQUIRE(f.store.Purge(spec, deleted));
    CHECK(f.history.ContextSnapshotsJson(10, 0).empty());
}

TEST(Store, V2EquivalentWebKeysMergeWithoutQuarantiningHistory)
{
    Fixture f;
    BrowseEventRecord row;
    row.eventMs = (f.now - 60) * 1000;
    row.browser = L"Edge"; row.url = L"https://www.youtube.com/watch?v=AbCdEf12345";
    row.docId = L"yt:AbCdEf12345"; row.title = L"Same document";
    REQUIRE(f.store.RecordBrowse(row));
    row.docId.clear(); // a legacy source kept the URL instead of the provider identity
    REQUIRE(f.store.RecordBrowse(row));
    f.Flush();
    f.store.Close();
    MakeLegacyV2(f.dir.File(L"quality.db"),
        "UPDATE browsing_activity SET entity_key=lower(entity_key);"
        "UPDATE inference SET entity_key=lower(entity_key);"
        "UPDATE entity_day SET entity_key=lower(entity_key);");
    ActivityStore restored;
    EntityStore entities(&restored);
    restored.AddSink(&entities);
    REQUIRE(restored.Open(f.dir.File(L"quality.db")));
    CHECK_EQ(restored.GetStats().integrity, std::string("ok"));
    const auto scores = entities.Lookup({ "yt:AbCdEf12345" });
    REQUIRE(!scores[0].entityKey.empty());
    CHECK_NEAR(scores[0].openCountTotal, 1, 1e-9);
    const auto aliases = entities.Lookup({ "yt:abcdef12345", "https://www.youtube.com/watch?v=abcdef12345" }, 0, true);
    REQUIRE(aliases.size() == 2);
    CHECK_EQ(aliases[0].entityKey, std::string("yt:AbCdEf12345"));
    CHECK_EQ(aliases[1].entityKey, std::string("yt:AbCdEf12345"));
    CHECK_EQ(restored.QueryBrowsing(0, f.now).size(), size_t(2));
    restored.Close();
}

TEST(Store, V2SessionEstimatesAreBoundedAndMarkedLegacy)
{
    Fixture f;
    const auto focus = f.Focus(600);
    REQUIRE(f.store.RecordFocus(focus));
    f.Flush();
    f.store.Close();
    MakeLegacyV2(f.dir.File(L"quality.db"), "UPDATE work_session SET active_secs=1200;");
    ActivityStore restored;
    HistoryEngine history(&restored);
    restored.AddSink(&history);
    REQUIRE(restored.Open(f.dir.File(L"quality.db")));
    const auto rows = restored.QueryFocus(0, f.now);
    REQUIRE(rows.size() == 1);
    CHECK_EQ(rows[0].activeSecs, -1);
    CHECK_EQ(rows[0].unknownSecs, 600);
    REQUIRE(restored.RecordPresence({ focus.startMs, focus.endMs, "active" }));
    REQUIRE(restored.RunSync([](WriterContext&) {}));
    const auto sessions = history.GetSessions(0, 0, 10);
    REQUIRE(sessions.size() == 1);
    CHECK_EQ(sessions[0].activeSecs, 600);
    CHECK_EQ(sessions[0].accountingVersion, 0);
    restored.Close();
}

// Explicit opt-in replay: no Windows hooks, event-log subscription, personal database,
// models or registry writes. The real store, identity rules and composer process fixtures.
int RunQualityReplay(const std::wstring& input, const std::wstring& output, const std::wstring& models)
{
    std::error_code error;
    if (std::filesystem::equivalent(input, output, error) || (error && std::filesystem::exists(output)))
        throw std::runtime_error("replay input/output must be verifiably distinct");
    std::ifstream in{ std::filesystem::path(input) };
    std::ofstream out{ std::filesystem::path(output), std::ios::binary };
    if (!in || !out) throw std::runtime_error("cannot open replay input/output");
    std::string line;
    int cases = 0;
    while (std::getline(in, line))
    {
        if (line.empty()) continue;
        if (line.size() > 1024 * 1024) throw std::runtime_error("replay case exceeds 1 MiB");
        auto parsed = WarpJson::Parse(line);
        if (!parsed.ok || !parsed.value.isObject()) throw std::runtime_error("invalid replay JSON");
        const auto& request = parsed.value;
        if (!request["signals"].isArray() || request["signals"].size() > 20000 || request["id"].asString().empty())
            throw std::runtime_error("replay requires id and a bounded signals array");
        const int64_t now = request["as_of"].asInt64();
        const int64_t window = request["window_seconds"].asInt64(1800);
        if (now < 2592000 || now > 253402300799LL || window < 1 || window > 2592000)
            throw std::runtime_error("invalid replay time/window");
        Fixture f;
        f.now = now;
        if (!models.empty())
        {
            f.context.Init(models);
            if (!f.context.IsModelLoaded()) throw std::runtime_error("requested Granite model/tokenizer could not be loaded");
            const auto probe = f.context.EmbedText("WARP signal quality validation");
            double norm = 0;
            for (float value : probe) norm += (double)value * value;
            if (probe.size() != 384 || !std::isfinite(norm) || std::abs(norm - 1.0) > 0.01)
                throw std::runtime_error("requested Granite encoder did not produce a valid normalized embedding");
        }
        auto decisions = WarpJson::Value::Array();
        std::set<std::string> seenKeys;
        int64_t feedCursor = 0;
        for (const auto& signal : request["signals"].items())
        {
            const auto type = signal["type"].asString();
            const int64_t beforeCount = RawCount(f.store, type);
            const int64_t from = signal["start_offset"].asInt64(-60), to = signal["end_offset"].asInt64(-60);
            if (from < -2592000 || to < -2592000 || from > 60 || to > 60)
                throw std::runtime_error("signal offset exceeds replay bounds");
            bool accepted = false, ambiguous = false;
            std::string key, resolved, action = type;
            if (type == "focus")
            {
                if (to < from) throw std::runtime_error("inverted focus interval");
                auto r = f.Focus((int)signal["active_secs"].asInt64(-1), (int)signal["passive_secs"].asInt64(0));
                r.startMs = (now + from) * 1000; r.endMs = (now + to) * 1000;
                r.idleSecs = (int)signal["idle_secs"].asInt64(0);
                r.exeName = Wide(signal, "exe_name"); r.exePath = Wide(signal, "exe_path");
                r.aumid = Wide(signal, "aumid"); r.windowTitle = Wide(signal, "title");
                r.docDisplay = Wide(signal, "doc_path");
                r.docKey = r.docDisplay.empty() ? "" : EntityKey::FileKey(r.docDisplay);
                r.docType = r.docKey.empty() ? "" : "file";
                r.docResolution = r.docKey.empty() ? "unresolved" : "filename_candidate";
                if (signal["doc_candidates"].isArray())
                {
                    std::vector<std::wstring> candidates;
                    for (const auto& candidate : signal["doc_candidates"].items()) candidates.push_back(EntityKey::FromUtf8(candidate.asString()));
                    DocResolver::MatchPath(Wide(signal, "doc_name"), candidates, r.docDisplay, &ambiguous);
                    r.docKey = r.docDisplay.empty() ? "" : EntityKey::FileKey(r.docDisplay);
                    r.docType = r.docKey.empty() ? "" : "file";
                    r.docResolution = ambiguous ? "ambiguous" : (r.docKey.empty() ? "unresolved" : "filename_candidate");
                }
                key = r.docKey.empty() ? StoreUtil::AppEntityKey(r.exePath, r.aumid) : r.docKey;
                seenKeys.insert(StoreUtil::AppEntityKey(r.exePath, r.aumid));
                resolved = r.docKey;
                accepted = f.store.RecordFocus(r);
            }
            else if (type == "presence")
            {
                const auto state = signal["state"].asString();
                if (to < from || (state != "active" && state != "passive" && state != "away" && state != "locked" && state != "asleep"))
                    throw std::runtime_error("invalid presence interval/state");
                accepted = f.store.RecordPresence({ (now + from) * 1000, (now + to) * 1000, state });
            }
            else if (type == "file")
            {
                FileEventRecord r;
                r.eventMs = (now + to) * 1000; r.path = Wide(signal, "path");
                r.action = Wide(signal, "action"); r.source = Wide(signal, "source");
                r.ctx.confidence = signal["confidence"].asDouble(1);
                key = EntityKey::FileKey(r.path);
                resolved = key;
                action = EntityKey::Key(r.action);
                accepted = FileCapture::ClassifyPath(r.path) == FileCapture::PathClass::User;
                if (accepted) accepted = f.store.RecordFile(r);
            }
            else if (type == "browse")
            {
                const auto url = UrlCanon::Canonicalize(Wide(signal, "url"));
                BrowseEventRecord r;
                r.eventMs = (now + to) * 1000; r.url = url.canonical; r.docId = url.docId; r.docKind = url.docKind;
                r.title = Wide(signal, "title"); r.browser = Wide(signal, "browser"); r.host = url.host;
                key = StoreUtil::WebEntityKey(r.url, r.docId, r.title);
                resolved = key;
                accepted = !signal["private"].asBool() && !url.excluded && !url.isInternal;
                if (accepted) accepted = f.store.RecordBrowse(r);
                action = "page_observed";
            }
            else if (type == "launch")
            {
                LaunchEventRecord r;
                r.eventMs = (now + to) * 1000; r.exeName = Wide(signal, "exe_name"); r.exePath = Wide(signal, "exe_path");
                r.aumid = Wide(signal, "aumid");
                key = StoreUtil::AppEntityKey(r.exePath, r.aumid);
                resolved = key;
                accepted = f.store.RecordLaunch(r);
            }
            else if (type == "print")
            {
                accepted = PrintMonitor::IsOwnLocalJob(Wide(signal, "owner"), Wide(signal, "client"),
                    { L"CONTOSO\\Alice", L"alice@contoso.example" }, { L"WORKPC" });
                if (accepted) accepted = f.store.RecordPrint({ (now + to) * 1000, Wide(signal, "document"), L"Fixture printer", 1 });
            }
            else if (type == "purge")
            {
                f.Flush();
                feedCursor = f.store.CurrentChangeSeq();
                ActivityStore::PurgeSpec spec;
                spec.entityKey = EntityStore::NormalizeKey(Wide(signal, "key"));
                if (spec.entityKey.empty()) throw std::runtime_error("replay purge requires a key");
                int64_t removed = 0;
                accepted = f.store.Purge(spec, removed);
            }
            else if (type == "clear")
            {
                feedCursor = f.store.CurrentChangeSeq();
                accepted = f.store.ClearAll();
            }
            else throw std::runtime_error("unknown replay signal type");
            f.Flush();
            if (type != "purge" && type != "clear") accepted = accepted && RawCount(f.store, type) > beforeCount;
            else if (!accepted) throw std::runtime_error("replay control operation failed");
            if (!key.empty()) seenKeys.insert(key);
            auto d = WarpJson::Value::Object();
            d.set("id", signal["id"]);
            d.set("accepted", WarpJson::Value::Bool(accepted));
            d.set("entity_key", WarpJson::Value::String(accepted ? key : ""));
            d.set("resolved_key", WarpJson::Value::String(accepted ? resolved : ""));
            d.set("type", WarpJson::Value::String(type));
            d.set("action", WarpJson::Value::String(action));
            d.set("ambiguous", WarpJson::Value::Bool(ambiguous));
            decisions.push(std::move(d));
        }
        f.Flush();
        const auto begin = std::chrono::steady_clock::now();
        auto snapshot = f.Snapshot(window);
        const double elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
        std::string result;
        SnapshotInput history;
        history.ts = now;
        history.windowSecs = window;
        history.label = snapshot.summary.empty() ? "" : snapshot.summary[0];
        for (const auto& e : snapshot.entities)
            history.entities.push_back({ e.entityKey, e.entityType, EntityKey::FromUtf8(e.display), (double)e.activeSecs });
        snapshot.threadId = f.history.RecordSnapshot(history);
        WarpJson::Writer w(result);
        w.BeginObject().KV("schema_version", 1).KV("id", request["id"].asString()).KV("mode", "synthetic_replay")
         .KV("model_mode", models.empty() ? "disabled" : "granite_only").Key("decisions").Value(decisions)
         .Key("snapshot").Raw(ContextInferenceTestAccess::Json(snapshot)).Key("sessions").BeginArray();
        for (const auto& s : f.history.GetSessions(now - window, now, 1000))
            w.BeginObject().KV("start", s.startTs).KV("end", s.endTs).KV("active_secs", s.activeSecs)
             .KV("passive_secs", s.passiveSecs).KV("accounting_version", s.accountingVersion).EndObject();
        w.EndArray().Key("correlation").BeginObject().Key("membership").BeginObject();
        for (const auto& item : snapshot.items)
            if (!item.entityKey.empty() && item.threadId > 0) w.KV(item.entityKey, item.threadId);
        w.EndObject().Key("co_use").BeginArray();
        for (const auto& e : snapshot.entities)
            for (const auto& related : f.history.Related(e.entityKey, 100, now))
                if (e.entityKey < related.entityKey) w.BeginArray().String(e.entityKey).String(related.entityKey).EndArray();
        w.EndArray().EndObject().Key("lifecycle").BeginObject().KV("reset", f.entities.GetChanges(feedCursor, 100).reset)
         .Key("visible_keys").BeginArray();
        for (const auto& record : f.entities.Lookup(std::vector<std::string>(seenKeys.begin(), seenKeys.end()), now))
            if (!record.entityKey.empty()) w.String(record.entityKey);
        for (const auto& e : snapshot.entities) w.String(e.entityKey);
        w.EndArray().EndObject().Key("diagnostics").BeginObject().KV("compose_ms", elapsed)
         .KV("database_bytes", f.store.GetStats().dbBytes).EndObject().EndObject();
        out << result << '\n';
        if (!out) throw std::runtime_error("could not write replay output");
        ++cases;
    }
    if (!cases) throw std::runtime_error("empty replay corpus");
    return 0;
}

TEST(Quality, ReplayCannotOverwriteItsInput)
{
    wt::TempDir dir;
    const auto path = dir.File(L"labels.jsonl");
    { std::ofstream out{ std::filesystem::path(path) }; out << "fixture"; }
    bool rejected = false;
    try { RunQualityReplay(path, path, L""); }
    catch (const std::exception&) { rejected = true; }
    CHECK(rejected);
    std::ifstream in{ std::filesystem::path(path) };
    std::string contents;
    std::getline(in, contents);
    CHECK_EQ(contents, std::string("fixture"));
}

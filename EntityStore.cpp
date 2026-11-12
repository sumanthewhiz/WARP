#include "framework.h"
#include "EntityStore.h"
#include "sqlite3.h"
#include "EntityKey.h"
#include "UrlCanonicalizer.h"
#include "Logger.h"

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <cwctype>
#include <set>

using namespace StoreUtil;

namespace
{
    const int64_t kOpenDedupMs       = 120000;   // repeated passive opens / visits / launches inside this window count once
    const int64_t kEditDedupMs       = 60000;    // autosave bursts inside this window count once
    const size_t  kCacheMax          = 20000;
    const int64_t kDistRefreshSec    = 300;

    // Legacy v1 `recency_score` (0..255) - pinned by tests/python/test_recency_constants_sync.py.
    static const double TAU_SECONDS = 172800.0;
    static const double MAX_SCORE   = 200.0;
    static const double SCORE_CAP   = 255.0;

    // Columns shared by every inference read (order matters: see ReadRecord).
    const char* const kCols =
        "entity_key, entity_type, display_name, title, last_event_ts, last_open_ts, last_edit_ts, last_active_ts, "
        "open_count_total, edit_count_total, active_secs_total, distinct_days, last_day, "
        "eng_s, eng_s_ts, eng_l, eng_l_ts, change_seq, updated_at, deleted";

    std::string ColU(sqlite3_stmt* st, int i)
    {
        const unsigned char* t = sqlite3_column_text(st, i);
        return t ? std::string(reinterpret_cast<const char*>(t)) : std::string();
    }

    std::wstring ColW(sqlite3_stmt* st, int i)
    {
        return U2W(reinterpret_cast<const char*>(sqlite3_column_text(st, i)));
    }

    void BindU(sqlite3_stmt* st, int i, const std::string& s)
    {
        if (s.empty()) sqlite3_bind_null(st, i);
        else sqlite3_bind_text(st, i, s.c_str(), (int)s.size(), SQLITE_TRANSIENT);
    }

    void BindW(sqlite3_stmt* st, int i, const std::wstring& w)
    {
        if (w.empty()) { sqlite3_bind_null(st, i); return; }
        std::string u = W2U(w);
        sqlite3_bind_text(st, i, u.c_str(), (int)u.size(), SQLITE_TRANSIENT);
    }

    void ReadRecord(sqlite3_stmt* st, EntityRecord& r, int base = 0)
    {
        r.entityKey       = ColU(st, base + 0);
        r.entityType      = ColU(st, base + 1);
        r.displayName     = ColW(st, base + 2);
        r.title           = ColW(st, base + 3);
        r.lastEventTs     = sqlite3_column_int64(st, base + 4);
        r.lastOpenTs      = sqlite3_column_int64(st, base + 5);
        r.lastEditTs      = sqlite3_column_int64(st, base + 6);
        r.lastActiveTs    = sqlite3_column_int64(st, base + 7);
        r.openCountTotal  = sqlite3_column_double(st, base + 8);
        r.editCountTotal  = sqlite3_column_double(st, base + 9);
        r.activeSecsTotal = sqlite3_column_double(st, base + 10);
        r.distinctDays    = sqlite3_column_int(st, base + 11);
        r.lastDay         = sqlite3_column_int64(st, base + 12);
        r.engS            = sqlite3_column_double(st, base + 13);
        r.engSTs          = sqlite3_column_int64(st, base + 14);
        r.engL            = sqlite3_column_double(st, base + 15);
        r.engLTs          = sqlite3_column_int64(st, base + 16);
        r.changeSeq       = sqlite3_column_int64(st, base + 17);
        r.updatedAt       = sqlite3_column_int64(st, base + 18);
        r.deleted         = sqlite3_column_int(st, base + 19) != 0;
    }

    // Exponentially-decayed accumulator: value(t) = s * e^{-(t - t0)/tau}.
    void DecayAdd(double& s, int64_t& t0, double w, int64_t t, double tau)
    {
        if (w <= 0.0) return;
        if (s <= 0.0 || t0 == 0) { s = w; t0 = t; return; }
        if (t >= t0) { s = s * std::exp(-(double)(t - t0) / tau) + w; t0 = t; }
        else         { s += w * std::exp(-(double)(t0 - t) / tau); }       // late / backfilled event
    }

    void DecayMerge(double& s, int64_t& t0, double s2, int64_t t2, double tau)
    {
        if (s2 <= 0.0 || t2 == 0) return;
        if (s <= 0.0 || t0 == 0) { s = s2; t0 = t2; return; }
        int64_t t = (std::max)(t0, t2);
        s = s * std::exp(-(double)(t - t0) / tau) + s2 * std::exp(-(double)(t - t2) / tau);
        t0 = t;
    }

    double DecayAt(double s, int64_t t0, int64_t now, double tau)
    {
        if (s <= 0.0 || t0 == 0) return 0.0;
        return s * std::exp(-(double)(std::max<int64_t>(0, now - t0)) / tau);
    }

    double Clamp01(double v) { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); }

    std::string HorizonOf(const std::string& h)
    {
        if (h == "short" || h == "long") return h;
        return "blend";
    }

    double PriorFor(const std::string& horizon, double shortTerm, double longTerm)
    {
        if (horizon == "short") return shortTerm;
        if (horizon == "long") return longTerm;
        return 0.7 * shortTerm + 0.3 * longTerm * (kTauShortSec / kTauLongSec);
    }

    std::string EscapeLike(const std::string& s)
    {
        std::string out;
        for (char c : s)
        {
            if (c == '^' || c == '%' || c == '_') out += '^';
            out += c;
        }
        return out;
    }

    bool Exec(sqlite3* db, const char* sql)
    {
        return sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
    }

    std::string RelAge(int64_t secs)
    {
        if (secs < 90) return "just now";
        if (secs < 3600) return std::to_string(secs / 60) + " min ago";
        if (secs < 2 * 86400) return std::to_string(secs / 3600) + " h ago";
        return std::to_string(secs / 86400) + " days ago";
    }
}

// =====================================================================
// Static helpers
// =====================================================================
EntityStore::EntityStore(ActivityStore* store) : m_store(store) {}

std::string EntityStore::NormalizeKey(const std::wstring& raw)
{
    if (raw.empty()) return std::string();
    // Keys WARP itself hands out round-trip unchanged: packaged apps and document
    // identities produced by UrlCanonicalizer ("yt:", "gdoc:", "gh:owner/repo#pr12", ...).
    static const wchar_t* const kOpaquePrefixes[] = {
        L"aumid:", L"doc:", L"yt:", L"gdoc:", L"gdrive:", L"spo:", L"spo-path:", L"od:", L"gh:", L"ado:", L"figma:", L"jira:",
    };
    for (const wchar_t* p : kOpaquePrefixes)
    {
        const size_t n = wcslen(p);
        if (raw.size() > n && _wcsnicmp(raw.c_str(), p, n) == 0)
            return _wcsicmp(p, L"aumid:") == 0 ? EntityKey::Key(raw) : EntityKey::DocKey(raw);
    }
    // Windows paths are decided before any URL heuristics.
    const bool drivePath = raw.size() >= 3 && std::iswalpha(raw[0]) && raw[1] == L':' && (raw[2] == L'\\' || raw[2] == L'/');
    const bool uncPath = raw.size() >= 2 && (raw[0] == L'\\' || raw[0] == L'/') && (raw[1] == L'\\' || raw[1] == L'/');
    if (drivePath || uncPath) return EntityKey::FileKey(raw);
    // Packaged-app identity: "Family_hash!AppId" (no path separators; bare domains never contain '!').
    if (raw.find(L'!') != std::wstring::npos && raw.find(L'\\') == std::wstring::npos && raw.find(L'/') == std::wstring::npos)
        return EntityKey::AppKey(raw);
    if (UrlCanon::LooksLikeUrl(raw))
    {
        UrlCanon::Result r = UrlCanon::Canonicalize(raw);
        if (r.excluded || (r.canonical.empty() && r.docId.empty())) return std::string();
        return WebEntityKey(r.canonical, r.docId, std::wstring());
    }
    return EntityKey::FileKey(raw);
}

bool EntityStore::IsTransientFileName(const std::wstring& path)
{
    size_t slash = path.find_last_of(L"\\/");
    std::wstring name = slash == std::wstring::npos ? path : path.substr(slash + 1);
    if (name.empty()) return true;
    std::wstring n;
    n.reserve(name.size());
    for (wchar_t c : name) n.push_back((wchar_t)std::towlower(c));

    static const wchar_t* const kPrefixes[] = { L"~$", L"~wrl", L"~rf", L"~df", L".~lock.", L".#", L"~" };
    for (const wchar_t* p : kPrefixes)
        if (n.compare(0, wcslen(p), p) == 0) return true;

    static const wchar_t* const kSuffixes[] = {
        L".tmp", L".temp", L".crdownload", L".partial", L".part", L".download", L".opdownload",
        L".swp", L".swo", L".swx", L"~", L".lock", L".lck", L".ldb", L".laccdb", L".asd", L".wbk",
        L".~tmp", L".bak", L".dwl", L".dwl2", L".sb-", L".!ut", L".dtmp",
    };
    for (const wchar_t* s : kSuffixes)
    {
        size_t len = wcslen(s);
        if (n.size() >= len && n.compare(n.size() - len, len, s) == 0) return true;
    }
    // Office / Adobe autosave & atomic-save names, e.g. "report.docx.sb-1a2b3c" or "4A3F1B2C" (8 hex, no extension)
    if (n.find(L".sb-") != std::wstring::npos) return true;
    if (n.size() == 8 && n.find(L'.') == std::wstring::npos &&
        std::all_of(n.begin(), n.end(), [](wchar_t c) { return std::iswxdigit(c) != 0; }))
        return true;
    return false;
}

double EntityStore::LegacyRecencyScore(int64_t nowUtc, int64_t lastOpenTs, double openCount7d)
{
    if (lastOpenTs <= 0) return 0.0;
    double score = MAX_SCORE * std::exp(-(double)(std::max<int64_t>)(0, nowUtc - lastOpenTs) / TAU_SECONDS);
    score += std::log(1.0 + (std::max)(0.0, openCount7d)) * 5.0;
    if (score > SCORE_CAP) score = SCORE_CAP;
    if (score < 0.0) score = 0.0;
    return score;
}

// =====================================================================
// Writer side (writer thread only)
// =====================================================================
void EntityStore::BeginEvent(WriterContext& w)
{
    if (m_cache.size() > kCacheMax)
        m_cache.clear();                 // everything is persisted; this only drops warm state
    FinalizePendingDeletes(w, false);
}

EntityStore::CacheEntry* EntityStore::Get(WriterContext& w, const std::string& key)
{
    auto it = m_cache.find(key);
    if (it != m_cache.end())
        return it->second.exists ? &it->second : nullptr;

    static const std::string sql = std::string("SELECT ") + kCols + " FROM inference WHERE entity_key = ?1;";
    sqlite3_stmt* st = w.Stmt(sql.c_str());
    if (!st) return nullptr;
    BindU(st, 1, key);
    if (sqlite3_step(st) != SQLITE_ROW) return nullptr;
    CacheEntry e;
    ReadRecord(st, e.rec);
    e.exists = true;
    e.ftsText = e.rec.displayName + L" " + e.rec.title;
    auto res = m_cache.emplace(key, std::move(e));
    return &res.first->second;
}

EntityStore::CacheEntry& EntityStore::GetOrCreate(WriterContext& w, const std::string& key, const char* type)
{
    if (std::strcmp(type, "url") == 0)
    {
        // A newly observed case variant makes a previously unique v2 alias ambiguous.
        if (auto* alias = w.Stmt("UPDATE entity_alias SET entity_key=NULL WHERE alias_key=?1 AND entity_key<>?2;"))
        {
            BindU(alias, 1, EntityKey::Key(EntityKey::FromUtf8(key)));
            BindU(alias, 2, key);
            sqlite3_step(alias);
            sqlite3_reset(alias);
        }
    }
    if (CacheEntry* e = Get(w, key))
    {
        if (e->rec.entityType.empty()) e->rec.entityType = type;
        return *e;
    }
    CacheEntry fresh;
    fresh.rec.entityKey = key;
    fresh.rec.entityType = type;
    auto res = m_cache.insert_or_assign(key, std::move(fresh));
    return res.first->second;
}

void EntityStore::SetDisplay(CacheEntry& e, const std::wstring& display, const std::wstring& title)
{
    if (!display.empty()) e.rec.displayName = display;
    if (!title.empty()) e.rec.title = title;
}

void EntityStore::Credit(WriterContext& w, CacheEntry& e, double weight, int64_t ts,
                         double opens, double edits, double activeSecs, int sessions)
{
    int64_t day = LocalDayIndex(ts);

    bool newDay = true;
    {
        sqlite3_stmt* st = w.Stmt("SELECT 1 FROM entity_day WHERE entity_key = ?1 AND day = ?2;");
        if (st)
        {
            BindU(st, 1, e.rec.entityKey);
            sqlite3_bind_int64(st, 2, day);
            newDay = sqlite3_step(st) != SQLITE_ROW;
        }
    }
    if (newDay)
    {
        // A "came back to it on another day" bonus, proportional to how strong today's first evidence is,
        // so a single low-confidence background touch can't earn a full habit bonus.
        weight += kWNewDay * (std::min)(1.0, weight);
        ++e.rec.distinctDays;
    }
    if (day > e.rec.lastDay) e.rec.lastDay = day;

    DecayAdd(e.rec.engS, e.rec.engSTs, weight, ts, kTauShortSec);
    DecayAdd(e.rec.engL, e.rec.engLTs, weight, ts, kTauLongSec);

    sqlite3_stmt* up = w.Stmt(
        "INSERT INTO entity_day (entity_key, day, opens, edits, active_secs, sessions) VALUES (?1, ?2, ?3, ?4, ?5, ?6) "
        "ON CONFLICT(entity_key, day) DO UPDATE SET opens = opens + excluded.opens, edits = edits + excluded.edits, "
        "active_secs = active_secs + excluded.active_secs, sessions = sessions + excluded.sessions;");
    if (up)
    {
        BindU(up, 1, e.rec.entityKey);
        sqlite3_bind_int64(up, 2, day);
        sqlite3_bind_double(up, 3, opens);
        sqlite3_bind_double(up, 4, edits);
        sqlite3_bind_double(up, 5, activeSecs);
        sqlite3_bind_int(up, 6, sessions);
        sqlite3_step(up);
    }
    if (ts > e.rec.lastEventTs) e.rec.lastEventTs = ts;
}

void EntityStore::Persist(WriterContext& w, CacheEntry& e)
{
    EntityRecord& r = e.rec;
    r.deleted = false;
    r.changeSeq = w.NextChangeSeq();
    r.updatedAt = w.nowMs / 1000;

    sqlite3_stmt* st = w.Stmt(
        "INSERT INTO inference (entity_key, entity_type, display_name, title, last_event_ts, last_open_ts, last_edit_ts, last_active_ts, "
        "open_count_total, edit_count_total, active_secs_total, distinct_days, last_day, "
        "eng_s, eng_s_ts, eng_s_key, eng_l, eng_l_ts, eng_l_key, recency_score, version, change_seq, updated_at, deleted) "
        "VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,?17,?18,?19,?20,?21,?21,?22,0) "
        "ON CONFLICT(entity_key) DO UPDATE SET entity_type = excluded.entity_type, display_name = excluded.display_name, "
        "title = excluded.title, last_event_ts = excluded.last_event_ts, last_open_ts = excluded.last_open_ts, "
        "last_edit_ts = excluded.last_edit_ts, last_active_ts = excluded.last_active_ts, open_count_total = excluded.open_count_total, "
        "edit_count_total = excluded.edit_count_total, active_secs_total = excluded.active_secs_total, "
        "distinct_days = excluded.distinct_days, last_day = excluded.last_day, eng_s = excluded.eng_s, eng_s_ts = excluded.eng_s_ts, "
        "eng_s_key = excluded.eng_s_key, eng_l = excluded.eng_l, eng_l_ts = excluded.eng_l_ts, eng_l_key = excluded.eng_l_key, "
        "recency_score = excluded.recency_score, version = excluded.version, change_seq = excluded.change_seq, "
        "updated_at = excluded.updated_at, deleted = 0;");
    if (!st) return;
    BindU(st, 1, r.entityKey);
    BindU(st, 2, r.entityType);
    BindW(st, 3, r.displayName);
    BindW(st, 4, r.title);
    sqlite3_bind_int64(st, 5, r.lastEventTs);
    sqlite3_bind_int64(st, 6, r.lastOpenTs);
    sqlite3_bind_int64(st, 7, r.lastEditTs);
    sqlite3_bind_int64(st, 8, r.lastActiveTs);
    sqlite3_bind_double(st, 9, r.openCountTotal);
    sqlite3_bind_double(st, 10, r.editCountTotal);
    sqlite3_bind_double(st, 11, r.activeSecsTotal);
    sqlite3_bind_int(st, 12, r.distinctDays);
    sqlite3_bind_int64(st, 13, r.lastDay);
    sqlite3_bind_double(st, 14, r.engS);
    sqlite3_bind_int64(st, 15, r.engSTs);
    sqlite3_bind_double(st, 16, DecayKey(r.engS, r.engSTs, kTauShortSec));
    sqlite3_bind_double(st, 17, r.engL);
    sqlite3_bind_int64(st, 18, r.engLTs);
    sqlite3_bind_double(st, 19, DecayKey(r.engL, r.engLTs, kTauLongSec));
    // Stored recency_score is a convenience snapshot for direct-SQL readers only; the API recomputes it at query time.
    sqlite3_bind_double(st, 20, LegacyRecencyScore(r.updatedAt, r.lastOpenTs, 0.0));
    sqlite3_bind_int64(st, 21, r.changeSeq);
    sqlite3_bind_int64(st, 22, r.updatedAt);
    if (sqlite3_step(st) != SQLITE_DONE)
    {
        WARP_LOG_WARN("entity", "persist failed: %s", sqlite3_errmsg(w.db));
        return;
    }
    e.exists = true;

    std::wstring text = r.displayName + L" " + r.title;
    if (w.ftsAvailable && text != e.ftsText)
    {
        sqlite3_stmt* del = w.Stmt("DELETE FROM activity_fts WHERE rowid = (SELECT rowid FROM inference WHERE entity_key = ?1);");
        if (del) { BindU(del, 1, r.entityKey); sqlite3_step(del); }
        sqlite3_stmt* ins = w.Stmt(
            "INSERT INTO activity_fts (rowid, kind, ref, ts, text) SELECT rowid, entity_type, entity_key, last_event_ts, ?2 "
            "FROM inference WHERE entity_key = ?1;");
        if (ins)
        {
            BindU(ins, 1, r.entityKey);
            BindW(ins, 2, text);
            sqlite3_step(ins);
        }
        e.ftsText = text;
    }
}

void EntityStore::MarkPendingDelete(WriterContext& w, const std::string& key)
{
    CacheEntry* e = Get(w, key);
    if (!e) return;
    e->rec.deleted = true;
    // Hidden from readers immediately; becomes a tombstone only if not recreated within the grace window.
    sqlite3_stmt* st = w.Stmt("UPDATE inference SET deleted = 1, updated_at = ?2 WHERE entity_key = ?1;");
    if (st)
    {
        BindU(st, 1, key);
        sqlite3_bind_int64(st, 2, w.nowMs / 1000);
        sqlite3_step(st);
    }
    m_pendingDeletes[key] = w.nowMs;
}

void EntityStore::FinalizePendingDeletes(WriterContext& w, bool force)
{
    if (m_pendingDeletes.empty()) return;
    if (!force && w.nowMs - m_lastPendingSweepMs < 2000) return;
    m_lastPendingSweepMs = w.nowMs;
    std::vector<std::string> due;
    for (const auto& kv : m_pendingDeletes)
        if (w.nowMs - kv.second >= m_deleteGraceMs.load())
            due.push_back(kv.first);
    for (const std::string& k : due)
    {
        std::string type = "file";
        auto it = m_cache.find(k);
        if (it != m_cache.end() && !it->second.rec.entityType.empty()) type = it->second.rec.entityType;
        m_pendingDeletes.erase(k);
        m_store->Tombstone(w, k, type, "deleted");     // also evicts the cache entry via OnEntityRemoved
    }
}

std::vector<std::string> EntityStore::KeysUnderFolder(WriterContext& w, const std::wstring& folder, int cap)
{
    std::vector<std::string> keys;
    std::string prefix = EntityKey::FileKey(folder);
    while (prefix.size() > 3 && prefix.back() == '\\') prefix.pop_back();
    if (prefix.empty()) return keys;
    std::string pat = EscapeLike(prefix);
    if (!pat.empty() && pat.back() == '\\') pat.pop_back();
    sqlite3_stmt* st = w.Stmt("SELECT entity_key FROM inference WHERE entity_type = 'file' AND entity_key LIKE ?1 || '\\%' ESCAPE '^' LIMIT ?2;");
    if (!st) return keys;
    BindU(st, 1, pat);
    sqlite3_bind_int(st, 2, cap);
    while (sqlite3_step(st) == SQLITE_ROW) keys.push_back(ColU(st, 0));
    return keys;
}

void EntityStore::RenameEntity(WriterContext& w, const std::string& oldKey, const std::string& newKey,
                               const std::wstring& newPath, int64_t ts)
{
    if (oldKey.empty() || newKey.empty()) return;
    m_pendingDeletes.erase(newKey);
    if (oldKey == newKey)
    {
        // Case-only rename: same entity, refresh the display path.
        if (CacheEntry* e = Get(w, newKey))
        {
            SetDisplay(*e, newPath, std::wstring());
            if (ts > e->rec.lastEventTs) e->rec.lastEventTs = ts;
            Persist(w, *e);
        }
        return;
    }

    EntityRecord old;
    bool hadOld = false;
    if (CacheEntry* o = Get(w, oldKey)) { old = o->rec; hadOld = true; }
    m_pendingDeletes.erase(oldKey);

    CacheEntry& n = GetOrCreate(w, newKey, "file");
    SetDisplay(n, newPath, std::wstring());
    if (hadOld)
    {
        EntityRecord& r = n.rec;
        r.openCountTotal  += old.openCountTotal;
        r.editCountTotal  += old.editCountTotal;
        r.activeSecsTotal += old.activeSecsTotal;
        r.lastOpenTs   = (std::max)(r.lastOpenTs, old.lastOpenTs);
        r.lastEditTs   = (std::max)(r.lastEditTs, old.lastEditTs);
        r.lastActiveTs = (std::max)(r.lastActiveTs, old.lastActiveTs);
        r.lastEventTs  = (std::max)(r.lastEventTs, old.lastEventTs);
        DecayMerge(r.engS, r.engSTs, old.engS, old.engSTs, kTauShortSec);
        DecayMerge(r.engL, r.engLTs, old.engL, old.engLTs, kTauLongSec);
        if (r.title.empty()) r.title = old.title;

        static const char* const kMove[] = {
            "INSERT INTO entity_day (entity_key, day, opens, edits, active_secs, sessions) "
            "SELECT ?2, day, opens, edits, active_secs, sessions FROM entity_day WHERE entity_key = ?1 "
            "ON CONFLICT(entity_key, day) DO UPDATE SET opens = opens + excluded.opens, edits = edits + excluded.edits, "
            "active_secs = active_secs + excluded.active_secs, sessions = sessions + excluded.sessions;",
            "DELETE FROM entity_day WHERE entity_key = ?1;",
            "UPDATE OR IGNORE edge SET a = ?2 WHERE a = ?1;",
            "UPDATE OR IGNORE edge SET b = ?2 WHERE b = ?1;",
            "DELETE FROM edge WHERE a = ?1 OR b = ?1 OR a = b;",
            "UPDATE OR IGNORE thread_member SET entity_key = ?2 WHERE entity_key = ?1;",
            "DELETE FROM thread_member WHERE entity_key = ?1;",
            "UPDATE app_focus_activity SET doc_key = ?2 WHERE doc_key = ?1;",
        };
        for (const char* sql : kMove)
        {
            sqlite3_stmt* st = w.Stmt(sql);
            if (!st) continue;
            BindU(st, 1, oldKey);
            BindU(st, 2, newKey);
            sqlite3_step(st);
        }
        sqlite3_stmt* agg = w.Stmt("SELECT COUNT(*), COALESCE(MAX(day), 0) FROM entity_day WHERE entity_key = ?1;");
        if (agg)
        {
            BindU(agg, 1, newKey);
            if (sqlite3_step(agg) == SQLITE_ROW)
            {
                r.distinctDays = sqlite3_column_int(agg, 0);
                r.lastDay = sqlite3_column_int64(agg, 1);
            }
        }
        m_store->Tombstone(w, oldKey, old.entityType.empty() ? "file" : old.entityType, "renamed");
    }
    if (ts > n.rec.lastEventTs) n.rec.lastEventTs = ts;
    Persist(w, n);
}

void EntityStore::OnFileEvent(WriterContext& w, const FileEventRecord& r)
{
    // An untimed recency-list entry is evidence that the file is known, not a
    // fresh open at the poll time. Keep its raw evidence without engagement credit.
    if (r.ctx.evidence.timestampKind=="recency_observation") return;
    BeginEvent(w);
    const int64_t ts = r.eventMs / 1000;
    const double conf = Clamp01(r.ctx.confidence);
    const std::wstring& a = r.action;

    if (a == L"RENAME")
    {
        if (r.isDir)
        {
            // Folder rename/move: re-key everything we know under it (bounded).
            std::wstring newDir = r.path;
            while (newDir.size() > 3 && (newDir.back() == L'\\' || newDir.back() == L'/')) newDir.pop_back();
            std::wstring oldDir = r.oldPath;
            while (oldDir.size() > 3 && (oldDir.back() == L'\\' || oldDir.back() == L'/')) oldDir.pop_back();
            for (const std::string& k : KeysUnderFolder(w, r.oldPath, 5000))
            {
                CacheEntry* e = Get(w, k);
                std::wstring oldDisplay = e ? e->rec.displayName : std::wstring();
                std::wstring newPath = (oldDisplay.size() > oldDir.size()) ? newDir + oldDisplay.substr(oldDir.size()) : std::wstring();
                std::string newKey = newPath.empty() ? std::string() : EntityKey::FileKey(newPath);
                if (!newKey.empty()) RenameEntity(w, k, newKey, newPath, ts);
            }
            return;
        }
        bool srcTransient = IsTransientFileName(r.oldPath);
        bool dstTransient = IsTransientFileName(r.path);
        if (dstTransient)
        {
            // "report.docx" -> "~WRL0002.tmp": the original is about to be replaced (safe-save) or really
            // removed. Hide it, but keep its history if it reappears within the grace window.
            if (!srcTransient) MarkPendingDelete(w, EntityKey::FileKey(r.oldPath));
            return;
        }
        if (srcTransient)
        {
            // "~WRL0001.tmp" -> "report.docx": an atomic save of the destination.
            FileEventRecord save = r;
            save.action = L"MODIFY";
            save.oldPath.clear();
            OnFileEvent(w, save);
            return;
        }
        RenameEntity(w, EntityKey::FileKey(r.oldPath), EntityKey::FileKey(r.path), r.path, ts);
        return;
    }

    if (a == L"DELETE")
    {
        if (r.isDir)
        {
            for (const std::string& k : KeysUnderFolder(w, r.path, 5000))
                MarkPendingDelete(w, k);
        }
        else if (!IsTransientFileName(r.path))
        {
            MarkPendingDelete(w, EntityKey::FileKey(r.path));
        }
        return;
    }

    if (r.isDir || IsTransientFileName(r.path)) return;
    std::string key = EntityKey::FileKey(r.path);
    if (key.empty()) return;
    m_pendingDeletes.erase(key);                // recreated within the grace window: same document

    const bool explicitSrc = r.source == L"shell" || r.source == L"recent" || r.source == L"mru" || r.source == L"jumplist";
    CacheEntry& e = GetOrCreate(w, key, "file");
    SetDisplay(e, r.path, std::wstring());

    if (a == L"OPEN")
    {
        if (ts > e.rec.lastOpenTs) e.rec.lastOpenTs = ts;
        bool dup = !explicitSrc && e.lastOpenCreditMs && (r.eventMs - e.lastOpenCreditMs) < kOpenDedupMs;
        if (!dup)
        {
            e.rec.openCountTotal += conf;
            Credit(w, e, (explicitSrc ? kWOpenExplicit : kWOpenPassive) * conf, ts, conf, 0.0, 0.0, 0);
            e.lastOpenCreditMs = r.eventMs;
        }
    }
    else if (a == L"CREATE" || a == L"MODIFY" || a == L"SAVE")
    {
        if (ts > e.rec.lastEditTs) e.rec.lastEditTs = ts;
        bool dup = e.lastEditCreditMs && (r.eventMs - e.lastEditCreditMs) < kEditDedupMs;
        if (!dup)
        {
            e.rec.editCountTotal += conf;
            Credit(w, e, kWEdit * conf, ts, 0.0, conf, 0.0, 0);
            e.lastEditCreditMs = r.eventMs;
        }
    }
    else if (a == L"DOWNLOAD")
    {
        if (ts > e.rec.lastOpenTs) e.rec.lastOpenTs = ts;
        e.rec.openCountTotal += conf;
        Credit(w, e, kWOpenExplicit * conf, ts, conf, 0.0, 0.0, 0);
    }
    else
    {
        return;
    }
    if (ts > e.rec.lastEventTs) e.rec.lastEventTs = ts;
    Persist(w, e);
}

void EntityStore::OnLaunchEvent(WriterContext& w, const LaunchEventRecord& r)
{
    BeginEvent(w);
    std::string key = AppEntityKey(r.exePath, r.aumid);
    if (key.empty()) return;
    const int64_t ts = r.eventMs / 1000;
    const double conf = Clamp01(r.ctx.confidence);
    CacheEntry& e = GetOrCreate(w, key, "app");
    SetDisplay(e, r.aumid.empty() ? r.exePath : r.aumid, r.exeName);
    if (ts > e.rec.lastOpenTs) e.rec.lastOpenTs = ts;
    bool dup = e.lastOpenCreditMs && (r.eventMs - e.lastOpenCreditMs) < 60000;
    if (!dup)
    {
        e.rec.openCountTotal += conf;
        Credit(w, e, kWOpenExplicit * conf, ts, conf, 0.0, 0.0, 0);
        e.lastOpenCreditMs = r.eventMs;
    }
    if (ts > e.rec.lastEventTs) e.rec.lastEventTs = ts;
    Persist(w, e);
}

void EntityStore::OnBrowseEvent(WriterContext& w, const BrowseEventRecord& r)
{
    if (r.ctx.evidence.timestampKind=="recency_observation") return;
    BeginEvent(w);
    if (r.url.empty() && r.docId.empty()) return;   // title-only rows are too ambiguous to be ranked entities
    std::string key = WebEntityKey(r.url, r.docId, r.title);
    if (key.empty()) return;
    const int64_t ts = r.eventMs / 1000;
    const double conf = Clamp01(r.ctx.confidence);
    CacheEntry& e = GetOrCreate(w, key, "url");
    SetDisplay(e, r.url.empty() ? r.title : r.url, r.title);
    if (ts > e.rec.lastOpenTs) e.rec.lastOpenTs = ts;
    bool dup = e.lastOpenCreditMs && (r.eventMs - e.lastOpenCreditMs) < 60000;
    if (!dup)
    {
        e.rec.openCountTotal += conf;
        Credit(w, e, kWVisit * conf, ts, conf, 0.0, 0.0, 0);
        e.lastOpenCreditMs = r.eventMs;
    }
    if (ts > e.rec.lastEventTs) e.rec.lastEventTs = ts;
    Persist(w, e);
}

void EntityStore::OnFocusEvent(WriterContext& w, const FocusEventRecord& r)
{
    BeginEvent(w);
    if (r.activeSecs < 2) return;                    // alt-tab fly-throughs are not engagement
    const int64_t ts = r.endMs / 1000;
    const double conf = Clamp01(r.ctx.confidence);
    const double minutes = (std::min)(r.activeSecs / 60.0, kMaxActiveMinutesPerSession);
    const double weight = kWActivePerMin * minutes * conf;

    std::string appKey = AppEntityKey(r.exePath, r.aumid);
    if (!appKey.empty())
    {
        CacheEntry& e = GetOrCreate(w, appKey, "app");
        SetDisplay(e, r.aumid.empty() ? r.exePath : r.aumid, r.exeName);
        if (ts > e.rec.lastActiveTs) e.rec.lastActiveTs = ts;
        e.rec.activeSecsTotal += r.activeSecs;
        Credit(w, e, weight, ts, 0.0, 0.0, (double)r.activeSecs, 1);
        Persist(w, e);
    }

    if (!r.docKey.empty())
    {
        const char* type = r.docType == "file" ? "file" : "url";
        if (r.docType == "file" && IsTransientFileName(r.docDisplay)) return;
        m_pendingDeletes.erase(r.docKey);
        CacheEntry& d = GetOrCreate(w, r.docKey, type);
        SetDisplay(d, r.docDisplay, r.docType == "file" ? std::wstring() : (d.rec.title.empty() ? r.windowTitle : std::wstring()));
        if (ts > d.rec.lastActiveTs) d.rec.lastActiveTs = ts;
        d.rec.activeSecsTotal += r.activeSecs;
        Credit(w, d, weight, ts, 0.0, 0.0, (double)r.activeSecs, 1);
        Persist(w, d);
    }
}

void EntityStore::OnMaintenance(WriterContext& w, int64_t nowUtc)
{
    FinalizePendingDeletes(w, true);

    // Rows left hidden by a previous run that ended inside a delete grace window.
    std::vector<std::pair<std::string, std::string>> stale;
    {
        sqlite3_stmt* st = w.Stmt("SELECT entity_key, entity_type FROM inference WHERE deleted = 1 AND updated_at < ?1 LIMIT 5000;");
        if (st)
        {
            sqlite3_bind_int64(st, 1, nowUtc - m_deleteGraceMs.load() / 1000);
            while (sqlite3_step(st) == SQLITE_ROW) stale.emplace_back(ColU(st, 0), ColU(st, 1));
        }
    }
    for (const auto& kv : stale)
        if (m_pendingDeletes.find(kv.first) == m_pendingDeletes.end())
            m_store->Tombstone(w, kv.first, kv.second, "deleted");

    // Keep the legacy rolling-count columns fresh for direct-SQL readers (the API derives them live).
    sqlite3_stmt* upd = w.Stmt(
        "UPDATE inference SET "
        "open_count_7d  = COALESCE((SELECT SUM(d.opens) FROM entity_day d WHERE d.entity_key = inference.entity_key AND d.day > ?1 - 7), 0), "
        "open_count_30d = COALESCE((SELECT SUM(d.opens) FROM entity_day d WHERE d.entity_key = inference.entity_key AND d.day > ?1 - 30), 0) "
        "WHERE deleted = 0;");
    if (upd)
    {
        sqlite3_bind_int64(upd, 1, LocalDayIndex(nowUtc));
        sqlite3_step(upd);
    }
    m_cache.clear();
    std::lock_guard<std::mutex> lk(m_distMtx);
    m_distBuiltUtc = 0;
}

void EntityStore::OnClearAll(WriterContext&)
{
    m_cache.clear();
    m_pendingDeletes.clear();
    std::lock_guard<std::mutex> lk(m_distMtx);
    m_dist.clear();
    m_distBuiltUtc = 0;
}

void EntityStore::OnRollback(WriterContext&)
{
    m_cache.clear();
    m_pendingDeletes.clear();
}

void EntityStore::OnEntityRemoved(WriterContext&, const std::string& entityKey)
{
    m_cache.erase(entityKey);
    m_pendingDeletes.erase(entityKey);
}

// =====================================================================
// Read side (any thread)
// =====================================================================
namespace
{
    // Live 7/30-day rollups for a batch of records (one prepared statement per call).
    class DerivedReader
    {
    public:
        DerivedReader(sqlite3* db, int64_t nowUtc) : m_today(LocalDayIndex(nowUtc)), m_now(nowUtc)
        {
            sqlite3_prepare_v2(db,
                "SELECT COALESCE(SUM(CASE WHEN day > ?2 - 7 THEN opens END), 0), COALESCE(SUM(opens), 0), "
                "COALESCE(SUM(CASE WHEN day > ?2 - 7 THEN edits END), 0), COALESCE(SUM(CASE WHEN day > ?2 - 7 THEN active_secs END), 0), "
                "COUNT(CASE WHEN day > ?2 - 7 THEN 1 END) FROM entity_day WHERE entity_key = ?1 AND day > ?2 - 30;",
                -1, &m_st, nullptr);
        }
        ~DerivedReader() { if (m_st) sqlite3_finalize(m_st); }
        DerivedReader(const DerivedReader&) = delete;
        DerivedReader& operator=(const DerivedReader&) = delete;

        void Fill(EntityRecord& r)
        {
            if (m_st && !r.entityKey.empty())
            {
                sqlite3_reset(m_st);
                BindU(m_st, 1, r.entityKey);
                sqlite3_bind_int64(m_st, 2, m_today);
                if (sqlite3_step(m_st) == SQLITE_ROW)
                {
                    r.openCount7d  = sqlite3_column_double(m_st, 0);
                    r.openCount30d = sqlite3_column_double(m_st, 1);
                    r.edits7d      = sqlite3_column_double(m_st, 2);
                    r.activeSecs7d = sqlite3_column_double(m_st, 3);
                    r.activeDays7  = sqlite3_column_int(m_st, 4);
                }
            }
            r.recencyScore = EntityStore::LegacyRecencyScore(m_now, r.lastOpenTs, r.openCount7d);
        }

    private:
        sqlite3_stmt* m_st = nullptr;
        int64_t       m_today = 0;
        int64_t       m_now = 0;
    };

    std::vector<std::string> Tokenize(const std::wstring& q, size_t maxTokens)
    {
        std::vector<std::string> out;
        std::wstring cur;
        auto flush = [&]()
        {
            if (!cur.empty() && out.size() < maxTokens) out.push_back(EntityKey::Key(cur));
            cur.clear();
        };
        for (wchar_t c : q)
        {
            if (std::iswalnum(c) || c > 0x7F) cur.push_back(c);
            else flush();
        }
        flush();
        return out;
    }
}

bool EntityStore::FtsAvailable()
{
    int v = m_fts.load();
    if (v >= 0) return v == 1;
    std::string val;
    if (!m_store->GetMeta("fts_available", val)) return false;   // transient (pool busy / closed): ask again next time
    const bool ok = val == "1";
    m_fts = ok ? 1 : 0;
    return ok;
}

void EntityStore::EnsureDistribution(int64_t nowUtc)
{
    {
        std::lock_guard<std::mutex> lk(m_distMtx);
        if (m_distBuiltUtc && nowUtc - m_distBuiltUtc < kDistRefreshSec && nowUtc >= m_distBuiltUtc) return;
    }
    // Build without holding m_distMtx (it is also taken by scorers that may hold read leases).
    std::unordered_map<std::string, std::vector<double>> dist;
    {
        ActivityStore::ReadLease rl = m_store->AcquireReader();
        if (!rl) return;
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(rl.get(), "SELECT entity_type, eng_s, eng_s_ts, eng_l, eng_l_ts FROM inference WHERE deleted = 0;", -1, &st, nullptr) != SQLITE_OK)
            return;
        while (sqlite3_step(st) == SQLITE_ROW)
        {
            std::string type = ColU(st, 0);
            double s = DecayAt(sqlite3_column_double(st, 1), sqlite3_column_int64(st, 2), nowUtc, kTauShortSec);
            double l = DecayAt(sqlite3_column_double(st, 3), sqlite3_column_int64(st, 4), nowUtc, kTauLongSec);
            dist[type + "|short"].push_back(s);
            dist[type + "|long"].push_back(l);
            dist[type + "|blend"].push_back(PriorFor("blend", s, l));
        }
        sqlite3_finalize(st);
    }
    for (auto& kv : dist) std::sort(kv.second.begin(), kv.second.end());
    std::lock_guard<std::mutex> lk(m_distMtx);
    m_dist.swap(dist);
    m_distBuiltUtc = nowUtc;
}

double EntityStore::Percentile(const std::string& type, const std::string& horizon, double prior)
{
    if (prior <= 0.0) return 0.0;
    std::lock_guard<std::mutex> lk(m_distMtx);
    auto it = m_dist.find(type + "|" + horizon);
    if (it == m_dist.end() || it->second.empty()) return 0.5;
    const std::vector<double>& v = it->second;
    size_t rank = (size_t)(std::upper_bound(v.begin(), v.end(), prior) - v.begin());
    return Clamp01((double)rank / (double)v.size());
}

void EntityStore::FillReasons(EntityScore& s, int64_t nowUtc)
{
    const EntityRecord& r = s.rec;
    std::vector<std::string>& out = s.reasons;
    out.clear();
    int64_t today = LocalDayIndex(nowUtc);
    if (r.lastEditTs > 0 && LocalDayIndex(r.lastEditTs) == today) out.push_back("edited today");
    else if (r.edits7d >= 0.5) out.push_back("edited in the last 7 days");
    if (r.activeSecs7d >= 60.0)
        out.push_back(std::to_string((int)(r.activeSecs7d / 60.0)) + " min active in the last 7 days");
    if (r.activeDays7 >= 2)
        out.push_back("used on " + std::to_string(r.activeDays7) + " of the last 7 days");
    else if (r.openCount7d >= 3.0)
        out.push_back("opened " + std::to_string((long long)std::llround(r.openCount7d)) + " times in the last 7 days");
    if (r.distinctDays >= 10)
        out.push_back("recurring: used on " + std::to_string(r.distinctDays) + " different days");
    int64_t last = (std::max)(r.lastEventTs, r.lastActiveTs);
    if (last > 0) out.push_back("last used " + RelAge((std::max<int64_t>)(0, nowUtc - last)));
}

void EntityStore::Finish(EntityScore& s, const std::string& horizon, int64_t nowUtc, bool withReasons)
{
    s.shortTerm = DecayAt(s.rec.engS, s.rec.engSTs, nowUtc, kTauShortSec);
    s.longTerm  = DecayAt(s.rec.engL, s.rec.engLTs, nowUtc, kTauLongSec);
    s.prior = PriorFor(horizon, s.shortTerm, s.longTerm);
    s.score = Percentile(s.rec.entityType, horizon, s.prior);
    if (withReasons) FillReasons(s, nowUtc);
}

std::vector<EntityRecord> EntityStore::Lookup(const std::vector<std::string>& keys, int64_t nowUtc, bool legacyIdentity)
{
    if (nowUtc == 0) nowUtc = NowUtc();
    std::vector<EntityRecord> out(keys.size());
    if (keys.empty()) return out;
    ActivityStore::ReadLease rl = m_store->AcquireReader();
    if (!rl) return out;
    std::string sql = std::string("SELECT ") + kCols + " FROM inference WHERE entity_key = " +
        (legacyIdentity ? "CASE WHEN EXISTS(SELECT 1 FROM entity_alias WHERE alias_key=?1) "
                          "THEN (SELECT entity_key FROM entity_alias WHERE alias_key=?1) "
                          "WHEN (SELECT entity_type FROM inference WHERE entity_key=?1)='url' THEN NULL ELSE ?1 END" : "?1") + " AND deleted = 0;";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(rl.get(), sql.c_str(), -1, &st, nullptr) != SQLITE_OK) return out;
    DerivedReader dr(rl.get(), nowUtc);
    for (size_t i = 0; i < keys.size(); ++i)
    {
        if (keys[i].empty()) continue;
        sqlite3_reset(st);
        BindU(st, 1, keys[i]);
        if (sqlite3_step(st) == SQLITE_ROW)
        {
            ReadRecord(st, out[i]);
            dr.Fill(out[i]);
        }
    }
    sqlite3_finalize(st);
    return out;
}

std::vector<EntityScore> EntityStore::Score(const std::vector<std::string>& keys, const std::string& horizon,
                                            int64_t nowUtc, bool withReasons, bool legacyIdentity)
{
    if (nowUtc == 0) nowUtc = NowUtc();
    const std::string h = HorizonOf(horizon);
    EnsureDistribution(nowUtc);
    std::vector<EntityRecord> recs = Lookup(keys, nowUtc, legacyIdentity);
    std::vector<EntityScore> out;
    out.reserve(recs.size());
    for (size_t i = 0; i < recs.size(); ++i)
    {
        EntityScore s;
        s.found = !recs[i].entityKey.empty();
        s.rec = std::move(recs[i]);
        if (s.found) Finish(s, h, nowUtc, withReasons);
        else s.rec.entityKey = keys[i];
        out.push_back(std::move(s));
    }
    return out;
}

std::vector<EntityScore> EntityStore::Top(const std::string& entityType, int limit, const std::string& horizon,
                                          int64_t nowUtc, bool withReasons)
{
    if (nowUtc == 0) nowUtc = NowUtc();
    limit = (std::max)(1, (std::min)(limit, 1000));
    const std::string h = HorizonOf(horizon);
    EnsureDistribution(nowUtc);

    std::vector<std::string> types;
    if (entityType.empty()) types = { "file", "app", "url" };
    else types.push_back(entityType);
    std::vector<const char*> orderCols;
    if (h != "long") orderCols.push_back("eng_s_key");
    if (h != "short") orderCols.push_back("eng_l_key");

    std::vector<EntityScore> cands;
    {
        ActivityStore::ReadLease rl = m_store->AcquireReader();
        if (!rl) return cands;
        std::set<std::string> seen;
        for (const char* col : orderCols)
        {
            // eng_*_key is a time-independent ordering of the decayed value, so this walks an index.
            std::string sql = std::string("SELECT ") + kCols + " FROM inference WHERE entity_type = ?1 AND deleted = 0 ORDER BY " +
                              col + " DESC LIMIT ?2;";
            sqlite3_stmt* st = nullptr;
            if (sqlite3_prepare_v2(rl.get(), sql.c_str(), -1, &st, nullptr) != SQLITE_OK) continue;
            for (const std::string& t : types)
            {
                sqlite3_reset(st);
                BindU(st, 1, t);
                sqlite3_bind_int(st, 2, limit * 3);
                while (sqlite3_step(st) == SQLITE_ROW)
                {
                    EntityScore s;
                    ReadRecord(st, s.rec);
                    if (!seen.insert(s.rec.entityKey).second) continue;
                    s.found = true;
                    cands.push_back(std::move(s));
                }
            }
            sqlite3_finalize(st);
        }
        DerivedReader dr(rl.get(), nowUtc);
        for (EntityScore& s : cands) dr.Fill(s.rec);
    }
    for (EntityScore& s : cands) Finish(s, h, nowUtc, false);
    const bool mixed = types.size() > 1;
    std::sort(cands.begin(), cands.end(), [mixed](const EntityScore& a, const EntityScore& b)
    {
        if (mixed && a.score != b.score) return a.score > b.score;       // percentiles are comparable across types
        if (a.prior != b.prior) return a.prior > b.prior;
        return a.rec.lastEventTs > b.rec.lastEventTs;
    });
    if ((int)cands.size() > limit) cands.resize((size_t)limit);
    if (withReasons)
        for (EntityScore& s : cands) FillReasons(s, nowUtc);
    return cands;
}

std::vector<EntityScore> EntityStore::Search(const std::wstring& query, const std::string& entityType, int limit, int64_t nowUtc)
{
    std::vector<EntityScore> out;
    if (nowUtc == 0) nowUtc = NowUtc();
    limit = (std::max)(1, (std::min)(limit, 200));
    std::vector<std::string> toks = Tokenize(query, 8);
    if (toks.empty()) return out;
    EnsureDistribution(nowUtc);
    const bool fts = FtsAvailable();      // before taking a lease: GetMeta needs its own reader

    {
        ActivityStore::ReadLease rl = m_store->AcquireReader();
        if (!rl) return out;
        sqlite3_stmt* st = nullptr;
        bool viaFts = false;
        if (fts)
        {
            // Every token is quoted (no FTS syntax injection); the last one is a prefix match.
            std::string match;
            for (size_t i = 0; i < toks.size(); ++i)
            {
                std::string t;
                for (char c : toks[i]) { if (c == '"') t += "\"\""; else t += c; }
                if (!match.empty()) match += ' ';
                match += '"' + t + '"';
                if (i + 1 == toks.size()) match += '*';
            }
            std::string sql = std::string("SELECT ") + kCols +
                " FROM activity_fts JOIN inference ON inference.rowid = activity_fts.rowid "
                "WHERE activity_fts MATCH ?1 AND inference.deleted = 0 AND (?2 = '' OR inference.entity_type = ?2) "
                "ORDER BY bm25(activity_fts) LIMIT ?3;";
            if (sqlite3_prepare_v2(rl.get(), sql.c_str(), -1, &st, nullptr) == SQLITE_OK)
            {
                sqlite3_bind_text(st, 1, match.c_str(), (int)match.size(), SQLITE_TRANSIENT);
                sqlite3_bind_text(st, 2, entityType.c_str(), (int)entityType.size(), SQLITE_TRANSIENT);
                sqlite3_bind_int(st, 3, limit * 4);
                viaFts = true;
            }
            else
            {
                WARP_LOG_WARN("entity", "fts search prepare failed: %s", sqlite3_errmsg(rl.get()));
                st = nullptr;
            }
        }
        if (!st)
        {
            std::string sql = std::string("SELECT ") + kCols +
                " FROM inference WHERE deleted = 0 AND (?2 = '' OR entity_type = ?2) "
                "AND (display_name LIKE ?1 ESCAPE '^' OR title LIKE ?1 ESCAPE '^') ORDER BY eng_l_key DESC LIMIT ?3;";
            if (sqlite3_prepare_v2(rl.get(), sql.c_str(), -1, &st, nullptr) != SQLITE_OK) return out;
            std::string pat = "%" + EscapeLike(toks[0]) + "%";
            sqlite3_bind_text(st, 1, pat.c_str(), (int)pat.size(), SQLITE_TRANSIENT);
            sqlite3_bind_text(st, 2, entityType.c_str(), (int)entityType.size(), SQLITE_TRANSIENT);
            sqlite3_bind_int(st, 3, limit * 8);
        }
        while (sqlite3_step(st) == SQLITE_ROW)
        {
            EntityScore s;
            ReadRecord(st, s.rec);
            s.found = true;
            // FTS already requires every token (diacritic-insensitively); the LIKE fallback matched
            // only the first one.
            if (!viaFts && toks.size() > 1)
            {
                std::string hay = EntityKey::Key(s.rec.displayName + L" " + s.rec.title);
                bool all = true;
                for (const std::string& t : toks) if (hay.find(t) == std::string::npos) { all = false; break; }
                if (!all) continue;
            }
            out.push_back(std::move(s));
        }
        sqlite3_finalize(st);
        DerivedReader dr(rl.get(), nowUtc);
        for (EntityScore& s : out) dr.Fill(s.rec);
    }

    const size_t n = out.size();
    for (size_t i = 0; i < n; ++i)
    {
        out[i].relevance = n > 1 ? 1.0 - (double)i / (double)n : 1.0;
        Finish(out[i], "blend", nowUtc, false);
    }
    std::stable_sort(out.begin(), out.end(), [](const EntityScore& a, const EntityScore& b)
    {
        return 0.6 * a.relevance + 0.4 * a.score > 0.6 * b.relevance + 0.4 * b.score;
    });
    if ((int)out.size() > limit) out.resize((size_t)limit);
    for (EntityScore& s : out) FillReasons(s, nowUtc);
    return out;
}

EntityChanges EntityStore::GetChanges(int64_t sinceSeq, int limit, bool includeDeletes)
{
    EntityChanges out;
    limit = (std::max)(1, (std::min)(limit, 5000));
    if (sinceSeq < 0) sinceSeq = 0;
    int64_t nowUtc = NowUtc();
    ActivityStore::ReadLease rl = m_store->AcquireReader();
    if (!rl) { out.nextSeq = sinceSeq; return out; }
    sqlite3* db = rl.get();

    auto scalar = [db](const char* sql) -> int64_t
    {
        int64_t v = 0;
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) == SQLITE_OK)
        {
            if (sqlite3_step(st) == SQLITE_ROW) v = sqlite3_column_int64(st, 0);
            sqlite3_finalize(st);
        }
        return v;
    };

    Exec(db, "BEGIN;");   // one snapshot for cursor checks, upserts and deletes
    int64_t resetSeq = scalar("SELECT CAST(value AS INTEGER) FROM meta WHERE key = 'reset_seq';");
    int64_t floorSeq = scalar("SELECT CAST(value AS INTEGER) FROM meta WHERE key = 'tombstone_floor_seq';");
    // Sequence numbers consumed by rolled-back jobs never appear in a table; the high-water mark
    // committed with each batch lets the cursor move past them.
    int64_t highWater = scalar("SELECT CAST(value AS INTEGER) FROM meta WHERE key = 'change_seq_hw';");
    int64_t maxSeq = scalar("SELECT MAX(s) FROM (SELECT MAX(change_seq) AS s FROM inference "
                            "UNION ALL SELECT MAX(change_seq) FROM tombstone UNION ALL SELECT MAX(change_seq) FROM work_thread);");
    out.currentSeq = (std::max)((std::max)(maxSeq, resetSeq), highWater);
    if (sinceSeq > 0 && (sinceSeq < resetSeq || (includeDeletes && sinceSeq < floorSeq)))
    {
        Exec(db, "COMMIT;");
        out.reset = true;
        out.nextSeq = 0;
        return out;
    }

    std::vector<EntityRecord> ups;
    {
        std::string sql = std::string("SELECT ") + kCols + " FROM inference WHERE change_seq > ?1 AND deleted = 0 ORDER BY change_seq LIMIT ?2;";
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) == SQLITE_OK)
        {
            sqlite3_bind_int64(st, 1, sinceSeq);
            sqlite3_bind_int(st, 2, limit + 1);
            while (sqlite3_step(st) == SQLITE_ROW)
            {
                EntityRecord r;
                ReadRecord(st, r);
                ups.push_back(std::move(r));
            }
            sqlite3_finalize(st);
        }
    }
    std::vector<EntityTombstone> dels;
    if (includeDeletes)
    {
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db, "SELECT change_seq, entity_key, entity_type, reason, ts FROM tombstone WHERE change_seq > ?1 ORDER BY change_seq LIMIT ?2;",
                               -1, &st, nullptr) == SQLITE_OK)
        {
            sqlite3_bind_int64(st, 1, sinceSeq);
            sqlite3_bind_int(st, 2, limit + 1);
            while (sqlite3_step(st) == SQLITE_ROW)
            {
                EntityTombstone t;
                t.changeSeq = sqlite3_column_int64(st, 0);
                t.entityKey = ColU(st, 1);
                t.entityType = ColU(st, 2);
                t.reason = ColU(st, 3);
                t.ts = sqlite3_column_int64(st, 4);
                dels.push_back(std::move(t));
            }
            sqlite3_finalize(st);
        }
    }
    {
        DerivedReader dr(db, nowUtc);
        size_t i = 0, j = 0;
        int64_t last = sinceSeq;
        while ((int)(out.upserts.size() + out.deletes.size()) < limit && (i < ups.size() || j < dels.size()))
        {
            bool takeUp = j >= dels.size() || (i < ups.size() && ups[i].changeSeq < dels[j].changeSeq);
            if (takeUp)
            {
                last = ups[i].changeSeq;
                dr.Fill(ups[i]);
                out.upserts.push_back(std::move(ups[i++]));
            }
            else
            {
                last = dels[j].changeSeq;
                out.deletes.push_back(std::move(dels[j++]));
            }
        }
        out.more = i < ups.size() || j < dels.size();
        out.nextSeq = out.more ? last : (std::max)(last, out.currentSeq);
    }
    Exec(db, "COMMIT;");
    return out;
}

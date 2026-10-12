#include "framework.h"
#include "ActivityStore.h"
#include "sqlite3.h"
#include "EntityKey.h"
#include "EntityStore.h"
#include "UrlCanonicalizer.h"
#include "Logger.h"

#include <ctime>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <chrono>
#include <map>
#include <unordered_set>
#include <utility>

// =====================================================================
// StoreUtil
// =====================================================================
namespace StoreUtil
{
    std::string W2U(const std::wstring& w)
    {
        if (w.empty()) return std::string();
        int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
        if (n <= 0) return std::string();
        std::string s((size_t)n, '\0');
        WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
        return s;
    }

    std::wstring U2W(const char* s)
    {
        if (!s || !*s) return std::wstring();
        int len = (int)strlen(s);
        int n = MultiByteToWideChar(CP_UTF8, 0, s, len, nullptr, 0);
        if (n <= 0) return std::wstring();
        std::wstring w((size_t)n, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s, len, &w[0], n);
        return w;
    }

    int64_t NowMs()
    {
        FILETIME ft;
        GetSystemTimeAsFileTime(&ft);
        ULARGE_INTEGER u;
        u.LowPart = ft.dwLowDateTime;
        u.HighPart = ft.dwHighDateTime;
        // 100ns ticks since 1601 -> ms since 1970
        return (int64_t)((u.QuadPart - 116444736000000000ULL) / 10000ULL);
    }

    int64_t NowUtc() { return NowMs() / 1000; }

    // Howard Hinnant's days_from_civil.
    static int64_t DaysFromCivil(int64_t y, unsigned m, unsigned d)
    {
        y -= m <= 2;
        const int64_t era = (y >= 0 ? y : y - 399) / 400;
        const unsigned yoe = (unsigned)(y - era * 400);
        const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
        const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
        return era * 146097 + (int64_t)doe - 719468;
    }

    int64_t LocalDayIndex(int64_t utcSeconds)
    {
        time_t t = (time_t)utcSeconds;
        struct tm lt = {};
        if (localtime_s(&lt, &t) != 0)
            return utcSeconds / 86400;
        return DaysFromCivil((int64_t)lt.tm_year + 1900, (unsigned)lt.tm_mon + 1, (unsigned)lt.tm_mday);
    }

    double DecayKey(double s, int64_t t0, double tauSec)
    {
        if (s <= 0.0) return -1.0e300;
        return std::log(s) + (double)t0 / tauSec;
    }
}

using StoreUtil::W2U;
using StoreUtil::U2W;

// =====================================================================
// SQL helpers
// =====================================================================
namespace
{
    bool Exec(sqlite3* db, const char* sql, std::string* errOut = nullptr)
    {
        char* err = nullptr;
        int rc = sqlite3_exec(db, sql, nullptr, nullptr, &err);
        if (rc != SQLITE_OK)
        {
            if (errOut) *errOut = err ? err : sqlite3_errstr(rc);
            if (err) sqlite3_free(err);
            return false;
        }
        if (err) sqlite3_free(err);
        return true;
    }

    std::string QueryText(sqlite3* db, const char* sql)
    {
        std::string out;
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK)
            return std::string("prepare failed: ") + sqlite3_errmsg(db);
        if (sqlite3_step(st) == SQLITE_ROW)
        {
            const unsigned char* t = sqlite3_column_text(st, 0);
            out = t ? reinterpret_cast<const char*>(t) : "";
        }
        sqlite3_finalize(st);
        return out;
    }

    int64_t QueryInt(sqlite3* db, const char* sql, int64_t def = 0)
    {
        int64_t v = def;
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK) return def;
        if (sqlite3_step(st) == SQLITE_ROW) v = sqlite3_column_int64(st, 0);
        sqlite3_finalize(st);
        return v;
    }

    void BindText(sqlite3_stmt* st, int idx, const std::wstring& w)
    {
        if (w.empty()) { sqlite3_bind_null(st, idx); return; }
        std::string u = W2U(w);
        sqlite3_bind_text(st, idx, u.c_str(), (int)u.size(), SQLITE_TRANSIENT);
    }

    void BindText(sqlite3_stmt* st, int idx, const std::string& u)
    {
        if (u.empty()) { sqlite3_bind_null(st, idx); return; }
        sqlite3_bind_text(st, idx, u.c_str(), (int)u.size(), SQLITE_TRANSIENT);
    }

    std::wstring ColW(sqlite3_stmt* st, int i)
    {
        return U2W(reinterpret_cast<const char*>(sqlite3_column_text(st, i)));
    }

    std::string ColU(sqlite3_stmt* st, int i)
    {
        const unsigned char* t = sqlite3_column_text(st, i);
        return t ? std::string(reinterpret_cast<const char*>(t)) : std::string();
    }

    // Bind the 9 EventContext columns starting at parameter `startIdx`.
    void BindEventContext(sqlite3_stmt* st, int startIdx, const EventContext& ctx)
    {
        if (ctx.sourcePid) sqlite3_bind_int64(st, startIdx + 0, ctx.sourcePid); else sqlite3_bind_null(st, startIdx + 0);
        BindText(st, startIdx + 1, ctx.sourceExe);
        if (ctx.foregroundPid) sqlite3_bind_int64(st, startIdx + 2, ctx.foregroundPid); else sqlite3_bind_null(st, startIdx + 2);
        BindText(st, startIdx + 3, ctx.foregroundExe);
        if (ctx.msSinceInput != 0xFFFFFFFFu) sqlite3_bind_int64(st, startIdx + 4, ctx.msSinceInput); else sqlite3_bind_null(st, startIdx + 4);
        if (ctx.parentPid) sqlite3_bind_int64(st, startIdx + 5, ctx.parentPid); else sqlite3_bind_null(st, startIdx + 5);
        BindText(st, startIdx + 6, ctx.parentExe);
        if (ctx.createdWindowMs) sqlite3_bind_int64(st, startIdx + 7, ctx.createdWindowMs); else sqlite3_bind_null(st, startIdx + 7);
        sqlite3_bind_double(st, startIdx + 8, ctx.confidence);
    }

    // ---- SQL functions (registered on every connection) ----
    void SqlCanonUrl(sqlite3_context* c, int argc, sqlite3_value** argv)
    {
        if (argc < 1 || sqlite3_value_type(argv[0]) == SQLITE_NULL) { sqlite3_result_null(c); return; }
        std::wstring w = U2W(reinterpret_cast<const char*>(sqlite3_value_text(argv[0])));
        UrlCanon::Result r = UrlCanon::Canonicalize(w);
        if (r.excluded || r.canonical.empty()) { sqlite3_result_null(c); return; }
        std::string u = W2U(r.canonical);
        sqlite3_result_text(c, u.c_str(), (int)u.size(), SQLITE_TRANSIENT);
    }

    void SqlUrlExcluded(sqlite3_context* c, int argc, sqlite3_value** argv)
    {
        if (argc < 1 || sqlite3_value_type(argv[0]) == SQLITE_NULL) { sqlite3_result_int(c, 0); return; }
        std::wstring w = U2W(reinterpret_cast<const char*>(sqlite3_value_text(argv[0])));
        UrlCanon::Result r = UrlCanon::Canonicalize(w);
        sqlite3_result_int(c, r.excluded ? 1 : 0);
    }

    void RegisterFunctions(sqlite3* db)
    {
        EntityKey::RegisterSqliteFunctions(db);
        sqlite3_create_function_v2(db, "warp_canon_url", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, nullptr, SqlCanonUrl, nullptr, nullptr, nullptr);
        sqlite3_create_function_v2(db, "warp_url_excluded", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, nullptr, SqlUrlExcluded, nullptr, nullptr, nullptr);
    }

    int64_t FileSize(const std::wstring& p)
    {
        WIN32_FILE_ATTRIBUTE_DATA d = {};
        if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &d)) return 0;
        return ((int64_t)d.nFileSizeHigh << 32) | d.nFileSizeLow;
    }
}

// =====================================================================
// WriterContext
// =====================================================================
sqlite3_stmt* WriterContext::Stmt(const char* sql)
{
    auto it = m_cache.find(sql);
    if (it != m_cache.end())
    {
        sqlite3_reset(it->second);
        sqlite3_clear_bindings(it->second);
        return it->second;
    }
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v3(db, sql, -1, SQLITE_PREPARE_PERSISTENT, &st, nullptr) != SQLITE_OK)
    {
        WARP_LOG_ERROR("store", "prepare failed: %s | %.120s", sqlite3_errmsg(db), sql);
        return nullptr;
    }
    m_cache[sql] = st;
    return st;
}

int64_t WriterContext::NextChangeSeq() { return m_seq ? ++(*m_seq) : 0; }
int64_t WriterContext::CurrentChangeSeq() const { return m_seq ? m_seq->load() : 0; }

// =====================================================================
// ActivityStore — open / close
// =====================================================================
ActivityStore::ActivityStore() {}
ActivityStore::~ActivityStore() { Close(); }

void ActivityStore::AddSink(IWriterSink* sink)
{
    if (sink) m_sinks.push_back(sink);
}

void ActivityStore::SetRetention(int rawDays, int entityDays)
{
    m_rawRetentionDays = (std::max)(1, (std::min)(rawDays, 3650));
    m_entityRetentionDays = (std::max)(7, (std::min)(entityDays, 3650));
}

bool ActivityStore::OpenConnection(sqlite3** db, bool writer)
{
    *db = nullptr;
    std::string u = W2U(m_path);
    int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX;
    int rc = sqlite3_open_v2(u.c_str(), db, flags, nullptr);
    if (rc != SQLITE_OK)
    {
        WARP_LOG_ERROR("store", "sqlite3_open_v2 failed (%d): %s", rc, *db ? sqlite3_errmsg(*db) : "?");
        if (*db) sqlite3_close(*db);
        *db = nullptr;
        return false;
    }
    sqlite3_busy_timeout(*db, 5000);
    if (writer)
    {
        Exec(*db, "PRAGMA journal_mode=WAL;");
        Exec(*db, "PRAGMA secure_delete=ON;");
        Exec(*db, "PRAGMA journal_size_limit=67108864;");
    }
    Exec(*db, "PRAGMA synchronous=NORMAL;");
    Exec(*db, "PRAGMA temp_store=MEMORY;");
    if (!writer)
        Exec(*db, "PRAGMA query_only=1;");
    RegisterFunctions(*db);
    return true;
}

bool ActivityStore::CheckIntegrityOrQuarantine()
{
    std::string res = QueryText(m_wdb, "PRAGMA quick_check(1);");
    if (res == "ok")
    {
        std::lock_guard<std::mutex> lk(m_statsMtx);
        m_stats.integrity = "ok";
        return true;
    }

    WARP_LOG_ERROR("store", "integrity check failed (%s) - quarantining database", res.c_str());
    sqlite3_close(m_wdb);
    m_wdb = nullptr;

    wchar_t stamp[32] = {};
    swprintf_s(stamp, L".corrupt-%lld", (long long)StoreUtil::NowUtc());
    const wchar_t* suffixes[] = { L"", L"-wal", L"-shm" };
    for (const wchar_t* sfx : suffixes)
    {
        std::wstring from = m_path + sfx;
        std::wstring to = m_path + stamp + sfx;
        MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING);
    }
    if (!OpenConnection(&m_wdb, true))
        return false;
    std::lock_guard<std::mutex> lk(m_statsMtx);
    m_stats.integrity = "recovered (" + res + ")";
    return true;
}

bool ActivityStore::Open(const std::wstring& dbPath)
{
    if (m_open.load()) return true;
    m_path = dbPath;
    m_stopping = false;

    if (!OpenConnection(&m_wdb, true))
    {
        // Unreadable file (e.g. "file is not a database"): quarantine and retry once.
        m_wdb = nullptr;
        std::wstring to = m_path + L".corrupt-" + std::to_wstring(StoreUtil::NowUtc());
        MoveFileExW(m_path.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING);
        if (!OpenConnection(&m_wdb, true))
            return false;
    }
    if (!CheckIntegrityOrQuarantine())
        return false;
    if (!Migrate())
    {
        // Keep the old file for diagnosis but never leave the always-on agent without a store.
        WARP_LOG_ERROR("store", "schema migration failed; quarantining database and starting fresh");
        sqlite3_close(m_wdb);
        m_wdb = nullptr;
        std::wstring stamp = L".migrate-failed-" + std::to_wstring(StoreUtil::NowUtc());
        const wchar_t* suffixes[] = { L"", L"-wal", L"-shm" };
        for (const wchar_t* sfx : suffixes)
        {
            std::wstring from = m_path + sfx;
            std::wstring to = m_path + stamp + sfx;
            MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING);
        }
        if (!OpenConnection(&m_wdb, true) || !Migrate())
        {
            WARP_LOG_ERROR("store", "fresh database could not be initialized; store not opened");
            if (m_wdb) { sqlite3_close(m_wdb); m_wdb = nullptr; }
            return false;
        }
        std::lock_guard<std::mutex> lk(m_statsMtx);
        m_stats.integrity = "recovered (migration failed; previous file quarantined)";
    }

    m_resetSeq = QueryInt(m_wdb, "SELECT CAST(value AS INTEGER) FROM meta WHERE key = 'reset_seq';", 0);
    int64_t maxSeq = QueryInt(m_wdb,
        "SELECT MAX(s) FROM (SELECT MAX(change_seq) AS s FROM inference "
        "UNION ALL SELECT MAX(change_seq) FROM tombstone "
        "UNION ALL SELECT MAX(change_seq) FROM work_thread);", 0);
    const int64_t highWater = QueryInt(m_wdb, "SELECT CAST(value AS INTEGER) FROM meta WHERE key = 'change_seq_hw';", 0);
    // The sequence must never move backwards: not across a clear-all (consumers compare against
    // reset_seq) and not below numbers already handed out as cursors (change_seq_hw).
    m_seq = (std::max)((std::max)(maxSeq, highWater), m_resetSeq.load());
    m_committedSeq = m_seq.load();
    m_ctx.ftsAvailable = QueryText(m_wdb, "SELECT value FROM meta WHERE key = 'fts_available';") == "1";
    LoadFloors();                                       // purged time ranges stay purged across restarts
    {
        std::lock_guard<std::mutex> lk(m_statsMtx);
        m_stats.schemaVersion = (int)QueryInt(m_wdb, "PRAGMA user_version;", 0);
    }

    for (int i = 0; i < kReaderCount; ++i)
    {
        sqlite3* r = nullptr;
        if (OpenConnection(&r, false))
        {
            m_readers.push_back(r);
            m_freeReaders.push_back(r);
        }
    }

    m_ctx.db = m_wdb;
    m_ctx.m_seq = &m_seq;
    m_open = true;
    m_writer = std::thread(&ActivityStore::WriterLoop, this);
    WARP_LOG_INFO("store", "opened %s (schema v%d, change_seq=%lld, readers=%d)",
                  W2U(m_path).c_str(), (int)QueryInt(m_wdb, "PRAGMA user_version;", 0),
                  (long long)m_seq.load(), (int)m_readers.size());
    return true;
}

void ActivityStore::Close()
{
    if (!m_open.load() && !m_writer.joinable())
    {
        if (m_wdb) { sqlite3_close(m_wdb); m_wdb = nullptr; }
        return;
    }
    m_stopDeadlineMs = StoreUtil::NowMs() + 5000;
    m_stopping = true;
    m_qCv.notify_all();
    if (m_writer.joinable()) m_writer.join();
    {
        std::lock_guard<std::mutex> lk(m_seqMtx);
        m_open = false;
    }
    {
        std::lock_guard<std::mutex> lk(m_readMtx);   // waiting AcquireReader() calls give up now
    }
    m_readCv.notify_all();

    FinalizeWriterStatements();
    {
        std::unique_lock<std::mutex> lk(m_readMtx);
        // Wait (bounded) for leased readers to come back.
        m_readCv.wait_for(lk, std::chrono::seconds(5), [this] { return m_freeReaders.size() == m_readers.size(); });
        for (sqlite3* r : m_readers) sqlite3_close_v2(r);
        m_readers.clear();
        m_freeReaders.clear();
    }
    if (m_wdb)
    {
        Exec(m_wdb, "PRAGMA wal_checkpoint(PASSIVE);");
        sqlite3_close_v2(m_wdb);
        m_wdb = nullptr;
    }
    m_seqCv.notify_all();
}

void ActivityStore::FinalizeWriterStatements()
{
    for (auto& kv : m_ctx.m_cache)
        if (kv.second) sqlite3_finalize(kv.second);
    m_ctx.m_cache.clear();
}

// =====================================================================
// Schema migrations (PRAGMA user_version)
//   v0/v1: legacy layout (four activity tables + inference, EventContext columns)
//   v2   : entity keys, event-time ms, provenance, focus intervals, global change_seq,
//          engagement accumulators, tombstones, daily rollups, presence, snapshots,
//          work sessions/threads, edges, clipboard/print, FTS5 search index.
// =====================================================================
namespace
{
    const char* const kLegacyDdl[] = {
        "CREATE TABLE IF NOT EXISTS file_activity (id INTEGER PRIMARY KEY AUTOINCREMENT, timestamp INTEGER NOT NULL, action TEXT NOT NULL, path TEXT NOT NULL, old_path TEXT);",
        "CREATE TABLE IF NOT EXISTS app_launch_activity (id INTEGER PRIMARY KEY AUTOINCREMENT, timestamp INTEGER NOT NULL, exe_name TEXT NOT NULL, exe_path TEXT NOT NULL, pid INTEGER NOT NULL);",
        "CREATE TABLE IF NOT EXISTS browsing_activity (id INTEGER PRIMARY KEY AUTOINCREMENT, timestamp INTEGER NOT NULL, browser TEXT NOT NULL, title TEXT NOT NULL, url TEXT);",
        "CREATE TABLE IF NOT EXISTS app_focus_activity (id INTEGER PRIMARY KEY AUTOINCREMENT, timestamp INTEGER NOT NULL, exe_name TEXT NOT NULL, exe_path TEXT NOT NULL, window_title TEXT NOT NULL, duration_secs INTEGER NOT NULL);",
        "CREATE TABLE IF NOT EXISTS inference (entity_key TEXT PRIMARY KEY, entity_type TEXT, last_event_ts INTEGER, last_open_ts INTEGER, last_edit_ts INTEGER, open_count_7d INTEGER, open_count_30d INTEGER, open_count_total INTEGER, recency_score REAL, version INTEGER, updated_at INTEGER);",
        "CREATE INDEX IF NOT EXISTS idx_activity_ts ON file_activity(timestamp);",
        "CREATE INDEX IF NOT EXISTS idx_activity_action ON file_activity(action, timestamp);",
        "CREATE INDEX IF NOT EXISTS idx_app_ts ON app_launch_activity(timestamp);",
        "CREATE INDEX IF NOT EXISTS idx_browse_ts ON browsing_activity(timestamp);",
        "CREATE INDEX IF NOT EXISTS idx_focus_ts ON app_focus_activity(timestamp);",
        "CREATE INDEX IF NOT EXISTS idx_inference_updated_at ON inference(updated_at);",
    };

    const char* const kActivityTables[] = { "file_activity", "app_launch_activity", "browsing_activity", "app_focus_activity" };
    const char* const kContextCols[] = {
        "source_pid INTEGER", "source_exe TEXT", "foreground_pid INTEGER", "foreground_exe TEXT",
        "ms_since_input INTEGER", "parent_pid INTEGER", "parent_exe TEXT", "created_window_ms INTEGER",
        "confidence REAL DEFAULT 1.0",
    };

    struct AddCol { const char* table; const char* def; };
    const AddCol kV2Cols[] = {
        { "file_activity", "entity_key TEXT" }, { "file_activity", "event_ms INTEGER" }, { "file_activity", "source TEXT" },
        { "file_activity", "presence TEXT" }, { "file_activity", "is_dir INTEGER DEFAULT 0" }, { "file_activity", "origin_url TEXT" },
        { "app_launch_activity", "entity_key TEXT" }, { "app_launch_activity", "event_ms INTEGER" }, { "app_launch_activity", "source TEXT" },
        { "app_launch_activity", "creator_pid INTEGER" }, { "app_launch_activity", "creator_exe TEXT" }, { "app_launch_activity", "aumid TEXT" },
        { "app_launch_activity", "session_id INTEGER" }, { "app_launch_activity", "exit_ts INTEGER" }, { "app_launch_activity", "lifetime_secs INTEGER" },
        { "browsing_activity", "entity_key TEXT" }, { "browsing_activity", "event_ms INTEGER" }, { "browsing_activity", "source TEXT" },
        { "browsing_activity", "host TEXT" }, { "browsing_activity", "doc_id TEXT" }, { "browsing_activity", "doc_kind TEXT" },
        { "browsing_activity", "dwell_secs INTEGER" }, { "browsing_activity", "presence TEXT" }, { "browsing_activity", "window_id INTEGER" },
        { "app_focus_activity", "entity_key TEXT" }, { "app_focus_activity", "event_ms INTEGER" }, { "app_focus_activity", "source TEXT" },
        { "app_focus_activity", "start_ts INTEGER" }, { "app_focus_activity", "end_ts INTEGER" }, { "app_focus_activity", "active_secs INTEGER" },
        { "app_focus_activity", "idle_secs INTEGER" }, { "app_focus_activity", "doc_key TEXT" }, { "app_focus_activity", "doc_display TEXT" },
        { "app_focus_activity", "doc_type TEXT" }, { "app_focus_activity", "window_class TEXT" }, { "app_focus_activity", "desktop_id TEXT" },
        { "app_focus_activity", "aumid TEXT" }, { "app_focus_activity", "presence TEXT" },
        { "inference", "display_name TEXT" }, { "inference", "title TEXT" }, { "inference", "doc_kind TEXT" }, { "inference", "file_id TEXT" },
        { "inference", "last_active_ts INTEGER DEFAULT 0" }, { "inference", "edit_count_total REAL DEFAULT 0" },
        { "inference", "active_secs_total REAL DEFAULT 0" }, { "inference", "distinct_days INTEGER DEFAULT 0" },
        { "inference", "last_day INTEGER DEFAULT 0" }, { "inference", "eng_s REAL DEFAULT 0" }, { "inference", "eng_s_ts INTEGER DEFAULT 0" },
        { "inference", "eng_s_key REAL DEFAULT -1e300" }, { "inference", "eng_l REAL DEFAULT 0" }, { "inference", "eng_l_ts INTEGER DEFAULT 0" },
        { "inference", "eng_l_key REAL DEFAULT -1e300" }, { "inference", "change_seq INTEGER DEFAULT 0" }, { "inference", "deleted INTEGER DEFAULT 0" },
    };

    const char* const kV2Ddl[] = {
        "CREATE TABLE IF NOT EXISTS meta (key TEXT PRIMARY KEY, value TEXT) WITHOUT ROWID;",
        "CREATE TABLE IF NOT EXISTS tombstone (change_seq INTEGER PRIMARY KEY, entity_key TEXT NOT NULL, entity_type TEXT, ts INTEGER NOT NULL, reason TEXT);",
        "CREATE TABLE IF NOT EXISTS entity_day (entity_key TEXT NOT NULL, day INTEGER NOT NULL, opens REAL NOT NULL DEFAULT 0, edits REAL NOT NULL DEFAULT 0, active_secs REAL NOT NULL DEFAULT 0, sessions INTEGER NOT NULL DEFAULT 0, PRIMARY KEY (entity_key, day)) WITHOUT ROWID;",
        "CREATE TABLE IF NOT EXISTS presence_interval (id INTEGER PRIMARY KEY AUTOINCREMENT, start_ts INTEGER NOT NULL, end_ts INTEGER NOT NULL, state TEXT NOT NULL);",
        "CREATE TABLE IF NOT EXISTS context_snapshot (id INTEGER PRIMARY KEY AUTOINCREMENT, ts INTEGER NOT NULL, window_secs INTEGER NOT NULL, json TEXT NOT NULL, thread_id INTEGER, embedding BLOB, model TEXT);",
        "CREATE TABLE IF NOT EXISTS edge (a TEXT NOT NULL, b TEXT NOT NULL, weight REAL NOT NULL, last_ts INTEGER NOT NULL, PRIMARY KEY (a, b)) WITHOUT ROWID;",
        "CREATE TABLE IF NOT EXISTS work_session (id INTEGER PRIMARY KEY AUTOINCREMENT, start_ts INTEGER NOT NULL, end_ts INTEGER NOT NULL, active_secs INTEGER NOT NULL DEFAULT 0, passive_secs INTEGER NOT NULL DEFAULT 0);",
        "CREATE TABLE IF NOT EXISTS work_thread (id INTEGER PRIMARY KEY AUTOINCREMENT, label TEXT, first_ts INTEGER NOT NULL, last_ts INTEGER NOT NULL, active_secs REAL NOT NULL DEFAULT 0, project_key TEXT, centroid BLOB, change_seq INTEGER NOT NULL DEFAULT 0);",
        "CREATE TABLE IF NOT EXISTS thread_member (thread_id INTEGER NOT NULL, entity_key TEXT NOT NULL, weight REAL NOT NULL, last_ts INTEGER NOT NULL, PRIMARY KEY (thread_id, entity_key)) WITHOUT ROWID;",
        "CREATE TABLE IF NOT EXISTS thread_span (thread_id INTEGER NOT NULL, start_ts INTEGER NOT NULL, end_ts INTEGER NOT NULL, active_secs REAL NOT NULL DEFAULT 0, PRIMARY KEY (thread_id, start_ts)) WITHOUT ROWID;",
        "CREATE TABLE IF NOT EXISTS clipboard_activity (id INTEGER PRIMARY KEY AUTOINCREMENT, timestamp INTEGER NOT NULL, event_ms INTEGER, owner_exe TEXT, formats TEXT);",
        "CREATE TABLE IF NOT EXISTS print_activity (id INTEGER PRIMARY KEY AUTOINCREMENT, timestamp INTEGER NOT NULL, event_ms INTEGER, document TEXT, printer TEXT, pages INTEGER);",
        "CREATE INDEX IF NOT EXISTS idx_file_entity ON file_activity(entity_key);",
        "CREATE INDEX IF NOT EXISTS idx_file_src_pid ON file_activity(source_pid);",
        "CREATE INDEX IF NOT EXISTS idx_app_entity ON app_launch_activity(entity_key);",
        "CREATE INDEX IF NOT EXISTS idx_app_pid ON app_launch_activity(pid);",
        "CREATE INDEX IF NOT EXISTS idx_browse_entity ON browsing_activity(entity_key);",
        "CREATE INDEX IF NOT EXISTS idx_browse_host ON browsing_activity(host);",
        "CREATE INDEX IF NOT EXISTS idx_focus_end ON app_focus_activity(end_ts);",
        "CREATE INDEX IF NOT EXISTS idx_focus_doc ON app_focus_activity(doc_key);",
        "CREATE INDEX IF NOT EXISTS idx_inference_seq ON inference(change_seq);",
        "CREATE INDEX IF NOT EXISTS idx_inference_type_s ON inference(entity_type, eng_s_key);",
        "CREATE INDEX IF NOT EXISTS idx_inference_type_l ON inference(entity_type, eng_l_key);",
        "CREATE INDEX IF NOT EXISTS idx_inference_last ON inference(last_event_ts);",
        "CREATE INDEX IF NOT EXISTS idx_entity_day_day ON entity_day(day);",
        "CREATE INDEX IF NOT EXISTS idx_tombstone_ts ON tombstone(ts);",
        "CREATE INDEX IF NOT EXISTS idx_presence_end ON presence_interval(end_ts);",
        "CREATE INDEX IF NOT EXISTS idx_snapshot_ts ON context_snapshot(ts);",
        "CREATE INDEX IF NOT EXISTS idx_edge_b ON edge(b);",
        "CREATE INDEX IF NOT EXISTS idx_work_session_end ON work_session(end_ts);",
        "CREATE INDEX IF NOT EXISTS idx_work_thread_last ON work_thread(last_ts);",
        "CREATE INDEX IF NOT EXISTS idx_thread_member_entity ON thread_member(entity_key);",
        "CREATE INDEX IF NOT EXISTS idx_thread_span_end ON thread_span(end_ts);",
        "CREATE INDEX IF NOT EXISTS idx_clip_ts ON clipboard_activity(timestamp);",
        "CREATE INDEX IF NOT EXISTS idx_print_ts ON print_activity(timestamp);",
    };

    // Backfill of legacy rows. Order matters: purge excluded URLs before canonicalizing.
    const char* const kV2Backfill[] = {
        "UPDATE file_activity SET entity_key = warp_file_key(path), event_ms = COALESCE(event_ms, timestamp * 1000), source = COALESCE(source, 'etw') WHERE entity_key IS NULL;",
        "UPDATE app_launch_activity SET entity_key = warp_file_key(exe_path), event_ms = COALESCE(event_ms, timestamp * 1000), source = COALESCE(source, 'etw') WHERE entity_key IS NULL;",
        "DELETE FROM browsing_activity WHERE url IS NOT NULL AND url <> '' AND warp_url_excluded(url) = 1;",
        "UPDATE browsing_activity SET url = warp_canon_url(url) WHERE url IS NOT NULL AND url <> '' AND entity_key IS NULL;",
        "UPDATE browsing_activity SET entity_key = warp_key(COALESCE(NULLIF(url, ''), title)), event_ms = COALESCE(event_ms, timestamp * 1000), source = COALESCE(source, 'uia') WHERE entity_key IS NULL;",
        "UPDATE app_focus_activity SET entity_key = warp_file_key(exe_path), event_ms = COALESCE(event_ms, timestamp * 1000), end_ts = COALESCE(end_ts, timestamp), "
        "start_ts = COALESCE(start_ts, timestamp - duration_secs), active_secs = COALESCE(active_secs, duration_secs), idle_secs = COALESCE(idle_secs, 0), source = COALESCE(source, 'focus') WHERE entity_key IS NULL;",
        // Re-key the per-entity store with the shared canonicalizer, merging collisions and
        // dropping sign-in / password-manager URLs that older builds stored verbatim.
        "DELETE FROM inference WHERE entity_type = 'url' AND warp_url_excluded(entity_key) = 1;",
        "CREATE TEMP TABLE inf_merge AS SELECT "
        "  CASE WHEN entity_type IN ('file','app') THEN warp_file_key(entity_key) "
        "       WHEN entity_type = 'url' AND warp_canon_url(entity_key) IS NOT NULL THEN warp_key(warp_canon_url(entity_key)) "
        "       ELSE warp_key(entity_key) END AS k, "
        "  MAX(entity_type) AS t, MAX(last_event_ts) AS le, MAX(last_open_ts) AS lo, MAX(last_edit_ts) AS led, "
        "  SUM(open_count_7d) AS c7, SUM(open_count_30d) AS c30, SUM(open_count_total) AS ct, MAX(recency_score) AS rs, "
        "  MAX(version) AS v, MAX(updated_at) AS ua FROM inference GROUP BY 1;",
        "DELETE FROM inference;",
        "INSERT INTO inference (entity_key, entity_type, last_event_ts, last_open_ts, last_edit_ts, open_count_7d, open_count_30d, open_count_total, recency_score, version, updated_at) "
        "SELECT k, t, le, lo, led, c7, c30, ct, rs, v, ua FROM inf_merge WHERE k IS NOT NULL;",
        "DROP TABLE inf_merge;",
        "UPDATE inference SET display_name = (SELECT f.path FROM file_activity f WHERE f.entity_key = inference.entity_key ORDER BY f.id DESC LIMIT 1) WHERE entity_type = 'file' AND display_name IS NULL;",
        "UPDATE inference SET display_name = (SELECT a.exe_path FROM app_launch_activity a WHERE a.entity_key = inference.entity_key ORDER BY a.id DESC LIMIT 1) WHERE entity_type = 'app' AND display_name IS NULL;",
        "UPDATE inference SET display_name = (SELECT f.exe_path FROM app_focus_activity f WHERE f.entity_key = inference.entity_key ORDER BY f.id DESC LIMIT 1) WHERE entity_type = 'app' AND display_name IS NULL;",
        "UPDATE inference SET display_name = (SELECT COALESCE(NULLIF(b.url, ''), b.title) FROM browsing_activity b WHERE b.entity_key = inference.entity_key ORDER BY b.id DESC LIMIT 1) WHERE entity_type = 'url' AND display_name IS NULL;",
        "UPDATE inference SET display_name = entity_key WHERE display_name IS NULL;",
        // Daily rollups from the raw history that is still retained.
        "INSERT INTO entity_day (entity_key, day, opens, edits, active_secs, sessions) "
        "SELECT entity_key, CAST(julianday(timestamp, 'unixepoch', 'localtime') - 2440587.5 AS INTEGER), "
        "  SUM(CASE WHEN action = 'OPEN' THEN COALESCE(confidence, 1.0) ELSE 0 END), "
        "  SUM(CASE WHEN action IN ('MODIFY','CREATE') THEN COALESCE(confidence, 1.0) ELSE 0 END), 0, 0 "
        "FROM file_activity WHERE entity_key IS NOT NULL GROUP BY 1, 2 "
        "ON CONFLICT (entity_key, day) DO UPDATE SET opens = opens + excluded.opens, edits = edits + excluded.edits;",
        "INSERT INTO entity_day (entity_key, day, opens, edits, active_secs, sessions) "
        "SELECT entity_key, CAST(julianday(timestamp, 'unixepoch', 'localtime') - 2440587.5 AS INTEGER), SUM(COALESCE(confidence, 1.0)), 0, 0, 0 "
        "FROM app_launch_activity WHERE entity_key IS NOT NULL GROUP BY 1, 2 "
        "ON CONFLICT (entity_key, day) DO UPDATE SET opens = opens + excluded.opens;",
        "INSERT INTO entity_day (entity_key, day, opens, edits, active_secs, sessions) "
        "SELECT entity_key, CAST(julianday(timestamp, 'unixepoch', 'localtime') - 2440587.5 AS INTEGER), 0, 0, SUM(COALESCE(active_secs, duration_secs)), COUNT(*) "
        "FROM app_focus_activity WHERE entity_key IS NOT NULL GROUP BY 1, 2 "
        "ON CONFLICT (entity_key, day) DO UPDATE SET active_secs = active_secs + excluded.active_secs, sessions = sessions + excluded.sessions;",
        "INSERT INTO entity_day (entity_key, day, opens, edits, active_secs, sessions) "
        "SELECT entity_key, CAST(julianday(timestamp, 'unixepoch', 'localtime') - 2440587.5 AS INTEGER), SUM(COALESCE(confidence, 1.0)), 0, 0, 0 "
        "FROM browsing_activity WHERE entity_key IS NOT NULL GROUP BY 1, 2 "
        "ON CONFLICT (entity_key, day) DO UPDATE SET opens = opens + excluded.opens;",
        "UPDATE inference SET "
        "  active_secs_total = COALESCE((SELECT SUM(d.active_secs) FROM entity_day d WHERE d.entity_key = inference.entity_key), 0), "
        "  edit_count_total  = COALESCE((SELECT SUM(d.edits) FROM entity_day d WHERE d.entity_key = inference.entity_key), 0), "
        "  distinct_days     = COALESCE((SELECT COUNT(*) FROM entity_day d WHERE d.entity_key = inference.entity_key), 0), "
        "  last_day          = COALESCE((SELECT MAX(d.day) FROM entity_day d WHERE d.entity_key = inference.entity_key), 0), "
        "  last_active_ts    = COALESCE(last_event_ts, 0), "
        "  change_seq        = rowid;",
    };

    // Seeds the decayed engagement accumulators from the daily rollups (C++ because it needs exp/log).
    bool SeedEngagement(sqlite3* db, int64_t nowUtc)
    {
        sqlite3_stmt* sel = nullptr;
        sqlite3_stmt* upd = nullptr;
        if (sqlite3_prepare_v2(db, "SELECT entity_key, day, opens, edits, active_secs FROM entity_day ORDER BY entity_key;", -1, &sel, nullptr) != SQLITE_OK)
            return false;
        if (sqlite3_prepare_v2(db, "UPDATE inference SET eng_s = ?1, eng_s_ts = ?2, eng_s_key = ?3, eng_l = ?4, eng_l_ts = ?2, eng_l_key = ?5 WHERE entity_key = ?6;", -1, &upd, nullptr) != SQLITE_OK)
        {
            sqlite3_finalize(sel);
            return false;
        }
        std::string cur;
        double s = 0.0, l = 0.0;
        auto flush = [&]()
        {
            if (cur.empty()) return;
            sqlite3_reset(upd);
            sqlite3_bind_double(upd, 1, s);
            sqlite3_bind_int64(upd, 2, nowUtc);
            sqlite3_bind_double(upd, 3, StoreUtil::DecayKey(s, nowUtc, StoreUtil::kTauShortSec));
            sqlite3_bind_double(upd, 4, l);
            sqlite3_bind_double(upd, 5, StoreUtil::DecayKey(l, nowUtc, StoreUtil::kTauLongSec));
            sqlite3_bind_text(upd, 6, cur.c_str(), (int)cur.size(), SQLITE_TRANSIENT);
            sqlite3_step(upd);
        };
        while (sqlite3_step(sel) == SQLITE_ROW)
        {
            std::string key = ColU(sel, 0);
            if (key != cur)
            {
                flush();
                cur = key;
                s = l = 0.0;
            }
            int64_t day = sqlite3_column_int64(sel, 1);
            double w = StoreUtil::kWOpenPassive * sqlite3_column_double(sel, 2)
                     + StoreUtil::kWEdit * sqlite3_column_double(sel, 3)
                     + StoreUtil::kWActivePerMin * (sqlite3_column_double(sel, 4) / 60.0)
                     + StoreUtil::kWNewDay;
            double t = (double)day * 86400.0 + 43200.0;      // local noon (approx. UTC) of that day
            double age = (std::max)(0.0, (double)nowUtc - t);
            s += w * std::exp(-age / StoreUtil::kTauShortSec);
            l += w * std::exp(-age / StoreUtil::kTauLongSec);
        }
        flush();
        sqlite3_finalize(sel);
        sqlite3_finalize(upd);
        return true;
    }
}

bool ActivityStore::Migrate()
{
    int64_t ver = QueryInt(m_wdb, "PRAGMA user_version;", 0);
    if (ver >= 2)
        return true;

    WARP_LOG_INFO("store", "migrating schema v%lld -> v2", (long long)ver);
    std::string err;
    if (!Exec(m_wdb, "BEGIN IMMEDIATE;", &err))
    {
        WARP_LOG_ERROR("store", "migration BEGIN failed: %s", err.c_str());
        return false;
    }
    bool ok = true;
    auto must = [&](const char* sql)
    {
        if (!ok) return;
        if (!Exec(m_wdb, sql, &err))
        {
            WARP_LOG_ERROR("store", "migration step failed: %s | %.160s", err.c_str(), sql);
            ok = false;
        }
    };

    for (const char* sql : kLegacyDdl) must(sql);
    if (ok)
    {
        // ALTERs: "duplicate column" errors are expected on partially-migrated databases.
        for (const char* tbl : kActivityTables)
            for (const char* col : kContextCols)
            {
                std::string sql = std::string("ALTER TABLE ") + tbl + " ADD COLUMN " + col + ";";
                Exec(m_wdb, sql.c_str());
            }
        for (const AddCol& c : kV2Cols)
        {
            std::string sql = std::string("ALTER TABLE ") + c.table + " ADD COLUMN " + c.def + ";";
            Exec(m_wdb, sql.c_str());
        }
    }
    for (const char* sql : kV2Ddl) must(sql);
    for (const char* sql : kV2Backfill) must(sql);
    if (ok && !SeedEngagement(m_wdb, StoreUtil::NowUtc()))
    {
        WARP_LOG_ERROR("store", "engagement seeding failed: %s", sqlite3_errmsg(m_wdb));
        ok = false;
    }

    // FTS5 is optional: if this SQLite build lacks it, search falls back to LIKE queries.
    // FTS rows share the rowid of their inference row so updates/deletes are O(log n).
    bool fts = ok && Exec(m_wdb,
        "CREATE VIRTUAL TABLE IF NOT EXISTS activity_fts USING fts5(kind UNINDEXED, ref UNINDEXED, ts UNINDEXED, text, tokenize = 'unicode61 remove_diacritics 2');");
    if (fts)
        Exec(m_wdb, "INSERT INTO activity_fts (rowid, kind, ref, ts, text) SELECT rowid, entity_type, entity_key, last_event_ts, display_name FROM inference;");
    if (ok)
    {
        std::string metaSql = std::string("INSERT OR REPLACE INTO meta (key, value) VALUES ('fts_available', '") + (fts ? "1" : "0") + "'), "
                              "('schema_v2_migrated_at', '" + std::to_string(StoreUtil::NowUtc()) + "');";
        must(metaSql.c_str());
    }
    must("PRAGMA user_version = 2;");

    if (!ok)
    {
        Exec(m_wdb, "ROLLBACK;");
        return false;
    }
    if (!Exec(m_wdb, "COMMIT;", &err))
    {
        WARP_LOG_ERROR("store", "migration COMMIT failed: %s", err.c_str());
        Exec(m_wdb, "ROLLBACK;");
        return false;
    }

    // Reclaimable free pages (secure clear / retention): switch to incremental auto-vacuum once.
    if (QueryInt(m_wdb, "PRAGMA auto_vacuum;", 0) != 2)
    {
        Exec(m_wdb, "PRAGMA auto_vacuum = INCREMENTAL;");
        Exec(m_wdb, "VACUUM;");
    }
    WARP_LOG_INFO("store", "schema migration to v2 complete (fts=%d)", fts ? 1 : 0);
    return true;
}

// =====================================================================
// Writer thread
// =====================================================================
namespace StoreUtil
{
    std::string AppEntityKey(const std::wstring& exePath, const std::wstring& aumid)
    {
        return !aumid.empty() ? EntityKey::AppKey(aumid) : EntityKey::AppKey(exePath);
    }

    std::string WebEntityKey(const std::wstring& canonicalUrl, const std::wstring& docId, const std::wstring& title)
    {
        if (!docId.empty())        return EntityKey::DocKey(docId);
        if (!canonicalUrl.empty()) return EntityKey::UrlKey(canonicalUrl);
        return EntityKey::Key(title);
    }
}

void ActivityStore::WriterLoop()
{
    SetThreadDescription(GetCurrentThread(), L"WARP store writer");
    std::vector<Job> batch;
    batch.reserve(kBatchMax);
    for (;;)
    {
        {
            std::unique_lock<std::mutex> lk(m_qMtx);
            m_qCv.wait_for(lk, std::chrono::milliseconds(1000),
                           [this] { return !m_queue.empty() || m_stopping.load(); });
            if (m_queue.empty())
            {
                if (m_stopping.load()) break;
                continue;
            }
            // Give bursts a moment to accumulate so they commit as one transaction.
            if (!m_stopping.load() && m_queue.size() < 64)
                m_qCv.wait_for(lk, std::chrono::milliseconds(40),
                               [this] { return m_queue.size() >= 64 || m_stopping.load(); });
            while (!m_queue.empty() && batch.size() < kBatchMax)
            {
                Job j = std::move(m_queue.front());
                m_queue.pop_front();
                // Bounded shutdown: past the deadline only critical jobs (clear/purge/RunSync) still run.
                if (m_stopping.load() && !j.critical && StoreUtil::NowMs() > m_stopDeadlineMs.load())
                {
                    std::lock_guard<std::mutex> sl(m_statsMtx);
                    ++m_stats.dropped;
                    continue;
                }
                batch.push_back(std::move(j));
            }
        }
        RunBatch(batch);
        batch.clear();
    }
}

void ActivityStore::RunBatch(std::vector<Job>& batch)
{
    if (batch.empty()) return;
    m_ctx.nowMs = StoreUtil::NowMs();

    bool inTxn = false;
    int64_t seqAtTxnStart = m_seq.load();
    uint64_t okJobs = 0, failedJobs = 0;
    std::vector<std::shared_ptr<std::promise<bool>>> pending;   // completions awaiting commit

    auto publishSeq = [this]()
    {
        {
            std::lock_guard<std::mutex> lk(m_seqMtx);
            m_committedSeq = m_seq.load();
        }
        m_seqCv.notify_all();
    };

    auto commit = [&]()
    {
        // Persist the sequence high-water mark with the rows it covers: numbers consumed by jobs that
        // were rolled back to their savepoint never appear in any table, and GetChanges must still be
        // able to advance a consumer's cursor past them (otherwise WaitForChanges keeps firing).
        if (inTxn && m_seq.load() > seqAtTxnStart)
        {
            sqlite3_stmt* hw = m_ctx.Stmt("INSERT OR REPLACE INTO meta (key, value) VALUES ('change_seq_hw', ?1);");
            if (hw)
            {
                const std::string v = std::to_string(m_seq.load());
                sqlite3_bind_text(hw, 1, v.c_str(), (int)v.size(), SQLITE_TRANSIENT);
                sqlite3_step(hw);
            }
        }
        // A cached SELECT left on a row keeps a read transaction open on the writer connection past
        // COMMIT, which makes every checkpoint fail (unbounded WAL) and VACUUM refuse to run.
        for (auto& kv : m_ctx.m_cache)
            if (kv.second) sqlite3_reset(kv.second);
        if (!inTxn) return;
        bool ok = Exec(m_wdb, "COMMIT;");
        if (!ok)
        {
            WARP_LOG_ERROR("store", "COMMIT failed: %s", sqlite3_errmsg(m_wdb));
            Exec(m_wdb, "ROLLBACK;");
            m_seq = seqAtTxnStart;
            // A clear-all in the failed batch may already have moved the in-memory reset marker.
            m_resetSeq = QueryInt(m_wdb, "SELECT CAST(value AS INTEGER) FROM meta WHERE key = 'reset_seq';", 0);
            // ... and a purge / clear its purge floors.
            if (m_floorsDirty) { m_floors = m_floorsCommitted; m_floorsDirty = false; }
            for (IWriterSink* s : m_sinks) s->OnRollback(m_ctx);
            std::lock_guard<std::mutex> lk(m_statsMtx);
            ++m_stats.writeErrors;
        }
        else
        {
            if (m_floorsDirty) { m_floorsCommitted = m_floors; m_floorsDirty = false; }
            publishSeq();
            std::lock_guard<std::mutex> lk(m_statsMtx);
            m_stats.lastCommitUtc = m_ctx.nowMs / 1000;
            ++m_stats.batches;
        }
        for (auto& p : pending) { try { p->set_value(ok); } catch (...) {} }
        pending.clear();
        inTxn = false;
    };

    auto runJob = [&](Job& j) -> bool
    {
        try { j.fn(m_ctx); return true; }
        catch (const std::exception& e) { WARP_LOG_ERROR("store", "write job failed: %s", e.what()); }
        catch (...) { WARP_LOG_ERROR("store", "write job failed: unknown exception"); }
        return false;
    };

    for (Job& j : batch)
    {
        if (j.noTxn)
        {
            commit();
            bool ok = runJob(j);
            ok ? ++okJobs : ++failedJobs;
            publishSeq();
            if (j.done) { try { j.done->set_value(ok); } catch (...) {} }
            continue;
        }
        if (!inTxn)
        {
            inTxn = Exec(m_wdb, "BEGIN IMMEDIATE;");
            seqAtTxnStart = m_seq.load();
            if (!inTxn)
            {
                WARP_LOG_ERROR("store", "BEGIN failed: %s", sqlite3_errmsg(m_wdb));
                ++failedJobs;
                if (j.done) { try { j.done->set_value(false); } catch (...) {} }
                continue;
            }
        }
        // Per-job savepoint: a failing job is rolled back alone instead of poisoning the batch.
        Exec(m_wdb, "SAVEPOINT job;");
        bool ok = runJob(j);
        if (ok)
        {
            Exec(m_wdb, "RELEASE job;");
            ++okJobs;
        }
        else
        {
            Exec(m_wdb, "ROLLBACK TO job;");
            Exec(m_wdb, "RELEASE job;");
            for (IWriterSink* s : m_sinks) s->OnRollback(m_ctx);   // caches may hold the undone changes
            ++failedJobs;
        }
        if (j.done)
        {
            if (ok) pending.push_back(j.done);
            else { try { j.done->set_value(false); } catch (...) {} }
        }
    }
    commit();

    std::lock_guard<std::mutex> lk(m_statsMtx);
    m_stats.written += okJobs;
    m_stats.writeErrors += failedJobs;
}

bool ActivityStore::Enqueue(WriteJob job, bool critical)
{
    if (!m_open.load() || m_stopping.load()) return false;
    {
        std::lock_guard<std::mutex> lk(m_qMtx);
        if (!critical && m_queue.size() >= kQueueCapacity)
        {
            std::lock_guard<std::mutex> sl(m_statsMtx);
            ++m_stats.dropped;
            return false;
        }
        Job j;
        j.fn = std::move(job);
        j.critical = critical;
        m_queue.push_back(std::move(j));
    }
    {
        std::lock_guard<std::mutex> sl(m_statsMtx);
        ++m_stats.enqueued;
    }
    m_qCv.notify_one();
    return true;
}

bool ActivityStore::Submit(Job job, DWORD timeoutMs)
{
    if (!m_open.load() || m_stopping.load()) return false;
    auto done = std::make_shared<std::promise<bool>>();
    std::future<bool> fut = done->get_future();
    job.done = done;
    job.critical = true;
    {
        std::lock_guard<std::mutex> lk(m_qMtx);
        m_queue.push_back(std::move(job));
    }
    m_qCv.notify_one();
    if (fut.wait_for(std::chrono::milliseconds(timeoutMs)) != std::future_status::ready)
        return false;
    try { return fut.get(); }
    catch (...) { return false; }
}

bool ActivityStore::RunSync(WriteJob job, DWORD timeoutMs)
{
    Job j;
    j.fn = std::move(job);
    return Submit(std::move(j), timeoutMs);
}

bool ActivityStore::EnqueueRecord(WriteJob job)
{
    return Enqueue(std::move(job), false);
}

// =====================================================================
// Typed records
// =====================================================================
namespace
{
    const char* const kInsFile =
        "INSERT INTO file_activity (timestamp, action, path, old_path, "
        "source_pid, source_exe, foreground_pid, foreground_exe, ms_since_input, parent_pid, parent_exe, created_window_ms, confidence, "
        "entity_key, event_ms, source, presence, is_dir, origin_url) "
        "VALUES (?,?,?,?, ?,?,?,?,?,?,?,?,?, ?,?,?,?,?,?);";
    const char* const kInsLaunch =
        "INSERT INTO app_launch_activity (timestamp, exe_name, exe_path, pid, "
        "source_pid, source_exe, foreground_pid, foreground_exe, ms_since_input, parent_pid, parent_exe, created_window_ms, confidence, "
        "entity_key, event_ms, source, creator_pid, creator_exe, aumid, session_id) "
        "VALUES (?,?,?,?, ?,?,?,?,?,?,?,?,?, ?,?,?,?,?,?,?);";
    const char* const kInsBrowse =
        "INSERT INTO browsing_activity (timestamp, browser, title, url, "
        "source_pid, source_exe, foreground_pid, foreground_exe, ms_since_input, parent_pid, parent_exe, created_window_ms, confidence, "
        "entity_key, event_ms, source, host, doc_id, doc_kind, dwell_secs, presence, window_id) "
        "VALUES (?,?,?,?, ?,?,?,?,?,?,?,?,?, ?,?,?,?,?,?,?,?,?);";
    const char* const kInsFocus =
        "INSERT INTO app_focus_activity (timestamp, exe_name, exe_path, window_title, duration_secs, "
        "source_pid, source_exe, foreground_pid, foreground_exe, ms_since_input, parent_pid, parent_exe, created_window_ms, confidence, "
        "entity_key, event_ms, source, start_ts, end_ts, active_secs, idle_secs, doc_key, doc_display, doc_type, window_class, desktop_id, aumid, presence) "
        "VALUES (?,?,?,?,?, ?,?,?,?,?,?,?,?,?, ?,?,?,?,?,?,?,?,?,?,?,?,?,?);";
    const char* const kInsPresence = "INSERT INTO presence_interval (start_ts, end_ts, state) VALUES (?,?,?);";
    const char* const kInsClip = "INSERT INTO clipboard_activity (timestamp, event_ms, owner_exe, formats) VALUES (?,?,?,?);";
    const char* const kInsPrint = "INSERT INTO print_activity (timestamp, event_ms, document, printer, pages) VALUES (?,?,?,?,?);";
    const char* const kProcExit =
        "UPDATE app_launch_activity SET exit_ts = ?1, lifetime_secs = MAX(0, ?1 - timestamp) "
        "WHERE id = (SELECT id FROM app_launch_activity WHERE pid = ?2 AND exit_ts IS NULL AND event_ms >= ?3 ORDER BY id DESC LIMIT 1);";

    int64_t EffectiveMs(int64_t ms, int64_t batchNowMs) { return ms > 0 ? ms : batchNowMs; }

    // Does purge floor (kind, v) cover a record with these identities? (Time ranges cover every record.)
    bool FloorCovers(int kind, const std::string& v, const std::string& key, const std::string& key2,
                     const std::string& host, const std::string& fileKey, const std::string& fileKey2)
    {
        auto under = [&v](const std::string& k)
        {
            return !k.empty() && k.size() >= v.size() && k.compare(0, v.size(), v) == 0 &&
                   (k.size() == v.size() || v.back() == '\\' || k[v.size()] == '\\');
        };
        switch (kind)
        {
        case 0: return true;
        case 1: return !v.empty() && (key == v || key2 == v);
        case 2:
            return !v.empty() && !host.empty() &&
                   (host == v || (host.size() > v.size() && host[host.size() - v.size() - 1] == '.' &&
                                  host.compare(host.size() - v.size(), v.size(), v) == 0));
        case 3: return !v.empty() && (under(fileKey) || under(fileKey2));
        default: return false;
        }
    }

    // Lowercase host of an http(s) URL, else empty.
    std::string UrlHost(const std::wstring& url)
    {
        const size_t p = url.find(L"://");
        if (p == std::wstring::npos || p > 8) return std::string();
        const size_t s = p + 3;
        const size_t e = url.find_first_of(L"/?#", s);
        std::wstring h = url.substr(s, e == std::wstring::npos ? std::wstring::npos : e - s);
        const size_t at = h.rfind(L'@');
        if (at != std::wstring::npos) h.erase(0, at + 1);
        const size_t colon = h.rfind(L':');
        if (colon != std::wstring::npos && h.find(L']') == std::wstring::npos) h.erase(colon);
        return EntityKey::Key(h);
    }
}

// ---------------------------------------------------------------------
// Purge floors (writer thread)
// ---------------------------------------------------------------------
bool ActivityStore::Suppressed(int64_t ms, const RecordIds& ids) const
{
    for (const PurgeFloor& f : m_floors)
        if (ms >= f.fromMs && ms <= f.toMs && FloorCovers(f.kind, f.value, ids.key, ids.key2, ids.host, ids.fileKey, ids.fileKey2))
            return true;
    return false;
}

std::vector<std::pair<int64_t, int64_t>> ActivityStore::UnpurgedParts(int64_t startMs, int64_t endMs, const RecordIds& ids) const
{
    std::vector<std::pair<int64_t, int64_t>> parts;
    parts.emplace_back(startMs, endMs);
    for (const PurgeFloor& f : m_floors)
    {
        if (parts.empty()) break;
        if (!FloorCovers(f.kind, f.value, ids.key, ids.key2, ids.host, ids.fileKey, ids.fileKey2)) continue;
        std::vector<std::pair<int64_t, int64_t>> next;
        for (const auto& p : parts)
        {
            if (p.second < f.fromMs || p.first > f.toMs) { next.push_back(p); continue; }
            if (p.first < f.fromMs) next.emplace_back(p.first, f.fromMs - 1);
            if (p.second > f.toMs) next.emplace_back(f.toMs + 1, p.second);
        }
        parts.swap(next);
    }
    return parts;
}

bool ActivityStore::PurgedBetween(int64_t t0, int64_t t1) const
{
    if (t1 < t0) std::swap(t0, t1);
    // Whole seconds: the earlier activity may end at the last ms of t0 and the later one start at the first ms of
    // t1, so a clear / purge "until now" ending inside t0 (pieces split there start in that second) is not between.
    const int64_t lo = t0 * 1000 + 999, hi = t1 * 1000;
    for (const PurgeFloor& f : m_floors)
        if (f.kind == 0 && f.fromMs <= hi && f.toMs >= lo) return true;
    return false;
}

void ActivityStore::AddFloor(WriterContext& w, PurgeFloor f)
{
    if (f.toMs < f.fromMs) return;
    m_floorsDirty = true;
    if (f.kind != 0)
    {
        size_t scoped = 0;
        for (const PurgeFloor& x : m_floors) if (x.kind != 0) ++scoped;
        if (scoped >= 64)
        {
            for (auto it = m_floors.begin(); it != m_floors.end(); ++it)
                if (it->kind != 0) { m_floors.erase(it); break; }   // the oldest scoped floor
        }
        m_floors.push_back(std::move(f));
        return;
    }
    // Time ranges: merged, newest 64 kept, persisted so a restart (USN journal replay) honors them too.
    std::vector<PurgeFloor> ranges, scopedFloors;
    for (PurgeFloor& x : m_floors) (x.kind == 0 ? ranges : scopedFloors).push_back(std::move(x));
    ranges.push_back(std::move(f));
    std::sort(ranges.begin(), ranges.end(), [](const PurgeFloor& l, const PurgeFloor& r) { return l.fromMs < r.fromMs; });
    std::vector<PurgeFloor> merged;
    for (PurgeFloor& r : ranges)
    {
        if (!merged.empty() && r.fromMs <= merged.back().toMs + 1)
            merged.back().toMs = (std::max)(merged.back().toMs, r.toMs);
        else
            merged.push_back(std::move(r));
    }
    if (merged.size() > 64) merged.erase(merged.begin(), merged.end() - 64);
    std::string v;
    for (const PurgeFloor& r : merged) v += std::to_string(r.fromMs) + ":" + std::to_string(r.toMs) + " ";
    m_floors = std::move(merged);
    for (PurgeFloor& x : scopedFloors) m_floors.push_back(std::move(x));
    sqlite3_stmt* st = w.Stmt("INSERT OR REPLACE INTO meta (key, value) VALUES ('purged_ranges', ?1);");
    if (st) { BindText(st, 1, v); sqlite3_step(st); }
}

void ActivityStore::LoadFloors()
{
    m_floors.clear();
    m_floorsCommitted.clear();
    m_floorsDirty = false;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(m_wdb, "SELECT value FROM meta WHERE key = 'purged_ranges';", -1, &st, nullptr) != SQLITE_OK) return;
    std::string v;
    if (sqlite3_step(st) == SQLITE_ROW) v = ColU(st, 0);
    sqlite3_finalize(st);
    size_t pos = 0;
    while (pos < v.size())
    {
        size_t sp = v.find(' ', pos);
        if (sp == std::string::npos) sp = v.size();
        const std::string item = v.substr(pos, sp - pos);
        pos = sp + 1;
        const size_t colon = item.find(':');
        if (colon == std::string::npos) continue;
        PurgeFloor f;
        f.fromMs = std::strtoll(item.c_str(), nullptr, 10);
        f.toMs = std::strtoll(item.c_str() + colon + 1, nullptr, 10);
        if (f.toMs >= f.fromMs) m_floors.push_back(std::move(f));
    }
    m_floorsCommitted = m_floors;
}

bool ActivityStore::RecordFile(FileEventRecord r)
{
    return EnqueueRecord([this, r = std::move(r)](WriterContext& w)
    {
        sqlite3_stmt* st = w.Stmt(kInsFile);
        if (!st) return;
        int64_t ms = EffectiveMs(r.eventMs, w.nowMs);
        const std::string key = EntityKey::FileKey(r.path);
        if (!m_floors.empty())
        {
            RecordIds ids;
            ids.key = key;
            ids.fileKey = key;
            if (!r.oldPath.empty()) ids.key2 = ids.fileKey2 = EntityKey::FileKey(r.oldPath);
            if (Suppressed(ms, ids)) return;   // reported after a purge of that time or item
        }
        sqlite3_bind_int64(st, 1, ms / 1000);
        BindText(st, 2, r.action);
        BindText(st, 3, r.path);
        BindText(st, 4, r.oldPath);
        BindEventContext(st, 5, r.ctx);
        BindText(st, 14, key);
        sqlite3_bind_int64(st, 15, ms);
        BindText(st, 16, r.source);
        BindText(st, 17, r.presence);
        sqlite3_bind_int(st, 18, r.isDir ? 1 : 0);
        BindText(st, 19, r.originUrl);
        if (sqlite3_step(st) != SQLITE_DONE)
            WARP_LOG_WARN("store", "file insert failed: %s", sqlite3_errmsg(w.db));
        FileEventRecord copy = r;
        copy.eventMs = ms;
        for (IWriterSink* s : m_sinks) s->OnFileEvent(w, copy);
    });
}

bool ActivityStore::RecordLaunch(LaunchEventRecord r)
{
    return EnqueueRecord([this, r = std::move(r)](WriterContext& w)
    {
        sqlite3_stmt* st = w.Stmt(kInsLaunch);
        if (!st) return;
        int64_t ms = EffectiveMs(r.eventMs, w.nowMs);
        const std::string key = StoreUtil::AppEntityKey(r.exePath, r.aumid);
        if (!m_floors.empty())
        {
            RecordIds ids;
            ids.key = key;
            if (Suppressed(ms, ids)) return;
        }
        sqlite3_bind_int64(st, 1, ms / 1000);
        BindText(st, 2, r.exeName);
        BindText(st, 3, r.exePath);
        sqlite3_bind_int64(st, 4, r.pid);
        BindEventContext(st, 5, r.ctx);
        BindText(st, 14, key);
        sqlite3_bind_int64(st, 15, ms);
        BindText(st, 16, std::string("etw"));
        if (r.creatorPid) sqlite3_bind_int64(st, 17, r.creatorPid); else sqlite3_bind_null(st, 17);
        BindText(st, 18, r.creatorExe);
        BindText(st, 19, r.aumid);
        sqlite3_bind_int64(st, 20, r.sessionId);
        if (sqlite3_step(st) != SQLITE_DONE)
            WARP_LOG_WARN("store", "launch insert failed: %s", sqlite3_errmsg(w.db));
        LaunchEventRecord copy = r;
        copy.eventMs = ms;
        for (IWriterSink* s : m_sinks) s->OnLaunchEvent(w, copy);
    });
}

bool ActivityStore::RecordProcessExit(ProcessExitRecord r)
{
    return EnqueueRecord([this, r](WriterContext& w)
    {
        sqlite3_stmt* st = w.Stmt(kProcExit);
        if (!st) return;
        int64_t ms = EffectiveMs(r.eventMs, w.nowMs);
        if (!m_floors.empty() && Suppressed(ms, RecordIds{})) return;
        sqlite3_bind_int64(st, 1, ms / 1000);
        sqlite3_bind_int64(st, 2, r.pid);
        sqlite3_bind_int64(st, 3, r.startMs > 0 ? r.startMs - 5000 : 0);
        sqlite3_step(st);
    });
}

bool ActivityStore::RecordBrowse(BrowseEventRecord r)
{
    return EnqueueRecord([this, r = std::move(r)](WriterContext& w)
    {
        sqlite3_stmt* st = w.Stmt(kInsBrowse);
        if (!st) return;
        int64_t ms = EffectiveMs(r.eventMs, w.nowMs);
        const std::string key = StoreUtil::WebEntityKey(r.url, r.docId, r.title);
        if (!m_floors.empty())
        {
            RecordIds ids;
            ids.key = key;
            ids.host = r.host.empty() ? UrlHost(r.url) : EntityKey::Key(r.host);
            if (Suppressed(ms, ids)) return;
        }
        sqlite3_bind_int64(st, 1, ms / 1000);
        BindText(st, 2, r.browser);
        BindText(st, 3, r.title);
        BindText(st, 4, r.url);
        BindEventContext(st, 5, r.ctx);
        BindText(st, 14, key);
        sqlite3_bind_int64(st, 15, ms);
        BindText(st, 16, std::string("uia"));
        BindText(st, 17, r.host);
        BindText(st, 18, r.docId);
        BindText(st, 19, r.docKind);
        sqlite3_bind_int(st, 20, r.dwellSecs);
        BindText(st, 21, r.presence);
        if (r.windowId) sqlite3_bind_int64(st, 22, r.windowId); else sqlite3_bind_null(st, 22);
        if (sqlite3_step(st) != SQLITE_DONE)
            WARP_LOG_WARN("store", "browse insert failed: %s", sqlite3_errmsg(w.db));
        BrowseEventRecord copy = r;
        copy.eventMs = ms;
        for (IWriterSink* s : m_sinks) s->OnBrowseEvent(w, copy);
    });
}

bool ActivityStore::RecordFocus(FocusEventRecord r)
{
    return EnqueueRecord([this, r = std::move(r)](WriterContext& w)
    {
        auto insert = [this, &w](const FocusEventRecord& f, int64_t startMs, int64_t endMs)
        {
            sqlite3_stmt* st = w.Stmt(kInsFocus);
            if (!st) return;
            int duration = (int)((endMs - startMs) / 1000);
            sqlite3_bind_int64(st, 1, endMs / 1000);
            BindText(st, 2, f.exeName);
            BindText(st, 3, f.exePath);
            BindText(st, 4, f.windowTitle);
            sqlite3_bind_int(st, 5, duration);
            BindEventContext(st, 6, f.ctx);
            BindText(st, 15, StoreUtil::AppEntityKey(f.exePath, f.aumid));
            sqlite3_bind_int64(st, 16, endMs);
            BindText(st, 17, std::string("focus"));
            sqlite3_bind_int64(st, 18, startMs / 1000);
            sqlite3_bind_int64(st, 19, endMs / 1000);
            sqlite3_bind_int(st, 20, f.activeSecs);
            sqlite3_bind_int(st, 21, f.idleSecs);
            BindText(st, 22, f.docKey);
            BindText(st, 23, f.docDisplay);
            BindText(st, 24, f.docType);
            BindText(st, 25, f.windowClass);
            BindText(st, 26, f.desktopId);
            BindText(st, 27, f.aumid);
            BindText(st, 28, f.presence);
            if (sqlite3_step(st) != SQLITE_DONE)
                WARP_LOG_WARN("store", "focus insert failed: %s", sqlite3_errmsg(w.db));
            FocusEventRecord copy = f;
            copy.endMs = endMs;
            copy.startMs = startMs;
            for (IWriterSink* s : m_sinks) s->OnFocusEvent(w, copy);
        };

        const int64_t endMs = EffectiveMs(r.endMs, w.nowMs);
        const int64_t startMs = r.startMs > 0 ? r.startMs : endMs;
        if (m_floors.empty())
        {
            insert(r, startMs, endMs);
            return;
        }
        RecordIds ids;
        ids.key = StoreUtil::AppEntityKey(r.exePath, r.aumid);
        ids.key2 = r.docKey;
        if (r.docType == "file") ids.fileKey = r.docKey;
        else ids.host = UrlHost(r.docDisplay);
        if (startMs > endMs)                           // malformed interval: judged by its end
        {
            if (!Suppressed(endMs, ids)) insert(r, startMs, endMs);
            return;
        }
        const std::vector<std::pair<int64_t, int64_t>> parts = UnpurgedParts(startMs, endMs, ids);
        if (parts.size() == 1 && parts[0].first == startMs && parts[0].second == endMs)
        {
            insert(r, startMs, endMs);
            return;
        }
        // A session that was still open during a purge keeps only its parts outside what was purged.
        const double total = (double)(std::max)((int64_t)1, endMs - startMs);
        for (const auto& p : parts)
        {
            const double share = (double)(p.second - p.first) / total;
            FocusEventRecord piece = r;
            piece.activeSecs = (int)(r.activeSecs * share + 0.5);
            piece.idleSecs = (int)(r.idleSecs * share + 0.5);
            insert(piece, p.first, p.second);
        }
    });
}

bool ActivityStore::RecordPresence(PresenceRecord r)
{
    return EnqueueRecord([this, r](WriterContext& w)
    {
        const int64_t endMs = EffectiveMs(r.endMs, w.nowMs);
        std::vector<std::pair<int64_t, int64_t>> parts;
        if (m_floors.empty())
            parts.emplace_back(r.startMs, endMs);
        else if (r.startMs > endMs)                    // malformed interval: judged by its end
        {
            if (!Suppressed(endMs, RecordIds{})) parts.emplace_back(r.startMs, endMs);
        }
        else
            parts = UnpurgedParts(r.startMs, endMs, RecordIds{});   // an interval still open during a purge
        for (const auto& p : parts)
        {
            sqlite3_stmt* st = w.Stmt(kInsPresence);
            if (!st) return;
            sqlite3_bind_int64(st, 1, p.first / 1000);
            sqlite3_bind_int64(st, 2, p.second / 1000);
            BindText(st, 3, r.state);
            sqlite3_step(st);
            PresenceRecord piece = r;
            piece.startMs = p.first;
            piece.endMs = p.second;
            for (IWriterSink* s : m_sinks) s->OnPresence(w, piece);
        }
    });
}

bool ActivityStore::RecordClipboard(ClipboardRecord r)
{
    return EnqueueRecord([this, r](WriterContext& w)
    {
        sqlite3_stmt* st = w.Stmt(kInsClip);
        if (!st) return;
        int64_t ms = EffectiveMs(r.eventMs, w.nowMs);
        if (!m_floors.empty() && Suppressed(ms, RecordIds{})) return;
        sqlite3_bind_int64(st, 1, ms / 1000);
        sqlite3_bind_int64(st, 2, ms);
        BindText(st, 3, r.ownerExe);
        BindText(st, 4, r.formats);
        sqlite3_step(st);
    });
}

bool ActivityStore::RecordPrint(PrintRecord r)
{
    return EnqueueRecord([this, r](WriterContext& w)
    {
        sqlite3_stmt* st = w.Stmt(kInsPrint);
        if (!st) return;
        int64_t ms = EffectiveMs(r.eventMs, w.nowMs);
        if (!m_floors.empty() && Suppressed(ms, RecordIds{})) return;
        sqlite3_bind_int64(st, 1, ms / 1000);
        sqlite3_bind_int64(st, 2, ms);
        BindText(st, 3, r.document);
        BindText(st, 4, r.printer);
        sqlite3_bind_int64(st, 5, r.pages);
        sqlite3_step(st);
    });
}

// =====================================================================
// Reads
// =====================================================================
ActivityStore::ReadLease& ActivityStore::ReadLease::operator=(ReadLease&& o) noexcept
{
    if (this != &o)
    {
        if (m_store && m_db) m_store->ReleaseReader(m_db);
        m_store = o.m_store;
        m_db = o.m_db;
        o.m_store = nullptr;
        o.m_db = nullptr;
    }
    return *this;
}

ActivityStore::ReadLease::~ReadLease()
{
    if (m_store && m_db) m_store->ReleaseReader(m_db);
}

ActivityStore::ReadLease ActivityStore::AcquireReader(DWORD timeoutMs)
{
    std::unique_lock<std::mutex> lk(m_readMtx);
    if (!m_readCv.wait_for(lk, std::chrono::milliseconds(timeoutMs),
                           [this] { return !m_freeReaders.empty() || !m_open.load(); }))
        return ReadLease();
    if (!m_open.load() || m_freeReaders.empty())
        return ReadLease();
    sqlite3* db = m_freeReaders.back();
    m_freeReaders.pop_back();
    return ReadLease(this, db);
}

void ActivityStore::ReleaseReader(sqlite3* db)
{
    {
        std::lock_guard<std::mutex> lk(m_readMtx);
        // A lease returned after Close() stopped waiting for it refers to a closed connection.
        if (std::find(m_readers.begin(), m_readers.end(), db) != m_readers.end())
            m_freeReaders.push_back(db);
    }
    m_readCv.notify_all();
}

namespace
{
    // Common bindings: ?1 start, ?2 end, ?3 beforeId (0 = none), ?4 minConfidence, ?5 limit (-1 = all)
    void BindRange(sqlite3_stmt* st, int64_t startUtc, int64_t endUtc, int64_t beforeId, double minConf, int limit)
    {
        sqlite3_bind_int64(st, 1, startUtc);
        sqlite3_bind_int64(st, 2, endUtc > 0 ? endUtc : INT64_MAX);
        sqlite3_bind_int64(st, 3, beforeId);
        sqlite3_bind_double(st, 4, minConf);
        sqlite3_bind_int(st, 5, limit > 0 ? limit : -1);
    }
}

std::vector<FileActivity> ActivityStore::QueryFiles(int64_t startUtc, int64_t endUtc, int limit, int64_t beforeId, double minConfidence)
{
    std::vector<FileActivity> out;
    ReadLease rl = AcquireReader();
    if (!rl) return out;
    const char* sql =
        "SELECT id, timestamp, COALESCE(event_ms, timestamp * 1000), action, path, old_path, COALESCE(source, 'etw'), source_exe, entity_key, "
        "COALESCE(confidence, 1.0), COALESCE(is_dir, 0) FROM file_activity "
        "WHERE timestamp >= ?1 AND timestamp <= ?2 AND COALESCE(confidence, 1.0) >= ?4 "
        "AND (?3 = 0 OR timestamp < (SELECT timestamp FROM file_activity WHERE id = ?3) "
        "     OR (timestamp = (SELECT timestamp FROM file_activity WHERE id = ?3) AND id < ?3)) "
        "ORDER BY timestamp DESC, id DESC LIMIT ?5;";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(rl.get(), sql, -1, &st, nullptr) != SQLITE_OK) return out;
    BindRange(st, startUtc, endUtc, beforeId, minConfidence, limit);
    while (sqlite3_step(st) == SQLITE_ROW)
    {
        FileActivity f;
        f.id = sqlite3_column_int64(st, 0);
        f.timestampUtc = sqlite3_column_int64(st, 1);
        f.eventMs = sqlite3_column_int64(st, 2);
        f.action = ColW(st, 3);
        f.path = ColW(st, 4);
        f.oldPath = ColW(st, 5);
        f.source = ColW(st, 6);
        f.sourceExe = ColW(st, 7);
        f.entityKey = ColU(st, 8);
        f.confidence = sqlite3_column_double(st, 9);
        f.isDir = sqlite3_column_int(st, 10) != 0;
        out.push_back(std::move(f));
    }
    sqlite3_finalize(st);
    return out;
}

std::vector<AppLaunchActivity> ActivityStore::QueryLaunches(int64_t startUtc, int64_t endUtc, int limit, int64_t beforeId, double minConfidence)
{
    std::vector<AppLaunchActivity> out;
    ReadLease rl = AcquireReader();
    if (!rl) return out;
    const char* sql =
        "SELECT id, timestamp, exe_name, exe_path, pid, creator_exe, aumid, COALESCE(confidence, 1.0) FROM app_launch_activity "
        "WHERE timestamp >= ?1 AND timestamp <= ?2 AND COALESCE(confidence, 1.0) >= ?4 "
        "AND (?3 = 0 OR timestamp < (SELECT timestamp FROM app_launch_activity WHERE id = ?3) "
        "     OR (timestamp = (SELECT timestamp FROM app_launch_activity WHERE id = ?3) AND id < ?3)) "
        "ORDER BY timestamp DESC, id DESC LIMIT ?5;";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(rl.get(), sql, -1, &st, nullptr) != SQLITE_OK) return out;
    BindRange(st, startUtc, endUtc, beforeId, minConfidence, limit);
    while (sqlite3_step(st) == SQLITE_ROW)
    {
        AppLaunchActivity a;
        a.id = sqlite3_column_int64(st, 0);
        a.timestampUtc = sqlite3_column_int64(st, 1);
        a.exeName = ColW(st, 2);
        a.exePath = ColW(st, 3);
        a.pid = (DWORD)sqlite3_column_int64(st, 4);
        a.creatorExe = ColW(st, 5);
        a.aumid = ColW(st, 6);
        a.confidence = sqlite3_column_double(st, 7);
        out.push_back(std::move(a));
    }
    sqlite3_finalize(st);
    return out;
}

std::vector<BrowsingActivity> ActivityStore::QueryBrowsing(int64_t startUtc, int64_t endUtc, int limit, int64_t beforeId, double minConfidence)
{
    std::vector<BrowsingActivity> out;
    ReadLease rl = AcquireReader();
    if (!rl) return out;
    const char* sql =
        "SELECT id, timestamp, browser, title, url, host, doc_id, entity_key, COALESCE(dwell_secs, 0), COALESCE(confidence, 1.0), "
        "COALESCE(window_id, 0) FROM browsing_activity "
        "WHERE timestamp >= ?1 AND timestamp <= ?2 AND COALESCE(confidence, 1.0) >= ?4 "
        "AND (?3 = 0 OR timestamp < (SELECT timestamp FROM browsing_activity WHERE id = ?3) "
        "     OR (timestamp = (SELECT timestamp FROM browsing_activity WHERE id = ?3) AND id < ?3)) "
        "ORDER BY timestamp DESC, id DESC LIMIT ?5;";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(rl.get(), sql, -1, &st, nullptr) != SQLITE_OK) return out;
    BindRange(st, startUtc, endUtc, beforeId, minConfidence, limit);
    while (sqlite3_step(st) == SQLITE_ROW)
    {
        BrowsingActivity b;
        b.id = sqlite3_column_int64(st, 0);
        b.timestampUtc = sqlite3_column_int64(st, 1);
        b.browser = ColW(st, 2);
        b.title = ColW(st, 3);
        b.url = ColW(st, 4);
        b.host = ColW(st, 5);
        b.docId = ColW(st, 6);
        b.entityKey = ColU(st, 7);
        b.dwellSecs = sqlite3_column_int(st, 8);
        b.confidence = sqlite3_column_double(st, 9);
        b.windowId = sqlite3_column_int64(st, 10);
        out.push_back(std::move(b));
    }
    sqlite3_finalize(st);
    return out;
}

std::vector<AppFocusActivity> ActivityStore::QueryFocus(int64_t startUtc, int64_t endUtc, int limit, int64_t beforeId, double minConfidence)
{
    std::vector<AppFocusActivity> out;
    ReadLease rl = AcquireReader();
    if (!rl) return out;
    const char* sql =
        "SELECT id, timestamp, COALESCE(start_ts, timestamp - duration_secs), COALESCE(end_ts, timestamp), exe_name, exe_path, window_title, "
        "duration_secs, COALESCE(active_secs, duration_secs), doc_key, doc_display, doc_type, COALESCE(confidence, 1.0) FROM app_focus_activity "
        "WHERE COALESCE(end_ts, timestamp) >= ?1 AND COALESCE(start_ts, timestamp - duration_secs) <= ?2 AND COALESCE(confidence, 1.0) >= ?4 "
        "AND (?3 = 0 OR COALESCE(end_ts, timestamp) < (SELECT COALESCE(end_ts, timestamp) FROM app_focus_activity WHERE id = ?3) "
        "     OR (COALESCE(end_ts, timestamp) = (SELECT COALESCE(end_ts, timestamp) FROM app_focus_activity WHERE id = ?3) AND id < ?3)) "
        "ORDER BY COALESCE(end_ts, timestamp) DESC, id DESC LIMIT ?5;";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(rl.get(), sql, -1, &st, nullptr) != SQLITE_OK) return out;
    BindRange(st, startUtc, endUtc, beforeId, minConfidence, limit);
    while (sqlite3_step(st) == SQLITE_ROW)
    {
        AppFocusActivity f;
        f.id = sqlite3_column_int64(st, 0);
        f.timestampUtc = sqlite3_column_int64(st, 1);
        f.startTs = sqlite3_column_int64(st, 2);
        f.endTs = sqlite3_column_int64(st, 3);
        f.exeName = ColW(st, 4);
        f.exePath = ColW(st, 5);
        f.windowTitle = ColW(st, 6);
        f.durationSecs = sqlite3_column_int(st, 7);
        f.activeSecs = sqlite3_column_int(st, 8);
        f.docKey = ColU(st, 9);
        f.docDisplay = ColW(st, 10);
        f.docType = ColU(st, 11);
        f.confidence = sqlite3_column_double(st, 12);
        out.push_back(std::move(f));
    }
    sqlite3_finalize(st);
    return out;
}

std::vector<FileActivity> ActivityStore::QueryFilesCustomSeconds(int64_t seconds)
{
    int64_t now = StoreUtil::NowUtc();
    return QueryFiles(now - seconds, now + 60);
}

std::vector<AppLaunchActivity> ActivityStore::QueryAppLaunchesCustomSeconds(int64_t seconds)
{
    int64_t now = StoreUtil::NowUtc();
    return QueryLaunches(now - seconds, now + 60);
}

std::vector<BrowsingActivity> ActivityStore::QueryBrowsingCustomSeconds(int64_t seconds)
{
    int64_t now = StoreUtil::NowUtc();
    return QueryBrowsing(now - seconds, now + 60);
}

std::vector<AppFocusActivity> ActivityStore::QueryAppFocusCustomSeconds(int64_t seconds)
{
    int64_t now = StoreUtil::NowUtc();
    return QueryFocus(now - seconds, now + 60);
}

int64_t ActivityStore::WindowToSeconds(TimeWindow w)
{
    switch (w)
    {
    case TimeWindow::Minutes15: return 15LL * 60;
    case TimeWindow::Minutes30: return 30LL * 60;
    case TimeWindow::Hours1:    return 3600LL;
    case TimeWindow::Hours2:    return 2LL * 3600;
    case TimeWindow::Hours6:    return 6LL * 3600;
    case TimeWindow::Hours24:   return 24LL * 3600;
    case TimeWindow::Days7:     return 7LL * 86400;
    case TimeWindow::Days15:    return 15LL * 86400;
    case TimeWindow::Days30:    return 30LL * 86400;
    default:                    return 3600LL;
    }
}

// =====================================================================
// Maintenance, clear, purge
// =====================================================================
namespace
{
    int64_t ExecDelete(sqlite3* db, const char* sql, int64_t a, int64_t b = 0)
    {
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK)
        {
            WARP_LOG_WARN("store", "prepare failed: %s | %.100s", sqlite3_errmsg(db), sql);
            return 0;
        }
        int n = sqlite3_bind_parameter_count(st);
        if (n >= 1) sqlite3_bind_int64(st, 1, a);
        if (n >= 2) sqlite3_bind_int64(st, 2, b);
        sqlite3_step(st);
        sqlite3_finalize(st);
        return sqlite3_changes(db);
    }

    // Delete one inference entity, recording a tombstone for the change feed and evicting
    // it from every sink's write-through cache (so a later event can't resurrect purged state).
    void TombstoneEntity(WriterContext& w, const std::vector<IWriterSink*>& sinks,
                         const std::string& key, const std::string& type, const char* reason)
    {
        for (IWriterSink* s : sinks) s->OnEntityRemoved(w, key);
        sqlite3_stmt* ts = w.Stmt("INSERT OR REPLACE INTO tombstone (change_seq, entity_key, entity_type, ts, reason) VALUES (?,?,?,?,?);");
        if (ts)
        {
            sqlite3_bind_int64(ts, 1, w.NextChangeSeq());
            BindText(ts, 2, key);
            BindText(ts, 3, type);
            sqlite3_bind_int64(ts, 4, w.nowMs / 1000);
            sqlite3_bind_text(ts, 5, reason, -1, SQLITE_STATIC);
            sqlite3_step(ts);
        }
        if (w.ftsAvailable)
        {
            sqlite3_stmt* fts = w.Stmt("DELETE FROM activity_fts WHERE rowid = (SELECT rowid FROM inference WHERE entity_key = ?);");
            if (fts) { BindText(fts, 1, key); sqlite3_step(fts); }
        }
        sqlite3_stmt* del = w.Stmt("DELETE FROM inference WHERE entity_key = ?;");
        if (del) { BindText(del, 1, key); sqlite3_step(del); }
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
}

void ActivityStore::Tombstone(WriterContext& w, const std::string& entityKey, const std::string& entityType, const char* reason)
{
    TombstoneEntity(w, m_sinks, entityKey, entityType, reason);
}

void ActivityStore::DoMaintenance(WriterContext& w)
{
    int64_t now = w.nowMs / 1000;
    int64_t rawCut = now - (int64_t)m_rawRetentionDays.load() * 86400;
    int64_t entCut = now - (int64_t)m_entityRetentionDays.load() * 86400;
    int64_t dayCut = StoreUtil::LocalDayIndex(entCut);
    int64_t n = 0;

    // Consumers whose cursor is older than the newest pruned tombstone may have missed deletes:
    // remember that horizon so GetChanges can tell them to resync.
    int64_t tombCut = now - 90LL * 86400;
    {
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(w.db, "SELECT MAX(change_seq) FROM tombstone WHERE ts < ?1;", -1, &st, nullptr) == SQLITE_OK)
        {
            sqlite3_bind_int64(st, 1, tombCut);
            if (sqlite3_step(st) == SQLITE_ROW && sqlite3_column_type(st, 0) != SQLITE_NULL)
            {
                std::string sql = "INSERT INTO meta (key, value) VALUES ('tombstone_floor_seq', '" +
                                  std::to_string(sqlite3_column_int64(st, 0)) + "') ON CONFLICT(key) DO UPDATE SET value = "
                                  "CAST(MAX(CAST(value AS INTEGER), CAST(excluded.value AS INTEGER)) AS TEXT);";
                sqlite3_finalize(st);
                st = nullptr;
                Exec(w.db, sql.c_str());
            }
            if (st) sqlite3_finalize(st);
        }
    }

    n += ExecDelete(w.db, "DELETE FROM file_activity WHERE timestamp < ?1;", rawCut);
    n += ExecDelete(w.db, "DELETE FROM app_launch_activity WHERE timestamp < ?1;", rawCut);
    n += ExecDelete(w.db, "DELETE FROM browsing_activity WHERE timestamp < ?1;", rawCut);
    n += ExecDelete(w.db, "DELETE FROM app_focus_activity WHERE timestamp < ?1;", rawCut);
    n += ExecDelete(w.db, "DELETE FROM presence_interval WHERE end_ts < ?1;", rawCut);
    n += ExecDelete(w.db, "DELETE FROM context_snapshot WHERE ts < ?1;", rawCut);
    n += ExecDelete(w.db, "DELETE FROM clipboard_activity WHERE timestamp < ?1;", rawCut);
    n += ExecDelete(w.db, "DELETE FROM print_activity WHERE timestamp < ?1;", rawCut);
    n += ExecDelete(w.db, "DELETE FROM entity_day WHERE day < ?1;", dayCut);
    n += ExecDelete(w.db, "DELETE FROM edge WHERE last_ts < ?1;", entCut);
    n += ExecDelete(w.db, "DELETE FROM work_session WHERE end_ts < ?1;", entCut);
    n += ExecDelete(w.db, "DELETE FROM thread_span WHERE end_ts < ?1;", entCut);
    n += ExecDelete(w.db, "DELETE FROM thread_member WHERE thread_id IN (SELECT id FROM work_thread WHERE last_ts < ?1);", entCut);
    n += ExecDelete(w.db, "DELETE FROM work_thread WHERE last_ts < ?1;", entCut);
    n += ExecDelete(w.db, "DELETE FROM tombstone WHERE ts < ?1;", tombCut);

    // Entities with no activity inside the entity retention window: tombstone + delete.
    std::vector<std::pair<std::string, std::string>> stale;
    {
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(w.db, "SELECT entity_key, entity_type FROM inference WHERE COALESCE(last_event_ts, 0) < ?1 LIMIT 20000;", -1, &st, nullptr) == SQLITE_OK)
        {
            sqlite3_bind_int64(st, 1, entCut);
            while (sqlite3_step(st) == SQLITE_ROW)
                stale.emplace_back(ColU(st, 0), ColU(st, 1));
            sqlite3_finalize(st);
        }
    }
    for (const auto& e : stale)
        TombstoneEntity(w, m_sinks, e.first, e.second, "retention");

    for (IWriterSink* s : m_sinks) s->OnMaintenance(w, now);
    WARP_LOG_INFO("store", "maintenance: %lld raw/derived rows and %zu entities expired (raw=%dd, entity=%dd)",
                  (long long)n, stale.size(), m_rawRetentionDays.load(), m_entityRetentionDays.load());
}

bool ActivityStore::ScheduleMaintenance()
{
    bool ok = Enqueue([this](WriterContext& w) { DoMaintenance(w); }, true);
    Job post;
    post.noTxn = true;
    post.critical = true;
    post.fn = [](WriterContext& w)
    {
        Exec(w.db, "PRAGMA incremental_vacuum(4000);");
        Exec(w.db, "PRAGMA wal_checkpoint(TRUNCATE);");
    };
    {
        std::lock_guard<std::mutex> lk(m_qMtx);
        if (m_open.load()) m_queue.push_back(std::move(post));
    }
    m_qCv.notify_one();
    return ok;
}

void ActivityStore::DoClearAll(WriterContext& w)
{
    static const char* const kTables[] = {
        "file_activity", "app_launch_activity", "browsing_activity", "app_focus_activity", "inference",
        "entity_day", "tombstone", "presence_interval", "context_snapshot", "edge", "work_session",
        "work_thread", "thread_member", "thread_span", "clipboard_activity", "print_activity",
    };
    for (const char* t : kTables)
    {
        std::string sql = std::string("DELETE FROM ") + t + ";";
        Exec(w.db, sql.c_str());
    }
    if (w.ftsAvailable) Exec(w.db, "DELETE FROM activity_fts;");
    int64_t reset = w.NextChangeSeq();
    m_resetSeq = reset;
    std::string meta = "INSERT OR REPLACE INTO meta (key, value) VALUES ('reset_seq', '" + std::to_string(reset) + "');";
    Exec(w.db, meta.c_str());
    // Nothing reported later about the time before the clear is stored (this range covers every scoped floor).
    m_floors.clear();
    PurgeFloor everything;
    everything.toMs = w.nowMs - 1;
    AddFloor(w, std::move(everything));
    for (IWriterSink* s : m_sinks) s->OnClearAll(w);
}

bool ActivityStore::ClearAll(DWORD timeoutMs)
{
    auto job = [this](WriterContext& w)
    {
        // A failed job rolls back to its savepoint: its purge floors and reset marker must go with it.
        std::vector<PurgeFloor> saved = m_floors;
        const bool dirty = m_floorsDirty;
        const int64_t resetSeq = m_resetSeq.load();
        try { DoClearAll(w); }
        catch (...) { m_floors = std::move(saved); m_floorsDirty = dirty; m_resetSeq = resetSeq; throw; }
    };
    if (!RunSync(job, timeoutMs))
        return false;
    Job post;
    post.noTxn = true;
    post.fn = [](WriterContext& w)
    {
        // secure_delete already zeroed the freed pages; truncate the WAL and rebuild the file
        // so no deleted content survives in free pages or the -wal file.
        Exec(w.db, "PRAGMA wal_checkpoint(TRUNCATE);");
        Exec(w.db, "VACUUM;");
        Exec(w.db, "PRAGMA wal_checkpoint(TRUNCATE);");
    };
    bool ok = Submit(std::move(post), timeoutMs);
    WARP_LOG_INFO("store", "clear-all completed (vacuum ok=%d)", ok ? 1 : 0);
    return true;
}

int64_t ActivityStore::DoPurge(WriterContext& w, const PurgeSpec& spec)
{
    int64_t n = 0;
    auto bindRun = [&](const char* sql, const std::string& a, const std::string& b = std::string()) -> int64_t
    {
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(w.db, sql, -1, &st, nullptr) != SQLITE_OK) return 0;
        int pc = sqlite3_bind_parameter_count(st);
        if (pc >= 1) BindText(st, 1, a);
        if (pc >= 2) BindText(st, 2, b);
        sqlite3_step(st);
        sqlite3_finalize(st);
        return sqlite3_changes(w.db);
    };
    // Work threads that held purged items or purged time: their label (a summary line) and centroid may
    // describe the purged activity, so they are re-derived at the end.
    std::vector<int64_t> threads;
    auto collectThreads = [&](const char* sql, const std::string& p1, const std::string& p2 = std::string())
    {
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(w.db, sql, -1, &st, nullptr) != SQLITE_OK) return;
        int pc = sqlite3_bind_parameter_count(st);
        if (pc >= 1) BindText(st, 1, p1);
        if (pc >= 2) BindText(st, 2, p2);
        while (sqlite3_step(st) == SQLITE_ROW) threads.push_back(sqlite3_column_int64(st, 0));
        sqlite3_finalize(st);
    };

    if (!spec.entityKey.empty())
    {
        const std::string& k = spec.entityKey;
        collectThreads("SELECT thread_id FROM thread_member WHERE entity_key = ?1 "
                       "UNION SELECT thread_id FROM context_snapshot WHERE thread_id IS NOT NULL AND instr(lower(replace(json, '\\\\', '\\')), ?1) > 0;", k);
        n += bindRun("DELETE FROM file_activity WHERE entity_key = ?1;", k);
        n += bindRun("DELETE FROM app_launch_activity WHERE entity_key = ?1;", k);
        n += bindRun("DELETE FROM browsing_activity WHERE entity_key = ?1;", k);
        n += bindRun("DELETE FROM app_focus_activity WHERE entity_key = ?1 OR doc_key = ?1;", k);
        n += bindRun("DELETE FROM entity_day WHERE entity_key = ?1;", k);
        n += bindRun("DELETE FROM edge WHERE a = ?1 OR b = ?1;", k);
        n += bindRun("DELETE FROM thread_member WHERE entity_key = ?1;", k);
        std::string type;
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(w.db, "SELECT entity_type FROM inference WHERE entity_key = ?1;", -1, &st, nullptr) == SQLITE_OK)
        {
            BindText(st, 1, k);
            if (sqlite3_step(st) == SQLITE_ROW) type = ColU(st, 0);
            sqlite3_finalize(st);
        }
        TombstoneEntity(w, m_sinks, k, type, "purge");
        ++n;
        n += bindRun("DELETE FROM context_snapshot WHERE instr(lower(replace(json, '\\\\', '\\')), ?1) > 0;", k);
        AddFloor(w, PurgeFloor{ 1, k, 0, w.nowMs - 1 });
    }
    if (!spec.domain.empty())
    {
        std::string exact = EntityKey::Key(spec.domain);
        std::string d = EscapeLike(exact);
        n += bindRun("DELETE FROM browsing_activity WHERE host = ?1 OR host LIKE '%.' || ?2 ESCAPE '^';", exact, d);
        n += bindRun("DELETE FROM browsing_activity WHERE url LIKE 'http%://' || ?1 || '/%' ESCAPE '^' OR url LIKE 'http%://' || ?1 ESCAPE '^' "
                     "OR url LIKE 'http%://%.' || ?1 || '/%' ESCAPE '^';", d);
        std::vector<std::pair<std::string, std::string>> victims;
        sqlite3_stmt* st = nullptr;
        // Document-keyed pages ("gdoc:...", "yt:...") only carry the site in their display URL.
        if (sqlite3_prepare_v2(w.db,
            "SELECT entity_key, entity_type FROM inference WHERE entity_type = 'url' AND "
            "(entity_key LIKE 'http%://' || ?1 || '/%' ESCAPE '^' OR entity_key LIKE 'http%://' || ?1 ESCAPE '^' OR entity_key LIKE 'http%://%.' || ?1 || '/%' ESCAPE '^' "
            "OR display_name LIKE 'http%://' || ?1 || '/%' ESCAPE '^' OR display_name LIKE 'http%://' || ?1 ESCAPE '^' OR display_name LIKE 'http%://%.' || ?1 || '/%' ESCAPE '^');",
            -1, &st, nullptr) == SQLITE_OK)
        {
            BindText(st, 1, d);
            while (sqlite3_step(st) == SQLITE_ROW) victims.emplace_back(ColU(st, 0), ColU(st, 1));
            sqlite3_finalize(st);
        }
        for (const auto& v : victims)
        {
            collectThreads("SELECT thread_id FROM thread_member WHERE entity_key = ?1;", v.first);
            bindRun("DELETE FROM entity_day WHERE entity_key = ?1;", v.first);
            bindRun("DELETE FROM edge WHERE a = ?1 OR b = ?1;", v.first);
            bindRun("DELETE FROM thread_member WHERE entity_key = ?1;", v.first);
            bindRun("DELETE FROM app_focus_activity WHERE doc_key = ?1;", v.first);
            TombstoneEntity(w, m_sinks, v.first, v.second, "purge");
            ++n;
        }
        collectThreads("SELECT thread_id FROM context_snapshot WHERE thread_id IS NOT NULL AND instr(lower(json), ?1) > 0;", exact);
        n += bindRun("DELETE FROM context_snapshot WHERE instr(lower(json), ?1) > 0;", exact);
        AddFloor(w, PurgeFloor{ 2, exact, 0, w.nowMs - 1 });
    }
    if (!spec.pathPrefix.empty())
    {
        std::string exact = EntityKey::FileKey(spec.pathPrefix);
        while (exact.size() > 3 && exact.back() == '\\') exact.pop_back();   // keep "c:\" intact
        std::string p = EscapeLike(exact);
        if (!p.empty() && p.back() == '\\') p.pop_back();                      // drive root: "c:" + "\%"
        n += bindRun("DELETE FROM file_activity WHERE entity_key = ?1 OR entity_key LIKE ?2 || '\\%' ESCAPE '^' "
                     "OR warp_file_key(old_path) = ?1 OR warp_file_key(old_path) LIKE ?2 || '\\%' ESCAPE '^';", exact, p);
        collectThreads("SELECT thread_id FROM thread_member WHERE entity_key = ?1 OR entity_key LIKE ?2 || '\\%' ESCAPE '^';", exact, p);
        // Derived rows of files under the folder that are no longer (or never were) ranked entities.
        n += bindRun("DELETE FROM app_focus_activity WHERE doc_key = ?1 OR doc_key LIKE ?2 || '\\%' ESCAPE '^';", exact, p);
        n += bindRun("DELETE FROM entity_day WHERE entity_key = ?1 OR entity_key LIKE ?2 || '\\%' ESCAPE '^';", exact, p);
        n += bindRun("DELETE FROM edge WHERE a = ?1 OR a LIKE ?2 || '\\%' ESCAPE '^' OR b = ?1 OR b LIKE ?2 || '\\%' ESCAPE '^';", exact, p);
        n += bindRun("DELETE FROM thread_member WHERE entity_key = ?1 OR entity_key LIKE ?2 || '\\%' ESCAPE '^';", exact, p);
        std::vector<std::pair<std::string, std::string>> victims;
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(w.db, "SELECT entity_key, entity_type FROM inference WHERE entity_type = 'file' AND (entity_key = ?1 OR entity_key LIKE ?2 || '\\%' ESCAPE '^');", -1, &st, nullptr) == SQLITE_OK)
        {
            BindText(st, 1, exact);
            BindText(st, 2, p);
            while (sqlite3_step(st) == SQLITE_ROW) victims.emplace_back(ColU(st, 0), ColU(st, 1));
            sqlite3_finalize(st);
        }
        for (const auto& v : victims)
        {
            bindRun("DELETE FROM entity_day WHERE entity_key = ?1;", v.first);
            bindRun("DELETE FROM edge WHERE a = ?1 OR b = ?1;", v.first);
            bindRun("DELETE FROM thread_member WHERE entity_key = ?1;", v.first);
            bindRun("DELETE FROM app_focus_activity WHERE doc_key = ?1;", v.first);
            TombstoneEntity(w, m_sinks, v.first, v.second, "purge");
            ++n;
        }
        collectThreads("SELECT thread_id FROM context_snapshot WHERE thread_id IS NOT NULL AND instr(lower(replace(json, '\\\\', '\\')), ?1) > 0;", exact);
        n += bindRun("DELETE FROM context_snapshot WHERE instr(lower(replace(json, '\\\\', '\\')), ?1) > 0;", exact);
        AddFloor(w, PurgeFloor{ 3, exact, 0, w.nowMs - 1 });
    }
    if (spec.startUtc > 0 || spec.endUtc > 0)
    {
        int64_t a = spec.startUtc;
        int64_t b = spec.endUtc > 0 ? spec.endUtc : INT64_MAX;
        const int64_t nowSec = w.nowMs / 1000;
        const int64_t bClamp = (std::min)(b, (int64_t)4102444800LL);   // keeps day arithmetic sane
        const int64_t dayA = StoreUtil::LocalDayIndex(a);
        const int64_t dayB = StoreUtil::LocalDayIndex(bClamp);
        // Local days the range covers completely lose every rollup, and so do partly covered days older than the
        // raw retention (no raw rows are left to tell activity inside the range from activity outside it). On the
        // other (boundary) days only what the purged rows contributed is subtracted, so other items active that
        // day are not re-scored (purging the last 15 minutes must not touch the rest of the day).
        const int64_t rawCutDay = StoreUtil::LocalDayIndex(nowSec - (int64_t)m_rawRetentionDays.load() * 86400);
        const int64_t fullA = (a <= 0 || StoreUtil::LocalDayIndex(a - 1) < dayA || dayA < rawCutDay) ? dayA : dayA + 1;
        const int64_t fullB = (StoreUtil::LocalDayIndex(bClamp + 1) > dayB || dayB < rawCutDay) ? dayB : dayB - 1;
        auto exec = [&](const char* sql)
        {
            char* err = nullptr;
            if (sqlite3_exec(w.db, sql, nullptr, nullptr, &err) != SQLITE_OK)
                WARP_LOG_WARN("store", "purge: %s", err ? err : "error");
            sqlite3_free(err);
        };

        // Entities with raw evidence inside the range.
        exec("CREATE TEMP TABLE IF NOT EXISTS purge_touched (entity_key TEXT PRIMARY KEY) WITHOUT ROWID;");
        exec("DELETE FROM temp.purge_touched;");
        {
            sqlite3_stmt* st = nullptr;
            if (sqlite3_prepare_v2(w.db,
                "INSERT OR IGNORE INTO temp.purge_touched (entity_key) "
                "SELECT entity_key FROM file_activity WHERE entity_key IS NOT NULL AND timestamp BETWEEN ?1 AND ?2 "
                "UNION SELECT entity_key FROM app_launch_activity WHERE entity_key IS NOT NULL AND timestamp BETWEEN ?1 AND ?2 "
                "UNION SELECT entity_key FROM browsing_activity WHERE entity_key IS NOT NULL AND timestamp BETWEEN ?1 AND ?2 "
                "UNION SELECT entity_key FROM app_focus_activity WHERE entity_key IS NOT NULL AND COALESCE(end_ts, timestamp) >= ?1 AND COALESCE(start_ts, timestamp) <= ?2 "
                "UNION SELECT doc_key FROM app_focus_activity WHERE doc_key IS NOT NULL AND doc_key <> '' AND COALESCE(end_ts, timestamp) >= ?1 AND COALESCE(start_ts, timestamp) <= ?2;",
                -1, &st, nullptr) == SQLITE_OK)
            {
                sqlite3_bind_int64(st, 1, a);
                sqlite3_bind_int64(st, 2, b);
                sqlite3_step(st);
                sqlite3_finalize(st);
            }
            else
            {
                WARP_LOG_WARN("store", "purge: %s", sqlite3_errmsg(w.db));
            }
        }
        // Entities to re-score: those, plus every entity with a rollup on a day the range covers completely.
        std::vector<std::pair<std::string, std::string>> touched;
        {
            sqlite3_stmt* st = nullptr;
            if (sqlite3_prepare_v2(w.db,
                "SELECT entity_key, entity_type FROM inference WHERE entity_key IN ("
                "  SELECT entity_key FROM temp.purge_touched UNION SELECT entity_key FROM entity_day WHERE day BETWEEN ?1 AND ?2);",
                -1, &st, nullptr) == SQLITE_OK)
            {
                sqlite3_bind_int64(st, 1, fullA);
                sqlite3_bind_int64(st, 2, fullB);
                while (sqlite3_step(st) == SQLITE_ROW) touched.emplace_back(ColU(st, 0), ColU(st, 1));
                sqlite3_finalize(st);
            }
        }
        std::vector<RollupSummary> before;
        before.reserve(touched.size());
        for (const auto& v : touched) before.push_back(SummarizeRollups(w, v.first, v.second));

        // Threads active inside the range (spans, or snapshots whose window overlaps it) lose that time.
        {
            sqlite3_stmt* st = nullptr;
            if (sqlite3_prepare_v2(w.db,
                "SELECT thread_id FROM thread_span WHERE end_ts >= ?1 AND start_ts <= ?2 "
                "UNION SELECT thread_id FROM context_snapshot WHERE thread_id IS NOT NULL AND ts >= ?1 AND ts - window_secs <= ?2;",
                -1, &st, nullptr) == SQLITE_OK)
            {
                sqlite3_bind_int64(st, 1, a);
                sqlite3_bind_int64(st, 2, b);
                while (sqlite3_step(st) == SQLITE_ROW) threads.push_back(sqlite3_column_int64(st, 0));
                sqlite3_finalize(st);
            }
        }
        auto execInts = [&](const char* sql, std::initializer_list<int64_t> v)
        {
            sqlite3_stmt* st = nullptr;
            if (sqlite3_prepare_v2(w.db, sql, -1, &st, nullptr) != SQLITE_OK)
            {
                WARP_LOG_WARN("store", "purge: %s", sqlite3_errmsg(w.db));
                return;
            }
            int i = 1;
            for (int64_t x : v) sqlite3_bind_int64(st, i++, x);
            sqlite3_step(st);
            sqlite3_finalize(st);
        };
        // The parts of [s, e] outside the range.
        auto outside = [a, b](int64_t s, int64_t e)
        {
            std::vector<std::pair<int64_t, int64_t>> keep;
            if (s < a) keep.emplace_back(s, (std::min)(e, a - 1));
            if (b < INT64_MAX && e > b) keep.emplace_back((std::max)(s, b + 1), e);
            return keep;
        };

        // Rollup corrections for the days that keep their rollups (days the range covers completely are dropped
        // below): what the purged rows contributed comes off; credits that now belong to a kept row instead (its
        // de-duplication predecessor was purged, or it is the kept part of a split focus session) are added.
        struct Delta { double opens = 0, edits = 0, active = 0; int sessions = 0; };
        std::map<std::pair<std::string, int64_t>, Delta> delta;
        auto keptDay = [&](int64_t day) { return day < fullA || day > fullB; };
        std::unordered_set<std::string> keys;                 // the touched (ranked) entities
        for (const auto& v : touched) keys.insert(v.first);
        {
            // Replay each touched entity's raw rows through EntityStore's live crediting rules twice - all rows,
            // and only the rows outside the range - starting two days before it so the de-duplication state is
            // right at its start; the per-day difference is what the purge changes. Applying only that difference
            // keeps everything else the live rollups hold (history moved over by renames, rows past the raw
            // retention).
            const int64_t scanFrom = a - 2 * 86400, scanTo = bClamp + 2 * 86400;
            auto clamp01 = [](double c) { return (std::min)(1.0, (std::max)(0.0, c)); };
            struct Dedup { int64_t all = 0, kept = 0; };   // last credited ms of each replay
            // window: de-duplication window in ms (0 = always credited); track: a credit restarts the window.
            auto credit = [](Dedup& d, int64_t ms, bool inside, int64_t window, bool track, bool& all, bool& kept)
            {
                all = window == 0 || d.all == 0 || ms - d.all >= window;
                if (all && track) d.all = ms;
                kept = false;
                if (inside) return;
                kept = window == 0 || d.kept == 0 || ms - d.kept >= window;
                if (kept && track) d.kept = ms;
            };
            auto addDelta = [&](const std::string& key, int64_t ms, double conf, bool all, bool kept, bool isEdit)
            {
                const double diff = (all ? conf : 0.0) - (kept ? conf : 0.0);
                const int64_t day = StoreUtil::LocalDayIndex(ms / 1000);
                if (diff == 0.0 || !keptDay(day)) return;
                Delta& d = delta[std::make_pair(key, day)];
                (isEdit ? d.edits : d.opens) += diff;
            };

            sqlite3_stmt* st = nullptr;
            if (sqlite3_prepare_v2(w.db,
                "SELECT entity_key, action, COALESCE(event_ms, timestamp * 1000), timestamp, COALESCE(confidence, 1.0), "
                "COALESCE(source, ''), old_path, path FROM file_activity "
                "WHERE entity_key IN (SELECT entity_key FROM temp.purge_touched) AND COALESCE(is_dir, 0) = 0 "
                "AND action IN ('OPEN','DOWNLOAD','MODIFY','CREATE','SAVE','RENAME') AND timestamp BETWEEN ?1 AND ?2 "
                "ORDER BY entity_key, 3, id;", -1, &st, nullptr) == SQLITE_OK)
            {
                sqlite3_bind_int64(st, 1, scanFrom);
                sqlite3_bind_int64(st, 2, scanTo);
                std::string cur;
                Dedup opens, edits;
                while (sqlite3_step(st) == SQLITE_ROW)
                {
                    const std::string key = ColU(st, 0);
                    if (key != cur) { cur = key; opens = Dedup(); edits = Dedup(); }
                    if (!keys.count(key)) continue;
                    const std::string action = ColU(st, 1);
                    const int64_t ms = sqlite3_column_int64(st, 2);
                    const int64_t ts = sqlite3_column_int64(st, 3);
                    const double conf = clamp01(sqlite3_column_double(st, 4));
                    const std::string src = ColU(st, 5);
                    const bool inside = ts >= a && ts <= b;
                    bool all = false, kept = false, isEdit = false;
                    if (action == "OPEN")
                    {
                        const bool explicitSrc = src == "shell" || src == "recent" || src == "mru" || src == "jumplist";
                        credit(opens, ms, inside, explicitSrc ? 0 : 120000, true, all, kept);
                    }
                    else if (action == "DOWNLOAD")
                    {
                        credit(opens, ms, inside, 0, false, all, kept);
                    }
                    else
                    {
                        // A rename is credited only as the atomic save of a safe-save ("~WRL0001.tmp" -> "report.docx").
                        if (action == "RENAME" &&
                            !(EntityStore::IsTransientFileName(ColW(st, 6)) && !EntityStore::IsTransientFileName(ColW(st, 7))))
                            continue;
                        isEdit = true;
                        credit(edits, ms, inside, 60000, true, all, kept);
                    }
                    addDelta(key, ms, conf, all, kept, isEdit);
                }
                sqlite3_finalize(st);
            }
            st = nullptr;
            if (sqlite3_prepare_v2(w.db,
                "SELECT entity_key, ms, ts, c FROM ("
                "  SELECT entity_key, COALESCE(event_ms, timestamp * 1000) AS ms, timestamp AS ts, COALESCE(confidence, 1.0) AS c, id, 0 AS t "
                "  FROM app_launch_activity WHERE entity_key IN (SELECT entity_key FROM temp.purge_touched) AND timestamp BETWEEN ?1 AND ?2 "
                "  UNION ALL SELECT entity_key, COALESCE(event_ms, timestamp * 1000), timestamp, COALESCE(confidence, 1.0), id, 1 "
                "  FROM browsing_activity WHERE entity_key IN (SELECT entity_key FROM temp.purge_touched) AND timestamp BETWEEN ?1 AND ?2 "
                "  AND (COALESCE(url, '') <> '' OR COALESCE(doc_id, '') <> '')) "
                "ORDER BY entity_key, ms, t, id;", -1, &st, nullptr) == SQLITE_OK)
            {
                sqlite3_bind_int64(st, 1, scanFrom);
                sqlite3_bind_int64(st, 2, scanTo);
                std::string cur;
                Dedup visits;
                while (sqlite3_step(st) == SQLITE_ROW)
                {
                    const std::string key = ColU(st, 0);
                    if (key != cur) { cur = key; visits = Dedup(); }
                    if (!keys.count(key)) continue;
                    const int64_t ms = sqlite3_column_int64(st, 1);
                    const int64_t ts = sqlite3_column_int64(st, 2);
                    bool all = false, kept = false;
                    credit(visits, ms, ts >= a && ts <= b, 60000, true, all, kept);    // launches and page visits
                    addDelta(key, ms, clamp01(sqlite3_column_double(st, 3)), all, kept, false);
                }
                sqlite3_finalize(st);
            }
            else
            {
                WARP_LOG_WARN("store", "purge replay: %s", sqlite3_errmsg(w.db));
            }
        }

        n += ExecDelete(w.db, "DELETE FROM file_activity WHERE timestamp BETWEEN ?1 AND ?2;", a, b);
        n += ExecDelete(w.db, "DELETE FROM app_launch_activity WHERE timestamp BETWEEN ?1 AND ?2;", a, b);
        n += ExecDelete(w.db, "DELETE FROM browsing_activity WHERE timestamp BETWEEN ?1 AND ?2;", a, b);
        // Focus sessions overlapping the range keep their parts outside it (seconds scaled to the kept share); what
        // the removed part had credited (to the day the session ended) comes off that day's rollups.
        {
            struct F { int64_t id, s, e; int act, idle; std::string app, doc; };
            std::vector<F> rows;
            sqlite3_stmt* st = nullptr;
            if (sqlite3_prepare_v2(w.db,
                "SELECT id, COALESCE(start_ts, timestamp - COALESCE(duration_secs, 0)), COALESCE(end_ts, timestamp), "
                "COALESCE(active_secs, duration_secs, 0), COALESCE(idle_secs, 0), COALESCE(entity_key, ''), COALESCE(doc_key, '') "
                "FROM app_focus_activity WHERE COALESCE(end_ts, timestamp) >= ?1 AND COALESCE(start_ts, timestamp) <= ?2;",
                -1, &st, nullptr) == SQLITE_OK)
            {
                sqlite3_bind_int64(st, 1, a);
                sqlite3_bind_int64(st, 2, b);
                while (sqlite3_step(st) == SQLITE_ROW)
                    rows.push_back({ sqlite3_column_int64(st, 0), sqlite3_column_int64(st, 1), sqlite3_column_int64(st, 2),
                                     sqlite3_column_int(st, 3), sqlite3_column_int(st, 4), ColU(st, 5), ColU(st, 6) });
                sqlite3_finalize(st);
            }
            for (const F& f : rows)
            {
                const std::vector<std::pair<int64_t, int64_t>> keep = outside(f.s, f.e);
                const double len = (double)(std::max)((int64_t)1, f.e - f.s);
                // EntityStore credited the whole session (when it had 2 active seconds or more) to the day it ended;
                // the kept parts are credited to the days they end on instead.
                auto adjust = [&](int64_t endTs, int act, int sign)
                {
                    if (act < 2) return;
                    const int64_t day = StoreUtil::LocalDayIndex(endTs);
                    if (!keptDay(day)) return;
                    for (const std::string* k : { &f.app, &f.doc })
                    {
                        if (k->empty() || !keys.count(*k)) continue;
                        Delta& d = delta[std::make_pair(*k, day)];
                        d.active += sign * (double)act;
                        d.sessions += sign;
                    }
                };
                adjust(f.e, f.act, 1);
                if (keep.empty())
                {
                    execInts("DELETE FROM app_focus_activity WHERE id = ?1;", { f.id });
                    ++n;
                }
                for (size_t i = 0; i < keep.size(); ++i)
                {
                    const double share = (double)(keep[i].second - keep[i].first) / len;
                    const int act = (int)(f.act * share + 0.5), idle = (int)(f.idle * share + 0.5);
                    adjust(keep[i].second, act, -1);
                    // The second part (the range lies strictly inside the session) is a copy of the row.
                    execInts(i == 0
                        ? "UPDATE app_focus_activity SET timestamp = ?2, duration_secs = ?3, event_ms = ?4, start_ts = ?5, end_ts = ?2, "
                          "active_secs = ?6, idle_secs = ?7 WHERE id = ?1;"
                        : "INSERT INTO app_focus_activity (timestamp, exe_name, exe_path, window_title, duration_secs, source_pid, source_exe, "
                          "foreground_pid, foreground_exe, ms_since_input, parent_pid, parent_exe, created_window_ms, confidence, entity_key, "
                          "event_ms, source, start_ts, end_ts, active_secs, idle_secs, doc_key, doc_display, doc_type, window_class, desktop_id, "
                          "aumid, presence) SELECT ?2, exe_name, exe_path, window_title, ?3, source_pid, source_exe, foreground_pid, foreground_exe, "
                          "ms_since_input, parent_pid, parent_exe, created_window_ms, confidence, entity_key, ?4, source, ?5, ?2, ?6, ?7, doc_key, "
                          "doc_display, doc_type, window_class, desktop_id, aumid, presence FROM app_focus_activity WHERE id = ?1;",
                        { f.id, keep[i].second, keep[i].second - keep[i].first, keep[i].second * 1000, keep[i].first, (int64_t)act, (int64_t)idle });
                }
            }
        }
        // Presence intervals keep their parts outside the range.
        {
            struct P { int64_t id, s, e; };
            std::vector<P> rows;
            sqlite3_stmt* st = nullptr;
            if (sqlite3_prepare_v2(w.db, "SELECT id, start_ts, end_ts FROM presence_interval WHERE end_ts >= ?1 AND start_ts <= ?2;",
                                   -1, &st, nullptr) == SQLITE_OK)
            {
                sqlite3_bind_int64(st, 1, a);
                sqlite3_bind_int64(st, 2, b);
                while (sqlite3_step(st) == SQLITE_ROW)
                    rows.push_back({ sqlite3_column_int64(st, 0), sqlite3_column_int64(st, 1), sqlite3_column_int64(st, 2) });
                sqlite3_finalize(st);
            }
            for (const P& p : rows)
            {
                const std::vector<std::pair<int64_t, int64_t>> keep = outside(p.s, p.e);
                if (keep.empty())
                {
                    execInts("DELETE FROM presence_interval WHERE id = ?1;", { p.id });
                    ++n;
                    continue;
                }
                if (keep.size() > 1)
                    execInts("INSERT INTO presence_interval (start_ts, end_ts, state) SELECT ?2, ?3, state FROM presence_interval WHERE id = ?1;",
                             { p.id, keep[1].first, keep[1].second });
                execInts("UPDATE presence_interval SET start_ts = ?2, end_ts = ?3 WHERE id = ?1;", { p.id, keep[0].first, keep[0].second });
            }
        }
        // A snapshot summarizes the window before it: every window that overlaps the range describes purged activity.
        n += ExecDelete(w.db, "DELETE FROM context_snapshot WHERE ts >= ?1 AND ts - window_secs <= ?2;", a, b);
        n += ExecDelete(w.db, "DELETE FROM clipboard_activity WHERE timestamp BETWEEN ?1 AND ?2;", a, b);
        n += ExecDelete(w.db, "DELETE FROM print_activity WHERE timestamp BETWEEN ?1 AND ?2;", a, b);
        // Thread spans keep their parts outside the range; their threads lose the active time of the removed part.
        {
            struct S { int64_t thread, s, e; double act; };
            std::vector<S> rows;
            sqlite3_stmt* st = nullptr;
            if (sqlite3_prepare_v2(w.db, "SELECT thread_id, start_ts, end_ts, active_secs FROM thread_span WHERE end_ts >= ?1 AND start_ts <= ?2;",
                                   -1, &st, nullptr) == SQLITE_OK)
            {
                sqlite3_bind_int64(st, 1, a);
                sqlite3_bind_int64(st, 2, b);
                while (sqlite3_step(st) == SQLITE_ROW)
                    rows.push_back({ sqlite3_column_int64(st, 0), sqlite3_column_int64(st, 1), sqlite3_column_int64(st, 2),
                                     sqlite3_column_double(st, 3) });
                sqlite3_finalize(st);
            }
            std::map<int64_t, double> lost;
            for (const S& sp : rows)
            {
                const std::vector<std::pair<int64_t, int64_t>> keep = outside(sp.s, sp.e);
                const double len = (double)(std::max)((int64_t)1, sp.e - sp.s);
                double kept = 0.0;
                execInts("DELETE FROM thread_span WHERE thread_id = ?1 AND start_ts = ?2;", { sp.thread, sp.s });
                if (keep.empty()) ++n;
                for (const auto& k : keep)
                {
                    const double act = sp.act * (double)(k.second - k.first) / len;
                    kept += act;
                    sqlite3_stmt* ins = nullptr;
                    if (sqlite3_prepare_v2(w.db, "INSERT OR REPLACE INTO thread_span (thread_id, start_ts, end_ts, active_secs) VALUES (?1, ?2, ?3, ?4);",
                                           -1, &ins, nullptr) != SQLITE_OK)
                        continue;
                    sqlite3_bind_int64(ins, 1, sp.thread);
                    sqlite3_bind_int64(ins, 2, k.first);
                    sqlite3_bind_int64(ins, 3, k.second);
                    sqlite3_bind_double(ins, 4, act);
                    sqlite3_step(ins);
                    sqlite3_finalize(ins);
                }
                lost[sp.thread] += sp.act - kept;
            }
            for (const auto& kv : lost)
            {
                sqlite3_stmt* up = nullptr;
                if (sqlite3_prepare_v2(w.db, "UPDATE work_thread SET active_secs = MAX(0, active_secs - ?2) WHERE id = ?1;", -1, &up, nullptr) != SQLITE_OK)
                    continue;
                sqlite3_bind_int64(up, 1, kv.first);
                sqlite3_bind_double(up, 2, kv.second);
                sqlite3_step(up);
                sqlite3_finalize(up);
            }
        }
        n += ExecDelete(w.db, "DELETE FROM edge WHERE last_ts BETWEEN ?1 AND ?2;", a, b);   // co-use last seen inside the range
        // Thread extents that began or ended inside the range fall back to the spans that remain.
        ExecDelete(w.db, "UPDATE work_thread SET "
                         "first_ts = CASE WHEN first_ts BETWEEN ?1 AND ?2 THEN COALESCE((SELECT MIN(s.start_ts) FROM thread_span s WHERE s.thread_id = work_thread.id), first_ts) ELSE first_ts END, "
                         "last_ts = CASE WHEN last_ts BETWEEN ?1 AND ?2 THEN COALESCE((SELECT MAX(s.end_ts) FROM thread_span s WHERE s.thread_id = work_thread.id), last_ts) ELSE last_ts END "
                         "WHERE first_ts BETWEEN ?1 AND ?2 OR last_ts BETWEEN ?1 AND ?2;", a, b);
        // Work sessions keep only their parts outside the range (their seconds scale with the kept share).
        {
            struct Sess { int64_t id, s, e; double act, pas; };
            std::vector<Sess> sessions;
            sqlite3_stmt* st = nullptr;
            if (sqlite3_prepare_v2(w.db, "SELECT id, start_ts, end_ts, active_secs, passive_secs FROM work_session WHERE end_ts >= ?1 AND start_ts <= ?2;",
                                   -1, &st, nullptr) == SQLITE_OK)
            {
                sqlite3_bind_int64(st, 1, a);
                sqlite3_bind_int64(st, 2, b);
                while (sqlite3_step(st) == SQLITE_ROW)
                    sessions.push_back({ sqlite3_column_int64(st, 0), sqlite3_column_int64(st, 1), sqlite3_column_int64(st, 2),
                                         sqlite3_column_double(st, 3), sqlite3_column_double(st, 4) });
                sqlite3_finalize(st);
            }
            for (const Sess& x : sessions)
            {
                std::vector<std::pair<int64_t, int64_t>> keep;
                if (x.s < a) keep.emplace_back(x.s, a - 1);
                if (b < INT64_MAX && x.e > b) keep.emplace_back(b + 1, x.e);
                ++n;
                if (keep.empty())
                {
                    ExecDelete(w.db, "DELETE FROM work_session WHERE id = ?1;", x.id);
                    continue;
                }
                const double len = (double)(std::max)((int64_t)1, x.e - x.s);
                for (size_t i = 0; i < keep.size(); ++i)
                {
                    sqlite3_stmt* up = nullptr;
                    const char* sql = i == 0 ? "UPDATE work_session SET start_ts = ?2, end_ts = ?3, active_secs = ?4, passive_secs = ?5 WHERE id = ?1;"
                                             : "INSERT INTO work_session (start_ts, end_ts, active_secs, passive_secs) VALUES (?2, ?3, ?4, ?5);";
                    if (sqlite3_prepare_v2(w.db, sql, -1, &up, nullptr) != SQLITE_OK) continue;
                    const double share = (double)(keep[i].second - keep[i].first) / len;
                    if (i == 0) sqlite3_bind_int64(up, 1, x.id);
                    sqlite3_bind_int64(up, 2, keep[i].first);
                    sqlite3_bind_int64(up, 3, keep[i].second);
                    sqlite3_bind_int64(up, 4, (int64_t)(x.act * share));
                    sqlite3_bind_int64(up, 5, (int64_t)(x.pas * share));
                    sqlite3_step(up);
                    sqlite3_finalize(up);
                }
            }
        }

        // Daily rollups: days the range covers completely are dropped; the other days get the corrections above
        // (subtracting what the purged rows contributed, adding credits that moved to kept rows), and a rollup
        // left without any evidence is dropped too.
        if (fullA <= fullB)
            n += ExecDelete(w.db, "DELETE FROM entity_day WHERE day BETWEEN ?1 AND ?2;", fullA, fullB);
        {
            sqlite3_stmt* up = nullptr;
            if (sqlite3_prepare_v2(w.db,
                "INSERT INTO entity_day (entity_key, day, opens, edits, active_secs, sessions) "
                "VALUES (?1, ?2, MAX(0, -?3), MAX(0, -?4), MAX(0, -?5), MAX(0, -?6)) "
                "ON CONFLICT(entity_key, day) DO UPDATE SET opens = MAX(0, opens - ?3), edits = MAX(0, edits - ?4), "
                "active_secs = MAX(0, active_secs - ?5), sessions = MAX(0, sessions - ?6);", -1, &up, nullptr) == SQLITE_OK)
            {
                for (const auto& kv : delta)
                {
                    const Delta& d = kv.second;
                    if (d.opens == 0.0 && d.edits == 0.0 && d.active == 0.0 && d.sessions == 0) continue;
                    sqlite3_reset(up);
                    BindText(up, 1, kv.first.first);
                    sqlite3_bind_int64(up, 2, kv.first.second);
                    sqlite3_bind_double(up, 3, d.opens);
                    sqlite3_bind_double(up, 4, d.edits);
                    sqlite3_bind_double(up, 5, d.active);
                    sqlite3_bind_int(up, 6, d.sessions);
                    sqlite3_step(up);
                }
                sqlite3_finalize(up);
            }
            else
            {
                WARP_LOG_WARN("store", "rollup correction prepare failed: %s", sqlite3_errmsg(w.db));
            }
            n += ExecDelete(w.db, "DELETE FROM entity_day WHERE opens < 1e-9 AND edits < 1e-9 AND active_secs < 1e-9 "
                                  "AND entity_key IN (SELECT entity_key FROM temp.purge_touched);", 0);
        }

        // Entities that only existed inside the range disappear; the others are scaled down to what remains.
        for (size_t i = 0; i < touched.size(); ++i)
        {
            const auto& v = touched[i];
            if (RecomputeEntityAfterPurge(w, v.first, v.second, a, b, before[i]))
                continue;
            collectThreads("SELECT thread_id FROM thread_member WHERE entity_key = ?1;", v.first);
            bindRun("DELETE FROM edge WHERE a = ?1 OR b = ?1;", v.first);
            bindRun("DELETE FROM thread_member WHERE entity_key = ?1;", v.first);
            bindRun("DELETE FROM app_focus_activity WHERE doc_key = ?1;", v.first);
            TombstoneEntity(w, m_sinks, v.first, v.second, "purge");
            ++n;
        }
        exec("DELETE FROM temp.purge_touched;");
        // Records reported later about the purged time (polls, held events, intervals still open) are not stored.
        if (a <= nowSec)
            AddFloor(w, PurgeFloor{ 0, std::string(), a * 1000, b >= nowSec ? w.nowMs - 1 : b * 1000 + 999 });
    }

    n += ScrubThreadsAfterPurge(w, std::move(threads));
    // Purges change derived tables underneath the sinks' write-through caches (thread members, entity
    // rows): drop the caches so nothing purged is written back from memory.
    for (IWriterSink* s : m_sinks) s->OnRollback(w);
    return n;
}

ActivityStore::RollupSummary ActivityStore::SummarizeRollups(WriterContext& w, const std::string& key, const std::string& type)
{
    RollupSummary r;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(w.db, "SELECT day, opens, edits, active_secs FROM entity_day WHERE entity_key = ?1;", -1, &st, nullptr) != SQLITE_OK)
    {
        r.days = -1;
        return r;
    }
    BindText(st, 1, key);
    // Day weighting of the v2 migration seed, with the open weight of the entity's kind (EntityStore credits
    // launches as explicit opens and page loads as visits).
    const double openW = type == "app" ? StoreUtil::kWOpenExplicit : (type == "url" ? StoreUtil::kWVisit : StoreUtil::kWOpenPassive);
    const double now = (double)(w.nowMs / 1000);
    while (sqlite3_step(st) == SQLITE_ROW)
    {
        const int64_t day = sqlite3_column_int64(st, 0);
        const double o = sqlite3_column_double(st, 1), e = sqlite3_column_double(st, 2), act = sqlite3_column_double(st, 3);
        r.opens += o;
        r.edits += e;
        r.active += act;
        ++r.days;
        r.lastDay = (std::max)(r.lastDay, day);
        const double wgt = openW * o + StoreUtil::kWEdit * e + StoreUtil::kWActivePerMin * (act / 60.0) + StoreUtil::kWNewDay;
        const double age = (std::max)(0.0, now - ((double)day * 86400.0 + 43200.0));
        r.s += wgt * std::exp(-age / StoreUtil::kTauShortSec);
        r.l += wgt * std::exp(-age / StoreUtil::kTauLongSec);
    }
    sqlite3_finalize(st);
    return r;
}

// Scales a purged-range entity down to the evidence that remains. Totals lose exactly what its rollups lost;
// the live engagement is multiplied by the share of rollup-derived engagement that remains, which keeps
// EntityStore's live calibration (explicit vs passive opens, de-duplication, caps). Timestamps inside the
// range fall back to the latest remaining raw evidence. Returns false when nothing remains (the caller
// tombstones the entity).
bool ActivityStore::RecomputeEntityAfterPurge(WriterContext& w, const std::string& key, const std::string& type, int64_t a, int64_t b,
                                              const RollupSummary& before)
{
    const RollupSummary after = SummarizeRollups(w, key, type);
    if (after.days < 0 || before.days < 0) return true;    // cannot tell: keep the entity untouched
    if (after.days == 0) return false;

    auto maxTs = [&](const char* sql) -> int64_t
    {
        int64_t v = 0;
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(w.db, sql, -1, &st, nullptr) == SQLITE_OK)
        {
            BindText(st, 1, key);
            if (sqlite3_step(st) == SQLITE_ROW && sqlite3_column_type(st, 0) != SQLITE_NULL) v = sqlite3_column_int64(st, 0);
            sqlite3_finalize(st);
        }
        return v;
    };
    int64_t rawOpen = 0, rawEdit = 0, rawActive = 0;
    if (type == "file")
    {
        rawOpen = maxTs("SELECT MAX(timestamp) FROM file_activity WHERE entity_key = ?1 AND action IN ('OPEN','DOWNLOAD');");
        rawEdit = maxTs("SELECT MAX(timestamp) FROM file_activity WHERE entity_key = ?1 AND action IN ('MODIFY','CREATE','SAVE');");
        rawActive = maxTs("SELECT MAX(COALESCE(end_ts, timestamp)) FROM app_focus_activity WHERE doc_key = ?1;");
    }
    else if (type == "app")
    {
        rawOpen = maxTs("SELECT MAX(timestamp) FROM app_launch_activity WHERE entity_key = ?1;");
        rawActive = maxTs("SELECT MAX(COALESCE(end_ts, timestamp)) FROM app_focus_activity WHERE entity_key = ?1;");
    }
    else
    {
        rawOpen = maxTs("SELECT MAX(timestamp) FROM browsing_activity WHERE entity_key = ?1;");
        rawActive = maxTs("SELECT MAX(COALESCE(end_ts, timestamp)) FROM app_focus_activity WHERE doc_key = ?1;");
    }

    sqlite3_stmt* cur = nullptr;
    if (sqlite3_prepare_v2(w.db, "SELECT COALESCE(last_open_ts, 0), COALESCE(last_edit_ts, 0), COALESCE(last_active_ts, 0), COALESCE(last_event_ts, 0), "
                                 "COALESCE(open_count_total, 0), COALESCE(edit_count_total, 0), COALESCE(active_secs_total, 0), COALESCE(distinct_days, 0), "
                                 "COALESCE(eng_s, 0), COALESCE(eng_s_ts, 0), COALESCE(eng_l, 0), COALESCE(eng_l_ts, 0) "
                                 "FROM inference WHERE entity_key = ?1;", -1, &cur, nullptr) != SQLITE_OK)
        return true;
    BindText(cur, 1, key);
    if (sqlite3_step(cur) != SQLITE_ROW) { sqlite3_finalize(cur); return true; }
    int64_t lastOpen = sqlite3_column_int64(cur, 0), lastEdit = sqlite3_column_int64(cur, 1);
    int64_t lastActive = sqlite3_column_int64(cur, 2), lastEvent = sqlite3_column_int64(cur, 3);
    const double openTotal = sqlite3_column_double(cur, 4), editTotal = sqlite3_column_double(cur, 5);
    const double activeTotal = sqlite3_column_double(cur, 6);
    const int64_t distinctDays = sqlite3_column_int64(cur, 7);
    const double engS = sqlite3_column_double(cur, 8), engL = sqlite3_column_double(cur, 10);
    const int64_t engSTs = sqlite3_column_int64(cur, 9), engLTs = sqlite3_column_int64(cur, 11);
    sqlite3_finalize(cur);

    // A timestamp inside the purged range is replaced by the latest remaining evidence of its kind.
    auto inRange = [a, b](int64_t t) { return t >= a && t <= b; };
    if (inRange(lastOpen)) lastOpen = rawOpen;
    if (inRange(lastEdit)) lastEdit = rawEdit;
    if (inRange(lastActive)) lastActive = rawActive;
    if (inRange(lastEvent))
    {
        lastEvent = (std::max)((std::max)(lastOpen, lastEdit), lastActive);
        if (lastEvent == 0) lastEvent = after.lastDay * 86400 + 43200;   // only rollups older than the raw retention remain
    }

    const int64_t now = w.nowMs / 1000;
    auto decayedNow = [now](double v, int64_t t, double tau) { return v * std::exp(-(double)(std::max)((int64_t)0, now - t) / tau); };
    auto kept = [](double remaining, double was) { return was > 0.0 ? (std::min)(1.0, (std::max)(0.0, remaining / was)) : 1.0; };
    const double s = decayedNow(engS, engSTs, StoreUtil::kTauShortSec) * kept(after.s, before.s);
    const double l = decayedNow(engL, engLTs, StoreUtil::kTauLongSec) * kept(after.l, before.l);
    const double opens = (std::max)(0.0, openTotal - (std::max)(0.0, before.opens - after.opens));
    const double edits = (std::max)(0.0, editTotal - (std::max)(0.0, before.edits - after.edits));
    const double active = (std::max)(0.0, activeTotal - (std::max)(0.0, before.active - after.active));
    const int64_t days = (std::max)((int64_t)after.days, distinctDays - (int64_t)(std::max)(0, before.days - after.days));

    sqlite3_stmt* up = nullptr;
    if (sqlite3_prepare_v2(w.db,
        "UPDATE inference SET last_open_ts = ?2, last_edit_ts = ?3, last_active_ts = ?4, last_event_ts = ?5, "
        "open_count_total = ?6, edit_count_total = ?7, active_secs_total = ?8, distinct_days = ?9, last_day = ?10, "
        "eng_s = ?11, eng_s_ts = ?12, eng_s_key = ?13, eng_l = ?14, eng_l_ts = ?12, eng_l_key = ?15, "
        "change_seq = ?16, version = ?16, updated_at = ?12 WHERE entity_key = ?1;", -1, &up, nullptr) != SQLITE_OK)
        return true;
    BindText(up, 1, key);
    sqlite3_bind_int64(up, 2, lastOpen);
    sqlite3_bind_int64(up, 3, lastEdit);
    sqlite3_bind_int64(up, 4, lastActive);
    sqlite3_bind_int64(up, 5, lastEvent);
    sqlite3_bind_double(up, 6, opens);
    sqlite3_bind_double(up, 7, edits);
    sqlite3_bind_double(up, 8, active);
    sqlite3_bind_int64(up, 9, days);
    sqlite3_bind_int64(up, 10, after.lastDay);
    sqlite3_bind_double(up, 11, s);
    sqlite3_bind_int64(up, 12, now);
    sqlite3_bind_double(up, 13, StoreUtil::DecayKey(s, now, StoreUtil::kTauShortSec));
    sqlite3_bind_double(up, 14, l);
    sqlite3_bind_double(up, 15, StoreUtil::DecayKey(l, now, StoreUtil::kTauLongSec));
    sqlite3_bind_int64(up, 16, w.NextChangeSeq());
    sqlite3_step(up);
    sqlite3_finalize(up);
    return true;
}

// Work threads that held purged items or purged time. A label is a summary line and the centroid an
// embedding of such lines, so either may describe the purged activity: the label is rebuilt from the
// strongest remaining members and the centroid cleared (the next snapshot re-seeds it). Threads left
// without members or spans are deleted.
int64_t ActivityStore::ScrubThreadsAfterPurge(WriterContext& w, std::vector<int64_t> ids)
{
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    int64_t removed = 0;
    for (int64_t id : ids)
    {
        if (id <= 0) continue;
        int spans = 0;
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(w.db, "SELECT COUNT(*) FROM thread_span WHERE thread_id = ?1;", -1, &st, nullptr) != SQLITE_OK) continue;
        sqlite3_bind_int64(st, 1, id);
        if (sqlite3_step(st) == SQLITE_ROW) spans = sqlite3_column_int(st, 0);
        sqlite3_finalize(st);

        std::string label;
        int members = 0;
        if (sqlite3_prepare_v2(w.db, "SELECT COUNT(*) FROM thread_member WHERE thread_id = ?1;", -1, &st, nullptr) != SQLITE_OK) continue;
        sqlite3_bind_int64(st, 1, id);
        if (sqlite3_step(st) == SQLITE_ROW) members = sqlite3_column_int(st, 0);
        sqlite3_finalize(st);
        // The label is rebuilt from the strongest members that are still ranked items (none: no label).
        if (sqlite3_prepare_v2(w.db, "SELECT COALESCE(NULLIF(i.title, ''), NULLIF(i.display_name, ''), m.entity_key) FROM thread_member m "
                                     "JOIN inference i ON i.entity_key = m.entity_key AND i.deleted = 0 "
                                     "WHERE m.thread_id = ?1 ORDER BY m.weight DESC LIMIT 2;", -1, &st, nullptr) != SQLITE_OK)
            continue;
        sqlite3_bind_int64(st, 1, id);
        while (sqlite3_step(st) == SQLITE_ROW)
        {
            std::string name = ColU(st, 0);
            const size_t slash = name.find_last_of("\\/");
            if (name.find("://") == std::string::npos && slash != std::string::npos && slash + 1 < name.size())
                name.erase(0, slash + 1);                  // a path: its file name
            if (name.empty()) continue;
            if (!label.empty()) label += ", ";
            label += name;
        }
        sqlite3_finalize(st);

        if (spans == 0 || members == 0)
        {
            ExecDelete(w.db, "DELETE FROM thread_member WHERE thread_id = ?1;", id);
            ExecDelete(w.db, "DELETE FROM thread_span WHERE thread_id = ?1;", id);
            ExecDelete(w.db, "UPDATE context_snapshot SET thread_id = NULL WHERE thread_id = ?1;", id);
            ExecDelete(w.db, "DELETE FROM work_thread WHERE id = ?1;", id);
            ++removed;
            continue;
        }
        if (sqlite3_prepare_v2(w.db, "UPDATE work_thread SET label = ?2, centroid = NULL, change_seq = ?3 WHERE id = ?1;", -1, &st, nullptr) == SQLITE_OK)
        {
            sqlite3_bind_int64(st, 1, id);
            BindText(st, 2, label);
            sqlite3_bind_int64(st, 3, w.NextChangeSeq());
            sqlite3_step(st);
            sqlite3_finalize(st);
        }
    }
    return removed;
}

bool ActivityStore::Purge(const PurgeSpec& spec, int64_t& rowsDeleted, DWORD timeoutMs)
{
    rowsDeleted = 0;
    auto counter = std::make_shared<int64_t>(0);
    PurgeSpec copy = spec;
    bool ok = RunSync([this, copy, counter](WriterContext& w)
    {
        // A failed job rolls back to its savepoint: its purge floors must go with it.
        std::vector<PurgeFloor> saved = m_floors;
        const bool dirty = m_floorsDirty;
        try { *counter = DoPurge(w, copy); }
        catch (...) { m_floors = std::move(saved); m_floorsDirty = dirty; throw; }
    }, timeoutMs);
    rowsDeleted = *counter;
    if (ok)
    {
        Job post;
        post.noTxn = true;
        post.fn = [](WriterContext& w)
        {
            Exec(w.db, "PRAGMA incremental_vacuum(4000);");
            Exec(w.db, "PRAGMA wal_checkpoint(TRUNCATE);");
        };
        Submit(std::move(post), timeoutMs);
    }
    return ok;
}

// =====================================================================
// Change feed, meta, stats
// =====================================================================
bool ActivityStore::WaitForChange(int64_t afterSeq, DWORD timeoutMs)
{
    std::unique_lock<std::mutex> lk(m_seqMtx);
    return m_seqCv.wait_for(lk, std::chrono::milliseconds(timeoutMs),
                            [&] { return m_committedSeq.load() > afterSeq || !m_open.load(); })
           && m_committedSeq.load() > afterSeq;
}

bool ActivityStore::GetMeta(const std::string& key, std::string& value)
{
    ReadLease rl = AcquireReader();
    if (!rl) return false;
    sqlite3_stmt* st = nullptr;
    bool found = false;
    if (sqlite3_prepare_v2(rl.get(), "SELECT value FROM meta WHERE key = ?1;", -1, &st, nullptr) == SQLITE_OK)
    {
        BindText(st, 1, key);
        if (sqlite3_step(st) == SQLITE_ROW)
        {
            value = ColU(st, 0);
            found = true;
        }
        sqlite3_finalize(st);
    }
    return found;
}

void ActivityStore::SetMeta(const std::string& key, const std::string& value)
{
    Enqueue([key, value](WriterContext& w)
    {
        sqlite3_stmt* st = w.Stmt("INSERT OR REPLACE INTO meta (key, value) VALUES (?1, ?2);");
        if (!st) return;
        BindText(st, 1, key);
        BindText(st, 2, value);
        sqlite3_step(st);
    }, true);
}

ActivityStore::Stats ActivityStore::GetStats() const
{
    Stats s;
    {
        std::lock_guard<std::mutex> lk(m_statsMtx);
        s = m_stats;
    }
    {
        std::lock_guard<std::mutex> lk(m_qMtx);
        s.queueDepth = m_queue.size();
    }
    s.dbBytes = FileSize(m_path);
    s.walBytes = FileSize(m_path + L"-wal");
    s.changeSeq = m_committedSeq.load();
    s.resetSeq = m_resetSeq.load();
    s.rawRetentionDays = m_rawRetentionDays.load();
    s.entityRetentionDays = m_entityRetentionDays.load();
    return s;
}

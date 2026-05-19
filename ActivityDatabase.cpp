#include "framework.h"
#include "ActivityDatabase.h"
#include "sqlite3.h"
#include <ctime>
#include <sstream>

ActivityDatabase::ActivityDatabase() {}

ActivityDatabase::~ActivityDatabase() { Close(); }

std::string ActivityDatabase::WideToUtf8(const std::wstring& w)
{
    if (w.empty()) return "";
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], len, nullptr, nullptr);
    return s;
}

std::wstring ActivityDatabase::Utf8ToWide(const char* s)
{
    if (!s || !*s) return L"";
    int wlen = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    std::wstring w(wlen - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, -1, &w[0], wlen);
    return w;
}

bool ActivityDatabase::Open(const std::wstring& dbPath)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    std::string utf8 = WideToUtf8(dbPath);
    if (sqlite3_open(utf8.c_str(), &m_db) != SQLITE_OK)
        return false;

    Execute("PRAGMA journal_mode=WAL;");
    Execute("PRAGMA synchronous=NORMAL;");

    // File activity table (existing)
    const char* createFileTable =
        "CREATE TABLE IF NOT EXISTS file_activity ("
        "  id          INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  timestamp   INTEGER NOT NULL,"
        "  action      TEXT    NOT NULL,"
        "  path        TEXT    NOT NULL,"
        "  old_path    TEXT"
        ");";
    if (!Execute(createFileTable))
        return false;

    Execute("CREATE INDEX IF NOT EXISTS idx_activity_ts ON file_activity(timestamp);");
    Execute("CREATE INDEX IF NOT EXISTS idx_activity_action ON file_activity(action, timestamp);");

    // App launch activity table
    const char* createAppTable =
        "CREATE TABLE IF NOT EXISTS app_launch_activity ("
        "  id          INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  timestamp   INTEGER NOT NULL,"
        "  exe_name    TEXT    NOT NULL,"
        "  exe_path    TEXT    NOT NULL,"
        "  pid         INTEGER NOT NULL"
        ");";
    if (!Execute(createAppTable))
        return false;

    Execute("CREATE INDEX IF NOT EXISTS idx_app_ts ON app_launch_activity(timestamp);");

    // Browsing activity table
    const char* createBrowseTable =
        "CREATE TABLE IF NOT EXISTS browsing_activity ("
        "  id          INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  timestamp   INTEGER NOT NULL,"
        "  browser     TEXT    NOT NULL,"
        "  title       TEXT    NOT NULL,"
        "  url         TEXT"
        ");";
    if (!Execute(createBrowseTable))
        return false;

    Execute("CREATE INDEX IF NOT EXISTS idx_browse_ts ON browsing_activity(timestamp);");

    // Inference table (precomputed scores, updated incrementally per event)
    const char* createInferenceTable =
        "CREATE TABLE IF NOT EXISTS inference ("
        "  entity_key        TEXT PRIMARY KEY,"
        "  entity_type       TEXT,"
        "  last_event_ts     INTEGER,"
        "  last_open_ts      INTEGER,"
        "  last_edit_ts      INTEGER,"
        "  open_count_7d     INTEGER,"
        "  open_count_30d    INTEGER,"
        "  open_count_total  INTEGER,"
        "  recency_score     REAL,"
        "  version           INTEGER,"
        "  updated_at        INTEGER"
        ");";
    Execute(createInferenceTable);
    Execute("CREATE INDEX IF NOT EXISTS idx_inference_updated_at ON inference(updated_at);");
    Execute("CREATE INDEX IF NOT EXISTS idx_inference_version ON inference(version);");

    // App focus activity table (foreground window tracking with dwell time)
    const char* createFocusTable =
        "CREATE TABLE IF NOT EXISTS app_focus_activity ("
        "  id              INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  timestamp       INTEGER NOT NULL,"
        "  exe_name        TEXT    NOT NULL,"
        "  exe_path        TEXT    NOT NULL,"
        "  window_title    TEXT    NOT NULL,"
        "  duration_secs   INTEGER NOT NULL"
        ");";
    Execute(createFocusTable);
    Execute("CREATE INDEX IF NOT EXISTS idx_focus_ts ON app_focus_activity(timestamp);");

    // -- EventContext schema migration ---------------------------------------
    // Each activity table gains a uniform set of context columns describing
    // the source/foreground/parent process and the producer's intent
    // confidence. SQLite has no "ADD COLUMN IF NOT EXISTS"; we simply run
    // each ALTER and rely on Execute() returning false (and being ignored)
    // when the column already exists. This keeps Open() idempotent across
    // upgrades from older schema versions.
    static const char* const kContextCols[] = {
        "source_pid       INTEGER",
        "source_exe       TEXT",
        "foreground_pid   INTEGER",
        "foreground_exe   TEXT",
        "ms_since_input   INTEGER",
        "parent_pid       INTEGER",
        "parent_exe       TEXT",
        "created_window_ms INTEGER",
        "confidence       REAL DEFAULT 1.0",
    };
    static const char* const kActivityTables[] = {
        "file_activity",
        "app_launch_activity",
        "browsing_activity",
        "app_focus_activity",
    };
    for (const char* tbl : kActivityTables)
    {
        for (const char* coldef : kContextCols)
        {
            std::string sql = std::string("ALTER TABLE ") + tbl +
                              " ADD COLUMN " + coldef + ";";
            // Ignore errors for already-existing columns; sqlite_exec sets a
            // duplicate-column error code we don't care about.
            char* err = nullptr;
            sqlite3_exec(m_db, sql.c_str(), nullptr, nullptr, &err);
            if (err) sqlite3_free(err);
        }
    }
    Execute("CREATE INDEX IF NOT EXISTS idx_file_src_pid ON file_activity(source_pid);");
    Execute("CREATE INDEX IF NOT EXISTS idx_file_conf ON file_activity(confidence);");

    return true;
}

void ActivityDatabase::Close()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_db)
    {
        sqlite3_close(m_db);
        m_db = nullptr;
    }
}

// ===================== File activity =====================

namespace
{
    // Bind the 9 EventContext columns starting at parameter index `startIdx`.
    // Caller is responsible for keeping the std::string utf-8 buffers alive
    // across the bind/step calls (we use SQLITE_TRANSIENT for safety).
    void BindEventContext(sqlite3_stmt* stmt, int startIdx, const EventContext& ctx)
    {
        // source_pid
        if (ctx.sourcePid != 0)
            sqlite3_bind_int64(stmt, startIdx + 0, static_cast<sqlite3_int64>(ctx.sourcePid));
        else
            sqlite3_bind_null(stmt, startIdx + 0);

        // source_exe
        if (!ctx.sourceExe.empty())
        {
            int len = WideCharToMultiByte(CP_UTF8, 0, ctx.sourceExe.c_str(), -1, nullptr, 0, nullptr, nullptr);
            std::string s(len > 0 ? len - 1 : 0, '\0');
            WideCharToMultiByte(CP_UTF8, 0, ctx.sourceExe.c_str(), -1, &s[0], len, nullptr, nullptr);
            sqlite3_bind_text(stmt, startIdx + 1, s.c_str(), -1, SQLITE_TRANSIENT);
        }
        else
            sqlite3_bind_null(stmt, startIdx + 1);

        // foreground_pid
        if (ctx.foregroundPid != 0)
            sqlite3_bind_int64(stmt, startIdx + 2, static_cast<sqlite3_int64>(ctx.foregroundPid));
        else
            sqlite3_bind_null(stmt, startIdx + 2);

        // foreground_exe
        if (!ctx.foregroundExe.empty())
        {
            int len = WideCharToMultiByte(CP_UTF8, 0, ctx.foregroundExe.c_str(), -1, nullptr, 0, nullptr, nullptr);
            std::string s(len > 0 ? len - 1 : 0, '\0');
            WideCharToMultiByte(CP_UTF8, 0, ctx.foregroundExe.c_str(), -1, &s[0], len, nullptr, nullptr);
            sqlite3_bind_text(stmt, startIdx + 3, s.c_str(), -1, SQLITE_TRANSIENT);
        }
        else
            sqlite3_bind_null(stmt, startIdx + 3);

        // ms_since_input
        if (ctx.msSinceInput != 0xFFFFFFFFu)
            sqlite3_bind_int64(stmt, startIdx + 4, static_cast<sqlite3_int64>(ctx.msSinceInput));
        else
            sqlite3_bind_null(stmt, startIdx + 4);

        // parent_pid
        if (ctx.parentPid != 0)
            sqlite3_bind_int64(stmt, startIdx + 5, static_cast<sqlite3_int64>(ctx.parentPid));
        else
            sqlite3_bind_null(stmt, startIdx + 5);

        // parent_exe
        if (!ctx.parentExe.empty())
        {
            int len = WideCharToMultiByte(CP_UTF8, 0, ctx.parentExe.c_str(), -1, nullptr, 0, nullptr, nullptr);
            std::string s(len > 0 ? len - 1 : 0, '\0');
            WideCharToMultiByte(CP_UTF8, 0, ctx.parentExe.c_str(), -1, &s[0], len, nullptr, nullptr);
            sqlite3_bind_text(stmt, startIdx + 6, s.c_str(), -1, SQLITE_TRANSIENT);
        }
        else
            sqlite3_bind_null(stmt, startIdx + 6);

        // created_window_ms
        if (ctx.createdWindowMs != 0)
            sqlite3_bind_int64(stmt, startIdx + 7, static_cast<sqlite3_int64>(ctx.createdWindowMs));
        else
            sqlite3_bind_null(stmt, startIdx + 7);

        // confidence
        sqlite3_bind_double(stmt, startIdx + 8, ctx.confidence);
    }

    constexpr const char* kContextColumnList =
        "source_pid, source_exe, foreground_pid, foreground_exe, ms_since_input, "
        "parent_pid, parent_exe, created_window_ms, confidence";
    constexpr const char* kContextValuePlaceholders = "?,?,?,?,?,?,?,?,?";
}

bool ActivityDatabase::InsertActivity(const std::wstring& action,
                                      const std::wstring& path,
                                      const std::wstring& oldPath,
                                      const EventContext& ctx)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_db) return false;

    std::string sql = std::string(
        "INSERT INTO file_activity(timestamp, action, path, old_path, ") +
        kContextColumnList + ") VALUES(?,?,?,?," + kContextValuePlaceholders + ");";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK)
        return false;

    int64_t now = static_cast<int64_t>(std::time(nullptr));
    sqlite3_bind_int64(stmt, 1, now);

    std::string aUtf8 = WideToUtf8(action);
    sqlite3_bind_text(stmt, 2, aUtf8.c_str(), -1, SQLITE_TRANSIENT);

    std::string pUtf8 = WideToUtf8(path);
    sqlite3_bind_text(stmt, 3, pUtf8.c_str(), -1, SQLITE_TRANSIENT);

    if (!oldPath.empty())
    {
        std::string oUtf8 = WideToUtf8(oldPath);
        sqlite3_bind_text(stmt, 4, oUtf8.c_str(), -1, SQLITE_TRANSIENT);
    }
    else
    {
        sqlite3_bind_null(stmt, 4);
    }

    BindEventContext(stmt, 5, ctx);

    bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    return ok;
}

std::vector<FileActivity> ActivityDatabase::QueryFiles(TimeWindow window)
{
    return QueryFilesCustomSeconds(WindowToSeconds(window));
}

std::vector<FileActivity> ActivityDatabase::QueryFilesCustomSeconds(int64_t seconds)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<FileActivity> results;
    if (!m_db) return results;

    int64_t cutoff = static_cast<int64_t>(std::time(nullptr)) - seconds;

    const char* sql = "SELECT id, timestamp, action, path, old_path "
                      "FROM file_activity WHERE timestamp >= ? ORDER BY timestamp DESC;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return results;

    sqlite3_bind_int64(stmt, 1, cutoff);

    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        FileActivity fa;
        fa.id = sqlite3_column_int64(stmt, 0);
        fa.timestampUtc = sqlite3_column_int64(stmt, 1);
        fa.action  = Utf8ToWide(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2)));
        fa.path    = Utf8ToWide(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3)));
        fa.oldPath = Utf8ToWide(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4)));
        results.push_back(std::move(fa));
    }

    sqlite3_finalize(stmt);
    return results;
}

// ===================== App launch activity =====================

bool ActivityDatabase::InsertAppLaunch(const std::wstring& exeName,
                                       const std::wstring& exePath,
                                       DWORD               pid,
                                       const EventContext& ctx)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_db) return false;

    std::string sql = std::string(
        "INSERT INTO app_launch_activity(timestamp, exe_name, exe_path, pid, ") +
        kContextColumnList + ") VALUES(?,?,?,?," + kContextValuePlaceholders + ");";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK)
        return false;

    int64_t now = static_cast<int64_t>(std::time(nullptr));
    sqlite3_bind_int64(stmt, 1, now);

    std::string nUtf8 = WideToUtf8(exeName);
    sqlite3_bind_text(stmt, 2, nUtf8.c_str(), -1, SQLITE_TRANSIENT);

    std::string pUtf8 = WideToUtf8(exePath);
    sqlite3_bind_text(stmt, 3, pUtf8.c_str(), -1, SQLITE_TRANSIENT);

    sqlite3_bind_int(stmt, 4, static_cast<int>(pid));

    BindEventContext(stmt, 5, ctx);

    bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    return ok;
}

std::vector<AppLaunchActivity> ActivityDatabase::QueryAppLaunches(TimeWindow window)
{
    return QueryAppLaunchesCustomSeconds(WindowToSeconds(window));
}

std::vector<AppLaunchActivity> ActivityDatabase::QueryAppLaunchesCustomSeconds(int64_t seconds)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<AppLaunchActivity> results;
    if (!m_db) return results;

    int64_t cutoff = static_cast<int64_t>(std::time(nullptr)) - seconds;

    const char* sql = "SELECT id, timestamp, exe_name, exe_path, pid "
                      "FROM app_launch_activity WHERE timestamp >= ? ORDER BY timestamp DESC;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return results;

    sqlite3_bind_int64(stmt, 1, cutoff);

    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        AppLaunchActivity a;
        a.id = sqlite3_column_int64(stmt, 0);
        a.timestampUtc = sqlite3_column_int64(stmt, 1);
        a.exeName = Utf8ToWide(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2)));
        a.exePath = Utf8ToWide(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3)));
        a.pid = static_cast<DWORD>(sqlite3_column_int(stmt, 4));
        results.push_back(std::move(a));
    }

    sqlite3_finalize(stmt);
    return results;
}

// ===================== Browsing activity =====================

bool ActivityDatabase::InsertBrowsingActivity(const std::wstring& browser,
                                              const std::wstring& title,
                                              const std::wstring& url,
                                              const EventContext& ctx)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_db) return false;

    std::string sql = std::string(
        "INSERT INTO browsing_activity(timestamp, browser, title, url, ") +
        kContextColumnList + ") VALUES(?,?,?,?," + kContextValuePlaceholders + ");";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK)
        return false;

    int64_t now = static_cast<int64_t>(std::time(nullptr));
    sqlite3_bind_int64(stmt, 1, now);

    std::string bUtf8 = WideToUtf8(browser);
    sqlite3_bind_text(stmt, 2, bUtf8.c_str(), -1, SQLITE_TRANSIENT);

    std::string tUtf8 = WideToUtf8(title);
    sqlite3_bind_text(stmt, 3, tUtf8.c_str(), -1, SQLITE_TRANSIENT);

    if (!url.empty())
    {
        std::string uUtf8 = WideToUtf8(url);
        sqlite3_bind_text(stmt, 4, uUtf8.c_str(), -1, SQLITE_TRANSIENT);
    }
    else
    {
        sqlite3_bind_null(stmt, 4);
    }

    BindEventContext(stmt, 5, ctx);

    bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    return ok;
}

std::vector<BrowsingActivity> ActivityDatabase::QueryBrowsing(TimeWindow window)
{
    return QueryBrowsingCustomSeconds(WindowToSeconds(window));
}

std::vector<BrowsingActivity> ActivityDatabase::QueryBrowsingCustomSeconds(int64_t seconds)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<BrowsingActivity> results;
    if (!m_db) return results;

    int64_t cutoff = static_cast<int64_t>(std::time(nullptr)) - seconds;

    const char* sql = "SELECT id, timestamp, browser, title, url "
                      "FROM browsing_activity WHERE timestamp >= ? ORDER BY timestamp DESC;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return results;

    sqlite3_bind_int64(stmt, 1, cutoff);

    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        BrowsingActivity b;
        b.id = sqlite3_column_int64(stmt, 0);
        b.timestampUtc = sqlite3_column_int64(stmt, 1);
        b.browser = Utf8ToWide(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2)));
        b.title   = Utf8ToWide(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3)));
        b.url     = Utf8ToWide(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4)));
        results.push_back(std::move(b));
    }

    sqlite3_finalize(stmt);
    return results;
}

// ===================== App focus activity =====================

bool ActivityDatabase::InsertAppFocusActivity(const std::wstring& exeName,
                                              const std::wstring& exePath,
                                              const std::wstring& windowTitle,
                                              int                 durationSecs,
                                              const EventContext& ctx)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_db) return false;

    std::string sql = std::string(
        "INSERT INTO app_focus_activity(timestamp, exe_name, exe_path, window_title, duration_secs, ") +
        kContextColumnList + ") VALUES(?,?,?,?,?," + kContextValuePlaceholders + ");";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK)
        return false;

    int64_t now = static_cast<int64_t>(std::time(nullptr));
    sqlite3_bind_int64(stmt, 1, now);

    std::string nUtf8 = WideToUtf8(exeName);
    sqlite3_bind_text(stmt, 2, nUtf8.c_str(), -1, SQLITE_TRANSIENT);

    std::string pUtf8 = WideToUtf8(exePath);
    sqlite3_bind_text(stmt, 3, pUtf8.c_str(), -1, SQLITE_TRANSIENT);

    std::string tUtf8 = WideToUtf8(windowTitle);
    sqlite3_bind_text(stmt, 4, tUtf8.c_str(), -1, SQLITE_TRANSIENT);

    sqlite3_bind_int(stmt, 5, durationSecs);

    BindEventContext(stmt, 6, ctx);

    bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    return ok;
}

std::vector<AppFocusActivity> ActivityDatabase::QueryAppFocus(TimeWindow window)
{
    return QueryAppFocusCustomSeconds(WindowToSeconds(window));
}

std::vector<AppFocusActivity> ActivityDatabase::QueryAppFocusCustomSeconds(int64_t seconds)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<AppFocusActivity> results;
    if (!m_db) return results;

    int64_t cutoff = static_cast<int64_t>(std::time(nullptr)) - seconds;

    const char* sql = "SELECT id, timestamp, exe_name, exe_path, window_title, duration_secs "
                      "FROM app_focus_activity WHERE timestamp >= ? ORDER BY timestamp DESC;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return results;

    sqlite3_bind_int64(stmt, 1, cutoff);

    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        AppFocusActivity f;
        f.id           = sqlite3_column_int64(stmt, 0);
        f.timestampUtc = sqlite3_column_int64(stmt, 1);
        f.exeName      = Utf8ToWide(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2)));
        f.exePath      = Utf8ToWide(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3)));
        f.windowTitle  = Utf8ToWide(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4)));
        f.durationSecs = sqlite3_column_int(stmt, 5);
        results.push_back(std::move(f));
    }

    sqlite3_finalize(stmt);
    return results;
}

// ===================== Eviction & utilities =====================

void ActivityDatabase::EvictOlderThan30Days()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_db) return;

    int64_t cutoff = static_cast<int64_t>(std::time(nullptr)) - 30LL * 24 * 3600;

    const char* tables[] = {
        "DELETE FROM file_activity WHERE timestamp < ?;",
        "DELETE FROM app_launch_activity WHERE timestamp < ?;",
        "DELETE FROM browsing_activity WHERE timestamp < ?;",
        "DELETE FROM app_focus_activity WHERE timestamp < ?;",
    };

    for (const char* sql : tables)
    {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) == SQLITE_OK)
        {
            sqlite3_bind_int64(stmt, 1, cutoff);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
    }
}

void ActivityDatabase::ClearAll()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_db) return;
    Execute("DELETE FROM file_activity;");
    Execute("DELETE FROM app_launch_activity;");
    Execute("DELETE FROM browsing_activity;");
    Execute("DELETE FROM app_focus_activity;");
    Execute("DELETE FROM inference;");
}

bool ActivityDatabase::Execute(const char* sql)
{
    if (!m_db) return false;
    char* err = nullptr;
    int rc = sqlite3_exec(m_db, sql, nullptr, nullptr, &err);
    if (err) sqlite3_free(err);
    return rc == SQLITE_OK;
}

int64_t ActivityDatabase::WindowToSeconds(TimeWindow w)
{
    switch (w)
    {
    case TimeWindow::Minutes15: return 15LL * 60;
    case TimeWindow::Minutes30: return 30LL * 60;
    case TimeWindow::Hours1:    return 3600LL;
    case TimeWindow::Hours2:    return 2LL * 3600;
    case TimeWindow::Hours6:    return 6LL * 3600;
    case TimeWindow::Hours24:   return 24LL * 3600;
    case TimeWindow::Days7:     return 7LL * 24 * 3600;
    case TimeWindow::Days15:    return 15LL * 24 * 3600;
    case TimeWindow::Days30:    return 30LL * 24 * 3600;
    default:                    return 3600LL;
    }
}

#include "framework.h"
#include "ActivityDatabase.h"
#include "sqlite3.h"
#include <ctime>
#include <sstream>

ActivityDatabase::ActivityDatabase() {}

ActivityDatabase::~ActivityDatabase() { Close(); }

bool ActivityDatabase::Open(const std::wstring& dbPath)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    // Convert wide path to UTF-8 for sqlite3_open
    int len = WideCharToMultiByte(CP_UTF8, 0, dbPath.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string utf8(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, dbPath.c_str(), -1, &utf8[0], len, nullptr, nullptr);

    if (sqlite3_open(utf8.c_str(), &m_db) != SQLITE_OK)
        return false;

    // WAL mode for better concurrency
    Execute("PRAGMA journal_mode=WAL;");
    Execute("PRAGMA synchronous=NORMAL;");

    const char* createTable =
        "CREATE TABLE IF NOT EXISTS file_activity ("
        "  id          INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  timestamp   INTEGER NOT NULL,"
        "  action      TEXT    NOT NULL,"
        "  path        TEXT    NOT NULL,"
        "  old_path    TEXT"
        ");";
    if (!Execute(createTable))
        return false;

    // Index on timestamp for efficient time-range queries
    Execute("CREATE INDEX IF NOT EXISTS idx_activity_ts ON file_activity(timestamp);");
    // Index on action for filtered queries
    Execute("CREATE INDEX IF NOT EXISTS idx_activity_action ON file_activity(action, timestamp);");

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

bool ActivityDatabase::InsertActivity(const std::wstring& action,
                                      const std::wstring& path,
                                      const std::wstring& oldPath)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_db) return false;

    const char* sql = "INSERT INTO file_activity(timestamp, action, path, old_path) VALUES(?,?,?,?);";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return false;

    int64_t now = static_cast<int64_t>(std::time(nullptr));
    sqlite3_bind_int64(stmt, 1, now);

    // action
    int alen = WideCharToMultiByte(CP_UTF8, 0, action.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string aUtf8(alen, '\0');
    WideCharToMultiByte(CP_UTF8, 0, action.c_str(), -1, &aUtf8[0], alen, nullptr, nullptr);
    sqlite3_bind_text(stmt, 2, aUtf8.c_str(), -1, SQLITE_TRANSIENT);

    // path
    int plen = WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string pUtf8(plen, '\0');
    WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, &pUtf8[0], plen, nullptr, nullptr);
    sqlite3_bind_text(stmt, 3, pUtf8.c_str(), -1, SQLITE_TRANSIENT);

    // old_path
    if (!oldPath.empty())
    {
        int olen = WideCharToMultiByte(CP_UTF8, 0, oldPath.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string oUtf8(olen, '\0');
        WideCharToMultiByte(CP_UTF8, 0, oldPath.c_str(), -1, &oUtf8[0], olen, nullptr, nullptr);
        sqlite3_bind_text(stmt, 4, oUtf8.c_str(), -1, SQLITE_TRANSIENT);
    }
    else
    {
        sqlite3_bind_null(stmt, 4);
    }

    bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    return ok;
}

std::vector<FileActivity> ActivityDatabase::Query(TimeWindow window)
{
    return QueryCustomSeconds(WindowToSeconds(window));
}

std::vector<FileActivity> ActivityDatabase::QueryCustomSeconds(int64_t seconds)
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

        const char* a = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        const char* p = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        const char* o = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));

        if (a)
        {
            int wlen = MultiByteToWideChar(CP_UTF8, 0, a, -1, nullptr, 0);
            fa.action.resize(wlen - 1);
            MultiByteToWideChar(CP_UTF8, 0, a, -1, &fa.action[0], wlen);
        }
        if (p)
        {
            int wlen = MultiByteToWideChar(CP_UTF8, 0, p, -1, nullptr, 0);
            fa.path.resize(wlen - 1);
            MultiByteToWideChar(CP_UTF8, 0, p, -1, &fa.path[0], wlen);
        }
        if (o)
        {
            int wlen = MultiByteToWideChar(CP_UTF8, 0, o, -1, nullptr, 0);
            fa.oldPath.resize(wlen - 1);
            MultiByteToWideChar(CP_UTF8, 0, o, -1, &fa.oldPath[0], wlen);
        }

        results.push_back(std::move(fa));
    }

    sqlite3_finalize(stmt);
    return results;
}

void ActivityDatabase::EvictOlderThan30Days()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_db) return;

    int64_t cutoff = static_cast<int64_t>(std::time(nullptr)) - 30LL * 24 * 3600;

    const char* sql = "DELETE FROM file_activity WHERE timestamp < ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) == SQLITE_OK)
    {
        sqlite3_bind_int64(stmt, 1, cutoff);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
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

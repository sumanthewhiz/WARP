#pragma once

#include <windows.h>
#include <string>
#include <vector>
#include <mutex>
#include <cstdint>

// --- Event type flags (bitmask for queries) ---
static const uint32_t EVENT_TYPE_FILE       = 0x01;
static const uint32_t EVENT_TYPE_APP_LAUNCH = 0x02;
static const uint32_t EVENT_TYPE_BROWSING   = 0x04;
static const uint32_t EVENT_TYPE_ALL        = 0x07;

struct FileActivity
{
    int64_t      id;
    int64_t      timestampUtc;   // Unix epoch seconds
    std::wstring action;         // CREATE, OPEN, MODIFY, DELETE, RENAME
    std::wstring path;
    std::wstring oldPath;        // For RENAME -- the previous path
};

struct AppLaunchActivity
{
    int64_t      id;
    int64_t      timestampUtc;
    std::wstring exeName;        // e.g. "notepad.exe"
    std::wstring exePath;        // full path to the executable
    DWORD        pid;
};

struct BrowsingActivity
{
    int64_t      id;
    int64_t      timestampUtc;
    std::wstring browser;        // e.g. "chrome", "msedge", "firefox"
    std::wstring title;          // page/tab title from window title bar
    std::wstring url;            // URL if extractable, else empty
};

enum class TimeWindow
{
    Minutes15,
    Minutes30,
    Hours1,
    Hours2,
    Hours6,
    Hours24,
    Days7,
    Days15,
    Days30
};

class ActivityDatabase
{
public:
    ActivityDatabase();
    ~ActivityDatabase();

    bool Open(const std::wstring& dbPath);
    void Close();

    // File activity
    bool InsertActivity(const std::wstring& action,
                        const std::wstring& path,
                        const std::wstring& oldPath = L"");

    std::vector<FileActivity> QueryFiles(TimeWindow window);
    std::vector<FileActivity> QueryFilesCustomSeconds(int64_t seconds);

    // App launch activity
    bool InsertAppLaunch(const std::wstring& exeName,
                         const std::wstring& exePath,
                         DWORD pid);

    std::vector<AppLaunchActivity> QueryAppLaunches(TimeWindow window);
    std::vector<AppLaunchActivity> QueryAppLaunchesCustomSeconds(int64_t seconds);

    // Browsing activity
    bool InsertBrowsingActivity(const std::wstring& browser,
                                const std::wstring& title,
                                const std::wstring& url = L"");

    std::vector<BrowsingActivity> QueryBrowsing(TimeWindow window);
    std::vector<BrowsingActivity> QueryBrowsingCustomSeconds(int64_t seconds);

    void EvictOlderThan30Days();
    void ClearAll();

    // Legacy wrappers
    std::vector<FileActivity> Query(TimeWindow window) { return QueryFiles(window); }
    std::vector<FileActivity> QueryCustomSeconds(int64_t seconds) { return QueryFilesCustomSeconds(seconds); }

    // Expose internals for InferenceEngine (it needs direct sqlite3 access
    // with its own lock ordering to avoid deadlocks).
    struct sqlite3** DbHandle() { return &m_db; }
    std::mutex*      DbMutex()  { return &m_mutex; }

private:
    struct sqlite3* m_db = nullptr;
    std::mutex      m_mutex;

    bool Execute(const char* sql);
    int64_t WindowToSeconds(TimeWindow w);

    static std::string WideToUtf8(const std::wstring& w);
    static std::wstring Utf8ToWide(const char* s);
};

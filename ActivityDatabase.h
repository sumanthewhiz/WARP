#pragma once

#include <windows.h>
#include <string>
#include <vector>
#include <mutex>
#include <cstdint>

struct FileActivity
{
    int64_t     id;
    int64_t     timestampUtc;   // Unix epoch seconds
    std::wstring action;        // CREATE, OPEN, MODIFY, DELETE, RENAME, COPY, MOVE
    std::wstring path;
    std::wstring oldPath;       // For RENAME/MOVE – the previous path
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

    bool InsertActivity(const std::wstring& action,
                        const std::wstring& path,
                        const std::wstring& oldPath = L"");

    std::vector<FileActivity> Query(TimeWindow window);
    std::vector<FileActivity> QueryCustomSeconds(int64_t seconds);

    void EvictOlderThan30Days();

private:
    struct sqlite3* m_db = nullptr;
    std::mutex      m_mutex;

    bool Execute(const char* sql);
    int64_t WindowToSeconds(TimeWindow w);
};

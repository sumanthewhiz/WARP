#pragma once

#include <windows.h>
#include <thread>
#include <atomic>
#include <functional>
#include "ActivityDatabase.h"

// Named pipe server that exposes a JSON query API for other Windows apps.
// Pipe name: \\.\pipe\WarpFileActivityAPI
//
// Protocol (text-based, line-delimited JSON):
//   Request:  { "window": "15m" }   – valid: 15m,30m,1h,2h,6h,24h,7d,15d,30d
//   Request:  { "seconds": 300 }    – custom time range in seconds
//   Response: JSON array of activity records
class QueryApi
{
public:
    QueryApi();
    ~QueryApi();

    void Start(ActivityDatabase* db);
    void Stop();

private:
    ActivityDatabase* m_db = nullptr;
    std::thread m_thread;
    std::atomic<bool> m_running{ false };
    HANDLE m_stopEvent = nullptr;

    void Run();
    void HandleClient(HANDLE hPipe);
    std::string BuildJsonResponse(const std::vector<FileActivity>& activities);
};

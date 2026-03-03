#pragma once

#include <windows.h>
#include <thread>
#include <atomic>
#include <functional>
#include "ActivityDatabase.h"

// Named pipe server that exposes a JSON query API for other Windows apps.
// Pipe name: \\.\pipe\WarpFileActivityAPI
//
// Protocol (text-based, JSON):
//   Request:  { "window": "15m" }          -- valid: 15m,30m,1h,2h,6h,24h,7d,15d,30d
//   Request:  { "seconds": 300 }           -- custom time range in seconds
//   Request:  { "window": "1h", "types": ["file","app_launch","browsing"] }
//   Response: JSON with segregated event types
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

    static std::string WideToUtf8(const std::wstring& w);
    static std::string EscapeJson(const std::string& s);
    std::string BuildJsonResponse(uint32_t eventTypes, int64_t seconds);
};

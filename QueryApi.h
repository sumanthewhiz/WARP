#pragma once

#include <windows.h>
#include <thread>
#include <atomic>
#include <functional>
#include "ActivityDatabase.h"

class InferenceEngine;
class TopicInference;

// Named pipe server
// Pipe name: \\.\pipe\WarpFileActivityAPI
//
// Protocol (text-based, JSON):
//   Request:  { "window": "15m" }          -- valid: 15m,30m,1h,2h,6h,24h,7d,15d,30d
//   Request:  { "seconds": 300 }           -- custom time range in seconds
//   Request:  { "window": "1h", "types": ["file","app_launch","browsing"] }
//   Request:  { "op": "QueryInferences", "paths": [...], "fields": [...] }
//   Request:  { "op": "GetInferenceDeltas", "since_version": 100 }
//   Request:  { "op": "GetRecentContext" }
//   Response: JSON with segregated event types or inference results
class QueryApi
{
public:
    QueryApi();
    ~QueryApi();

    void Start(ActivityDatabase* db, InferenceEngine* inference, TopicInference* topicInf = nullptr);
    void Stop();

private:
    ActivityDatabase*  m_db        = nullptr;
    InferenceEngine*   m_inference = nullptr;
    TopicInference*    m_topicInf  = nullptr;
    std::thread m_thread;
    std::atomic<bool> m_running{ false };
    HANDLE m_stopEvent = nullptr;

    void Run();
    void HandleClient(HANDLE hPipe);

    static std::string WideToUtf8(const std::wstring& w);
    static std::string EscapeJson(const std::string& s);
    std::string BuildJsonResponse(uint32_t eventTypes, int64_t seconds);
};

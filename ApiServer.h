#pragma once
// ApiServer — local consumer API (replaces QueryApi).
//
// Gap-analysis fixes:
//   API-1  explicit pipe DACL (user + SYSTEM + Administrators, network denied, medium label),
//          PIPE_REJECT_REMOTE_CLIENTS, FILE_FLAG_FIRST_PIPE_INSTANCE squatting detection,
//          caller identification (PID / session / image) with policy allow-lists and scopes.
//   API-2  ERROR_PIPE_CONNECTED / ERROR_NO_DATA handled; listeners are re-armed before dispatch.
//   API-3  requests read in a loop (ERROR_MORE_DATA) up to 1 MiB, structured `request_too_large`.
//   API-4  strict JSON parsing (WarpJson), canonical keys, caller keys echoed verbatim,
//          `unknown_op` instead of a raw-event dump.
//   API-5  bounded worker pool, per-connection deadlines, hard row limits with cursors,
//          exception guard, in-flight requests drained on Stop().
//   API-6  v2 ops: ScoreEntities, TopEntities, GetCurrentContext, QueryTimeline, SearchActivity,
//          GetWorkThreads, GetRelated, GetChanges, WaitForChanges, GetStatus, Purge, Pause, Resume.
//   API-7  `api_version` on every response, one compliant JSON writer, documented limits.
//
// Pipes: the legacy name \\.\pipe\WarpFileActivityAPI (served to clients in the same session)
// and the per-session name \\.\pipe\WarpFileActivityAPI-<SessionId> (preferred).

#include <windows.h>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "Json.h"

class ActivityStore;
class EntityStore;
class HistoryEngine;
class ContextInference;

struct ApiClient
{
    bool         inProcess = false;   // WARP's own UI / CLI (all scopes)
    DWORD        pid = 0;
    DWORD        sessionId = 0;
    std::wstring exePath;
    bool         activityScope = true;  // may read titles / URLs / paths / timeline
    bool         controlScope = false;  // may Purge / Pause / Resume

    static ApiClient InProcess();
};

class ApiServer
{
public:
    static constexpr int kApiVersion = 2;
    static constexpr size_t kMaxRequestBytes = 1024 * 1024;

    struct Deps
    {
        ActivityStore*    store = nullptr;
        EntityStore*      entities = nullptr;
        HistoryEngine*    history = nullptr;
        ContextInference* context = nullptr;
        // Writes extra members (monitor health, presence, models) into the open GetStatus object.
        // `full` is true for WARP's own CLI / tray: only then may paths and error texts be included.
        std::function<void(WarpJson::Writer&, bool full)> statusProvider;
        // Pause capture until `untilUtc` (INT64_MAX = until resumed, 0 = resume now). Returns false if locked by policy.
        std::function<bool(int64_t untilUtc)> setPaused;
        std::string productVersion;
    };

    struct Stats
    {
        uint64_t requests = 0, errors = 0, rejected = 0, busy = 0;
        int      activeConnections = 0;
        bool     legacyPipeOwned = false;
        bool     sessionPipeOwned = false;
    };

    ApiServer();
    ~ApiServer();

    // Sets the dependencies without listening (in-process callers and tests); Start() calls it.
    void Configure(const Deps& deps);
    bool Start(const Deps& deps);
    void Stop();

    // Handles one request (any thread). Used by the pipe workers and in-process callers.
    std::string Handle(const std::string& requestUtf8, const ApiClient& client);
    Stats GetStats() const;

    static const wchar_t* LegacyPipeName() { return L"\\\\.\\pipe\\WarpFileActivityAPI"; }
    static std::wstring   SessionPipeName(DWORD sessionId);

private:
    struct Listener
    {
        std::wstring name;
        bool         legacy = false;
        HANDLE       pipe = INVALID_HANDLE_VALUE;
        HANDLE       evt = nullptr;
        OVERLAPPED   ov = {};
        bool         preConnected = false;
        bool         owned = false;
        int64_t      retryAtMs = 0;
        std::atomic<int> live{ 0 };        // instances alive (listening or being served)
    };
    struct Conn { HANDLE pipe; Listener* listener; };

    void AcceptLoop();
    bool CreateInstance(Listener& l);
    void WorkerLoop();
    void Serve(HANDLE pipe, Listener* l);
    bool ReadMessage(HANDLE pipe, HANDLE evt, std::string& out, DWORD timeoutMs, bool& tooLarge);
    bool WriteMessage(HANDLE pipe, HANDLE evt, const std::string& data, DWORD timeoutMs);
    ApiClient IdentifyClient(HANDLE pipe, bool legacy, std::string& rejectCode);

    // ---- request handlers (return complete JSON documents) ----
    std::string Dispatch(const WarpJson::Value& req, const ApiClient& c);
    std::string RawEvents(const WarpJson::Value& req, const ApiClient& c);
    std::string QueryInferences(const WarpJson::Value& req);
    std::string GetInferenceDeltas(const WarpJson::Value& req);
    std::string ScoreEntities(const WarpJson::Value& req, const ApiClient& c);
    std::string TopEntities(const WarpJson::Value& req, const ApiClient& c);
    std::string SearchActivity(const WarpJson::Value& req);
    std::string GetChanges(const WarpJson::Value& req, const ApiClient& c);
    std::string WaitForChanges(const WarpJson::Value& req);
    std::string QueryTimeline(const WarpJson::Value& req);
    std::string GetWorkThreads(const WarpJson::Value& req);
    std::string GetRelated(const WarpJson::Value& req);
    std::string GetCurrentContext(const WarpJson::Value& req);
    std::string GetStatus(const ApiClient& c);
    std::string Purge(const WarpJson::Value& req);
    std::string Pause(const WarpJson::Value& req, bool resume);

    static std::string Error(const char* code, const std::string& message);
    static std::string WithApiVersion(const std::string& jsonObject);

    Deps m_deps;
    std::atomic<bool> m_running{ false };
    HANDLE m_stopEvent = nullptr;
    std::thread m_acceptThread;
    std::vector<std::thread> m_workers;
    Listener m_legacy;
    Listener m_session;
    PSECURITY_DESCRIPTOR m_sd = nullptr;
    int64_t m_startedUtc = 0;
    DWORD m_sessionId = 0;
    std::wstring m_selfExe;

    std::mutex m_qMtx;
    std::condition_variable m_qCv;
    std::deque<Conn> m_queue;

    std::atomic<int> m_longPolls{ 0 };
    mutable std::mutex m_statsMtx;
    Stats m_stats;
};

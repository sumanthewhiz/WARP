#pragma once

#include <windows.h>
#include <string>
#include <atomic>
#include <functional>
#include <mutex>

#include "EventContext.h"

// Callback: exeName, exePath, windowTitle, durationSecs, EventContext.
using ForegroundCallback = std::function<void(const std::wstring& exeName,
                                              const std::wstring& exePath,
                                              const std::wstring& windowTitle,
                                              int                 durationSecs,
                                              const EventContext& ctx)>;

class ForegroundMonitor
{
public:
    ForegroundMonitor();
    ~ForegroundMonitor();

    void SetCallback(ForegroundCallback cb);
    void Start();
    void Stop();
    void Pause();
    void Resume();

    // Snapshot of the currently-tracked foreground session.  ContextInference
    // calls this once per snapshot so that the live (not-yet-emitted) session
    // counts toward the rolling window -- otherwise a user staying in one
    // window for >15 minutes would have *zero* AppFocus rows in the DB query
    // and the summary would be empty.  Returns false if no session is being
    // tracked (idle, system-foreground, or just before the first focus event).
    struct ActiveSession
    {
        std::wstring exeName;
        std::wstring exePath;
        std::wstring windowTitle;
        int          durationSoFarSecs = 0;
        int64_t      startedAtUtcSecs  = 0; // Wall-clock start; for window filtering.
    };
    bool GetCurrentSession(ActiveSession& out) const;

private:
    // Invoked by the ForegroundChangeBroker on every focus change.
    void OnForegroundChanged(HWND hwnd, DWORD pid);

    // Emit the previous session if it had meaningful duration. Caller
    // must hold m_stateMtx.
    void EmitPreviousLocked(ULONGLONG nowTick);

    ForegroundCallback m_callback;
    std::atomic<bool>  m_running{ false };
    std::atomic<bool>  m_paused{ false };
    size_t             m_brokerToken = 0;

    // State for the currently-tracked foreground session.
    mutable std::mutex m_stateMtx;
    DWORD              m_prevPid = 0;
    std::wstring       m_prevExeName;
    std::wstring       m_prevExePath;
    std::wstring       m_prevTitle;
    ULONGLONG          m_prevStartTick    = 0;
    int64_t            m_prevStartUtcSecs = 0;
};

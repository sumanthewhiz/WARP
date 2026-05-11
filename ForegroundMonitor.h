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
    std::mutex   m_stateMtx;
    DWORD        m_prevPid = 0;
    std::wstring m_prevExeName;
    std::wstring m_prevExePath;
    std::wstring m_prevTitle;
    ULONGLONG    m_prevStartTick = 0;
};

#pragma once

#include <windows.h>
#include <string>
#include <thread>
#include <atomic>
#include <functional>

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
    ForegroundCallback m_callback;
    std::thread m_thread;
    std::atomic<bool> m_running{ false };
    std::atomic<bool> m_paused{ false };
    HANDLE m_stopEvent = nullptr;

    void MonitorLoop();
};

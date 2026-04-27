#pragma once

#include <windows.h>
#include <string>
#include <thread>
#include <atomic>
#include <functional>

// Callback: exeName, exePath, windowTitle, durationSecs
using ForegroundCallback = std::function<void(const std::wstring& exeName,
                                              const std::wstring& exePath,
                                              const std::wstring& windowTitle,
                                              int durationSecs)>;

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

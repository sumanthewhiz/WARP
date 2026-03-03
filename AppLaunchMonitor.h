#pragma once

#include <windows.h>
#include <string>
#include <thread>
#include <atomic>
#include <functional>

// Callback: exeName, exePath, pid
using AppLaunchCallback = std::function<void(const std::wstring& exeName,
                                             const std::wstring& exePath,
                                             DWORD pid)>;

class AppLaunchMonitor
{
public:
    AppLaunchMonitor();
    ~AppLaunchMonitor();

    void SetCallback(AppLaunchCallback cb);
    void Start();
    void Stop();
    void Pause();
    void Resume();

private:
    AppLaunchCallback m_callback;
    std::thread m_thread;
    std::atomic<bool> m_running{ false };
    std::atomic<bool> m_paused{ false };
    HANDLE m_stopEvent = nullptr;

    void MonitorLoop();
};

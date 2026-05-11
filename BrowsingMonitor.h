#pragma once

#include <windows.h>
#include <string>
#include <thread>
#include <atomic>
#include <functional>

#include "EventContext.h"

// Callback: browser name, page title, url, EventContext.
using BrowsingCallback = std::function<void(const std::wstring& browser,
                                            const std::wstring& title,
                                            const std::wstring& url,
                                            const EventContext& ctx)>;

class BrowsingMonitor
{
public:
    BrowsingMonitor();
    ~BrowsingMonitor();

    void SetCallback(BrowsingCallback cb);
    void Start();
    void Stop();
    void Pause();
    void Resume();

private:
    BrowsingCallback m_callback;
    std::thread m_thread;
    std::atomic<bool> m_running{ false };
    std::atomic<bool> m_paused{ false };
    HANDLE m_stopEvent = nullptr;

    void MonitorLoop();
};

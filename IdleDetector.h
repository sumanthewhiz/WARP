#pragma once

#include <windows.h>
#include <atomic>
#include <functional>
#include <thread>

// Detects PC idle state and sleep/wake transitions.
// Calls onIdle() when idle, onActive() when active again.
class IdleDetector
{
public:
    using Callback = std::function<void()>;

    IdleDetector();
    ~IdleDetector();

    void SetCallbacks(Callback onIdle, Callback onActive);
    void Start(DWORD idleThresholdMs = 120000); // default 2 min idle
    void Stop();

private:
    Callback m_onIdle;
    Callback m_onActive;
    std::thread m_thread;
    std::atomic<bool> m_running{ false };
    HANDLE m_stopEvent = nullptr;
    DWORD m_idleThreshold = 120000;
    bool m_isIdle = false;

    // Power notification
    static HWND s_msgWnd;
    static IdleDetector* s_instance;
    static LRESULT CALLBACK MsgWndProc(HWND, UINT, WPARAM, LPARAM);

    void Run();
};

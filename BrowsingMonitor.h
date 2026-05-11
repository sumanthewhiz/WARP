#pragma once

#include <windows.h>
#include <string>
#include <atomic>
#include <functional>
#include <mutex>

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
    // Foreground change callback from ForegroundChangeBroker.
    void OnForegroundChanged(HWND hwnd, DWORD pid);

    // Per-PID NAMECHANGE callback for in-window navigation.
    static void CALLBACK NameChangeProc(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD);

    // Re-evaluate the current foreground HWND -- used by both the foreground
    // hook and the per-PID NAMECHANGE hook (since both signal "browser title
    // may have changed").
    void EvaluateBrowserState(HWND hwnd, DWORD pid);

    BrowsingCallback   m_callback;
    std::atomic<bool>  m_running{ false };
    std::atomic<bool>  m_paused{ false };
    size_t             m_brokerToken = 0;

    std::mutex         m_stateMtx;
    DWORD              m_currentBrowserPid = 0;     // 0 if FG is not a browser
    HWND               m_currentBrowserHwnd = nullptr;
    HWINEVENTHOOK      m_nameHook = nullptr;        // installed only while FG is a browser
    std::wstring       m_lastTitle;
    std::wstring       m_lastBrowser;
};

// ---------------------------------------------------------------------------
// ForegroundChangeBroker
//
// Single source of truth for "the foreground window changed" notifications.
//
// The previous design had ForegroundMonitor and BrowsingMonitor each running
// their own 3-second polling thread that called GetForegroundWindow(). That
// meant:
//   * Up to 3 seconds of attribution lag on every foreground switch.
//   * Two threads doing duplicate work and arriving at slightly different
//     conclusions depending on poll timing.
//   * No way to react to an Alt+Tab burst -- by the time the next poll
//     fired, the user had moved on.
//
// This broker installs a single SetWinEventHook(EVENT_SYSTEM_FOREGROUND)
// on the main UI thread (which already has a message pump for the
// LaunchCorrelator hook) and fans the event out to all subscribers. The
// hook fires synchronously the moment Windows promotes a new HWND to the
// foreground -- typically within milliseconds of the user gesture.
//
// The broker also notifies EventContext so subsequent CaptureContext()
// calls can attribute an event to the foreground PID without needing to
// call GetForegroundWindow() themselves (which is process-thread-affine
// and racy from worker threads like the ETW callback).
// ---------------------------------------------------------------------------

#pragma once

#include <windows.h>
#include <functional>
#include <mutex>
#include <vector>

class ForegroundChangeBroker
{
public:
    // Subscriber callback: (newForegroundHwnd, newForegroundPid).
    // Called on the broker's hook thread (the main UI thread).
    // Subscribers MUST do minimal work or marshal to their own thread;
    // blocking here stalls the UI message pump.
    using Listener = std::function<void(HWND, DWORD)>;

    static ForegroundChangeBroker& Instance();

    // Install the EVENT_SYSTEM_FOREGROUND hook. Must be called from a
    // thread with a message pump (we expect the main UI thread).
    // Subsequent calls are no-ops.
    void Start();

    // Tear down the hook and clear listeners.
    void Stop();

    // Subscribe a listener. Returns an opaque token used by Unsubscribe.
    // Thread-safe.
    size_t Subscribe(Listener cb);

    void Unsubscribe(size_t token);

private:
    ForegroundChangeBroker() = default;
    ForegroundChangeBroker(const ForegroundChangeBroker&) = delete;
    ForegroundChangeBroker& operator=(const ForegroundChangeBroker&) = delete;

    static void CALLBACK WinEventProc(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD);

    void Dispatch(HWND hwnd);

    HWINEVENTHOOK m_hook = nullptr;
    std::mutex    m_mtx;
    std::vector<std::pair<size_t, Listener>> m_listeners;
    size_t        m_nextToken = 1;
};

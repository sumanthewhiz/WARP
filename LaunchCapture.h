#pragma once
// LaunchCapture — process launches and exits (replaces AppLaunchMonitor + LaunchCorrelator).
//
//   APP-1  The Kernel-Process ProcessStart payload is decoded with TDH (ProcessID, ParentProcessID,
//          SessionID, CreateTime, ImageName, package identity). The event HEADER pid is the
//          creator (explorer.exe for Start/taskbar launches) and is kept as creator_pid.
//   APP-2  Launches are parked until a user-facing window shows; windows that already exist at
//          park time and windows shown shortly BEFORE the (lagging) ETW delivery are matched too.
//   APP-3  "User saw a window" = visible, root, unowned, not a tool window, not cloaked, real size.
//   APP-4  Real tools (cmd, PowerShell, Task Manager, installers) are no longer hidden; helper
//          processes of an app (same image as the parent) are not launches; exits are recorded
//          with lifetimes; packaged apps carry their AUMID.
//   RUN-3 / SIG-5  Only processes in WARP's own session are recorded.
//   FILE-9 / SIG-6 Every start/stop feeds EventContextUtil's process table.

#include <windows.h>
#include <evntrace.h>
#include <evntcons.h>

#include <atomic>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "ActivityStore.h"

class LaunchCapture
{
public:
    using LaunchCallback = std::function<void(LaunchEventRecord&& rec)>;
    using ExitCallback   = std::function<void(ProcessExitRecord&& rec)>;

    struct Stats
    {
        uint64_t starts = 0, recorded = 0, helpers = 0, system = 0, otherSession = 0;
        uint64_t withWindow = 0, noWindow = 0, exits = 0, eventsLost = 0;
        bool     etwRunning = false;
        int64_t  lastEventMs = 0;
        std::wstring sessionName, lastError;
    };

    LaunchCapture();
    ~LaunchCapture();

    void SetCallbacks(LaunchCallback onLaunch, ExitCallback onExit);
    bool Start();
    void Stop();
    void SetPaused(bool paused);         // privacy pause: nothing recorded (process table still maintained)
    void Heartbeat();                    // supervisor: restart a dead consumer (backoff), poll EventsLost
    // Called from the UI thread's WinEvent hook for EVENT_OBJECT_SHOW (OBJID_WINDOW).
    void OnWindowShown(HWND hwnd);
    Stats GetStats() const;

    static bool IsUserFacingWindow(HWND hwnd);
    static std::wstring DevicePathToDos(const std::wstring& devicePath);

private:
    struct Pending
    {
        LaunchEventRecord rec;
        ULONGLONG         parkedTick = 0;
        ULONGLONG         startTick = 0;
    };

    void EtwThread();
    bool StartSession();
    void StopSession();
    static void WINAPI EventCallback(PEVENT_RECORD ev);
    void OnProcessStart(PEVENT_RECORD ev);
    void OnProcessStop(PEVENT_RECORD ev);
    void SweeperThread();
    void Finalize(Pending&& p, bool sawWindow, DWORD windowDelayMs);
    bool HasUserFacingWindowNow(DWORD pid) const;

    LaunchCallback m_onLaunch;
    ExitCallback   m_onExit;
    std::atomic<bool> m_running{ false };
    std::atomic<bool> m_paused{ false };
    std::atomic<bool> m_etwAlive{ false };
    std::atomic<bool> m_etwExited{ true };     // EtwThread has returned: joining it cannot block
    HANDLE m_stopEvent = nullptr;
    std::thread m_etwThread;
    std::thread m_sweeper;
    TRACEHANDLE m_session = 0;
    std::atomic<TRACEHANDLE> m_trace{ INVALID_PROCESSTRACE_HANDLE };
    std::wstring m_sessionName;
    ULONGLONG m_nextRestartTick = 0;
    int m_restartBackoffSec = 5;

    mutable std::mutex m_mtx;
    std::unordered_map<DWORD, Pending> m_pending;              // pid -> parked launch
    std::unordered_map<DWORD, ULONGLONG> m_recentWindows;      // pid -> tick a user-facing window showed
    std::unordered_map<DWORD, int64_t> m_recorded;             // pid -> start ms (for exits)
    Stats m_stats;
};

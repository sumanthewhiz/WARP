#pragma once

// PresenceMonitor
// -----------------------------------------------------------------------------
// Classifies whether the user is at the machine:
//
//   Active   keyboard/mouse input within the last awayAfterMs
//   Passive  no recent input, but a "present without typing" signal is on:
//            audible media playback, microphone or webcam in use, or a
//            full-screen / presentation app
//   Away     no recent input and no passive signal (passive signals are
//            ignored once the display has been off for longer than
//            awayAfterMs -- nobody watches a dark screen)
//   Locked   session locked or disconnected (console / remote)
//   Asleep   system suspended (PBT_APMSUSPEND until resume)
//
// Precedence: Asleep > Locked > Active > Passive > Away.
//
// Everything runs on one dedicated thread that owns a message-only window
// (session, suspend/resume and power-setting notifications) and polls input
// every 2 s; passive signals are refreshed every 5 s and right before a
// transition that depends on them. Listeners run on that thread, outside
// internal locks, and must return quickly (PBT_APMSUSPEND is delivered while
// the OS waits). On resume the monitor calls EventContextUtil::SetWakeBoundary.
//
// Start() computes the initial state silently (read it with GetSnapshot());
// listeners fire for every transition after that.
// -----------------------------------------------------------------------------

#include <windows.h>
#include <functional>
#include <vector>
#include <mutex>
#include <thread>
#include <atomic>
#include <cstdint>

enum class PresenceState { Active, Passive, Away, Locked, Asleep };

const char* PresenceStateName(PresenceState s);   // "active","passive","away","locked","asleep"

struct PresenceSnapshot
{
    PresenceState state = PresenceState::Active;
    int64_t sinceUtc      = 0;   // when the current state began (Unix seconds)
    int64_t lastInputUtc  = 0;   // Unix seconds of last keyboard/mouse input (GetLastInputInfo)
    DWORD   msSinceInput  = 0;
    bool    mediaPlaying  = false;  // any active, non-silent audio render session
    bool    micInUse      = false;  // microphone in use (ConsentStore and/or active capture sessions)
    bool    cameraInUse   = false;  // webcam in use (ConsentStore)
    bool    fullscreen    = false;  // SHQueryUserNotificationState: D3D full screen / presentation / busy
    bool    sessionLocked = false;
    bool    onBattery     = false;  // GetSystemPowerStatus ACLineStatus==0
    bool    batterySaver  = false;  // SYSTEM_POWER_STATUS.SystemStatusFlag == 1
};

class PresenceMonitor
{
public:
    using Callback = std::function<void(PresenceState oldState, PresenceState newState, const PresenceSnapshot& snap)>;

    static PresenceMonitor& Instance();

    // awayAfterMs: no input for this long AND no passive signal => Away (default 300000; 0 restores the default).
    void Configure(DWORD awayAfterMs);
    void AddListener(Callback cb);        // invoked on the monitor thread, outside internal locks
    bool Start();                         // idempotent
    void Stop();                          // idempotent; joins the monitor thread
    PresenceSnapshot GetSnapshot() const; // thread-safe
    // Called by other components when they observe user activity (e.g. foreground window changed):
    // forces immediate re-evaluation (an Away->Active transition must not wait for the next poll).
    void NotifyUserActivity();

private:
    struct Worker;   // monitor-thread state; defined in PresenceMonitor.cpp

    PresenceMonitor();
    ~PresenceMonitor();
    PresenceMonitor(const PresenceMonitor&) = delete;
    PresenceMonitor& operator=(const PresenceMonitor&) = delete;

    void ThreadMain();
    bool IsMonitorThread() const;

    std::mutex            m_lifecycleMutex;          // serializes Start/Stop
    std::thread           m_thread;
    std::atomic<DWORD>    m_threadId{ 0 };
    std::atomic<int>      m_startResult{ 0 };        // 0 pending, 1 ok, -1 failed
    std::atomic<DWORD>    m_awayAfterMs{ 300000 };
    HANDLE                m_stopEvent  = nullptr;    // manual reset
    HANDLE                m_wakeEvent  = nullptr;    // auto reset
    HANDLE                m_readyEvent = nullptr;    // auto reset

    mutable std::mutex    m_snapMutex;
    PresenceSnapshot      m_snap;

    std::mutex            m_listenerMutex;
    std::vector<Callback> m_listeners;
};

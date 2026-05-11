#pragma once

#include <windows.h>
#include <atomic>
#include <functional>
#include <thread>

// IdleDetector
//
// Two-tier idle awareness:
//
//   * Attribution threshold (default 30s): user has stopped driving the
//     system, so subsequent events that fire are increasingly unlikely to
//     be user-initiated. We don't pause monitoring -- we just mark that
//     attribution is "lost" so consumers can downgrade confidence.
//
//   * Pause threshold (default 120s): user has truly walked away. We
//     stop emitting events entirely (the monitors call their own
//     Pause()) and the ETW buffers will accumulate.
//
// On Resume from sleep / lock / long idle, IdleDetector publishes a
// "wake boundary" tick (now + 5s by default). EventContext consults this
// and downgrades confidence on every event during the boundary window,
// because the seconds after wake are dominated by:
//
//   * SuperFetch/Prefetch reading files
//   * Search indexer catching up
//   * Defender real-time scan kicking off
//   * Browser background sync, OneDrive reconciliation
//   * COM activations from system housekeeping
//
// None of those are user activity, even though the human just pressed a
// key to wake the machine. The 5s window is enough to absorb the burst
// without noticeably delaying genuine user activity (which usually
// takes longer than 5s to start after wake anyway).

class IdleDetector
{
public:
    using Callback = std::function<void()>;

    IdleDetector();
    ~IdleDetector();

    // pause-threshold callbacks (the original Start() semantics)
    void SetCallbacks(Callback onIdle, Callback onActive);

    // attribution-threshold callbacks. Optional; if not set the broker
    // is silent on attribution transitions but still tracks state.
    void SetAttributionCallbacks(Callback onAttribLost, Callback onAttribRegained);

    void Start(DWORD pauseThresholdMs = 120000,
               DWORD attribThresholdMs = 30000,
               DWORD wakeBoundaryMs = 5000);
    void Stop();

private:
    Callback m_onIdle;
    Callback m_onActive;
    Callback m_onAttribLost;
    Callback m_onAttribRegained;
    std::thread m_thread;
    std::atomic<bool> m_running{ false };
    HANDLE m_stopEvent      = nullptr;
    DWORD  m_pauseThreshold = 120000;
    DWORD  m_attribThreshold = 30000;
    DWORD  m_wakeBoundaryMs = 5000;
    bool   m_isIdle         = false;
    bool   m_attribLost     = false;

    // Power notification
    static HWND s_msgWnd;
    static IdleDetector* s_instance;
    static LRESULT CALLBACK MsgWndProc(HWND, UINT, WPARAM, LPARAM);

    void Run();
    void TriggerWakeBoundary();
};

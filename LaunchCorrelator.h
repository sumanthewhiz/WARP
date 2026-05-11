#pragma once

// LaunchCorrelator.h
// -----------------------------------------------------------------------------
// Window-creation correlation for app-launch events.
//
// When AppLaunchMonitor observes a process-start ETW event the source PID
// is known but it isn't yet clear whether the user actually saw anything --
// the process could be a console-only worker, a shell extension surrogate,
// or a background updater that will never paint a window. Instead of firing
// the launch event immediately and hoping it isn't noise, we PARK the event
// in this correlator and wait up to 5 seconds for a top-level visible
// window owned by the same PID to appear.
//
//   * If a top-level window appears within the budget, the event fires with
//     confidence=1.0 and createdWindowMs set to the observed delta, which
//     downstream consumers use as evidence that this was a user-visible
//     launch.
//   * If the budget elapses, the event still fires (downstream may want it
//     for its own reasons), but with confidence=0.3 -- below the 0.5
//     threshold InferenceEngine uses for incrementing open_count_*d. The
//     row therefore lands in the database for forensic review without
//     polluting the rolling popularity counts.
//
// The window observer is installed by WARP!.cpp's main thread via
// SetWinEventHook(EVENT_OBJECT_CREATE) and routes incoming HWNDs into
// LaunchCorrelator::Instance().OnWindowCreated(). A small sweeper thread
// owned by the correlator handles the timeout case.
// -----------------------------------------------------------------------------

#include <windows.h>
#include <string>
#include <functional>

#include "EventContext.h"

class LaunchCorrelator
{
public:
    using FireCallback = std::function<void(const std::wstring& exeName,
                                            const std::wstring& exePath,
                                            DWORD                pid,
                                            const EventContext&  ctx)>;

    static LaunchCorrelator& Instance();

    // Background sweeper lifetime control. Call Start() once at startup
    // (after subsystems are constructed) and Stop() before shutdown.
    void Start();
    void Stop();

    // Called from AppLaunchMonitor's ETW callback (worker thread). The
    // event is parked; the correlator will eventually invoke `cb` itself
    // either with a confirmed window (confidence=1.0) or after the
    // timeout (confidence=0.3).
    void RecordPending(const std::wstring& exeName,
                       const std::wstring& exePath,
                       DWORD                pid,
                       const EventContext&  ctx,
                       FireCallback         cb);

    // Called from the main thread's SetWinEventHook proc when ANY window
    // is created. We resolve the owning PID and complete the matching
    // pending entry (if any).
    void OnWindowCreated(HWND hwnd);

private:
    LaunchCorrelator();
    ~LaunchCorrelator();
    LaunchCorrelator(const LaunchCorrelator&) = delete;
    LaunchCorrelator& operator=(const LaunchCorrelator&) = delete;

    struct Pending;     // opaque to header
    struct Impl;
    Impl* m_impl = nullptr;
};

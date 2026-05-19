#pragma once

// EventContext.h
// -----------------------------------------------------------------------------
// Cross-cutting "intent context" attached to every captured event.
//
// Surface-level event metadata (path/exe/title) is not enough to distinguish
// user-initiated activity from system noise. The EventContext bundles the
// secondary signals the rest of the pipeline needs to score user-intent:
//
//   * The PID that actually originated the event (for ETW: pEvent->EventHeader.
//     ProcessId; for foreground/browsing: GetWindowThreadProcessId of the
//     event HWND).
//   * The exe path of that PID (resolved once via QueryFullProcessImageName).
//   * The PID currently in the foreground at event time, plus its exe path.
//   * Milliseconds since the last user input (GetLastInputInfo).
//   * Optional parent-PID/exe (filled by AppLaunchMonitor when source is a
//     process-start event; left zero otherwise).
//   * `confidence` -- a [0,1] float indicating how likely this event is
//     user-initiated. Producers populate it from per-PID-path scoring, ETW
//     IRP class, foreground correlation, and rate-limit state. Consumers
//     (InferenceEngine) weight roll-up counters by this value rather than
//     treating every row as a unit count.
//   * `createdWindowMs` -- elapsed ms between a process-start event and the
//     first top-level window we observed for that PID. Populated by
//     AppLaunchMonitor's window-correlation hook; 0 means "no window yet".
//
// Producers (FileMonitor, AppLaunchMonitor, BrowsingMonitor, ForegroundMonitor)
// populate the struct via CaptureContext() and pass it to the user callback.
// All fields default to "unknown / neutral" so that callers that have not been
// updated yet still produce valid records.
// -----------------------------------------------------------------------------

#include <windows.h>
#include <string>
#include <cstdint>

struct EventContext
{
    DWORD        sourcePid       = 0;     // PID that actually emitted the event
    std::wstring sourceExe;               // full path to that PID's exe
    DWORD        foregroundPid   = 0;     // PID owning the foreground HWND
    std::wstring foregroundExe;           // full path of foreground PID's exe
    DWORD        msSinceInput    = 0xFFFFFFFFu; // 0xFFFFFFFF == unknown
    DWORD        parentPid       = 0;     // populated for process-start events
    std::wstring parentExe;               // exe path of parentPid
    DWORD        createdWindowMs = 0;     // ms from process-start to first HWND
    double       confidence      = 1.0;   // [0,1] producer-assigned weight
};

namespace EventContextUtil
{
    // Resolve a PID's full image path. Returns empty wstring on failure.
    // Cached internally with a bounded LRU so repeated calls are cheap.
    std::wstring GetExePathByPid(DWORD pid);

    // Drop a cached entry (call from AppLaunchMonitor on ProcessStop so we
    // don't return stale paths after a PID is recycled).
    void ForgetPid(DWORD pid);

    // Look up the process that created `pid`. Returns 0 on failure. Uses
    // NtQueryInformationProcess(ProcessBasicInformation) under the hood and
    // is O(1) -- prefer this over Toolhelp32 snapshots.
    DWORD GetParentPid(DWORD pid);

    // Build an EventContext for a freshly-observed event whose source PID is
    // `sourcePid`. If `sourcePid == 0` or unresolvable, the source* fields are
    // left empty. Always populates foreground* and msSinceInput.
    EventContext CaptureContext(DWORD sourcePid);

    // Record the current foreground HWND/PID. The ForegroundChangeBroker
    // (added in a later commit) will call this on every WinEvent foreground
    // change so CaptureContext() doesn't have to call GetForegroundWindow()
    // itself (which is allowed only from the calling thread's desktop).
    void NotifyForegroundChanged(HWND hwnd, DWORD pid);

    // IdleDetector calls this on resume from sleep / long-idle wake. All
    // events captured before `untilTickMs` will have their confidence
    // multiplied by 0.2 to absorb the burst of system-housekeeping
    // activity that fires in the seconds after wake (SuperFetch,
    // Defender, indexer, sync clients, etc).
    void SetWakeBoundary(ULONGLONG untilTickMs);
}

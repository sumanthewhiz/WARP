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
//   * Optional parent-PID/exe (filled by LaunchCapture for process-start
//     events; left zero otherwise).
//   * `confidence` -- a [0,1] float indicating how likely this event is
//     user-initiated. Producers start from CaptureContext() and multiply in
//     ScoreEvent() (source class, action, foreground match, input recency,
//     wake boundary). Consumers (EntityStore engagement, ContextInference)
//     weight evidence by this value rather than treating every row as a unit.
//   * `createdWindowMs` -- elapsed ms between a process-start event and the
//     first user-facing top-level window observed for that PID. Populated by
//     LaunchCapture's window correlation; 0 means "no window yet".
//
// Producers (FileCapture, LaunchCapture, WebCapture, FocusTracker) populate the
// struct via CaptureContext() and attach it to the record they hand to the store.
// All fields default to "unknown / neutral".
// -----------------------------------------------------------------------------

#include <windows.h>
#include <string>
#include <cstdint>
#include "Evidence.h"

struct EventContext
{
    ObservationEvidence evidence;
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
    // Cached internally with a bounded LRU so repeated calls are cheap. Failed
    // lookups are cached only briefly (exited / protected processes, FILE-9).
    std::wstring GetExePathByPid(DWORD pid);

    // Drop a cached entry (LaunchCapture calls this on process stop so we
    // don't return stale paths after a PID is recycled).
    void ForgetPid(DWORD pid);

    // Look up the process that created `pid`. Returns 0 on failure. Uses the
    // process table first, then NtQueryInformationProcess(ProcessBasicInformation).
    DWORD GetParentPid(DWORD pid);

    // Build an EventContext for a freshly-observed event whose source PID is
    // `sourcePid`. If `sourcePid == 0` or unresolvable, the source* fields are
    // left empty. Always populates foreground* and msSinceInput. `confidence` starts
    // at 1.0 and is multiplied by 0.2 inside a wake boundary; producers then call
    // ScoreEvent() which MULTIPLIES into it (SIG-4: never assign).
    EventContext CaptureContext(DWORD sourcePid);
    EventContext CaptureContextAt(DWORD sourcePid, int64_t eventMs, bool replayed = false);
    std::string ProcessInstanceId(DWORD pid);
    void SampleContext();

    // Record the current foreground HWND/PID (called by ForegroundChangeBroker).
    void NotifyForegroundChanged(HWND hwnd, DWORD pid);

    // PresenceMonitor calls this on resume from sleep. All events captured
    // before `untilTickMs` (GetTickCount64 units) get their confidence multiplied
    // by 0.2 to absorb the burst of system housekeeping after wake.
    void SetWakeBoundary(ULONGLONG untilTickMs);

    // Milliseconds since the last keyboard/mouse input (0xFFFFFFFF if unknown).
    DWORD MsSinceInput();

    // ---- Process table (FILE-9, SIG-6, RUN-3) ------------------------------
    // Maintained from Kernel-Process start/stop events (LaunchCapture) so that
    // attribution survives process exit and PID reuse is detected via the start key.
    struct ProcessInfo
    {
        DWORD        pid = 0;
        DWORD        parentPid = 0;
        DWORD        sessionId = 0xFFFFFFFFu;
        ULONGLONG    startKey = 0;        // ETW process start key / creation time
        int64_t      startMs = 0;         // UTC ms
        int64_t      exitMs = 0;          // 0 while running
        std::wstring imagePath;           // DOS path
    };
    void RegisterProcess(const ProcessInfo& info);
    void MarkProcessExited(DWORD pid, int64_t exitMs);
    bool LookupProcess(DWORD pid, ProcessInfo& out);

    // The Windows session WARP runs in; events from other sessions belong to other
    // users / desktops and must be dropped (RUN-3, SIG-5).
    DWORD OwnSessionId();
    bool  IsOwnSession(DWORD sessionId);
    bool  IsOwnSessionPid(DWORD pid);        // resolves the PID's session (table, then ProcessIdToSessionId)

    // ---- Confidence model (SIG-3) -------------------------------------------
    enum class EventKind   { FileOpen, FileEdit, FileCreate, FileDelete, FileRename, Download, AppLaunch, Browse, Focus };
    enum class SourceClass { Etw, Usn, Rdcw, Shell, Recent, Mru, Uia, Focus };
    struct ScoreHints
    {
        bool sourceIsSystem = false;      // SystemProcessClassifier verdict for the source process
        bool sourceIsSyncClient = false;  // OneDrive / Dropbox / Google Drive / Box ... ("external change")
        bool sourceIsShellInfra = false;  // Explorer thumbnailing / indexer / preview handlers / AV
        bool userVisibleWindow = false;   // AppLaunch: a visible top-level window appeared for the process
    };
    // Returns ctx.confidence * score, where score reflects: source class, foreground
    // match (source app == foreground app), input recency, attribution, process role
    // and event kind. Result clamped to [0.02, 1].
    double ScoreEvent(const EventContext& ctx, EventKind kind, SourceClass source, const ScoreHints& hints = ScoreHints());
}

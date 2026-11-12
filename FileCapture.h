#pragma once

// FileCapture -- file-activity capture for WARP (replaces FileMonitor).
//
// Sources (each enabled per Settings; see FileCapture.cpp for the details):
//   * "etw"    Microsoft-Windows-Kernel-File in a private, per-session real-time session
//              (L"WARP-KernelFile-<session id>"). Create (12) is correlated by Irp with
//              OperationEnd (24): only successful, non-directory creates are used and the
//              create disposition decides OPEN / CREATE / MODIFY. DeletePath (26) and
//              RenamePath (27) are confirmed the same way. The WRITE keyword is NOT enabled.
//              Events are decoded with TDH by property name and filtered to WARP's own
//              Terminal Services session (EVENT_ENABLE_PROPERTY_TS_ID).
//   * "usn"    NTFS/ReFS change journal (UsnJournalReader): CREATE / MODIFY / DELETE / RENAME
//              on fixed volumes, attributed to the process that recently opened the path
//              (from ETW), with downtime backfill through the cursor store.
//   * "rdcw"   ReadDirectoryChangesW on removable and network drives only.
//   * "recent" Shell Recent Items (.lnk) = explicit user OPEN.
//   * "mru"    Office MRU lists = explicit user OPEN (OneDrive/SharePoint URLs mapped to
//              their synced local path when possible).
// Renames are decided on the destination (atomic saves become MODIFY, browser download
// completions DOWNLOAD, moves to the Recycle Bin DELETE), repeated saves are coalesced and
// downloads carry their Mark-of-the-Web origin URL.
//
// Threading: the ETW consumer thread only decodes, pairs start/completion events by Irp and
// enqueues; one worker thread does path resolution, filtering, attribution, scoring,
// coalescing and invokes the Callback. Polling sources apply back-pressure instead of dropping.
// All public methods are thread-safe. Do not call Stop() or SetCallback(), or destroy the
// object, from inside the Callback.

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "ActivityStore.h"

class FileCapture
{
public:
    using Callback = std::function<void(FileEventRecord&& rec)>;

    struct Stats
    {
        uint64_t etwEvents = 0, usnEvents = 0, rdcwEvents = 0, recentEvents = 0, mruEvents = 0,
                 emitted = 0, filtered = 0, queueDropped = 0, etwEventsLost = 0, etwBuffersLost = 0;
        int64_t  lastEventMs = 0;
        bool     etwRunning = false, usnRunning = false, recentRunning = false, mruRunning = false;
        std::wstring etwSessionName, lastError;

        // Additional diagnostics.
        uint64_t rdcwOverflows = 0;       // ReadDirectoryChangesW buffer overflows (changes lost)
        uint64_t etwOtherSession = 0;     // Kernel-File events dropped: other Windows session
        uint64_t etwUnresolved = 0;       // device paths that could not be mapped to a DOS path
        uint64_t etwRealTimeLost = 0;     // in-band "real-time events/buffers lost" notifications
        bool     etwUnconfirmed = false;  // degraded mode: OperationEnd correlation unavailable
        size_t   queueDepth = 0;
        size_t   rdcwRoots = 0;
        std::wstring usnStatus;
    };

    FileCapture();
    ~FileCapture();

    FileCapture(const FileCapture&) = delete;
    FileCapture& operator=(const FileCapture&) = delete;

    void SetCallback(Callback cb);
    void SetCloudCallback(std::function<void(BrowseEventRecord&&)> cb);

    // Persist USN cursors (WARP stores them in its DB meta table as strings keyed by volume).
    // Keys are the volume GUID paths reported by UsnJournalReader; the Recent-items and
    // Office-MRU high-water marks are stored through the same functions under the keys
    // L"recent-items" and L"office-mru". Both functions may be called from internal threads.
    void SetCursorStore(std::function<bool(const std::wstring& volume, std::string& blob)> load,
                        std::function<void(const std::wstring& volume, const std::string& blob)> save);

    bool Start();                 // starts every enabled source per Settings; returns false only if nothing could start
    void Stop();                  // prompt, joins all threads
    bool IsRunning() const;       // Start() succeeded and Stop() has not run (Heartbeat restarts dead sources meanwhile)
    void SetPaused(bool paused);  // PRIVACY pause only: events are read and DISCARDED (USN cursor still advances so nothing is replayed later). Never used for idle.
    void Heartbeat();             // called every ~30 s by a supervisor: restart a dead ETW consumer with backoff, poll ControlTraceW(EVENT_TRACE_CONTROL_QUERY) for EventsLost/RealTimeBuffersLost, refresh RDCW roots (newly mounted removable/network drives) and the device map
    Stats GetStats() const;

    // Recently opened/created/modified USER files attributed to `pid`, newest first (used to resolve which document a window shows).
    std::vector<std::wstring> RecentPathsForPid(DWORD pid, DWORD maxAgeMs, size_t max = 8) const;
    // Recent explicit user opens (Recent .lnk / Office MRU), newest first.
    std::vector<std::wstring> RecentExplicitOpens(DWORD maxAgeMs, size_t max = 32) const;

    // ---- Path rules (pure, data-driven; exposed for unit tests) -------------------------
    enum class PathClass { User, Transient, Excluded };

    // Safe-save / lock / partial-download names: ~$x, ~WRL*/~WRD*/~WRS*, ~RF*, ~DF*, *.tmp,
    // *.temp, *.crdownload, *.partial, *.part, *.opdownload, *.download, *.swp/*.swo/*.swx,
    // *.bak, *~, *.~tmp, .~lock.*#, .#*, Excel 8-hex extensionless names, JetBrains
    // *___jb_tmp___/*___jb_old___, *.sb-*. Accepts a bare file name or a full path (the last
    // component is tested).
    static bool IsTransientName(const std::wstring& nameOrPath);
    // Browser download temporaries (.crdownload, .partial, .opdownload, .download).
    static bool IsDownloadTempName(const std::wstring& nameOrPath);
    // src -> dst completes a download: src is a download temp, or Firefox-style "<dst>.part".
    static bool IsDownloadRename(const std::wstring& srcPath, const std::wstring& dstPath);
    // Path lies in a Recycle Bin ($Recycle.Bin, RECYCLER, RECYCLED).
    static bool IsRecycleBinPath(const std::wstring& path);
    // Built-in exclusions for a DOS/UNC path: anchored to resolved known folders (Windows,
    // Program Files, ProgramData, AppData, temp), volume-root system folders, tool/VCS
    // dot-directories, build outputs, mailbox/database stores and system files. Transient
    // names are excluded too. Settings (user/policy) exclusions are NOT applied here.
    static bool IsExcludedPath(const std::wstring& dosPath);
    // Transient (by name) > Excluded (IsExcludedPath) > User.
    static PathClass ClassifyPath(const std::wstring& dosPath);

    enum class RenameOutcome { Drop, Rename, ModifyDest, DownloadDest, CreateDest, DeleteSource };
    // Rename decided on the destination (FILE-2):
    //   dst in Recycle Bin: user src -> DeleteSource, else Drop
    //   user -> user Rename | user -> transient Drop (safe-save step) | user -> excluded DeleteSource
    //   transient -> user ModifyDest (DownloadDest when isDownloadRename)
    //   excluded -> user CreateDest (caller upgrades to DOWNLOAD from the Zone.Identifier)
    //   anything else Drop
    static RenameOutcome DecideRename(PathClass src, PathClass dst, bool dstInRecycleBin, bool isDownloadRename);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

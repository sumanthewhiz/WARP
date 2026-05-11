# WARP - Windows Activity Reasoning Platform

WARP is a lightweight Windows desktop application that silently monitors file/folder
activity, application launches, foreground app focus (with window titles and dwell
time), and browsing activity on the local PC, stores everything in a rolling 30-day
on-disk database, and exposes a queryable named-pipe API so that other applications
running on the same machine can programmatically retrieve activity history.

The application starts minimized to the system tray (notification area), requires
administrator privileges, and is designed to run continuously in the background for as
long as the PC is actively being used.

---

## Features

| Feature | Details |
|---|---|
| **System-tray residence** | Launches hidden; a light-bulb tray icon provides *Open* and *Exit* right-click menu items. Double-clicking the icon opens the window maximized. |
| **Administrator privileges** | The linker UAC setting requests `requireAdministrator`, so a UAC prompt is shown on launch. |
| **Modern themed UI** | Dark-on-light default theme with Segoe UI / Cascadia Mono fonts, owner-drawn buttons with rounded corners and accent borders, and a one-click **Dark Mode / Light Mode** toggle in the top-right corner. |
| **Built-in API test panel** | 9 predefined time-window buttons, a custom-seconds input field, and a default-query button -- all wired to the named-pipe API. Responses are pretty-printed as indented JSON in a scrollable monospace area. |
| **Event type filter checkboxes** | Four checkboxes (**File Activity**, **App Launches**, **App Focus**, **Browsing Activity**) control which event types are included in query results. All are checked by default. |
| **Two-tier idle / sleep awareness** | A *soft* threshold (default 2 min) downgrades event confidence; a *hard* threshold (default 5 min) pauses monitoring outright. Sleep / hibernate transitions trigger an immediate hard pause. On wake (and for 5 s after), `EventContext::SetWakeBoundary()` multiplies the producer's confidence by 0.2 to attenuate the burst of background I/O that follows resume. |
| **File-system monitoring** | Removable and network drives are watched recursively via `ReadDirectoryChangesW`. **Fixed drives no longer use RDCW** — they are covered exclusively by the ETW path (below) to eliminate the duplicate-event firehose that user-mode RDCW produced for system / cache / build directories. All paths are filtered through a comprehensive **goop exclusion list** (12 categories, 130+ patterns including Docker / WSL / OneDrive / Dropbox) and an explicit **AppData re-admit allowlist** (OneNote, Outlook, Sticky Notes, Visual Studio, Office UnsavedFiles, Notepad++ backup) that is checked *before* goop matching so genuine user files inside `\AppData\` survive. |
| **Shell-level monitoring** | `SHChangeNotifyRegister` on the entire shell namespace catches higher-level operations (copy, move, shell renames) that pure file-system notifications may miss. The same goop path exclusion is applied. |
| **ETW file monitoring (private session)** | A dedicated `WARP-FileTrace` private ETW session attaches to the manifested **Microsoft-Windows-Kernel-File** provider (no longer the shared NT Kernel Logger), so WARP cannot be starved by another consumer turning the global session off. Each event is enriched with the multi-signal `SystemProcessClassifier` (parent ancestry, image path, Authenticode subject, session, integrity, name pattern) and a per-PID **token bucket** (64 tokens / sec) — events that drain the bucket are downgraded to confidence 0.1 instead of being recorded as full-weight user activity. |
| **App launch monitoring (ETW + window correlation)** | New process launches arrive on a `WARP-ProcessTrace` private session attached to **Microsoft-Windows-Kernel-Process** (the 2 s polling loop is gone). Every PID is parked with `LaunchCorrelator`, which uses a `SetWinEventHook(EVENT_OBJECT_CREATE)` to wait up to 5 s for the launched process to create its first top-level visible window. Headless launches (services, COM surrogates, scheduled tasks, build steps) miss the window deadline and are emitted with confidence 0.3 — visible launches get confidence 1.0 and a `created_window_ms` value. The same `SystemProcessClassifier` runs in parallel as a redundant veto. |
| **Browsing activity monitoring** | The foreground window is no longer polled. `BrowsingMonitor` subscribes to `ForegroundChangeBroker`; when focus enters a recognised browser, a scoped per-PID `SetWinEventHook(EVENT_OBJECT_NAMECHANGE, …, pid, threadId, …)` is installed that fires on the actual title-bar update. URLs are extracted by `UrlExtractor` (UIA, MTA worker thread, bounded queue) so a hung browser process cannot block the message pump. |
| **Foreground app monitoring (event-driven)** | A single global `SetWinEventHook(EVENT_SYSTEM_FOREGROUND)` lives in `ForegroundChangeBroker` and fans out to every interested monitor. The 1 Hz `GetForegroundWindow()` poll is removed. When focus changes, the previous session's exe name, exe path, window title, and dwell time (seconds) are recorded; system / shell processes are excluded via the same `SystemProcessClassifier`. |
| **SQLite storage with EventContext** | All events are persisted in `%LOCALAPPDATA%\WARP\activity.db` using WAL mode across four tables. Every row carries a uniform **EventContext** payload — `source_pid`, `source_exe`, `foreground_pid`, `foreground_exe`, `ms_since_input`, `parent_pid`, `parent_exe`, `created_window_ms`, and `confidence` (REAL, default 1.0) — so downstream consumers can filter or weight events by the producer's user-intent estimate. Migration is idempotent (`ALTER TABLE … ADD COLUMN`); upgrading from an older WARP install does not require a fresh DB. |
| **30-day rolling eviction** | Records older than 30 days are deleted on startup and every 6 hours thereafter from all tables. Rolling window counts (`open_count_7d`, `open_count_30d`) in the inference engine are also recomputed on this schedule as `SUM(COALESCE(confidence, 1.0))` so noisy events contribute proportionally less to popularity ranking. |
| **Named-pipe query API** | Other Windows processes can connect to `\\.\pipe\WarpFileActivityAPI` and retrieve activity data as JSON for any supported time window, optionally filtered by event type. |
| **Confidence-weighted inference engine** | Every captured event incrementally updates per-entity inference records (files, apps, URLs) with open/edit timestamps and an exponential-decay recency score. Counters are accumulated by the producer's `confidence` (a REAL value in [0, 1]) rather than by `1`, so a stream of 10 events at confidence 0.1 contributes the same weight as one full-confidence event instead of being dropped wholesale by a hard threshold. JSON output rounds to integer via `llround()` so the documented integer `open_count_*` API contract still holds. Two dedicated API operations (`QueryInferences`, `GetInferenceDeltas`) let client apps retrieve these precomputed insights without scanning raw events. |
| **Inference explorer UI** | A built-in "Explore Precomputed Inferences" panel lets you browse the top-N entities ranked by recency score, filtered by entity type (Files / Apps / URLs), and look up the inference record for any specific path or URL -- all without leaving the WARP window. |
| **Semantic topic inference** | Every 5 minutes, all activities from the last 15-minute window are gathered, and a local **all-MiniLM-L6-v2** sentence-embedding model (run via ONNX Runtime) computes 384-dimensional vector embeddings for each activity's descriptive text. Each embedding is matched to the closest topic from ~50 pre-embedded candidate labels via cosine similarity. A greedy set-cover then selects the **top 3 semantic topics** that collectively explain ≥ 90 % of recent activities. Results are stored with timestamps and retrievable via the **Show Recent Context** button or the `GetRecentContext` API operation. |

---

## Event Types

WARP captures three categories of events:

| Event Type | Description | Source |
|---|---|---|
| **File Activity** | User-initiated file/folder creates, opens, modifications, deletes, renames. System/goop paths excluded; per-PID classifier and token-bucket downgrade noisy producers. | ETW `Microsoft-Windows-Kernel-File` (private session) for fixed drives; `ReadDirectoryChangesW` for removable / network; `SHChangeNotifyRegister` |
| **App Launch** | Process creation events correlated against the appearance of a top-level visible window (5 s deadline). Headless launches downgraded to confidence 0.3. | ETW `Microsoft-Windows-Kernel-Process` (private session) + `LaunchCorrelator` (`SetWinEventHook(EVENT_OBJECT_CREATE)`) |
| **App Focus** | Foreground application sessions with window title and dwell time (seconds). System/shell processes excluded by `SystemProcessClassifier`. | `ForegroundChangeBroker` (single global `EVENT_SYSTEM_FOREGROUND` hook) |
| **Browsing Activity** | Browser page title changes (browser name, page title, URL). URL extracted via UI Automation, not parsed from the title bar. | `ForegroundChangeBroker` + scoped per-PID `EVENT_OBJECT_NAMECHANGE` hook + `UrlExtractor` (UIA on MTA worker thread) |

---

## Architecture

```
+----------------------------------------------------------------------------+
|                              WARP!.cpp                                     |
|                        (Win32 entry point + UI)                            |
+----------------------------------------------------------------------------+

  Producers (run on dedicated threads / ETW callbacks)
  ----------------------------------------------------
   FileMonitor        AppLaunchMonitor   BrowsingMonitor    ForegroundMonitor
   --------------     ---------------    ---------------    -----------------
   ETW Kernel-File    ETW Kernel-Process broker subscriber  broker subscriber
   (private session,  (private session)  + per-PID NAME-    (event-driven)
    fixed drives)     + LaunchCorrelator CHANGE hook
   RDChangesW         (window-create     + UrlExtractor
   (rm / net only)     hook, 5s window)   (UIA, MTA worker
   SHChangeNotify                          thread, bounded
                                           queue)

   Each producer emits an event + a populated EventContext:
     {sourcePid, sourceExe, foregroundPid, foregroundExe,
      msSinceInput, parentPid, parentExe, createdWindowMs, confidence}

  Cross-cutting infrastructure
  ----------------------------
   ForegroundChangeBroker     SystemProcessClassifier   IdleDetector
   ----------------------     -----------------------   ------------
   single global              multi-signal voting:      two-tier:
   EVENT_SYSTEM_FOREGROUND    parent ancestry,          soft -> attenuate
   hook fans out to           image path,                       confidence
   subscribers                Authenticode subject,     hard -> pause
                              session, integrity,       monitors
                              name pattern              + wake boundary
                                                        (5s, x0.2 conf)

  Storage + analytics
  -------------------
   InferenceEngine -> ActivityDatabase -> SQLite (activity.db)
   ---------------    -----------------    ---------------------
   counters +=        4 activity tables    %LOCALAPPDATA%\WARP\
   confidence         + uniform Event-     activity.db (WAL mode)
   recency =          Context columns
   e^(-Δt/τ) + ...    + inference table

  Query surface
  -------------
   QueryApi -> Named pipe \\.\pipe\WarpFileActivityAPI
   --------    -----------------------------------------
   serves event queries, QueryInferences, GetInferenceDeltas,
   and GetRecentContext

  Semantic layer (independent)
  ----------------------------
   TopicInference (BertTokenizer + MiniLM ONNX)
   ---------------------------------------------
   every 5 min: embed last 15 min of activities -> cosine match
   against ~50 pre-embedded topic labels -> greedy set-cover for
   top 3 topics covering >= 90% of activities
```

### Component overview

#### `WARP!.cpp` -- Application shell & UI

The Win32 entry point. Creates the main window (opened maximized when restored from
tray) with a modern themed layout:

- **Header row** -- status label on the left, theme toggle and clear history buttons
  on the right, separated from the body by an accent-colored horizontal line.
- **Predefined query section** -- 9 owner-drawn buttons ("Last 15 min" through
  "Last 30 days") that wrap responsively based on window width.
- **Custom seconds section** -- a numeric edit field and "Send Custom Query" button.
- **Default query section** -- a single button that sends an empty `{}` request
  (returns last 1 hour).
- **Event type filter** -- four checkboxes (**File Activity**, **App Launches**,
  **App Focus**, **Browsing Activity**) that control which event types are queried.
  All checked by default. The selected types are sent as a `"types"` array in the
  JSON request.
- **Inference exploration section** -- labelled "Explore Precomputed Inferences":
  - *Entity type filter* -- three checkboxes (**Files**, **Apps**, **URLs**) to
    select which entity types to include. All checked by default.
  - *Top N* -- a numeric input (default 50) and a **Show Top Inferences** button
    that fetches all inference records via `GetInferenceDeltas`, filters by the
    selected entity types, sorts by `recency_score` descending, truncates to the
    top N, and formats a human-readable report with rank, type, score, open counts
    (7d / 30d / total), entity key, and local-time timestamps.
  - *Lookup* -- a text field with cue banner "Enter path or URL to look up..." and
    a **Lookup** button that sends a `QueryInferences` request for the entered
    entity key and displays the pretty-printed JSON result.
- **Response area** -- a read-only, scrollable, monospace (`Cascadia Mono`) edit
  control that displays pretty-printed JSON responses or the inference explorer
  output.

Each query button spawns a background thread that connects to
`\\.\pipe\WarpFileActivityAPI`, sends the appropriate JSON request, reads and
pretty-prints the response, then marshals it back to the UI thread via a custom
`WM_USER` message -- so the interface never freezes.

The UI supports two themes (**light** and **dark**) switchable at runtime via the
toggle button. All colors are defined in a `Theme` struct; switching recreates the
GDI brushes and issues a full `RedrawWindow` with `RDW_ALLCHILDREN`.

Closing or minimizing the window hides it back to the tray; only **Exit** from the
tray menu terminates the process.

#### `FileMonitor` (`FileMonitor.h` / `FileMonitor.cpp`)

Composite file-activity producer that reconciles three sources:

1. **ETW (`Microsoft-Windows-Kernel-File`, private session)** — owns *fixed*
   drives. The monitor opens its own `WARP-FileTrace` private session via
   `StartTraceW` + `EnableTraceEx2` so a misbehaving global consumer cannot
   starve it (older WARP shared the NT Kernel Logger, which any other tool
   could disable). Each event arrives on the consumer thread, is enriched
   with an `EventContext` (source/foreground/parent IDs, `ms_since_input`,
   `confidence`), then run through `SystemProcessClassifier` (cached
   per-PID) and a per-PID **token bucket** of 64 tokens / sec. Events that
   classify as system are dropped; events that drain the bucket are
   downgraded to confidence 0.1 instead of being dropped — so a real burst
   of user saves (e.g. a build finishing) still survives, just at lower
   weight.
2. **`ReadDirectoryChangesW`** — kept *only* for removable and network
   drives. The previous design ran RDCW on every fixed drive too, which
   double-counted every event with the ETW path and forced WARP to do all
   filtering in user mode. Fixed-drive RDCW is now removed.
3. **`SHChangeNotifyRegister`** — kept for shell-level operations (copy,
   move, shell renames) that the file-system layer alone cannot
   reconstruct.

Detected actions: **CREATE**, **OPEN**, **DELETE**, **MODIFY**, **RENAME**.

**Goop folder exclusion + AppData allowlist (all three paths):**

Every file path from all three sources is passed through `ShouldExclude()`,
which now (a) applies an explicit **AppData re-admit allowlist** *before*
goop matching so genuine user files inside `\AppData\` survive, then
(b) runs the goop filter. The allowlist covers OneNote (`OneNote\\…\.one`),
Outlook OST/PST under `\AppData\Local\Microsoft\Outlook\`, Sticky Notes
(`Microsoft.MicrosoftStickyNotes_*\…`), Visual Studio user settings,
Office UnsavedFiles, and Notepad++ backup. The goop filter itself
implements 12 categories:

| Category | Examples |
|---|---|
| **1. Windows OS directories** | `\Windows\` and all subdirectories |
| **2. Program Files** | `\Program Files\`, `\Program Files (x86)\` |
| **3. ProgramData** | `\ProgramData\` (entire tree) |
| **4. AppData** | `\AppData\` (entire tree -- caches, browser data, runtime state) |
| **5. Per-volume system folders** | `\System Volume Information\`, `\$Recycle.Bin\`, `\Recovery\`, `\$WINDOWS.~BT\`, `\$WINDOWS.~WS\`, `\$WINDOWS.~Q\`, `\$WinREAgent\`, `\$GetCurrent\`, `\$SysReset\`, `\Config.Msi\`, `\MSOCache\`, `\PerfLogs\`, `\DfsrPrivate\`, `\found.000\`–`\found.999\` (CHKDSK fragments) |
| **6. Developer toolchain** | `\node_modules\`, `\__pycache__\`, `\obj\`, `\bin\Debug\`, `\bin\Release\`, `\target\`, `\bower_components\`, `\CMakeFiles\`, `\Pods\`, `\venv\`, `\env\`, `\coverage\`, plus **all dot-prefixed folders** (`.git`, `.vs`, `.vscode`, `.cargo`, `.gradle`, `.nuget`, `.npm`, `.yarn`, `.conda`, etc.) |
| **7. User profile shell junctions** | `\Application Data\`, `\Local Settings\`, `\Cookies\`, `\NetHood\`, `\PrintHood\`, `\Recent\`, `\SendTo\`, `\Start Menu\`, `\Templates\`, `\My Documents\`, `\MicrosoftEdgeBackups\`, `\IntelGraphicsProfiles\` |
| **8. App caches by name pattern** | `\Cache\`, `\CacheStorage\`, `\Code Cache\`, `\GPUCache\`, `\DawnCache\`, `\GrShaderCache\`, `\ShaderCache\`, `\Crash Reports\`, `\CrashDumps\`, `\Crashpad\`, `\blob_storage\`, `\IndexedDB\`, `\Local Storage\`, `\Session Storage\`, `\Service Worker\`, `\WebStorage\`, `\databases\`, `\Logs\`, `\Log\`, `\Temp\`, `\Tmp\` |
| **9. OS upgrade/recovery** | `\Windows.old\`, `\OneDriveTemp\`, `\inetpub\` |
| **10. NTFS metadata** | `$MFT`, `$MFTMirr`, `$LogFile`, `$Bitmap`, `$Boot`, `$BadClus`, `$Secure`, `$UpCase`, `$AttrDef`, `$Extend` |
| **11. Container / WSL backing stores** | `\docker-desktop-data\`, `\wsl\…\ext4.vhdx`, `\Hyper-V\Virtual Machines\`, `\Containers\BaseImages\` |
| **12. Cloud-sync staging dirs** | `\OneDrive\…\.849C9593-…`, `\Dropbox\.dropbox.cache\`, `\Box\…\.box\`, `\Google\DriveFS\`, `\iCloudDrive\…\.icloud` |

Additional file-level exclusions: 49+ file extensions (`.exe`, `.dll`, `.sys`,
`.tmp`, `.log`, `.pdb`, `.dat`, etc.), system filenames (`pagefile.sys`,
`hiberfil.sys`, `NTUSER.DAT*`, `bootmgr`, etc.), Office lock files (`~$...`), and
temp-file prefixes (`~...`).

> **Note:** The goop exclusion applies **only to file activities**. App launch and
> browsing monitors are not affected by these path filters.

The monitor exposes `Pause()` / `Resume()` methods that the idle detector calls to
suspend event recording when the PC is hard-idle.

#### `AppLaunchMonitor` (`AppLaunchMonitor.h` / `AppLaunchMonitor.cpp`)

Subscribes to the **Microsoft-Windows-Kernel-Process** ETW provider via a
private `WARP-ProcessTrace` session, so every process create event is
delivered the moment the kernel emits it (no more 2 s `CreateToolhelp32Snapshot`
poll, which raced against short-lived processes that started and exited
between samples).

For each new PID the monitor:

1. Builds an `EventContext` from the source/parent metadata that the ETW
   record carries directly — no separate `OpenProcess` round trip needed.
2. Hands the PID to `LaunchCorrelator`, which "parks" it for up to 5 s
   while listening on `SetWinEventHook(EVENT_OBJECT_CREATE)` for the
   process's first top-level visible window. If a window appears the
   launch is emitted with `created_window_ms` populated and confidence
   1.0; if the deadline expires the launch is still emitted but with
   confidence 0.3 so headless launches (services, COM surrogates,
   scheduled-task workers, build-step children) ride at a fraction of
   the weight of user-visible launches instead of contaminating the
   "what apps did the user run" picture.
3. Runs the same `SystemProcessClassifier` used by `FileMonitor` as a
   redundant veto so well-known service ancestries are dropped even
   if they momentarily race a window onto the screen.

Every detected launch reports the executable name, full path, PID, and
the populated `EventContext`.

#### `BrowsingMonitor` (`BrowsingMonitor.h` / `BrowsingMonitor.cpp`)

Subscribes to `ForegroundChangeBroker`. When the foreground process is a
recognised browser, the monitor installs a **scoped** per-PID
`SetWinEventHook(EVENT_OBJECT_NAMECHANGE, …, pid, threadId, …)` so it
fires only when *that browser's* title bar updates — and tears the hook
down the moment focus leaves. This avoids the global NAMECHANGE
firehose (every IME composition, status-bar tick, and tooltip change in
the system) that a process-agnostic hook would deliver.

URLs are not parsed from the title bar (which is unreliable and locale-
dependent). Instead the monitor enqueues a request to `UrlExtractor`,
which uses **UI Automation** on a dedicated MTA worker thread to read
the actual address-bar `Value` property out of the browser. Because
UIA crosses process boundaries (and a hung browser can hang any UIA
caller), the extractor uses a bounded queue (32 entries; oldest dropped
on overflow) so the foreground broker thread never blocks.

Supported browsers: **Chrome**, **Edge**, **Firefox**, **Brave**,
**Opera**, **Vivaldi**, **Internet Explorer**.

Processing:
- Common browser suffixes ("- Google Chrome", "- Microsoft Edge", etc.) are stripped
  to extract the clean page title.
- The same `(browser, title)` combination is reported only once until the
  page changes.

#### `ForegroundMonitor` (`ForegroundMonitor.h` / `ForegroundMonitor.cpp`)

Subscribes to `ForegroundChangeBroker`. The previous 1 Hz
`GetForegroundWindow()` poll is gone — the broker delivers a
notification the moment Windows changes the foreground, with no
per-second wakeup cost when the user isn't actively switching apps.

When the foreground changes (different PID or different window title),
the previous session is emitted with:

- **exe_name** — executable filename (e.g. `OUTLOOK.EXE`).
- **exe_path** — full path to the executable.
- **window_title** — the window title that was active during the session.
- **duration_secs** — how many seconds the app was in the foreground.

Sessions shorter than 3 seconds are discarded. System / shell processes
are excluded via `SystemProcessClassifier` rather than a hard-coded
name list.

This monitor provides the **app usage context** (window titles and dwell time)
required by user-context scenarios such as improving search relevance based on
current app activity, inferring user intent from window titles, and tracking app
usage frequency and duration patterns over time.

#### `IdleDetector` (`IdleDetector.h` / `IdleDetector.cpp`)

A polling thread that checks `GetLastInputInfo` every 5 seconds and
manages **two thresholds**:

| Threshold | Default | What happens |
|---|---|---|
| **Soft** (`SetSoftIdleThreshold`) | 2 minutes | Fires `onSoftIdle`. Monitors keep running but `EventContext.confidence` for new events is attenuated, so events that fire during idle (Windows Update, Defender scans, telemetry uploads) contribute less weight to inference and ranking. |
| **Hard** (`SetHardIdleThreshold`) | 5 minutes | Fires `onHardIdle`. All four monitors are paused outright. |

A message-only window receives `WM_POWERBROADCAST` notifications
(`PBT_APMSUSPEND` / `PBT_APMRESUMEAUTOMATIC`) so sleep / hibernate
trigger an immediate hard pause regardless of input activity. On wake,
`EventContext::SetWakeBoundary()` is armed for 5 seconds — every event
captured during that window has its confidence multiplied by 0.2 so the
post-wake burst of background I/O (driver init, Defender catch-up,
sync clients reconnecting) does not look like a flood of user activity.

#### `EventContext` (`EventContext.h` / `EventContext.cpp`)

A shared value type that travels with every event from the producing
monitor through to `ActivityDatabase` and `InferenceEngine`. Fields:

| Field | Meaning |
|---|---|
| `sourcePid` / `sourceExe` | The process that actually performed the I/O or generated the event. |
| `foregroundPid` / `foregroundExe` | The user-facing process owning the foreground window at event time. |
| `parentPid` / `parentExe` | The parent of the source process (for ancestry checks). |
| `msSinceInput` | Milliseconds since the last keyboard / mouse / touch input from `GetLastInputInfo`. |
| `createdWindowMs` | For app launches: time from process create to first top-level visible window (set by `LaunchCorrelator`). |
| `confidence` | Producer's `[0, 1]` estimate that this event is user-initiated. Default 1.0. Attenuated by the soft-idle and wake-boundary multipliers, by the per-PID token bucket, and by `LaunchCorrelator` when no window appears in time. |

`EventContext::CaptureContext(pid)` populates a context for a given source
PID at event time and applies the wake-boundary multiplier (×0.2 if the
event lands inside the 5 s window after `SetWakeBoundary()` was armed).
`ActivityDatabase::BindEventContext()` binds the nine context fields onto
any prepared statement that has the columns appended in the standard
order.

#### `LaunchCorrelator` (`LaunchCorrelator.h` / `LaunchCorrelator.cpp`)

Bridges the gap between "process was created" and "user actually saw a
window". The correlator is started on the WARP UI thread (so it can use
`SetWinEventHook(WINEVENT_OUTOFCONTEXT)`, which requires a message
pump on the calling thread) and listens for `EVENT_OBJECT_CREATE` on
top-level windows. `AppLaunchMonitor` calls `Park(pid, callback)` when a
new PID arrives; the callback fires once with `created_window_ms` set
when a visible top-level window appears for that PID, or once with
`created_window_ms = -1` after a 5 s timeout. Headless launches fall in
the latter bucket and are emitted at confidence 0.3.

#### `SystemProcessClassifier` (`SystemProcessClassifier.h` / `SystemProcessClassifier.cpp`)

Replaces the old hard-coded "is this exe name in a 65-entry list"
checks. Each unknown PID is run through six independent signals:

1. **Parent ancestry** — service host (`services.exe`, `svchost.exe`,
   `wininit.exe`) anywhere in the chain.
2. **Image path** — `\Windows\System32\`, `\WinSxS\`, `\SystemApps\`,
   `\WindowsApps\`, `\Servicing\`, `\Windows\Temp\`, etc.
3. **Authenticode subject** — read via `WinVerifyTrust` +
   `CryptMsgGetParam` and matched against Microsoft Windows
   subjects (`Microsoft Windows`, `Microsoft Windows Publisher`,
   `Microsoft Corporation` for OS-signed binaries).
4. **Session ID** — session 0 is non-interactive by definition.
5. **Token integrity level** — System / High-mandatory in a
   non-interactive session is a strong system signal.
6. **Name pattern** — well-known noise patterns
   (`*svc.exe`, `*Host.exe`, `*Broker.exe`, `*Agent.exe` plus
   the explicit MsMpEng / SearchIndexer / SIHClient / WmiPrvSE list).

Each signal contributes a vote. A configurable threshold decides whether
the PID is "system" or "user". Results are cached per-PID with a 60 s
TTL. The point of this design is robustness to a single signal being
spoofed or unavailable — e.g. an unsigned binary running from
`\Users\Public\` that nonetheless lives in the service host ancestry is
still classified as system.

> Implementation note: `CertGetNameStringW` returns `const wchar_t*` for
> the buffer, so the Authenticode reader uses a `std::vector<wchar_t>`
> rather than `std::wstring::data()`.

#### `ForegroundChangeBroker` (`ForegroundChangeBroker.h` / `ForegroundChangeBroker.cpp`)

A single global `SetWinEventHook(EVENT_SYSTEM_FOREGROUND, …,
WINEVENT_OUTOFCONTEXT, …)` installed on the WARP UI thread.
Subscribers (`ForegroundMonitor`, `BrowsingMonitor`, anything else
that needs foreground transitions) register a callback via
`Subscribe()`; the broker invokes them all in order when the
foreground window changes. The broker eliminates the previous
1 Hz `GetForegroundWindow()` poll in `ForegroundMonitor` and the
3 s poll in `BrowsingMonitor`, plus the duplicated work of both
monitors asking the same question every interval.

#### `UrlExtractor` (`UrlExtractor.h` / `UrlExtractor.cpp`)

Asynchronous UI Automation client that reads the actual URL out of
browser address bars. Why not parse the title bar? Title bars are
locale-dependent, frequently empty (e.g. on `about:blank`), and
strip query strings — the address-bar `Value` property is the
ground truth. Why a worker thread? UIA crosses process boundaries
into the browser; if the browser is hung the calling thread blocks
indefinitely. The extractor:

- Runs on a dedicated worker thread that calls
  `CoInitializeEx(MULTITHREADED)` once and creates a single
  `IUIAutomation` instance.
- Accepts requests via a bounded queue (32 entries; drops the oldest
  on overflow) so a hung browser cannot back-pressure the
  foreground broker thread.
- For each request, walks the UIA tree from the top-level browser
  window, finds the address-bar `EditControl` (Chrome / Edge:
  `automation_id == "address-bar"` or class `Chrome_OmniboxView`;
  Firefox: `automation_id == "urlbar-input"`), and reads
  `Value.Value`.

Empty / failed extractions fall back to leaving the URL field NULL so
the title-only browsing event still records.

#### `ActivityDatabase` (`ActivityDatabase.h` / `ActivityDatabase.cpp`)

A thread-safe SQLite wrapper. The database is stored at:

```
%LOCALAPPDATA%\WARP\activity.db
```

**Schema:**

The four activity tables share a uniform **EventContext** suffix —
`source_pid`, `source_exe`, `foreground_pid`, `foreground_exe`,
`ms_since_input`, `parent_pid`, `parent_exe`, `created_window_ms`,
`confidence` (REAL DEFAULT 1.0). On open, `ActivityDatabase` runs an
idempotent `ALTER TABLE … ADD COLUMN` for each context column on each
activity table, so upgrading from an older WARP install does not require
a fresh DB (SQLite returns a duplicate-column error which is silently
ignored). For brevity the DDL below shows the context columns only on
`file_activity`; the other three tables carry the same nine columns.

```sql
CREATE TABLE file_activity (
    id        INTEGER PRIMARY KEY AUTOINCREMENT,
    timestamp INTEGER NOT NULL,       -- Unix epoch seconds (UTC)
    action    TEXT    NOT NULL,        -- CREATE | OPEN | DELETE | MODIFY | RENAME
    path      TEXT    NOT NULL,        -- full path of the affected file/folder
    old_path  TEXT,                    -- previous path (RENAME only; NULL otherwise)
    -- EventContext (added to every activity table via idempotent ALTER):
    source_pid        INTEGER,
    source_exe        TEXT,
    foreground_pid    INTEGER,
    foreground_exe    TEXT,
    ms_since_input    INTEGER,
    parent_pid        INTEGER,
    parent_exe        TEXT,
    created_window_ms INTEGER,
    confidence        REAL DEFAULT 1.0
);

CREATE TABLE app_launch_activity (
    id        INTEGER PRIMARY KEY AUTOINCREMENT,
    timestamp INTEGER NOT NULL,
    exe_name  TEXT    NOT NULL,
    exe_path  TEXT    NOT NULL,
    pid       INTEGER NOT NULL
    -- + EventContext columns
);

CREATE TABLE browsing_activity (
    id        INTEGER PRIMARY KEY AUTOINCREMENT,
    timestamp INTEGER NOT NULL,
    browser   TEXT    NOT NULL,
    title     TEXT    NOT NULL,
    url       TEXT
    -- + EventContext columns
);

CREATE TABLE app_focus_activity (
    id            INTEGER PRIMARY KEY AUTOINCREMENT,
    timestamp     INTEGER NOT NULL,
    exe_name      TEXT    NOT NULL,
    exe_path      TEXT    NOT NULL,
    window_title  TEXT    NOT NULL,
    duration_secs INTEGER NOT NULL
    -- + EventContext columns
);

CREATE INDEX idx_activity_ts      ON file_activity(timestamp);
CREATE INDEX idx_activity_action  ON file_activity(action, timestamp);
CREATE INDEX idx_app_ts           ON app_launch_activity(timestamp);
CREATE INDEX idx_browse_ts        ON browsing_activity(timestamp);
CREATE INDEX idx_focus_ts         ON app_focus_activity(timestamp);
CREATE INDEX idx_file_src_pid     ON file_activity(source_pid);
CREATE INDEX idx_file_conf        ON file_activity(confidence);

-- Inference table (precomputed per-entity analytics).
-- open_count_* columns have INTEGER affinity but store REAL values
-- (SQLite's dynamic typing accepts REAL into INTEGER-affinity columns
-- losslessly; we read them back via sqlite3_column_double). They
-- accumulate the producer's `confidence` rather than counting events.
CREATE TABLE inference (
    entity_key        TEXT PRIMARY KEY,
    entity_type       TEXT,         -- 'file', 'app', or 'url'
    last_event_ts     INTEGER,
    last_open_ts      INTEGER,
    last_edit_ts      INTEGER,
    open_count_7d     INTEGER,      -- stores REAL; sum of confidence over 7 days
    open_count_30d    INTEGER,      -- stores REAL; sum of confidence over 30 days
    open_count_total  INTEGER,      -- stores REAL; lifetime sum of confidence
    recency_score     REAL,         -- 0-255 composite score
    version           INTEGER,      -- monotonic per-entity version
    updated_at        INTEGER
);

CREATE INDEX idx_inference_updated_at ON inference(updated_at);
CREATE INDEX idx_inference_version    ON inference(version);
```

**Pragmas:** `journal_mode=WAL`, `synchronous=NORMAL`.

**Eviction:** Records older than 30 days are deleted from all four event tables on
startup and every 6 hours via a `WM_TIMER`. Rolling window counts in the inference
engine (`open_count_7d`, `open_count_30d`) are also recomputed on this schedule via
`RefreshRollingCounts()`, which now `SUM(COALESCE(confidence, 1.0))` from the raw
event tables (rather than `COUNT(*)`) so the periodic resync agrees with the
per-event update path.

#### `InferenceEngine` (`InferenceEngine.h` / `InferenceEngine.cpp`)

A real-time analytics layer that maintains per-entity inference records across
three entity types: **files**, **apps**, and **URLs**.

Every time a raw event is written to the database, the corresponding monitor
callback also calls `OnFileEvent`, `OnAppLaunchEvent`, `OnAppFocusEvent`, or
`OnBrowsingEvent` on the inference engine. The engine:

1. Normalizes the entity key to lowercase UTF-8.
2. Loads or creates an `InferenceRecord` (in-memory cache backed by the `inference`
   SQLite table).
3. Adds the producer's `confidence` (a REAL value in `[0, 1]`) to the
   running counters `open_count_7d`, `open_count_30d`,
   `open_count_total`, and updates timestamps (`last_event_ts`,
   `last_open_ts`, `last_edit_ts`). The previous design used
   `(confidence >= 0.5) ? 1 : 0` and dropped low-confidence events
   entirely; the new design lets a stream of (n) events with average
   confidence c contribute (n × c) so a steady trickle of dim signal
   still surfaces in popularity ranking, just at a commensurate weight.
4. Recomputes a **recency score** using exponential decay:
   `score = 200 × e^(−Δt / 172800) + 5 × ln(1 + open_count_7d)`, capped at 255.
5. Bumps the record `version` and persists via `INSERT OR REPLACE`.

`RefreshRollingCounts()` (run on the 6-hour eviction schedule) recomputes
the rolling windows from the raw event tables as
`SUM(COALESCE(confidence, 1.0))` rather than `COUNT(*)`, so the periodic
resync agrees with the per-event update path. JSON serialisation rounds
the counters to integer via `llround()` so the documented integer
`open_count_*` API contract is preserved for existing clients.

Two query operations are exposed through the named pipe (see the
[Inference API](#5-inference-api) section below):

| Operation | Purpose |
|---|---|
| `QueryInferences` | Batch lookup of inference records by entity key, with optional field projection. |
| `GetInferenceDeltas` | Retrieve all records whose `version` exceeds a given watermark (up to 5 000 per call). |

Clearing the activity history (`ClearAll`) also clears the inference in-memory
cache via `ClearCache()`.

#### `TopicInference` (`TopicInference.h` / `TopicInference.cpp`)

A semantic topic-deduction engine that uses a local **all-MiniLM-L6-v2**
sentence-transformer model (via ONNX Runtime) to understand what the user has been
working on.

**Lifecycle:**

1. `Init(modelsDir)` -- loads `vocab.txt` into the `BertTokenizer`, creates an ONNX
   Runtime session for `minilm.onnx`, and pre-embeds ~50 candidate topic labels
   (e.g. *"C and C++ software development and programming"*, *"Email reading and
   writing correspondence"*, *"Debugging and troubleshooting software issues"*).
2. `Start(db)` / `Stop()` -- manages a background timer thread.
3. Every **5 minutes**, `DeduceTopics()` runs:
   - Gathers all file, app launch, browsing, and focus activities from the last
     **15 minutes** via `ActivityDatabase`.
   - Composes a natural-language description for each activity (e.g.
     *"Working in devenv.exe: WARP!.cpp - Visual Studio"*).
   - Embeds each description into a **384-dimensional vector** using the MiniLM
     model with mean pooling and L2 normalisation.
   - Matches each activity embedding to the **nearest topic candidate** via cosine
     similarity (dot product on normalised vectors).
   - A **greedy set-cover** selects the top 3 topics that collectively cover
     ≥ 90 % of activities.
4. Results (`TopicResult`: timestamp, 3 topic strings, coverage %, activity count)
   are stored in a rolling buffer (up to 288 entries = 24 hours).

The `GetRecentContext()` method returns the latest result as JSON (see the
[GetRecentContext API](#getrecentcontext) section below).

Because the model computes **dense semantic embeddings**, it genuinely understands
meaning: *"reviewing John's changes to the auth module"* matches *"Code review and
pull request review"* even with zero keyword overlap -- both map to nearby points in
the 384-dimensional semantic space.

#### `BertTokenizer` (`BertTokenizer.h`)

A header-only BERT WordPiece tokenizer implementation in C++. Loads `vocab.txt`
(~30 000 tokens), then:

1. **Basic tokenization** -- lowercases the input and splits on whitespace and
   punctuation.
2. **WordPiece sub-word tokenization** -- for each word, performs greedy
   longest-match lookup against the vocabulary, using `##` prefixed sub-tokens for
   continuation pieces.
3. **Framing** -- wraps the token sequence with `[CLS]` ... `[SEP]` and pads to
   the configured maximum sequence length (128 tokens).

This is the same tokenizer algorithm used by the Hugging Face `transformers`
library for BERT-family models.

#### `QueryApi` (`QueryApi.h` / `QueryApi.cpp`)

A named-pipe server listening on:

```
\\.\pipe\WarpFileActivityAPI
```

The pipe is created with `PIPE_UNLIMITED_INSTANCES` so multiple clients can connect
concurrently. Each client connection is served on a detached thread.

The API accepts four kinds of requests:

1. **Event queries** — retrieve raw activity events for a time window, optionally
   filtered by event type via the `"types"` array. The response JSON is segregated
   by event type at the root level.
2. **`QueryInferences`** — batch lookup of precomputed inference records.
3. **`GetInferenceDeltas`** — incremental sync of inference records since a version
   watermark.
4. **`GetRecentContext`** — retrieve the most recently deduced semantic topics from
   the MiniLM topic inference engine.

Inference operations are routed to the `InferenceEngine` instance;
`GetRecentContext` is routed to the `TopicInference` instance; event queries
are handled by `BuildJsonResponse` which reads directly from `ActivityDatabase`.

---

## User interface

### Light mode (default)

The window opens maximized with a light grey background (`#F3F3F3`), dark text, and
white response panel. Buttons have soft grey fills with rounded corners and a blue
accent border on hover/press.

### Dark mode

Click the **Dark Mode** button in the top-right corner to switch. The background
becomes near-black (`#1E1E1E`), text turns light grey, buttons become charcoal with
accent borders, and the response area uses light blue text on a dark surface. Click
**Light Mode** to switch back. The toggle is instant -- all controls repaint
immediately.

### Layout

```
+------------------------------------------------------------+
| * Your friend WARP is ...     [ Clear History ] [Dark Mode] |
|------------------------------------------------------------|
| Query by Predefined Time Window                            |
| [Last 15 min] [Last 30 min] [Last 1 hour] [Last 2 hours]  |
| [Last 6 hours] [Last 24 hours] [Last 7 days] ...          |
|                                                            |
| Query by Custom Seconds                                    |
| [  300  ] [Send Custom Query]                              |
|                                                            |
| Query with Default (empty body -- returns last 1 hour)     |
| [Send Default Query]                                       |
|                                                            |
| Event Types to Query:                                      |
|   [x] File Activity  [x] App Launches  [x] App Focus      |
|   [x] Browsing Act.                                        |
|                                                            |
| Explore Precomputed Inferences                             |
| Entity Types: [x] Files [x] Apps [x] URLs  Top N: [50]    |
|                                  [Show Top Inferences]     |
| [Enter path or URL to look up...                ] [Lookup] |
|                                                            |
| [Show Recent Context]                                      |
|                                                            |
| API Response
| +--------------------------------------------------------+ |
| | {                                                      | |
| |   "file_activities": { ... },                          | |
| |   "app_launch_activities": { ... },                    | |
| |   "browsing_activities": { ... }                       | |
| | }                                                      | |
| +--------------------------------------------------------+ |
+------------------------------------------------------------+
```

All controls reflow when the window is resized. Minimum window size is 800 x 500.

---

## Building

**Prerequisites:**

- Visual Studio 2022 (v143 toolset)
- Windows 10 SDK (10.0 or later)
- C++14 standard

The project compiles SQLite as an embedded amalgamation (`sqlite3.c` / `sqlite3.h`)
and uses **Microsoft.ML.OnnxRuntime** (NuGet, v1.22.0) for running the MiniLM
semantic model.

**Steps:**

1. Open `WARP!.sln` in Visual Studio 2022.
2. NuGet restore will automatically fetch `Microsoft.ML.OnnxRuntime` into the
   `packages/` directory.
3. Download the MiniLM model files into the `models/` directory:
   - `minilm.onnx` (~86 MB) from
     [Hugging Face](https://huggingface.co/sentence-transformers/all-MiniLM-L6-v2/resolve/main/onnx/model.onnx)
   - `vocab.txt` (~220 KB) from
     [Hugging Face](https://huggingface.co/sentence-transformers/all-MiniLM-L6-v2/resolve/main/vocab.txt)
4. Build any configuration (Debug/Release × Win32/x64/ARM64).

The build will automatically:
- Copy `onnxruntime.dll` to the output directory.
- Copy `models/minilm.onnx` and `models/vocab.txt` to `$(OutDir)models/` if they
  exist in the project directory.

At runtime, the app looks for the `models/` directory next to the executable first,
then falls back to `%LOCALAPPDATA%\WARP\models`.

---

## Runtime behaviour

1. On launch the app requests elevation (UAC prompt).
2. The main window is created but kept hidden; a light-bulb system-tray icon appears.
3. The SQLite database is opened (created if it doesn't exist), the
   EventContext columns are added to all activity tables via idempotent
   `ALTER TABLE`, and old records are evicted from all four tables.
4. `ForegroundChangeBroker` and `LaunchCorrelator` are started on the UI
   thread (both need a message pump for `SetWinEventHook`).
5. `FileMonitor` starts: a `WARP-FileTrace` private ETW session is opened
   against `Microsoft-Windows-Kernel-File` for fixed drives;
   `ReadDirectoryChangesW` threads start for removable / network drives;
   the shell-change subscription is registered. The goop filter and
   AppData allowlist are active from the first event.
6. `AppLaunchMonitor` opens a `WARP-ProcessTrace` private ETW session
   against `Microsoft-Windows-Kernel-Process`. Each new PID is parked
   with `LaunchCorrelator` and emitted with `created_window_ms` and
   confidence after the window correlation resolves.
7. `BrowsingMonitor` and `ForegroundMonitor` subscribe to
   `ForegroundChangeBroker`. `BrowsingMonitor` additionally starts the
   `UrlExtractor` worker thread.
8. `IdleDetector` starts polling `GetLastInputInfo` every 5 seconds and
   creates the `WM_POWERBROADCAST` listener window. Two thresholds are
   armed (soft attenuation, hard pause).
9. The inference engine is initialised with direct access to the SQLite
   database. Confidence-weighted rolling counts are recomputed from the
   raw event tables (`SUM(COALESCE(confidence, 1.0))`).
10. The named-pipe server starts accepting connections.
11. The topic inference engine loads the MiniLM ONNX model and vocabulary,
    pre-embeds ~50 topic candidate labels, and begins its 5-minute cycle.
12. Every detected event (file, app launch, app focus, or browsing) is
    enriched with an `EventContext` (source / foreground / parent ids,
    `ms_since_input`, `confidence`), inserted into the appropriate
    database table, **and** fed to the inference engine, which adds the
    event's confidence to the per-entity rolling counters.
13. Every 5 minutes, the topic inference engine gathers all activities
    from the last 15 minutes, embeds each with MiniLM, matches to the
    nearest semantic topic, and stores the top 3 topics with a coverage
    percentage.
14. Every 6 hours, records older than 30 days are purged from all event
    tables and inference rolling counts are recomputed.
15. When the user crosses the **soft** idle threshold, new events are
    captured at attenuated confidence. When they cross the **hard**
    threshold (or the PC sleeps), all monitors pause. On wake, all
    monitors resume but events captured within 5 s carry a 0.2× wake-
    boundary confidence multiplier.
16. Right-clicking the tray icon shows **Open** / **Exit**.
17. The built-in API test buttons can be used at any time to query the
    pipe and inspect results. Use the checkboxes to select which event
    types to include.
18. The "Explore Precomputed Inferences" section can be used to browse
    the top entities by recency score (filtered by entity type) or look
    up the inference record for a specific file path, app path, or URL.
19. Clearing activity history also clears the inference engine's
    in-memory cache.

---

## Query API documentation

### Connection

Connect to the named pipe from any Windows process:

```
\\.\pipe\WarpFileActivityAPI
```

The pipe uses **message mode** (`PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE`).
Open it with `CreateFile` using `GENERIC_READ | GENERIC_WRITE`.

### Request format

Send a single UTF-8 JSON message. The following request shapes are supported:

#### 1. Predefined time window

```json
{ "window": "<window_code>" }
```

| Window code | Meaning |
|---|---|
| `15m` | Last 15 minutes |
| `30m` | Last 30 minutes |
| `1h` | Last 1 hour |
| `2h` | Last 2 hours |
| `6h` | Last 6 hours |
| `24h` | Last 24 hours |
| `7d` | Last 7 days |
| `15d` | Last 15 days |
| `30d` | Last 30 days |

#### 2. Custom time range (in seconds)

```json
{ "seconds": 300 }
```

Returns all activity from the last 300 seconds (5 minutes). Any positive integer is
accepted.

#### 3. No parameters (default)

```json
{}
```

Defaults to the last 1 hour.

#### 4. Event type filtering (optional)

Any request can include a `"types"` array to specify which event categories to return:

```json
{ "window": "1h", "types": ["file", "app_launch", "browsing"] }
```

| Type value | Event category |
|---|---|
| `file` | File/folder activity |
| `app_launch` | Application launch events |
| `app_focus` | Foreground app focus sessions (window title + dwell time) |
| `browsing` | Browsing activity events |

If `"types"` is omitted, all four event types are returned (backward compatible).

**Examples:**

```json
{"window":"15m","types":["file"]}
{"seconds":300,"types":["app_launch","browsing"]}
{"window":"1h","types":["file","app_launch","browsing"]}
{}
```

#### 5. Inference API

The named pipe also supports two inference operations that return precomputed
per-entity analytics instead of raw event lists. These are identified by the
`"op"` field in the request.

##### QueryInferences

Batch-lookup inference records for a list of entity keys (file paths, app paths,
or URLs). An optional `"fields"` array limits which fields are returned.

```json
{
  "op": "QueryInferences",
  "paths": [
    "c:\\users\\alice\\documents\\report.docx",
    "c:\\windows\\system32\\notepad.exe"
  ],
  "fields": ["recency_score", "last_open_ts", "open_count_7d"]
}
```

If `"fields"` is omitted, all fields are returned.

**Response:**

```json
{
  "now": 1750012345,
  "results": {
    "c:\\users\\alice\\documents\\report.docx": {
      "recency_score": 187.3,
      "last_open_ts": 1750012000,
      "open_count_7d": 12
    },
    "c:\\windows\\system32\\notepad.exe": {
      "recency_score": 42.1,
      "last_open_ts": 1749998000,
      "open_count_7d": 3
    }
  }
}
```

##### GetInferenceDeltas

Retrieve all inference records that have changed since a given version watermark.
Returns up to 5 000 records per call, ordered by ascending `version`.

```json
{
  "op": "GetInferenceDeltas",
  "since_version": 0
}
```

**Response:**

```json
{
  "now": 1750012345,
  "deltas": [
    {
      "entity_key": "c:\\users\\alice\\documents\\report.docx",
      "entity_type": "file",
      "last_event_ts": 1750012000,
      "last_open_ts": 1750012000,
      "last_edit_ts": 1750011500,
      "open_count_7d": 12,
      "open_count_30d": 45,
      "open_count_total": 45,
      "recency_score": 187.3,
      "version": 1,
      "updated_at": 1750012000
    }
  ]
}
```

##### GetRecentContext

Retrieve the most recently deduced semantic topics from the MiniLM-powered topic
inference engine. No parameters are required.

```json
{
  "op": "GetRecentContext"
}
```

**Response:**

```json
{
  "recent_context": {
    "timestamp": 1750012345,
    "activity_count": 47,
    "coverage_pct": 93.6,
    "topics": [
      "C and C++ software development and programming",
      "Source control and Git version management",
      "Technical research and documentation reading"
    ],
    "history_count": 12,
    "model": "all-MiniLM-L6-v2"
  }
}
```

| Field | Type | Description |
|---|---|---|
| `timestamp` | `integer` | When this inference was produced (Unix epoch seconds). |
| `activity_count` | `integer` | Total activities examined in the 15-minute window. |
| `coverage_pct` | `number` | Percentage of activities covered by the top 3 topics. |
| `topics` | `string[]` | Up to 3 semantic topic labels, ordered by coverage. |
| `history_count` | `integer` | Number of stored inference snapshots (max 288 = 24 h). |
| `model` | `string` | The embedding model used (`"all-MiniLM-L6-v2"`). |

##### Inference record fields

| Field | Type | Description |
|---|---|---|
| `entity_key` | `string` | Lowercase entity identifier (file path, app path, or URL). |
| `entity_type` | `string` | `"file"`, `"app"`, or `"url"`. |
| `last_event_ts` | `integer` | Timestamp of the most recent event of any kind. |
| `last_open_ts` | `integer` | Timestamp of the most recent OPEN (files) or launch (apps/URLs). |
| `last_edit_ts` | `integer` | Timestamp of the most recent MODIFY/CREATE (files only). |
| `open_count_7d` | `integer` | Confidence-weighted sum of OPEN events in the rolling 7-day window (rounded to integer). |
| `open_count_30d` | `integer` | Confidence-weighted sum of OPEN events in the rolling 30-day window (rounded to integer). |
| `open_count_total` | `integer` | Confidence-weighted lifetime sum since WARP started tracking this entity (rounded to integer). |
| `recency_score` | `number` | Composite score (0–255) combining exponential time-decay and frequency. Higher = more recently/frequently used. |
| `version` | `integer` | Monotonically increasing per-entity version; use as watermark for delta sync. |
| `updated_at` | `integer` | Timestamp of the last inference update. |

### Response format

A single UTF-8 JSON message with event types segregated at the root level.
Only the requested event types are included in the response:

```json
{
    "file_activities": {
        "count": 3,
        "events": [
            {
                "id": 1042,
                "timestamp": 1750012345,
                "action": "CREATE",
                "path": "C:\\Users\\Alice\\Documents\\report.docx"
            },
            {
                "id": 1041,
                "timestamp": 1750012300,
                "action": "RENAME",
                "path": "C:\\Users\\Alice\\Documents\\draft-v2.docx",
                "old_path": "C:\\Users\\Alice\\Documents\\draft.docx"
            },
            {
                "id": 1040,
                "timestamp": 1750012280,
                "action": "DELETE",
                "path": "C:\\Users\\Alice\\Downloads\\temp.zip"
            }
        ]
    },
    "app_launch_activities": {
        "count": 2,
        "events": [
            {
                "id": 15,
                "timestamp": 1750012400,
                "exe_name": "notepad.exe",
                "exe_path": "C:\\Windows\\System32\\notepad.exe",
                "pid": 12340
            },
            {
                "id": 14,
                "timestamp": 1750012200,
                "exe_name": "code.exe",
                "exe_path": "C:\\Users\\Alice\\AppData\\Local\\Programs\\Microsoft VS Code\\Code.exe",
                "pid": 9876
            }
        ]
    },
    "browsing_activities": {
        "count": 2,
        "events": [
            {
                "id": 8,
                "timestamp": 1750012350,
                "browser": "chrome",
                "title": "GitHub - Pull Requests",
                "url": "https://github.com/pulls"
            },
            {
                "id": 7,
                "timestamp": 1750012100,
                "browser": "msedge",
                "title": "Stack Overflow - Search"
            }
        ]
    },
    "app_focus_activities": {
        "count": 2,
        "events": [
            {
                "id": 5,
                "timestamp": 1750012345,
                "exe_name": "OUTLOOK.EXE",
                "exe_path": "C:\\Program Files\\Microsoft Office\\root\\Office16\\OUTLOOK.EXE",
                "window_title": "Tax Returns for Nancy - Outlook",
                "duration_secs": 300
            },
            {
                "id": 4,
                "timestamp": 1750012000,
                "exe_name": "EXCEL.EXE",
                "exe_path": "C:\\Program Files\\Microsoft Office\\root\\Office16\\EXCEL.EXE",
                "window_title": "Route planner simulator.xlsx - Excel",
                "duration_secs": 600
            }
        ]
    }
}
```

#### Response fields -- file_activities

| Field | Type | Description |
|---|---|---|
| `file_activities.count` | integer | Total number of file activity records. |
| `file_activities.events` | array | Ordered list (most recent first). |
| `events[].id` | integer | Auto-increment row ID. |
| `events[].timestamp` | integer | Unix epoch seconds (UTC). |
| `events[].action` | string | One of: `CREATE`, `OPEN`, `DELETE`, `MODIFY`, `RENAME`. |
| `events[].path` | string | Full path. Backslashes are JSON-escaped (`\\`). |
| `events[].old_path` | string *(optional)* | Present only for `RENAME` actions. |

#### Response fields -- app_launch_activities

| Field | Type | Description |
|---|---|---|
| `app_launch_activities.count` | integer | Total number of app launch records. |
| `app_launch_activities.events` | array | Ordered list (most recent first). |
| `events[].id` | integer | Auto-increment row ID. |
| `events[].timestamp` | integer | Unix epoch seconds (UTC). |
| `events[].exe_name` | string | Executable filename (e.g. `"notepad.exe"`). |
| `events[].exe_path` | string | Full path to the executable. |
| `events[].pid` | integer | Process ID at launch time. |

#### Response fields -- browsing_activities

| Field | Type | Description |
|---|---|---|
| `browsing_activities.count` | integer | Total number of browsing records. |
| `browsing_activities.events` | array | Ordered list (most recent first). |
| `events[].id` | integer | Auto-increment row ID. |
| `events[].timestamp` | integer | Unix epoch seconds (UTC). |
| `events[].browser` | string | Browser identifier (`chrome`, `msedge`, `firefox`, `brave`, `opera`, `vivaldi`, `ie`). |
| `events[].title` | string | Page title (browser suffix stripped). |
| `events[].url` | string *(optional)* | URL if extractable from the title bar. |

#### Response fields -- app_focus_activities

| Field | Type | Description |
|---|---|---|
| `app_focus_activities.count` | integer | Total number of focus session records. |
| `app_focus_activities.events` | array | Ordered list (most recent first). |
| `events[].id` | integer | Auto-increment row ID. |
| `events[].timestamp` | integer | Unix epoch seconds (UTC) when the focus session started. |
| `events[].exe_name` | string | Executable filename (e.g. `"OUTLOOK.EXE"`). |
| `events[].exe_path` | string | Full path to the executable. |
| `events[].window_title` | string | The window title during this foreground session. |
| `events[].duration_secs` | integer | How many seconds the app was in the foreground. |

### Client examples

#### C++ (Win32)

```cpp
#include <windows.h>
#include <cstdio>

int main()
{
    HANDLE hPipe = CreateFileW(
        L"\\\\.\\pipe\\WarpFileActivityAPI",
        GENERIC_READ | GENERIC_WRITE,
        0, nullptr, OPEN_EXISTING, 0, nullptr);

    if (hPipe == INVALID_HANDLE_VALUE) {
        printf("Failed to connect: %lu\n", GetLastError());
        return 1;
    }

    // Set pipe to message-read mode
    DWORD mode = PIPE_READMODE_MESSAGE;
    SetNamedPipeHandleState(hPipe, &mode, nullptr, nullptr);

    // Send request -- last 15 min, only file and app launch events
    const char* request = R"({"window":"15m","types":["file","app_launch"]})";
    DWORD written = 0;
    WriteFile(hPipe, request, (DWORD)strlen(request), &written, nullptr);

    // Read response
    char buffer[65536] = {};
    DWORD bytesRead = 0;
    ReadFile(hPipe, buffer, sizeof(buffer) - 1, &bytesRead, nullptr);

    printf("Response:\n%s\n", buffer);

    CloseHandle(hPipe);
    return 0;
}
```

#### Python

```python
import json
import win32file
import win32pipe

pipe_name = r"\\.\pipe\WarpFileActivityAPI"

handle = win32file.CreateFile(
    pipe_name,
    win32file.GENERIC_READ | win32file.GENERIC_WRITE,
    0, None,
    win32file.OPEN_EXISTING,
    0, None,
)
win32pipe.SetNamedPipeHandleState(
    handle, win32pipe.PIPE_READMODE_MESSAGE, None, None
)

# Query last 30 minutes -- all event types
request = json.dumps({"window": "30m"}).encode("utf-8")
win32file.WriteFile(handle, request)

_, response_bytes = win32file.ReadFile(handle, 65536)
handle.Close()

data = json.loads(response_bytes.decode("utf-8"))

# File events
if "file_activities" in data:
    fa = data["file_activities"]
    print(f"File events: {fa['count']}")
    for e in fa["events"]:
        print(f"  [{e['action']}] {e['path']}")

# App launches
if "app_launch_activities" in data:
    al = data["app_launch_activities"]
    print(f"App launches: {al['count']}")
    for e in al["events"]:
        print(f"  {e['exe_name']} (PID {e['pid']})")

# Browsing
if "browsing_activities" in data:
    ba = data["browsing_activities"]
    print(f"Browsing events: {ba['count']}")
    for e in ba["events"]:
        print(f"  [{e['browser']}] {e['title']}")
```

#### PowerShell

```powershell
$pipe = New-Object System.IO.Pipes.NamedPipeClientStream(
    ".", "WarpFileActivityAPI",
    [System.IO.Pipes.PipeDirection]::InOut)
$pipe.Connect(5000)
$pipe.ReadMode = [System.IO.Pipes.PipeTransmissionMode]::Message

$request = [Text.Encoding]::UTF8.GetBytes('{"window":"1h"}')
$pipe.Write($request, 0, $request.Length)

$buffer = New-Object byte[] 65536
$bytesRead = $pipe.Read($buffer, 0, $buffer.Length)
$response = [Text.Encoding]::UTF8.GetString($buffer, 0, $bytesRead)

$pipe.Close()
$data = $response | ConvertFrom-Json

Write-Host "File events: $($data.file_activities.count)"
Write-Host "App launches: $($data.app_launch_activities.count)"
Write-Host "Browsing events: $($data.browsing_activities.count)"
```

#### C# (.NET)

```csharp
using System.IO.Pipes;
using System.Text;
using System.Text.Json;

using var pipe = new NamedPipeClientStream(".", "WarpFileActivityAPI",
    PipeDirection.InOut);
pipe.Connect(5000);
pipe.ReadMode = PipeTransmissionMode.Message;

byte[] request = Encoding.UTF8.GetBytes("""{"window":"6h","types":["browsing"]}""");
pipe.Write(request, 0, request.Length);

byte[] buffer = new byte[65536];
int bytesRead = pipe.Read(buffer, 0, buffer.Length);
string json = Encoding.UTF8.GetString(buffer, 0, bytesRead);

Console.WriteLine(json);
```

---

## File layout

```
WARP!\
|-- WARP!.sln                   Solution file
|-- WARP!.vcxproj               Project file (v143, C++14, Unicode)
|-- WARP!.vcxproj.filters       IDE filter assignments
|-- .gitignore                   Git ignore rules (build artifacts, .vs, etc.)
|-- README.md                   This file
|-- API.md                      Detailed API integration guide
|
|-- WARP!.cpp                   Application entry point, UI, tray icon, theme system
|-- WARP!.h                     App header (includes resource.h)
|-- WARP!.rc                    Resource script (icons, strings, dialogs)
|-- WARP!.ico                   Application icon (light bulb, multi-resolution)
|-- small.ico                   Small application icon (light bulb)
|-- Resource.h                  Resource ID definitions
|-- framework.h                 Precompiled / common system headers
|-- targetver.h                 Windows SDK version targeting
|
|-- ActivityDatabase.h          Database class interface (4 tables + EventContext columns)
|-- ActivityDatabase.cpp        SQLite storage; idempotent ALTER for context columns
|-- EventContext.h              Per-event source/foreground/parent context + confidence
|-- EventContext.cpp            CaptureContext, wake-boundary multiplier, BindEventContext
|-- FileMonitor.h               File/shell monitoring interface
|-- FileMonitor.cpp             ETW Microsoft-Windows-Kernel-File (private session) +
|                                RDChangesW (rm/net only) + SHChangeNotify;
|                                goop filter + AppData allowlist + token bucket
|-- AppLaunchMonitor.h          App launch monitoring interface
|-- AppLaunchMonitor.cpp        ETW Microsoft-Windows-Kernel-Process (private session) +
|                                LaunchCorrelator parking
|-- LaunchCorrelator.h          Window-creation correlator interface
|-- LaunchCorrelator.cpp        SetWinEventHook(EVENT_OBJECT_CREATE) on UI thread,
|                                5s deadline -> created_window_ms + confidence
|-- SystemProcessClassifier.h   Multi-signal system-process classifier interface
|-- SystemProcessClassifier.cpp Parent ancestry + image path + Authenticode +
|                                session + integrity + name-pattern voting
|-- ForegroundChangeBroker.h    Single global EVENT_SYSTEM_FOREGROUND hook + fan-out
|-- ForegroundChangeBroker.cpp  Subscriber registry + dispatch on UI thread
|-- BrowsingMonitor.h           Browsing activity monitoring interface
|-- BrowsingMonitor.cpp         Broker subscriber + per-PID EVENT_OBJECT_NAMECHANGE +
|                                UrlExtractor enqueue
|-- UrlExtractor.h              UIA-based URL extraction interface
|-- UrlExtractor.cpp            MTA worker thread, bounded queue, Chrome/Edge/Firefox
|                                address-bar EditControl reader
|-- ForegroundMonitor.h         Foreground app focus monitoring interface
|-- ForegroundMonitor.cpp       Broker subscriber; emits exe + title + dwell time
|-- IdleDetector.h              Two-tier idle/sleep detector interface
|-- IdleDetector.cpp            GetLastInputInfo + WM_POWERBROADCAST;
|                                soft (attenuate) + hard (pause) + wake boundary
|-- QueryApi.h                  Named-pipe API interface
|-- QueryApi.cpp                Pipe server and JSON serialization impl
|-- InferenceEngine.h           Inference engine interface (per-entity analytics)
|-- InferenceEngine.cpp         Confidence-weighted REAL counters, recency score,
|                                QueryInferences & GetInferenceDeltas impl
|-- TopicInference.h            Semantic topic inference interface (MiniLM embedding pipeline)
|-- TopicInference.cpp          ONNX Runtime model loading, embedding, topic deduction impl
|-- BertTokenizer.h             Header-only BERT WordPiece tokenizer for MiniLM
|
|-- packages.config             NuGet package references (Microsoft.ML.OnnxRuntime)
|-- models/                     MiniLM model files (not checked in — see Building)
|   |-- minilm.onnx             all-MiniLM-L6-v2 ONNX model (~86 MB)
|   +-- vocab.txt               BERT WordPiece vocabulary (~220 KB)
|
|-- sqlite3.c                   SQLite amalgamation (compiled as C)
+-- sqlite3.h                   SQLite public header
```

---

## License

SQLite is in the [public domain](https://www.sqlite.org/copyright.html).


## Author

Suman Ghosh — [@sumanthewhiz](https://github.com/sumanthewhiz)

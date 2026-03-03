# WARP - Windows Activity Reasoning Platform

WARP is a lightweight Windows desktop application that silently monitors file/folder
activity, application launches, and browsing activity on the local PC, stores
everything in a rolling 30-day on-disk database, and exposes a queryable named-pipe
API so that other applications running on the same machine can programmatically
retrieve activity history.

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
| **Event type filter checkboxes** | Three checkboxes (**File Activity**, **App Launches**, **Browsing Activity**) control which event types are included in query results. All are checked by default. |
| **Idle / sleep awareness** | Monitoring pauses automatically when the PC has been idle for >= 2 minutes or enters sleep/hibernate, and resumes the moment user input is detected or the PC wakes. |
| **File-system monitoring** | Every fixed, removable, and network drive is watched recursively via `ReadDirectoryChangesW`. |
| **Shell-level monitoring** | `SHChangeNotifyRegister` on the entire shell namespace catches higher-level operations (copy, move, shell renames) that pure file-system notifications may miss. |
| **App launch monitoring** | New user-initiated process launches are detected by polling the process table every 2 seconds. System and background processes are filtered out. |
| **Browsing activity monitoring** | The foreground window is polled every 3 seconds. When a recognized browser is active, the page title (and URL if present) is captured and deduplicated. |
| **SQLite storage** | All events are persisted in `%LOCALAPPDATA%\WARP\activity.db` using WAL mode across three tables. |
| **30-day rolling eviction** | Records older than 30 days are deleted on startup and every 6 hours thereafter from all tables. |
| **Named-pipe query API** | Other Windows processes can connect to `\\.\pipe\WarpFileActivityAPI` and retrieve activity data as JSON for any supported time window, optionally filtered by event type. |
| **Inference engine** | Every captured event incrementally updates per-entity inference records (files, apps, URLs) with open/edit timestamps, access counts, and an exponential-decay recency score. Two dedicated API operations (`QueryInferences`, `GetInferenceDeltas`) let client apps retrieve these precomputed insights without scanning raw events. |

---

## Event Types

WARP captures three categories of events:

| Event Type | Description | Source |
|---|---|---|
| **File Activity** | File/folder creates, opens, modifications, deletes, renames | `ReadDirectoryChangesW`, `SHChangeNotifyRegister`, NT Kernel Logger ETW |
| **App Launch** | User-initiated application launches (exe name, path, PID) | Process table polling via `CreateToolhelp32Snapshot` |
| **Browsing Activity** | Browser page title changes (browser name, page title, URL) | Foreground window title polling via `GetForegroundWindow` + `GetWindowText` |

---

## Architecture

```
+-----------------------------------------------------------------------+
|                            WARP!.cpp                                  |
|                        (Win32 entry point)                            |
|  +-----------+ +---------+ +----------+ +----------+ +-----------+   |
|  |FileMonitor| |AppLaunch| |Browsing  | |IdleDetect| | QueryApi  |   |
|  | .h/.cpp   | |Monitor  | |Monitor   | |or .h/.cpp| | .h/.cpp   |   |
|  |           | |.h/.cpp  | |.h/.cpp   | |          | |           |   |
|  +-----+-----+ +----+----+ +----+-----+ +-----+----+ +-----+-----+  |
|        |             |           |             |             |        |
|        | callback    | callback  | callback    | pause/      | read   |
|        +------------>+---------->+------------>| resume      |<------>|
|        |             |           |             +------------>|        |
|        |             |           |             |             |        |
+--------+-----+-------+-----+-----+-------------+------+------+-------+
         |     |       |     |     |                     |
   ReadDirectory   Toolhelp32   GetForeground  GetLastInput  Named Pipe
   ChangesW /      Snapshot     Window +       Info / WM_    \\.\pipe\
   SHChangeNotify               GetWindowText  POWERBROADCAST WarpFileActivityAPI
   / ETW
                   +------------------+
                   | InferenceEngine  |  <-- updated per event
                   | .h/.cpp          |      (recency scores,
                   +--------+---------+       access counts)
                            |
                   +--------+---------+
                   |  ActivityDB      |
                   |  .h/.cpp         |
                   +--------+---------+
                            |
                          SQLite
                       activity.db
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
- **Event type filter** -- three checkboxes (**File Activity**, **App Launches**,
  **Browsing Activity**) that control which event types are queried. All checked by
  default. The selected types are sent as a `"types"` array in the JSON request.
- **Response area** -- a read-only, scrollable, monospace (`Cascadia Mono`) edit
  control that displays pretty-printed JSON responses.

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

Spawns one background thread per logical drive (fixed, removable, or network) that
calls `ReadDirectoryChangesW` in overlapped mode with recursive watching. A
separate thread registers for shell change notifications via
`SHChangeNotifyRegister` on the desktop namespace root to capture shell-level
events. An additional thread runs an NT Kernel Logger ETW trace to capture
file-open events from interactive user processes.

Detected actions: **CREATE**, **OPEN**, **DELETE**, **MODIFY**, **RENAME**.

The monitor exposes `Pause()` / `Resume()` methods that the idle detector calls to
suspend event recording when the PC is inactive.

#### `AppLaunchMonitor` (`AppLaunchMonitor.h` / `AppLaunchMonitor.cpp`)

A polling thread that takes a snapshot of all running processes every 2 seconds via
`CreateToolhelp32Snapshot`. New PIDs that were not present in the previous snapshot
are classified as newly launched applications.

Filters applied:
- **Session 0 excluded** -- all Windows services run in session 0; only interactive
  sessions (>= 1) are reported.
- **System process blocklist** -- 35+ known system/noise processes (svchost, dwm,
  csrss, RuntimeBroker, SearchIndexer, Windows Defender, etc.) are excluded.
- **Self-exclusion** -- WARP's own process is excluded.

Each detected launch reports the executable name, full path, and PID.

#### `BrowsingMonitor` (`BrowsingMonitor.h` / `BrowsingMonitor.cpp`)

A polling thread that checks the foreground window every 3 seconds. When the
foreground process matches a recognized browser, the window title is captured.

Supported browsers: **Chrome**, **Edge**, **Firefox**, **Brave**, **Opera**,
**Vivaldi**, **Internet Explorer**.

Processing:
- Common browser suffixes ("- Google Chrome", "- Microsoft Edge", etc.) are stripped
  to extract the clean page title.
- If the title looks like a URL (`http://` or `https://`), it is captured separately.
- **Deduplication** -- the same title + browser combination is reported only once
  until the user navigates to a different page.

#### `IdleDetector` (`IdleDetector.h` / `IdleDetector.cpp`)

A polling thread that checks `GetLastInputInfo` every 5 seconds. When the idle
duration exceeds the configured threshold (default 2 minutes) the `onIdle` callback
fires; when input resumes, the `onActive` callback fires.

A message-only window is also created to receive `WM_POWERBROADCAST` notifications
(`PBT_APMSUSPEND` / `PBT_APMRESUMEAUTOMATIC`) so that sleep/wake transitions
trigger the same pause/resume path.

When idle or asleep, all three monitors (file, app launch, browsing) are paused.

#### `ActivityDatabase` (`ActivityDatabase.h` / `ActivityDatabase.cpp`)

A thread-safe SQLite wrapper. The database is stored at:

```
%LOCALAPPDATA%\WARP\activity.db
```

**Schema:**

```sql
CREATE TABLE file_activity (
    id        INTEGER PRIMARY KEY AUTOINCREMENT,
    timestamp INTEGER NOT NULL,       -- Unix epoch seconds (UTC)
    action    TEXT    NOT NULL,        -- CREATE | OPEN | DELETE | MODIFY | RENAME
    path      TEXT    NOT NULL,        -- full path of the affected file/folder
    old_path  TEXT                     -- previous path (RENAME only; NULL otherwise)
);

CREATE TABLE app_launch_activity (
    id        INTEGER PRIMARY KEY AUTOINCREMENT,
    timestamp INTEGER NOT NULL,       -- Unix epoch seconds (UTC)
    exe_name  TEXT    NOT NULL,        -- e.g. "notepad.exe"
    exe_path  TEXT    NOT NULL,        -- full path to the executable
    pid       INTEGER NOT NULL        -- process ID at launch time
);

CREATE TABLE browsing_activity (
    id        INTEGER PRIMARY KEY AUTOINCREMENT,
    timestamp INTEGER NOT NULL,       -- Unix epoch seconds (UTC)
    browser   TEXT    NOT NULL,        -- e.g. "chrome", "msedge", "firefox"
    title     TEXT    NOT NULL,        -- page title from window title bar
    url       TEXT                     -- URL if extractable; NULL otherwise
);

CREATE INDEX idx_activity_ts   ON file_activity(timestamp);
CREATE INDEX idx_activity_action ON file_activity(action, timestamp);
CREATE INDEX idx_app_ts        ON app_launch_activity(timestamp);
CREATE INDEX idx_browse_ts     ON browsing_activity(timestamp);

-- Inference table (precomputed per-entity analytics)
CREATE TABLE inference (
    entity_key        TEXT PRIMARY KEY,
    entity_type       TEXT,         -- 'file', 'app', or 'url'
    last_event_ts     INTEGER,
    last_open_ts      INTEGER,
    last_edit_ts      INTEGER,
    open_count_7d     INTEGER,
    open_count_30d    INTEGER,
    open_count_total  INTEGER,
    recency_score     REAL,         -- 0-255 composite score
    version           INTEGER,      -- monotonic per-entity version
    updated_at        INTEGER
);

CREATE INDEX idx_inference_updated_at ON inference(updated_at);
CREATE INDEX idx_inference_version    ON inference(version);
```

**Pragmas:** `journal_mode=WAL`, `synchronous=NORMAL`.

**Eviction:** Records older than 30 days are deleted from all three event tables on
startup and every 6 hours via a `WM_TIMER`.

#### `InferenceEngine` (`InferenceEngine.h` / `InferenceEngine.cpp`)

A real-time analytics layer that maintains per-entity inference records across
three entity types: **files**, **apps**, and **URLs**.

Every time a raw event is written to the database, the corresponding monitor
callback also calls `OnFileEvent`, `OnAppLaunchEvent`, or `OnBrowsingEvent` on the
inference engine. The engine:

1. Normalizes the entity key to lowercase UTF-8.
2. Loads or creates an `InferenceRecord` (in-memory cache backed by the `inference`
   SQLite table).
3. Increments counters (`open_count_7d`, `open_count_30d`, `open_count_total`) and
   updates timestamps (`last_event_ts`, `last_open_ts`, `last_edit_ts`).
4. Recomputes a **recency score** using exponential decay:
   `score = 200 × e^(−Δt / 172800) + 5 × ln(1 + open_count_7d)`, capped at 255.
5. Bumps the record `version` and persists via `INSERT OR REPLACE`.

Two query operations are exposed through the named pipe (see the
[Inference API](#5-inference-api) section below):

| Operation | Purpose |
|---|---|
| `QueryInferences` | Batch lookup of inference records by entity key, with optional field projection. |
| `GetInferenceDeltas` | Retrieve all records whose `version` exceeds a given watermark (up to 5 000 per call). |

Clearing the activity history (`ClearAll`) also clears the inference in-memory
cache via `ClearCache()`.

#### `QueryApi` (`QueryApi.h` / `QueryApi.cpp`)

A named-pipe server listening on:

```
\\.\pipe\WarpFileActivityAPI
```

The pipe is created with `PIPE_UNLIMITED_INSTANCES` so multiple clients can connect
concurrently. Each client connection is served on a detached thread.

The API accepts three kinds of requests:

1. **Event queries** — retrieve raw activity events for a time window, optionally
   filtered by event type via the `"types"` array. The response JSON is segregated
   by event type at the root level.
2. **`QueryInferences`** — batch lookup of precomputed inference records.
3. **`GetInferenceDeltas`** — incremental sync of inference records since a version
   watermark.

Inference operations are routed to the `InferenceEngine` instance; event queries
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
|   [x] File Activity  [x] App Launches  [x] Browsing Act.  |
|                                                            |
| API Response                                               |
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

The project compiles SQLite as an embedded amalgamation (`sqlite3.c` / `sqlite3.h`);
no external dependencies are required.

Open `WARP!.sln` in Visual Studio and build any configuration (Debug/Release x
Win32/x64/ARM64). The output executable will request administrator privileges
automatically via the linker's UAC manifest setting.

---

## Runtime behaviour

1. On launch the app requests elevation (UAC prompt).
2. The main window is created but kept hidden; a light-bulb system-tray icon appears.
3. The SQLite database is opened (created if it doesn't exist) and old records are evicted from all three tables.
4. File-system, shell, and ETW monitors start on all eligible drives.
5. The app launch monitor begins polling the process table.
6. The browsing monitor begins polling the foreground window.
7. The idle detector begins polling for user inactivity and power events.
8. The inference engine is initialized with direct access to the SQLite database.
9. The named-pipe server starts accepting connections.
10. Every detected event (file, app launch, or browsing) is inserted into the appropriate database table **and** fed to the inference engine, which incrementally updates per-entity scores in real time.
11. Every 6 hours, records older than 30 days are purged from all event tables.
12. When the user is idle >= 2 min or the PC sleeps, all monitors pause; they resume on activity/wake.
13. Right-clicking the tray icon shows **Open** / **Exit**. *Open* shows the window maximized; *Exit* tears everything down cleanly.
14. The built-in API test buttons can be used at any time to query the pipe and inspect results. Use the checkboxes to select which event types to include.
15. Clearing activity history also clears the inference engine's in-memory cache.

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
| `browsing` | Browsing activity events |

If `"types"` is omitted, all three event types are returned (backward compatible).

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

##### Inference record fields

| Field | Type | Description |
|---|---|---|
| `entity_key` | `string` | Lowercase entity identifier (file path, app path, or URL). |
| `entity_type` | `string` | `"file"`, `"app"`, or `"url"`. |
| `last_event_ts` | `integer` | Timestamp of the most recent event of any kind. |
| `last_open_ts` | `integer` | Timestamp of the most recent OPEN (files) or launch (apps/URLs). |
| `last_edit_ts` | `integer` | Timestamp of the most recent MODIFY/CREATE (files only). |
| `open_count_7d` | `integer` | Number of OPEN events in the rolling 7-day window. |
| `open_count_30d` | `integer` | Number of OPEN events in the rolling 30-day window. |
| `open_count_total` | `integer` | Lifetime OPEN count since WARP started tracking this entity. |
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
|-- ActivityDatabase.h          Database class interface (3 tables)
|-- ActivityDatabase.cpp        SQLite storage implementation
|-- FileMonitor.h               File/shell monitoring interface
|-- FileMonitor.cpp             ReadDirectoryChangesW + SHChangeNotify + ETW impl
|-- AppLaunchMonitor.h          App launch monitoring interface
|-- AppLaunchMonitor.cpp        Process table polling impl
|-- BrowsingMonitor.h           Browsing activity monitoring interface
|-- BrowsingMonitor.cpp         Foreground window title polling impl
|-- IdleDetector.h              Idle/sleep detector interface
|-- IdleDetector.cpp            GetLastInputInfo + WM_POWERBROADCAST impl
|-- QueryApi.h                  Named-pipe API interface
|-- QueryApi.cpp                Pipe server and JSON serialization impl
|-- InferenceEngine.h           Inference engine interface (per-entity analytics)
|-- InferenceEngine.cpp         Real-time scoring, QueryInferences & GetInferenceDeltas impl
|
|-- sqlite3.c                   SQLite amalgamation (compiled as C)
+-- sqlite3.h                   SQLite public header
```

---

## License

SQLite is in the [public domain](https://www.sqlite.org/copyright.html).


## Author

Suman Ghosh — [@sumanthewhiz](https://github.com/sumanthewhiz)

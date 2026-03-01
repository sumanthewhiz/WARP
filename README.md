# WARP! — Windows Activity Recording & Playback

WARP! is a lightweight Windows desktop application that silently monitors all file and folder
activity on the local PC, stores it in a rolling 30-day on-disk database, and exposes a
queryable named-pipe API so that other applications running on the same machine can
programmatically retrieve file activity history.

The application starts minimized to the system tray (notification area), requires
administrator privileges, and is designed to run continuously in the background for as
long as the PC is actively being used.

---

## Features

| Feature | Details |
|---|---|
| **System-tray residence** | Launches hidden; a tray icon (light-bulb) provides *Open* and *Exit* right-click menu items. Double-clicking the icon also opens the window. |
| **Administrator privileges** | The executable manifest requests `requireAdministrator`, so a UAC prompt is shown on launch. |
| **Idle / sleep awareness** | Monitoring pauses automatically when the PC has been idle for ? 2 minutes or enters sleep/hibernate, and resumes the moment user input is detected or the PC wakes. |
| **File-system monitoring** | Every fixed, removable, and network drive is watched recursively via `ReadDirectoryChangesW`. |
| **Shell-level monitoring** | `SHChangeNotifyRegister` on the entire shell namespace catches higher-level operations (copy, move, shell renames) that pure file-system notifications may miss. |
| **SQLite storage** | All events are persisted in `%LOCALAPPDATA%\WARP\activity.db` using WAL mode. |
| **30-day rolling eviction** | Records older than 30 days are deleted on startup and every 6 hours thereafter. |
| **Named-pipe query API** | Other Windows processes can connect to `\\.\pipe\WarpFileActivityAPI` and retrieve activity data as JSON for any supported time window. |

---

## Architecture

```
???????????????????????????????????????????????????????????????
?                        WARP!.cpp                            ?
?                    (Win32 entry point)                       ?
?  ????????????  ????????????  ??????????????  ????????????  ?
?  ?FileMonitor?  ?IdleDetect?  ?ActivityDB  ?  ? QueryApi ?  ?
?  ? .h/.cpp  ?  ? or .h/.cpp?  ? .h/.cpp   ?  ? .h/.cpp  ?  ?
?  ????????????  ????????????  ??????????????  ????????????  ?
?       ?              ?              ?               ?        ?
?       ?  callback    ?  pause/      ?  insert/      ?  read  ?
?       ????????????????  resume      ?  query        ??????????
?       ?              ????????????????               ?        ?
?       ?              ?              ?               ?        ?
????????????????????????????????????????????????????????????????
        ?              ?              ?               ?
   ReadDirectory   GetLastInputInfo  SQLite       Named Pipe
   ChangesW /      WM_POWERBROADCAST activity.db  \\.\pipe\
   SHChangeNotify                                 WarpFileActivityAPI
```

### Component overview

#### `WARP!.cpp` — Application shell

The Win32 entry point. Creates the main (hidden) window with a single static-text
label, sets up the system-tray icon and its context menu, starts all background
subsystems, and runs the message loop. Closing or minimizing the window hides it
back to the tray; only **Exit** from the tray menu terminates the process.

#### `FileMonitor` (`FileMonitor.h` / `FileMonitor.cpp`)

Spawns one background thread per logical drive (fixed, removable, or network) that
calls `ReadDirectoryChangesW` in overlapped mode with recursive watching. A
separate thread registers for shell change notifications via
`SHChangeNotifyRegister` on the desktop namespace root to capture shell-level
events.

Detected actions: **CREATE**, **DELETE**, **MODIFY**, **RENAME**.

The monitor exposes `Pause()` / `Resume()` methods that the idle detector calls to
suspend event recording when the PC is inactive.

#### `IdleDetector` (`IdleDetector.h` / `IdleDetector.cpp`)

A polling thread that checks `GetLastInputInfo` every 5 seconds. When the idle
duration exceeds the configured threshold (default 2 minutes) the `onIdle` callback
fires; when input resumes, the `onActive` callback fires.

A message-only window is also created to receive `WM_POWERBROADCAST` notifications
(`PBT_APMSUSPEND` / `PBT_APMRESUMEAUTOMATIC`) so that sleep/wake transitions
trigger the same pause/resume path.

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
    action    TEXT    NOT NULL,        -- CREATE | DELETE | MODIFY | RENAME
    path      TEXT    NOT NULL,        -- full path of the affected file/folder
    old_path  TEXT                     -- previous path (RENAME only; NULL otherwise)
);

CREATE INDEX idx_activity_ts     ON file_activity(timestamp);
CREATE INDEX idx_activity_action ON file_activity(action, timestamp);
```

**Pragmas:** `journal_mode=WAL`, `synchronous=NORMAL`.

**Eviction:** `DELETE FROM file_activity WHERE timestamp < (now ? 30 days)` runs on
startup and every 6 hours via a `WM_TIMER`.

#### `QueryApi` (`QueryApi.h` / `QueryApi.cpp`)

A named-pipe server listening on:

```
\\.\pipe\WarpFileActivityAPI
```

The pipe is created with `PIPE_UNLIMITED_INSTANCES` so multiple clients can connect
concurrently. Each client connection is served on a detached thread.

---

## Building

**Prerequisites:**

- Visual Studio 2022 (v143 toolset)
- Windows 10 SDK (10.0 or later)
- C++14 standard

The project compiles SQLite as an embedded amalgamation (`sqlite3.c` / `sqlite3.h`);
no external dependencies are required.

Open `WARP!.sln` in Visual Studio and build any configuration (Debug/Release ×
Win32/x64/ARM64). The output executable will request administrator privileges
automatically via the linker's UAC manifest setting.

---

## Runtime behaviour

1. On launch the app requests elevation (UAC prompt).
2. The main window is created but kept hidden; a system-tray icon appears.
3. The SQLite database is opened (created if it doesn't exist) and old records are evicted.
4. File-system and shell monitors start on all eligible drives.
5. The idle detector begins polling for user inactivity and power events.
6. The named-pipe server starts accepting connections.
7. Every detected file activity is inserted into the database in real time.
8. Every 6 hours, records older than 30 days are purged.
9. When the user is idle ? 2 min or the PC sleeps, monitoring pauses; it resumes on activity/wake.
10. Right-clicking the tray icon shows **Open** / **Exit**. *Open* shows the window; *Exit* tears everything down cleanly.

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

Send a single UTF-8 JSON message. Two request shapes are supported:

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

### Response format

A single UTF-8 JSON message:

```json
{
    "count": 3,
    "activities": [
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
}
```

#### Response fields

| Field | Type | Description |
|---|---|---|
| `count` | integer | Total number of activity records returned. |
| `activities` | array | Ordered list of activity objects (most recent first). |
| `activities[].id` | integer | Auto-increment row ID. |
| `activities[].timestamp` | integer | Unix epoch seconds (UTC) when the event was recorded. |
| `activities[].action` | string | One of: `CREATE`, `DELETE`, `MODIFY`, `RENAME`. |
| `activities[].path` | string | Full path of the affected file or folder. Backslashes are JSON-escaped (`\\`). |
| `activities[].old_path` | string *(optional)* | Present only for `RENAME` actions. The previous full path before the rename. |

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

    // Send request
    const char* request = R"({"window":"15m"})";
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
import struct
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

# Query last 30 minutes of activity
request = json.dumps({"window": "30m"}).encode("utf-8")
win32file.WriteFile(handle, request)

_, response_bytes = win32file.ReadFile(handle, 65536)
handle.Close()

data = json.loads(response_bytes.decode("utf-8"))
print(f"Total events: {data['count']}")
for activity in data["activities"]:
    print(f"  [{activity['action']}] {activity['path']}")
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
$response | ConvertFrom-Json | Format-List
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

byte[] request = Encoding.UTF8.GetBytes("""{"window":"6h"}""");
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
??? WARP!.sln                   Solution file
??? WARP!.vcxproj               Project file (v143, C++14, Unicode)
??? WARP!.vcxproj.filters       IDE filter assignments
??? WARP!.vcxproj.user          Per-user project settings
?
??? WARP!.cpp                   Application entry point, window, tray icon
??? WARP!.h                     App header (includes resource.h)
??? WARP!.rc                    Resource script (icons, strings, dialogs)
??? WARP!.ico                   Application icon (light bulb)
??? small.ico                   Small application icon
??? Resource.h                  Resource ID definitions
??? framework.h                 Precompiled / common system headers
??? targetver.h                 Windows SDK version targeting
?
??? ActivityDatabase.h          Database class interface
??? ActivityDatabase.cpp         SQLite storage implementation
??? FileMonitor.h               File/shell monitoring interface
??? FileMonitor.cpp              ReadDirectoryChangesW + SHChangeNotify impl
??? IdleDetector.h              Idle/sleep detector interface
??? IdleDetector.cpp             GetLastInputInfo + WM_POWERBROADCAST impl
??? QueryApi.h                  Named-pipe API interface
??? QueryApi.cpp                 Pipe server and JSON serialization impl
?
??? sqlite3.c                   SQLite amalgamation (compiled as C)
??? sqlite3.h                   SQLite public header
```

---

## License

SQLite is in the [public domain](https://www.sqlite.org/copyright.html).

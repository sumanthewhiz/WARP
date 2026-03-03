# WARP Activity API -- Integration Guide

> **Version:** 2.0
> **Pipe endpoint:** `\\.\pipe\WarpFileActivityAPI`
> **Transport:** Windows Named Pipe (message mode)
> **Encoding:** UTF-8 JSON

This document describes how third-party applications running on the same Windows
machine can connect to the WARP service and query activity history -- including
file/folder events, application launches, and browsing activity -- to make
better-informed decisions in their own workflows.

---

## Table of Contents

- [Overview](#overview)
- [Prerequisites](#prerequisites)
- [Connection](#connection)
- [Request Format](#request-format)
  - [Predefined Time Window](#1-predefined-time-window)
  - [Custom Time Range](#2-custom-time-range-in-seconds)
  - [Default Query](#3-default-query)
  - [Event Type Filtering](#4-event-type-filtering)
- [Response Format](#response-format)
  - [Top-Level Structure](#top-level-structure)
  - [File Activity Fields](#file-activity-fields)
  - [File Action Types](#file-action-types)
  - [App Launch Activity Fields](#app-launch-activity-fields)
  - [Browsing Activity Fields](#browsing-activity-fields)
- [Error Handling](#error-handling)
- [Data Retention & Limits](#data-retention--limits)
- [Integration Patterns](#integration-patterns)
  - [Recent File Picker](#1-recent-file-picker)
  - [Backup & Sync Engine](#2-backup--sync-engine)
  - [Security Auditing](#3-security-auditing)
  - [Developer Tooling](#4-developer-tooling)
  - [Smart Cleanup Utility](#5-smart-cleanup-utility)
  - [App Usage Analytics](#6-app-usage-analytics)
  - [Browsing History Dashboard](#7-browsing-history-dashboard)
- [Client Examples](#client-examples)
  - [C++ (Win32)](#c-win32)
  - [C# (.NET)](#c-net)
  - [Python](#python)
  - [PowerShell](#powershell)
  - [Rust](#rust)
  - [Node.js](#nodejs)
- [Best Practices](#best-practices)
- [Troubleshooting](#troubleshooting)
- [Limitations](#limitations)
- [Schema Reference](#schema-reference)

---

## Overview

WARP continuously records three categories of user activity on the local PC:

| Event Type | What is Captured | Source |
|---|---|---|
| **File activity** | Creates, opens, modifications, deletions, renames of files and folders | `ReadDirectoryChangesW`, `SHChangeNotifyRegister`, NT Kernel Logger ETW |
| **App launches** | Every user-initiated application start (exe name, path, PID) | Process table polling via `CreateToolhelp32Snapshot` (every 2 seconds) |
| **Browsing activity** | Browser page title changes and URLs when a browser is in the foreground | Foreground window polling via `GetForegroundWindow` + `GetWindowText` (every 3 seconds) |

All events are stored in a local SQLite database with a 30-day rolling window. Any
Windows process running on the same machine can connect to the WARP named pipe and
retrieve this history as structured JSON, optionally filtered by event type.

**Typical use cases:**

| Use Case | How WARP Helps |
|---|---|
| Show "recently opened" files | Query `file` type, filter for `OPEN` actions |
| Incremental backup | Query `file` type, filter `CREATE` + `MODIFY` actions since last sync |
| Audit trail | Query all types for the full 30-day window |
| Dev tooling | Query `file` type for `MODIFY` actions matching your project directory |
| Smart cleanup | Query `file` type for 30 days, find untouched files |
| App usage analytics | Query `app_launch` type to see which apps were used and when |
| Browsing dashboard | Query `browsing` type to review page titles and URLs visited |

---

## Prerequisites

| Requirement | Details |
|---|---|
| **WARP must be running** | The named pipe is only available while the WARP process is active. WARP runs as a system-tray application and starts automatically if configured. |
| **Administrator privileges** | WARP itself requires admin to start (for kernel-level ETW tracing). Your client application does **not** need admin privileges to connect to the pipe. |
| **Same machine** | The pipe is local-only (`\\.\pipe\...`). Remote connections are not supported. |
| **Windows 10 or later** | WARP uses NT Kernel Logger ETW, which requires Windows 10+. |

---

## Connection

### Pipe Details

| Property | Value |
|---|---|
| **Pipe name** | `\\.\pipe\WarpFileActivityAPI` |
| **Pipe mode** | `PIPE_TYPE_MESSAGE \| PIPE_READMODE_MESSAGE` |
| **Access** | `GENERIC_READ \| GENERIC_WRITE` |
| **Concurrency** | `PIPE_UNLIMITED_INSTANCES` -- multiple clients can connect simultaneously |
| **Max message size** | 64 KB (65,536 bytes) for both request and response buffers |

### Connection Lifecycle

1. **Open** the pipe with `CreateFile` (or your language's equivalent).
2. **Set message mode** via `SetNamedPipeHandleState` with `PIPE_READMODE_MESSAGE`.
3. **Write** a single UTF-8 JSON request message.
4. **Read** a single UTF-8 JSON response message.
5. **Close** the pipe handle.

Each connection is one request -> one response -> close. To make multiple queries,
open a new connection for each.

---

## Request Format

Send a single UTF-8 JSON object. The following fields are supported:

### 1. Predefined Time Window

```json
{ "window": "<window_code>" }
```

| Code | Time Range | Seconds |
|---|---|---|
| `15m` | Last 15 minutes | 900 |
| `30m` | Last 30 minutes | 1,800 |
| `1h` | Last 1 hour | 3,600 |
| `2h` | Last 2 hours | 7,200 |
| `6h` | Last 6 hours | 21,600 |
| `24h` | Last 24 hours | 86,400 |
| `7d` | Last 7 days | 604,800 |
| `15d` | Last 15 days | 1,296,000 |
| `30d` | Last 30 days | 2,592,000 |

**Example -- get the last 6 hours of activity:**

```json
{"window":"6h"}
```

### 2. Custom Time Range (in seconds)

```json
{ "seconds": <positive_integer> }
```

Any positive integer is accepted. The value represents the number of seconds to
look back from the current time.

**Example -- get the last 5 minutes:**

```json
{"seconds":300}
```

### 3. Default Query

```json
{}
```

An empty JSON object (or any request without a `window` or `seconds` field)
defaults to the **last 1 hour**.

### 4. Event Type Filtering

Any request can include a `"types"` array to specify which event categories to
include in the response:

```json
{ "window": "1h", "types": ["file", "app_launch", "browsing"] }
```

| Type value | Description |
|---|---|
| `file` | File/folder activity (creates, opens, modifications, deletes, renames) |
| `app_launch` | Application launch events (exe name, path, PID) |
| `browsing` | Browsing activity events (browser, page title, URL) |

**If `"types"` is omitted**, all three event types are returned (backward
compatible with v1.0 clients that don't send `"types"`).

**Examples:**

```json
{"window":"15m","types":["file"]}
{"seconds":300,"types":["app_launch","browsing"]}
{"window":"1h","types":["file","app_launch","browsing"]}
{"types":["browsing"]}
{}
```

---

## Response Format

The response is a single UTF-8 JSON message. Event types are **segregated at the
root level** -- each requested type appears as a separate top-level object. Only
the types that were requested (or all, if `"types"` was omitted) are included.

### Top-Level Structure

```json
{
    "file_activities": {
        "count": <integer>,
        "events": [ ... ]
    },
    "app_launch_activities": {
        "count": <integer>,
        "events": [ ... ]
    },
    "browsing_activities": {
        "count": <integer>,
        "events": [ ... ]
    }
}
```

Each section has:
- `count` -- the number of events in that category.
- `events` -- an array of event objects, ordered **most recent first** (descending
  by `timestamp`).

### File Activity Fields

| Field | Type | Presence | Description |
|---|---|---|---|
| `id` | `integer` | Always | Auto-increment database row ID. |
| `timestamp` | `integer` | Always | Unix epoch seconds (UTC). |
| `action` | `string` | Always | The type of file/folder activity. See [File Action Types](#file-action-types). |
| `path` | `string` | Always | Full absolute path. Backslashes are JSON-escaped as `\\`. |
| `old_path` | `string` | **RENAME only** | The previous full path before the rename/move. |

**Example event:**

```json
{
    "id": 1042,
    "timestamp": 1750012345,
    "action": "CREATE",
    "path": "C:\\Users\\Alice\\Documents\\report.docx"
}
```

### File Action Types

| Action | Meaning | `old_path` present? |
|---|---|---|
| `CREATE` | A new file or folder was created. | No |
| `OPEN` | A file or folder was opened / accessed. | No |
| `MODIFY` | A file or folder was written to. | No |
| `DELETE` | A file or folder was deleted. | No |
| `RENAME` | A file or folder was renamed or moved. | **Yes** |

### App Launch Activity Fields

| Field | Type | Presence | Description |
|---|---|---|---|
| `id` | `integer` | Always | Auto-increment database row ID. |
| `timestamp` | `integer` | Always | Unix epoch seconds (UTC) when the process was first detected. |
| `exe_name` | `string` | Always | Executable filename (e.g. `"notepad.exe"`). |
| `exe_path` | `string` | Always | Full path to the executable. Backslashes are JSON-escaped. |
| `pid` | `integer` | Always | Process ID at launch time. |

**Example event:**

```json
{
    "id": 15,
    "timestamp": 1750012400,
    "exe_name": "notepad.exe",
    "exe_path": "C:\\Windows\\System32\\notepad.exe",
    "pid": 12340
}
```

**Notes:**
- Only interactive user-session processes are captured (session >= 1).
- System processes (svchost, dwm, csrss, SearchIndexer, Defender, etc.) are excluded.
- WARP's own process is excluded.

### Browsing Activity Fields

| Field | Type | Presence | Description |
|---|---|---|---|
| `id` | `integer` | Always | Auto-increment database row ID. |
| `timestamp` | `integer` | Always | Unix epoch seconds (UTC). |
| `browser` | `string` | Always | Browser identifier. See table below. |
| `title` | `string` | Always | Page title (browser suffix stripped). |
| `url` | `string` | **Optional** | URL if extractable from the title bar; absent otherwise. |

**Browser identifiers:**

| Value | Browser |
|---|---|
| `chrome` | Google Chrome |
| `msedge` | Microsoft Edge |
| `firefox` | Mozilla Firefox |
| `brave` | Brave |
| `opera` | Opera |
| `vivaldi` | Vivaldi |
| `ie` | Internet Explorer |

**Example event:**

```json
{
    "id": 8,
    "timestamp": 1750012350,
    "browser": "chrome",
    "title": "GitHub - Pull Requests",
    "url": "https://github.com/pulls"
}
```

**Notes:**
- Events are only captured while a recognized browser window is in the foreground.
- Duplicate titles (same browser + same title) are suppressed until the title changes.
- Common browser suffixes ("- Google Chrome", "- Microsoft Edge", etc.) are stripped.

---

## Error Handling

| Scenario | What Happens |
|---|---|
| WARP is not running | `CreateFile` fails with `ERROR_FILE_NOT_FOUND` (error 2). Your app should retry or inform the user. |
| Malformed JSON request | WARP treats it as a default query and returns the last 1 hour of all event types. |
| Unknown `window` code | WARP falls back to `1h` (last 1 hour). |
| Unknown value in `types` array | That value is silently ignored. If no valid types remain, all types are returned. |
| Negative or zero `seconds` | The query returns empty result sets (`"count": 0`). |
| No activity in the time range | Returns sections with `"count": 0` and empty `"events": []`. |
| Response exceeds 64 KB | Very large result sets may be truncated at the pipe buffer boundary. Use a shorter time window, fewer event types, or custom seconds to reduce result size. |

### Checking if WARP is Available

```cpp
// Quick availability check (C++)
HANDLE hPipe = CreateFileW(L"\\\\.\\pipe\\WarpFileActivityAPI",
    GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
bool available = (hPipe != INVALID_HANDLE_VALUE);
if (available) CloseHandle(hPipe);
```

```python
# Quick availability check (Python)
import os
available = os.path.exists(r"\\.\pipe\WarpFileActivityAPI")
```

```csharp
// Quick availability check (C#)
bool available = System.IO.File.Exists(@"\\.\pipe\WarpFileActivityAPI");
```

---

## Data Retention & Limits

| Property | Value |
|---|---|
| **Retention period** | 30 days (rolling). Records older than 30 days are evicted from all three tables on startup and every 6 hours. |
| **Maximum queryable range** | 30 days (`"window":"30d"` or `"seconds":2592000`). |
| **Database location** | `%LOCALAPPDATA%\WARP\activity.db` |
| **Storage format** | SQLite 3 with WAL mode |
| **Timestamp precision** | 1-second resolution (Unix epoch seconds, UTC) |
| **OPEN event dedup window** | 2 seconds -- multiple kernel-level opens of the same file within 2 seconds are collapsed into one event. |
| **Browsing dedup** | Same browser + same title is reported only once until the title changes. |
| **App launch polling interval** | 2 seconds |
| **Browsing polling interval** | 3 seconds |

---

## Integration Patterns

### 1. Recent File Picker

Show users the files they opened most recently:

```json
{"window":"15m","types":["file"]}
```

Filter the response `file_activities.events` for `action == "OPEN"` and display
unique paths.

### 2. Backup & Sync Engine

Find all files created or modified since the last backup:

```json
{"seconds":3600,"types":["file"]}
```

Filter for `action == "CREATE"` or `action == "MODIFY"`. Use the `id` field to
track the high-water mark.

### 3. Security Auditing

Generate a compliance report of all activity over the past week:

```json
{"window":"7d"}
```

Log every event across all three types. The `RENAME` action with `old_path` lets
you track file provenance. App launch events show what was executed. Browsing
events show what was accessed online.

### 4. Developer Tooling

Detect which source files changed during a build:

```json
{"seconds":120,"types":["file"]}
```

Filter for `action == "MODIFY"` and paths matching your project directory.

### 5. Smart Cleanup Utility

Find files that haven't been touched recently:

```json
{"window":"30d","types":["file"]}
```

Collect all unique `path` values with `action == "OPEN"` or `action == "MODIFY"`.

### 6. App Usage Analytics

Show which applications the user launched today:

```json
{"window":"24h","types":["app_launch"]}
```

Group `app_launch_activities.events` by `exe_name` and count occurrences.

### 7. Browsing History Dashboard

Show pages visited in the last hour:

```json
{"window":"1h","types":["browsing"]}
```

Display `browsing_activities.events` with `browser`, `title`, and `url` fields.

---

## Client Examples

### C++ (Win32)

```cpp
#include <windows.h>
#include <cstdio>
#include <cstring>

int main()
{
    HANDLE hPipe = CreateFileW(
        L"\\\\.\\pipe\\WarpFileActivityAPI",
        GENERIC_READ | GENERIC_WRITE,
        0, nullptr, OPEN_EXISTING, 0, nullptr);

    if (hPipe == INVALID_HANDLE_VALUE) {
        printf("WARP is not running (error %lu)\n", GetLastError());
        return 1;
    }

    DWORD mode = PIPE_READMODE_MESSAGE;
    SetNamedPipeHandleState(hPipe, &mode, nullptr, nullptr);

    // Request last 15 minutes of app launches and browsing
    const char* request = R"({"window":"15m","types":["app_launch","browsing"]})";
    DWORD written = 0;
    WriteFile(hPipe, request, (DWORD)strlen(request), &written, nullptr);

    char buffer[65536] = {};
    DWORD bytesRead = 0;
    ReadFile(hPipe, buffer, sizeof(buffer) - 1, &bytesRead, nullptr);

    printf("Response (%lu bytes):\n%s\n", bytesRead, buffer);

    CloseHandle(hPipe);
    return 0;
}
```

### C# (.NET)

```csharp
using System;
using System.IO.Pipes;
using System.Text;
using System.Text.Json;

using var pipe = new NamedPipeClientStream(".", "WarpFileActivityAPI",
    PipeDirection.InOut);
pipe.Connect(5000);
pipe.ReadMode = PipeTransmissionMode.Message;

// Request all event types for the last 6 hours
byte[] request = Encoding.UTF8.GetBytes("""{"window":"6h"}""");
pipe.Write(request, 0, request.Length);

byte[] buffer = new byte[65536];
int bytesRead = pipe.Read(buffer, 0, buffer.Length);
string json = Encoding.UTF8.GetString(buffer, 0, bytesRead);

using var doc = JsonDocument.Parse(json);

// File activities
if (doc.RootElement.TryGetProperty("file_activities", out var fa))
{
    int count = fa.GetProperty("count").GetInt32();
    Console.WriteLine($"File events: {count}");
    foreach (var e in fa.GetProperty("events").EnumerateArray())
        Console.WriteLine($"  [{e.GetProperty("action").GetString()}] {e.GetProperty("path").GetString()}");
}

// App launches
if (doc.RootElement.TryGetProperty("app_launch_activities", out var al))
{
    int count = al.GetProperty("count").GetInt32();
    Console.WriteLine($"App launches: {count}");
    foreach (var e in al.GetProperty("events").EnumerateArray())
        Console.WriteLine($"  {e.GetProperty("exe_name").GetString()} (PID {e.GetProperty("pid").GetInt32()})");
}

// Browsing activities
if (doc.RootElement.TryGetProperty("browsing_activities", out var ba))
{
    int count = ba.GetProperty("count").GetInt32();
    Console.WriteLine($"Browsing events: {count}");
    foreach (var e in ba.GetProperty("events").EnumerateArray())
        Console.WriteLine($"  [{e.GetProperty("browser").GetString()}] {e.GetProperty("title").GetString()}");
}
```

### Python

```python
import json
import win32file
import win32pipe

PIPE_NAME = r"\\.\pipe\WarpFileActivityAPI"

def query_warp(request_obj: dict) -> dict:
    """Connect to WARP and return the parsed JSON response."""
    handle = win32file.CreateFile(
        PIPE_NAME,
        win32file.GENERIC_READ | win32file.GENERIC_WRITE,
        0, None,
        win32file.OPEN_EXISTING,
        0, None,
    )
    win32pipe.SetNamedPipeHandleState(
        handle, win32pipe.PIPE_READMODE_MESSAGE, None, None
    )

    request = json.dumps(request_obj).encode("utf-8")
    win32file.WriteFile(handle, request)

    _, response_bytes = win32file.ReadFile(handle, 65536)
    handle.Close()

    return json.loads(response_bytes.decode("utf-8"))


# --- Example: Get all activity for the last 15 minutes ---
data = query_warp({"window": "15m"})

if "file_activities" in data:
    fa = data["file_activities"]
    print(f"File events: {fa['count']}")
    for e in fa["events"][:5]:
        print(f"  [{e['action']}] {e['path']}")

if "app_launch_activities" in data:
    al = data["app_launch_activities"]
    print(f"\nApp launches: {al['count']}")
    for e in al["events"][:5]:
        print(f"  {e['exe_name']} (PID {e['pid']})")

if "browsing_activities" in data:
    ba = data["browsing_activities"]
    print(f"\nBrowsing events: {ba['count']}")
    for e in ba["events"][:5]:
        url_str = f" - {e['url']}" if 'url' in e else ""
        print(f"  [{e['browser']}] {e['title']}{url_str}")

# --- Example: Only browsing activity for the last 2 hours ---
data = query_warp({"window": "2h", "types": ["browsing"]})
for e in data.get("browsing_activities", {}).get("events", []):
    print(f"  [{e['browser']}] {e['title']}")
```

### PowerShell

```powershell
function Query-Warp {
    param(
        [string]$Request = '{}'
    )

    $pipe = New-Object System.IO.Pipes.NamedPipeClientStream(
        ".", "WarpFileActivityAPI",
        [System.IO.Pipes.PipeDirection]::InOut)

    try {
        $pipe.Connect(5000)
        $pipe.ReadMode = [System.IO.Pipes.PipeTransmissionMode]::Message

        $requestBytes = [Text.Encoding]::UTF8.GetBytes($Request)
        $pipe.Write($requestBytes, 0, $requestBytes.Length)

        $buffer = New-Object byte[] 65536
        $bytesRead = $pipe.Read($buffer, 0, $buffer.Length)
        $response = [Text.Encoding]::UTF8.GetString($buffer, 0, $bytesRead)

        return $response | ConvertFrom-Json
    }
    finally {
        $pipe.Close()
    }
}

# Get last 1 hour of all activity
$result = Query-Warp '{"window":"1h"}'
Write-Host "File events: $($result.file_activities.count)"
Write-Host "App launches: $($result.app_launch_activities.count)"
Write-Host "Browsing events: $($result.browsing_activities.count)"

# Show recent app launches
$result.app_launch_activities.events |
    Select-Object timestamp, exe_name, pid |
    Format-Table -AutoSize

# Only browsing activity for the last 24 hours
$browse = Query-Warp '{"window":"24h","types":["browsing"]}'
$browse.browsing_activities.events |
    Select-Object timestamp, browser, title |
    Format-Table -AutoSize
```

### Rust

```rust
use std::io::{Read, Write};
use std::fs::OpenOptions;

fn query_warp(request: &str) -> Result<String, Box<dyn std::error::Error>> {
    let mut pipe = OpenOptions::new()
        .read(true)
        .write(true)
        .open(r"\\.\pipe\WarpFileActivityAPI")?;

    pipe.write_all(request.as_bytes())?;
    pipe.flush()?;

    let mut buf = [0u8; 65536];
    let n = pipe.read(&mut buf)?;
    Ok(std::str::from_utf8(&buf[..n])?.to_string())
}

fn main() {
    match query_warp(r#"{"window":"15m","types":["app_launch"]}"#) {
        Ok(json) => println!("{}", json),
        Err(e) => eprintln!("Failed to query WARP: {}", e),
    }
}
```

> **Note:** For proper message-mode pipe handling in Rust, consider using the
> `windows` or `winapi` crate to call `SetNamedPipeHandleState` with
> `PIPE_READMODE_MESSAGE`.

### Node.js

```javascript
const net = require('net');

function queryWarp(request) {
    return new Promise((resolve, reject) => {
        const client = net.connect('\\\\.\\pipe\\WarpFileActivityAPI', () => {
            client.write(JSON.stringify(request));
        });

        let data = '';
        client.on('data', (chunk) => { data += chunk.toString('utf8'); });
        client.on('end', () => {
            try { resolve(JSON.parse(data)); }
            catch (e) { reject(e); }
        });
        client.on('error', reject);
    });
}

// Example: Get last 30 minutes of all activity
(async () => {
    try {
        const result = await queryWarp({ window: '30m' });

        // File activities
        if (result.file_activities) {
            console.log(`File events: ${result.file_activities.count}`);
        }

        // App launches
        if (result.app_launch_activities) {
            console.log(`App launches: ${result.app_launch_activities.count}`);
            for (const e of result.app_launch_activities.events.slice(0, 5)) {
                console.log(`  ${e.exe_name} (PID ${e.pid})`);
            }
        }

        // Browsing activity
        if (result.browsing_activities) {
            console.log(`Browsing events: ${result.browsing_activities.count}`);
            for (const e of result.browsing_activities.events.slice(0, 5)) {
                console.log(`  [${e.browser}] ${e.title}`);
            }
        }
    } catch (err) {
        console.error('WARP is not available:', err.message);
    }
})();
```

---

## Best Practices

### 1. Use Short Time Windows

Prefer the shortest time window that meets your needs. Querying `15m` is
significantly faster than `30d` when the database contains millions of records.

### 2. Request Only the Types You Need

Use the `"types"` array to limit the response to the event categories your
application actually uses. This reduces response size and database query time.

```json
{"window":"1h","types":["file"]}
```

### 3. Filter Client-Side

Within each event type, WARP returns all events in the requested window. Filter by
`action`, `path` prefix, `exe_name`, `browser`, etc. in your client code.

### 4. Track High-Water Mark with `id`

For incremental processing (e.g., backup sync), remember the highest `id` from
your last query. On the next query, discard any events with `id` <= your
high-water mark. Each event type has its own independent `id` sequence.

### 5. Handle Pipe Unavailability Gracefully

WARP may not be running. Always handle `CreateFile` failure and implement a
fallback path in your application.

### 6. One Connection Per Query

Open a fresh pipe connection for each query. Don't try to reuse the handle for
multiple request/response cycles -- the pipe server expects one message per
connection.

### 7. Parse Timestamps as UTC

The `timestamp` field is Unix epoch seconds in **UTC**. Convert to local time
in your application if needed for display.

### 8. Normalize Paths for Comparison

Paths are returned with native Windows casing. When comparing paths from
different queries or with your own file lists, use case-insensitive comparison.

---

## Troubleshooting

| Problem | Cause | Solution |
|---|---|---|
| `CreateFile` returns `INVALID_HANDLE_VALUE` with error 2 | WARP is not running | Start WARP. It requires Administrator privileges. |
| `CreateFile` returns `INVALID_HANDLE_VALUE` with error 231 | All pipe instances are busy | Retry after a short delay (100-500 ms). |
| Response is empty or all counts are 0 | No activity in the requested time range, or WARP was recently started | Try a wider time window. WARP only records events while it's running. |
| `OPEN` events are missing | WARP's ETW trace may have failed to start | Ensure WARP is running with Administrator privileges. |
| App launch events missing for a known app | The app may be in the system process blocklist | Only interactive user-session processes not in the blocklist are captured. |
| Browsing events missing | The browser may not be in the foreground, or is not a recognized browser | Events are only captured while a supported browser window is the foreground window. |
| Duplicate file events for the same file | Normal -- a single user action can trigger multiple kernel events | Deduplicate by path + action within a short time window in your client. |
| `ReadFile` returns partial data | Response exceeded the 64 KB pipe buffer | Use a shorter time window or fewer event types to reduce result size. |

---

## Limitations

1. **Local only** -- The named pipe is not accessible from remote machines.
2. **No server-side filtering by path or action** -- You cannot filter by action
   type, path, exe_name, or browser in the request. All matching events for the
   requested types are returned; filter in your client.
3. **No streaming / subscription** -- The API is request/response only. There is no
   push notification or event stream. Poll periodically if you need near-real-time
   updates.
4. **30-day maximum** -- Events older than 30 days are automatically evicted.
5. **No authentication** -- Any local process can connect to the pipe. The data
   may contain sensitive information (file paths, browsing titles, URLs).
6. **64 KB response limit** -- Very large result sets may be truncated. Use shorter
   time windows or specific event types for busy systems.
7. **1-second timestamp granularity** -- Events within the same second have the
   same `timestamp` value. Use `id` for precise ordering within a type.
8. **Excluded paths** -- File activity in system directories (Windows, Program Files,
   AppData, ProgramData, etc.), hidden dot-folders, and common build/cache
   artifacts is excluded by design.
9. **App launch detection latency** -- The process table is polled every 2 seconds,
   so very short-lived processes may be missed.
10. **Browsing detection** -- Only works when the browser is the foreground window.
    Background tab changes are not captured. URL extraction depends on the browser
    showing it in the title bar.

---

## Schema Reference

For advanced users who want to query the SQLite database directly (e.g., for
complex joins or aggregations), here are the schemas:

```sql
-- File/folder activity
CREATE TABLE file_activity (
    id        INTEGER PRIMARY KEY AUTOINCREMENT,
    timestamp INTEGER NOT NULL,       -- Unix epoch seconds (UTC)
    action    TEXT    NOT NULL,        -- CREATE | OPEN | MODIFY | DELETE | RENAME
    path      TEXT    NOT NULL,        -- Full path (UTF-8)
    old_path  TEXT                     -- Previous path (RENAME only; NULL otherwise)
);

CREATE INDEX idx_activity_ts     ON file_activity(timestamp);
CREATE INDEX idx_activity_action ON file_activity(action, timestamp);

-- Application launches
CREATE TABLE app_launch_activity (
    id        INTEGER PRIMARY KEY AUTOINCREMENT,
    timestamp INTEGER NOT NULL,       -- Unix epoch seconds (UTC)
    exe_name  TEXT    NOT NULL,        -- Executable filename
    exe_path  TEXT    NOT NULL,        -- Full path to executable
    pid       INTEGER NOT NULL        -- Process ID at launch time
);

CREATE INDEX idx_app_ts ON app_launch_activity(timestamp);

-- Browsing activity
CREATE TABLE browsing_activity (
    id        INTEGER PRIMARY KEY AUTOINCREMENT,
    timestamp INTEGER NOT NULL,       -- Unix epoch seconds (UTC)
    browser   TEXT    NOT NULL,        -- Browser identifier
    title     TEXT    NOT NULL,        -- Page title (suffix stripped)
    url       TEXT                     -- URL if extractable; NULL otherwise
);

CREATE INDEX idx_browse_ts ON browsing_activity(timestamp);
```

**Database location:** `%LOCALAPPDATA%\WARP\activity.db`
**Pragmas:** `journal_mode=WAL`, `synchronous=NORMAL`

> **Warning:** The database is actively written to by WARP. If you open it
> directly, use `PRAGMA query_only=ON` or open in read-only mode to avoid
> interfering with WARP's WAL writer. The named-pipe API is the recommended
> access method.

---

*This documentation describes WARP API version 2.0. The response format changed
from v1.0 -- event types are now segregated at the root level rather than in a
single flat array. The `"types"` request field is new in v2.0. Requests without
`"types"` return all event types (backward compatible). New fields may be added
to response objects in future versions, but existing fields will not be removed
or change meaning.*

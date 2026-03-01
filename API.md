# WARP File Activity API — Integration Guide

> **Version:** 1.0  
> **Pipe endpoint:** `\\.\pipe\WarpFileActivityAPI`  
> **Transport:** Windows Named Pipe (message mode)  
> **Encoding:** UTF-8 JSON

This document describes how third-party applications running on the same Windows
machine can connect to the WARP service and query file/folder activity history to
make better-informed decisions in their own workflows.

---

## Table of Contents

- [Overview](#overview)
- [Prerequisites](#prerequisites)
- [Connection](#connection)
- [Request Format](#request-format)
  - [Predefined Time Window](#1-predefined-time-window)
  - [Custom Time Range](#2-custom-time-range-in-seconds)
  - [Default Query](#3-default-query)
- [Response Format](#response-format)
  - [Top-Level Fields](#top-level-fields)
  - [Activity Object Fields](#activity-object-fields)
  - [Action Types](#action-types)
- [Error Handling](#error-handling)
- [Data Retention & Limits](#data-retention--limits)
- [Integration Patterns](#integration-patterns)
  - [Recent File Picker](#1-recent-file-picker)
  - [Backup & Sync Engine](#2-backup--sync-engine)
  - [Security Auditing](#3-security-auditing)
  - [Developer Tooling](#4-developer-tooling)
  - [Smart Cleanup Utility](#5-smart-cleanup-utility)
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

---

## Overview

WARP continuously records file and folder activity on the local PC — including
creates, opens/reads, modifications, deletions, and renames — and stores events in
a local SQLite database with a 30-day rolling window. Any Windows process running
on the same machine can connect to the WARP named pipe and retrieve this history
as structured JSON.

**Typical use cases:**

| Use Case | How WARP Helps |
|---|---|
| Show "recently opened" files | Query `OPEN` actions for the last 15 minutes |
| Incremental backup | Query `CREATE` + `MODIFY` actions since last sync |
| Audit trail | Query the full 30-day window for a compliance report |
| Dev tooling | Detect which source files changed during a build |
| Smart cleanup | Find files that haven't been opened in 30 days |

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
| **Concurrency** | `PIPE_UNLIMITED_INSTANCES` — multiple clients can connect simultaneously |
| **Max message size** | 64 KB (65,536 bytes) for both request and response buffers |

### Connection Sequence

```
Client                              WARP Service
  ?                                      ?
  ????? CreateFile(pipe_name) ????????????  Connect
  ?                                      ?
  ????? SetNamedPipeHandleState ??????????  Set message-read mode
  ?     (PIPE_READMODE_MESSAGE)          ?
  ?                                      ?
  ????? WriteFile(JSON request) ??????????  Send query
  ?                                      ?
  ?????? ReadFile(JSON response) ?????????  Receive results
  ?                                      ?
  ????? CloseHandle ??????????????????????  Disconnect
  ?                                      ?
```

### Connection Lifecycle

1. **Open** the pipe with `CreateFile` (or your language's equivalent).
2. **Set message mode** via `SetNamedPipeHandleState` with `PIPE_READMODE_MESSAGE`.
3. **Write** a single UTF-8 JSON request message.
4. **Read** a single UTF-8 JSON response message.
5. **Close** the pipe handle.

Each connection is one request ? one response ? close. To make multiple queries,
open a new connection for each.

---

## Request Format

Send a single UTF-8 JSON object. Three request shapes are supported:

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

**Example — get the last 6 hours of activity:**

```json
{"window":"6h"}
```

### 2. Custom Time Range (in seconds)

```json
{ "seconds": <positive_integer> }
```

Any positive integer is accepted. The value represents the number of seconds to
look back from the current time.

**Example — get the last 5 minutes:**

```json
{"seconds":300}
```

**Example — get the last 45 minutes:**

```json
{"seconds":2700}
```

### 3. Default Query

```json
{}
```

An empty JSON object (or any request without a `window` or `seconds` field)
defaults to the **last 1 hour**.

---

## Response Format

The response is a single UTF-8 JSON message with the following structure:

```json
{
    "count": 5,
    "activities": [
        {
            "id": 1042,
            "timestamp": 1750012345,
            "action": "OPEN",
            "path": "C:\\Users\\Alice\\Documents\\report.docx"
        },
        {
            "id": 1041,
            "timestamp": 1750012300,
            "action": "MODIFY",
            "path": "C:\\Users\\Alice\\Documents\\report.docx"
        },
        {
            "id": 1040,
            "timestamp": 1750012290,
            "action": "CREATE",
            "path": "C:\\Users\\Alice\\Documents\\report.docx"
        },
        {
            "id": 1039,
            "timestamp": 1750012200,
            "action": "RENAME",
            "path": "C:\\Users\\Alice\\Documents\\draft-v2.docx",
            "old_path": "C:\\Users\\Alice\\Documents\\draft.docx"
        },
        {
            "id": 1038,
            "timestamp": 1750012100,
            "action": "DELETE",
            "path": "C:\\Users\\Alice\\Downloads\\temp.zip"
        }
    ]
}
```

### Top-Level Fields

| Field | Type | Description |
|---|---|---|
| `count` | `integer` | Total number of activity records in this response. |
| `activities` | `array` | Ordered list of activity objects, **most recent first** (descending by `timestamp`). |

### Activity Object Fields

| Field | Type | Presence | Description |
|---|---|---|---|
| `id` | `integer` | Always | Auto-increment database row ID. Unique and monotonically increasing. Useful for tracking "what's new since last query" by remembering the highest `id` seen. |
| `timestamp` | `integer` | Always | Unix epoch seconds (UTC) when the event was recorded. |
| `action` | `string` | Always | The type of file/folder activity. See [Action Types](#action-types) below. |
| `path` | `string` | Always | Full absolute path of the affected file or folder. Backslashes are JSON-escaped as `\\`. |
| `old_path` | `string` | **RENAME only** | The previous full path before the rename/move. Only present when `action` is `"RENAME"`. |

### Action Types

| Action | Meaning | Source | `old_path` present? |
|---|---|---|---|
| `CREATE` | A new file or folder was created. | ReadDirectoryChangesW, SHChangeNotify | No |
| `OPEN` | A file or folder was opened / accessed (read). | NT Kernel Logger ETW (FileIoCreate) | No |
| `MODIFY` | A file or folder was written to, or its attributes/metadata changed. | ReadDirectoryChangesW, SHChangeNotify | No |
| `DELETE` | A file or folder was deleted. | ReadDirectoryChangesW, SHChangeNotify | No |
| `RENAME` | A file or folder was renamed or moved to a new path. | ReadDirectoryChangesW, SHChangeNotify | **Yes** — `old_path` contains the path before the rename. |

**Notes on action semantics:**

- A single user action (e.g., saving a document) may generate multiple events
  (e.g., `CREATE` + `MODIFY`, or `DELETE` + `CREATE` for atomic saves).
- `OPEN` events are deduplicated with a 2-second window — rapid repeated opens
  of the same file are collapsed into one event.
- `OPEN` events cover both files and folders. Opening a folder in Explorer will
  generate an `OPEN` event for that folder path.
- The `RENAME` action covers both in-place renames and cross-directory moves.
  Check whether the parent directory of `path` differs from that of `old_path`
  to distinguish a move from a rename.

---

## Error Handling

| Scenario | What Happens |
|---|---|
| WARP is not running | `CreateFile` fails with `ERROR_FILE_NOT_FOUND` (error 2). Your app should retry or inform the user. |
| Malformed JSON request | WARP treats it as a default query and returns the last 1 hour of activity. |
| Unknown `window` code | WARP falls back to `1h` (last 1 hour). |
| Negative or zero `seconds` | The query returns an empty result set (`"count": 0`). |
| No activity in the time range | Returns `{"count":0,"activities":[]}`. |
| Response exceeds 64 KB | Very large result sets may be truncated at the pipe buffer boundary. Use a shorter time window or custom seconds to reduce result size. |

### Checking if WARP is Available

Before relying on WARP data, you can probe whether the service is running:

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
| **Retention period** | 30 days (rolling). Records older than 30 days are evicted on startup and every 6 hours. |
| **Maximum queryable range** | 30 days (`"window":"30d"` or `"seconds":2592000`). |
| **Database location** | `%LOCALAPPDATA%\WARP\activity.db` |
| **Storage format** | SQLite 3 with WAL mode |
| **Timestamp precision** | 1-second resolution (Unix epoch seconds, UTC) |
| **OPEN event dedup window** | 2 seconds — multiple kernel-level opens of the same file within 2 seconds are collapsed into one event. |

---

## Integration Patterns

### 1. Recent File Picker

Show users the files they opened most recently:

```json
{"window":"15m"}
```

Filter the response for `action == "OPEN"` and display unique paths.

### 2. Backup & Sync Engine

Find all files created or modified since the last backup:

```json
{"seconds":3600}
```

Filter for `action == "CREATE"` or `action == "MODIFY"`. Use the `id` field to
track the high-water mark — on the next sync, query the full window and skip
any `id` values you've already processed.

### 3. Security Auditing

Generate a compliance report of all file activity over the past week:

```json
{"window":"7d"}
```

Log every event. The `RENAME` action with `old_path` lets you track file
provenance across renames and moves.

### 4. Developer Tooling

Detect which source files changed during a build:

```json
{"seconds":120}
```

Filter for `action == "MODIFY"` and paths matching your project directory.

### 5. Smart Cleanup Utility

Find files that haven't been touched recently by comparing the full 30-day
activity log against a directory listing:

```json
{"window":"30d"}
```

Collect all unique `path` values with `action == "OPEN"` or `action == "MODIFY"`.
Any file in the target directory that does **not** appear in this set hasn't been
accessed in at least 30 days.

---

## Client Examples

### C++ (Win32)

```cpp
#include <windows.h>
#include <cstdio>
#include <cstring>

int main()
{
    // Connect to the WARP named pipe
    HANDLE hPipe = CreateFileW(
        L"\\\\.\\pipe\\WarpFileActivityAPI",
        GENERIC_READ | GENERIC_WRITE,
        0, nullptr, OPEN_EXISTING, 0, nullptr);

    if (hPipe == INVALID_HANDLE_VALUE) {
        printf("WARP is not running (error %lu)\n", GetLastError());
        return 1;
    }

    // Set pipe to message-read mode
    DWORD mode = PIPE_READMODE_MESSAGE;
    SetNamedPipeHandleState(hPipe, &mode, nullptr, nullptr);

    // Send request — last 15 minutes
    const char* request = R"({"window":"15m"})";
    DWORD written = 0;
    WriteFile(hPipe, request, (DWORD)strlen(request), &written, nullptr);

    // Read response
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

// Connect
using var pipe = new NamedPipeClientStream(".", "WarpFileActivityAPI",
    PipeDirection.InOut);
pipe.Connect(5000); // 5-second timeout
pipe.ReadMode = PipeTransmissionMode.Message;

// Send request — last 6 hours
byte[] request = Encoding.UTF8.GetBytes("""{"window":"6h"}""");
pipe.Write(request, 0, request.Length);

// Read response
byte[] buffer = new byte[65536];
int bytesRead = pipe.Read(buffer, 0, buffer.Length);
string json = Encoding.UTF8.GetString(buffer, 0, bytesRead);

// Parse and use
using var doc = JsonDocument.Parse(json);
int count = doc.RootElement.GetProperty("count").GetInt32();
Console.WriteLine($"Total events: {count}");

foreach (var activity in doc.RootElement.GetProperty("activities").EnumerateArray())
{
    string action = activity.GetProperty("action").GetString()!;
    string path = activity.GetProperty("path").GetString()!;
    long timestamp = activity.GetProperty("timestamp").GetInt64();

    DateTimeOffset time = DateTimeOffset.FromUnixTimeSeconds(timestamp).ToLocalTime();
    Console.WriteLine($"  [{time:HH:mm:ss}] {action,-8} {path}");

    // For RENAME actions, also print the old path
    if (action == "RENAME" && activity.TryGetProperty("old_path", out var oldPath))
        Console.WriteLine($"             from: {oldPath.GetString()}");
}
```

### Python

```python
import json
import struct

# Using the built-in win32 pipe via ctypes (no pywin32 dependency)
# Alternatively, if pywin32 is available:
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


# --- Example: Get recently opened files ---
data = query_warp({"window": "15m"})
print(f"Total events: {data['count']}")

# Filter for OPEN events only
opens = [a for a in data["activities"] if a["action"] == "OPEN"]
print(f"Files/folders opened in last 15 min: {len(opens)}")
for item in opens:
    print(f"  {item['path']}")

# --- Example: Get modified files in the last 2 hours ---
data = query_warp({"window": "2h"})
modified = [a for a in data["activities"] if a["action"] == "MODIFY"]
unique_paths = list(dict.fromkeys(a["path"] for a in modified))
print(f"\nUnique files modified in last 2 hours: {len(unique_paths)}")
for p in unique_paths[:10]:
    print(f"  {p}")
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

# Get last 1 hour of activity
$result = Query-Warp '{"window":"1h"}'
Write-Host "Total events: $($result.count)"

# Show recent OPEN events
$result.activities |
    Where-Object { $_.action -eq "OPEN" } |
    Select-Object timestamp, path |
    Format-Table -AutoSize

# Get last 24 hours, group by action type
$result = Query-Warp '{"window":"24h"}'
$result.activities |
    Group-Object -Property action |
    Select-Object Name, Count |
    Sort-Object -Property Count -Descending |
    Format-Table -AutoSize
```

### Rust

```rust
use std::io::{Read, Write};
use std::os::windows::io::FromRawHandle;
use std::fs::OpenOptions;

fn query_warp(request: &str) -> Result<String, Box<dyn std::error::Error>> {
    // On Windows, named pipes can be opened as files
    let mut pipe = OpenOptions::new()
        .read(true)
        .write(true)
        .open(r"\\.\pipe\WarpFileActivityAPI")?;

    pipe.write_all(request.as_bytes())?;
    pipe.flush()?;

    let mut response = String::new();
    let mut buf = [0u8; 65536];
    let n = pipe.read(&mut buf)?;
    response.push_str(std::str::from_utf8(&buf[..n])?);

    Ok(response)
}

fn main() {
    match query_warp(r#"{"window":"15m"}"#) {
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

// Example: Get last 30 minutes of activity
(async () => {
    try {
        const result = await queryWarp({ window: '30m' });
        console.log(`Total events: ${result.count}`);

        // Group by action
        const grouped = {};
        for (const a of result.activities) {
            grouped[a.action] = (grouped[a.action] || 0) + 1;
        }
        console.log('By action:', grouped);

        // Show OPEN events
        const opens = result.activities.filter(a => a.action === 'OPEN');
        for (const o of opens.slice(0, 10)) {
            const time = new Date(o.timestamp * 1000).toLocaleTimeString();
            console.log(`  [${time}] ${o.path}`);
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

### 2. Filter Client-Side

WARP returns all event types in the requested window. Filter by `action` and/or
`path` prefix in your client code to get exactly what you need.

### 3. Track High-Water Mark with `id`

For incremental processing (e.g., backup sync), remember the highest `id` from
your last query. On the next query, discard any events with `id` ? your
high-water mark. This is more reliable than timestamp-based tracking.

### 4. Handle Pipe Unavailability Gracefully

WARP may not be running. Always handle `CreateFile` failure and implement a
fallback path in your application.

### 5. One Connection Per Query

Open a fresh pipe connection for each query. Don't try to reuse the handle for
multiple request/response cycles — the pipe server expects one message per
connection.

### 6. Parse Timestamps as UTC

The `timestamp` field is Unix epoch seconds in **UTC**. Convert to local time
in your application if needed for display.

### 7. Normalize Paths for Comparison

Paths are returned with native Windows casing. When comparing paths from
different queries or with your own file lists, use case-insensitive comparison.

---

## Troubleshooting

| Problem | Cause | Solution |
|---|---|---|
| `CreateFile` returns `INVALID_HANDLE_VALUE` with error 2 | WARP is not running | Start WARP. It requires Administrator privileges. |
| `CreateFile` returns `INVALID_HANDLE_VALUE` with error 231 | All pipe instances are busy | Retry after a short delay (100–500 ms). |
| Response is empty or `{"count":0,"activities":[]}` | No activity in the requested time range, or WARP was recently started | Try a wider time window. WARP only records events while it's running. |
| `OPEN` events are missing | WARP's ETW trace may have failed to start | Ensure WARP is running with Administrator privileges. The NT Kernel Logger requires elevation. |
| Duplicate events for the same file | Normal — a single user action can trigger multiple kernel events | Deduplicate by path + action within a short time window in your client. |
| Paths contain `\\Device\\HarddiskVolume...` | Drive letter mapping failed for that volume | These are filtered out by WARP. If you see them, report as a bug. |
| `ReadFile` returns partial data | Response exceeded the 64 KB pipe buffer | Use a shorter time window to reduce result size. |

---

## Limitations

1. **Local only** — The named pipe is not accessible from remote machines.
2. **No filtering server-side** — You cannot filter by action type or path in the
   request. All matching events are returned; filter in your client.
3. **No streaming / subscription** — The API is request/response only. There is no
   push notification or event stream. Poll periodically if you need near-real-time
   updates.
4. **30-day maximum** — Events older than 30 days are automatically evicted.
5. **No authentication** — Any local process can connect to the pipe. The data
   contains full file paths which may be sensitive.
6. **64 KB response limit** — Very large result sets may be truncated. Use shorter
   time windows for busy systems.
7. **1-second timestamp granularity** — Events within the same second have the
   same `timestamp` value. Use `id` for precise ordering.
8. **Excluded paths** — Activity in system directories (Windows, Program Files,
   AppData, ProgramData, etc.), hidden dot-folders, and common build/cache
   artifacts is excluded by design. See the README for the full exclusion list.

---

## Schema Reference

For advanced users who want to query the SQLite database directly (e.g., for
complex joins or aggregations), here is the schema:

```sql
CREATE TABLE file_activity (
    id        INTEGER PRIMARY KEY AUTOINCREMENT,
    timestamp INTEGER NOT NULL,       -- Unix epoch seconds (UTC)
    action    TEXT    NOT NULL,        -- CREATE | OPEN | MODIFY | DELETE | RENAME
    path      TEXT    NOT NULL,        -- Full path (UTF-8)
    old_path  TEXT                     -- Previous path (RENAME only; NULL otherwise)
);

CREATE INDEX idx_activity_ts     ON file_activity(timestamp);
CREATE INDEX idx_activity_action ON file_activity(action, timestamp);
```

**Database location:** `%LOCALAPPDATA%\WARP\activity.db`  
**Pragmas:** `journal_mode=WAL`, `synchronous=NORMAL`

> ?? **Warning:** The database is actively written to by WARP. If you open it
> directly, use `PRAGMA query_only=ON` or open in read-only mode to avoid
> interfering with WARP's WAL writer. The named-pipe API is the recommended
> access method.

---

*This documentation describes WARP API version 1.0. The API is stable and
backward-compatible — new fields may be added to response objects in future
versions, but existing fields will not be removed or change meaning.*

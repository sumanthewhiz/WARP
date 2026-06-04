# WARP Activity API -- Integration Guide


# WARP Activity API -- Integration Guide

> **Version:** 4.0
> **Pipe endpoint:** `\\.\pipe\WarpFileActivityAPI`
> **Transport:** Windows Named Pipe (message mode)
> **Encoding:** UTF-8 JSON

This document describes how third-party applications running on the same Windows
machine can connect to the WARP service and query activity history -- including
file/folder events, application launches, and browsing activity -- as well as
precomputed per-entity inference data (recency scores, access counts, edit
timestamps) to make better-informed decisions in their own workflows.

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
  - [Inference Operations](#5-inference-operations)
    - [QueryInferences](#queryinferences)
    - [GetInferenceDeltas](#getinferencedeltas)
    - [GetRecentContext](#getrecentcontext)
    - [GetRecentContexts](#getrecentcontexts)
- [Response Format](#response-format)
  - [Top-Level Structure](#top-level-structure)
  - [File Activity Fields](#file-activity-fields)
  - [File Action Types](#file-action-types)
  - [App Launch Activity Fields](#app-launch-activity-fields)
  - [Browsing Activity Fields](#browsing-activity-fields)
- [Inference Response Format](#inference-response-format)
  - [QueryInferences Response](#queryinferences-response)
  - [GetInferenceDeltas Response](#getinferencedeltas-response)
  - [GetRecentContext Response](#getrecentcontext-response)
  - [GetRecentContexts Response](#getrecentcontexts-response)
  - [Inference Record Fields](#inference-record-fields)
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
  - [Smart File Ranking](#8-smart-file-ranking)
  - [Incremental Inference Sync](#9-incremental-inference-sync)
  - [Semantic Context Awareness](#10-semantic-context-awareness)
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
| Smart file ranking | Use `QueryInferences` to get recency scores and open counts for a set of files |
| Incremental inference sync | Use `GetInferenceDeltas` to stream changes since your last known version watermark |
| Dynamic context awareness | Use `GetRecentContext` for the latest **context summary** (1–3 phrase lines describing what the user is doing right now — e.g., `["Working on Context Inference (across Visual Studio & Edge)", "Discussing Daily Standup in Teams & Outlook", "Researching React Hooks"]`); use `GetRecentContexts` for short-term memory (last *N* snapshots, newest first) |

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

### 5. Inference Operations

In addition to raw event queries, the pipe supports three **inference operations**
that return precomputed analytics. Inference requests are identified by the `"op"`
field. These operations do **not** use `"window"`, `"seconds"`, or `"types"`.

#### QueryInferences

Batch-lookup inference records for a list of entity keys (file paths, exe paths,
or URLs). Keys are matched case-insensitively (the engine normalizes to lowercase
internally).

```json
{
  "op": "QueryInferences",
  "paths": [
    "C:\\Users\\Alice\\Documents\\report.docx",
    "C:\\Windows\\System32\\notepad.exe",
    "https://github.com/pulls"
  ]
}
```

An optional `"fields"` array limits which fields are included in each result
object. If omitted, all fields are returned.

```json
{
  "op": "QueryInferences",
  "paths": ["C:\\Users\\Alice\\Documents\\report.docx"],
  "fields": ["recency_score", "last_open_ts", "open_count_7d"]
}
```

**Available field names:** `entity_type`, `last_event_ts`, `last_open_ts`,
`last_edit_ts`, `open_count_7d`, `open_count_30d`, `open_count_total`,
`recency_score`, `version`, `updated_at`.

#### GetInferenceDeltas

Retrieve all inference records whose `version` is greater than a given watermark.
Returns up to **5 000 records** per call, ordered by ascending `version`.

```json
{
  "op": "GetInferenceDeltas",
  "since_version": 0
}
```

Pass `0` to get all records. On subsequent calls, pass the highest `version` value
you received in the previous response to get only new/updated records.

```json
{
  "op": "GetInferenceDeltas",
  "since_version": 1042
}
```

#### GetRecentContext

Retrieve the **latest context-summary** snapshot from the `ContextInference`
summarizer.  The summary is a JSON array of 1–3 short phrase lines (one per
cluster of activity) that describe what the user is actively doing.

Every 60 seconds, WARP gathers all activities from the lookback window
(overlaying the user's currently-active foreground window as a virtual focus
row), classifies each app via a layered classifier (~80-entry exact-match table
→ vendor-path heuristics → exe-name fallback), and composes a human-readable
context summary that names the actual documents, browser tabs, and
apps the user is working with — not a fixed bucket label.

```json
{
  "op": "GetRecentContext",
  "category": "all",
  "window_seconds": 900
}
```

| Param | Type | Default | Notes |
|---|---|---|---|
| `category` | `string` | `"all"` | One of `all`, `files`, `websites`, `apps`. Controls **both** which summary is surfaced as the top-level `summary` field **and** the response shape: `"all"` returns the combined summary plus all three per-category summaries; any other value returns only the matching category's summary. The legacy value `documents` is accepted as a backward-compat alias for `files`. |
| `window_seconds` | `integer` | `900` (= 15 min) | Activity lookback span. Snapped to the nearest of: 300, 900, 1800, 3600, 7200, 21600, 86400, 604800, 1296000, 2592000 (5 min, 15 min, 30 min, 1 h, 2 h, 6 h, 24 h, 7 d, 15 d, 30 d). 15 min serves the cached snapshot from the 60-sec timer; any other value composes fresh on demand. |

The response is grounded in observed window titles and tab names, so it stays
accurate as the user moves between projects, customers, or topics — there is
no fixed taxonomy to fall out of.

#### GetRecentContexts

Retrieve the last *N* snapshots from the rolling history buffer, **newest
first**. Useful for charting context drift over time, building a short-term
memory for an LLM agent, or rendering a "what was I doing" timeline.

```json
{
  "op": "GetRecentContexts",
  "count": 10,
  "category": "all",
  "window_seconds": 900
}
```

| Param | Type | Default | Notes |
|---|---|---|---|
| `count` | `integer` | `10` | Hard-capped at 200 to keep responses under the 64 KB pipe buffer. Negative or zero values are treated as the default. |
| `category` | `string` | `"all"` | Same semantics as `GetRecentContext` — applied to every snapshot in the response. |
| `window_seconds` | `integer` | `900` | Filters history to snapshots whose `timestamp` falls within the last `window_seconds`.  Larger values include more historical rows; each row's underlying compute window stays at the rolling-history cadence (15 min). |

History entries are de-duplicated on append: a new snapshot is recorded only on
**material change** — different summary, different dominant app, or a
5-minute heartbeat — so the returned list reflects context *transitions*
rather than 60-second polling artifacts.

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

## Inference Response Format

### QueryInferences Response

```json
{
  "now": 1750012345,
  "results": {
    "C:\\Users\\Alice\\Documents\\report.docx": {
      "entity_type": "file",
      "last_event_ts": 1750012000,
      "last_open_ts": 1750012000,
      "last_edit_ts": 1750011500,
      "open_count_7d": 12,
      "open_count_30d": 45,
      "open_count_total": 45,
      "recency_score": 187.3,
      "version": 58,
      "updated_at": 1750012000
    },
    "C:\\Windows\\System32\\notepad.exe": {
      "entity_type": "app",
      "last_event_ts": 1749998000,
      "last_open_ts": 1749998000,
      "last_edit_ts": 0,
      "open_count_7d": 3,
      "open_count_30d": 10,
      "open_count_total": 10,
      "recency_score": 42.1,
      "version": 22,
      "updated_at": 1749998000
    },
    "https://github.com/pulls": {}
  }
}
```

- The `"results"` object is keyed by the **exact strings you sent** in `"paths"`
  (not the normalized form).
- If an entity has never been seen by WARP, its value is an empty object `{}`.
- If `"fields"` was specified, only those fields appear in each result object.
- `"now"` is the server-side Unix epoch timestamp at query time.

### GetInferenceDeltas Response

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
      "version": 58,
      "updated_at": 1750012000
    },
    {
      "entity_key": "c:\\windows\\system32\\notepad.exe",
      "entity_type": "app",
      "last_event_ts": 1749998000,
      "last_open_ts": 1749998000,
      "last_edit_ts": 0,
      "open_count_7d": 3,
      "open_count_30d": 10,
      "open_count_total": 10,
      "recency_score": 42.1,
      "version": 22,
      "updated_at": 1749998000
    }
  ]
}
```

- `"deltas"` is an array ordered by ascending `version`.
- Entity keys are in **lowercase normalized form**.
- At most 5 000 records are returned per call. If you receive exactly 5 000, call
  again with `since_version` set to the highest `version` in the response.
- All fields are always present in delta records (no field projection).

### GetRecentContext Response

The shape varies with the requested `category`:

* **`category == "all"` (default).** The response carries the **combined**
  summary as `summary` plus all three per-category summaries
  (`summary_files` / `_websites` / `_apps`), so a UI that wants to
  switch facets locally can do so without re-querying.
* **`category == "files" | "websites" | "apps"`.** Only the matching
  per-category summary is returned (as `summary`). The other
  categories — including the combined "all" summary — are **omitted**,
  keeping the response focused on what the caller asked for.

Each `summary` is a JSON **array** of 1–3 short phrase strings (one per
cluster of activity).  Consumers render them as separate lines.

**Example — `category == "all"`, `window_seconds = 900`:**

```json
{
  "recent_context": {
    "timestamp": 1750012345,
    "window_start": 1750011445,
    "window_end": 1750012345,
    "window_seconds": 900,
    "category": "all",
    "summary": [
      "Working on Context Inference (across Visual Studio & Edge)",
      "Discussing Daily Standup (across Teams & Outlook)",
      "Researching React Hooks"
    ],
    "summary_files":    ["Editing Context Inference module", "Drafting Daily Standup notes"],
    "summary_websites": ["Researching React Hooks", "Reviewing GitHub PR for ContextInference"],
    "summary_apps":     ["Discussing in Slack & Teams", "Triaging Outlook inbox"],
    "activity_count": 47,
    "focus_seconds": 812,
    "dominant_focus_pct": 61.4,
    "confidence": 0.84,
    "model": "bge-small-en-v1.5",
    "thread_count": 3,
    "signal_types": ["focus", "file", "browsing"],
    "items": [
      { "app": "Visual Studio",  "exe": "devenv.exe",        "title": "ContextInference.cpp - WARP", "raw_title": "● ContextInference.cpp - WARP! - Microsoft Visual Studio",                        "focus_seconds": 498, "pct": 61.4, "thread_id": 1 },
      { "app": "Edge",           "exe": "msedge.exe",        "title": "ContextInference PR review",  "raw_title": "ContextInference PR review · sumanthewhiz/WARP · Pull Request #34 — Microsoft Edge", "focus_seconds": 184, "pct": 22.7, "thread_id": 1 },
      { "app": "GitHub Desktop", "exe": "GitHubDesktop.exe", "title": "WARP - dev",                  "raw_title": "WARP - dev — GitHub Desktop",                                                       "focus_seconds":  60, "pct":  7.4, "thread_id": 1 },
      { "app": "Slack",          "exe": "slack.exe",         "title": "Slack - WARP channel",        "raw_title": "Slack - WARP channel — Slack",                                                      "focus_seconds":  52, "pct":  6.4, "thread_id": 2 },
      { "app": "Outlook",        "exe": "OUTLOOK.EXE",       "title": "Inbox - Suman Ghosh",         "raw_title": "Inbox - Suman Ghosh - Outlook",                                                     "focus_seconds":  18, "pct":  2.2, "thread_id": 3 }
    ]
  },
  "category": "all",
  "window_seconds": 900,
  "history_count": 12
}
```

**Example — `category == "files"` (same shape for `"websites"` and `"apps"`):**

```json
{
  "recent_context": {
    "timestamp": 1750012345,
    "window_start": 1750011445,
    "window_end": 1750012345,
    "window_seconds": 900,
    "category": "files",
    "summary": ["Editing Context Inference module", "Drafting Daily Standup notes"],
    "activity_count": 47,
    "focus_seconds": 812,
    "dominant_focus_pct": 61.4,
    "confidence": 0.84,
    "model": "bge-small-en-v1.5",
    "thread_count": 3,
    "signal_types": ["focus", "file", "browsing"],
    "items": [ /* same shape as above; always reflects the All clustering */ ]
  },
  "category": "files",
  "window_seconds": 900,
  "history_count": 12
}
```

| Field | Type | Description |
|---|---|---|
| `timestamp` | `integer` | Unix epoch seconds (UTC) when this snapshot was produced. |
| `window_start` / `window_end` | `integer` | Bounds of the lookback window (Unix epoch seconds, UTC). |
| `window_seconds` | `integer` | Width of the lookback window in seconds. Echoes the requested `window_seconds` after normalisation to the nearest allowed value. |
| `category` | `string` | Echo of the requested category (`all` / `files` / `websites` / `apps`). |
| `summary` | `string[]` | Array of 1–3 short phrase lines describing what the user is actively doing. Equals the **combined** summary when `category == "all"`; equals the matching per-category summary otherwise. Render each entry on its own line. |
| `summary_files` | `string[]` | **Only present when `category == "all"`.** Composed from any app where the user is engaged with a real file: code editors / IDEs, Office apps, OneNote / Notion / Obsidian, PDF readers, image viewers / editors (Photos / IrfanView / Paint / Paint.NET / GIMP), design tools — **plus** any arbitrary app whose window title contains a recognized file extension (e.g. `.docx`, `.xlsx`, `.pptx`, `.pdf`, `.jpg`, `.png`, `.cpp`, `.json`, `.txt`, `.md`, …) — **plus** recent file basenames from `FileMonitor`. |
| `summary_websites` | `string[]` | **Only present when `category == "all"`.** Composed from browser tab titles, aggregated per unique cleaned title. |
| `summary_apps` | `string[]` | **Only present when `category == "all"`.** Composed from non-file, non-browser apps: communications (Outlook / Teams / Slack / Discord / WhatsApp / Signal / Telegram / Zoom / Webex), media players (Spotify / VLC), terminals (Windows Terminal / PowerShell / Command Prompt / Git Bash), remote desktop & VMs, version-control UIs (GitHub Desktop / Sourcetree). |
| `activity_count` | `integer` | Total activities examined in the window. |
| `focus_seconds` | `integer` | Total foreground dwell time accounted for in the window. |
| `dominant_focus_pct` | `number` | Percentage of focus time held by the top app (0.0 – 100.0). |
| `confidence` | `number` | Heuristic confidence in the summary (0.0 – 0.99). Combines focus volume, dominance, and signal-type breadth. |
| `model` | `string` | `"granite-embedding-small-english-r2"` when the granite ModernBERT sentence-encoder is loaded (preferred); `"bge-small-en-v1.5"` when the legacy BGE-small WordPiece encoder is the only one present; `"all-MiniLM-L6-v2"` when only the older MiniLM model is present (transparent backward-compat fallback); `"deterministic"` when no model file was found. |
| `model_polish` | `string` | `"qwen3-0.6b"` when the optional LLM polishing layer is loaded and active; `"(not loaded)"` when the polisher's model files aren't present. |
| `summary_polished` | `string[]` | LLM-polished natural-prose rewrite of `summary` (or the category-matching summary). Present in every response (possibly empty array). Empty when the LLM isn't loaded, the inference timed out, or the output failed grounding validation. |
| `summary_files_polished` | `string[]` | **Only present when `category == "all"`.** LLM-polished `summary_files`. Same emptiness semantics as `summary_polished`. |
| `summary_websites_polished` | `string[]` | **Only present when `category == "all"`.** LLM-polished `summary_websites`. |
| `summary_apps_polished` | `string[]` | **Only present when `category == "all"`.** LLM-polished `summary_apps`. |
| `thread_count` | `integer` | Number of distinct *threads of work* the clusterer collapsed the activity into. ≥ 1; always reflects the **All** clustering regardless of `category`. |
| `signal_types` | `string[]` | Which event categories contributed: any of `"focus"`, `"file"`, `"app"`, `"browsing"`. |
| `items` | `object[]` | Up to 5 per-app breakdowns: `{ app, exe, title, raw_title, focus_seconds, pct, thread_id }`. `title` is the cleaned/de-suffixed form; `raw_title` is the **full** original window title before any cleaning, so callers can see the complete context. Items sharing a `thread_id` were merged by the sentence-encoder clusterer (cosine ≥ 0.65); the summary shows them as `(across X & Y …)`. Always reflects the **All** clustering. |
| `history_count` | `integer` | Number of snapshots currently in the rolling history (max 1 440 ≈ 24 hours at 60-sec cadence). |

**Notes:**
- If no activities occurred in the lookback window, `summary` will be an
  array containing `"User appears to be idle"` and `items[]` will be empty.
- The summarizer runs every 60 seconds. Calling `GetRecentContext` between
  runs returns the result from the most recent completed cycle.
- Snapshots are stored in memory (not persisted to disk). Restarting WARP
  clears the history.
- The composer uses the sentence-encoder for **dynamic clustering, not bucket matching** —
  there is no fixed taxonomy of topics. Each cluster is induced from the
  actual document, tab, and app titles in the window. If `models/bge-small.onnx`
  (or the legacy `models/minilm.onnx`) + `models/vocab.txt` are missing the engine still runs and emits
  `"model": "deterministic"`; consumers should treat both modes as a single
  interface and just read `summary` / `items` / `thread_id` as documented.

### GetRecentContexts Response

```json
{
  "recent_contexts": [
    {
      "timestamp": 1750012345,
      "window_start": 1750011445,
      "window_end": 1750012345,
      "summary": ["Working on Context Inference (across Visual Studio & Edge)", "Discussing Daily Standup in Teams & Outlook", "+ 2 other threads"],
      "activity_count": 47,
      "focus_seconds": 812,
      "dominant_focus_pct": 61.4,
      "confidence": 0.84,
      "model": "bge-small-en-v1.5",
      "thread_count": 3,
      "signal_types": ["focus", "file", "browsing"],
      "items": [ /* … same shape as GetRecentContext … */ ]
    },
    {
      "timestamp": 1750012045,
      "window_start": 1750011145,
      "window_end": 1750012045,
      "summary": ["Researching React Hooks", "Reading about API Reference"],
      "activity_count": 31,
      "focus_seconds": 712,
      "dominant_focus_pct": 48.9,
      "confidence": 0.71,
      "model": "bge-small-en-v1.5",
      "thread_count": 2,
      "signal_types": ["focus", "browsing"],
      "items": [ /* … */ ]
    }
  ],
  "returned": 2,
  "history_count": 12,
  "requested": 10
}
```

| Field | Type | Description |
|---|---|---|
| `recent_contexts` | `object[]` | Snapshots ordered **newest first**. Each entry has the same shape as the `recent_context` object documented above. |
| `returned` | `integer` | Actual number of snapshots in `recent_contexts` (≤ `requested`, ≤ `history_count`). |
| `history_count` | `integer` | Total snapshots currently in the rolling history. |
| `requested` | `integer` | The effective `count` after defaulting (10) and capping (200). |

### Inference Record Fields

| Field | Type | Description |
|---|---|---|
| `entity_key` | `string` | Lowercase entity identifier (file path, exe path, or URL). |
| `entity_type` | `string` | One of `"file"`, `"app"`, or `"url"`. |
| `last_event_ts` | `integer` | Unix epoch seconds of the most recent event of any kind for this entity. |
| `last_open_ts` | `integer` | Unix epoch seconds of the most recent OPEN event (files) or launch/visit (apps/URLs). `0` if never opened. |
| `last_edit_ts` | `integer` | Unix epoch seconds of the most recent MODIFY or CREATE event. Only meaningful for `"file"` entities. `0` if never edited. |
| `open_count_7d` | `integer` | Rolling count of OPEN events in the last 7 days. |
| `open_count_30d` | `integer` | Rolling count of OPEN events in the last 30 days. |
| `open_count_total` | `integer` | Lifetime OPEN count since WARP started tracking this entity. |
| `recency_score` | `number` | Composite score (0�255) combining exponential time-decay and 7-day frequency. Higher values indicate more recently and frequently accessed entities. Formula: `200 � e^(??t / 172800) + 5 � ln(1 + open_count_7d)`. |
| `version` | `integer` | Monotonically increasing per-entity counter. Bumped on every event. Use as a watermark for `GetInferenceDeltas`. |
| `updated_at` | `integer` | Unix epoch seconds of the last inference record update. |

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
| `QueryInferences` returns `{}` for a path | The entity has never been seen by WARP. No inference record exists. |
| `GetInferenceDeltas` returns empty `"deltas"` | No records have changed since the given `since_version`. |
| `GetRecentContext` returns an empty `summary` array and zero counts | No activities occurred in the last 15 minutes, or WARP was just started and the first 60-second cycle hasn't completed yet. |
| `GetRecentContexts` returns `"recent_contexts": []` | History is empty (just started, or `ClearAll` was called). `history_count` will also be `0`. |
| Unknown `"op"` value | The request is treated as a default event query (last 1 hour, all types). |

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

### 8. Smart File Ranking

Rank a set of known files by how recently and frequently the user has interacted
with them, without scanning raw events:

```json
{
  "op": "QueryInferences",
  "paths": [
    "C:\\Users\\Alice\\Projects\\app\\main.cpp",
    "C:\\Users\\Alice\\Projects\\app\\utils.h",
    "C:\\Users\\Alice\\Projects\\app\\README.md"
  ],
  "fields": ["recency_score", "open_count_7d", "last_open_ts"]
}
```

Sort the results by `recency_score` descending to surface the most relevant files.

### 9. Incremental Inference Sync

Keep a local cache of inference data in your app and periodically pull only
what has changed:

```json
{"op": "GetInferenceDeltas", "since_version": 0}
```

Store the highest `version` from the response. On the next sync call:

```json
{"op": "GetInferenceDeltas", "since_version": 1042}
```

Only records updated after version 1042 are returned. Repeat until `"deltas"` is
empty. This is ideal for dashboard widgets or IDE extensions that need near-real-time
insight without re-querying the full dataset.

### 10. Dynamic Context Awareness

Understand what the user is currently working on to provide contextually relevant
suggestions, search results, or UI adaptations:

```json
{"op": "GetRecentContext"}
```

The response gives you a single human-readable line that names the
**semantic theme** of what the user is doing right now (e.g.
*"Working on Context Inference (across Visual Studio & Edge) ·
Discussing Daily Standup (across Outlook & Teams) · Researching React
Hooks"*) — the cluster theme is distilled from the actual document/tab/
app titles by tokenizing, stripping stop-words / brand names / file
extensions, and ranking the remaining content tokens by frequency
weighted by their cosine similarity to the cluster centroid. The
response also includes a structured `items[]` breakdown (which **does**
preserve the verbatim per-app titles for programmatic consumers) and a
confidence score. Use this to:

- **Search ranking** -- boost results related to the user's current
  themes, docs, tabs, or app set.
- **Smart suggestions** -- recommend tools, files, or actions relevant
  to the detected activity.
- **Dashboard widgets** -- show a "Currently working on" summary.

For short-term memory or context drift over time, call:

```json
{"op": "GetRecentContexts", "count": 20}
```

Snapshots are returned newest-first with material-change dedup, so the list
reflects context *transitions* rather than 60-second polling artifacts.

The composer prefers `ibm-granite/granite-embedding-small-english-r2`
(ModernBERT byte-level BPE, COIR-trained for code retrieval, 384-dim) when
its files are present, and transparently falls back to
`BAAI/bge-small-en-v1.5` (BERT WordPiece, 384-dim) and then
`sentence-transformers/all-MiniLM-L6-v2` (same dim, same tokenizer as BGE)
when granite isn't available. Whichever encoder is loaded, it drives
**dynamic semantic clustering** *and* **theme distillation** — *not* mapping
to a fixed taxonomy. Two activities are merged into one *thread of work*
iff their embeddings are similar to each other (cosine ≥ 0.65), and within
each thread the dominant content tokens (after stripping stop-words, file
extensions, and brand/app names) are scored by `frequency × (1 +
cosine-to-cluster-centroid)` — the top 1-2 are emitted as the theme
phrase. The verb is selected from a small fixed set (`Working on`,
`Reviewing`, `Researching`, `Reading about`, `Discussing`, …) based on
the dominant app type and content keywords. The verbatim per-app titles
remain available in `items[]` for consumers that want them. If the
model files are missing the engine still runs and reports
`"model": "deterministic"` — themes are extracted by frequency alone,
and clusters that yield no usable content tokens fall back to the
prior verbatim format `<verb> "<title>" in <app>`.

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

**GetRecentContext example (C++):**

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

    // Latest context summary snapshot
    const char* req1 = R"({"op":"GetRecentContext"})";
    DWORD written = 0;
    WriteFile(hPipe, req1, (DWORD)strlen(req1), &written, nullptr);

    char buffer[65536] = {};
    DWORD bytesRead = 0;
    ReadFile(hPipe, buffer, sizeof(buffer) - 1, &bytesRead, nullptr);
    printf("Current context:\n%s\n\n", buffer);

    // Last 20 snapshots (newest first)
    const char* req2 = R"({"op":"GetRecentContexts","count":20})";
    WriteFile(hPipe, req2, (DWORD)strlen(req2), &written, nullptr);
    bytesRead = 0;
    memset(buffer, 0, sizeof(buffer));
    ReadFile(hPipe, buffer, sizeof(buffer) - 1, &bytesRead, nullptr);
    printf("Recent context history:\n%s\n", buffer);

    CloseHandle(hPipe);
    return 0;
}
```

**Inference query example (C++):**

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

    // QueryInferences: get recency scores for two files
    const char* request = R"({
        "op": "QueryInferences",
        "paths": [
            "C:\\Users\\Alice\\Documents\\report.docx",
            "C:\\Windows\\System32\\notepad.exe"
        ],
        "fields": ["recency_score", "open_count_7d", "last_open_ts"]
    })";
    DWORD written = 0;
    WriteFile(hPipe, request, (DWORD)strlen(request), &written, nullptr);

    char buffer[65536] = {};
    DWORD bytesRead = 0;
    ReadFile(hPipe, buffer, sizeof(buffer) - 1, &bytesRead, nullptr);

    printf("Inference results:\n%s\n", buffer);

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

**Inference query example (C#):**

```csharp
using System;
using System.IO.Pipes;
using System.Text;
using System.Text.Json;

// QueryInferences
using var pipe = new NamedPipeClientStream(".", "WarpFileActivityAPI",
    PipeDirection.InOut);
pipe.Connect(5000);
pipe.ReadMode = PipeTransmissionMode.Message;

byte[] request = Encoding.UTF8.GetBytes("""
{
  "op": "QueryInferences",
  "paths": ["C:\\Users\\Alice\\Documents\\report.docx"],
  "fields": ["recency_score", "open_count_7d"]
}
""");
pipe.Write(request, 0, request.Length);

byte[] buffer = new byte[65536];
int bytesRead = pipe.Read(buffer, 0, buffer.Length);
string json = Encoding.UTF8.GetString(buffer, 0, bytesRead);

using var doc = JsonDocument.Parse(json);
var results = doc.RootElement.GetProperty("results");
foreach (var prop in results.EnumerateObject())
{
    Console.Write($"{prop.Name}: ");
    if (prop.Value.TryGetProperty("recency_score", out var score))
        Console.Write($"score={score.GetDouble():F1} ");
    if (prop.Value.TryGetProperty("open_count_7d", out var cnt))
        Console.Write($"opens_7d={cnt.GetInt32()}");
    Console.WriteLine();
}
```

**GetInferenceDeltas example (C#):**

```csharp
using var pipe2 = new NamedPipeClientStream(".", "WarpFileActivityAPI",
    PipeDirection.InOut);
pipe2.Connect(5000);
pipe2.ReadMode = PipeTransmissionMode.Message;

byte[] deltaReq = Encoding.UTF8.GetBytes("""
{"op": "GetInferenceDeltas", "since_version": 0}
""");
pipe2.Write(deltaReq, 0, deltaReq.Length);

byte[] buf2 = new byte[65536];
int read2 = pipe2.Read(buf2, 0, buf2.Length);
string deltaJson = Encoding.UTF8.GetString(buf2, 0, read2);

using var deltaDoc = JsonDocument.Parse(deltaJson);
foreach (var delta in deltaDoc.RootElement.GetProperty("deltas").EnumerateArray())
{
    Console.WriteLine($"  [{delta.GetProperty("entity_type").GetString()}] " +
        $"{delta.GetProperty("entity_key").GetString()} " +
        $"score={delta.GetProperty("recency_score").GetDouble():F1} " +
        $"v{delta.GetProperty("version").GetInt32()}");
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

# --- Example: GetRecentContext (latest context summary) ---
context = query_warp({"op": "GetRecentContext"})
rc = context.get("recent_context", {})
print(f"\nCurrent context (confidence {rc.get('confidence', 0):.2f}, "
      f"{rc.get('activity_count', 0)} activities):")
print('  ' + ' / '.join(rc.get('summary', []) or ['(none)']))
for item in rc.get("items", []):
    print(f"    - {item['app']}: {item['title']}  ({item['pct']:.0f}%)")

# --- Example: GetRecentContexts (last 20 snapshots, newest first) ---
hist = query_warp({"op": "GetRecentContexts", "count": 20})
print(f"\nLast {hist.get('returned', 0)} context snapshots:")
for snap in hist.get("recent_contexts", []):
    print(f"  [{snap['timestamp']}] {' / '.join(snap.get('summary', []))}")

# --- Example: QueryInferences for specific files ---
inference = query_warp({
    "op": "QueryInferences",
    "paths": [
        "C:\\Users\\Alice\\Documents\\report.docx",
        "C:\\Windows\\System32\\notepad.exe",
    ],
    "fields": ["recency_score", "open_count_7d", "entity_type"],
})
for path, info in inference.get("results", {}).items():
    if info:  # empty dict means entity not tracked
        print(f"{path}: score={info.get('recency_score', 0):.1f}, "
              f"opens_7d={info.get('open_count_7d', 0)}, "
              f"type={info.get('entity_type', '?')}")
    else:
        print(f"{path}: not tracked by WARP")

# --- Example: GetInferenceDeltas (incremental sync) ---
deltas = query_warp({"op": "GetInferenceDeltas", "since_version": 0})
for d in deltas.get("deltas", []):
    print(f"  [{d['entity_type']}] {d['entity_key']} "
          f"score={d['recency_score']:.1f} v{d['version']}")
# Store max version for next call:
if deltas.get("deltas"):
    watermark = max(d["version"] for d in deltas["deltas"])
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

# GetRecentContext -- what is the user working on right now?
$context = Query-Warp '{"op":"GetRecentContext"}'
$rc = $context.recent_context
Write-Host "`nCurrent context (confidence $([math]::Round($rc.confidence,2)), $($rc.activity_count) activities):"
Write-Host "  $(($rc.summary -join ' / '))"
foreach ($item in $rc.items) {
    Write-Host "    - $($item.app): $($item.title)  ($([math]::Round($item.pct,0))%)"
}

# GetRecentContexts -- short-term memory of recent contexts (newest first)
$hist = Query-Warp '{"op":"GetRecentContexts","count":20}'
Write-Host "`nLast $($hist.returned) context snapshots:"
foreach ($snap in $hist.recent_contexts) {
    Write-Host "  [$($snap.timestamp)] $(($snap.summary -join ' / '))"
}

# QueryInferences for specific files
$inf = Query-Warp
$inf.results.PSObject.Properties | ForEach-Object {
    Write-Host "$($_.Name): score=$($_.Value.recency_score), opens_7d=$($_.Value.open_count_7d)"
}

# GetInferenceDeltas (incremental sync)
$deltas = Query-Warp '{"op":"GetInferenceDeltas","since_version":0}'
Write-Host "Received $($deltas.deltas.Count) delta records"
$deltas.deltas | Select-Object entity_key, entity_type, recency_score, version |
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

        // GetRecentContext -- what is the user working on right now?
        const context = await queryWarp({ op: 'GetRecentContext' });
        const rc = context.recent_context || {};
        console.log(`\nCurrent context (confidence ${(rc.confidence || 0).toFixed(2)}, ${rc.activity_count || 0} activities):`);
        console.log(`  ${(rc.summary || []).join(' / ') || '(none)'}`);
        for (const item of (rc.items || [])) {
            console.log(`    - ${item.app}: ${item.title}  (${item.pct.toFixed(0)}%)`);
        }

        // GetRecentContexts -- short-term memory of recent contexts (newest first)
        const hist = await queryWarp({ op: 'GetRecentContexts', count: 20 });
        console.log(`\nLast ${hist.returned || 0} context snapshots:`);
        for (const snap of (hist.recent_contexts || [])) {
            console.log(`  [${snap.timestamp}] ${(snap.summary || []).join(' / ')}`);
        }

        // QueryInferences -- get recency scores for specific files
        const inference = await queryWarp({
            op: 'QueryInferences',
            paths: [
                'C:\\Users\\Alice\\Documents\\report.docx',
                'C:\\Windows\\System32\\notepad.exe',
            ],
            fields: ['recency_score', 'open_count_7d'],
        });
        console.log('\nInference results:');
        for (const [path, info] of Object.entries(inference.results || {})) {
            if (Object.keys(info).length > 0) {
                console.log(`  ${path}: score=${info.recency_score}, opens_7d=${info.open_count_7d}`);
            } else {
                console.log(`  ${path}: not tracked`);
            }
        }

        // GetInferenceDeltas -- incremental sync
        const deltas = await queryWarp({ op: 'GetInferenceDeltas', since_version: 0 });
        console.log(`\nInference deltas: ${deltas.deltas.length} records`);
        for (const d of deltas.deltas.slice(0, 5)) {
            console.log(`  [${d.entity_type}] ${d.entity_key} score=${d.recency_score} v${d.version}`);
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

### 9. Use Inference for Ranking, Not Raw Events

If you need to rank or sort entities by relevance (e.g., "most recently used
files"), prefer `QueryInferences` over scanning raw events. The inference engine
precomputes recency scores and access counts incrementally, making lookups
instant regardless of how many raw events exist.

### 10. Use `GetInferenceDeltas` for Incremental Sync

Rather than re-querying all inference data on every poll, track the highest
`version` you have seen and pass it as `since_version`. This minimizes data
transfer and parsing overhead.

```json
{"op": "GetInferenceDeltas", "since_version": 1042}
```

### 10. Use `GetRecentContext` for Contextual Awareness

Poll `GetRecentContext` periodically to adapt your application to what the user
is currently working on. The summarizer runs every 60 seconds, so polling more
frequently than that will return the same result.

```json
{"op": "GetRecentContext"}
```

The `confidence` field (0.0 – 0.99) indicates how strongly the snapshot
represents recent activity — values above ~0.7 mean a clear focus pattern, while
lower values suggest the user is context-switching across many apps. The
`dominant_focus_pct` field tells you how concentrated the user is on the top app.

For short-term memory of *how* context evolved (newest first), use:

```json
{"op": "GetRecentContexts", "count": 20}
```

The returned snapshots are de-duplicated on append (material-change + 5-minute
heartbeat), so each entry represents an actual context transition rather than a
60-second polling tick.

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

-- Inference table (precomputed per-entity analytics)
CREATE TABLE inference (
    entity_key        TEXT PRIMARY KEY,   -- lowercase file path / exe path / URL
    entity_type       TEXT,               -- 'file', 'app', or 'url'
    last_event_ts     INTEGER,            -- last event of any kind
    last_open_ts      INTEGER,            -- last OPEN / launch / visit
    last_edit_ts      INTEGER,            -- last MODIFY/CREATE (files only)
    open_count_7d     INTEGER,            -- rolling 7-day open count
    open_count_30d    INTEGER,            -- rolling 30-day open count
    open_count_total  INTEGER,            -- lifetime open count
    recency_score     REAL,               -- 0-255 composite score
    version           INTEGER,            -- monotonic per-entity version
    updated_at        INTEGER             -- last inference update timestamp
);

CREATE INDEX idx_inference_updated_at ON inference(updated_at);
CREATE INDEX idx_inference_version    ON inference(version);
```

**Database location:** `%LOCALAPPDATA%\WARP\activity.db`
**Pragmas:** `journal_mode=WAL`, `synchronous=NORMAL`

> **Warning:** The database is actively written to by WARP. If you open it
> directly, use `PRAGMA query_only=ON` or open in read-only mode to avoid
> interfering with WARP's WAL writer. The named-pipe API is the recommended
> access method.

---

*This documentation describes WARP API version 5.11.*

*Changes from v5.10:*
- ***Embedding model upgraded from `BAAI/bge-small-en-v1.5` to
  `ibm-granite/granite-embedding-small-english-r2`.***  The dynamic
  context-inference clusterer now uses the ModernBERT-based granite
  encoder (47 M params, 384-dim, Apache 2.0, August 2025) instead of
  BGE-small.  Granite is explicitly trained on COIR (Code Information
  Retrieval) so it handles the code-heavy window titles WARP sees
  (`auth.cpp`, `useEffect`, `node_modules`, `OnnxRuntimeGenAI`)
  far better than BGE-small's WordPiece tokenizer, which would
  shatter such tokens into 4-6 sub-pieces and dilute the semantic
  signal.  Same 384-dim output -> the existing 0.65 cluster cosine
  threshold and downstream theme/verb scoring are unchanged.
  BGE-small + MiniLM remain as transparent fallbacks if the granite
  files aren't present.
- ***New: hand-rolled `ModernBertTokenizer.h`*** -- a header-only
  byte-level BPE tokenizer modelled on the existing
  `BertTokenizer.h` pattern (no JSON parser, no new NuGet packages).
  Loads three flat artefacts produced at CI time from the upstream
  `tokenizer.json` via `scripts/extract_modernbert_tokenizer.py`:
  `vocab.txt` (one byte-level-encoded token per line, line N = id
  N), `merges.txt` (one space-separated pair per line, in HF rank
  order), and `special_tokens.txt` (cls/sep/pad/unk/mask IDs).
  Implements NFC normalisation via Windows `NormalizeString`, the
  standard 256-entry GPT-2 byte->visible-codepoint map, a
  GPT-2-style pre-tokenizer hand-coded as a UTF-8 state machine
  (English contractions, Unicode-block letter/digit/whitespace
  classifiers, the `\s+(?!\S)` carve-out), and the lowest-rank-merge
  BPE loop with the `ignore_merges` fast path.  Verified
  **bit-exact** against `transformers.AutoTokenizer` for 15
  representative inputs (ASCII code-y titles, contractions, NFC
  accents, Greek, Japanese, Chinese mixed with English) using
  `scripts/modernbert_tokenizer_ref.py` (Python spec) +
  `scripts/tokenizer_test_driver.cpp` (standalone C++ harness).
- ***`ContextInference::Embed()` is now model-aware:*** for granite
  it sends `input_ids + attention_mask` (no `token_type_ids`) and
  reads the model's bundled `sentence_embedding` output directly --
  the ONNX graph already mean-pools.  The BGE / MiniLM legacy
  branch still sends the BERT 3-input shape and mean-pools
  `last_hidden_state` manually.
- ***Snapshot `model` field*** can now take the value
  `"granite-embedding-small-english-r2"` in addition to the existing
  `"bge-small-en-v1.5"`, `"all-MiniLM-L6-v2"`, and `"deterministic"`.
- ***Fixed: Qwen3 `<think>` block leaking into
  `summary_polished`.***  Qwen3 ships with chain-of-thought
  "thinking" mode enabled by default, which caused the polisher to
  emit reasoning prelude text (`<think>`, `"First, I need to extract
  the information from the user input..."`, etc.) instead of the
  actual summary because the 96-token budget was being consumed by
  the reasoning and never reached the answer.  `LlmSummarizer::
  BuildPrompt()` now appends the official Qwen3 no-think sentinel
  `<think>\n\n</think>\n\n` after the assistant header (replicating
  what `enable_thinking=False` does in the upstream
  `chat_template.jinja`); `Polish()` also strips any residual
  `<think>...</think>` block in post-processing as a safety net.
- ***Build pipeline:*** CI now downloads
  `onnx-community/granite-embedding-small-english-r2-ONNX`
  (`model_quantized.onnx` + `model_quantized.onnx_data`, ~52 MB
  INT8) plus the upstream `tokenizer.json`, runs
  `extract_modernbert_tokenizer.py` to produce the flat tokenizer
  files, then drops `tokenizer.json` from the staged artefact to
  keep the shipped model directory minimal.  The vcxproj copies the
  entire `models/granite/` subtree at post-build on every platform
  (x86 included -- granite is a pure ORT-CPU model, no ORT-GenAI
  dependency).

*Changes from v5.9:*
- ***LLM polishing model upgraded from Qwen2.5-0.5B-Instruct to
  `Qwen3-0.6B`.***  The polisher now uses the CPU-INT4 ORT-GenAI
  bundle `xiaoyao9184/Qwen3-0.6B-onnx-genai`
  (`cpu_and_mobile/cpu-int4-rtn-block-32-acc-level-4`, ~430 MB).
  Qwen3 is a substantially stronger instruct-tuned model than
  Qwen2.5 at the sub-1B tier (better instruction following,
  fewer hallucinations on short structured prompts) while keeping
  the same `<|im_start|>` / `<|im_end|>` chat template, so the
  prompt and post-processing pipeline are unchanged.  The
  `model_polish` field in snapshot responses now reads
  `"qwen3-0.6b"` when loaded.
- ***Build pipeline:*** the `Download LLM polishing model` step now
  fetches the 6-file Qwen3 bundle (`chat_template.jinja`,
  `genai_config.json`, `model.onnx`, `model.onnx.data`,
  `tokenizer.json`, `tokenizer_config.json`) instead of the
  10-file Qwen2.5 layout.  The staging step still bundles the
  entire `models/qwen/` subtree verbatim, so any future
  re-packaging works without code changes.
- ***Dependency bumps required for Qwen3:***
  `Microsoft.ML.OnnxRuntimeGenAI 0.7.0 -> 0.14.1` (the first
  release line that registers `"qwen3"` as a supported model type;
  0.7.0 fails with *unsupported model type* at `OgaCreateModel`)
  and the transitive `Microsoft.ML.OnnxRuntime 1.22.0 -> 1.23.0`.
  The C-API surface used by `LlmSummarizer.cpp` (`OgaCreateModel`,
  `OgaCreateTokenizer`, `OgaTokenizerEncode`,
  `OgaCreateGeneratorParams`, `OgaCreateGenerator`,
  `OgaGenerator_AppendTokenSequences`,
  `OgaGenerator_GenerateNextToken`,
  `OgaGenerator_GetSequenceData`, `OgaTokenizerDecode` and the
  matching `OgaDestroy*` entry points) is unchanged across
  0.7 -> 0.14, and the ORT 1.22 -> 1.23 jump only affects the
  underlying inference runtime DLL (no source changes needed in
  `ContextInference.cpp`'s embedding path).
- ***Embedding model deferred.***  Replacing BGE-small-en-v1.5 with
  `google/embeddinggemma-300m` was evaluated but **not landed in
  this revision**.  The architectural incompatibility is real and
  blocking:
    - embeddinggemma uses a SentencePiece BPE tokenizer
      (`tokenizer.model`), whereas the in-tree
      [`BertTokenizer`](BertTokenizer.h) is a hand-rolled WordPiece
      implementation that loads `vocab.txt`.
    - embeddinggemma produces 768-dim Matryoshka vectors;
      `ContextInference::EMBED_DIM` is hard-coded to 384 and the
      cluster centroid / cosine-similarity paths assume that size.
    - The base `google/embeddinggemma-300m` repo is gated
      (Gemma-license click-through); the community
      `onnx-community/embeddinggemma-300m-ONNX` mirror is open
      but still ships only sentencepiece tokenizer assets.
  Landing the swap cleanly requires either
  (a) pulling in `onnxruntime-extensions` to get a SentencePiece
  tokenizer op, or (b) vendoring Google's `sentencepiece` C++
  library into the build.  Both are tracked as follow-up work.

*Changes from v5.8:*
- ***Optional LLM polishing layer (`Qwen2.5-0.5B-Instruct`).***  Each
  snapshot now carries a `summary_polished` field alongside the
  existing template-composed `summary`.  When the LLM model files are
  present under `models/qwen/` and the polisher is loaded, the
  template summary + the per-app `items[]` are fed to a CPU-INT4
  Qwen2.5-0.5B-Instruct as a structured prompt, and the natural-
  prose rewrite is returned as 1-3 lines.  The polishing is purely
  additive -- the template summary remains the source of truth and
  is unchanged.  Polished output is **rejected** (empty array
  returned) when:
    - the LLM is not loaded (`model_polish == "(not loaded)"`)
    - the inference times out (5 s hard limit)
    - the output exceeds 160 chars per line or hallucinates content
      not present in the input items (grounding check).
- ***New fields on the snapshot:***
    - `summary_polished` (string[]) -- polished version of the
      category that matches the request.  Present in every response
      (empty array when polishing was unavailable or rejected).
    - `summary_files_polished`, `summary_websites_polished`,
      `summary_apps_polished` (string[]) -- per-category polished
      facets.  **Only present when `category == "all"`.**
    - `model_polish` (string) -- `"qwen2.5-0.5b-instruct"` when the
      polisher is loaded; `"(not loaded)"` otherwise.
- ***Dependency:*** *`Microsoft.ML.OnnxRuntimeGenAI 0.7.0`* (separate
  NuGet from `Microsoft.ML.OnnxRuntime`).  Ships its own BPE
  tokenizer + KV-cache + sampling loop, so the integration is
  ~200 lines of glue plus a model download.
- ***Build pipeline:*** CI now downloads the CPU-INT4 Qwen ONNX
  bundle (~330 MB) from
  `microsoft/Qwen2.5-0.5B-Instruct-onnx/cpu-int4-rtn-block-32-acc-level-4`
  on HuggingFace and stages it under `models/qwen/`.  The
  `CopyOnnxRuntime` MSBuild target copies the entire subtree
  verbatim to the output folder.  The ORT-GenAI NuGet only ships
  x64 and ARM64 binaries -- x86 builds compile without the
  polishing layer (graceful degrade).

*Changes from v5.7:*
- ***Umbrella detection on cross-cluster shared themes.***  When a single
  content token appears in titles of at least 50% of the top clusters AND
  those clusters together carry at least 50% of focus, the composer now
  collapses them into a single descriptive line of the form

      "Exploring <Umbrella> and its various aspects like <F1>, <F2> and <F3>"

  (with `"Researching"` instead of `"Exploring"` when the absorbed
  clusters are all browser-side, and `"Working on"` when they're all
  editor-side).  Each absorbed sub-cluster contributes its most
  distinctive non-umbrella token as a facet.  Facets are deduped and
  capped at 4 in the phrase.

  Example: three clusters about `Football Strategy.pdf`,
  `Football Players.docx`, and `Football Match Broadcasting.pptx`
  previously emitted three separate lines (`"Working on Football
  Strategy"` / `"Working on Football Players"` / `"Working on
  Broadcasting"`).  They now collapse into a single coherent line:

      "Exploring Football and its various aspects like Strategy, Players and Broadcasting"

  When the umbrella fires, the absorbed clusters are skipped in the
  rest of the summary; remaining (non-absorbed) clusters get their
  normal `<verb> <theme>` lines up to the 3-line cap.

- ***Dropped the `"… + N other thread(s)"` trailing suffix.***  The
  summary is now multi-line, so the per-line phrases stand on their
  own — there's no need to apologize for over-flow with a meaningless
  tail.  Clusters that don't fit the 3-line cap or don't meet the 5%
  focus floor are simply dropped from the summary (they're still
  counted in `thread_count` and visible in `items[]`).

- ***No request- or response-shape changes.***  All field names
  (`summary`, `summary_files`, `summary_websites`, `summary_apps`)
  and types are unchanged from v5.7.  Existing consumers see only
  better, more coherent summary lines.

*Changes from v5.6:*
- ***Context "one-liner" renamed to "context summary" and reshaped as
  an array of 1-3 phrase lines.***  The single-string `one_liner`
  field is replaced by a JSON-array `summary` field that holds
  separate phrase strings (one per cluster of activity).  Consumers
  render each entry on its own line, giving the user a multi-line
  semantic summary instead of a single bullet-separated line.
- ***Field renames:***
  - `one_liner`           → `summary`           (now `string[]`, was `string`)
  - `one_liner_files`     → `summary_files`     (now `string[]`, was `string`)
  - `one_liner_websites`  → `summary_websites`  (now `string[]`, was `string`)
  - `one_liner_apps`      → `summary_apps`      (now `string[]`, was `string`)
- ***Composer changes:***
  - Per-category bags (Files / Websites / Apps) no longer force every
    entry into one virtual cluster.  Natural sentence-encoder
    clustering runs over them, so each meaningful sub-thread becomes
    its own summary line.  (The original "force single cluster"
    workaround was needed because of pre-focus-weighted scoring; the
    focus-weighted scoring introduced in v5.6 makes it unnecessary
    and lets us get multi-line per-category summaries.)
  - Hard cap of 3 lines on the "All" summary (was: "All" was a
    single-string line of arbitrary length).  Each line is a
    `<verb> <theme>` phrase optionally followed by an `(across X &
    Y …)` tail.
  - Per-cluster phrases are no longer joined with ` · `; each is a
    separate array element.
- ***UI button labels:*** "Show Recent Context" → "Show Context
  Summary"; "Show Context History" → "Show Summary History".  The
  IPC operation names `GetRecentContext` / `GetRecentContexts` stay
  the same -- only the rendered field shape changes.
- ***Backward compatibility:*** **None for the JSON field names.**
  Consumers that read `one_liner` / `one_liner_files` / etc. must be
  updated to read `summary` / `summary_files` / etc. and to expect
  an array rather than a string.  The legacy `category=documents`
  enum alias for `category=files` is preserved.

*Changes from v5.5:*
- ***`category` enum renamed: `"documents"` → `"files"`.*** *The
  per-category facet was renamed to make it clear that it covers
  *any* file the user has open — `.docx` in Word, `.xlsx` in Excel,
  `.pptx` in PowerPoint, `.pdf` / `.jpg` / `.png` / `.heic` in image
  viewers, `.cpp` / `.json` / `.md` / `.txt` in any editor, etc. —
  not just "document" files in the colloquial sense.  The legacy
  value `"documents"` is still accepted (treated as an alias for
  `"files"`) so existing callers don't break.*
- ***Files category broadened.*** *Routing rules now form a strict
  three-way partition in priority order:* (1) *browsers → Websites
  only;* (2) *known comms / media / terminals / VMs / VCS UIs
  (Outlook, Teams, Slack, Discord, WhatsApp, Signal, Telegram,
  Zoom, Webex, Spotify, VLC, Windows Terminal, PowerShell, Remote
  Desktop, GitHub Desktop, Sourcetree, etc.) → Apps only, **even
  if** their title mentions a filename;* (3) *whitelisted file
  apps (editors, Office, OneNote, PDF readers, image viewers like
  Photos / IrfanView / Paint / Paint.NET / GIMP, design tools) →
  Files;* (4) *any other app whose window title contains a
  recognized file extension (45+ extensions across documents,
  spreadsheets, slides, PDFs, images, code, data, config,
  archives) → Files;* (5) *everything else → Apps.*
- ***Response shape: new `one_liner_files` field*** (replaces
  `one_liner_documents` from v5.3; same semantics, broader scope).
  *The combined `one_liner` plus the three per-category lines
  remain only present when `category == "all"`.*
- ***New `raw_title` field in `items[]`*** — the **full** original
  window title before any cleaning, so callers can see the
  complete context (e.g.
  `"● ContextInference.cpp - WARP! - Microsoft Visual Studio"`
  instead of just `"ContextInference.cpp - WARP"`).  The existing
  `title` (cleaned form) is unchanged.
- ***New `window_seconds` request parameter*** for both
  `GetRecentContext` and `GetRecentContexts`.  Allowed values
  (snapped to nearest match): `300, 900, 1800, 3600, 7200, 21600,
  86400, 604800, 1296000, 2592000` corresponding to 5 min / 15 min
  (**default**) / 30 min / 1 h / 2 h / 6 h / 24 h / 7 d / 15 d /
  30 d.  For 15 min WARP serves the cached snapshot from the
  60-sec background timer.  For any other value
  `GetRecentContext` composes a snapshot fresh on demand, and
  `GetRecentContexts` filters the rolling history to only return
  entries whose `timestamp` falls within the last
  `window_seconds`.  Echoed back in the response as
  `window_seconds` at both envelope and snapshot level.
- ***`windowSeconds` field added to every snapshot*** alongside
  `window_start` / `window_end` so callers don't have to compute it
  themselves.
- ***Better one-liner coherence.*** *The combined "All" one-liner
  is now hard-capped at the top 3 clusters by focus, and any
  cluster contributing less than 5 % of total focus is dropped
  from the head.  Theme tokens are now scored by `cross-title
  coverage × (1 + cosine-to-clean-cluster-centroid)` instead of
  raw frequency × cosine — so a token appearing in 3 different
  titles dominates a token appearing 5× in one title (the latter
  was the main cause of "mish-mash" one-liners).  Single-title
  tokens are demoted to 0.4× score when the cluster has 3+
  titles.  Per-category one-liners can now emit up to 3 theme
  tokens (up from 2) when the bag has 4+ titles, for richer
  themes.*
- ***UI: window-length dropdown added*** to the right of the
  category dropdown (10 entries from "Last 5 minutes" to "Last 30
  days", default "Last 15 minutes").  Both context buttons
  forward the selection as the `window_seconds` JSON parameter.
- ***UI: category dropdown item renamed*** from "Documents" to
  "Files" (mirrors the API enum change).

*Changes from v5.4:*
- ***Response shape is now category-gated.*** *`GetRecentContext` and
  `GetRecentContexts` only emit the **requested** category's one-liner
  in the response:*
  - *`category == "all"` (default) returns the combined one-liner as
    `one_liner` plus all three per-category lines
    (`one_liner_documents` / `_websites` / `_apps`).*
  - *`category == "documents" | "websites" | "apps"` returns **only**
    the matching category's one-liner as `one_liner`. The other
    categories are **omitted** from the response.*
- *Rationale: the previous v5.3 behavior of always emitting all four
  one-liners regardless of the requested category wasted ~3 of the 4
  one-liner strings worth of response bytes (and ~3× the per-snapshot
  embedding compute is irrelevant — those still run every cycle so the
  snapshot is ready for the next request — but the wire format now
  reflects what the caller asked for).*
- *The redundant `one_liner_all` field (which duplicated `one_liner`
  in the `category == "all"` response) has been removed.*
- *Consumers that were reading `one_liner_documents` / `_websites` /
  `_apps` from a non-`"all"` response must either (a) request
  `category=all` to receive all four, or (b) issue separate per-category
  requests.*

*Changes from v5.3:*
- *Switched the default sentence-encoder model from
  `sentence-transformers/all-MiniLM-L6-v2` to
  `BAAI/bge-small-en-v1.5`. Same 384-dim embedding, same BERT WordPiece
  tokenizer (no code-side tokenizer change), but **+6.5 MTEB Clustering**
  (42.4 → 48.9) — directly relevant to the short-text-clustering +
  theme-distillation workload that drives the `one_liner` and the
  per-category facets. ~33 M params (vs. 22 M for MiniLM), ~130 MB on
  disk (vs. ~86 MB).*
- *The `model` field in `GetRecentContext` / `GetRecentContexts`
  responses now reads `"bge-small-en-v1.5"` (or `"all-MiniLM-L6-v2"`
  when the legacy model is the only one present, or `"deterministic"`
  when no model file is present).*
- *Backward-compatible runtime: `ContextInference::Init()` looks for
  `models/bge-small.onnx` first and transparently falls back to
  `models/minilm.onnx` if BGE isn't present. Existing installations
  with only the legacy model file keep working without re-download.*
- *Build pipeline updated: `models/bge-small.onnx` is downloaded from
  HuggingFace at CI build time and copied next to `WARP!.exe` by the
  `CopyOnnxRuntime` MSBuild target. (`models/minilm.onnx` is still
  copied if present, for upgrade scenarios.)*
- *No request- or response-shape changes at v5.4. Existing API consumers
  saw only the `model` string change.*

*Changes from v5.2:*
- *Per-category one-liner facets. In addition to the combined `one_liner`,
  every snapshot now carries three independent one-liners —
  `one_liner_documents` (composed from non-browser document apps + recent
  file basenames), `one_liner_websites` (composed from browser tab
  titles), and `one_liner_apps` (composed from comms, terminals, media
  players, remote-desktop, and other non-document, non-browser apps).
  The three categories form a strict partition of the activity stream,
  so no app contributes to two facets.*
- *New optional `category` request parameter on `GetRecentContext` and
  `GetRecentContexts` (`"all"` | `"documents"` | `"websites"` | `"apps"`;
  default `"all"`). When non-`"all"`, the top-level `one_liner` field is
  mirrored from the matching per-category field so raw-JSON consumers
  see a focused response; the other three are always still emitted.*
- *Response also echoes the resolved `category` value at both the
  envelope and the snapshot level.*
- *UI: a category dropdown (`All` / `Documents` / `Websites` / `Apps`,
  default `All`) sits next to the *Show Recent Context* / *Show Context
  History* buttons and is plumbed through to the JSON request.*
- *History dedup widened: a new snapshot is now appended on a material
  change in **any** of the four one-liner fields (previously only the
  combined one).*

*Changes from v5.1:*
- *The `one_liner` is now a **semantic description** of what the user
  is doing, not a verbatim concatenation of document / tab / app
  titles. Each cluster's titles are tokenized into content tokens
  (whitespace + punctuation + camelCase split), filtered against
  stop-word, file-extension, and brand/app-name lists (also matched
  on the un-split form, so `YouTube` doesn't leak as `Tube`), and the
  top 1-2 surviving tokens — scored by `frequency × (1 + cosine-to-
  cluster-centroid)` when the sentence-encoder is loaded — become the cluster's
  **theme phrase**. The activity verb is chosen from a small fixed
  vocabulary (`Working on`, `Reviewing`, `Reading about`,
  `Researching`, `Discussing`, `Watching`, `Designing`, `Reading`)
  based on the dominant app type and content keywords. Multi-app
  clusters get an `(across App1, App2 & App3)` tail so consumers can
  still see *where* the work is happening.*
- *No response-shape change. The `items[]` breakdown still returns the
  **verbatim cleaned per-app title** for programmatic use; only the
  `one_liner` string is now semantic. `model`, `thread_count`, and
  per-item `thread_id` are unchanged from v5.1.*
- *Fallback: clusters whose titles yield zero usable content tokens
  (after stop-word / brand / extension filtering) revert to the prior
  verbatim format `<verb> "<title>" in <friendlyName>`, so no
  context-free verb is ever emitted.*

*Changes from v5.0:*
- ***Additive:*** *`GetRecentContext` response gains three fields:
  `model` (`"all-MiniLM-L6-v2"` or `"deterministic"` at this version;
  see v5.4 notes for the BGE-small default introduced later),
  `thread_count`
  (number of clustered threads of work), and a per-item `thread_id`
  (1-based cluster ID, items sharing it were merged). All previous fields
  are unchanged in shape — v5.0 consumers continue to work.*
- *The summarizer now uses the **MiniLM (`all-MiniLM-L6-v2`) ONNX**
  sentence-encoder for **dynamic semantic clustering** of the per-app
  phrases observed in the rolling 15-minute window. There is **no fixed
  taxonomy of buckets** — clusters are induced from the actual document/
  tab/app titles. Activities with cosine similarity ≥ 0.65 are merged into
  one *thread of work*, surfaced as `(with X & Y …)` in the one-liner.*
- *Graceful degradation: if `models/minilm.onnx` + `models/vocab.txt` are
  missing the engine logs and falls through to the deterministic per-app
  composer (the v5.0 behavior). `model` then reads `"deterministic"` and
  `thread_count` equals the number of items.*
- *Build adds NuGet `Microsoft.ML.OnnxRuntime 1.22.0`. The MiniLM model
  files are downloaded once into `models/` (instructions in `README.md`).*

*Changes from v4.0:*
- ***Breaking:*** *`GetRecentContext` response shape replaced. The previous fields
  (`topics[]`, `coverage_pct`, `model`) are gone. The new payload returns
  `one_liner` (a single human-readable summary naming the actual documents/tabs/
  apps the user is engaged with), `confidence`, `dominant_focus_pct`,
  `signal_types[]`, `items[]` (per-app breakdown with `app`, `exe`, `title`,
  `focus_seconds`, `pct`), and the `window_start` / `window_end` window bounds.*
- *New `"op": "GetRecentContexts"` request that returns the last `count` snapshots
  (newest first; default 10, hard-capped at 200) for short-term memory and context-
  drift charting.*
- *In v5.0 the summarizer was fully deterministic and shipped no ONNX Runtime
  or embedding model. (v5.1 reintroduces MiniLM but for **clustering**, not
  bucket matching — see the v5.1 notes above.)*
- *Recompute cycle changed from 5 minutes → 60 seconds. History buffer grew from
  288 → 1 440 entries (~24 hours at 60-sec cadence). New entries are appended
  only on material change (different one-liner, different dominant app, or a
  5-minute heartbeat).*
- *The summarizer overlays the user's currently-active foreground window as a
  virtual focus row, so a single 15-minute deep-work session is now correctly
  represented even though `AppFocusActivity` rows are only persisted on focus
  change.*

*Changes from v3.0:*
- *New `"op": "GetRecentContext"` request that returns a snapshot of what the
  user is currently working on. (See v5.0 notes for the current response shape.)*
- *All other operations are unchanged and fully backward compatible.*

*Changes from v2.0:*
- *New `"op": "QueryInferences"` request for batch lookup of precomputed per-entity
  inference records (recency scores, access counts, timestamps).*
- *New `"op": "GetInferenceDeltas"` request for incremental sync of inference
  records using a version watermark.*
- *New `inference` table in the SQLite schema.*
- *Existing event query requests (`"window"`, `"seconds"`, `"types"`) are unchanged
  and fully backward compatible.*

*Changes from v1.0:*
- *Event types are segregated at the root level rather than in a single flat array.*
- *The `"types"` request field was introduced. Requests without `"types"` return
  all event types (backward compatible).*

*New fields may be added to response objects in future versions, but existing
fields will not be removed or change meaning.*

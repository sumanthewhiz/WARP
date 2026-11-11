# WARP Activity API -- Integration Guide

> **API version:** 2 -- every response carries `"api_version": 2`; v1 requests keep working
> **Server:** `ApiServer` in WARP 6.0 (replaces the v1 `QueryApi`)
> **Pipe endpoints:** `\\.\pipe\WarpFileActivityAPI-<SessionId>` (preferred) and `\\.\pipe\WarpFileActivityAPI` (legacy, same-session clients only)
> **Transport:** local Windows named pipe, message mode, one JSON request -> one JSON response, keep-alive (reconnect when the server closes a connection)
> **Encoding:** UTF-8 JSON (strict RFC 8259)

This document describes how applications running on the same Windows machine, in the
same user session, can connect to WARP and query activity history -- file, app-launch,
app-focus and browsing events -- together with precomputed per-entity engagement
(scores, counts, human-readable reasons), an incremental change feed, the work
session / work thread history and a summary of what the user is doing right now.

---

## Table of Contents

- [Overview](#overview)
  - [What Changed in API v2](#what-changed-in-api-v2)
- [Prerequisites](#prerequisites)
- [Connection](#connection)
  - [Pipe Details](#pipe-details)
  - [Connection Lifecycle](#connection-lifecycle)
  - [Server Deadlines](#server-deadlines)
  - [Verifying the Server](#verifying-the-server)
- [Access Control](#access-control)
  - [Caller Identification](#caller-identification)
  - [Policy Settings](#policy-settings)
  - [Scopes](#scopes)
- [Limits](#limits)
- [Request Format](#request-format)
- [Responses and Errors](#responses-and-errors)
  - [Error Format](#error-format)
  - [Error Codes](#error-codes)
- [Entity Keys and Normalization](#entity-keys-and-normalization)
- [Operations](#operations)
  - [Operation Summary](#operation-summary)
  - [1. Raw Events (v1)](#1-raw-events-v1)
  - [2. Inference Lookups (v1-compatible)](#2-inference-lookups-v1-compatible)
    - [QueryInferences](#queryinferences)
    - [GetInferenceDeltas](#getinferencedeltas)
  - [3. Entities](#3-entities)
    - [ScoreEntities](#scoreentities)
    - [TopEntities](#topentities)
    - [SearchActivity](#searchactivity)
    - [GetChanges](#getchanges)
    - [WaitForChanges](#waitforchanges)
  - [4. Timeline and History](#4-timeline-and-history)
    - [QueryTimeline](#querytimeline)
    - [GetWorkThreads](#getworkthreads)
    - [GetRelated](#getrelated)
  - [5. Context](#5-context)
    - [GetRecentContext](#getrecentcontext)
    - [GetRecentContexts](#getrecentcontexts)
    - [GetCurrentContext](#getcurrentcontext)
    - [Context Snapshot Fields](#context-snapshot-fields)
  - [6. Status and Control](#6-status-and-control)
    - [Ping](#ping)
    - [GetStatus](#getstatus)
    - [Purge](#purge)
    - [Pause and Resume](#pause-and-resume)
- [Entity Records and Scores](#entity-records-and-scores)
  - [Entity Record Fields](#entity-record-fields)
  - [Score Fields](#score-fields)
  - [How Scores Work](#how-scores-work)
- [Data Retention](#data-retention)
- [Integration Patterns](#integration-patterns)
  - [Recent Items Picker](#1-recent-items-picker)
  - [Context-Aware Ranking](#2-context-aware-ranking)
  - [Local Index Sync](#3-local-index-sync)
  - [Backup & Sync Engine](#4-backup--sync-engine)
  - [Security Auditing](#5-security-auditing)
  - [Developer Tooling](#6-developer-tooling)
  - [Smart Cleanup Utility](#7-smart-cleanup-utility)
  - [App Usage Analytics](#8-app-usage-analytics)
  - [Browsing History Dashboard](#9-browsing-history-dashboard)
  - [Work Resumption](#10-work-resumption)
  - [Semantic Context Awareness](#11-semantic-context-awareness)
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

WARP continuously records user activity on the local PC:

| Event type | What is captured | Source |
|---|---|---|
| **File activity** | Opens, creates, modifications, deletes, renames and downloads of files | Kernel file I/O ETW, NTFS USN journal, shell Recent items, Office MRU (see the `source` field) |
| **App launches** | User-visible process starts: exe name and path, PID, parent process, AUMID of packaged apps | Kernel process ETW |
| **App focus** | Foreground sessions: app, window title, start / end, active (non-idle) seconds and the document or page that was shown | WinEvent foreground hooks + presence |
| **Browsing** | Title, canonical (privacy-filtered) URL and host of the page shown in a supported browser | UI Automation |
| **Presence** | `active` / `passive` / `away` / `locked` / `asleep` intervals | Input idle time, media / microphone / camera use, lock and power state |

Content from private browsing windows and from the apps, window titles, domains and
folders excluded in WARP's settings is not recorded.

On top of the raw events WARP maintains:

- **Entities** -- one record per file, app and web page / document with decayed engagement,
  counts and a per-type percentile score, plus a global change feed.
- **History** -- work sessions (presence split by gaps of 15 minutes or more), work threads
  (groups of entities used together, resumable across days, with a project anchor) and
  co-occurrence edges between entities.
- **Context** -- a summary of what the user is doing, recomposed every
  `ContextCadenceSeconds` (default 60 s), together with the entities behind it.

Everything is stored locally in SQLite (`%LOCALAPPDATA%\WARP\activity.db`). Raw events are
kept for 30 days and entities / history for 395 days by default (see
[Data Retention](#data-retention)).

**Typical use cases:**

| Use case | Operation |
|---|---|
| "Recent" / "frequent" files, apps or pages | [`TopEntities`](#topentities) |
| Rank or boost a candidate list (search results, file picker, launcher) | [`ScoreEntities`](#scoreentities), optionally with `"context": "current"` |
| Find something the user worked on by name | [`SearchActivity`](#searchactivity) |
| Keep a local index in sync | [`GetChanges`](#getchanges) + [`WaitForChanges`](#waitforchanges) |
| Audit or replay a time range | [`QueryTimeline`](#querytimeline) (paged) or [raw events](#1-raw-events-v1) |
| "What was I working on yesterday?" | [`GetWorkThreads`](#getworkthreads), `QueryTimeline` with `sessions` / `threads` |
| "Items related to this file" | [`GetRelated`](#getrelated) |
| What is the user doing right now? | [`GetCurrentContext`](#getcurrentcontext), [`GetRecentContext`](#getrecentcontext) |
| Context drift / short-term memory | [`GetRecentContexts`](#getrecentcontexts) |
| v1 integrations (recency scores, delta sync) | [`QueryInferences`](#queryinferences), [`GetInferenceDeltas`](#getinferencedeltas) |
| Health checks and diagnostics | [`Ping`](#ping), [`GetStatus`](#getstatus) |

### What Changed in API v2

API v2 is served by `ApiServer` (WARP 6.0), which replaces the v1 `QueryApi`. v1 requests --
raw event queries, `QueryInferences`, `GetInferenceDeltas`, `GetRecentContext` and
`GetRecentContexts` -- keep their request and response shapes. What changed:

| Area | v1 (`QueryApi`) | v2 (`ApiServer`) |
|---|---|---|
| Versioning | no version field | `"api_version": 2` is the first member of every response, including errors |
| Pipe | one global name, default security | per-session name `WarpFileActivityAPI-<SessionId>` (the legacy name is kept for same-session clients), explicit DACL, medium integrity label, remote clients rejected |
| Callers | anonymous | identified by PID, session and image; optional allow-list and scopes (see [Access Control](#access-control)) |
| Errors | silent fallbacks: malformed JSON, an unknown `op` or an unknown `window` returned the last hour of raw events | structured errors `{"ok": false, "error": {"code", "message"}}` (see [Error Codes](#error-codes)) |
| Sizes | 64 KB buffers; larger responses were cut off | requests up to 1 MiB; responses of any size (read until the message is complete); hard row limits with `truncated` flags and cursors |
| Connections | one request per connection | keep-alive: up to 256 requests per connection; the server closes a kept-alive connection after any response while other clients are waiting, so clients must be ready to reconnect |
| Server resources | unlimited instances, no deadlines | 8 workers, 64 queued connections (beyond that: a `busy` error), per-connection deadlines, at most 4 concurrent long-polls |
| Operations | raw events + 4 ops | raw events + 18 ops: adds `ScoreEntities`, `TopEntities`, `SearchActivity`, `GetChanges`, `WaitForChanges`, `QueryTimeline`, `GetWorkThreads`, `GetRelated`, `GetCurrentContext`, `Ping`, `GetStatus`, `Purge`, `Pause` and `Resume` |
| Raw events | file, app launch, browsing | adds `app_focus`, `confidence`, `source`, `entity_key`, `limit`, `min_confidence`, `truncated` and paging with `before_id` / `next_before_id` |
| Inference `version` | per-entity counter | the global `change_seq` (monotonic across all entities); `GetInferenceDeltas` also returns `deleted`, `next_version`, `current_version`, `more` and `reset` |

**Phase A (`dev`, storage schema 3):** the transport stays API v2. Attention now
separates active/passive/idle/unknown, web keys preserve case-sensitive identity,
and persisted history supports every facet. Missing raw attention is `null`, not
fabricated wall time. See [SCHEMA.md](SCHEMA.md) for migration/retention semantics.

---

## Prerequisites

| Requirement | Details |
|---|---|
| **WARP running in your session** | WARP is a per-user tray (or `--headless`) application that runs elevated in the interactive session; every Windows session has its own instance and its own pipe. While nothing listens, opening the pipe fails with `ERROR_FILE_NOT_FOUND` (2). |
| **No elevation for clients** | The pipe grants read/write access to the interactive user at medium integrity, so ordinary (non-elevated) desktop apps of that user can connect. Low-integrity and AppContainer (sandboxed UWP / MSIX) processes can connect only when policy `ApiAllowAppContainerClients` = 1. Processes of other user accounts cannot connect (SYSTEM and elevated administrators can). |
| **Same machine** | Remote (SMB) clients are rejected. |
| **Windows 10 or later** | Required by WARP. Clients only need the Win32 named-pipe API, available from any language or runtime. |

---

## Connection

### Pipe Details

| Property | Value |
|---|---|
| **Pipe name (preferred)** | `\\.\pipe\WarpFileActivityAPI-<SessionId>`, where `<SessionId>` is the Windows session of the client process (`ProcessIdToSessionId(GetCurrentProcessId(), &id)`; .NET: `Process.GetCurrentProcess().SessionId`). |
| **Pipe name (legacy)** | `\\.\pipe\WarpFileActivityAPI`, kept for v1 clients. Served only to clients in the same session as the WARP instance that owns it; callers from other sessions get [`wrong_session`](#error-codes), whose message names the pipe to use. When several sessions run WARP (fast user switching, RDP) only one instance can own the legacy name. |
| **Pipe mode** | `PIPE_TYPE_MESSAGE \| PIPE_READMODE_MESSAGE` -- every request and every response is exactly one message. |
| **Client access** | Open with `GENERIC_READ \| GENERIC_WRITE`, then switch the handle to `PIPE_READMODE_MESSAGE`. |
| **Security descriptor** | `D:P(D;;GA;;;NU)(A;;GA;;;SY)(A;;GA;;;BA)(A;;0x12019f;;;<user SID>)S:(ML;;NW;;;ME)` -- network logons denied, SYSTEM and Administrators full access, the interactive user (the account WARP runs as) `FILE_GENERIC_READ \| FILE_GENERIC_WRITE` (`0x12019f`), medium mandatory label. With `ApiAllowAppContainerClients` = 1 WARP adds `(A;;0x12019f;;;AC)` (all AppContainers) and uses the low label `S:(ML;;NW;;;LW)`. The descriptor is built when the API starts. |
| **Remote clients** | Rejected (`PIPE_REJECT_REMOTE_CLIENTS` and the `NU` deny entry). |
| **Instances** | Unlimited; a new instance is armed before each accepted connection is handed to a worker. The 64 KiB pipe buffers are not a message-size limit. |
| **Name squatting** | WARP creates the first instance of each name with `FILE_FLAG_FIRST_PIPE_INSTANCE`. If another process already owns the name, WARP logs it, retries every 10 s and reports `session_pipe_owned` / `legacy_pipe_owned` = `false` in [`GetStatus`](#getstatus). Clients should [verify the server](#verifying-the-server). |

### Connection Lifecycle

1. **Compute the pipe name** from your session id (see above).
2. **Open** it with `CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL)`.
   - `ERROR_PIPE_BUSY` (231): call `WaitNamedPipeW(name, timeout)` and retry.
   - `ERROR_FILE_NOT_FOUND` (2): WARP is not running in this session. (WARP builds before
     API v2 only expose the legacy name; a client may fall back to it.)
   - `ERROR_ACCESS_DENIED` (5): see [Troubleshooting](#troubleshooting).
3. **Optionally verify the server** with `GetNamedPipeServerProcessId` (see
   [Verifying the Server](#verifying-the-server)).
4. **Set message mode**: `DWORD mode = PIPE_READMODE_MESSAGE;`
   `SetNamedPipeHandleState(h, &mode, NULL, NULL)`.
5. **Write** one UTF-8 JSON request with a single `WriteFile` call (one message, at most 1 MiB).
6. **Read** the response: call `ReadFile` until it succeeds; while it fails with
   `ERROR_MORE_DATA` (234), keep what was read and call it again. Responses are routinely
   larger than any fixed buffer.
7. **Send further requests** on the same handle (keep-alive) or **close** it.

Requests on one connection are processed strictly in order: send the next request only
after the previous response has been read completely.

**Be ready to reconnect.** The server may close a kept-alive connection after any response
(see below). A reply that was written is still delivered -- the server only closes its
handle, so the client can read it to the end. The closure shows up on the next request:
when `WriteFile` or `ReadFile` fails with `ERROR_NO_DATA` (232),
`ERROR_PIPE_NOT_CONNECTED` (233) or `ERROR_BROKEN_PIPE` (109), open a new connection and
send the request again -- every operation except `Purge`, `Pause` and `Resume` is
read-only and safe to repeat.

### Server Deadlines

| Deadline | Value | What happens |
|---|---|---|
| First request | 5 s after a worker picks up the connection | connection closed without a response |
| Next request on a kept-alive connection | 10 s idle | connection closed |
| Other connections waiting in the accept queue | checked after every response | connection closed after the response: the worker moves on to the waiting client, and you reconnect for your next request |
| Delivering a response | 30 s (the client must keep reading) | connection closed |
| Requests per connection | 256 | connection closed after the 256th response |
| Request size | 1 MiB | `request_too_large` is returned, then the connection is closed |
| Rejected client (`wrong_session`, `unauthorized`) | first request | the error answers the first request, then the connection is closed |

An open connection occupies one of the 8 server workers until it is closed; queued
connections are served as workers free up. When all workers are busy and 64 connections
are already queued, a new connection is answered with
`{"api_version":2,"ok":false,"error":{"code":"busy","message":"the server is busy; retry shortly"}}`
and closed (counted as `busy` in [`GetStatus`](#getstatus)). The error is written as soon
as the connection is accepted, possibly before your request: if your `WriteFile` fails, a
`ReadFile` can still return it. Back off and retry. A zero-byte message also closes the
connection.

### Verifying the Server

Any process of the user could create a pipe with WARP's name while WARP is not running
(WARP itself detects squatting when it starts, see above). Before sending sensitive
requests, check who serves the pipe:

1. `GetNamedPipeServerProcessId(h, &pid)` returns the server's PID.
2. Resolve the image path of that PID (`OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION)` +
   `QueryFullProcessImageNameW`) and check that it is WARP's executable (`WARP!.exe` in its
   install location; optionally verify its signature).
3. [`GetStatus`](#getstatus) reports WARP's own `pid`; it must equal the PID from step 1.

---

## Access Control

### Caller Identification

For every connection WARP calls `GetNamedPipeClientProcessId` and
`GetNamedPipeClientSessionId` and resolves the client's image path with
`QueryFullProcessImageNameW`. The checks below use the lower-cased exe **file name**
(e.g. `myapp.exe`); only the control scope compares the full path.

1. **Session** (legacy pipe only): a client from another session gets `wrong_session`.
2. **Allow-list**: when `ApiAllowedClients` is non-empty, a client whose exe name is not
   listed (or cannot be determined) gets `unauthorized`.
3. **Activity scope**: when `ApiRestrictActivityScope` = 1, only exe names listed in
   `ApiActivityClients` receive the activity scope (titles, URLs, paths, timeline);
   everyone else gets `forbidden` for the activity operations (see [Scopes](#scopes)).
4. **Control scope**: `Purge`, `Pause` and `Resume` are accepted only when the client's full
   image path equals WARP's own executable -- i.e. the WARP command line
   (`WARP!.exe --status`, `--pause [minutes]`, `--resume`, `--purge-hours <n>`,
   `--purge-domain <host>`, `--purge-path <folder>`) or WARP's tray UI. Every other client
   gets `forbidden`.

`wrong_session` and `unauthorized` answer the first request and close the connection;
`forbidden` is returned per request and the connection stays usable.

### Policy Settings

Settings are layered: defaults <- `HKCU\Software\WARP` <- `HKLM\Software\Policies\WARP`
(IT policy, 64-bit view; policy values win and are locked). Lists are `REG_MULTI_SZ` (or a
`;`-separated `REG_SZ`) of exe file names. WARP reads them at start-up; restart WARP after
changing them.

| Value | Type | Default | Effect |
|---|---|---|---|
| `ApiAllowedClients` | list | empty = any local client that passes the pipe DACL | Exe names allowed to use the API; others get `unauthorized`. |
| `ApiRestrictActivityScope` | DWORD 0/1 | 0 | 1 = only `ApiActivityClients` get the activity scope. |
| `ApiActivityClients` | list | empty | Exe names granted the activity scope while `ApiRestrictActivityScope` = 1. |
| `ApiAllowAppContainerClients` | DWORD 0/1 | 0 | 1 = AppContainer and low-integrity clients may open the pipe (adds the `AC` entry and a low integrity label). |

### Scopes

| Scope | Operations | Without the scope |
|---|---|---|
| none (any client allowed to connect) | `Ping`, `GetStatus`, `QueryInferences`, `ScoreEntities`, `WaitForChanges` | -- |
| **activity** (`kActivityOps`, plus raw event queries) | raw event queries (no `op`), `GetInferenceDeltas`, `TopEntities`, `SearchActivity`, `GetChanges`, `QueryTimeline`, `GetWorkThreads`, `GetRelated`, `GetRecentContext`, `GetRecentContexts`, `GetCurrentContext` | `forbidden` |
| **control** (`kControlOps`) | `Purge`, `Pause`, `Resume` | `forbidden` |

Clients without the activity scope can still score entities they already know:
`ScoreEntities` then omits `display_name` and `title`, and `QueryInferences` never returns
names or titles. `GetStatus` answers every client, but only WARP's own executable (control
scope) also receives the database, log and data-folder paths, recent log messages, capture
error texts, the media / microphone / camera flags and the effective settings; other
clients get the health summary (see [GetStatus](#getstatus)).

---

## Limits

| Limit | Value | When exceeded |
|---|---|---|
| Request size | 1 MiB (`kMaxRequestBytes`) | `request_too_large`, connection closed |
| JSON nesting depth | 32 | `invalid_json` |
| Server workers | 8 (one connection each) | connections wait in the queue; a kept-alive connection is closed after its current response while others wait |
| Accept queue | 64 connections | `busy` error, connection closed (`GetStatus.api.busy`) |
| Requests per connection | 256 | connection closed |
| Concurrent `WaitForChanges` | 4 (server-wide) | immediate answer with `retry_after_ms: 1000` |
| `WaitForChanges.timeout_ms` | default 20000, clamped to 0-25000 | clamped |
| Keys per `QueryInferences` / `ScoreEntities` | 5000 | `too_many_keys` |
| `SearchActivity.query` | 512 bytes (at most 8 search tokens are used) | `invalid_request` |
| Raw events `limit` | default 5000, clamped to 1-50000 per event type | `truncated: true` + `next_before_id` |
| Raw events `seconds` | must be > 0; capped at 400 days | `invalid_request` / capped |
| `QueryTimeline.limit` | default 1000, clamped to 1-20000 per event type | `next_cursor` |
| `QueryTimeline` sessions / threads | 500 sessions, 50 threads (not paged) | cut |
| `GetChanges.limit` | default 1000, clamped to 1-5000 (upserts + deletes) | `more: true` |
| `GetInferenceDeltas.limit` | default 5000, clamped to 1-5000 | `more: true` |
| `TopEntities.limit` | default 50, clamped to 1-1000 | clamped |
| `SearchActivity.limit` | default 25, clamped to 1-200 | clamped |
| `GetWorkThreads.limit` | default 20, clamped to 1-200 (top 15 members per thread) | clamped |
| `GetRelated.limit` | default 20, clamped to 1-100 | clamped |
| `GetRecentContexts.count` | default 10 (also for values <= 0), max 200 | clamped |
| Context `window_seconds` | clamped to 0-30 days, then snapped to an allowed window | snapped |
| `Pause.minutes` | default 60, clamped to 1-10080 (7 days) | clamped |

Out-of-range numbers are clamped silently; parameters of the wrong JSON type are treated as
absent (their default applies).

---

## Request Format

Every request is one JSON object, UTF-8 encoded, parsed strictly (RFC 8259: no comments,
trailing commas, single quotes, `NaN`, BOM or trailing data).

| Rule | Detail |
|---|---|
| `op` | String naming the operation, case-sensitive. A request without `op` (or with an empty `op`) is a v1 [raw event query](#1-raw-events-v1). |
| Unknown members | Ignored. If a member appears twice, the last one wins. |
| Wrong types | A parameter of the wrong type falls back to its default (`"limit": "10"` is treated as absent). Exception: `paths` / `keys` must be arrays (`invalid_request`). |
| Numbers | Integer parameters given with a fraction are truncated toward zero. |
| Timestamps | Unix epoch **seconds**, UTC, in requests and responses. |
| Aliases | `paths` / `keys`, `since` / `since_seq`, `window_seconds` / `window_secs` (see each operation). |

```json
{"op": "TopEntities", "type": "file", "limit": 10}
```

---

## Responses and Errors

Every response is one JSON object whose first member is `"api_version": 2`.

- v2 operations add `"ok": true` on success.
- v1-shaped responses -- raw events, `QueryInferences`, `GetInferenceDeltas`,
  `GetRecentContext`, `GetRecentContexts` -- keep their v1 shape and carry **no** `ok`
  member on success. Treat a response without an `error` member as a success.
- Errors always carry `"ok": false` and an `error` object.
- Strings are valid UTF-8 (invalid sequences become U+FFFD) and control characters are
  escaped; `NaN` is written as `null`.
- Integers are 64-bit. `paused_until` is `9223372036854775807` (INT64_MAX) while capture is
  paused until resumed -- more than JavaScript's `Number` represents exactly.
- New members may be added to response objects; ignore members you do not know.

### Error Format

```json
{"api_version":2,"ok":false,"error":{"code":"invalid_request","message":"'query' is required"}}
```

`code` is stable and meant for programs; `message` is human-readable and may change.

### Error Codes

| Code | Meaning | Typical causes | Connection |
|---|---|---|---|
| `invalid_json` | The request is not valid JSON; the message includes the byte offset. | malformed or truncated JSON, invalid UTF-8, nesting deeper than 32 | stays open |
| `invalid_request` | Valid JSON, but not a valid request. | not an object; unknown `window`; `seconds` <= 0; `paths` / `keys` not an array; missing or over-long `query`; `horizon` not `short` / `long` / `blend` (`ScoreEntities`, `TopEntities`); `type` not `file` / `app` / `url` (`TopEntities`, `SearchActivity`); invalid `start` / `end`; missing `key` / `entity_key` (`GetRelated`); `Purge` without exactly one kind of criterion, or with `start` > `end`; `Pause` `until` not in the future | stays open |
| `unknown_op` | `op` names no operation. | typo, wrong case, operation from a newer API version | stays open |
| `too_many_keys` | More than 5000 keys in one request. | `QueryInferences`, `ScoreEntities` | stays open |
| `unavailable` | A component the operation needs is not running. | the activity store is not open (every operation except `Ping` and `GetStatus` then fails this way); entity store, history engine or context inference not started; pause not supported | stays open |
| `forbidden` | The client lacks the scope the operation needs. | activity operation while `ApiRestrictActivityScope` = 1 and the exe is not in `ApiActivityClients`; `Purge` / `Pause` / `Resume` from any program other than WARP | stays open |
| `unauthorized` | The client is not in the allow-list. | `ApiAllowedClients` is set and does not list the client's exe name | closed after the response |
| `wrong_session` | Legacy pipe used from another Windows session. | the message names the per-session pipe to use | closed after the response |
| `busy` | The server is saturated: all 8 workers are busy and 64 connections are already queued. | written as soon as the connection is accepted, possibly before your request; back off and retry | closed |
| `request_too_large` | The request exceeds 1 MiB. | | closed after the response |
| `internal_error` | Unexpected failure while handling the request (including out of memory). | also a `Purge` that did not complete (store busy or closed) | stays open |
| `locked` | `Pause` / `Resume` could not change the pause state. | `PausedUntilUtc` is set by policy, or the registry write failed; returned inside the normal `Pause` / `Resume` response together with `paused` and `paused_until` | stays open |

---

## Entity Keys and Normalization

Entities -- files, apps and web pages / documents -- are identified by an `entity_key`
that is shared by every operation, by the raw events (`entity_key`, `doc_key`) and by the
context snapshots. Keys are UTF-8 and **typed**: file paths/AUMIDs remain
case-folded; URL paths/query values and opaque document-ID payloads preserve case.

| `entity_type` | Key | Example |
|---|---|---|
| `file` | canonical full path: `\\?\` and `\Device\Mup\` prefixes unwrapped, `/` -> `\`, `.` / `..` resolved, duplicate and trailing separators and NTFS stream suffixes removed, 8.3 short names expanded | `c:\users\alice\documents\report.docx` |
| `app` | the exe path as a file key, or `aumid:` + the AUMID for packaged apps | `c:\program files\microsoft vs code\code.exe`, `aumid:microsoft.windowsterminal_8wekyb3d8bbwe!app` |
| `url` | a stable document identity when the site is recognized (`gh:`, `ado:`, `jira:`, `gdoc:`, `gdrive:`, `spo:`, `spo-path:`, `od:`, `figma:`, `yt:`), otherwise the canonical, privacy-filtered URL; the page title for pages without a URL | `gh:contoso/app#pr12`, `https://learn.microsoft.com/windows/win32/ipc/named-pipes` |

Operations that take **raw identifiers** -- `QueryInferences.paths`, `ScoreEntities.keys`,
`GetRelated.key`, `Purge.key` -- normalize every string the way WARP's own producers do.
The first matching rule wins:

1. A key WARP hands out round-trips: strings starting with `aumid:`,
   `doc:`, `yt:`, `gdoc:`, `gdrive:`, `spo:`, `spo-path:`, `od:`, `gh:`, `ado:`, `figma:` or
   `jira:` (prefix matched case-insensitively, followed by at least one character).
   Only the provider prefix is folded for document keys; AUMIDs remain case-folded.
2. A drive-letter path (`C:\...`, `C:/...`) or UNC path (`\\server\share\...`) is a file
   path -> file key.
3. A string containing `!` and no `\` or `/` is a raw packaged-app AUMID
   (`Family_hash!App`) -> `aumid:<aumid>`.
4. A URL or bare domain is canonicalized (fragment, user info and most query parameters
   removed; sign-in, password-manager, banking and patient-portal hosts excluded) and mapped
   to its document identity or canonical URL. Excluded or unusable URLs normalize to an
   empty key and are never found.
5. Anything else is treated as a file path -> file key.

Responses echo your exact strings: `QueryInferences` keys its `results` by them and
`ScoreEntities` returns `key` (yours) next to `entity_key` (normalized).

**Identity format 2 (default).** `https://example.org/PlanA` is not `/plana`;
`yt:AbCdEf12345` is not `yt:abcdef12345`. Do not lowercase keys in clients.
For `ScoreEntities` and `QueryInferences`, optional `identity_version: 1` explicitly
resolves retained old folded-key aliases; `2` is the default. Other values/types
return `invalid_request`. An ambiguous/unrecoverable/expired legacy web identity
returns not found, never an arbitrary case variant. A newly seen case variant can
invalidate a previously unique alias. Purge and other operations use canonical
format-2 keys; resolve an old key first if necessary.
Legacy mode takes already-canonical keys returned by old WARP, rather than
reinterpreting old URL keys as new provider document IDs.

The v3 migration requests a change-feed reset for affected existing history.
Discard cached old keys and resync. Unrecoverable collided aggregate history is
not guessed; see [upgrade behavior](SCHEMA.md#upgrade-behavior).

**Round trips.** Every `entity_key` WARP returns -- file keys, exe-path and `aumid:` app
keys, document identities and canonical URLs -- normalizes to itself, so keys from
`GetChanges`, `TopEntities`, `SearchActivity` or `current_entities[]` can be passed straight
back. The exception are pages recorded without a URL, which are keyed by their lower-cased
title: a title that contains `!`, `:`, `/` or `\`, or looks like a domain, may not
round-trip. For those use the `entity_key` parameter of `GetRelated` or `Purge`, which is
used verbatim.

---

## Operations

### Operation Summary

| Op | Group | Scope | Purpose |
|---|---|---|---|
| *(none)* | [Raw events (v1)](#1-raw-events-v1) | activity | Raw events of a recent window |
| `QueryInferences` | [Inference (v1)](#queryinferences) | -- | Per-entity records for keys you supply |
| `GetInferenceDeltas` | [Inference (v1)](#getinferencedeltas) | activity | v1 change feed |
| `ScoreEntities` | [Entities](#scoreentities) | -- | Scores, records and reasons for keys you supply |
| `TopEntities` | [Entities](#topentities) | activity | Top-N entities by engagement |
| `SearchActivity` | [Entities](#searchactivity) | activity | Search entity names and titles |
| `GetChanges` | [Entities](#getchanges) | activity | Change feed: upserts and deletes since a cursor |
| `WaitForChanges` | [Entities](#waitforchanges) | -- | Long-poll until the change sequence moves |
| `QueryTimeline` | [Timeline](#querytimeline) | activity | Events, work sessions and threads of a time range, paged |
| `GetWorkThreads` | [History](#getworkthreads) | activity | Work threads with their member entities |
| `GetRelated` | [History](#getrelated) | activity | Entities used together with an entity |
| `GetRecentContext` | [Context](#getrecentcontext) | activity | Latest context snapshot for a window |
| `GetRecentContexts` | [Context](#getrecentcontexts) | activity | Recent snapshot history, newest first |
| `GetCurrentContext` | [Context](#getcurrentcontext) | activity | Presence, foreground window, latest snapshot, current thread |
| `Ping` | [Status](#ping) | -- | Liveness and product version |
| `GetStatus` | [Status](#getstatus) | -- | Health, statistics, settings |
| `Purge` | [Control](#purge) | control | Delete recorded data |
| `Pause` / `Resume` | [Control](#pause-and-resume) | control | Pause or resume capture |

### 1. Raw Events (v1)

A request without `op` returns the raw events of a recent window, grouped by event type.
This is the v1 query; v2 adds app-focus sessions, a row limit and a confidence filter.
Requires the activity scope.

| Param | Type | Default | Notes |
|---|---|---|---|
| `window` | string | -- | `15m`, `30m`, `1h`, `2h`, `6h`, `24h`, `7d`, `15d` or `30d`; any other value -> `invalid_request`. Takes precedence over `seconds`. |
| `seconds` | integer | -- | Look-back in seconds; must be > 0 (`invalid_request` otherwise); capped at 400 days. |
| *(neither)* | | 1 hour | |
| `types` | string[] | all four | `file`, `app_launch`, `browsing`, `app_focus`. Unknown values are ignored; if no valid value remains, all types are returned. |
| `limit` | integer | 5000 | Maximum events **per type**, clamped to 1-50000. |
| `min_confidence` | number | 0.0 | Drops events whose `confidence` is lower. |
| `before_id` | integer or object | -- | Paging: pass back a `next_before_id` to get the events older than that row. A number applies to every requested type (ids are per type, so use it with a single type); an object keyed by type -- `{"file": 1041, "browsing": 877}`, keys `file`, `app_launch`, `browsing`, `app_focus` -- pages each type separately (types without a key start from the newest event). |

The query covers `[now - window, now + 60 s]` (the slack admits slightly future
timestamps); the response reports `window_start` = now - window and `window_end` = now. The
window is re-evaluated on every call, so pages fetched later see a slightly later window;
to page through a fixed range, use [`QueryTimeline`](#querytimeline).

| `window` | Seconds |
|---|---|
| `15m` | 900 |
| `30m` | 1,800 |
| `1h` | 3,600 |
| `2h` | 7,200 |
| `6h` | 21,600 |
| `24h` | 86,400 |
| `7d` | 604,800 |
| `15d` | 1,296,000 |
| `30d` | 2,592,000 |

**Examples:**

```json
{"window":"6h"}
{"seconds":300,"types":["app_launch","browsing"]}
{"window":"24h","types":["file"],"limit":200,"min_confidence":0.5}
{"window":"24h","types":["file"],"limit":200,"before_id":1041}
{"window":"1h","before_id":{"file":1041,"app_focus":312}}
{"types":["app_focus"]}
{}
```

**Response** (`{"window":"1h","limit":2}`, abbreviated):

```json
{
  "api_version": 2,
  "window_start": 1760000400,
  "window_end": 1760004000,
  "file_activities": {
    "count": 2,
    "truncated": true,
    "events": [
      {
        "id": 1042,
        "timestamp": 1760003990,
        "action": "MODIFY",
        "path": "C:\\Users\\Alice\\Documents\\report.docx",
        "source": "etw",
        "confidence": 1.0,
        "entity_key": "c:\\users\\alice\\documents\\report.docx"
      },
      {
        "id": 1041,
        "timestamp": 1760003900,
        "action": "RENAME",
        "path": "C:\\Users\\Alice\\Documents\\report.docx",
        "old_path": "C:\\Users\\Alice\\Documents\\draft.docx",
        "source": "etw",
        "confidence": 0.9,
        "entity_key": "c:\\users\\alice\\documents\\report.docx"
      }
    ],
    "next_before_id": 1041
  },
  "app_launch_activities": { "count": 0, "truncated": false, "events": [] },
  "browsing_activities":   { "count": 0, "truncated": false, "events": [] },
  "app_focus_activities":  { "count": 0, "truncated": false, "events": [] }
}
```

- A section appears only for requested types, in the order file, app launch, browsing,
  app focus. There is no `ok` member (v1 shape).
- Events are newest first (by `timestamp`, then `id`; app-focus sessions by their end).
  App-focus sessions are included when they overlap the window.
- `count` is the number of events returned. `truncated` is `true` when more than `limit`
  events matched; `next_before_id` (present only then) is the `id` of the oldest event
  returned. Repeat the request with `"before_id": next_before_id` (or an object collecting
  the `next_before_id` of several types) for the next page. A `before_id` whose row has
  meanwhile been deleted (retention, purge) returns an empty page; start again without it.
- `id` sequences are independent per event type.

#### Raw Event Fields

**File** (`file_activities.events[]`):

| Field | Type | Presence | Description |
|---|---|---|---|
| `id` | integer | always | Row id. |
| `timestamp` | integer | always | Unix seconds (UTC). |
| `action` | string | always | See [File Action Types](#file-action-types). |
| `path` | string | always | Full path, original casing. |
| `old_path` | string | renames | Previous path. |
| `source` | string | always | Producer: `etw`, `rdcw`, `shell`, `usn`, `recent` or `mru`. |
| `confidence` | number | always | 0-1: how likely the event reflects the user's own action rather than background activity. |
| `entity_key` | string | always | File [entity key](#entity-keys-and-normalization) (empty if none). |

**App launch** (`app_launch_activities.events[]`):

| Field | Type | Presence | Description |
|---|---|---|---|
| `id` | integer | always | Row id. |
| `timestamp` | integer | always | Launch time. |
| `exe_name` | string | always | Executable file name, e.g. `notepad.exe`. |
| `exe_path` | string | always | Full path of the executable. |
| `pid` | integer | always | Process id. |
| `confidence` | number | always | 0-1. |
| `creator_exe` | string | when known | Executable that started the process. |
| `aumid` | string | packaged apps | Application User Model ID. |

**Browsing** (`browsing_activities.events[]`):

| Field | Type | Presence | Description |
|---|---|---|---|
| `id` | integer | always | Row id. |
| `timestamp` | integer | always | When the page was shown. |
| `browser` | string | always | Friendly browser name, e.g. `Microsoft Edge`, `Google Chrome`, `Firefox`. |
| `title` | string | always | Page title. |
| `url` | string | when known | Canonical, privacy-filtered URL. |
| `host` | string | when known | Host name. |
| `confidence` | number | always | 0-1. |
| `entity_key` | string | always | Page / document entity key. |

**App focus** (`app_focus_activities.events[]`):

| Field | Type | Presence | Description |
|---|---|---|---|
| `id` | integer | always | Row id. |
| `timestamp` | integer | always | Session end (v1 compatibility). |
| `exe_name` / `exe_path` | string | always | Foreground application. |
| `window_title` | string | always | Window title. |
| `duration_secs` | integer | always | Wall-clock length of the session. |
| `start_ts` / `end_ts` | integer | always | Session start / end. |
| `active_secs` | integer or null | always | Interaction-active foreground seconds; excludes passive presence. `null` means unknown, including an old positive active/passive total that cannot be separated. Zero is measured zero. |
| `passive_secs` / `idle_secs` | integer or null | always | Separately measured passive-presence / old-input time; unavailable legacy components are null. Passive media does not prove attention to this document. |
| `unknown_attention_secs` | integer | always | Unclassified foreground-exposure remainder. |
| `attention_status` | string | always | `measured`, `unknown` or `legacy_combined`. |
| `aumid` | string | always | Packaged-app identity, empty if unavailable. |
| `doc_resolution` | string | always | `browser_window`, `filename_candidate`, `ambiguous`, `unresolved`, `withheld` or migrated `legacy_candidate`. |
| `doc_key` | string | when resolved | Entity key of the document or page shown. |
| `doc_display` | string | with `doc_key` | Path or URL of that document / page. |
| `doc_type` | string | with `doc_key` | `file`, `url` or `webdoc`. |
| `confidence` | number | always | 0-1. |

#### File Action Types

| Action | Meaning | `old_path` |
|---|---|---|
| `OPEN` | A file was opened. | no |
| `CREATE` | A file was created. | no |
| `MODIFY` | A file was written / saved. | no |
| `DELETE` | A file was deleted. | no |
| `RENAME` | A file was renamed or moved. | **yes** |
| `DOWNLOAD` | A file was downloaded from the web. | no |

### 2. Inference Lookups (v1-compatible)

These v1 operations keep their v1 request and response shapes (no `ok` member on
success). New code should prefer [`ScoreEntities`](#scoreentities) (adds scores, names and
reasons) and [`GetChanges`](#getchanges) (full records with names).

#### QueryInferences

Looks up the entity records of the identifiers you pass. No scope required.

| Param | Type | Default | Notes |
|---|---|---|---|
| `paths` (alias `keys`) | string[] | required | File paths, exe paths, URLs or AUMIDs, [normalized](#entity-keys-and-normalization) by WARP. Not an array -> `invalid_request`; more than 5000 -> `too_many_keys`; non-string elements are skipped. `paths` wins when both are present. |
| `fields` | string[] | all | Projection. Names: `entity_type`, `last_event_ts`, `last_open_ts`, `last_edit_ts`, `open_count_7d`, `open_count_30d`, `open_count_total`, `recency_score`, `version`, `updated_at`, `entity_key`, `score`, `last_active_ts`, `active_secs_7d`. |

```json
{
  "op": "QueryInferences",
  "paths": [
    "C:\\Users\\Alice\\Documents\\report.docx",
    "c:\\users\\alice\\documents\\REPORT.DOCX",
    "https://example.com/never-visited"
  ],
  "fields": ["entity_type", "last_open_ts", "open_count_7d", "recency_score", "version", "score"]
}
```

**Response:**

```json
{
  "api_version": 2,
  "now": 1760004000,
  "results": {
    "C:\\Users\\Alice\\Documents\\report.docx": {
      "entity_type": "file",
      "last_open_ts": 1760003990,
      "open_count_7d": 12,
      "recency_score": 212.8,
      "version": 58231,
      "score": 0.97
    },
    "c:\\users\\alice\\documents\\REPORT.DOCX": {
      "entity_type": "file",
      "last_open_ts": 1760003990,
      "open_count_7d": 12,
      "recency_score": 212.8,
      "version": 58231,
      "score": 0.97
    },
    "https://example.com/never-visited": {}
  }
}
```

- `results` is keyed by the **exact strings you sent**; different spellings of the same
  entity each get their own member. Duplicate input strings produce duplicate member names.
- An empty object means WARP has no record for that identifier (never seen, deleted,
  excluded or not normalizable).
- Fields are written in the order of the list above, whatever the order of `fields`.
  Counts are confidence-weighted and rounded; `score` is the blend-horizon percentile (see
  [Entity Records and Scores](#entity-records-and-scores)); `version` equals the entity's
  `change_seq`; `updated_at` is when the record was last written.

#### GetInferenceDeltas

The v1 change feed (activity scope): the same data and semantics as
[`GetChanges`](#getchanges) with deletes always included, in v1 field names.

| Param | Type | Default | Notes |
|---|---|---|---|
| `since_version` | integer | 0 | Cursor; 0 = everything. Negative values count as 0. |
| `limit` | integer | 5000 | Changes per call (`deltas` + `deleted`), clamped to 1-5000. |

```json
{"op": "GetInferenceDeltas", "since_version": 58000}
```

**Response:**

```json
{
  "api_version": 2,
  "now": 1760004000,
  "deltas": [
    {
      "entity_key": "c:\\users\\alice\\documents\\report.docx",
      "entity_type": "file",
      "last_event_ts": 1760003990,
      "last_open_ts": 1760003990,
      "last_edit_ts": 1760003990,
      "open_count_7d": 12,
      "open_count_30d": 45,
      "open_count_total": 61,
      "recency_score": 212.8,
      "version": 58231,
      "updated_at": 1760003991
    }
  ],
  "deleted": [
    { "entity_key": "c:\\users\\alice\\documents\\old.docx", "entity_type": "file", "version": 58102, "reason": "deleted" }
  ],
  "next_version": 58240,
  "current_version": 58240,
  "more": false,
  "reset": false
}
```

- `version` is the global change sequence (`change_seq`), not a per-entity counter: it is
  monotonic across all entities, so values jump.
- Pass `next_version` back as `since_version` instead of computing it from the records,
  and call again immediately while `more` is `true`.
- `reset: true` (with empty arrays and `next_version: 0`): the cursor predates a clear-all
  or deletes that are no longer retained -- drop your cache and restart from 0.
- `deleted[].reason`: `deleted`, `renamed`, `purge` or `retention`.

### 3. Entities

Entity operations read the per-entity engagement model. Results carry the
[entity record fields](#entity-record-fields); scoring operations add the
[score fields](#score-fields).

#### ScoreEntities

Scores the identifiers you pass -- e.g. to rank search results or a file picker. No scope
required (without the activity scope, `display_name` and `title` are omitted).

| Param | Type | Default | Notes |
|---|---|---|---|
| `keys` (alias `paths`) | string[] | required | File paths, exe paths, URLs or AUMIDs ([normalized](#entity-keys-and-normalization)). Not an array -> `invalid_request`; more than 5000 -> `too_many_keys`. Non-string elements produce `found: false` with an empty `key`. `keys` wins when both are present. |
| `horizon` | string | `blend` | `short` (days), `long` (weeks) or `blend`; any other value -> `invalid_request` (`'horizon' must be short, long or blend`). |
| `reasons` | bool | `true` | Include `reasons[]`. |
| `context` | string | -- | `"current"` adds `context_boost` to every result and `current_thread` to the response (only while the history engine runs). |
| `identity_version` | integer | `2` | `1` explicitly resolves old folded-key aliases; ambiguous legacy identities are not found. Also supported by `QueryInferences`. |

```json
{
  "op": "ScoreEntities",
  "keys": ["C:\\Users\\Alice\\Documents\\report.docx", "https://example.com/never-visited"],
  "horizon": "short",
  "context": "current"
}
```

**Response:**

```json
{
  "api_version": 2,
  "ok": true,
  "now": 1760004000,
  "horizon": "short",
  "current_thread": 42,
  "results": [
    {
      "key": "C:\\Users\\Alice\\Documents\\report.docx",
      "entity_key": "c:\\users\\alice\\documents\\report.docx",
      "found": true,
      "entity_type": "file",
      "display_name": "C:\\Users\\Alice\\Documents\\report.docx",
      "last_event_ts": 1760003990,
      "last_open_ts": 1760003990,
      "last_edit_ts": 1760003990,
      "last_active_ts": 1760003950,
      "open_count_7d": 12,
      "open_count_30d": 45,
      "open_count_total": 61,
      "edit_count_total": 30,
      "active_secs_7d": 5400,
      "active_secs_total": 40210,
      "distinct_days": 14,
      "recency_score": 212.8,
      "change_seq": 58231,
      "score": 0.97,
      "prior": 31.2,
      "short_term": 31.2,
      "long_term": 118.6,
      "reasons": ["edited today", "90 min active in the last 7 days", "used on 5 of the last 7 days",
                  "recurring: used on 14 different days", "last used just now"],
      "context_boost": 0.92
    },
    {
      "key": "https://example.com/never-visited",
      "entity_key": "https://example.com/never-visited",
      "found": false,
      "context_boost": 0.0
    }
  ]
}
```

- `results` are in request order; `key` is your string, `entity_key` the normalized key.
- Results with `found: false` carry only `key`, `entity_key`, `found` (and `context_boost`).
- `context_boost` (0-1) says how close the entity is to the current work thread: 0.5-1.0
  for members of the thread (by member weight), below 0.5 for entities that co-occurred with
  its members, 0 otherwise or when there is no current thread. A simple ranking is
  `score + w * context_boost`.

#### TopEntities

The most engaged entities (activity scope).

| Param | Type | Default | Notes |
|---|---|---|---|
| `type` | string | all types | `file`, `app` or `url`; any other value -> `invalid_request`. |
| `limit` | integer | 50 | Clamped to 1-1000. |
| `horizon` | string | `blend` | `short`, `long` or `blend` (see [How Scores Work](#how-scores-work)); any other value -> `invalid_request`. |

```json
{"op": "TopEntities", "type": "file", "limit": 3, "horizon": "short"}
```

**Response** (abbreviated):

```json
{
  "api_version": 2,
  "ok": true,
  "now": 1760004000,
  "horizon": "short",
  "results": [
    {
      "entity_key": "c:\\users\\alice\\documents\\report.docx",
      "entity_type": "file",
      "display_name": "C:\\Users\\Alice\\Documents\\report.docx",
      "last_event_ts": 1760003990,
      /* ... remaining entity record fields ... */
      "score": 0.97,
      "prior": 31.2,
      "short_term": 31.2,
      "long_term": 118.6,
      "reasons": ["edited today", "90 min active in the last 7 days", "last used just now"]
    }
  ]
}
```

- With a `type`, results are ordered by `prior` (decayed engagement), then by
  `last_event_ts`. Without one, the three types are merged by `score` (percentiles are
  comparable across types), then by `prior`.
- `reasons` are always included.

#### SearchActivity

Finds entities by name or title (activity scope).

| Param | Type | Default | Notes |
|---|---|---|---|
| `query` | string | required | Missing or empty -> `invalid_request`; at most 512 bytes. Split into word tokens (letters, digits and non-ASCII characters; case-insensitive; at most 8). Every token must match the display name or title; the last token also matches as a prefix. |
| `type` | string | all types | `file`, `app` or `url`; any other value -> `invalid_request`. |
| `limit` | integer | 25 | Clamped to 1-200. |

```json
{"op": "SearchActivity", "query": "quarterly rep", "type": "file", "limit": 5}
```

**Response** -- like `TopEntities` without `horizon`, plus `relevance`:

```json
{
  "api_version": 2,
  "ok": true,
  "now": 1760004000,
  "results": [
    {
      "entity_key": "c:\\users\\alice\\documents\\quarterly-report.txt",
      "entity_type": "file",
      "display_name": "C:\\Users\\Alice\\Documents\\quarterly-report.txt",
      /* ... remaining entity record fields ... */
      "relevance": 1.0,
      "score": 0.81,
      "prior": 4.34,
      "short_term": 5.9,
      "long_term": 7.1,
      "reasons": ["edited today", "last used 3 h ago"]
    }
  ]
}
```

- Matching uses the SQLite FTS5 index (best match first) or, when the SQLite build lacks
  FTS5, a substring match on the first token.
- `relevance` (0-1] is rank-based (1 for the best text match). Results are ordered by
  `0.6 * relevance + 0.4 * score`, where `score` uses the `blend` horizon.
- A query without any letter or digit returns no results.

#### GetChanges

The entity change feed (activity scope): every entity created or updated, and every
entity deleted, since a cursor. Use it to keep a local index in sync.

| Param | Type | Default | Notes |
|---|---|---|---|
| `since` (alias `since_seq`) | integer | 0 | Cursor from the previous `next`; 0 = full sync. Negative values count as 0. |
| `limit` | integer | 1000 | Changes per call (upserts + deletes), clamped to 1-5000. |
| `include_deletes` | bool | `true` | `false` skips tombstones (and the tombstone-retention part of the reset check). |

```json
{"op": "GetChanges", "since": 58000, "limit": 1000}
```

**Response:**

```json
{
  "api_version": 2,
  "ok": true,
  "now": 1760004000,
  "reset": false,
  "next": 58240,
  "current": 58240,
  "more": false,
  "upserts": [
    {
      "entity_key": "c:\\users\\alice\\documents\\report.docx",
      "entity_type": "file",
      "display_name": "C:\\Users\\Alice\\Documents\\report.docx",
      "last_event_ts": 1760003990,
      /* ... remaining entity record fields ... */
      "change_seq": 58231
    }
  ],
  "deletes": [
    {
      "entity_key": "c:\\users\\alice\\documents\\old.docx",
      "entity_type": "file",
      "change_seq": 58102,
      "reason": "deleted",
      "ts": 1760001200
    }
  ]
}
```

| Field | Description |
|---|---|
| `upserts[]` | Full current [entity records](#entity-record-fields) (no score fields) of entities created or changed after `since`. Replace your copy. |
| `deletes[]` | Tombstones: `entity_key`, `entity_type`, `change_seq`, `reason` and `ts` (Unix seconds). `reason` is `deleted`, `renamed` (the entity continues under a new key, which arrives as an upsert), `purge` or `retention`. |
| `next` | Cursor for the next call. When `more` is `false` it equals `current`. |
| `current` | Highest change sequence handed out so far: the latest change in the store, or the persisted high-water mark (`meta` key `change_seq_hw`, written with every committed batch) if that is higher. |
| `more` | `true` when more changes are immediately available: call again with `since` = `next`. |
| `reset` | `true` when your cursor is no longer valid: it predates a clear-all, or deletes after it were pruned (tombstones are kept for 90 days). `next` is then 0 and both arrays are empty: drop your local copy and resync from `since: 0`. |

Changes come in ascending `change_seq` order, at most `limit` upserts and deletes
combined. An entity deleted and re-created after `since` appears in both arrays: apply
`deletes` before `upserts` (or merge by `change_seq`). The change sequence is shared with
other derived data (e.g. work threads), and numbers consumed by writes that were rolled back
never appear in any record; `next` still moves past them, so `next` can advance without any
upsert and a caught-up cursor equals the `current` of `WaitForChanges`.

**Sync loop:**

```text
since = load()                                  # 0 on the first run
loop:
    r = GetChanges(since, limit = 1000)
    if r.reset: clear the local copy; since = 0; continue
    remove r.deletes by entity_key; then upsert r.upserts by entity_key
    since = r.next; save(since)
    if not r.more: WaitForChanges(since)         # or sleep, then loop
```

#### WaitForChanges

Long-poll: returns as soon as the store's change sequence exceeds `since`, or after the
timeout. No scope required (it only returns sequence numbers).

| Param | Type | Default | Notes |
|---|---|---|---|
| `since` (alias `since_seq`) | integer | 0 | Usually the `next` of your last `GetChanges`. |
| `timeout_ms` | integer | 20000 | Clamped to 0-25000. |

```json
{"op": "WaitForChanges", "since": 58240, "timeout_ms": 20000}
```

**Response:**

```json
{"api_version": 2, "ok": true, "changed": true, "current": 58251, "reset_seq": 0}
```

| Field | Description |
|---|---|
| `changed` | `true` when `current` > `since`: call `GetChanges`. |
| `current` | Latest committed change sequence. |
| `reset_seq` | Change sequence of the last clear-all (0 if none). If it is greater than your cursor, the next `GetChanges` returns `reset`. |
| `retry_after_ms` | Present (1000) when 4 long-polls were already pending server-wide: the call returned at once (`changed` reflects the current state); wait this long before polling again. |

- The wait also ends when the store closes (WARP shutting down).
- Use a client read timeout longer than `timeout_ms`. The 10 s idle limit applies only
  between requests, not while a long-poll is pending.
- The sequence also advances for changes that `GetChanges` does not return (e.g. work
  threads), so `changed: true` can be followed by an empty `GetChanges`. Pass the `next`
  of that `GetChanges` (not the `change_seq` of the last record) to the following
  `WaitForChanges`; it then blocks until something new is committed.

### 4. Timeline and History

#### QueryTimeline

Raw events, work sessions and work threads of an absolute time range, with per-type
cursors (activity scope).

| Param | Type | Default | Notes |
|---|---|---|---|
| `start` | integer | `end` - 3600 | Unix seconds. |
| `end` | integer | now | Unix seconds. `start` or `end` <= 0, or `end` < `start` -> `invalid_request`. |
| `types` | string[] | everything | Any of `file`, `app_launch`, `app_focus`, `browsing`, `sessions`, `threads`. When present, only the listed sections are returned. Unknown names are ignored; an empty list, or one with only unknown names, means everything (including `sessions` and `threads`), as for the raw query. |
| `limit` | integer | 1000 | Events per event type, clamped to 1-20000. |
| `min_confidence` | number | 0.0 | Drops events whose `confidence` is lower. |
| `cursor` | object | -- | `next_cursor` of the previous page: `{"file": id, "app_launch": id, "app_focus": id, "browsing": id}`. |

```json
{"op": "QueryTimeline", "start": 1760000400, "end": 1760004000, "types": ["file", "app_focus", "sessions"], "limit": 500}
```

**Response** (abbreviated):

```json
{
  "api_version": 2,
  "ok": true,
  "start": 1760000400,
  "end": 1760004000,
  "files": [
    { "id": 1042, "timestamp": 1760003990, "action": "MODIFY", "path": "C:\\Users\\Alice\\Documents\\report.docx",
      "entity_key": "c:\\users\\alice\\documents\\report.docx", "source": "etw", "confidence": 1.0 }
  ],
  "focus": [
    { "id": 881, "start_ts": 1760003100, "end_ts": 1760003950, "active_secs": 790, "exe_name": "WINWORD.EXE",
      "window_title": "report.docx - Word", "doc_key": "c:\\users\\alice\\documents\\report.docx",
      "doc_display": "C:\\Users\\Alice\\Documents\\report.docx", "doc_type": "file" }
  ],
  "sessions": [
    { "id": 77, "start_ts": 1759998000, "end_ts": 1760004000, "active_secs": 4810, "passive_secs": 600 }
  ],
  "next_cursor": { "file": 1003 }
}
```

| Section | Present when | Fields |
|---|---|---|
| `files` | `file` requested | `id`, `timestamp`, `action`, `path`, `old_path` (renames), `entity_key`, `source`, `confidence` |
| `app_launches` | `app_launch` requested | `id`, `timestamp`, `exe_name`, `exe_path`, `aumid` (empty if none), `confidence` |
| `focus` | `app_focus` requested | `id`, `start_ts`, `end_ts`, nullable `active_secs` / `passive_secs` / `idle_secs`, `unknown_attention_secs`, `attention_status`, `exe_name`, `exe_path`, `aumid`, `window_title`, `doc_resolution`, plus `doc_key` / `doc_display` / `doc_type` when resolved; whole sessions overlapping the range |
| `browsing` | `browsing` requested | `id`, `timestamp`, `browser`, `title`, `url` (empty if none), `entity_key` |
| `sessions` | `sessions` requested and the history engine runs | whole work sessions overlapping the range, newest first, at most 500: `id`, `start_ts`, `end_ts`, `active_secs`, `passive_secs`, `accounting_version` (`1`: durable interval union; `0`: contains a bounded legacy estimate). Overlapping focus/presence do not add duplicate time. |
| `threads` | `threads` requested and the history engine runs | work threads active in the range, most recent first, at most 50: `id`, `label`, `active_secs_in_range`, `spans` (`[start, end]` pairs) |
| `next_cursor` | always | `null`, or an object naming each event type that has more rows |

**Paging.** Event types page independently, newest first: by (`timestamp`, `id`), and
focus sessions by (session end, `id`) -- stable keyset paging, so rows recorded while you
page do not shift the pages. When a type has more rows than `limit`, `next_cursor` holds
the `id` of its last (oldest) returned row. Repeat the request with the same `start` /
`end`, `"cursor": next_cursor` and `types` reduced to the keys of `next_cursor`: a type
requested without a cursor starts again from its newest row, and `sessions` / `threads`
are not paged (they come complete with the first page). Stop when `next_cursor` is `null`.
A cursor whose row has meanwhile been deleted (retention, purge) returns an empty page for
that type.

#### GetWorkThreads

Work threads -- groups of entities used together, resumable across days -- that were
active in a time range (activity scope).

| Param | Type | Default | Notes |
|---|---|---|---|
| `start` | integer | `end` - 7 days | Unix seconds. |
| `end` | integer | now | `end` < `start` -> `invalid_request`. |
| `limit` | integer | 20 | Clamped to 1-200. |
| `members` | bool | `true` | Include the top 15 members of each thread. |

```json
{"op": "GetWorkThreads", "limit": 5}
```

**Response:**

```json
{
  "api_version": 2,
  "ok": true,
  "start": 1759399200,
  "end": 1760004000,
  "current_thread": 42,
  "threads": [
    {
      "id": 42,
      "label": "User is working on the quarterly report in Word and Excel",
      "first_ts": 1759740000,
      "last_ts": 1760003990,
      "active_secs": 21840,
      "active_secs_in_range": 9420,
      "project": "od:c:\\users\\alice\\onedrive - contoso\\finance",
      "current": true,
      "change_seq": 58233,
      "spans": [[1759912000, 1759919800], [1760001200, 1760003990]],
      "members": [
        { "entity_key": "c:\\users\\alice\\onedrive - contoso\\finance\\q3.xlsx", "entity_type": "file",
          "display_name": "C:\\Users\\Alice\\OneDrive - Contoso\\Finance\\Q3.xlsx", "weight": 3.41 },
        { "entity_key": "c:\\users\\alice\\documents\\report.docx", "entity_type": "file",
          "display_name": "C:\\Users\\Alice\\Documents\\report.docx", "weight": 2.87 }
      ]
    }
  ]
}
```

| Field | Description |
|---|---|
| `current_thread` | Id of the thread the latest context snapshot was assigned to (0 if none). |
| `threads[]` | Most recently active first. Threads without a span inside the range are skipped. |
| `label` | Human-readable label taken from the context summary. |
| `first_ts` / `last_ts` | First and last activity of the thread (all time). |
| `active_secs` / `active_secs_in_range` | Active seconds over the thread's lifetime / in its spans that overlap the range. |
| `project` | Project anchor: `git:<repo folder>`, `ws:<solution or workspace folder>`, `od:<OneDrive library folder>`, `sp:<host>/sites/<site>`, `repo:<host>/<owner>/<repo>` or `dir:<folder>`; empty if none. |
| `current` | `true` for the current thread. |
| `change_seq` | Change sequence of the thread's last update. |
| `spans` | `[start, end]` periods of the thread that overlap the range (at most 200). |
| `members[]` | `entity_key`, `entity_type`, `display_name`, `title` (when known) and `weight` (membership weight that decays with a ~2-day time constant). Deleted entities are omitted. |

#### GetRelated

Entities used together with an entity -- co-occurrence in the same context snapshots
(activity scope).

| Param | Type | Default | Notes |
|---|---|---|---|
| `entity_key` | string | -- | An exact entity key, used verbatim. |
| `key` | string | -- | A raw identifier, [normalized](#entity-keys-and-normalization); used when `entity_key` is absent or empty. One of the two is required (`invalid_request`). |
| `limit` | integer | 20 | Clamped to 1-100. |

```json
{"op": "GetRelated", "key": "C:\\Users\\Alice\\Documents\\report.docx", "limit": 5}
```

**Response:**

```json
{
  "api_version": 2,
  "ok": true,
  "entity_key": "c:\\users\\alice\\documents\\report.docx",
  "results": [
    { "entity_key": "c:\\users\\alice\\onedrive - contoso\\finance\\q3.xlsx", "entity_type": "file",
      "display_name": "C:\\Users\\Alice\\OneDrive - Contoso\\Finance\\Q3.xlsx", "weight": 6.12, "last_ts": 1760003990 },
    { "entity_key": "gh:contoso/finance-tools#pr12", "entity_type": "url",
      "display_name": "https://github.com/contoso/finance-tools/pull/12", "weight": 1.73, "last_ts": 1759919800 }
  ]
}
```

`weight` is the co-occurrence weight decayed to now (14-day time constant); results are
strongest first; `last_ts` is the last time both were seen together. Deleted entities are
omitted.

### 5. Context

WARP composes a context snapshot every `ContextCadenceSeconds` (default 60 s; only when new
activity was committed or the foreground window changed, and at least every 5 minutes) from
the last 15 minutes of activity: a 1-3 line summary of what the user is doing, per-facet
summaries for files, websites and apps, the top apps and the entities behind it. Summaries
are built from the actual window, tab and document titles (there is no fixed topic
taxonomy). With the optional granite embedding model related activities are clustered into
threads, and the optional Qwen LLM refiner (`LlmEnabled`, off by default) may rephrase the
combined summary. All context operations need the activity scope and return `unavailable`
when context inference is not running.

#### GetRecentContext

The latest snapshot for a look-back window (v1 shape: no `ok` member).

| Param | Type | Default | Notes |
|---|---|---|---|
| `category` | string | `all` | `all`, `files`, `websites` or `apps` (`documents` = `files`; any other value = `all`). Selects which summary becomes `summary` and whether the per-facet summaries are included (see [Context Snapshot Fields](#context-snapshot-fields)). |
| `window_seconds` (alias `window_secs`) | integer | 900 | Look-back. Clamped to 0-2592000, then snapped to the nearest of 300, 900, 1800, 3600, 7200, 21600, 86400, 604800, 1296000, 2592000 (0 -> 900). |
| `include_vector` | bool | `true` | Include `summary_embedding`. |

```json
{"op": "GetRecentContext", "category": "all", "window_seconds": 900, "include_vector": false}
```

**Response:**

```json
{
  "api_version": 2,
  "recent_context": { /* snapshot, see Context Snapshot Fields */ },
  "category": "all",
  "window_seconds": 900,
  "history_count": 37
}
```

- A 900-second window returns the snapshot of the last periodic compose (the only one the
  LLM may have refined). Any other window is composed on demand without the LLM and cached
  for 60 s (windows up to 2 h), 5 minutes (up to 24 h) or 15 minutes (longer).
- `recent_context` is `null` until the first compose has finished.
- `category` and `window_seconds` echo the normalized values; `history_count` is the
  number of snapshots in the in-memory history.

#### GetRecentContexts

Recent snapshots, newest first -- for context drift, a "what was I doing" timeline or an
agent's short-term memory (v1 shape: no `ok` member).

| Param | Type | Default | Notes |
|---|---|---|---|
| `count` | integer | 10 | Values <= 0 mean 10; at most 200. |
| `category` | string | `all` | As for `GetRecentContext`, applied to every snapshot. |
| `window_seconds` (alias `window_secs`) | integer | 900 | Only snapshots taken within the last `window_seconds` (same clamping and snapping as above). |
| `include_vector` | bool | `true` | Include `summary_embedding` (in-memory snapshots only). |

```json
{"op": "GetRecentContexts", "count": 20, "window_seconds": 86400, "include_vector": false}
```

**Response:**

```json
{
  "api_version": 2,
  "recent_contexts": [ { /* newest snapshot */ }, { /* older snapshot */ } ],
  "category": "all",
  "window_seconds": 86400,
  "returned": 2,
  "from_persisted": 0,
  "history_count": 37,
  "requested": 20
}
```

| Field | Description |
|---|---|
| `recent_contexts` | Snapshots, newest first. |
| `category` / `window_seconds` | The normalized request values. |
| `returned` | Number of snapshots returned. |
| `from_persisted` | How many of them were read from the database (see below). |
| `history_count` | Snapshots in the in-memory history (at most 1440). |
| `requested` | The effective `count`. |

- A snapshot enters the history only when its window had activity and it differs
  materially from the previous entry (a summary line or the dominant app changed) or the
  previous entry is 5 minutes old -- entries are context *transitions*, not polling ticks.
- History entries are also persisted (table `context_snapshot`, kept for the raw-event
  retention). Every category (`all`, `files`, `websites`, `apps`, and legacy
  `documents`) fills older entries from the database and applies the same summary
  projection as memory. `include_vector: false` always omits `summary_embedding`;
  when true, unavailable persisted vectors are `[]`, not a different label embedding.
  Persisted entries carry their real work-thread id. Old snapshots retain their
  historical estimates and are marked `attention_semantics: "legacy_combined"`.
- This lists previously generated snapshots. It is not reconstruction of a new
  arbitrary historical interval; `window_seconds` filters snapshot generation times.

#### GetCurrentContext

Presence, the live foreground window, the latest snapshot and the current work thread in
one call.

| Param | Type | Default | Notes |
|---|---|---|---|
| `include_vector` | bool | **`false`** | Include `summary_embedding` in the snapshot. |

```json
{"op": "GetCurrentContext"}
```

**Response:**

```json
{
  "api_version": 2,
  "ok": true,
  "context": {
    "now": 1760004000,
    "presence": "active",
    "foreground": {
      "exe_name": "WINWORD.EXE",
      "exe_path": "C:\\Program Files\\Microsoft Office\\root\\Office16\\WINWORD.EXE",
      "window_title": "report.docx - Word",
      "since": 1760003100,
      "active_secs": 790,
      "entity_key": "c:\\users\\alice\\documents\\report.docx",
      "entity_type": "file",
      "display": "C:\\Users\\Alice\\Documents\\report.docx"
    },
    "snapshot": { /* latest periodic snapshot, category "all", see Context Snapshot Fields */ }
  },
  "thread": { "id": 42, "label": "User is working on the quarterly report in Word and Excel" }
}
```

| Field | Description |
|---|---|
| `context.now` | Server time. |
| `context.presence` | `active`, `passive`, `away`, `locked` or `asleep` (empty if unknown). |
| `context.foreground` | The current foreground session, or `null`: `exe_name`, `exe_path`, `aumid`, `window_title`, `since` (session start), `active_secs`, `passive_secs`, `idle_secs`, `unknown_attention_secs` so far, and `resolution`; plus `entity_key`, `entity_type` (`file` or `url`) and `display` when resolved. |
| `context.snapshot` | The latest periodic snapshot (`null` before the first compose). |
| `thread` | Current work thread: `id` (0 if none) and `label`. Present while the history engine runs. |

#### Context Snapshot Fields

The snapshot object returned by `GetRecentContext` (`recent_context`), `GetRecentContexts`
(`recent_contexts[]`) and `GetCurrentContext` (`context.snapshot`). Example for
`category: "all"` with `include_vector: false`:

```json
{
  "timestamp": 1760003960,
  "window_start": 1760003060,
  "window_end": 1760003960,
  "window_seconds": 900,
  "category": "all",
  "summary": ["User is working on the quarterly report and Q3 budget in Word and Excel",
              "Reviewing finance-tools pull request 12 in Microsoft Edge"],
  "summary_files": ["User is working on the quarterly report and Q3 budget in Word and Excel"],
  "summary_websites": ["Reviewing finance-tools pull request 12 in Microsoft Edge"],
  "summary_apps": [],
  "activity_count": 47,
  "focus_seconds": 812,
  "foreground_exposure_seconds": 900,
  "passive_seconds": 40,
  "idle_seconds": 48,
  "unknown_attention_seconds": 0,
  "attention_semantics": "interaction_active_v1",
  "dominant_focus_pct": 61,
  "confidence": 0.84,
  "thread_count": 2,
  "thread_id": 42,
  "presence": "active",
  "model": "granite-embedding-small-english-r2",
  "model_polish": "(not loaded)",
  "signal_types": ["app_focus", "file", "browsing"],
  "items": [
    { "app": "Word", "exe": "WINWORD.EXE", "title": "report.docx", "raw_title": "report.docx - Word",
      "focus_seconds": 498, "pct": 61, "thread_id": 1, "entity_key": "c:\\users\\alice\\documents\\report.docx" },
    { "app": "Microsoft Edge", "exe": "msedge.exe", "title": "Add Q3 export - Pull Request #12",
      "raw_title": "Add Q3 export - Pull Request #12 - contoso/finance-tools - Microsoft Edge",
      "focus_seconds": 184, "pct": 22, "thread_id": 2, "entity_key": "gh:contoso/finance-tools#pr12" }
  ],
  "current_entities": [
    { "entity_key": "c:\\users\\alice\\documents\\report.docx", "entity_type": "file",
      "display": "C:\\Users\\Alice\\Documents\\report.docx", "title": "report.docx", "app": "Word",
      "role": "focus", "active_secs_in_window": 498, "last_active_ts": 1760003950, "score": 0.97, "thread_id": 42 },
    { "entity_key": "gh:contoso/finance-tools#pr12", "entity_type": "url",
      "display": "https://github.com/contoso/finance-tools/pull/12", "title": "Add Q3 export - Pull Request #12",
      "app": "Microsoft Edge", "role": "reference", "active_secs_in_window": 184, "last_active_ts": 1760003600,
      "score": 0.64, "thread_id": 42 }
  ]
}
```

With `category` `files`, `websites` or `apps`, `summary` holds that facet's summary and the
`summary_files` / `summary_websites` / `summary_apps` members are omitted; everything else
(including `items`, which always reflect the combined view) is unchanged.

| Field | Type | Description |
|---|---|---|
| `timestamp` | integer | When the snapshot was composed (Unix seconds). |
| `window_start` / `window_end` / `window_seconds` | integer | The look-back window. |
| `category` | string | The view the snapshot was rendered for. |
| `summary` | string[] | 1-3 lines describing what the user is doing -- the combined summary for `all`, otherwise the requested facet's summary. Empty when the window had no usable activity. Render one line per entry. |
| `summary_files` | string[] | Only for `all`. Apps engaged with a file -- editors and IDEs, Office, note-taking apps, PDF and image viewers, design tools, any window whose title names a file -- plus recently opened files. |
| `summary_websites` | string[] | Only for `all`. Browser tabs. |
| `summary_apps` | string[] | Only for `all`. Everything else: communication, media, terminals, remote desktops, version-control UIs, ... |
| `activity_count` | integer | Raw signals considered. |
| `focus_seconds` | integer | Measured interaction-active foreground seconds in the window; zero is never expanded to wall duration. |
| `foreground_exposure_seconds` | integer | Foreground wall-time union, including passive/idle/unknown exposure. |
| `passive_seconds` / `idle_seconds` / `unknown_attention_seconds` | integer | Separate clipped attention components; background media is not promoted to active document time. Mixed long segments use proportional estimates. |
| `attention_semantics` | string | `interaction_active_v1` for new compositions; `legacy_combined` on migrated historical snapshots. |
| `dominant_focus_pct` | integer | Share of focus time in the top app, 0-100. |
| `confidence` | number | 0-0.99 heuristic signal quality (focus volume, dominance, variety of signals) -- not a model probability. |
| `thread_count` | integer | Activity clusters in the window (without the embedding model every app is its own cluster). |
| `thread_id` | integer | Persistent work-thread id (see [`GetWorkThreads`](#getworkthreads)); 0 if none. |
| `presence` | string | Presence when composed: `active`, `passive`, `away`, `locked`, `asleep` or empty. |
| `model` | string | `granite-embedding-small-english-r2` when the embedding model is loaded, else `deterministic`. |
| `model_polish` | string | `qwen3-0.6b` when the optional LLM refiner is loaded, else `(not loaded)`. |
| `signal_types` | string[] | Contributing signals: `app_focus`, `browsing`, `app_launch`, `file`. |
| `summary_embedding` | number[] | Only with `include_vector`: 384-dimension, L2-normalized embedding of the joined `summary` lines (6 decimals); empty without the embedding model. |
| `items[]` | object[] | Top apps by focus time (at most 5): `app` (friendly name), `exe`, `title` (cleaned), `raw_title` (original window title), `focus_seconds`, `pct` (integer share of focus), `thread_id` (1-based cluster id **within this snapshot**, not a work-thread id) and `entity_key` (document / page when resolved, else the app; omitted when empty). |
| `current_entities[]` | object[] | Joinable `entity_key`, `entity_type`, `display`, `title`, `app`, `role` (`focus`, `reference`, `communication`, `observed`, `passive`, `attention_unknown`), `active_secs_in_window`, `foreground_exposure_secs`, `passive_secs`, `idle_secs`, `unknown_attention_secs`, `observed_action`, `resolution`, `last_active_ts` (0 without measured active time), `last_observed_ts`, `score` and `thread_id`. A qualified observation can exist without an engagement-store record (`ScoreEntities.found` may be false). |

When there is no interaction-active focus, admitted file/browser/app observations
still populate summaries, facets and structured entities, without synthetic focus
seconds. `observed_action` is `foreground`, `open`, `modify`, `create`, `save`,
`download`, `page_observed` or `launch`. An OPEN is rendered as "Opened", not
"Editing"; a filesystem modification is described as observed, not proven to be a
user edit. An unresolved browser window does not borrow another window's page
identity. Comprehensive all-signal canonical evidence and page episodes remain
later-phase work.

### 6. Status and Control

#### Ping

Liveness check. No parameters, no scope.

```json
{"op": "Ping"}
```

```json
{"api_version": 2, "ok": true, "pong": true, "now": 1760004000, "version": "6.0.0", "store_open": true}
```

`version` is WARP's product version. `Ping` and `GetStatus` answer even when the activity
store is not open -- then `store_open` is `false` and every other operation returns
`unavailable`.

**Checking availability.** `Ping` is the reliable check. To test only whether the pipe
exists without connecting, call `WaitNamedPipeW(name, 1)`: it fails with
`ERROR_FILE_NOT_FOUND` when WARP is not running in the session.

#### GetStatus

Health and statistics; for WARP's own executable (control scope) also paths, recent log
messages, capture error texts, the media / microphone / camera flags and the effective
settings. No parameters, no scope; answers even when the activity store is not open.

```json
{"op": "GetStatus"}
```

**Response** for a third-party client (abbreviated):

```json
{
  "api_version": 2,
  "ok": true,
  "version": "6.0.0",
  "pid": 9412,
  "session_id": 1,
  "started_utc": 1759990000,
  "uptime_secs": 14000,
  "paused": false,
  "paused_until": 0,
  "consent_acknowledged": true,
  "store": {
    "open": true, "schema_version": 3,
    "integrity": "ok", "db_bytes": 73400320, "wal_bytes": 4194304, "queue_depth": 0,
    "enqueued": 182311, "dropped": 0, "written": 182311, "batches": 9120, "write_errors": 0,
    "last_commit_utc": 1760003995, "change_seq": 58251, "reset_seq": 0,
    "raw_retention_days": 30, "entity_retention_days": 395
  },
  "api": {
    "requests": 5120, "errors": 3, "rejected": 0, "busy": 0, "active_connections": 1,
    "session_pipe": "\\\\.\\pipe\\WarpFileActivityAPI-1", "session_pipe_owned": true,
    "legacy_pipe": "\\\\.\\pipe\\WarpFileActivityAPI", "legacy_pipe_owned": true
  },
  "capture": {
    "files": { "enabled": true, "etw_running": true, "usn_running": true, "recent_running": true, "mru_running": true,
               "emitted": 41022, "filtered": 380113, "queue_dropped": 0, "etw_events_lost": 0, "etw_buffers_lost": 0,
               "last_event_ms": 1760003990120, "session": "WARP-KernelFile-1" },
    "launches": { "enabled": true, "etw_running": true, "starts": 2210, "recorded": 312, "with_window": 280,
                  "no_window": 1898, "helpers": 640, "system": 900, "other_session": 0, "exits": 2150,
                  "events_lost": 0, "last_event_ms": 1760003001000 },
    "web": { "enabled": true, "pages": 1830, "private_dropped": 12, "excluded_dropped": 40, "uia_requests": 2100,
             "uia_timeouts": 3, "uia_failures": 1, "last_event_ms": 1760003600000 },
    "focus_enabled": true
  },
  "presence": { "state": "active", "since": 1760001000, "ms_since_input": 1200, "fullscreen": false,
                "locked": false, "on_battery": false },
  "models": { "embedding_loaded": true, "llm_enabled": false, "llm_available": false, "llm_resident": false },
  "elevated": true,
  "headless": false,
  "logon_task_installed": true,
  "log": { "warnings": 4, "errors": 0 }
}
```

For WARP's own executable (its command line, e.g. `WARP!.exe --status`, and the tray) the
response also contains `store.path`, `capture.files.last_error`,
`capture.launches.last_error`, `presence.media_playing` / `mic_in_use` / `camera_in_use`,
`data_dir`, `log.path`, `log.recent_problems` and `settings` (excerpt; all other members
as above):

```json
{
  "store": { "open": true, "path": "C:\\Users\\Alice\\AppData\\Local\\WARP\\activity.db", "schema_version": 3 },
  "capture": { "files": { "session": "WARP-KernelFile-1", "last_error": "" },
               "launches": { "last_event_ms": 1760003001000, "last_error": "" } },
  "presence": { "state": "active", "on_battery": false, "media_playing": false, "mic_in_use": false,
                "camera_in_use": false },
  "data_dir": "C:\\Users\\Alice\\AppData\\Local\\WARP",
  "log": { "warnings": 4, "errors": 0, "path": "C:\\Users\\Alice\\AppData\\Local\\WARP\\logs\\warp.log",
           "recent_problems": [] },
  "settings": { "CaptureFiles": "true", "RawRetentionDays": "30", "ApiAllowedClients": "", "PausedUntilUtc": "0" }
}
```

| Member | Description |
|---|---|
| `version` | Product version. |
| `pid` | WARP's process id -- compare it with `GetNamedPipeServerProcessId`. |
| `session_id` | Windows session WARP runs in (its pipe is `WarpFileActivityAPI-<session_id>`). |
| `started_utc` / `uptime_secs` | When the API started, and the seconds since. |
| `paused` / `paused_until` | Capture pause state; `paused_until` is 0 (not paused), a Unix time, or INT64_MAX (until resumed). |
| `consent_acknowledged` | Whether the user acknowledged WARP's consent notice. |
| `store` | Database: `open`, `path` (WARP only), `schema_version` (the database's `PRAGMA user_version`, read when the store opened; 0 if it never opened), `integrity` (`ok`, `recovered ...` or an error text), `db_bytes`, `wal_bytes`; writer queue: `queue_depth`, `enqueued`, `dropped`, `written`, `batches`, `write_errors`, `last_commit_utc`; change feed: `change_seq`, `reset_seq`; retention: `raw_retention_days`, `entity_retention_days`. |
| `api` | `requests`, `errors` (error responses), `rejected` (connections refused with `wrong_session` / `unauthorized`), `busy` (connections answered with `busy` because the queue was full), `active_connections`, and both pipe names with whether WARP owns them. |
| `capture` | Per-source health: `files` (ETW, USN, Recent-items and Office-MRU producers, counters, ETW losses, ETW session name, last event; `last_error` WARP only), `launches` (counters, last event; `last_error` WARP only), `web`, `focus_enabled`. |
| `presence` | `state`, `since`, `ms_since_input`, `fullscreen`, `locked`, `on_battery`; WARP only also `media_playing`, `mic_in_use`, `camera_in_use`. |
| `models` | `embedding_loaded`, `llm_enabled`, `llm_available`, `llm_resident`. |
| `data_dir` | WARP only: WARP's data folder. |
| `elevated`, `headless`, `logon_task_installed` | Runtime facts. |
| `log` | `warnings` and `errors` counts; for WARP only also the log file `path` and up to 10 `recent_problems`. |
| `settings` | WARP only: every effective setting as a string (`"true"` / `"false"`, numbers, `;`-joined lists), with ` (policy)` appended when the value is set by policy. |

`capture`, `presence`, `models`, `data_dir`, `elevated`, `headless` and
`logon_task_installed` come from WARP's status provider and are absent when the API runs
without one (e.g. in tests).

#### Purge

Deletes recorded data (control scope: WARP's command line and tray only).

| Param | Type | Default | Notes |
|---|---|---|---|
| `start` | integer | 0 | Unix seconds; 0 = from the beginning. |
| `end` | integer | 0 | Unix seconds; 0 = no upper bound. `start` > `end` -> `invalid_request`. |
| `entity_key` | string | -- | Purge one entity everywhere (exact key, used verbatim). |
| `key` | string | -- | The same from a raw identifier ([normalized](#entity-keys-and-normalization)); used when `entity_key` is absent or empty. |
| `domain` | string | -- | A web host; subdomains are included. |
| `path_prefix` | string | -- | A folder (or file): everything at or below it. |

Exactly **one** kind of criterion per request: a time range (`start` and/or `end`), an
entity (`key` / `entity_key`), a `domain` or a `path_prefix`. No criterion, or more than
one kind, -> `invalid_request` (the message names the accepted criteria).

```json
{"op": "Purge", "domain": "example.com"}
```

```json
{"api_version": 2, "ok": true, "rows_deleted": 412}
```

| Criterion | What is deleted |
|---|---|
| time range | Raw events of every type, clipboard / print records, context snapshots whose summary window overlaps the range, and co-occurrence edges last updated inside it. Intervals that cross a range boundary -- focus sessions, presence intervals, work-thread spans, work sessions -- keep their parts outside the range (active / idle / passive seconds scaled to the kept share). Daily rollups: days the range covers completely are dropped (so are partly covered days older than the raw retention, where activity inside and outside the range can no longer be told apart); on the boundary days exactly what the purged rows had contributed is subtracted -- WARP replays each affected item's raw rows through the live crediting rules (de-duplication windows, safe-saves) with and without the purged rows -- so other items used that day are not re-scored. Entities left without evidence are tombstoned with `reason: "purge"`; every other affected entity keeps its live totals and engagement minus the purged share (`last_*_ts` timestamps fall back to the latest remaining evidence) and appears as an upsert in the change feed. |
| `entity_key` / `key` | The entity's raw events (including focus sessions that showed it), daily rollups, co-occurrence edges, thread memberships and the context snapshots mentioning it; the entity is tombstoned. |
| `domain` | Browsing events of the host and its subdomains; the URL entities on that host -- matched by key or display URL, so document-keyed pages (`gdoc:...`, `yt:...`) are included -- with their rollups, edges, memberships and focus sessions; context snapshots mentioning the host. |
| `path_prefix` | File events at or below the folder (also matched by old path) and the focus sessions, rollups, edges and thread memberships of files there -- including files that are no longer ranked entities; its file entities are tombstoned; context snapshots mentioning it. |

Every criterion also re-derives the [work threads](#getworkthreads) the purged items or time
belonged to: a thread's label is a summary line written by a snapshot and may name what was
purged, so it is rebuilt from the thread's strongest remaining members (e.g. `"spec.docx,
model.xlsx"`) and its label embedding is reset; a thread left without members or spans is
deleted. Changed threads carry a new `change_seq`.

A purge also covers activity that is **reported after it**: an Office MRU or Recent-items poll,
a held safe-save, or a focus session / presence interval that was still open. Records about a
purged time range are not stored (intervals keep only their parts outside it), and work
sessions are never joined across purged time; purged time ranges are remembered across
restarts. Records about a purged item, web host or folder that happened before the purge are
not stored either (until WARP restarts -- the purged names are not written to disk). Activity
after the purge is recorded normally.

Tombstoned entities appear in the change feed as deletes with `reason: "purge"`. After
every purge WARP drops its write caches so nothing purged is written back from memory, and
on success it also clears its in-memory context history. `rows_deleted` is approximate
(every tombstoned entity counts as one row). When the purge does not complete (store busy
or closed, 120 s timeout) the answer is an error and nothing is reported as deleted:

```json
{"api_version":2,"ok":false,"error":{"code":"internal_error","message":"the purge did not complete (store busy or closed); nothing was reported as deleted"}}
```

#### Pause and Resume

Pause or resume capture (control scope: WARP's command line and tray only). While paused,
the capture sources drop new events and the context is not recomposed; the API keeps
serving the data recorded so far.

| Param (`Pause`) | Type | Default | Notes |
|---|---|---|---|
| `indefinite` | bool | `false` | `true` = until `Resume`. Takes precedence over `until` and `minutes`. |
| `until` | integer | -- | Unix seconds; must be in the future (`invalid_request`). Takes precedence over `minutes`. |
| `minutes` | integer | 60 | Clamped to 1-10080 (7 days). |

`Resume` takes no parameters.

```json
{"op": "Pause", "minutes": 30}
```

```json
{"api_version": 2, "ok": true, "paused": true, "paused_until": 1760005800}
```

- `Resume` answers `{"api_version": 2, "ok": true, "paused": false, "paused_until": 0}`.
- For `indefinite` pauses `paused_until` is `9223372036854775807` (INT64_MAX).
- When the pause state cannot be changed (`PausedUntilUtc` is set by policy, or the
  registry write failed), both operations answer
  `{"api_version": 2, "ok": false, "paused": false, "paused_until": 0, "error": {"code": "locked", "message": "pause state is controlled by policy"}}`.
- The pause state is stored as `PausedUntilUtc` under `HKCU\Software\WARP` and survives
  restarts.

---

## Entity Records and Scores

### Entity Record Fields

Returned by `ScoreEntities`, `TopEntities`, `SearchActivity` and `GetChanges` (upserts).
`QueryInferences` and `GetInferenceDeltas` return a subset under the same names, plus
`version` (= `change_seq`) and `updated_at` (when the record was last written).

| Field | Type | Description |
|---|---|---|
| `entity_key` | string | [Entity key](#entity-keys-and-normalization). |
| `entity_type` | string | `file`, `app` or `url`. |
| `display_name` | string | Full path, exe path or AUMID, or canonical URL. Omitted by `ScoreEntities` for clients without the activity scope. |
| `title` | string | Best human title (page title, app name). Omitted when empty or without the activity scope. |
| `last_event_ts` | integer | Last event of any kind (Unix seconds; 0 = never). |
| `last_open_ts` | integer | Last open, launch or visit. |
| `last_edit_ts` | integer | Last save, modify or create (files). |
| `last_active_ts` | integer | Last time the entity was in the foreground with the user present. |
| `open_count_7d` / `open_count_30d` | integer | Opens, launches and visits during the last 7 / 30 local days (today included), confidence-weighted and rounded; computed at query time from the daily rollups. |
| `open_count_total` | integer | Lifetime opens (confidence-weighted, rounded). |
| `edit_count_total` | integer | Lifetime edits (confidence-weighted, rounded). |
| `active_secs_7d` / `active_secs_total` | integer | Active foreground seconds during the last 7 local days / lifetime. |
| `distinct_days` | integer | Number of different local days with engagement. |
| `recency_score` | number | Legacy v1 score, 0-255, evaluated at query time: `200 * e^(-(now - last_open_ts) / 172800) + 5 * ln(1 + open_count_7d)` (0 if never opened). |
| `change_seq` | integer | Global change sequence of the record's last update (the v1 `version`). |

### Score Fields

Returned by `ScoreEntities`, `TopEntities` and `SearchActivity`:

| Field | Type | Description |
|---|---|---|
| `score` | number | 0-1: percentile of `prior` among all entities of the same `entity_type` for the requested horizon (0 without engagement). Comparable across types. |
| `prior` | number | Raw engagement for the horizon: `short_term` (`short`), `long_term` (`long`) or `0.7 * short_term + 0.3 * long_term * 3 / 30` (`blend`). |
| `short_term` | number | Engagement decayed with a 3-day time constant, evaluated now. |
| `long_term` | number | Engagement decayed with a 30-day time constant, evaluated now. |
| `reasons` | string[] | Human-readable explanations, e.g. `edited today`, `edited in the last 7 days`, `42 min active in the last 7 days`, `used on 3 of the last 7 days`, `opened 5 times in the last 7 days`, `recurring: used on 12 different days`, `last used 2 h ago`. |

### How Scores Work

1. **Engagement credit.** Every engagement adds a weight to two exponentially decayed
   accumulators of the entity: kernel-observed open 0.3 x confidence; explicit open (shell,
   Recent items, Office MRU, a user-visible launch) 1.5; edit (save / modify / create)
   3.0 x confidence; web visit 0.5 x confidence; 1.0 per active foreground minute (at most
   60 per session). The first engagement on a new local day adds a bonus of up to 2.0.
   Repeated opens within 2 minutes and edit bursts within 1 minute count once.
2. **Decay at read time.** An accumulator stores its value S at time t0; its value now is
   `S * e^(-(now - t0) / tau)`, with tau = 3 days (`short_term`) or 30 days (`long_term`).
   Nothing is frozen at write time, so a score falls while the entity goes unused.
3. **Horizon.** `short` ranks by the last few days, `long` by the last weeks; `blend` (the
   default) combines both, the long-term part rescaled by 3/30 to the short-term scale.
4. **Percentile.** `score` is the rank of the entity's `prior` among all non-deleted
   entities of its type (the distribution is rebuilt at most every 5 minutes), so scores of
   files, apps and pages are comparable. `TopEntities` without `type` merges types by
   `score`; `SearchActivity` orders by `0.6 * relevance + 0.4 * score`.

---

## Data Retention

| Data | Kept | Setting |
|---|---|---|
| Raw events (file, app launch, browsing, app focus), presence, context snapshots, clipboard / print metadata | 30 days | `RawRetentionDays` (1-3650) |
| Entities, daily rollups, co-occurrence edges, work sessions and threads | 395 days after their last activity | `EntityRetentionDays` (7-3650) |
| Tombstones (deletes in the change feed) | 90 days; older cursors get `reset` | -- |

| Property | Value |
|---|---|
| **Database** | `%LOCALAPPDATA%\WARP\activity.db` (or `activity.db` in the `--data-dir` folder); SQLite in WAL mode, schema version 2. |
| **Encryption** | The data folder is EFS-encrypted when `EncryptDataDir` = 1 (default). |
| **Maintenance** | Retention runs at start-up and periodically; expired entities are tombstoned with `reason: "retention"`. |
| **Clear-all** | Clearing all data in WARP's UI deletes everything and advances `reset_seq`; change-feed cursors then get `reset`. |
| **Timestamps** | Unix seconds, UTC. |

---

## Integration Patterns

### 1. Recent Items Picker

Show the files (or apps, or pages) the user works with most right now:

```json
{"op": "TopEntities", "type": "file", "limit": 20, "horizon": "short"}
```

Display `display_name` / `title`, and show `reasons` as a tooltip ("edited today", "last
used 5 min ago"). Use `"horizon": "long"` for "frequently used" lists.

### 2. Context-Aware Ranking

Rank candidates your app already has (search results, launcher entries, attachments) by
engagement and by relevance to the current task:

```json
{
  "op": "ScoreEntities",
  "keys": ["C:\\Users\\Alice\\Projects\\app\\main.cpp", "C:\\Users\\Alice\\Projects\\app\\utils.h",
           "https://github.com/contoso/app/pull/12"],
  "context": "current",
  "reasons": false
}
```

Blend your own relevance with `score` and `context_boost` (e.g.
`0.6 * relevance + 0.25 * score + 0.15 * context_boost`). Unknown items come back with
`found: false`. `ScoreEntities` needs no activity scope.

### 3. Local Index Sync

Mirror WARP's entities into your own index (desktop search, IDE extension, launcher)
without re-reading everything: run the [sync loop](#getchanges) -- `GetChanges` until
`more` is `false`, then `WaitForChanges` with the returned `next`, then `GetChanges`
again. Persist `next`; on `reset`, rebuild from `since: 0`. v1 clients can keep using
`GetInferenceDeltas` with `next_version`.

### 4. Backup & Sync Engine

Find files created or modified in a time range, page by page:

```json
{"op": "QueryTimeline", "start": 1760000400, "end": 1760004000, "types": ["file"], "limit": 1000}
```

Keep the `CREATE`, `MODIFY` and `RENAME` actions (use `old_path`) and follow `next_cursor`
until it is `null`. Remember the `end` of the last completed range as your high-water mark.

### 5. Security Auditing

Replay everything that happened in a period -- events, work sessions and threads:

```json
{"op": "QueryTimeline", "start": 1759399200, "end": 1760004000, "limit": 5000}
```

Page each event type with `next_cursor`, restricting `types` to the keys of
`next_cursor`. `confidence` and `source` separate user actions from background activity;
`RENAME` with `old_path` tracks provenance.

### 6. Developer Tooling

Detect which source files changed during a build (v1 raw query):

```json
{"seconds":120,"types":["file"]}
```

Filter for `action == "MODIFY"` and paths under your project directory; compare
`entity_key` values for case-insensitive matching.

### 7. Smart Cleanup Utility

Find files that have not been used for a long time by scoring the candidates you found on
disk with the long horizon:

```json
{"op": "ScoreEntities", "keys": ["D:\\Archive\\old-report.docx", "D:\\Archive\\notes.txt"], "horizon": "long", "reasons": false}
```

Items with `found: false` or an old `last_event_ts` are candidates (entities are kept for
395 days after their last activity).

### 8. App Usage Analytics

Show which applications the user spends time in:

```json
{"op": "TopEntities", "type": "app", "limit": 20, "horizon": "short"}
```

`active_secs_7d` gives the foreground time per app. For a per-day breakdown, sum the
`active_secs` of `QueryTimeline` `focus` sessions by `exe_name`.

### 9. Browsing History Dashboard

Show pages visited in the last hour:

```json
{"window":"1h","types":["browsing"]}
```

Display `browsing_activities.events` with `browser`, `title` and `url`. For the most
visited pages, use `TopEntities` with `"type": "url"`.

### 10. Work Resumption

Help the user pick up where they left off:

```json
{"op": "GetWorkThreads", "start": 1759399200, "limit": 10}
```

Show each thread's `label`, `spans` and top `members`, and offer to reopen the members of a
thread. For a single item, `GetRelated` lists what was used together with it:

```json
{"op": "GetRelated", "key": "C:\\Users\\Alice\\Documents\\report.docx", "limit": 10}
```

### 11. Semantic Context Awareness

Adapt suggestions, search results or UI to what the user is doing right now:

```json
{"op": "GetCurrentContext"}
```

`context.snapshot.summary` names the documents, tabs and apps of the current activity
(e.g. *"User is working on the quarterly report and Q3 budget in Word and Excel"*),
`context.snapshot.current_entities` lists the entities behind it with their `role`, and
`thread` identifies the work thread. Use them for:

- **Search ranking** -- score results with `ScoreEntities` and `"context": "current"`, or
  boost results related to the current entities.
- **Smart suggestions** -- recommend files or actions related to the current entities
  (`GetRelated`).
- **Dashboard widgets** -- show a "Currently working on" summary.

For short-term memory or context drift over time:

```json
{"op": "GetRecentContexts", "count": 20, "include_vector": false}
```

Snapshots are returned newest first and recorded only on material change, so the list
reflects context *transitions* rather than polling ticks. With the granite model loaded,
`summary_embedding` (requested with `include_vector`) lets you cluster or compare
snapshots.

---

## Client Examples

All examples open the per-session pipe, switch to message mode, read every response until
the message is complete, reuse one connection for several requests and reconnect (once per
request) when WARP has closed it.

### C++ (Win32)

```cpp
#include <windows.h>
#include <cstdio>
#include <string>

// Opens WARP's pipe for this session in message mode (INVALID_HANDLE_VALUE on failure).
HANDLE OpenWarp()
{
    DWORD session = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &session);
    const std::wstring names[] = {
        L"\\\\.\\pipe\\WarpFileActivityAPI-" + std::to_wstring(session),
        L"\\\\.\\pipe\\WarpFileActivityAPI",                  // only needed for pre-v2 WARP builds
    };
    for (const std::wstring& name : names)
    {
        for (int attempt = 0; attempt < 5; ++attempt)
        {
            HANDLE h = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                   OPEN_EXISTING, 0, nullptr);
            if (h != INVALID_HANDLE_VALUE)
            {
                DWORD mode = PIPE_READMODE_MESSAGE;
                if (SetNamedPipeHandleState(h, &mode, nullptr, nullptr)) return h;
                CloseHandle(h);
                return INVALID_HANDLE_VALUE;
            }
            if (GetLastError() != ERROR_PIPE_BUSY) break;    // ERROR_FILE_NOT_FOUND: try the next name
            WaitNamedPipeW(name.c_str(), 1000);
        }
    }
    return INVALID_HANDLE_VALUE;
}

// One request -> the complete response message, or "" when the server has closed the
// connection. A "busy" error written before the request is still read after a failed write.
std::string Exchange(HANDLE h, const std::string& request)
{
    DWORD written = 0;
    WriteFile(h, request.data(), (DWORD)request.size(), &written, nullptr);
    std::string response;
    char buf[65536];
    for (;;)
    {
        DWORD n = 0;
        BOOL ok = ReadFile(h, buf, sizeof(buf), &n, nullptr);
        response.append(buf, n);
        if (ok) return response;                             // message complete
        if (GetLastError() != ERROR_MORE_DATA) return {};    // ERROR_MORE_DATA: read the rest
    }
}

// Reuses the connection and reconnects once when WARP has closed it (other clients waiting,
// 10 s idle, 256 requests). Returns "" when WARP cannot be reached.
std::string CallWarp(HANDLE& h, const std::string& request)
{
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        if (h == INVALID_HANDLE_VALUE) h = OpenWarp();
        if (h == INVALID_HANDLE_VALUE) return {};
        std::string response = Exchange(h, request);
        if (!response.empty()) return response;
        CloseHandle(h);
        h = INVALID_HANDLE_VALUE;
    }
    return {};
}

int main()
{
    HANDLE h = OpenWarp();
    if (h == INVALID_HANDLE_VALUE)
    {
        printf("WARP is not running in this session (error %lu)\n", GetLastError());
        return 1;
    }
    ULONG serverPid = 0;
    GetNamedPipeServerProcessId(h, &serverPid);              // should be WARP's PID (GetStatus "pid")

    printf("%s\n\n", CallWarp(h, R"({"op":"Ping"})").c_str());
    printf("%s\n\n", CallWarp(h, R"({"op":"TopEntities","type":"file","limit":10,"horizon":"short"})").c_str());
    printf("%s\n\n", CallWarp(h, R"({"op":"GetChanges","since":0,"limit":100})").c_str());
    printf("%s\n", CallWarp(h, R"({"window":"15m","types":["app_launch","browsing"]})").c_str());

    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    return 0;
}
```

Parse the responses with any JSON library (e.g. nlohmann/json) and check for an `error`
member (for example `busy`: back off and retry) before using the payload.

### C# (.NET)

```csharp
using System;
using System.Diagnostics;
using System.IO;
using System.IO.Pipes;
using System.Text;
using System.Text.Json;

using var warp = new WarpClient();

// Most engaged files of the last few days
using (var top = warp.Call("""{"op":"TopEntities","type":"file","limit":10,"horizon":"short"}"""))
{
    foreach (var e in top.RootElement.GetProperty("results").EnumerateArray())
        Console.WriteLine($"{e.GetProperty("score").GetDouble():F2}  {e.GetProperty("display_name").GetString()}");
}

// Incremental sync: GetChanges until "more" is false
long since = 0;                                       // persist between runs
while (true)
{
    using var ch = warp.Call($$"""{"op":"GetChanges","since":{{since}},"limit":1000}""");
    var root = ch.RootElement;
    if (root.GetProperty("reset").GetBoolean()) { since = 0; continue; }   // drop the local copy, resync
    foreach (var d in root.GetProperty("deletes").EnumerateArray())
        Console.WriteLine($"delete {d.GetProperty("entity_key").GetString()} ({d.GetProperty("reason").GetString()})");
    foreach (var u in root.GetProperty("upserts").EnumerateArray())
        Console.WriteLine($"upsert {u.GetProperty("entity_key").GetString()}");
    since = root.GetProperty("next").GetInt64();
    if (!root.GetProperty("more").GetBoolean()) break;
}

// v1 raw events still work
using (var raw = warp.Call("""{"window":"6h","types":["file"]}"""))
{
    var fa = raw.RootElement.GetProperty("file_activities");
    Console.WriteLine($"File events: {fa.GetProperty("count").GetInt32()}, truncated: {fa.GetProperty("truncated").GetBoolean()}");
    foreach (var e in fa.GetProperty("events").EnumerateArray())
        Console.WriteLine($"  [{e.GetProperty("action").GetString()}] {e.GetProperty("path").GetString()}");
}

sealed class WarpClient : IDisposable
{
    NamedPipeClientStream? pipe;

    // One request -> one response. Reuses the connection and reconnects once when WARP has
    // closed it (other clients waiting, 10 s idle, 256 requests). Throws on WARP errors.
    public JsonDocument Call(string request)
    {
        for (int attempt = 0; ; attempt++)
        {
            try
            {
                pipe ??= Connect();
                return Exchange(pipe, request);
            }
            catch (IOException) when (attempt == 0)
            {
                pipe?.Dispose();
                pipe = null;
            }
        }
    }

    public void Dispose() => pipe?.Dispose();

    // \\.\pipe\WarpFileActivityAPI-<SessionId>
    static NamedPipeClientStream Connect()
    {
        var name = $"WarpFileActivityAPI-{Process.GetCurrentProcess().SessionId}";
        var p = new NamedPipeClientStream(".", name, PipeDirection.InOut);   // GENERIC_READ | GENERIC_WRITE
        p.Connect(5000);                              // TimeoutException: WARP is not running in this session
        p.ReadMode = PipeTransmissionMode.Message;
        return p;
    }

    static JsonDocument Exchange(NamedPipeClientStream p, string request)
    {
        byte[] req = Encoding.UTF8.GetBytes(request);
        p.Write(req, 0, req.Length);
        using var ms = new MemoryStream();
        byte[] buf = new byte[65536];
        do
        {
            int n = p.Read(buf, 0, buf.Length);
            if (n == 0) throw new IOException("WARP closed the connection");
            ms.Write(buf, 0, n);
        } while (!p.IsMessageComplete);
        var doc = JsonDocument.Parse(ms.ToArray());
        if (doc.RootElement.TryGetProperty("error", out var err))
        {
            string message = $"{err.GetProperty("code").GetString()}: {err.GetProperty("message").GetString()}";
            doc.Dispose();
            throw new InvalidOperationException(message);   // e.g. "busy: ..." -> back off and retry
        }
        return doc;
    }
}
```

The raw string literals need C# 11 (.NET 7) or later.

### Python

Standard library only (`ctypes`); the repository's `tests/python/warp_pipe.py` is a fuller
version of the same client.

```python
from __future__ import annotations

import ctypes
import ctypes.wintypes as wt
import json
import os

kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
kernel32.CreateFileW.restype = wt.HANDLE
kernel32.CreateFileW.argtypes = [wt.LPCWSTR, wt.DWORD, wt.DWORD, wt.LPVOID, wt.DWORD, wt.DWORD, wt.HANDLE]
kernel32.SetNamedPipeHandleState.argtypes = [wt.HANDLE, ctypes.POINTER(wt.DWORD), wt.LPVOID, wt.LPVOID]
kernel32.WriteFile.argtypes = [wt.HANDLE, wt.LPCVOID, wt.DWORD, ctypes.POINTER(wt.DWORD), wt.LPVOID]
kernel32.ReadFile.argtypes = [wt.HANDLE, wt.LPVOID, wt.DWORD, ctypes.POINTER(wt.DWORD), wt.LPVOID]
kernel32.CloseHandle.argtypes = [wt.HANDLE]
kernel32.ProcessIdToSessionId.argtypes = [wt.DWORD, ctypes.POINTER(wt.DWORD)]

GENERIC_READ, GENERIC_WRITE, OPEN_EXISTING = 0x80000000, 0x40000000, 3
PIPE_READMODE_MESSAGE, ERROR_MORE_DATA = 0x2, 234
INVALID_HANDLE_VALUE = ctypes.c_void_p(-1).value


def session_pipe_name() -> str:
    sid = wt.DWORD(0)
    kernel32.ProcessIdToSessionId(os.getpid(), ctypes.byref(sid))
    return r"\\.\pipe\WarpFileActivityAPI-%d" % sid.value


class WarpClient:
    """Reuses one connection and reconnects once per request when WARP has closed it
    (other clients waiting, 10 s idle, 256 requests)."""

    def __init__(self, name: str | None = None):
        self.name = name or session_pipe_name()
        self.h = None

    def _open(self):
        h = kernel32.CreateFileW(self.name, GENERIC_READ | GENERIC_WRITE, 0, None, OPEN_EXISTING, 0, None)
        if not h or h == INVALID_HANDLE_VALUE:
            raise ctypes.WinError(ctypes.get_last_error())      # 2: WARP not running in this session
        mode = wt.DWORD(PIPE_READMODE_MESSAGE)
        if not kernel32.SetNamedPipeHandleState(h, ctypes.byref(mode), None, None):
            err = ctypes.get_last_error()
            kernel32.CloseHandle(h)
            raise ctypes.WinError(err)
        self.h = h

    def _exchange(self, data: bytes) -> bytes:
        written = wt.DWORD(0)
        kernel32.WriteFile(self.h, data, len(data), ctypes.byref(written), None)  # a pending "busy" stays readable
        chunks, buf = [], ctypes.create_string_buffer(65536)
        while True:
            got = wt.DWORD(0)
            ok = kernel32.ReadFile(self.h, buf, len(buf), ctypes.byref(got), None)
            chunks.append(buf.raw[:got.value])
            if ok:
                return b"".join(chunks)                          # message complete
            err = ctypes.get_last_error()
            if err != ERROR_MORE_DATA:
                raise ctypes.WinError(err)                       # closed by the server

    def call(self, request: dict) -> dict:
        data = json.dumps(request).encode("utf-8")
        for attempt in range(2):
            if self.h is None:
                self._open()
            try:
                raw = self._exchange(data)
                break
            except OSError:
                self.close()
                if attempt:
                    raise
        resp = json.loads(raw.decode("utf-8"))
        if "error" in resp:                                      # e.g. busy: back off and retry
            raise RuntimeError("%s: %s" % (resp["error"]["code"], resp["error"]["message"]))
        return resp

    def close(self):
        if self.h is not None:
            kernel32.CloseHandle(self.h)
            self.h = None

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()


with WarpClient() as warp:
    print(warp.call({"op": "Ping"}))

    # Most engaged files of the last few days
    for e in warp.call({"op": "TopEntities", "type": "file", "limit": 10, "horizon": "short"})["results"]:
        print(f"{e['score']:.2f}  {e['display_name']}  ({'; '.join(e['reasons'])})")

    # Rank your own candidates, boosted by what the user is doing now
    ranked = warp.call({"op": "ScoreEntities", "context": "current", "reasons": False,
                        "keys": [r"C:\Users\Alice\Documents\report.docx",
                                 "https://github.com/contoso/app/pull/12"]})
    for r in ranked["results"]:
        print(r["key"], r["found"], r.get("score"), r.get("context_boost"))

    # Incremental sync with the change feed
    since = 0                                                    # persist between runs
    while True:
        ch = warp.call({"op": "GetChanges", "since": since, "limit": 1000})
        if ch["reset"]:
            since = 0                                            # drop the local copy, then resync
            continue
        for d in ch["deletes"]:
            pass                                                 # remove d["entity_key"] from your index
        for u in ch["upserts"]:
            pass                                                 # upsert u["entity_key"] into your index
        since = ch["next"]
        if not ch["more"]:
            break

    # v1 raw events still work
    raw = warp.call({"window": "15m", "types": ["file", "browsing"]})
    for e in raw.get("file_activities", {}).get("events", [])[:5]:
        print(f"  [{e['action']}] {e['path']}")

    # What is the user doing right now?
    ctx = warp.call({"op": "GetCurrentContext"})["context"]
    snap = ctx.get("snapshot") or {}
    print(ctx["presence"], " / ".join(snap.get("summary", [])))
```

### PowerShell

```powershell
$script:WarpPipe = $null

function Connect-Warp {
    $session = (Get-Process -Id $PID).SessionId
    $pipe = [System.IO.Pipes.NamedPipeClientStream]::new(
        ".", "WarpFileActivityAPI-$session", [System.IO.Pipes.PipeDirection]::InOut)
    $pipe.Connect(5000)                      # TimeoutException: WARP is not running in this session
    $pipe.ReadMode = [System.IO.Pipes.PipeTransmissionMode]::Message
    return $pipe
}

# One request -> one response. Reuses the connection and reconnects once when WARP has
# closed it (other clients waiting, 10 s idle, 256 requests).
function Invoke-Warp([string]$Request) {
    $bytes = [Text.Encoding]::UTF8.GetBytes($Request)
    for ($attempt = 0; ; $attempt++) {
        try {
            if (-not $script:WarpPipe) { $script:WarpPipe = Connect-Warp }
            $script:WarpPipe.Write($bytes, 0, $bytes.Length)
            $ms = [System.IO.MemoryStream]::new()
            $buffer = [byte[]]::new(65536)
            do {                             # read until the response message is complete
                $n = $script:WarpPipe.Read($buffer, 0, $buffer.Length)
                if ($n -eq 0) { throw [System.IO.IOException]::new("WARP closed the connection") }
                $ms.Write($buffer, 0, $n)
            } while (-not $script:WarpPipe.IsMessageComplete)
            break
        }
        catch [System.IO.IOException] {
            if ($script:WarpPipe) { $script:WarpPipe.Dispose(); $script:WarpPipe = $null }
            if ($attempt -ge 1) { throw }
        }
    }
    $response = [Text.Encoding]::UTF8.GetString($ms.ToArray()) | ConvertFrom-Json
    if ($response.error) { throw "WARP error $($response.error.code): $($response.error.message)" }
    return $response
}

try {
    Invoke-Warp '{"op":"Ping"}' | Format-List

    # Apps the user spent most time in recently
    (Invoke-Warp '{"op":"TopEntities","type":"app","limit":10,"horizon":"short"}').results |
        Select-Object @{ n = 'score'; e = { [math]::Round($_.score, 2) } }, active_secs_7d, display_name |
        Format-Table -AutoSize

    # Change feed
    $since = 0                               # persist between runs
    while ($true) {
        $ch = Invoke-Warp ('{"op":"GetChanges","since":' + $since + ',"limit":1000}')
        if ($ch.reset) { $since = 0; continue }   # drop the local copy, resync from 0
        $ch.deletes | ForEach-Object { "delete $($_.entity_key) ($($_.reason))" }
        $ch.upserts | ForEach-Object { "upsert $($_.entity_key)" }
        $since = $ch.next
        if (-not $ch.more) { break }
    }

    # v1 raw events still work
    (Invoke-Warp '{"window":"1h","types":["app_launch"]}').app_launch_activities.events |
        Select-Object timestamp, exe_name, pid |
        Format-Table -AutoSize

    # What is the user doing right now?
    $ctx = (Invoke-Warp '{"op":"GetCurrentContext"}').context
    Write-Host "$($ctx.presence): $($ctx.snapshot.summary -join ' / ')"
}
finally {
    if ($script:WarpPipe) { $script:WarpPipe.Dispose() }
}
```

Control operations are reserved for WARP's own executable; from a script, use its command
line:

```powershell
& "$env:ProgramFiles\WARP\WARP!.exe" --status          # GetStatus
& "$env:ProgramFiles\WARP\WARP!.exe" --pause 30        # Pause for 30 minutes
& "$env:ProgramFiles\WARP\WARP!.exe" --resume
& "$env:ProgramFiles\WARP\WARP!.exe" --purge-domain example.com
```

### Rust

No crates needed: the standard library opens the pipe with `GENERIC_READ | GENERIC_WRITE`;
two kernel32 functions are declared directly. Add `serde_json` to parse the responses.

```rust
use std::ffi::c_void;
use std::fs::{File, OpenOptions};
use std::io::{self, Read, Write};
use std::os::windows::io::AsRawHandle;

#[link(name = "kernel32")]
unsafe extern "system" {          // Rust 1.82+; drop `unsafe` on older toolchains
    fn GetCurrentProcessId() -> u32;
    fn ProcessIdToSessionId(pid: u32, session: *mut u32) -> i32;
    fn SetNamedPipeHandleState(pipe: *mut c_void, mode: *mut u32, max: *mut u32, timeout: *mut u32) -> i32;
}

const PIPE_READMODE_MESSAGE: u32 = 0x2;
const ERROR_MORE_DATA: i32 = 234;

fn open_warp() -> io::Result<File> {
    let mut session = 0u32;
    unsafe { ProcessIdToSessionId(GetCurrentProcessId(), &mut session) };
    let name = format!(r"\\.\pipe\WarpFileActivityAPI-{}", session);
    let pipe = OpenOptions::new().read(true).write(true).open(name)?;   // NotFound: WARP not running in this session
    let mut mode = PIPE_READMODE_MESSAGE;
    let ok = unsafe {
        SetNamedPipeHandleState(pipe.as_raw_handle() as *mut c_void, &mut mode,
                                std::ptr::null_mut(), std::ptr::null_mut())
    };
    if ok == 0 { return Err(io::Error::last_os_error()); }
    Ok(pipe)
}

// One request -> one complete response message (Err when the server has closed the connection).
fn exchange(pipe: &mut File, request: &str) -> io::Result<String> {
    let _ = pipe.write_all(request.as_bytes());   // if this fails, a pending "busy" error is still readable
    let mut out = Vec::new();
    let mut buf = vec![0u8; 65536];
    loop {
        match pipe.read(&mut buf) {
            Ok(0) if out.is_empty() => return Err(io::ErrorKind::BrokenPipe.into()),   // closed by the server
            Ok(n) => { out.extend_from_slice(&buf[..n]); break; }
            // ERROR_MORE_DATA: the buffer was filled completely and the message continues.
            Err(e) if e.raw_os_error() == Some(ERROR_MORE_DATA) => out.extend_from_slice(&buf),
            Err(e) => return Err(e),
        }
    }
    String::from_utf8(out).map_err(|e| io::Error::new(io::ErrorKind::InvalidData, e))
}

// Reuses one connection and reconnects once per request when WARP has closed it
// (other clients waiting, 10 s idle, 256 requests).
struct Warp { pipe: Option<File> }

impl Warp {
    fn call(&mut self, request: &str) -> io::Result<String> {
        let mut attempt = 0;
        loop {
            if self.pipe.is_none() { self.pipe = Some(open_warp()?); }
            match exchange(self.pipe.as_mut().unwrap(), request) {
                Ok(response) => return Ok(response),
                Err(_) if attempt == 0 => { self.pipe = None; attempt += 1; }
                Err(e) => return Err(e),
            }
        }
    }
}

fn main() -> io::Result<()> {
    let mut warp = Warp { pipe: None };
    println!("{}", warp.call(r#"{"op":"Ping"}"#)?);
    println!("{}", warp.call(r#"{"op":"TopEntities","type":"url","limit":10}"#)?);
    println!("{}", warp.call(r#"{"op":"GetChanges","since":0,"limit":100}"#)?);
    println!("{}", warp.call(r#"{"window":"15m","types":["app_launch"]}"#)?);
    Ok(())
}
```

### Node.js

Node opens pipes in byte mode, so message boundaries are not visible: since the server
keeps the connection open (keep-alive), detect the end of a response by parsing -- every
response is a single JSON object ending with `}`. Waiting for `'end'` would stall until the
server closes the connection. Node has no API for the session id; this example reads it
from `tasklist`.

```javascript
const net = require('net');
const { execFileSync } = require('child_process');

function sessionPipeName() {
    // tasklist CSV row: "node.exe","1234","Console","1","45,000 K" -> session id is column 4
    const row = execFileSync('tasklist', ['/FI', `PID eq ${process.pid}`, '/FO', 'CSV', '/NH'],
                             { encoding: 'utf8' }).trim().split(/\r?\n/)[0];
    return `\\\\.\\pipe\\WarpFileActivityAPI-${JSON.parse(`[${row}]`)[3]}`;
}

class WarpClient {
    constructor(name = sessionPipeName()) { this.name = name; this.socket = null; }

    connect() {
        return new Promise((resolve, reject) => {
            this.socket = net.connect(this.name, resolve);
            this.socket.on('error', () => {});                  // between calls: the next call reconnects
            this.socket.once('error', reject);                  // ENOENT: WARP not running in this session
        });
    }

    // One request -> one response; send the next request only after this one resolved.
    // Reuses the connection and reconnects once when WARP has closed it (other clients
    // waiting, 10 s idle, 256 requests).
    async call(request, retry = true) {
        if (!this.socket || this.socket.destroyed) await this.connect();
        try {
            return await this.exchange(request);
        } catch (err) {
            if (err.warpError || !retry) throw err;             // WARP's own errors (e.g. busy) are final
            this.socket.destroy();
            this.socket = null;
            return this.call(request, false);
        }
    }

    exchange(request) {
        return new Promise((resolve, reject) => {
            const socket = this.socket, chunks = [];
            const done = (err, value) => {
                socket.off('data', onData).off('end', onEnd).off('error', onError);
                err ? reject(err) : resolve(value);
            };
            const onData = (chunk) => {
                chunks.push(chunk);
                if (chunk[chunk.length - 1] !== 0x7d) return;   // last byte not '}': more to come
                let resp;
                try { resp = JSON.parse(Buffer.concat(chunks).toString('utf8')); }
                catch { return; }                               // incomplete: wait for more data
                if (resp.error) done(Object.assign(new Error(`${resp.error.code}: ${resp.error.message}`),
                                                   { warpError: resp.error.code }));
                else done(null, resp);
            };
            const onEnd = () => done(new Error('WARP closed the connection'));
            const onError = (e) => done(e);
            socket.on('data', onData).once('end', onEnd).once('error', onError);
            socket.write(JSON.stringify(request));
        });
    }

    close() { if (this.socket) this.socket.end(); }
}

(async () => {
    const warp = new WarpClient();
    try {
        console.log(await warp.call({ op: 'Ping' }));

        // Most visited pages of the last few days
        const top = await warp.call({ op: 'TopEntities', type: 'url', limit: 10, horizon: 'short' });
        for (const e of top.results) console.log(e.score.toFixed(2), e.title || e.display_name);

        // Change feed
        let since = 0;                                          // persist between runs
        for (;;) {
            const ch = await warp.call({ op: 'GetChanges', since, limit: 1000 });
            if (ch.reset) { since = 0; continue; }              // drop the local copy, resync
            console.log(`${ch.deletes.length} deletes, ${ch.upserts.length} upserts`);
            since = ch.next;
            if (!ch.more) break;
        }

        // v1 raw events still work
        const raw = await warp.call({ window: '30m', types: ['app_launch'] });
        for (const e of raw.app_launch_activities.events.slice(0, 5)) console.log(`${e.exe_name} (PID ${e.pid})`);

        // What is the user doing right now?
        const { context } = await warp.call({ op: 'GetCurrentContext' });
        console.log(context.presence, (context.snapshot?.summary || []).join(' / '));
    } catch (err) {
        console.error('WARP request failed:', err.message);
    } finally {
        warp.close();
    }
})();
```

`JSON.parse` turns 64-bit integers into doubles; `paused_until` = INT64_MAX (`GetStatus`,
`Pause`) is not exact in JavaScript -- treat any value above `Number.MAX_SAFE_INTEGER` as
"until resumed".

---

## Best Practices

### 1. Use the Per-Session Pipe and Verify the Server

Compute `\\.\pipe\WarpFileActivityAPI-<SessionId>` from your own session id; use the
legacy name only as a fallback for WARP builds before API v2. Before sending sensitive
requests, [verify the server](#verifying-the-server) with `GetNamedPipeServerProcessId`.

### 2. Read Whole Messages and Check for Errors

Loop on `ERROR_MORE_DATA` (or `IsMessageComplete`) until the response is complete --
never assume it fits a fixed buffer. Check for an `error` member before using a response
and branch on `error.code`, not on `message`.

### 3. Reuse Connections, but Be Ready to Reconnect

Send bursts of requests over one connection (one at a time, up to 256). Every open
connection holds one of the 8 server workers, so WARP closes it after 10 s without a
request and right after a response whenever other clients are waiting. When a write or
read fails because the connection was closed, reconnect and resend (all operations except
`Purge`, `Pause` and `Resume` are read-only). A `busy` error means the server is
saturated: back off and retry.

### 4. Sync Incrementally with `GetChanges` and `WaitForChanges`

Do not poll raw events or re-read all entities. Persist the `next` cursor of
[`GetChanges`](#getchanges), call again while `more` is `true`, then block in
`WaitForChanges` (honouring `retry_after_ms`). When `reset` is `true`, discard your copy
and resync from `since: 0`. Apply `deletes` before `upserts`.

### 5. Rank with Entities, Not Raw Events

For "recent", "frequent" or "relevant" lists use `TopEntities`, `ScoreEntities` (with
`"context": "current"` for task relevance) or `SearchActivity`. Their scores are computed
at query time from decayed engagement and are comparable across files, apps and pages.
Aggregating raw events yourself is slower, limited to the 30-day raw retention and does
not follow renames.

### 6. Request Only What You Need

Use `types` to limit raw and timeline queries to the event types you use, keep time
ranges short, set `limit`, and turn off `reasons`, `include_vector` and `members` when you
do not need them. Filter by action, path, exe or browser on the client: the API has no
server-side filters beyond event type, time and confidence.

### 7. Page with Cursors

When a raw query reports `truncated: true`, pass its `next_before_id` back as `before_id`
(per type, as an object, when several types are truncated). For a fixed time range use
`QueryTimeline` and follow `next_cursor` (restricting `types` to its keys) until it is
`null`.

### 8. Join on Entity Keys

Use `entity_key` (and `doc_key`) to join events, entities, threads and context snapshots
and to compare paths case-insensitively. Keys WARP returns can be passed back as they are
to `QueryInferences`, `ScoreEntities`, `GetRelated` and `Purge`; original paths, URLs and
AUMIDs work too (see [Entity Keys and Normalization](#entity-keys-and-normalization)).

### 9. Treat Timestamps as UTC Seconds and Numbers as 64-bit

All timestamps are Unix seconds in UTC; convert to local time only for display. Ids,
sequence numbers and `paused_until` are 64-bit integers; in JavaScript only values up to
2^53 are exact (in practice this affects `paused_until` = INT64_MAX).

### 10. Poll Context at Its Cadence

The context is recomposed at most every `ContextCadenceSeconds` (default 60 s): polling
`GetCurrentContext` or `GetRecentContext` faster returns the same snapshot. A
`confidence` above ~0.7 indicates a clear focus pattern; lower values suggest the user is
switching between many apps. Use `GetRecentContexts` to see how the context evolved.

### 11. Handle Unavailability Gracefully

WARP may not run in the user's session, capture may be paused (`GetStatus.paused`), and
policy may restrict your client (`unauthorized`, `forbidden`). Degrade gracefully instead
of failing.

---

## Troubleshooting

| Problem | Cause | Solution |
|---|---|---|
| `CreateFile` fails with error 2 (`ERROR_FILE_NOT_FOUND`) | WARP is not running in **this** session, or the pipe name uses the wrong session id | Start WARP; build the name from `ProcessIdToSessionId` of your own process. Services in session 0 have no WARP. |
| Error 231 (`ERROR_PIPE_BUSY`) | All instances are momentarily in use | `WaitNamedPipeW`, then retry. |
| Error 5 (`ERROR_ACCESS_DENIED`) | The client runs as another user, at low integrity or in an AppContainer (sandboxed / packaged app), or connects remotely | Run as the interactive user at medium integrity. For AppContainer or low-integrity clients an administrator must set the policy `ApiAllowAppContainerClients` = 1 and restart WARP. |
| `wrong_session` | The legacy pipe name was used from another Windows session | Connect to the per-session name given in the error message. |
| `unauthorized` | The policy `ApiAllowedClients` is set and does not list your exe | Ask the administrator to add your exe file name. |
| `forbidden` on activity operations | `ApiRestrictActivityScope` = 1 and your exe is not in `ApiActivityClients` | Ask for your exe to be added, or use `Ping`, `GetStatus`, `QueryInferences`, `ScoreEntities` and `WaitForChanges`, which need no scope. |
| `forbidden` on `Purge` / `Pause` / `Resume` | Control operations are reserved for WARP's own executable | Use WARP's tray or command line (`WARP!.exe --pause`, `--resume`, `--purge-hours`, `--purge-domain`, `--purge-path`). |
| `unavailable` for every operation except `Ping` / `GetStatus` (`Ping` reports `store_open: false`) | WARP's activity store did not open | Check `GetStatus` (`store.open`, `store.integrity`) and `%LOCALAPPDATA%\WARP\logs\warp.log`, then restart WARP. |
| `unavailable` for context or history operations | Context inference or the history engine is not running | Check `GetStatus`; retry shortly after WARP starts. |
| `busy` | Server saturated: 8 busy workers and 64 queued connections | Back off (e.g. 100 ms, doubling) and retry on a new connection. |
| A write or read fails with error 232, 233 or 109 after earlier requests succeeded | WARP closed the kept-alive connection: other clients were waiting, more than 10 s idle, or 256 requests on the connection | Reconnect and resend the request. |
| Connection closed before the first response | The first request was not sent within 5 s | Send the request right after connecting. |
| `ReadFile` fails with error 234 (`ERROR_MORE_DATA`) | Normal: the response is larger than your buffer | Keep reading until `ReadFile` succeeds. |
| A byte-mode client (e.g. Node.js) stalls on a request | It waits for the connection to end instead of the end of the JSON message | Detect the end of the response by parsing (see [Node.js](#nodejs)). |
| `truncated: true` | More events than `limit` | Pass `next_before_id` back as `before_id`, raise `limit`, or page a fixed range with `QueryTimeline` cursors. |
| `reset: true` from `GetChanges` / `GetInferenceDeltas` | Your cursor predates a clear-all or pruned deletes | Drop the local copy and resync from 0. |
| `WaitForChanges` returns at once with `retry_after_ms` | 4 long-polls are already pending | Wait `retry_after_ms`, then poll again. |
| Events are missing | The capture source is disabled or not running, capture is paused, or the app, title, domain or folder is excluded | Check `GetStatus` (`capture`, `paused`); the effective settings, including exclusions, are shown by `WARP!.exe --status`. Private browsing windows are never recorded. |
| Empty context `summary` | No usable activity in the window, or the first compose has not finished | Wait for the next cadence (default 60 s). |
| Several file events for one save | A single user action can produce events from several sources (`source`) | Group by `entity_key` + `action` within a short time window, or rank with entities instead. |

---

## Limitations

1. **Local, per user, per session** -- only processes of the interactive user (plus
   SYSTEM and elevated administrators) on the same machine can connect, and every Windows
   session has its own WARP instance and data.
2. **No server-side filtering by path, action, app or browser** -- requests filter by
   event type, time and confidence only; filter on the client, or rank with entities.
3. **No push notifications** -- `WaitForChanges` long-polls for at most 25 s, with at most
   4 concurrent long-polls server-wide.
4. **Retention** -- raw events are kept for 30 days and entities / history for 395 days by
   default; deletes stay in the change feed for 90 days.
5. **Response size** -- the transport does not cap responses, but row limits do (up to
   50000 events per type for raw queries and 20000 per type and page for
   `QueryTimeline`); large responses cost memory and time on both sides.
6. **Sliding raw window** -- the v1 raw query's window is relative to the time of each call,
   so `before_id` pages of a long listing see a slightly later window; page a fixed range
   with `QueryTimeline`.
7. **1-second timestamp granularity** -- events within the same second share a
   `timestamp`; order by `id` within a type.
8. **Coarse scopes** -- the activity scope is all-or-nothing. `GetStatus` reserves paths,
   log messages, capture error texts, media / microphone / camera flags and settings for
   WARP itself; its health summary goes to every client that may connect.
9. **Heuristic context** -- summaries, roles and work threads are inferred from window
   titles and usage patterns and can be wrong.
10. **Capture coverage** -- browsing is captured from supported browsers through UI
    Automation; private windows and excluded apps, titles, domains and folders are not
    recorded; temporary, lock and partial-download files never become entities.

---

## Schema Reference

The API is the supported interface; the database schema may change between versions. For
read-only analysis, the database (`%LOCALAPPDATA%\WARP\activity.db`,
`PRAGMA user_version` = 2) contains the tables below. Every activity table also carries
the event-context columns `source_pid`, `source_exe`, `foreground_pid`, `foreground_exe`,
`ms_since_input`, `parent_pid`, `parent_exe`, `created_window_ms` and `confidence`
(REAL, default 1.0).

| Table | Purpose | Columns |
|---|---|---|
| `file_activity` | File events | `id`, `timestamp`, `action`, `path`, `old_path`, `entity_key`, `event_ms`, `source`, `presence`, `is_dir`, `origin_url` |
| `app_launch_activity` | Process starts | `id`, `timestamp`, `exe_name`, `exe_path`, `pid`, `entity_key`, `event_ms`, `source`, `creator_pid`, `creator_exe`, `aumid`, `session_id`, `exit_ts`, `lifetime_secs` |
| `browsing_activity` | Pages shown | `id`, `timestamp`, `browser`, `title`, `url`, `entity_key`, `event_ms`, `source`, `host`, `doc_id`, `doc_kind`, `dwell_secs`, `presence`, `window_id` |
| `app_focus_activity` | Foreground sessions | `id`, `timestamp` (session end), `exe_name`, `exe_path`, `window_title`, `duration_secs`, `entity_key`, `event_ms`, `source`, `start_ts`, `end_ts`, `active_secs`, `idle_secs`, `doc_key`, `doc_display`, `doc_type`, `window_class`, `desktop_id`, `aumid`, `presence` |
| `inference` | One row per entity | `entity_key` (primary key), `entity_type`, `display_name`, `title`, `doc_kind`, `file_id`, `last_event_ts`, `last_open_ts`, `last_edit_ts`, `last_active_ts`, `open_count_total`, `edit_count_total`, `active_secs_total`, `distinct_days`, `last_day`, `eng_s`, `eng_s_ts`, `eng_s_key`, `eng_l`, `eng_l_ts`, `eng_l_key`, `change_seq`, `deleted`, `updated_at`, and the v1 columns `open_count_7d`, `open_count_30d`, `recency_score`, `version` |
| `entity_day` | Daily rollups per entity and local day | `entity_key`, `day`, `opens`, `edits`, `active_secs`, `sessions` |
| `tombstone` | Deletes in the change feed | `change_seq` (primary key), `entity_key`, `entity_type`, `ts`, `reason` |
| `meta` | Key / value state | `key`, `value` (e.g. `reset_seq`, `change_seq_hw`, `tombstone_floor_seq`, `purged_ranges`, `fts_available`, `schema_v2_migrated_at`) |
| `presence_interval` | Presence history | `id`, `start_ts`, `end_ts`, `state` |
| `context_snapshot` | Persisted context snapshots | `id`, `ts`, `window_secs`, `json`, `thread_id`, `embedding`, `model` |
| `edge` | Entity co-occurrence | `a`, `b`, `weight`, `last_ts` |
| `work_session` | Work sessions | `id`, `start_ts`, `end_ts`, `active_secs`, `passive_secs` |
| `work_thread` | Work threads | `id`, `label`, `first_ts`, `last_ts`, `active_secs`, `project_key`, `centroid`, `change_seq` |
| `thread_member` | Members of a thread | `thread_id`, `entity_key`, `weight`, `last_ts` |
| `thread_span` | Activity periods of a thread | `thread_id`, `start_ts`, `end_ts`, `active_secs` |
| `clipboard_activity` | Clipboard metadata (never content; off by default) | `id`, `timestamp`, `event_ms`, `owner_exe`, `formats` |
| `print_activity` | Print jobs | `id`, `timestamp`, `event_ms`, `document`, `printer`, `pages` |
| `activity_fts` | Optional FTS5 index of entity names and titles (`rowid` = `inference.rowid`) | `kind`, `ref`, `ts`, `text` |

- In `inference`, `version` equals `change_seq`; `open_count_7d` and `open_count_30d` are
  no longer maintained and `recency_score` is a snapshot taken at write time -- the API
  computes all three at query time from `entity_day`. `eng_s` / `eng_l` are the decayed
  accumulators at `eng_s_ts` / `eng_l_ts` (see [How Scores Work](#how-scores-work)) and
  `eng_*_key` their time-independent ranking keys. Tombstoned entities are removed.
- `entity_day.day` is the local day index (days since 1970-01-01 in local time).

**Pragmas:** `journal_mode=WAL`, `synchronous=NORMAL`, `secure_delete=ON`.

> **Warning:** The database is actively written to by WARP. If you open it directly,
> open it read-only (or with `PRAGMA query_only=ON`) to avoid interfering with WARP's
> writer. The named-pipe API is the recommended access method.

---

*This documentation describes API version 2 (`"api_version": 2`), served by WARP 6.0. The
change notes below for v5.17 and earlier refer to the context summarizer and the v1 API
of those releases.*

*Changes for API v2 (WARP 6.0, `ApiServer` replaces `QueryApi`):*
- ***Transport and security.*** Per-session pipe `\\.\pipe\WarpFileActivityAPI-<SessionId>`;
  the legacy name is served to same-session clients only (others get `wrong_session`).
  Explicit pipe DACL with a medium integrity label, remote clients rejected, caller
  identification with the `ApiAllowedClients`, `ApiRestrictActivityScope` /
  `ApiActivityClients` and `ApiAllowAppContainerClients` policies; control operations are
  reserved for WARP itself.
- ***Protocol.*** `api_version` on every response; structured errors instead of silent
  fallbacks (malformed JSON, unknown ops and unknown windows no longer return raw events);
  requests up to 1 MiB; responses of any size (clients must loop on `ERROR_MORE_DATA`);
  keep-alive connections that the server closes after a response when other clients wait
  (clients reconnect); bounded worker pool and queue (`busy` when full) and deadlines.
  `Ping` (with `store_open`) and `GetStatus` answer even when the store is not open.
- ***New operations.*** `ScoreEntities`, `TopEntities`, `SearchActivity`, `GetChanges`,
  `WaitForChanges`, `QueryTimeline`, `GetWorkThreads`, `GetRelated`, `GetCurrentContext`,
  `Ping`, `GetStatus`, `Purge`, `Pause` and `Resume`.
- ***v1 operations.*** Same request and response shapes, plus: raw queries gain
  `app_focus`, `limit`, `min_confidence`, `truncated` / `next_before_id` with `before_id`
  paging, and per-event `confidence`, `source` and `entity_key`; `QueryInferences` accepts
  `keys` and the fields `entity_key`, `score`, `last_active_ts` and `active_secs_7d`;
  `GetInferenceDeltas` takes `limit` and returns `deleted`, `next_version`,
  `current_version`, `more` and `reset`; the inference `version` is now the global
  `change_seq`. Context snapshots gain `thread_id`, `presence`, `current_entities[]` and
  `items[].entity_key`; `GetRecentContexts` also returns persisted history
  (`from_persisted`), and `include_vector` (default `true`) controls `summary_embedding`.

*Changes from v5.16:*
- ***Canonical summary is now a rich deterministic listing, not the
  LLM output.***  Through v5.16 the Qwen3-0.6B "brain" generated the
  user-facing `summary` (overwriting the algorithmic output).  An A/B
  harness over 12 realistic scenarios -- running the actual Qwen3-0.6B
  + granite encoder locally and scoring against human-authored gold
  summaries with granite cosine + ROUGE-L + token coverage -- showed
  this was a net loss:
    - The 0.6B model's output was **rejected by the grounding gate
      75-90% of the time** (it copies the few-shot examples or trips
      the hallucination check), so the user was usually shown the
      terse 1-token cluster theme (e.g. *"Working on Auth
      Middleware"*) -- the "average and generic" summaries.
    - When the LLM output *was* accepted it was, on average, **less
      grounded** in the actual titles than a plain listing (granite
      cos-to-titles 0.906 vs 0.920), because the 0.6B model drops
      activity breadth.
    - A 0.6B model fundamentally **cannot abstract** diverse titles
      into a higher-level intent (*"auth_middleware.cpp + login.ts +
      session.go"* -> *"authentication system"*); all it can do is
      rephrase/list -- less completely than a deterministic listing.
  The new `ComposeRichListing` composes
  *"User is working on auth middleware, login handler and session
  store in Visual Studio Code"* from the top distinct cleaned title
  phrases.  Versus the prior summary it scored: granite cos-to-gold
  **+0.016 (8 wins / 3 losses)**, cos-to-titles **+0.049**, ROUGE-L
  **+0.205 (+80%)**, specificity **2x**, zero new hallucinations --
  at **zero inference cost** and **full determinism**.  It also
  *matches* an expensive best-of-N LLM on semantic fidelity.
- ***The Qwen3 LLM is retained as a gated refiner, not removed.***
  When loaded, its prose may still replace the listing -- but only
  when it is at least as grounded in the window titles as the listing
  (granite cosine, with a small tolerance).  So it can only help
  (cleaner prose for a single-topic facet), never regress below the
  much-better listing floor.  No new CPU cost: the LLM already ran in
  v5.16; v5.17 adds a rich-listing floor + a groundedness check
  (3 cheap granite embeds per facet).
- ***No JSON shape change.***  `summary` / `summary_files` /
  `summary_websites` / `summary_apps` / `summary_embedding` /
  `model_polish` are unchanged on the wire; only the *content* of the
  summary fields improves.  `model_polish` still reports `"qwen3-0.6b"`
  vs `"(not loaded)"`.

*Changes from v5.15:*
- ***Removed BGE-small and MiniLM as supported sentence-encoder
  fallbacks.***  The production stack has been granite + qwen
  exclusively since v5.11, but the BGE-small and MiniLM fallback code
  paths were still in `ContextInference::Init()` / `Embed()` and the
  build pipeline was still downloading + bundling
  `models/bge-small.onnx` + `models/vocab.txt` into every shipped
  artefact (~130 MB).  None of this contributed to a customer-visible
  configuration, but it inflated the test surface (the v5.14
  fallback matrix had 8 rows), the shipped artefact size, and the
  CI download time.
- ***What was removed.***
    - `BertTokenizer.h` header (WordPiece tokenizer used only by the
      BGE / MiniLM paths).
    - `ContextInference::m_tokenizer` (BertTokenizer member) and
      `ContextInference::m_useGranite` (now always implicitly true
      when `m_modelReady` is true).
    - The Path-2 / Path-3 fallback branches in `Init()` -- it now
      tries granite only.
    - The BGE / MiniLM branch in `Embed()` (token-type-ids input,
      manual mean-pool over `last_hidden_state`).
    - The BGE-small download step in `.github/workflows/build.yml`
      and the corresponding staging copy.
    - The `bge-small.onnx` / `minilm.onnx` / `vocab.txt` post-build
      copy targets in `WARP!.vcxproj`.
- ***What didn't change (deliberately).***
    - `model` field on snapshots still uses the same value strings;
      with this change the only valid values are
      `"granite-embedding-small-english-r2"` and `"deterministic"`.
    - When granite is absent the engine still degrades gracefully
      to deterministic mode (no semantic clustering, no
      `summary_embedding`) -- identical to the v5.15
      "no model file at all" behaviour.
    - `summary_embedding` is still emitted in the same format when
      granite IS loaded; nothing on the wire changes for normal
      operation.
- ***Effect on consumers.***  None for the granite-loaded path.  The
  only observable change is that snapshots no longer report
  `"bge-small-en-v1.5"` or `"all-MiniLM-L6-v2"` in the `model`
  field, and the shipped zip is ~130 MB smaller (the BGE files were
  the only sub-100-MB-per-platform component still in the bundle
  after the v5.12 reorg).

*Changes from v5.14:*
- ***Fixed a recurring hallucination where the LLM-as-brain layer
  would copy a few-shot example out of the system prompt verbatim
  whenever the input loosely matched the example's app set.***  Most
  visible failure mode: the model emitted
  `"User is reading about indexer reliability across their emails
  and chats (Outlook, Microsoft Teams)."` whenever the user had
  Outlook + Teams open, regardless of what was actually in the
  window titles.  This was the literal text of the few-shot
  example, sometimes with a stray trailing `)` appended.
- ***Root cause.***  The old grounding gate required only *at least
  one* >= 4-char token from the LLM output to appear in the input.
  For the indexer-reliability leak, the example mentioned the same
  app names ("outlook", "teams") that the user actually had open,
  so the leak was technically partially grounded -- enough to pass
  the loose gate, even though the topic tokens ("indexer",
  "reliability") were pure invention.
- ***Fix (two-pronged).***
    - Tightened the post-processing grounding gate in `Polish()`:
      every >= 4-char *topic-bearing* token in the LLM output must
      appear in the input items (app names, cleaned title, raw
      title) or in the algorithmic topic hint.  Topic-bearing means
      "not a discourse marker, generic verb, app-type noun, or
      function word" -- those are weeded out via an explicit
      `DiscourseStops()` set so the gate doesn't reject legitimate
      connective text.  Fuzzy prefix/suffix match catches compound
      tokens (e.g. `m365copilot` in the output matches `m365` +
      `copilot` in input).
    - Replaced the three concrete few-shot examples in the system
      prompt with abstract placeholders (Q3 budget / customer
      onboarding / team announcements) that are less domain-y and
      explicitly labelled "do NOT copy their topic words; the
      topic must come from the actual window titles you are
      given".  Both layers (less-sticky examples + post-processing
      gate) work together so even if the model still attempts a
      verbatim copy, the gate drops the line.
- ***Behavioural effect.***  Hallucinated lines are silently dropped
  during post-processing.  If every generated line is rejected,
  `Polish()` returns empty and the snapshot falls back to the
  algorithmic cluster->theme summary (still correct, just less
  natural).  False-positive rejections of legitimate phrasings are
  the explicit design trade-off: showing the user invented content
  is worse than showing them slightly-less-polished prose.
- ***Pinned via test fixture.***  `scripts/test_hallucination_guard.py`
  is a pure-Python port of the C++ guard that locks down 8 reject
  cases (the original bug plus 7 related leak patterns) and 7
  accept cases (legitimate rewrites, compound-word matching, etc.).
  Run it before changing `DiscourseStops()` or `IsHallucination()`
  in `LlmSummarizer.cpp`.

*Changes from v5.13:*
- ***Dynamic context inference now builds on top of the
  confidence-weighted per-entity InferenceEngine instead of recomputing
  recency / popularity from raw activity tables.***  Every snapshot
  compose now does a single batch lookup against the inference store
  (keyed by lowercased file path, exe path, or URL/title) and
  multiplicatively boosts each in-window entry's effective focus
  seconds by:

      boost = 1 + log1p(recency_score / 50) + log1p(open_count_7d / 5)

  Range: 1.0x (unknown entity) .. ~5.3x (high-recency, frequently-opened
  entity).  The boost only changes ranking when in-window focus seconds
  are comparable; a 10-minute focused session at 1.0x still wins over
  a 5-minute session at 5.3x.
- ***Why this is a strict win:***
    - (a) **Consistency.**  The dynamic context summary and the
      per-entity inference now agree on which entities matter.  A file
      with recency_score=200 and open_count_7d=20 in the inference store
      will no longer tie or lose to a similarly-focused but unknown
      entity in the per-snapshot ranking.
    - (b) **No wheel reinvention.**  The InferenceEngine already
      maintains the exponential-decay recency score (TAU=48h) and
      rolling open counts incrementally on every raw event with
      confidence weighting applied at capture time.  Snapshots now
      consume those precomputed weights instead of recomputing their
      own popularity signal from `lastSeenTs` / visit count.
    - (c) **Per-event confidence weighting flows through.**  A 30 s
      focus session that the noise filter scored confidence=0.3
      contributes less to open_count_7d than a confidence=1.0 session
      of the same length.  By boosting via open_count_7d, the dynamic
      context inference now inherits that confidence weighting without
      having to re-read the per-event `confidence` column.
- ***User-facing fields unchanged.***  `focus_seconds`,
  `dominant_focus_pct`, and `items[].pct` continue to report **raw**
  in-window dwell time -- the boost is an internal ranking signal that
  never leaks into a user-visible percentage.  Otherwise "100% of
  focus" would lose its meaning when a single boosted entry crossed
  the raw window total.
- ***Graceful degradation.***  When the InferenceEngine is not wired
  (unit tests, custom embedders), `weightedFocusSecs` falls back to
  `totalFocusSecs` and the ranking is identical to pre-v5.14
  behaviour.

*Changes from v5.12:*
- ***Removed the `summary_polished` field (and the per-facet
  `summary_files_polished` / `summary_websites_polished` /
  `summary_apps_polished` variants) from the snapshot JSON.***  Under
  the v5.12 "LLM-as-brain" architecture these fields were just
  backward-compat aliases that always carried the same content as
  `summary` / `summary_files` / `summary_websites` / `summary_apps`,
  doubling the JSON payload for no information gain.  Consumers
  should now read the canonical `summary*` fields directly.  The
  `model_polish` string field is retained (still reports
  `"qwen3-0.6b"` or `"(not loaded)"`) so callers can still tell
  which pipeline produced the summary.
- ***No semantic change.***  The summary content, the cluster->theme
  topic-hint feeder, the LLM-as-brain rewrite step, and the granite
  `summary_embedding` translator step all behave exactly as in v5.12 --
  only the redundant alias fields were dropped from the wire format.

*Changes from v5.11:*
- ***New architecture: LLM as brain, granite as translator.***  The
  cluster->theme algorithmic pipeline now runs as an INTERNAL topic
  hint feeder for the Qwen3-0.6B LLM rather than as the user-facing
  summary source.  When the LLM is loaded, the four summary fields
  (`summary` + per-facet `summary_files` / `summary_websites` /
  `summary_apps`) are GENERATED DIRECTLY BY THE LLM from the raw item
  titles, with the algorithmic cluster->theme output passed as a
  topic hint to keep grounding tight.  When the LLM isn't loaded,
  `summary` falls back to the algorithmic cluster->theme output
  unchanged -- graceful degradation.
  *(In v5.12 these fields were mirrored into legacy `*_polished`
  aliases; those aliases were removed in v5.13 -- see above.)*
- ***New field: `summary_embedding` (float[384]).***  After the
  canonical summary is finalized, the granite encoder produces a
  384-dim L2-normalized vector representation of the joined summary
  lines.  Suitable for similarity search / clustering across
  snapshots in downstream consumers.  Always present in the response
  (empty array when the granite model isn't loaded or the summary is
  empty).  Floats are emitted with 6-decimal precision; total payload
  is ~3-4 KB per snapshot.
- ***Why this swap solved the previous quality complaints:***  earlier
  revisions ran a two-stage pipeline (algorithmic `summary` +
  Qwen3-polished `summary_polished`) where the two stages competed
  for the user's attention -- if the polished version was
  app-listing or hallucinated, the algorithmic version next to it
  highlighted the contrast.  Promoting the LLM output to be the
  canonical `summary` (with the algorithmic output as an internal
  topic hint) eliminates that competition: there is one summary, it
  reads as natural prose, and the topic hint from the cluster
  pipeline keeps it grounded in the actual titles.

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

*New fields may be added to response objects in future versions; ignore members you do not
know. Existing fields keep their meaning within an `api_version`.*

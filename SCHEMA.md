# WARP storage schema v4

The `dev` Phase B update uses **SQLite schema 4**, **API version 2**, and
**identity format 2**. These version numbers describe different contracts.
`ActivityStore::Migrate` / `MigrateV3` / `MigrateV4` are authoritative; consumers should normally
use [API.md](API.md), not write the database directly.

The database remains `%LOCALAPPDATA%\WARP\activity.db`: WAL, one writer,
transactional raw/derived updates, bounded producer queue, three pooled readers.
There are no SQL foreign keys; the writer maintains the logical relationships.

## Phase B: durable evidence

Schema v4 adds five tables without rewriting legacy raw rows:

| Table | Stored contract |
|---|---|
| `observation` | `id` (stable event ID), `kind`, `row_id`, `event_ms`, `received_ms`, JSON `payload`, `change_seq`. Unique `(kind,row_id)`. The envelope commits with the corresponding typed row and derived updates. |
| `activity_episode` | Immutable `(episode_id,revision)` slices: `kind`, millisecond bounds, `entity_key`, `app_key`, `display`, `title`, exposure/active/passive/idle/unknown milliseconds, `state`, supporting `raw_id`, `received_ms`, provenance `payload`. |
| `source_coverage` | `(source,instance,start_ms)` checkpoints with `end_ms`, `state`, `reason`, lost/failed observation count and `received_ms`. No excluded titles, URLs or document content. |
| `artifact_alias` | `(path_key,from_ms)`, exclusive `to_ms` (0 means not closed), `artifact_id`, full-width `physical_id`, `basis`, supporting `evidence_id`. Separate from the v3 URL compatibility aliases. |
| `privacy_scope` | Bounded durable scoped-purge floors: kind, scope hash and time bounds. Prevents old records from reappearing after restart without retaining plaintext scope strings in this table. |

```mermaid
flowchart LR
    Collectors["Collectors: event time + observation context"] --> Typed["Typed raw rows"]
    Collectors --> Provenance["observation: provenance and receipt time"]
    Focus["Focus / presence: incremental slices"] --> Episodes["activity_episode"]
    Web["Page identity + bounded URL rechecks"] --> Episodes
    Health["Collector health and loss counters"] --> Coverage["source_coverage"]
    Typed --> Alias["artifact_alias: valid-time lineage"]
    Typed --> Reader["One SQLite read snapshot"]
    Provenance --> Reader
    Episodes --> Reader
    Coverage --> Reader
    Reader --> Canonical["Canonical selected evidence"]
    Canonical --> Projections["Combined / facets / items / consumer entities"]
    Reader --> EvidenceAPI["GetContextEvidence"]
    Projections --> IntervalAPI["GetIntervalContext"]
```

### Observation envelope

`payload` is the same `provenance` object exposed by raw event queries and
`GetContextEvidence`. It records:

- Stable `event_id`, runtime `source_instance`, `observed_ms`, `received_ms`,
  source `timestamp_kind`, `replayed`, `confirmation`, and rule version.
- `process_instance`, `foreground_instance`, session ID, `actor_method`,
  attribution age and foreground-context sample age. Unknown attribution does
  not become the foreground application at delayed emission time.
- Resolution method, observation/validity times, redaction class, and separately
  qualified source/actor/identity rule weights. Missing weights are JSON null.
  These weights are **not calibrated probabilities**.
- When supplied by the source: full-width file ID, volume, journal ID, USN/reason
  information, originating Office app and canonical mapped cloud URL.
- Episode/window instance IDs, revision, continuation state and boundary reason.
  `support_count` counts known supporting observations, not all subsequently
  suppressed duplicates. `observed_utc_offset_minutes` describes the observation,
  not an inferred historical time zone.

Legacy rows have `available:false` provenance and no invented observation or
receipt timestamps. Read APIs retain them but qualify their evidence/coverage as
partial. A source receipt timestamp is not evidence that a recency-list item was
opened at receipt time. Replayed file events have unknown contemporaneous
presence and actor attribution.
USN IDs are derived from volume/journal/USN/kind; Recent/MRU IDs use the source
identity and source timestamp. They remain stable across process restarts.
Untimed recency entries are labeled `recency_observation`, not fresh opens.
They do not receive fresh engagement credit. File IDs are 32 lowercase hex
digits (high 64 bits followed by low 64 bits; NTFS has a zero high half), scoped
by the volume identity.

### Physical and logical file identity

The physical key uses the volume and both 64-bit halves of a file ID; it does not
use the reader's legacy folded `fileRef`. Path-only evidence is marked
`path_fallback`. A known explicit rename retains its artifact ID and closes the
old path's validity interval. A modification/atomic replacement at the same path
can retain the logical artifact while recording a different physical instance.
A new create/copy is not assumed to be the same artifact. Ambiguous or missing
identity remains a fallback, not a basename join.

Raw paths remain paths-at-observation. `ResolveFileIdentity` exposes the recorded
lineage and its validity intervals; API v2 path keys are not silently replaced by
artifact IDs. Aliases require retained supporting observations. ReFS IDs, copies
across volumes, paths reused for new creates, and safe-save lineage have native
fixtures. This is not a full filesystem inventory or proof of arbitrary moves
that happened outside capture.

### Checkpoints and page episodes

Focus and presence target 30-second incremental checkpoints. Each slice has a
stable episode ID and increasing revision; replaying the same event ID does not
credit engagement twice. Focus samples retain active/passive/idle/unknown
separately. Away/lock/sleep boundaries are **detected transition times**; they do
not retroactively erase already-checkpointed foreground exposure.

Page episodes record foreground exposure, not interaction time. They close on
navigation, loss of foreground, pause/withholding, window destruction and clean
shutdown, even without a successor page. A continuing page is checkpointed.
`open:true` means the slice ended at a continuing checkpoint; consumers must
never extend its end after a crash. A new foreground visit/window gets an opaque
instance; process identity and destruction handling prevent cached identity from
being carried across observed PID/HWND reuse.

Foreground URL rechecks are bounded (approximately 5-7 seconds plus UIA
latency/retry). Same-title URL changes can therefore be detected without reading
typed omnibox text. Unknown private-window status fails closed; incomplete UIA
walks no longer become permission to persist content. Browser-version coverage
still requires real-browser validation. Pending reads are tied to the window
instance and uninterrupted foreground generation; late replies cannot reopen
background/replaced windows or change their privacy verdict. A recorded page
boundary is the verified observation time, not an earlier queued title event
or a claim about the exact browser navigation time.

The 30-second cadence is not a universal crash-loss guarantee: scheduler stalls,
queue pressure, write failures and unavailable sources can lose more. Coverage
and dropped counters expose these limitations; uncheckpointed time is unknown.
Legacy `browsing_activity.dwell_secs` keeps its previous-page meaning for API
compatibility. New consumers use page episodes.

### Consistent interval reads and lifecycle

`ReadEvidence` materializes all raw types, episodes, coverage and a committed
watermark inside one SQLite read transaction, then releases it before model
work. Bounds are `[start_ms,end_ms)`. An `as_of` cutoff excludes observations
received later. Historic queries do not read today's foreground, presence,
current thread or long-horizon engagement score.

Coverage is checkpointed for files, launch, browser, focus, presence and storage.
Disabled, paused, degraded, privacy-withheld and unavailable states are distinct.
Uncovered periods, including offline/crash gaps, remain unknown. `healthy` means
the collector was available, **not complete capture recall**. Truncation and the
new-evidence/retention availability horizon are explicit.

Raw-record deletion triggers remove companion provenance and supported slices.
Retention expires standalone page slices and coverage at the raw-data horizon.
Entity purge follows retained file aliases; time purge conservatively removes
intersecting new slices/aliases/coverage rather than leaving deleted evidence.
Clear-all empties the five new tables and retains the global cleared-time floor.
The newest 64 distinct scoped floors persist as hashes; like the existing 64
merged time floors, this is a bounded replay guard, not a permanent unlimited
denylist. Hashing is not a claim of anonymization. New activity after the purge
time remains eligible. Queries intersecting a retained scoped purge expose
`privacy_redacted:true` and are partial. Legacy raw interval trimming remains in force.

Snapshots persist their actual summary vector in JSON when available, alongside
the `summary_joined_v1` input profile, model/tokenizer file hashes, dimension and
normalization. The separate historical label embedding is not silently exposed
as that summary vector. Old snapshots without a summary vector remain empty.
Do not compare vectors with missing or different compatibility metadata.

## Earlier v2-to-v3 changes retained in v4

| Area | v3 contract |
|---|---|
| Focus | Adds nullable `passive_secs`, `unknown_secs`, and `doc_resolution`. New `active_secs` excludes passive presence. Missing measurement is SQL `NULL`, not zero or wall duration. |
| Session occupancy | Adds `session_attention`, a durable, non-overlapping interval union. `work_session` adds `accounting_version`. Focus and presence cannot independently credit the same wall time. |
| Web identity | URL path/query and opaque document-ID case are preserved. Scheme, authority and provider prefixes are normalized separately. |
| Compatibility | `entity_alias` resolves old folded keys only through an explicit legacy-identity request. `NULL` targets mean ambiguous/unrecoverable, never an arbitrary winner. |
| Context reads | AUMID and attention/resolution metadata survive the focus read projection. Sparse observations have actions and zero measured focus time. Persisted facets use the same projection as memory. |
| Printing | Table unchanged. The collector admits only a matching qualified own-user identity and local client machine, before persistence. |

```mermaid
flowchart LR
    Focus["Focus: exposure + active/passive/idle/unknown"] --> Raw["app_focus_activity"]
    Presence["Presence intervals"] --> PresenceRows["presence_interval"]
    Raw --> Union["session_attention: interval union"]
    PresenceRows --> Union
    Union --> Sessions["work_session: bounded occupancy"]
    Raw --> Context["Context: measured time + qualified observations"]
    Files["File / browser / app observations"] --> Context
    Context --> Snapshots["context_snapshot: all facets"]
    Snapshots --> API["API v2: identical facet projection after restart"]
    Legacy["Explicit identity_version=1 lookup"] --> Aliases["entity_alias"]
    Aliases --> Entities["inference: case-safe identities"]
```

## Raw records

Times named `timestamp`, `start_ts`, `end_ts`, `*_ts` are UTC Unix seconds.
`event_ms` is UTC Unix milliseconds. Focus persists its end in both `timestamp`
and `event_ms`. New focus episode slices retain millisecond start bounds; the
read projection prefers these over the second-precision legacy `start_ts`.
Do not treat integer timestamp precision as exact UI/input timing.

The four core activity tables share the existing `EventContext` fields:

`source_pid`, `source_exe`, `foreground_pid`, `foreground_exe`, `ms_since_input`,
`parent_pid`, `parent_exe`, `created_window_ms`, `confidence`.

These are heuristic attribution, not proof of intent or a calibrated model
probability. Schema v4 stores the additional receipt/replay/process-instance
fields in companion observations, with source availability in coverage rows.

| Table | Persisted fields, in addition to the shared context where applicable |
|---|---|
| `file_activity` | `id`, `timestamp`, `event_ms`, `action`, `path`, `old_path`, `entity_key`, `source`, `presence`, `is_dir`, `origin_url` |
| `app_launch_activity` | `id`, `timestamp`, `event_ms`, `exe_name`, `exe_path`, `pid`, `entity_key`, `source`, `creator_pid`, `creator_exe`, `aumid`, `session_id`, `exit_ts`, `lifetime_secs` |
| `browsing_activity` | `id`, `timestamp`, `event_ms`, `browser`, `title`, `url`, `entity_key`, `source`, `host`, `doc_id`, `doc_kind`, `dwell_secs`, `presence`, `window_id` |
| `app_focus_activity` | `id`, `timestamp`, `event_ms`, `exe_name`, `exe_path`, `window_title`, `duration_secs`, `entity_key`, `source`, `start_ts`, `end_ts`, `active_secs`, `idle_secs`, `passive_secs`, `unknown_secs`, `doc_key`, `doc_display`, `doc_type`, `doc_resolution`, `window_class`, `desktop_id`, `aumid`, `presence` |
| `presence_interval` | `id`, `start_ts`, `end_ts`, `state` |
| `clipboard_activity` | `id`, `timestamp`, `event_ms`, `owner_exe`, `formats` |
| `print_activity` | `id`, `timestamp`, `event_ms`, `document`, `printer`, `pages` |

Browser `dwell_secs` still describes the **previous page** in `window_id`.
Schema v4 adds page episodes but does not claim complete tab/navigation history.
Clipboard metadata does not establish a paste or content-transfer relationship.
Print owner/client names are checked in memory; they are not added to the table.

### Focus attention

| Field | Meaning |
|---|---|
| `duration_secs` | Foreground wall-time exposure, not interaction time. |
| `active_secs` | New capture: recent-input-supported foreground time. SQL `NULL` means unavailable. It is not a count of seconds containing a keystroke. |
| `passive_secs` | New capture: device-wide passive-presence support while input is old/unavailable. Background media does not make these active document seconds. |
| `idle_secs` | New capture: old input without passive-presence support. This does not prove that the user was not reading. |
| `unknown_secs` | Unclassified/unmeasured remainder of foreground exposure. |
| `doc_resolution` | `browser_window`, `filename_candidate`, `ambiguous`, `unresolved`, `withheld`, or migrated `legacy_candidate`. Candidate matching is not a direct app document API. |

New records are bounded so their known components plus unknown time equal the
stored wall duration. Internally `-1` represents missing active measurement and
is written as `NULL`; it is never promoted to wall time.

The read API adds `attention_status`: `measured`, `unknown`, or `legacy_combined`.
In v2, active time included passive time and the passive component was discarded.
For those rows (`passive_secs IS NULL`), a positive old active total becomes
**unknown** in the read/context projection. Measured zero stays zero. The
original aggregate remains in SQL for audit; a split cannot be recovered from it.
Legacy v1 rows without active measurement also remain unknown.

AUMID is retained through the read structs, live foreground overlay and context
aggregation. An unresolved document uses its app identity. Multiple distinct
same-name paths cause abstention; no file is chosen merely because it was first
in a recent-file list. The candidate list is not persisted.

## Attention union and work sessions

```sql
CREATE TABLE session_attention (
    start_ts INTEGER PRIMARY KEY,
    end_ts INTEGER NOT NULL,
    active_rate REAL NOT NULL,
    passive_rate REAL NOT NULL
) WITHOUT ROWID;
CREATE INDEX idx_session_attention_end ON session_attention(end_ts);

-- Added to the existing work_session table:
-- accounting_version INTEGER NOT NULL DEFAULT 0
```

Each slice is a half-open interval with active/passive fractions. The writer
normalizes overlapping inputs into disjoint slices, takes the maximum active
support, and caps passive support to the remaining wall time. It does not sum
independent sensors' votes. Equal adjacent slices are coalesced.

Examples:

| Evidence | Result |
|---|---|
| 600-second focus, zero active, 600 idle | Context active = 0; exposure = 600. |
| Focus and presence both active over the same 600 seconds | Work-session active = 600, not 1,200. |
| 600 passive seconds plus 300 overlapping active seconds | Session active = 300, passive = 300. |
| Two 100-second active intervals separated by a 100-second gap | Session extent = 300; credited attention = 200. |

Mixed segments do not say *where* inside the interval interaction occurred.
Their rates are therefore proportional estimates, not exact attention episodes.
Context clipping uses cumulative floors so adjacent windows conserve measured
component totals. Schema v4 checkpoints bound the size of new slices and retain
millisecond bounds. Attention counters/work-session totals still have
second-level quantization, and mixed-slice estimates are not exact input timing.

`work_session` retains `id`, `start_ts`, `end_ts`, `active_secs`, `passive_secs`,
and adds `accounting_version`:

- `1`: duration derived from the durable union.
- `0`: contains a bounded legacy estimate. Upgrade bounds old totals by wall
  time and preserves that qualification when a new interval joins the session.

Intervals join sessions across gaps shorter than 15 minutes, but never across
purged time. A late bridging interval merges all eligible neighboring sessions.
Repeated or out-of-order presence/focus overlap does not add duplicate occupancy,
including after restart. Session totals returned by `QueryTimeline` describe the
**whole overlapping session**, not a clipped arbitrary-history reconstruction.

## Typed identities and migration aliases

| Identity | Normalization |
|---|---|
| Windows file | API v2 keeps the canonical, invariant-lowercase path. Schema v4 adds qualified physical/logical identities alongside it; case-sensitive NTFS/WSL path semantics are not implemented. |
| Application | AUMID where available, otherwise canonical exe path. AUMIDs remain case-folded. |
| URL | Lowercase scheme/authority; preserve canonical path/query case. Privacy filtering happens first in `UrlCanonicalizer`. |
| Document | Normalize the provider prefix, preserve the provider-normalized payload. The URL canonicalizer still applies provider-specific rules, such as GitHub repository normalization. |

`https://example.org/PlanA` and `/plana` are different; `yt:AbCdEf12345` and
`yt:abcdef12345` are different. Consumers must stop lowercasing arbitrary keys.

```sql
CREATE TABLE entity_alias (
    alias_key TEXT PRIMARY KEY,
    entity_key TEXT,                  -- NULL: do not resolve this old identity
    created_ts INTEGER NOT NULL
) WITHOUT ROWID;
CREATE INDEX idx_entity_alias_target ON entity_alias(entity_key);
```

This is a **v2 case-correction compatibility map**, separate from schema v4's
`artifact_alias` file rename/physical/logical lineage.

Default lookups use identity format 2 and never consult aliases. In
`ScoreEntities` / `QueryInferences`, `identity_version: 1` explicitly requests old
folded identities. Known unique aliases resolve to a canonical key; ambiguous or
expired web identities return not found. File/app identities remain compatible.
If another case variant is later observed, its old alias is marked ambiguous,
not silently reassigned. Purge uses canonical identity-format-2 keys.

### Upgrade behavior

Ordered migrations run transactionally:

1. Existing v0/v1 data receives the v2 layout without inventing missing active
   duration.
2. v3 adds the attention/resolution columns and derived tables.
3. Original URL/doc-display evidence rekeys retained browser and URL-focus rows.
   Unique entity identities rekey their rollups, memberships, edges and snapshots.
4. Case-collided or merged web aggregates are **not divided or duplicated by
   guesswork**: the affected aggregates are rebuilt from retained browser
   observations using the real engagement implementation. Older unrecoverable
   aggregate contributions are discarded; raw observations are retained. Affected
   thread labels/centroids are cleared rather than preserving unsupported links.
5. FTS is rebuilt; persisted snapshot entity references are updated. Old
   snapshots retain their historical prose/time estimates and are marked
   `attention_semantics: "legacy_combined"`, not presented as newly measured time.
6. Existing consumers receive a change-feed reset where affected history exists.
   They must discard their old key cache and resync. Surviving feed records are
   resequenced after the reset epoch so multi-page bootstrap cannot get stuck in
   repeated resets, including after old tombstones are pruned.
7. v4 adds observation, episode, source-coverage, artifact-alias and scoped-privacy
   tables without fabricating missing historical provenance or checkpoints.

Old entity engagement totals may still contain legacy combined active/passive
contributions when their identity did not need reconstruction. Phase A does not
pretend those historical inputs can be separated; new contributions and current
context use the corrected attention contract.

## Other derived tables

| Table | Purpose |
|---|---|
| `inference` | Per-entity engagement, timestamps, display/title, decay accumulators, change sequence. `doc_kind`/`file_id` remain reserved in this table. |
| `entity_day` | Local-day opens, edits, active seconds and session counts. |
| `activity_fts` | Names/titles, not a local file-content index. |
| `tombstone` | Deleted keys and change-feed sequence. |
| `context_snapshot` | Generation time, window width, JSON containing all facets, thread id, optional label embedding and model. |
| `work_thread`, `thread_member`, `thread_span` | Existing resumable task heuristics and time spans. |
| `edge` | Existing snapshot co-use heuristic, not verified causal/temporal overlap. |
| `meta` | Cursors, change/reset high watermarks, purge time floors, migration/FTS metadata. |

## Retention and deletion

Raw events and snapshots retain the existing 30-day default. Derived sessions,
`session_attention` and other historical structures use the 395-day default.
Aliases expire at that horizon or when their resolved target disappears.
Tombstones retain their existing 90-day feed horizon.

Time purge trims the attention union before re-deriving surviving session
totals. The existing late-record floors prevent deleted time from being
reintroduced. Entity purge removes matching aliases and uses **case-sensitive,
parsed JSON identity matching** for snapshots, not a lowercased substring.
References inside prose are removed conservatively as well, without folding
case-sensitive web/document identities. Scoped item purges retain anonymous session occupancy, as before; whole/time
purges remove the corresponding occupancy. Clear-all empties the v3 and v4 derived evidence tables.

## Public projections and validation

See [API.md](API.md) for nullable attention fields, observed actions, resolution
status and persisted facet behavior. See
[tests/SIGNAL_QUALITY.md](tests/SIGNAL_QUALITY.md) for on-demand native replay,
metric definitions, labeled external observations, and the distinction between
Phase A/B invariants and still-unmeasured live/SLM/search quality.

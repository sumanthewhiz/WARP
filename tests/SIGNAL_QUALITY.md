# On-demand capture and inference quality validation

## Run it

From a source checkout with Visual Studio C++ tools, Python 3.11+ and the normal
application NuGet packages restored:

```powershell
nuget restore 'WARP!.sln' -PackagesDirectory ..\packages
.\tests\run_signal_quality.ps1

# Reuse a built test binary, or select ARM64.
.\tests\run_signal_quality.ps1 -NoBuild
.\tests\run_signal_quality.ps1 -Platform ARM64
```

The first command is prerequisite setup, not something the wrapper repeats.
The scorer uses Python's standard library; it does not install dependencies.
`tests\requirements.txt` is needed only for the separate pytest metric/unit tests.

The default run:

1. Builds the native `WarpTests` executable with the **real** `ContextInference`,
   store, entity/history engines and capture-rule helpers. The old context link
   stubs are gone.
2. Runs native regressions, including the six original reproduced defects,
   v1/v2 migrations, alias ambiguity, scoped purge and restart.
3. Replays `fixtures\signal_quality.jsonl` twice against isolated temporary
   databases, with a fixed per-case clock and **models disabled**.
4. Writes observations, native results/log, a machine-readable `scorecard.json`
   and a readable `scorecard.md` under the gitignored `eval\results\` directory.
5. Returns **0** for passing Phase B gates (including Phase A), **1** for a failure/invalid input and
   **2** for incomplete required measurements.

No live hooks, browser/print subscriptions, personal activity database, registry
writes or remote judge are used. Synthetic paths do not need to exist.
Temporary fixture databases are removed when the native process finishes.

```mermaid
flowchart LR
    Gold["Frozen inputs + independent expected labels"] --> Replay["Native source rules and real storage/composer"]
    Replay --> Observed["Observed decisions, context, sessions and co-use"]
    Live["Optional labeled live / SLM / search results"] --> Score["Local metric scorer"]
    Observed --> Score
    Gold --> Score
    Score --> Report["JSON + Markdown scorecards"]
    Report --> Gates["Phase A/B invariants / optional measured quality gates"]
    Missing["Missing measurements"] --> NA["Explicit NOT MEASURED, never a pass"]
    NA --> Report
```

## What a pass does and does not establish

The synthetic capture metrics measure **intended policy/admission decisions and
storage**, not Windows ETW/USN/UIA event-generation precision or recall.
In particular, a supplied `private` browser flag tests exclusion handling; it
does not validate real private-window detection. Print fixtures exercise the
qualified-owner/local-client rule, not every printer driver's metadata.

The default inference measurements cover actual deterministic context output:
time, identities, action claims, required entities, output shape and repeat
stability. They are not measurements of Qwen or a future evidence-reasoning SLM.

Correlation diagnostics also exercise the context/history path. Phase C removes
the old blanket bag-co-use edges and adds its own typed/task/search invariants
below. No aggregate "AI quality percentage" is emitted.

The Phase B corpus additionally checks persisted provenance, coverage states,
exclusive-end/as-of membership, final-page exposure and checkpoint/restart
idempotency. Its crash-cut fixture measures the uncheckpointed tail from a
controlled cut point and committed slices; it is **not** a production crash or
backlog guarantee. Native tests exercise the real page/focus checkpoint helpers,
the running focus sampler against a hidden synthetic window, stale browser-read
guards, cross-table snapshot consistency, schema-v3 upgrades, alias lineage and
deletion.

Search quality, trained-SLM claim support, continuous-agent CPU/battery cost,
real crash/backlog loss and real collector accuracy require corresponding labeled
observations/measurements. They appear as **not_measured** otherwise. Empty
denominators are also unmeasured, not perfect scores.

## Optional Granite comparison

Use the existing model directory; the runner never downloads a model and fails
if the explicitly requested encoder/tokenizer cannot load or produce a finite,
normalized 384-dimensional probe embedding:

```powershell
python tests\eval\signal_quality.py `
  --native-bin build\tests\x64\Release\WarpTests.exe `
  --models-dir models `
  --output-dir tests\eval\results\granite
```

This runs the same corrected evidence through the production Granite clustering
path. It **does not enable Qwen** or change registry settings. Use the separate
Phase C command below to evaluate the evidence-linked model interface. Reports record the model mode and
actual SHA-256/size of model/tokenizer inputs. Native binary, corpus and evaluator
hashes, source revision and dirty-worktree status identify the evaluated build.

For narrow debugging:

```powershell
build\tests\x64\Release\WarpTests.exe /filter Context.
build\tests\x64\Release\WarpTests.exe /replay tests\fixtures\signal_quality.jsonl /output build\observations.jsonl
```

## Native corpus format

One JSON object per line. `id` is unique; `group_id` groups related tasks/sessions
for evaluation; `split` is `train`, `tune` or `test`; `cohort` contains optional
query-kind, seen/unseen, language and app labels. The shipped corpus is synthetic
regression material used during development, not independent held-out evidence
or a production search benchmark.

```json
{
  "id": "read-only-open",
  "group_id": "project-alpha",
  "split": "test",
  "as_of": 1760000000,
  "window_seconds": 1800,
  "cohort": {"application": "Word", "scenario": "sparse"},
  "signals": [
    {"id": "f1", "type": "file", "end_offset": -60, "action": "OPEN",
     "source": "recent", "path": "C:\\WarpQuality\\Proposal.pdf"}
  ],
  "expected": {
    "accepted": {"f1": true},
    "resolved_keys": {"f1": "c:\\warpquality\\proposal.pdf"},
    "durations": {"focus_seconds": 0, "foreground_exposure_seconds": 0},
    "allowed_entity_keys": ["c:\\warpquality\\proposal.pdf"],
    "required_entity_keys": ["c:\\warpquality\\proposal.pdf"],
    "allowed_actions": {"c:\\warpquality\\proposal.pdf": ["open"]},
    "forbidden_summary_substrings": ["Editing"]
  }
}
```

Offsets are seconds relative to the fixture's `as_of` clock. This replay clock
is test-only. Separately, Phase B's interval APIs accept a receipt-time `as_of`
cutoff; they cannot reconstruct evidence already removed by retention or purge.

| Signal type | Inputs beyond `id`, `type`, offsets |
|---|---|
| `file` | `path`, `action`, `source`, optional `confidence`; actual path-classification rules run before recording |
| `focus` | `exe_name`, `exe_path`, `title`, `active_secs` (null/missing = unknown), `passive_secs`, `idle_secs`, optional `aumid`, `doc_path`; or `doc_name` + `doc_candidates` to exercise ambiguity |
| `browse` | `url`, `title`, `browser`, optional `private`; actual URL canonicalizer/privacy rules run |
| `launch` | `exe_name`, `exe_path`, optional `aumid`; starts at the post-collector record boundary |
| `presence` | `state`, `start_offset`, `end_offset` |
| `print` | `owner`, `client`, `document`; synthetic own identity is `CONTOSO\Alice` / `alice@contoso.example`, local client `WORKPC` |
| `purge` / `clear` | Test controls, not capture decisions. Purge takes `key`; both call the actual store maintenance implementation. |

`accepted` observations require a persisted row, not just successful queue
submission. `resolved_key` distinguishes a precise document association from
an app-only fallback. The runner does not use expected labels to choose a model
result. New fixtures should include a failing/boundary case, not just another
example of the happy path.

## Labeled live, SLM or search observations

Export **consented, independently labeled** controlled traces from the collector,
model or search application into the same row-per-case protocol, then score:

```powershell
python tests\eval\signal_quality.py `
  --corpus C:\QualityLab\labels.jsonl `
  --observations C:\QualityLab\observations.jsonl `
  --output-dir C:\QualityLab\results `
  --gate-profile measured `
  --require-metric search.ndcg_at_10
```

The scorer does not launch a monitor or obtain private activity to create those
labels. It rejects missing/extra/duplicate case IDs. Every capture decision needs
a label; every emitted model claim needs an independent support judgment.
Missing labeled outputs are errors, not trials silently removed from an average.
Use explicit empty predictions for requests that failed.

Observation envelope: `id`, `mode` (for example `labeled_live`), and the applicable
blocks below. Only blocks with corresponding labels/measurements are scored.

| Block | Observation | Labels (`expected`) |
|---|---|---|
| Capture | `decisions`: `{id,type,accepted,entity_key,resolved_key,action,ambiguous}` | `accepted`, `resolved_keys`, `cross_user_ids`, `excluded_ids` |
| Context | `snapshot`: actual context JSON; `sessions`: `{start,end,active_secs,passive_secs}` | `durations`, `window`, `session_attention_seconds`, `allowed_entity_keys`, `required_entity_keys`, `allowed_actions`, `forbidden_entity_keys`, optional `forbidden_summary_substrings` |
| Correlation | `correlation`: `boundaries` (seconds), `membership` (entity -> task), `co_use` (entity pairs), `task_changes` (`task`,`timestamp`), `abstained` | Same fields under `correlation`, with `abstain` and optional `tolerance_seconds` (default 5) |
| SLM | `model`: `entity_ids`, `evidence_ids`, `claims` (`id`,`kind`: action/relationship/task) | `allowed_entity_keys`, `required_evidence_ids`, `claim_labels` (claim id -> independently judged support boolean) |
| Search | `search`: `ranking` (unique candidate IDs), optional measured `task_success` boolean | `search`: `relevance` (candidate -> grade 0..5), `recall_k` (default 10), optional `exact_match_ids` |
| Lifecycle | `lifecycle`: `visible_keys`, `reset` | `deleted_keys`, `reset_required` |
| Cost/crash | `measurements`: fields below | Measurements come from instrumentation, not generated from expected values |

A search observation example (the labels belong in the corpus, not in this row):

```json
{
  "id": "historical-budget-query",
  "mode": "labeled_live",
  "search": {"ranking": ["budget-2025", "new-unseen-budget", "notes"], "task_success": true},
  "measurements": {"search_api_latency_ms": [12, 14, 19, 37],
                   "timeouts": [false, false, false, false],
                   "fallbacks": [false, false, true, false]}
}
```

This generic export format remains an **evaluation protocol**. The implemented
Phase C task/search API has its own contract in [API.md](../API.md).

## Phase C synthetic search and model evaluation

```powershell
# Builds native code, runs focused safeguards, then evaluates the complete corpus.
.\tests\run_signal_quality.ps1 -PhaseC
.\tests\run_signal_quality.ps1 -PhaseC -Platform ARM64

# The actual pinned dev Qwen model on identical evidence; no registry changes.
.\tests\run_signal_quality.ps1 -PhaseC -NoBuild -WithSlm -ModelsDir .\models

# Development/tuning split only (not a held-out release gate).
python tests\eval\phase_c_quality.py `
  --native-bin build\tests\x64\Release\WarpTests.exe `
  --split tune --output-dir build\phase-c-tune
```

Default results go to `build\phase-c-quality`: frozen replay inputs, native log,
observations and `scorecard.json`. CI runs the deterministic gate on every build;
manual `workflow_dispatch` can additionally request `evaluate_slm`.
Model downloads are a separate step: `scripts\fetch_models.ps1 -RequirePinned`.
The lock preserves the known-good dev model inputs rather than upgrading them.

`fixtures\phase_c_search.jsonl` is **272 synthetic labeled cases** across
16 independent project/session groups: 68 tuning cases and 204 held-out cases.
Groups are disjoint. The 17 scenarios cover ambiguous filenames, content-only
matches, unseen relevant files, exact filenames, task switches/returns,
historical windows, stale/missing context, unrelated queries, downloads,
multilingual metadata, adversarial titles, privacy invalidation, reused paths
and delayed receipt/as-of cutoffs. The generator is deterministic and never
reads user files. Relevance labels identify the synthetic intended target;
they are not chosen from the baseline's winning rank.

The native runner inserts synthetic observations into scratch stores and calls
the actual context/feature API. The predictor never reads `labels`; Python uses
those only for scoring. Frozen candidate lists isolate **reranking**, not
retrieval across a real disk:

| Arm | What runs |
|---|---|
| No context | Stable baseline filename/content match scores. |
| Engagement only | The previous FindMyFile-style `+125 * engagement` policy; a comparison arm, not the new shipping default. |
| Deterministic | Real Phase C features plus the shared production `SearchRanking.h` exact-match/maximum-delta policy. |
| Local SLM | Same evidence/candidates, actual Qwen, strict host validation and deterministic fallback. |

The model subset is selected **before held-out evaluation**: one
ambiguous/task-switch/multilingual/adversarial case per project, stratified across
groups (4 tuning, 12 held-out). This bounds local CPU cost without silently
calling unevaluated cases model successes. Model aggregates use only that paired
subset. This small experiment is not a broad trained-model benchmark.

`eval\phase_c_gates.json` freezes the policy and thresholds: at least +0.03
held-out nDCG@10, a positive 95% paired-project bootstrap lower bound (4,000
resamples), zero exact-match or unseen-recall regressions, zero fallback-order
changes, zero unsupported accepted claims, and task-switch/boundary invariants.
MRR@10, Recall@10 and top-result task success are reported by cohort.
The scorer independently audits accepted model IDs/support/roles in addition
to the native validator. Abstention, malformed output, unavailability and
unrun model cases remain distinct.

Qwen is not enabled by a deterministic pass. The model must additionally improve
paired nDCG@10 by at least +0.02 with a positive lower bound and satisfy the
declared cost budget. A valid abstention is not an improvement. The default stays
deterministic when these conditions are not met.

### Recorded Phase C result

The final local ARM64 replay (272 cases, 204 held-out across 12 project groups)
produced the following **synthetic reranking** result:

| Held-out metric | Baseline | Deterministic context |
|---|---:|---:|
| nDCG@10 | 0.3760 | 0.7556 |
| MRR@10 | 0.2737 | 0.7158 |
| Recall@10 | 0.7255 | 0.8922 |
| Top-result success | 0.1618 | 0.6716 |

Paired-project nDCG gain was 0.3796, with a 95% bootstrap interval of
[0.3504, 0.4039]. Exact-match, unseen-recall, fallback-order, task-boundary,
task-switch, unsupported-accepted-claim, API and candidate-set invariants all
had zero failures. Final native regressions passed on ARM64 and x64 (124 each);
the product built for ARM64, x64 and x86.

The populated-store regression adds 20,500 unsupported ETW OPEN rows alongside
one explicit OPEN, plus an 81-qualified-entity projection case. Search must
retain the supported workspace boost without running the full summary pipeline.
A separate small raw-budget test still requires global fallback when relevant
evidence itself is omitted. Page-checkpoint-only exposure remains usable without
inventing active attention or requiring a navigation in the query window.

The Markdown-prefix regression opens a synthetic Desktop guide containing
"Evals", switches to an unrelated task, and searches `eval`. The direct observed
file receives only the weaker recent-activity boost; unrelated and stale files
remain unboosted. This does not broaden same-directory association.

Consumer integration subsequently added a real, isolated 150 KB pipe-request
regression. It reproduced loss of immediate `ERROR_MORE_DATA` chunks and now
verifies every byte is retained before JSON dispatch. The fixed ARM64 native
suite passes all 125 tests; this transport correction does not change the
recorded ranking features.

The actual pinned Qwen subset ran 16 cases (12 held-out): six accepted
abstentions, no accepted task hypotheses, and no incremental ranking gain.
The deterministic paired subset already scored 1.0, so that subset has a
ceiling effect: it cannot establish a general absence of model value.
Observed model p95 was about 48.8 seconds under concurrent local build load,
not a controlled production benchmark. This experiment provides no basis to
enable `ContextModelEnabled` by default. Keep the corpus and gates fixed when
reproducing this result; new capability experiments need separately labeled data.

API latency is measured in-process on synthetic scratch databases, not over
the pipe or across a populated index. Model latency includes the observed
generation call; cold loading/prefill can exceed the cooperative decode budget.
Background machine load can affect timing. No measured capture precision,
continuous-agent battery impact, real-user relevance or full-disk recall is
implied by a synthetic pass.

## Metrics

The JSON scorecard records value, unit, contributing sample count and
`measured`/`not_measured` for every metric. Ratios pool numerators/denominators;
search scores are per-query macro averages. Zero denominators are not measured.

| Family | Computation |
|---|---|
| Capture | Action precision = correctly admitted / admitted; recall = correctly admitted / expected meaningful/admitted actions in the labeled evaluation scope. |
| Identity | Precision = correct nonempty resolution / nonempty resolutions; coverage = correct / known identities; ambiguity rate = ambiguous decisions / opportunities; false joins = nonempty wrong resolutions, including an unjustified assignment where the label requires abstention. |
| Time | Mean absolute error of labeled component/session totals; positive excess over labeled attention union; mean absolute boundary error in ms; supplied checkpoint crash loss; age of returned snapshot vs `as_of` (not the age of its oldest activity). |
| Correlation | One-to-one boundary F1 within tolerance; pairwise task-membership precision/recall; false co-use pairs; recovery delay and recovery rate after labeled switches; correctness of explicit abstention decisions. |
| Grounding | Supported claims / independently judged emitted claims; invalid entity IDs; unsupported action/relationship rates; structured action-label errors; required entity/evidence recall; output-shape/time-invariant validity; equality of repeated structured outputs. These do not independently judge all nuances of prose. |
| Search | nDCG@10 using `(2^grade - 1)/log2(rank+1)`; MRR@10 at the first positive grade; Recall@K against **all labeled relevant items**, including unseen files; measured task success; missing exact matches or irrelevant results above them. Queries with no labeled relevant items are not measured. |
| Cost | Supplied `capture_cpu_percent`, `model_cpu_ms`, `working_set_bytes`, `battery_mwh`, `package_bytes`, `database_growth_bytes`; interpolated p50/p95 of `search_api_latency_ms`; boolean timeout/fallback rates. Native replay `compose_ms` is diagnostic, not mislabeled as search-API latency or continuous capture CPU. |
| Privacy/lifecycle | Cross-user/excluded decisions admitted; forbidden entity IDs in derived output; deleted IDs still visible; reset behavior vs the labeled expectation. Test every relevant projection in live export, not just one cache. |

### Gates and comparisons

Phase A gates require perfect controlled admission/resolution precision where
labeled, no false joins/invalid IDs/action claims, zero labeled duration/overlap
errors, valid deterministic output and the privacy/deletion/resync invariants.
An unmeasured required invariant makes the run **incomplete**.

The default `--gate-profile phase-b` requires all Phase A invariant families
plus zero labeled provenance/coverage contract errors and zero checkpoint-bound
overrun in the controlled fixture. The measured crash-loss value is reported
separately. `--gate-profile phase-a` remains available for older corpora.
Use `--gate-profile measured` for deliberately partial external evaluations:
only observed invariants are gated, and the report is prominently labeled
**partial evaluation**, not a Phase A pass. An entirely empty evaluation cannot
pass. Output directories must not overwrite input/baseline files; a failed rerun
replaces any previous passing scorecard with a failure record.

Correlation diagnostics and unmeasured future capabilities do not magically
become Phase A successes. `--require-metric <name>` makes a missing chosen metric
an explicit incomplete gate. Measured exact-match overrides and unsupported
model action/relationship claims fail their corresponding gates.

For a fixed held-out search experiment, select thresholds **before** running:

```powershell
python tests\eval\signal_quality.py `
  --corpus C:\QualityLab\labels.jsonl --observations C:\QualityLab\treatment.jsonl `
  --baseline C:\QualityLab\baseline\scorecard.json `
  --gate-profile measured `
  --min-ndcg-gain 0.02 --max-cohort-regression 0.01 `
  --output-dir C:\QualityLab\treatment
```

Those numbers are example experiment thresholds, **not measured WARP gains**.
The corpus hash and paired case/group identities must match. Search cases require
explicit `group_id` and `split`; groups cannot cross train/tune and test. The
comparison uses held-out test groups, macro-averages within each user/task group,
and resamples groups (fixed seed, 2,000 bootstrap draws), not correlated events.
At least two independent groups are required; substantially more are needed for
a credible product decision. The gain gate uses the lower 95% bound. Cohort
deltas cover declared exact/ambiguous/historical, seen/unseen, language and app
segments; the optional cohort gate bounds their observed regression.

Do not claim a new model helps by comparing richer inputs against an impoverished
baseline. Compare current/corrected deterministic/same-evidence model arms, then
one optional enrichment at a time. If the model does not improve the relevant
held-out outcome, retain the deterministic baseline.

## CI

The Windows build job runs the same entry point model-free and with the Granite
bundle already downloaded for packaging, and uploads scorecards, native
results and synthetic observations even when a gate fails. Qwen remains disabled.
Python CI runs the
metric self-tests. Existing live API tests remain separate smoke coverage; they
do not establish full collector recall, SLM quality or end-to-end search gains.

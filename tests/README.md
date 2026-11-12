# WARP test suite

This directory implements the test plan described in
[`/TEST_PLAN.md`](../TEST_PLAN.md). See the one-page summary in
[`/TEST_PLAN_SUMMARY.md`](../TEST_PLAN_SUMMARY.md) for the high-level
framework.

For the implemented Phase B signal-quality contract (including Phase A), run
`.\tests\run_signal_quality.ps1`. It builds the real native composer, runs
regressions and labeled replay twice, and produces JSON/Markdown scorecards.
See [SIGNAL_QUALITY.md](SIGNAL_QUALITY.md) for all metric definitions, optional
Granite runs, labeled live/SLM/search inputs, required-measurement gates and
grouped held-out comparisons. Unmeasured metrics are not reported as successes.

## Quick start

```powershell
# Install Python test deps (one-time, ~5 s)
python -m pip install -r tests/requirements.txt

# Run everything
.\tests\run_tests.ps1

# Or one layer at a time
.\tests\run_tests.ps1 -Layer l1            # deterministic unit tests
.\tests\run_tests.ps1 -Layer l1_property   # Hypothesis property fuzzing
.\tests\run_tests.ps1 -Layer l2            # model-contract + adversarial
.\tests\run_tests.ps1 -Layer l3            # eval corpus + metrics

# List every test pytest would run, without executing
.\tests\run_tests.ps1 -List

# Cross-platform (bash, macOS, Linux dev)
python tests/run_tests.py --layer l1
```

C++ unit tests (real composer, storage, API, identity and capture rules):

```powershell
nuget restore 'WARP!.sln' -PackagesDirectory ..\packages
msbuild tests\WarpTests.vcxproj /p:Configuration=Release /p:Platform=x64
build\tests\x64\Release\WarpTests.exe                 # all tests
build\tests\x64\Release\WarpTests.exe /filter Store.  # one suite
build\tests\x64\Release\WarpTests.exe /list           # list tests
.\tests\run_signal_quality.ps1 -NoBuild               # native regressions + replay + scorecard
```

Live API integration test (ENG-3; needs a running WARP in this session):

```powershell
$env:WARP_LIVE_API = "1"
python -m pytest tests/python/test_live_api.py -m live -v
```

You can also drive `pytest` directly:

```bash
cd tests
python -m pytest -c pytest.ini python/test_hallucination_guard.py -v
python -m pytest -c pytest.ini -m l1_property
python -m pytest -c pytest.ini eval/
```

## What's implemented today (and what's still stubbed)

| TEST_PLAN.md section | Status | Notes |
|---|---|---|
| §2.4 Engagement / ranking math (EntityStore) | ✅ via `python/property/test_inference_engine_props.py` + `WarpTests.Store.cpp` | Decay accumulators, bounded boost and legacy recency score fuzzed against a Python port pinned to C++ by `test_recency_constants_sync.py`; ranking, read-time decay and percentiles exercised on a real store in C++. |
| §2.8 Property-based tests | ✅ partial | Engagement math + grounding gate covered; JSON shape + pipeline state machine still stubbed. |
| §3.3.5 Hallucination guard | ✅ via `python/test_hallucination_guard.py` | 15 example cases + Hypothesis fuzz. |
| §3.3.4 Near-copy gate | ✅ via `python/test_near_copy.py` | 9 cases including all known regressions. |
| §3.3.1 Post-processing sanitizer | ✅ via `python/adversarial/test_prompt_markers.py` | All `<think>` / `<\|im_*\|>` variants covered. |
| Rich-listing composer (v5.17) | ✅ via `python/test_rich_listing.py` | `CleanPhrase` + `ComposeRichListing` Python port, pinned bit-for-bit against the C++ source (validated via standalone g++ build). |
| §3.6 Adversarial fuzz | ✅ via `python/adversarial/test_title_perturbations.py` + `fixtures/adversarial_titles.jsonl` | Pure-Python replay through the post-processing strippers; full end-to-end (with Qwen3) deferred to P3.5. |
| §4.2.1b ROUGE-L | ✅ via `eval/metrics.py` + `eval/test_metrics_self.py` | LCS-based F1; self-tested. |
| §4.2.2 Programmatic token metrics | ✅ via `eval/metrics.py` | must_contain recall, must_not_contain hits, token recall. |
| §4.2.1a Granite cosine | ⏳ TODO | Needs `tests/eval/run_eval.py` that loads the granite ONNX. Deferred to P4 of phased rollout. |
| §4.1 Eval corpus | ✅ seed (5 scenarios) | `eval/corpus.jsonl` + `eval/test_corpus_integrity.py` validate schema + reference satisfies its own assertions. Expand to 50+ in P5. |
| Phase A signal quality | Implemented | `fixtures/signal_quality.jsonl`, `WarpTests.Context.cpp`, `eval/signal_quality.py`: actual capture-rule/storage/context replay, time/identity/action/privacy/lifecycle invariants, correlation diagnostics, and optional labeled model/search/cost metrics. This is separate from the older five-row summary corpus. |
| Phase B durable evidence | Implemented | `WarpTests.Evidence.cpp`: provenance/idempotency, full-width file IDs and alias validity, incremental focus/page slices, restart loss, consistent snapshots, as-of/half-open bounds, coverage, v3 -> v4 migration and deletion. The default scorer requires the new measured contract gates. |
| §2.1 Capture layer | ✅ partial | Pure capture rules in C++ (`WarpTests.Canon.cpp`: transient/download names, anchored exclusions, rename decisions, title grammars, URL canonicalization and document ids); end-to-end capture against a live headless WARP in CI (`python/test_live_api.py`). ⏳ ETW replay fixtures (`.etl` recorded with WPR) still to be recorded. |
| §2.3 Storage (ActivityStore / EntityStore / HistoryEngine) | ✅ via `WarpTests.Store.cpp` and `WarpTests.Context.cpp` | Existing lifecycle/concurrency tests plus v1/v2 -> v3 migration, case-collision reconstruction, explicit legacy aliases, interval union, purge/replay and restart. |
| Consumer API (ApiServer) | ✅ via `WarpTests.Api.cpp` + `python/test_live_api.py` | Structured errors, escaped/Unicode keys, scopes, ranking/search/feed/purge/timeline cursors, long-poll; live: per-session pipe, standard-user (medium IL) client, server PID check. |
| JSON protocol (WarpJson) | ✅ via `WarpTests.Json.cpp` | Strict parsing, exact int64, surrogates, invalid UTF-8, depth limits, writer never emits invalid JSON (BUG-7). |
| §3.1 Tokenizer bit-exactness | ⏳ TODO | Existing `scripts/modernbert_tokenizer_ref.py` is a starting point; needs pytest wrapper. |
| §4.6 Chaos / fault injection | ⏳ TODO | Needs fault-injection harness. Deferred to P4.5. |
| §4.7 Local scorecard anomaly detection | ⏳ TODO | Needs ≥ 30 scorecards in `eval/results/` first. |

`✅` = runnable today. `⏳` = planned in [`/TEST_PLAN.md` §9 Phased rollout](../TEST_PLAN.md#9-phased-rollout).

## Layout

```
tests/
  pytest.ini                   # pytest config + markers (l1, l1_property, l2, l3, live)
  requirements.txt             # pytest, hypothesis
  run_tests.py                 # cross-platform driver
  run_tests.ps1                # Windows wrapper
  run_signal_quality.ps1       # native build + on-demand quality gates
  SIGNAL_QUALITY.md            # evaluation protocol and metric contracts

  WarpTests.vcxproj            # C++ unit tests (not in WARP!.sln; built by CI)
  WarpTests.h                  # minimal TEST/CHECK/REQUIRE harness + temp dirs
  WarpTests.Main.cpp           # runner (/filter, /timeout watchdog, /list)
  WarpTests.Context.cpp        # actual composer, migrations and source-record replay (ORT linked; models optional)
  WarpTests.Evidence.cpp       # durable evidence, episodes, aliases, coverage and interval consistency
  WarpTests.Json.cpp           # WarpJson parser / writer
  WarpTests.Canon.cpp          # UrlCanon, EntityKey (+ SQL), DocResolver, FileCapture path rules
  WarpTests.Store.cpp          # ActivityStore + EntityStore + HistoryEngine on a real SQLite file
  WarpTests.Api.cpp            # ApiServer::Handle against a real store

  python/                      # all pytest-driven tests
    _warp_guards.py            # pure-Python ports of C++ guards (shared)
    conftest.py                # fixtures (repo_root, cpp_source, etc.)
    warp_pipe.py               # ctypes named-pipe client (also a CLI)
    test_live_api.py           # live API integration (marker: live; CI only)
    test_hallucination_guard.py
    test_near_copy.py
    test_discourse_stops_sync.py     # pins C++ DISCOURSE_STOPS ↔ Python set
    test_recency_constants_sync.py   # pins EntityStore / StoreUtil constants

    property/                  # Hypothesis-based fuzz tests
      test_inference_engine_props.py
      test_grounding_gate_props.py

    adversarial/               # prompt-injection / fuzz
      test_prompt_markers.py
      test_title_perturbations.py

    contract/                  # JSON schema, model-contract (stub)

  eval/                        # L3 quality tests
    signal_quality.py          # native runner + labeled metric scorer / report
    test_signal_quality.py     # metric math, bad-input, coverage and comparison gates
    corpus.jsonl               # seed eval scenarios (handwritten gold)
    metrics.py                 # ROUGE-L, must_contain recall, etc.
    test_metrics_self.py       # sanity checks on the metric impl
    test_corpus_integrity.py   # schema + self-consistency of corpus
    results/                   # gitignored; populated per run

  fixtures/
    signal_quality.jsonl       # labeled Phase A regression/correlation scenarios
    adversarial_titles.jsonl   # seed adversarial corpus

  schema/                      # JSON schemas (stub for P2)
```

## Drift-prevention design

The Python tests exist for tractability — fuzzing the engagement math
and the LlmSummarizer guards is far cheaper in Python. The risk that
Python ports diverge from C++ over time is real, and is mitigated by
**two source-of-truth pinning tests**:

* `test_discourse_stops_sync.py` parses `LlmSummarizer.cpp`,
  extracts the literal `DiscourseStops()` set, and asserts it
  equals the Python `DISCOURSE_STOPS`. Any edit on either side
  forces an explicit re-sync.
* `test_recency_constants_sync.py` parses `EntityStore.cpp`
  (`TAU_SECONDS` / `MAX_SCORE` / `SCORE_CAP` of the legacy recency
  score) and `ActivityStore.h` (engagement time constants and
  weights) and asserts the Python module mirrors them exactly.

These sync tests are the contract that lets the property-based
tests in `property/` remain meaningful as the codebase evolves. The
C++ unit tests then check the same behaviour on the real code.

## Writing new tests

* **Pure-logic ports go in `python/_warp_guards.py`** with a
  corresponding sync test in `python/test_*_sync.py`.
* **Example-based regression tests** go next to the existing
  `test_hallucination_guard.py` / `test_near_copy.py` pattern.
* **Property-based fuzz** goes in `python/property/`.
* **Adversarial cases** go in `python/adversarial/` plus a new row
  in `tests/fixtures/adversarial_titles.jsonl` (one JSON per line,
  schema documented in `test_title_perturbations.py`).
* **Eval scenarios** go in `tests/eval/corpus.jsonl` (one JSON per
  line). Read the corpus authoring rules in
  [`TEST_PLAN.md §4.1`](../TEST_PLAN.md) before adding rows.

## CI integration

`.github/workflows/build.yml`:

* The `tests` job runs the Python L1 + L1 property + L2 + L3 layers on
  every push (no models, no build).
* The `build` job builds `tests\WarpTests.vcxproj` (x64), runs its real-composer
  regressions and two replay passes through `eval/signal_quality.py`, repeats
  with the already-downloaded Granite bundle (Qwen off), and uploads
  the scorecard/native results/synthetic observations even on failure. Native
  regressions retain the 15-minute watchdog. The job then
  starts the freshly built `WARP!.exe --headless --data-dir <temp>`,
  runs `python/test_live_api.py` against its pipe (including as a
  filtered medium-integrity client), calls `WARP!.exe --status`, stops
  it through its shutdown event and prints its log.

Everything is local-only — no cloud LLM, no API key, no telemetry.
See [`TEST_PLAN.md §4.0`](../TEST_PLAN.md) for the privacy boundary.

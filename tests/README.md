# WARP test suite

This directory implements the test plan described in
[`/TEST_PLAN.md`](../TEST_PLAN.md). See the one-page summary in
[`/TEST_PLAN_SUMMARY.md`](../TEST_PLAN_SUMMARY.md) for the high-level
framework.

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
| §2.4 InferenceEngine math | ✅ via `python/property/test_inference_engine_props.py` | Hypothesis-fuzzed against a Python port pinned to C++ via `test_recency_constants_sync.py`. |
| §2.8 Property-based tests | ✅ partial | InferenceEngine math + grounding gate covered; JSON shape + pipeline state machine still stubbed. |
| §3.3.5 Hallucination guard | ✅ via `python/test_hallucination_guard.py` | 15 example cases + Hypothesis fuzz. |
| §3.3.4 Near-copy gate | ✅ via `python/test_near_copy.py` | 9 cases including all known regressions. |
| §3.3.1 Post-processing sanitizer | ✅ via `python/adversarial/test_prompt_markers.py` | All `<think>` / `<\|im_*\|>` variants covered. |
| §3.6 Adversarial fuzz | ✅ via `python/adversarial/test_title_perturbations.py` + `fixtures/adversarial_titles.jsonl` | Pure-Python replay through the post-processing strippers; full end-to-end (with Qwen3) deferred to P3.5. |
| §4.2.1b ROUGE-L | ✅ via `eval/metrics.py` + `eval/test_metrics_self.py` | LCS-based F1; self-tested. |
| §4.2.2 Programmatic token metrics | ✅ via `eval/metrics.py` | must_contain recall, must_not_contain hits, token recall. |
| §4.2.1a Granite cosine | ⏳ TODO | Needs `tests/eval/run_eval.py` that loads the granite ONNX. Deferred to P4 of phased rollout. |
| §4.1 Eval corpus | ✅ seed (5 scenarios) | `eval/corpus.jsonl` + `eval/test_corpus_integrity.py` validate schema + reference satisfies its own assertions. Expand to 50+ in P5. |
| §2.1 Capture layer | ⏳ TODO | Needs `IRawSource` injection seam refactor (§2.1.0 + §6 Q6). Deferred to P3. |
| §2.3 ActivityDatabase | ⏳ TODO | Needs a C++ test framework (doctest/gtest). Deferred to P1. |
| §3.1 Tokenizer bit-exactness | ⏳ TODO | Existing `scripts/modernbert_tokenizer_ref.py` is a starting point; needs pytest wrapper. |
| §4.6 Chaos / fault injection | ⏳ TODO | Needs fault-injection harness. Deferred to P4.5. |
| §4.7 Local scorecard anomaly detection | ⏳ TODO | Needs ≥ 30 scorecards in `eval/results/` first. |

`✅` = runnable today. `⏳` = planned in [`/TEST_PLAN.md` §9 Phased rollout](../TEST_PLAN.md#9-phased-rollout).

## Layout

```
tests/
  pytest.ini                   # pytest config + markers
  requirements.txt             # pytest, hypothesis
  run_tests.py                 # cross-platform driver
  run_tests.ps1                # Windows wrapper

  python/                      # all pytest-driven tests
    _warp_guards.py            # pure-Python ports of C++ guards (shared)
    conftest.py                # fixtures (repo_root, cpp_source, etc.)
    test_hallucination_guard.py
    test_near_copy.py
    test_discourse_stops_sync.py     # pins C++ DISCOURSE_STOPS ↔ Python set
    test_recency_constants_sync.py   # pins TAU_SECONDS / MAX_SCORE / SCORE_CAP

    property/                  # Hypothesis-based fuzz tests
      test_inference_engine_props.py
      test_grounding_gate_props.py

    adversarial/               # prompt-injection / fuzz
      test_prompt_markers.py
      test_title_perturbations.py

    contract/                  # JSON schema, model-contract (stub)

  eval/                        # L3 quality tests
    corpus.jsonl               # seed eval scenarios (handwritten gold)
    metrics.py                 # ROUGE-L, must_contain recall, etc.
    test_metrics_self.py       # sanity checks on the metric impl
    test_corpus_integrity.py   # schema + self-consistency of corpus
    results/                   # gitignored; populated per run

  fixtures/
    adversarial_titles.jsonl   # seed adversarial corpus

  schema/                      # JSON schemas (stub for P2)
```

## Drift-prevention design

The Python tests exist for tractability — running a full C++ test
harness against the InferenceEngine math + LlmSummarizer guards
would need doctest/gtest infrastructure we don't yet have. The
risk that Python ports diverge from C++ over time is real, and is
mitigated by **two source-of-truth pinning tests**:

* `test_discourse_stops_sync.py` parses `LlmSummarizer.cpp`,
  extracts the literal `DiscourseStops()` set, and asserts it
  equals the Python `DISCOURSE_STOPS`. Any edit on either side
  forces an explicit re-sync.
* `test_recency_constants_sync.py` parses `InferenceEngine.cpp`
  for `TAU_SECONDS` / `MAX_SCORE` / `SCORE_CAP` and asserts the
  Python module mirrors them exactly.

These sync tests are the contract that lets the property-based
tests in `property/` remain meaningful as the codebase evolves.

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

The `tests` job in `.github/workflows/build.yml` runs the L1 + L1
property + L2 layers on every push. The L3 eval (which requires the
granite ONNX session and a full WARP build) runs in a separate
follow-up job once the granite-cosine eval driver lands (P4).

Everything is local-only — no cloud LLM, no API key, no telemetry.
See [`TEST_PLAN.md §4.0`](../TEST_PLAN.md) for the privacy boundary.

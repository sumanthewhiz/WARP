"""
Eval corpus integrity tests.  These don't run WARP -- they just
validate that the corpus file itself is well-formed and that the
metrics-vs-reference comparisons behave sensibly on the
handwritten reference summaries.

The full eval pipeline (drive WARP, embed via granite, compute
cosine + ROUGE-L, write scorecard) lives in
`tests/eval/run_eval.py` (TODO -- requires the granite ONNX
session, gets implemented in P4 of TEST_PLAN.md §9).
"""

from __future__ import annotations

import json
import pathlib

import pytest

from metrics import (
    must_contain_recall,
    must_not_contain_hits,
    rouge_l_f1,
)


CORPUS_PATH = pathlib.Path(__file__).parent / "corpus.jsonl"

REQUIRED_TOP_KEYS  = {"id", "scenario_class", "input_window_secs",
                      "events", "inference_engine_state", "expected"}
REQUIRED_EXPECTED  = {"reference_summary", "must_contain",
                      "must_not_contain", "expected_facet",
                      "min_confidence", "should_emit_summary"}


def _load_corpus():
    rows = []
    for i, line in enumerate(CORPUS_PATH.read_text(encoding="utf-8").splitlines(), 1):
        line = line.strip()
        if not line:
            continue
        try:
            rows.append(json.loads(line))
        except json.JSONDecodeError as exc:
            pytest.fail(f"corpus.jsonl line {i} is not valid JSON: {exc}")
    return rows


@pytest.fixture(scope="module")
def corpus():
    return _load_corpus()


# =====================================================================
# Schema validation
# =====================================================================

def test_corpus_nonempty(corpus):
    assert len(corpus) >= 3, "Need at least 3 seed scenarios"


def test_unique_ids(corpus):
    ids = [row["id"] for row in corpus]
    assert len(ids) == len(set(ids)), "duplicate scenario ids"


@pytest.mark.parametrize("idx", range(20))  # generous; skipped beyond corpus size
def test_row_shape(corpus, idx):
    if idx >= len(corpus):
        pytest.skip(f"only {len(corpus)} rows in corpus")
    row = corpus[idx]
    missing = REQUIRED_TOP_KEYS - set(row.keys())
    assert not missing, f"row {row.get('id','?')!r} missing keys: {missing}"
    expected = row["expected"]
    missing_e = REQUIRED_EXPECTED - set(expected.keys())
    assert not missing_e, (
        f"row {row['id']!r} expected.* missing: {missing_e}"
    )


def test_should_emit_consistency(corpus):
    """When should_emit_summary == False, reference_summary must be empty;
    when True, reference_summary must be non-empty."""
    for row in corpus:
        e = row["expected"]
        if e["should_emit_summary"]:
            assert e["reference_summary"], (
                f"{row['id']}: should_emit_summary=True but reference_summary is empty"
            )
        else:
            assert not e["reference_summary"], (
                f"{row['id']}: should_emit_summary=False but reference_summary is non-empty"
            )


# =====================================================================
# Self-comparison sanity: reference vs reference scores perfectly
# =====================================================================

def test_reference_vs_self_rouge_l_is_one(corpus):
    """ROUGE-L of the reference against itself is 1.0 for all
    scenarios that emit a summary.  Catches metric-impl regressions."""
    for row in corpus:
        ref = row["expected"]["reference_summary"]
        if not ref:
            continue
        assert rouge_l_f1(ref, ref) == pytest.approx(1.0), row["id"]


def test_reference_satisfies_must_contain(corpus):
    """The handwritten reference must include every must_contain token.
    If it doesn't, either the reference is wrong or must_contain is
    over-specified."""
    for row in corpus:
        e = row["expected"]
        ref = e["reference_summary"]
        if not ref:
            continue
        recall = must_contain_recall(e["must_contain"], ref)
        assert recall == 1.0, (
            f"{row['id']}: reference fails its own must_contain check "
            f"(recall={recall}, must_contain={e['must_contain']}, "
            f"reference={ref!r})"
        )


def test_reference_passes_must_not_contain(corpus):
    """The handwritten reference must not contain any must_not_contain
    token.  Catches accidental leak via the reference itself."""
    for row in corpus:
        e = row["expected"]
        ref = e["reference_summary"]
        if not ref:
            continue
        hits = must_not_contain_hits(e["must_not_contain"], ref)
        assert not hits, (
            f"{row['id']}: reference contains must_not_contain tokens {hits}"
        )

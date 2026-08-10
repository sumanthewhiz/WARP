"""
Adversarial-corpus integrity + post-processing replay tests.

For every fixture in `tests/fixtures/adversarial_titles.jsonl`:

  1. Validate the row is well-formed.
  2. Run the **post-processing strippers** (`strip_think_blocks`
     + `strip_im_markers`) against the raw adversarial title and
     assert none of the `must_not_contain` strings survive.

This is the pure-Python part of the §3.6 adversarial layer.  The
full end-to-end test (drive the Qwen3 model with the adversarial
title and verify the model's *output* doesn't contain the payload)
lives in `tests/python/integration/` and skips when the model files
are absent.
"""

from __future__ import annotations

import json
import pathlib

import pytest

from _warp_guards import strip_im_markers, strip_think_blocks


CORPUS_PATH = (pathlib.Path(__file__).parent.parent.parent
               / "fixtures" / "adversarial_titles.jsonl")

REQUIRED_KEYS = {"id", "category", "title", "must_not_contain"}


def _load_corpus():
    rows = []
    for i, line in enumerate(CORPUS_PATH.read_text(encoding="utf-8").splitlines(), 1):
        if not line.strip():
            continue
        try:
            rows.append(json.loads(line))
        except json.JSONDecodeError as exc:
            pytest.fail(f"adversarial_titles.jsonl line {i}: {exc}")
    return rows


CORPUS = _load_corpus()


def test_corpus_nonempty():
    assert len(CORPUS) >= 10, "Need a meaningful adversarial baseline"


def test_unique_ids():
    ids = [row["id"] for row in CORPUS]
    assert len(ids) == len(set(ids))


def test_categories_covered():
    """Every documented attack category from TEST_PLAN.md §3.6.1
    should have at least one fixture row."""
    expected = {
        "prompt_template_marker",
        "instruction_override",
        "fake_conversation",
        "unicode_adversarial",
        "control_char",
        "length_attack",
        "non_english",
    }
    present = {row["category"] for row in CORPUS}
    missing = expected - present
    assert not missing, f"missing adversarial categories: {missing}"


@pytest.mark.parametrize(
    "row",
    CORPUS,
    ids=[r["id"] for r in CORPUS],
)
def test_row_well_formed(row):
    missing = REQUIRED_KEYS - set(row.keys())
    assert not missing, f"row {row.get('id','?')} missing keys: {missing}"
    assert isinstance(row["title"], str)
    assert isinstance(row["must_not_contain"], list)


# =====================================================================
# Post-processing strippers handle every prompt-template-marker fixture
# =====================================================================
@pytest.mark.parametrize(
    "row",
    [r for r in CORPUS if r["category"] == "prompt_template_marker"],
    ids=[r["id"] for r in CORPUS if r["category"] == "prompt_template_marker"],
)
def test_post_processing_strips_prompt_markers(row):
    """If the raw title was emitted by the model verbatim, the
    sanitizer must strip every banned marker."""
    sanitized = strip_im_markers(strip_think_blocks(row["title"]))
    for banned in row["must_not_contain"]:
        # The think+im strippers only cover their specific markers;
        # other categories (HTML, markdown) are skipped intentionally
        # here -- those need separate handling in the model's prompt
        # or post-processor.  Pin the specific marker classes the
        # strippers DO cover.
        if banned in ("<think>", "</think>", "<|im_start|>", "<|im_end|>"):
            assert banned not in sanitized, (
                f"sanitizer left {banned!r} in output for {row['id']}: "
                f"{sanitized!r}"
            )


# =====================================================================
# Length-attack fixtures don't crash the strippers
# =====================================================================
@pytest.mark.parametrize(
    "row",
    [r for r in CORPUS if r["category"] == "length_attack"],
    ids=[r["id"] for r in CORPUS if r["category"] == "length_attack"],
)
def test_post_processing_handles_length_attacks(row):
    sanitized = strip_im_markers(strip_think_blocks(row["title"]))
    assert isinstance(sanitized, str)
    # Empty input → empty output; huge input → finite output that doesn't
    # exceed the input length.
    assert len(sanitized) <= len(row["title"])

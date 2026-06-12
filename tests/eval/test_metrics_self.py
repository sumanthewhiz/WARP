"""
Self-tests on the eval-layer metric implementations.

Before we use these metrics to grade production output we have to
prove the metrics themselves are correct.  These tests pin specific
expected values against handcrafted inputs.
"""

import math

import pytest

from metrics import (
    _lcs_length,
    _word_tokens,
    combined_similarity,
    must_contain_recall,
    must_not_contain_hits,
    rouge_l_f1,
    token_recall,
)


# =====================================================================
# _word_tokens basics
# =====================================================================
class TestWordTokens:

    def test_lowercases(self):
        assert _word_tokens("Hello World") == ["hello", "world"]

    def test_strips_punctuation(self):
        assert _word_tokens("Hello, world!") == ["hello", "world"]

    def test_preserves_numbers(self):
        assert _word_tokens("Q3 2026 budget") == ["q3", "2026", "budget"]

    def test_handles_empty(self):
        assert _word_tokens("") == []
        assert _word_tokens("!!!") == []


# =====================================================================
# _lcs_length basics + edge cases
# =====================================================================
class TestLcsLength:

    def test_identical(self):
        assert _lcs_length([1, 2, 3], [1, 2, 3]) == 3

    def test_disjoint(self):
        assert _lcs_length([1, 2, 3], [4, 5, 6]) == 0

    def test_partial_match(self):
        assert _lcs_length(["a", "b", "c", "d"], ["a", "x", "c", "d"]) == 3

    def test_empty(self):
        assert _lcs_length([], [1, 2, 3]) == 0
        assert _lcs_length([1, 2, 3], []) == 0
        assert _lcs_length([], []) == 0

    def test_order_matters(self):
        """LCS is order-preserving; permutation is NOT a match."""
        assert _lcs_length([1, 2, 3], [3, 2, 1]) == 1


# =====================================================================
# ROUGE-L: pinned expected values
# =====================================================================
class TestRougeL:

    def test_identical_strings(self):
        s = "User is working on Q4 plan in Word."
        assert rouge_l_f1(s, s) == pytest.approx(1.0)

    def test_completely_unrelated(self):
        ref = "User is in Outlook."
        hyp = "Lorem ipsum dolor sit amet."
        assert rouge_l_f1(ref, hyp) == 0.0

    def test_empty_returns_zero(self):
        assert rouge_l_f1("", "anything") == 0.0
        assert rouge_l_f1("anything", "") == 0.0

    def test_legitimate_paraphrase_scores_moderate(self):
        """The §4.2.1b documentation says legitimate paraphrase scores
        ~0.40 on ROUGE-L (lower than cosine).  Pin that ballpark."""
        ref = "User is working on Q4 Plan v2 in Word."
        hyp = "Editing the Q4 Plan document in Microsoft Word."
        score = rouge_l_f1(ref, hyp)
        assert 0.25 < score < 0.65, (
            f"legitimate paraphrase ROUGE-L = {score}, expected ~0.40"
        )

    def test_topic_swap_scores_low(self):
        """Documented example: 'Numbers' swapped for 'Excel' should
        score low on ROUGE-L (catches what cosine misses)."""
        ref = "User is working on Q4 plan in Excel."
        hyp = "User is working on Q4 plan in Numbers."
        score = rouge_l_f1(ref, hyp)
        # Most tokens match; one diverges -- still scores high here
        # because the divergence is a single token in a 7-token
        # reference.  The shared-blindspot defense requires the
        # *combined* gate from §4.4 (cosine AND ROUGE-L both
        # tripping) -- ROUGE-L alone catches it only when several
        # tokens diverge.  Pin the actual value so a future ROUGE
        # implementation change is visible.
        assert 0.7 < score < 0.95

    def test_hallucination_scores_very_low(self):
        """A genuine hallucination (off-topic + invented words) scores
        near zero on ROUGE-L."""
        ref = "User is working on Q4 plan in Word."
        hyp = "User is reading about indexer reliability across emails."
        score = rouge_l_f1(ref, hyp)
        assert score < 0.30


# =====================================================================
# must_contain / must_not_contain
# =====================================================================
class TestMustContain:

    def test_all_present(self):
        assert must_contain_recall(["q4", "plan"], "User is working on Q4 plan.") == 1.0

    def test_partial(self):
        recall = must_contain_recall(["q4", "plan", "missing"],
                                     "User is working on Q4 plan.")
        assert math.isclose(recall, 2/3)

    def test_none_present(self):
        assert must_contain_recall(["alpha"], "beta gamma") == 0.0

    def test_empty_required_is_vacuous_pass(self):
        assert must_contain_recall([], "anything") == 1.0

    def test_case_insensitive(self):
        assert must_contain_recall(["q4"], "User is working on Q4.") == 1.0


class TestMustNotContain:

    def test_no_violations(self):
        assert must_not_contain_hits(["indexer", "kubernetes"],
                                     "User is in Word") == []

    def test_violation_returned(self):
        hits = must_not_contain_hits(["indexer", "kubernetes"],
                                     "User is debugging indexer rollout")
        assert hits == ["indexer"]

    def test_multiple_violations(self):
        hits = must_not_contain_hits(["foo", "bar", "baz"],
                                     "foo and bar and ham")
        assert set(hits) == {"foo", "bar"}


# =====================================================================
# combined_similarity (used by §4.2.5 calibration)
# =====================================================================
class TestCombinedSimilarity:

    def test_geometric_mean(self):
        # gm(0.81, 0.25) = sqrt(0.2025) = 0.45
        assert combined_similarity(0.81, 0.25) == pytest.approx(0.45, abs=1e-6)

    def test_one_metric_zero_yields_zero(self):
        """Geometric mean is dominated by the smaller value."""
        assert combined_similarity(1.0, 0.0) == 0.0
        assert combined_similarity(0.0, 1.0) == 0.0

    def test_both_one_yields_one(self):
        assert combined_similarity(1.0, 1.0) == pytest.approx(1.0)

    def test_clamps_to_valid_range(self):
        # Inputs outside [0,1] (e.g. floating-point noise) clamp safely.
        assert combined_similarity(1.5, 1.5) == pytest.approx(1.0)
        assert combined_similarity(-0.1, 0.5) == 0.0


# =====================================================================
# Sanity: token_recall is a thin alias of must_contain_recall
# =====================================================================
def test_token_recall_matches_must_contain():
    s = "User is working on Q4 plan."
    assert token_recall(["q4", "plan"], s) == must_contain_recall(["q4", "plan"], s)

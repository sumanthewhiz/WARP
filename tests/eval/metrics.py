"""
Eval-layer metrics.  All metrics are pure-Python and local-only;
no embedding model, no LLM judge, no cloud.

§4.2.1a — granite cosine — lives in `cosine.py` (when the granite
ONNX is available; skipped otherwise).
§4.2.1b — ROUGE-L surface overlap — lives here.
§4.2.2  — programmatic token metrics — live here.
"""

from __future__ import annotations

import re
from typing import List, Sequence


# =====================================================================
# Tokenisation for surface metrics (NOT the same as the C++ guard
# tokenizer; here we want full word tokens, not >= 4-char filter).
# =====================================================================
def _word_tokens(text: str) -> List[str]:
    """Lowercase word tokens for ROUGE-L / token-set comparisons."""
    return re.findall(r"[a-z0-9]+", text.lower())


# =====================================================================
# §4.2.1b — ROUGE-L (granite-independent)
# =====================================================================
def _lcs_length(a: Sequence, b: Sequence) -> int:
    """Length of longest common subsequence between `a` and `b`."""
    m, n = len(a), len(b)
    if m == 0 or n == 0:
        return 0
    # Space-optimized: only the previous row matters.
    prev = [0] * (n + 1)
    curr = [0] * (n + 1)
    for i in range(1, m + 1):
        ai = a[i - 1]
        for j in range(1, n + 1):
            if ai == b[j - 1]:
                curr[j] = prev[j - 1] + 1
            else:
                curr[j] = max(prev[j], curr[j - 1])
        prev, curr = curr, prev
        # curr is now the older row; clear for next iteration
        for j in range(n + 1):
            curr[j] = 0
    return prev[n]


def rouge_l_f1(reference: str, hypothesis: str) -> float:
    """ROUGE-L F1 between `reference` and `hypothesis` strings.
    Returns a value in [0, 1].  0 when either side is empty."""
    r_toks = _word_tokens(reference)
    h_toks = _word_tokens(hypothesis)
    if not r_toks or not h_toks:
        return 0.0
    lcs = _lcs_length(r_toks, h_toks)
    if lcs == 0:
        return 0.0
    p = lcs / len(h_toks)
    r = lcs / len(r_toks)
    if p + r == 0:
        return 0.0
    return (2.0 * p * r) / (p + r)


# =====================================================================
# §4.2.2 — programmatic token metrics
# =====================================================================
def must_contain_recall(must_contain: Sequence[str], summary: str) -> float:
    """Fraction of `must_contain` tokens that appear in `summary`
    (case-insensitive, word-level)."""
    if not must_contain:
        return 1.0  # vacuously satisfied
    s_toks = set(_word_tokens(summary))
    hits = sum(1 for tok in must_contain if tok.lower() in s_toks)
    return hits / len(must_contain)


def must_not_contain_hits(must_not_contain: Sequence[str], summary: str) -> List[str]:
    """Return the list of `must_not_contain` tokens that DO appear in
    `summary`.  Non-empty means a hallucination got through."""
    s_toks = set(_word_tokens(summary))
    return [tok for tok in must_not_contain if tok.lower() in s_toks]


def token_recall(expected_keywords: Sequence[str], summary: str) -> float:
    """Same shape as `must_contain_recall` but used for soft topic
    keywords that aren't hard assertions."""
    return must_contain_recall(expected_keywords, summary)


# =====================================================================
# §4.2.5 — combined similarity for confidence calibration
# =====================================================================
def combined_similarity(cosine: float, rouge_l: float) -> float:
    """Geometric mean of normalized cosine and ROUGE-L (both in [0,1]).
    Used by the confidence-calibration bucket aggregation so neither
    metric dominates."""
    c = max(0.0, min(1.0, cosine))
    r = max(0.0, min(1.0, rouge_l))
    return (c * r) ** 0.5

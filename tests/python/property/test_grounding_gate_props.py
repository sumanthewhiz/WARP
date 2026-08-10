"""
Property-based tests for the v5.15 hallucination guard
(`LlmSummarizer::IsHallucination`).  These exercise the §2.8.3
invariants from TEST_PLAN.md by generating random inputs with
Hypothesis and asserting properties that should hold for *all* such
inputs.

Where the example-based suite in
`tests/python/test_hallucination_guard.py` pins specific known cases,
this suite explores the input space for unknown-unknown failure
modes.
"""

import string

import pytest
from hypothesis import given, settings, strategies as st

from _warp_guards import (
    DISCOURSE_STOPS,
    build_input_tokens,
    is_hallucination,
    tokenize,
)


# -- strategies -------------------------------------------------------

# A vocabulary of grounded input tokens we can compose into both
# input bags and synthetic output lines.  ASCII-lowercase, >= 4 chars
# so they always reach the guard's tokenizer cutoff.
_GROUNDED_WORDS = st.text(
    alphabet=string.ascii_lowercase,
    min_size=4, max_size=10,
)

_STOP_WORD = st.sampled_from(sorted(DISCOURSE_STOPS))


@st.composite
def _stop_only_sentence(draw):
    """Generate a sentence composed entirely of discourse-stop words
    + plain whitespace.  Punctuation skipped so we don't accidentally
    introduce a 'word' that's actually a short content token."""
    n = draw(st.integers(min_value=1, max_value=12))
    words = [draw(_STOP_WORD) for _ in range(n)]
    return " ".join(words)


@st.composite
def _subset_sentence(draw, input_tokens):
    """Generate a sentence composed only of tokens from `input_tokens`
    plus stops + whitespace.  Should be grounded."""
    n = draw(st.integers(min_value=1, max_value=10))
    pool = sorted(input_tokens) + sorted(DISCOURSE_STOPS)
    if not pool:
        return ""
    return " ".join(draw(st.sampled_from(pool)) for _ in range(n))


# =====================================================================
# §2.8.3 invariants
# =====================================================================

@settings(max_examples=200, deadline=None)
@given(_stop_only_sentence())
def test_stop_only_sentence_is_grounded(line):
    """A sentence composed entirely of DISCOURSE_STOPS words is never
    flagged as hallucination -- regardless of input vocabulary."""
    # Empty input vocab is the hardest case (nothing grounded).
    assert not is_hallucination(line, set())


@settings(max_examples=200, deadline=None)
@given(input_tokens=st.sets(_GROUNDED_WORDS, min_size=0, max_size=20))
def test_empty_line_is_grounded(input_tokens):
    """The empty string never trips the guard."""
    assert not is_hallucination("", input_tokens)


@settings(max_examples=200, deadline=None)
@given(input_tokens=st.sets(_GROUNDED_WORDS, min_size=1, max_size=20),
       data=st.data())
def test_subset_of_input_is_grounded(input_tokens, data):
    """A line composed only of tokens from `input_tokens` (plus
    stops) is never flagged as hallucination."""
    line = data.draw(_subset_sentence(input_tokens))
    assert not is_hallucination(line, input_tokens), (
        f"line={line!r} input_tokens={sorted(input_tokens)}"
    )


@settings(max_examples=200, deadline=None)
@given(
    input_tokens=st.sets(_GROUNDED_WORDS, min_size=1, max_size=10),
    intruder=_GROUNDED_WORDS,
)
def test_hapax_injection_is_hallucinated(input_tokens, intruder):
    """Prepending a single content word that's neither in input nor
    in stops nor a fuzzy match makes the line a hallucination."""
    # Skip if intruder is grounded (in input, in stops, or fuzzy-related)
    if intruder in input_tokens or intruder in DISCOURSE_STOPS:
        return
    # Fuzzy: intruder must not be a prefix/suffix of any input token
    # and no input token a prefix/suffix of it.
    for itok in input_tokens:
        if len(itok) >= 4 and (itok in intruder or intruder in itok):
            return  # legitimately grounded by fuzzy match

    line = f"{intruder} and other text"
    assert is_hallucination(line, input_tokens), (
        f"intruder={intruder!r} should have tripped the guard "
        f"against input_tokens={sorted(input_tokens)}"
    )


@settings(max_examples=200, deadline=None)
@given(
    a=_GROUNDED_WORDS,
    b=_GROUNDED_WORDS,
)
def test_fuzzy_match_symmetry(a, b):
    """If `is_hallucination(a, {b}) == False` because of the fuzzy
    prefix/suffix rule, then swapping (a in {b} -> b in {a}) is also
    grounded."""
    # Only check when fuzzy is the *only* reason for grounding.
    if a == b or a in DISCOURSE_STOPS or b in DISCOURSE_STOPS:
        return
    a_grounded_by_b = (len(b) >= 4 and (b in a or a in b))
    if not a_grounded_by_b:
        return
    # Then both directions should report grounded.
    assert not is_hallucination(a, {b})
    assert not is_hallucination(b, {a})


# =====================================================================
# Bonus: the guard is robust to unicode / control chars in input
# =====================================================================

@settings(max_examples=100, deadline=None)
@given(line=st.text(min_size=0, max_size=500))
def test_guard_never_raises(line):
    """Whatever junk we feed in -- emoji, RTL marks, ZWJ, control
    chars, very long strings -- the guard either returns True or
    False, never raises."""
    result = is_hallucination(line, set())
    assert isinstance(result, bool)


@settings(max_examples=100, deadline=None)
@given(line=st.text(min_size=0, max_size=200))
def test_tokenize_emits_only_ascii_lowercase(line):
    """Tokens are always lowercased ASCII alnum.  Anything else
    means the tokenizer is leaking unicode into downstream
    comparisons (which would silently corrupt the grounding check)."""
    for tok in tokenize(line):
        assert tok == tok.lower()
        assert all(c.isalnum() for c in tok)

"""
Property-based tests for the recency-score formula and
ApplyInferenceBoost weighting.  These exercise the §2.8.1 / §2.8.2
invariants from TEST_PLAN.md against the Python ports of the C++
logic; the `test_recency_constants_sync.py` test guarantees the
ports stay in lockstep with the C++ source.
"""

import math

import pytest
from hypothesis import given, settings, strategies as st

from _warp_guards import (
    MAX_SCORE,
    SCORE_CAP,
    TAU_SECONDS,
    apply_inference_boost,
    compute_boost,
    compute_recency_score,
)


# =====================================================================
# §2.8.1 — ComputeRecencyScore invariants
# =====================================================================

@settings(max_examples=200, deadline=None)
@given(
    now=st.integers(min_value=0, max_value=2_000_000_000),
    last_open_ts=st.integers(min_value=1, max_value=2_000_000_000),
    open_count_7d=st.floats(min_value=0.0, max_value=1_000_000.0,
                            allow_nan=False, allow_infinity=False),
)
def test_recency_score_bounded(now, last_open_ts, open_count_7d):
    """Score is always in [0, SCORE_CAP]."""
    s = compute_recency_score(now, last_open_ts, open_count_7d)
    assert 0.0 <= s <= SCORE_CAP, f"score {s} outside [0, {SCORE_CAP}]"


@settings(max_examples=200, deadline=None)
@given(
    open_count_7d=st.floats(min_value=0.0, max_value=10_000.0,
                            allow_nan=False, allow_infinity=False),
)
def test_recency_score_zero_when_never_opened(open_count_7d):
    """lastOpenTs <= 0 short-circuits to 0 regardless of count."""
    assert compute_recency_score(1_000_000, 0,  open_count_7d) == 0.0
    assert compute_recency_score(1_000_000, -5, open_count_7d) == 0.0


@settings(max_examples=200, deadline=None)
@given(
    now=st.integers(min_value=1_000, max_value=2_000_000_000),
    delta_t=st.integers(min_value=0, max_value=10 * 86_400),
    base_count=st.floats(min_value=0.0, max_value=100.0, allow_nan=False),
    extra_count=st.floats(min_value=0.0, max_value=100.0, allow_nan=False),
)
def test_recency_score_monotonic_in_count(now, delta_t, base_count, extra_count):
    """Holding now/lastOpenTs fixed, higher count never lowers the score.
    (Until clamp kicks in -- then both pin at SCORE_CAP, still satisfies.)"""
    last_open_ts = now - delta_t
    if last_open_ts <= 0:
        return
    s_low  = compute_recency_score(now, last_open_ts, base_count)
    s_high = compute_recency_score(now, last_open_ts, base_count + extra_count)
    assert s_high >= s_low - 1e-9, (
        f"score not monotonic in count: low={s_low} high={s_high}"
    )


@settings(max_examples=200, deadline=None)
@given(
    now=st.integers(min_value=1_000, max_value=2_000_000_000),
    delta_close=st.integers(min_value=0, max_value=86_400),
    delta_extra=st.integers(min_value=0, max_value=10 * 86_400),
    count=st.floats(min_value=0.0, max_value=100.0, allow_nan=False),
)
def test_recency_score_monotonic_in_recency(now, delta_close, delta_extra, count):
    """Holding count fixed, more recent open never lowers the score."""
    last_open_close = now - delta_close
    last_open_far   = now - (delta_close + delta_extra)
    if last_open_far <= 0 or last_open_close <= 0:
        return
    s_close = compute_recency_score(now, last_open_close, count)
    s_far   = compute_recency_score(now, last_open_far,   count)
    assert s_close >= s_far - 1e-9, (
        f"score not monotonic in recency: close={s_close} far={s_far}"
    )


def test_recency_score_known_values():
    """Pin specific values so changes to the formula force an explicit
    update.  Computed from MAX_SCORE * exp(-Δt/TAU) + log(1+count)*5."""
    # Δt = 0, count = 0  -> MAX_SCORE
    assert math.isclose(compute_recency_score(1_000_000, 1_000_000, 0.0),
                        MAX_SCORE, rel_tol=1e-9)
    # Δt = TAU, count = 0  -> MAX_SCORE / e
    s = compute_recency_score(int(TAU_SECONDS) + 1_000_000, 1_000_000, 0.0)
    assert math.isclose(s, MAX_SCORE / math.e, rel_tol=1e-6), s
    # Δt = 10*TAU, count = 0  -> effectively 0
    s = compute_recency_score(int(10 * TAU_SECONDS) + 1_000_000, 1_000_000, 0.0)
    assert s < 0.1, s
    # Δt = 0, count = huge  -> SCORE_CAP
    assert math.isclose(compute_recency_score(1_000_000, 1_000_000, 1e6),
                        SCORE_CAP, rel_tol=1e-9)


# =====================================================================
# §2.8.2 — ApplyInferenceBoost invariants
# =====================================================================

@settings(max_examples=200, deadline=None)
@given(
    recency=st.floats(min_value=0.0, max_value=SCORE_CAP, allow_nan=False),
    count=st.floats(min_value=0.0, max_value=10_000.0, allow_nan=False),
)
def test_boost_lower_bound(recency, count):
    """Boost is always >= 1.0 (multiplicative, never *reduces* weight)."""
    b = compute_boost(recency, count)
    assert b >= 1.0


@settings(max_examples=200, deadline=None)
@given(
    recency=st.floats(min_value=0.0, max_value=SCORE_CAP, allow_nan=False),
    count=st.floats(min_value=0.0, max_value=10_000.0, allow_nan=False),
)
def test_boost_upper_bound(recency, count):
    """Boost saturates well below 10x for any plausible store state.
    Documented range: 1.0x -> ~5.3x for production-realistic inputs."""
    b = compute_boost(recency, count)
    # The largest possible boost happens at recency=SCORE_CAP (255),
    # count=10000.  Compute the exact upper bound to pin it.
    max_boost = 1.0 + math.log1p(SCORE_CAP / 50.0) + math.log1p(10_000.0 / 5.0)
    assert b <= max_boost + 1e-9, f"boost {b} > expected max {max_boost}"
    # Realistic-bound sanity: even at SCORE_CAP recency + 10 events/week
    # in count, boost stays under 6x.
    realistic = compute_boost(SCORE_CAP, 10.0)
    assert realistic < 6.0


def test_boost_baseline_is_one():
    """Engine-unknown entity (recency=0, count=0) -> boost = 1.0."""
    assert compute_boost(0.0, 0.0) == 1.0


@settings(max_examples=100, deadline=None)
@given(
    raw_focus=st.lists(
        st.integers(min_value=0, max_value=3600),
        min_size=1, max_size=20,
    ),
)
def test_apply_boost_null_engine_is_noop(raw_focus):
    """When lookup is None, weighted == raw for every entry."""
    bag = [{"totalFocusSecs": x, "exePath": f"app{i}.exe"}
           for i, x in enumerate(raw_focus)]
    apply_inference_boost(bag, lookup=None,
                          key_of=lambda e: e["exePath"])
    for entry in bag:
        assert entry["weightedFocusSecs"] == float(entry["totalFocusSecs"])


@settings(max_examples=100, deadline=None)
@given(
    raw_focus=st.lists(
        st.integers(min_value=0, max_value=3600),
        min_size=1, max_size=20,
    ),
)
def test_apply_boost_unknown_entities_unchanged(raw_focus):
    """Entries unknown to the engine keep weighted == raw."""
    bag = [{"totalFocusSecs": x, "exePath": f"app{i}.exe"}
           for i, x in enumerate(raw_focus)]
    apply_inference_boost(
        bag,
        lookup=lambda k: None,  # engine knows nobody
        key_of=lambda e: e["exePath"],
    )
    for entry in bag:
        assert entry["weightedFocusSecs"] == float(entry["totalFocusSecs"])


@settings(max_examples=100, deadline=None)
@given(
    raw=st.integers(min_value=0, max_value=3600),
)
def test_apply_boost_no_invention(raw):
    """totalFocusSecs == 0 → weightedFocusSecs == 0 even with a
    fully-warm InferenceEngine record."""
    bag = [{"totalFocusSecs": raw, "exePath": "x.exe"}]
    if raw == 0:
        apply_inference_boost(
            bag,
            lookup=lambda k: {"entityKey": "x.exe",
                              "recencyScore": SCORE_CAP,
                              "openCount7d": 100.0},
            key_of=lambda e: e["exePath"],
        )
        assert bag[0]["weightedFocusSecs"] == 0.0


@settings(max_examples=100, deadline=None)
@given(
    raw=st.integers(min_value=1, max_value=3600),
)
def test_apply_boost_preserves_raw_field(raw):
    """totalFocusSecs is never mutated; only weightedFocusSecs is written."""
    bag = [{"totalFocusSecs": raw, "exePath": "x.exe"}]
    apply_inference_boost(
        bag,
        lookup=lambda k: {"entityKey": "x.exe",
                          "recencyScore": 150.0,
                          "openCount7d": 8.0},
        key_of=lambda e: e["exePath"],
    )
    assert bag[0]["totalFocusSecs"] == raw  # unchanged
    assert bag[0]["weightedFocusSecs"] > raw  # boosted

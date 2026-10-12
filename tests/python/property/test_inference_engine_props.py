"""
Property-based tests for the engagement model and the ApplyInferenceBoost
weighting.  These exercise the §2.8.1 / §2.8.2 invariants from
TEST_PLAN.md (plus the v6 decayed-engagement invariants for RANK-1 /
RANK-2 / INTEL-4) against the Python ports of the C++ logic; the
`test_recency_constants_sync.py` test keeps the ports in lockstep with
the C++ source.
"""

import math

import pytest
from hypothesis import given, settings, strategies as st

from _warp_guards import (
    MAX_SCORE,
    SCORE_CAP,
    TAU_LONG_SECONDS,
    TAU_SECONDS,
    TAU_SHORT_SECONDS,
    apply_inference_boost,
    blended_prior,
    compute_boost,
    compute_recency_score,
    decay_add,
    decay_at,
    decay_key,
    percentile,
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
# §2.8.2 — ApplyInferenceBoost invariants (bounded tie-breaker, INTEL-4)
# =====================================================================

@settings(max_examples=200, deadline=None)
@given(score=st.floats(min_value=-10.0, max_value=10.0, allow_nan=False))
def test_boost_bounds(score):
    """History can never more than 1.5x what happened in the window."""
    b = compute_boost(score)
    assert 1.0 <= b <= 1.5


def test_boost_baseline_is_one():
    """Unknown / never-engaged entity -> boost = 1.0."""
    assert compute_boost(0.0) == 1.0


@settings(max_examples=200, deadline=None)
@given(
    a=st.floats(min_value=0.0, max_value=1.0, allow_nan=False),
    b=st.floats(min_value=0.0, max_value=1.0, allow_nan=False),
)
def test_boost_monotonic_in_score(a, b):
    lo, hi = min(a, b), max(a, b)
    assert compute_boost(hi) >= compute_boost(lo)


@settings(max_examples=200, deadline=None)
@given(
    focus_now=st.integers(min_value=1, max_value=3600),
    score_now=st.floats(min_value=0.0, max_value=1.0, allow_nan=False),
    score_habit=st.floats(min_value=0.0, max_value=1.0, allow_nan=False),
)
def test_boost_cannot_override_current_task(focus_now, score_now, score_habit):
    """INTEL-4: an entry with >= 1.5x the in-window focus always ranks first,
    however habitual the other one is."""
    focus_habit = focus_now
    focus_task = int(math.ceil(focus_now * 1.5)) + 1
    task = focus_task * compute_boost(score_now)
    habit = focus_habit * compute_boost(score_habit)
    assert task > habit


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
    """Entries unknown to the store keep weighted == raw."""
    bag = [{"totalFocusSecs": x, "exePath": f"app{i}.exe"}
           for i, x in enumerate(raw_focus)]
    apply_inference_boost(
        bag,
        lookup=lambda k: None,
        key_of=lambda e: e["exePath"],
    )
    for entry in bag:
        assert entry["weightedFocusSecs"] == float(entry["totalFocusSecs"])


def test_apply_boost_no_invention():
    """totalFocusSecs == 0 -> weightedFocusSecs == 0 even for a top entity."""
    bag = [{"totalFocusSecs": 0, "exePath": "x.exe"}]
    apply_inference_boost(bag, lookup=lambda k: {"found": True, "score": 1.0},
                          key_of=lambda e: e["exePath"])
    assert bag[0]["weightedFocusSecs"] == 0.0


@settings(max_examples=100, deadline=None)
@given(raw=st.integers(min_value=1, max_value=3600))
def test_apply_boost_preserves_raw_field(raw):
    """totalFocusSecs is never mutated; only weightedFocusSecs is written."""
    bag = [{"totalFocusSecs": raw, "exePath": "x.exe"}]
    apply_inference_boost(bag, lookup=lambda k: {"found": True, "score": 0.8},
                          key_of=lambda e: e["exePath"])
    assert bag[0]["totalFocusSecs"] == raw
    assert raw < bag[0]["weightedFocusSecs"] <= raw * 1.5


# =====================================================================
# Decayed engagement accumulators (RANK-1 / RANK-2)
# =====================================================================

_events = st.lists(
    st.tuples(st.integers(min_value=1_600_000_000, max_value=1_700_000_000),
              st.floats(min_value=0.01, max_value=10.0, allow_nan=False)),
    min_size=1, max_size=25,
)


def _accumulate(events, tau):
    s, t0 = 0.0, 0
    for t, w in events:
        s, t0 = decay_add(s, t0, w, t, tau)
    return s, t0


@settings(max_examples=200, deadline=None)
@given(events=_events, tau=st.sampled_from([TAU_SHORT_SECONDS, TAU_LONG_SECONDS]))
def test_decay_is_order_independent(events, tau):
    """Late / backfilled events (USN downtime backfill) give the same value as in-order ones."""
    now = 1_700_000_100
    s1, t1 = _accumulate(sorted(events), tau)
    s2, t2 = _accumulate(list(reversed(sorted(events))), tau)
    v1, v2 = decay_at(s1, t1, now, tau), decay_at(s2, t2, now, tau)
    assert math.isclose(v1, v2, rel_tol=1e-9, abs_tol=1e-12)
    exact = sum(w * math.exp(-(now - t) / tau) for t, w in events)
    assert math.isclose(v1, exact, rel_tol=1e-9, abs_tol=1e-12)


@settings(max_examples=200, deadline=None)
@given(a=_events, b=_events, tau=st.sampled_from([TAU_SHORT_SECONDS, TAU_LONG_SECONDS]),
       later=st.integers(min_value=0, max_value=400 * 86400))
def test_decay_key_orders_like_value_at_any_time(a, b, tau, later):
    """RANK-1: ranking by the stored key equals ranking by the decayed value at ANY query time."""
    sa, ta = _accumulate(sorted(a), tau)
    sb, tb = _accumulate(sorted(b), tau)
    now = max(ta, tb) + later
    va, vb = decay_at(sa, ta, now, tau), decay_at(sb, tb, now, tau)
    ka, kb = decay_key(sa, ta, tau), decay_key(sb, tb, tau)
    if abs(ka - kb) > 1e-9 and va > 0 and vb > 0 and abs(math.log(va) - math.log(vb)) > 1e-9:
        assert (ka > kb) == (va > vb)


@settings(max_examples=200, deadline=None)
@given(s=st.floats(min_value=0.001, max_value=1e6, allow_nan=False),
       t0=st.integers(min_value=1_600_000_000, max_value=1_700_000_000),
       d1=st.integers(min_value=0, max_value=200 * 86400),
       d2=st.integers(min_value=0, max_value=200 * 86400))
def test_decay_is_monotonically_non_increasing(s, t0, d1, d2):
    lo, hi = min(d1, d2), max(d1, d2)
    assert decay_at(s, t0, t0 + hi, TAU_SHORT_SECONDS) <= decay_at(s, t0, t0 + lo, TAU_SHORT_SECONDS) + 1e-12


def test_short_horizon_forgets_faster_than_long():
    s, t0 = decay_add(0.0, 0, 10.0, 1_000_000, TAU_SHORT_SECONDS)
    ls, lt0 = decay_add(0.0, 0, 10.0, 1_000_000, TAU_LONG_SECONDS)
    week_later = 1_000_000 + 7 * 86400
    assert decay_at(s, t0, week_later, TAU_SHORT_SECONDS) < decay_at(ls, lt0, week_later, TAU_LONG_SECONDS)


@settings(max_examples=200, deadline=None)
@given(priors=st.lists(st.floats(min_value=0.0, max_value=100.0, allow_nan=False), min_size=1, max_size=50),
       x=st.floats(min_value=0.0, max_value=100.0, allow_nan=False))
def test_percentile_bounded_and_monotonic(priors, x):
    sp = sorted(priors)
    p = percentile(sp, x)
    assert 0.0 <= p <= 1.0
    assert percentile(sp, x + 1.0) >= p


def test_blended_prior_weights():
    assert math.isclose(blended_prior(1.0, 0.0), 0.7)
    assert math.isclose(blended_prior(0.0, 1.0), 0.3 * TAU_SHORT_SECONDS / TAU_LONG_SECONDS)

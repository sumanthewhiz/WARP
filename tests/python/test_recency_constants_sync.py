"""
Sync test: the recency-score constants in `_warp_guards.py`
(`TAU_SECONDS`, `MAX_SCORE`, `SCORE_CAP`) must mirror the values in
`InferenceEngine.cpp`.  Drift means the property tests in
`tests/python/property/test_inference_engine_props.py` exercise the
wrong formula and silently let bugs through.
"""

import re

from _warp_guards import TAU_SECONDS, MAX_SCORE, SCORE_CAP


def _extract_cpp_constant(cpp_text: str, name: str) -> float:
    """Find `static const double NAME = VALUE;` in the source."""
    m = re.search(
        rf"static\s+const\s+double\s+{re.escape(name)}\s*=\s*([0-9.eE+\-]+)\s*;",
        cpp_text,
    )
    assert m, (
        f"Could not locate `static const double {name}` in "
        f"InferenceEngine.cpp -- either the constant was renamed or "
        f"its definition shape changed.  Update this test."
    )
    return float(m.group(1))


def test_recency_constants_match_cpp_source(cpp_source):
    cpp_text = cpp_source["InferenceEngine.cpp"]
    cpp_tau = _extract_cpp_constant(cpp_text, "TAU_SECONDS")
    cpp_max = _extract_cpp_constant(cpp_text, "MAX_SCORE")
    cpp_cap = _extract_cpp_constant(cpp_text, "SCORE_CAP")

    mismatches = []
    if cpp_tau != TAU_SECONDS:
        mismatches.append(f"  TAU_SECONDS: cpp={cpp_tau!r}  python={TAU_SECONDS!r}")
    if cpp_max != MAX_SCORE:
        mismatches.append(f"  MAX_SCORE:   cpp={cpp_max!r}  python={MAX_SCORE!r}")
    if cpp_cap != SCORE_CAP:
        mismatches.append(f"  SCORE_CAP:   cpp={cpp_cap!r}  python={SCORE_CAP!r}")

    assert not mismatches, (
        "Recency-score constants drifted between C++ and Python.\n"
        "Update `tests/python/_warp_guards.py` to match the C++ "
        "source of truth in InferenceEngine.cpp:\n" + "\n".join(mismatches)
    )

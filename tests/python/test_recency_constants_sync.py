"""
Sync tests: the constants of the Python ports in `_warp_guards.py` must
mirror the C++ sources of truth.

* Legacy v1 `recency_score` (`TAU_SECONDS`, `MAX_SCORE`, `SCORE_CAP`)
  lives in `EntityStore.cpp` (`EntityStore::LegacyRecencyScore`).
* The decayed-engagement model (time constants and event weights) lives
  in `ActivityStore.h` (`namespace StoreUtil`).

Drift means the property tests exercise the wrong formulas and silently
let bugs through.
"""

import re

from _warp_guards import (
    ENGAGEMENT_WEIGHTS,
    MAX_SCORE,
    SCORE_CAP,
    TAU_LONG_SECONDS,
    TAU_SECONDS,
    TAU_SHORT_SECONDS,
)


def _extract_cpp_constant(cpp_text: str, name: str, file_name: str) -> float:
    """Find `static const double NAME = VALUE;` or `constexpr double NAME = EXPR;`."""
    m = re.search(
        rf"(?:static\s+const|constexpr)\s+double\s+{re.escape(name)}\s*=\s*([^;]+);",
        cpp_text,
    )
    assert m, (
        f"Could not locate the double constant `{name}` in {file_name} -- "
        f"either it was renamed or its definition shape changed.  Update this test."
    )
    expr = m.group(1).strip()
    assert re.fullmatch(r"[0-9.eE+\-*/ ()]+", expr), f"unexpected expression for {name}: {expr!r}"
    return float(eval(expr, {"__builtins__": {}}, {}))  # noqa: S307 - digits/operators only (checked above)


def test_recency_constants_match_cpp_source(cpp_source):
    cpp_text = cpp_source["EntityStore.cpp"]
    pairs = {
        "TAU_SECONDS": TAU_SECONDS,
        "MAX_SCORE": MAX_SCORE,
        "SCORE_CAP": SCORE_CAP,
    }
    mismatches = []
    for name, py in pairs.items():
        cpp = _extract_cpp_constant(cpp_text, name, "EntityStore.cpp")
        if cpp != py:
            mismatches.append(f"  {name}: cpp={cpp!r}  python={py!r}")
    assert not mismatches, (
        "Recency-score constants drifted between C++ and Python.\n"
        "Update `tests/python/_warp_guards.py` to match EntityStore.cpp:\n" + "\n".join(mismatches)
    )


def test_engagement_constants_match_cpp_source(cpp_source):
    cpp_text = cpp_source["ActivityStore.h"]
    pairs = {"kTauShortSec": TAU_SHORT_SECONDS, "kTauLongSec": TAU_LONG_SECONDS}
    pairs.update(ENGAGEMENT_WEIGHTS)
    mismatches = []
    for name, py in pairs.items():
        cpp = _extract_cpp_constant(cpp_text, name, "ActivityStore.h")
        if abs(cpp - py) > 1e-12:
            mismatches.append(f"  {name}: cpp={cpp!r}  python={py!r}")
    assert not mismatches, (
        "Engagement constants drifted between C++ and Python.\n"
        "Update `tests/python/_warp_guards.py` to match ActivityStore.h:\n" + "\n".join(mismatches)
    )

"""
Sync test: the `DISCOURSE_STOPS` set in `tests/python/_warp_guards.py`
must mirror exactly the `DiscourseStops()` function in
`LlmSummarizer.cpp`.  Drift here means the hallucination guard's
behaviour diverges between the C++ runtime and the Python test
harness — silently letting bugs through that the suite is supposed
to catch.

Extracts the C++ set by parsing the source; intentionally fragile to
formatting so a drifted edit forces an explicit re-sync rather than
silently passing.
"""

import re

from _warp_guards import DISCOURSE_STOPS


def _extract_cpp_stops(cpp_text: str) -> set:
    """Find the `DiscourseStops()` function and return the set of
    literal strings inside its initializer."""
    # Match the function body up to the matching `};`
    m = re.search(
        r"DiscourseStops\(\)\s*\{[^}]*?"
        r"static\s+const\s+std::unordered_set<std::string>\s+s\s*=\s*\{(.*?)\};",
        cpp_text,
        re.DOTALL,
    )
    assert m, (
        "Could not locate `DiscourseStops()` in LlmSummarizer.cpp -- "
        "either the function was renamed or its initializer shape "
        "changed.  Update this test together with the renaming."
    )
    body = m.group(1)
    # Pull every string literal "..." from the body.
    literals = re.findall(r'"([^"]+)"', body)
    return set(literals)


def test_discourse_stops_match_cpp_source(cpp_source):
    cpp_text = cpp_source["LlmSummarizer.cpp"]
    cpp_set = _extract_cpp_stops(cpp_text)

    only_in_cpp = cpp_set - DISCOURSE_STOPS
    only_in_python = DISCOURSE_STOPS - cpp_set

    assert not only_in_cpp and not only_in_python, (
        "DISCOURSE_STOPS drift between C++ and Python.  Update "
        "`tests/python/_warp_guards.py::DISCOURSE_STOPS` to match.\n"
        f"  Only in C++ (LlmSummarizer.cpp): {sorted(only_in_cpp)}\n"
        f"  Only in Python (_warp_guards.py): {sorted(only_in_python)}"
    )


def test_discourse_stops_nontrivial():
    """Sanity guard: the set should be non-empty and contain at least
    the most-common false-positive trigger words.  Catches the
    failure mode where the C++ extractor silently returned the empty
    set (e.g. regex break) and both sides happened to match."""
    assert len(DISCOURSE_STOPS) >= 50
    for tok in ["user", "they", "reading", "about", "email", "browser"]:
        assert tok in DISCOURSE_STOPS, f"missing core stop {tok!r}"

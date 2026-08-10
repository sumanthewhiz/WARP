"""
Regression tests for `LlmSummarizer::IsNearCopyOfExisting`.
Ports the case fixtures from `scripts/test_near_copy.py` to pytest.
"""

import pytest

from _warp_guards import is_near_copy


# (description, line, existing, expect_reject)
CASES = [
    # legitimate paraphrases — must NOT be rejected
    (
        "natural prefix addition is legitimate paraphrase",
        "User is reading about Indexer Reliability in Outlook and M365 Copilot.",
        ["Reading about Indexer Reliability (in Outlook & M365 Copilot)"],
        False,
    ),
    (
        "structural rewrite with new content (5 tabs)",
        "User is exploring the indexer rollout across 5 browser tabs.",
        ["Exploring Indexer Rollout (across 5 browser tabs)"],
        False,
    ),
    (
        "longer prose form is legitimate",
        "User is working on the auth refactor in their browser and "
        "editing the related source file in Visual Studio.",
        ["Working on auth refactor (across Visual Studio & Edge)"],
        False,
    ),
    (
        "different verb is legitimate",
        "User is checking email in Outlook.",
        ["Checking email in Outlook"],
        # ↑ identical post-normalization after stripping "user is " → reject
        True,
    ),
    (
        "facet-broadening rewrite is legitimate",
        "User is working on Indexer Reliability across Word, Excel, and PowerPoint.",
        ["Working on Indexer Reliability (across Word, Excel, PowerPoint)"],
        False,
    ),

    # pure copies — must be rejected
    (
        "pure verbatim copy is rejected",
        "Reading about Indexer Reliability (in Outlook & M365 Copilot)",
        ["Reading about Indexer Reliability (in Outlook & M365 Copilot)"],
        True,
    ),
    (
        "opener-only filler (adds 'User is ' but no other change) is rejected",
        "User is reading about Indexer Reliability in Outlook M365 Copilot",
        ["Reading about Indexer Reliability in Outlook M365 Copilot"],
        True,
    ),
    (
        "empty line is not flagged",
        "",
        ["Reading about indexer reliability"],
        False,
    ),
    (
        "empty existing list never triggers",
        "Any line here",
        [],
        False,
    ),
]


@pytest.mark.parametrize(
    "desc,line,existing,expect_reject",
    [(c[0], c[1], c[2], c[3]) for c in CASES],
    ids=[c[0] for c in CASES],
)
def test_is_near_copy(desc, line, existing, expect_reject):
    actual = is_near_copy(line, existing)
    assert actual == expect_reject, (
        f"{desc}: expected expect_reject={expect_reject}, got {actual}\n"
        f"  line:     {line!r}\n"
        f"  existing: {existing}"
    )

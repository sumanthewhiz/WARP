"""
Regression tests for the v5.15 hallucination guard
(`LlmSummarizer::IsHallucination`).  Ports the case fixtures from
`scripts/test_hallucination_guard.py` to pytest.

These are example-based tests; property-based fuzz on the same guard
lives in `tests/python/property/test_grounding_gate_props.py`.
"""

import pytest

from _warp_guards import build_input_tokens, is_hallucination


# ----- REJECT cases (must be flagged as hallucination) ---------------
REJECT_CASES = [
    # The literal text the user reported in v5.15.
    (
        "indexer reliability leak with Outlook+Teams input",
        "User is reading about indexer reliability across "
        "their emails and chats (Outlook, Microsoft Teams).",
        [("Outlook", "Inbox - Suman.Ghosh@microsoft.com"),
         ("Microsoft Teams", "Activity")],
        ["Reading email in Outlook (across Outlook & Teams)"],
    ),
    (
        "indexer reliability leak with stray paren",
        "User is reading about indexer reliability across "
        "their emails and chats (Outlook, Microsoft Teams)).",
        [("Outlook", "Inbox"), ("Microsoft Teams", "Chat")],
        ["Reading email in Outlook"],
    ),
    (
        "auth refactor leak with VS Code input",
        "User is reviewing the auth refactor PR in their "
        "browser and editing the related source file in "
        "Visual Studio.",
        [("Microsoft Edge", "Google Search"),
         ("Visual Studio Code", "settings.json")],
        ["Searching in browser"],
    ),
    (
        "React hooks leak with unrelated browsing",
        "User is exploring various websites about React "
        "hooks and state management.",
        [("Microsoft Edge", "Azure Portal - Resource group")],
        ["Browsing Azure Portal"],
    ),
    (
        "Q3 budget leak with unrelated documents",
        "User is reviewing the Q3 budget across their "
        "spreadsheets and a status email.",
        [("Word", "Customer interview notes.docx"),
         ("Outlook", "Inbox - Re: Customer follow-up")],
        ["Editing customer notes"],
    ),
    (
        "customer onboarding leak with unrelated docs",
        "User is working on a customer onboarding flow in "
        "their browser and a related design document.",
        [("Excel", "Quarterly sales data.xlsx"),
         ("Outlook", "Inbox - Sales numbers Q4")],
        ["Reviewing sales numbers"],
    ),
    (
        "invented project name not in input",
        "User is drafting the Project Phoenix specification in their notes app.",
        [("OneNote", "Untitled section")],
        ["Writing notes in OneNote"],
    ),
    (
        "invented technical noun (kubernetes)",
        "User is debugging kubernetes deployment issues in their terminal.",
        [("Windows Terminal", "PowerShell - ls"),
         ("Visual Studio Code", "main.py")],
        ["Working in terminal"],
    ),
    (
        "rewrite that invents an ungrounded word",
        "User is reviewing the authentication refactor in their IDE.",
        [("Visual Studio Code", "auth.cpp"),
         ("Visual Studio Code", "authentication.h")],
        ["Editing auth (Visual Studio Code)"],
    ),
]


# ----- ACCEPT cases (must be allowed through) ------------------------
ACCEPT_CASES = [
    (
        "Q3 budget LEGITIMATELY in input",
        "User is reviewing the Q3 budget across their "
        "spreadsheets and a status email.",
        [("Excel", "Q3 budget v2 - draft.xlsx"),
         ("Outlook", "Re: Q3 budget status update")],
        ["Reviewing Q3 budget (Excel & Outlook)"],
    ),
    (
        "indexer reliability LEGITIMATELY in input",
        "User is reading about indexer reliability across "
        "their emails and chats.",
        [("Outlook", "Indexer reliability rollout - status"),
         ("Microsoft Teams", "Indexer reliability war room")],
        ["Reading about Indexer Reliability"],
    ),
    (
        "natural rewrite of existing topic hint",
        "User is reviewing the authentication refactor in Visual Studio Code.",
        [("Visual Studio Code", "auth_refactor.cpp"),
         ("Visual Studio Code", "authentication.h")],
        ["Editing auth refactor (Visual Studio Code)"],
    ),
    (
        "compound word fuzzy match (m365copilot vs m365 + copilot)",
        "User is working in M365Copilot.",
        [("M365Copilot", "Untitled chat")],
        ["Working in M365Copilot"],
    ),
    (
        "all-stops sentence is not hallucination",
        "User is reading emails and chats.",
        [("Outlook", "Inbox"), ("Microsoft Teams", "Activity")],
        ["Reading email"],
    ),
    (
        "topic from existing only (model rephrased the hint)",
        "User is summarizing telemetry data in their docs.",
        [("Word", "telemetry_data_notes.docx")],
        ["Working on telemetry data summary (Word)"],
    ),
]


@pytest.mark.parametrize(
    "name,line,items,existing",
    [(c[0], c[1], c[2], c[3]) for c in REJECT_CASES],
    ids=[c[0] for c in REJECT_CASES],
)
def test_hallucination_rejected(name, line, items, existing):
    input_tokens = build_input_tokens(items, existing)
    assert is_hallucination(line, input_tokens), (
        f"Expected REJECT for {name!r} but guard accepted it.\n"
        f"  line:           {line!r}\n"
        f"  input_tokens:   {sorted(input_tokens)}"
    )


@pytest.mark.parametrize(
    "name,line,items,existing",
    [(c[0], c[1], c[2], c[3]) for c in ACCEPT_CASES],
    ids=[c[0] for c in ACCEPT_CASES],
)
def test_hallucination_accepted(name, line, items, existing):
    input_tokens = build_input_tokens(items, existing)
    assert not is_hallucination(line, input_tokens), (
        f"Expected ACCEPT for {name!r} but guard rejected it.\n"
        f"  line:           {line!r}\n"
        f"  input_tokens:   {sorted(input_tokens)}"
    )

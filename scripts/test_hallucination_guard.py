"""
Pure-Python port of LlmSummarizer::IsHallucination() and its
DiscourseStops() set, used as a regression test for the
hallucination guard added in v5.15.

Run with `python test_hallucination_guard.py`.  Exits non-zero if any
case disagrees with the expected outcome.

Keep this in sync with LlmSummarizer.cpp when the C++ guard logic
changes -- the C++ implementation is the source of truth, this file
is the executable spec.
"""

import sys

DISCOURSE_STOPS = {
    # pronouns / determiners / discourse openers
    "user", "they", "their", "them", "theirs", "themselves",
    "themself", "your", "yours", "ours", "mine",
    "currently", "right", "just", "still", "also", "then",
    "here", "there",
    # generic verbs that describe kinds of activity
    "reading", "reviewing", "writing", "editing", "working",
    "exploring", "researching", "checking", "looking",
    "browsing", "watching", "listening", "discussing",
    "designing", "playing", "drafting", "drafts", "draft",
    "drafted", "viewing", "managing", "scrolling",
    "running", "open", "opens", "opening", "opened",
    "summarizing", "summarize", "summarized", "summarising",
    "summarised", "catching", "catch", "caught",
    "have", "having", "been", "being",
    # connectives / prepositions / conjunctions (>= 4 chars)
    "about", "across", "between", "through", "while", "during",
    "with", "from", "into", "onto", "upon", "over", "under",
    "after", "before", "until", "where", "when", "what",
    "which", "that", "this", "these", "those", "some", "other",
    "another", "several", "various", "many", "multiple",
    "more", "than", "both", "either", "neither",
    # generic context / app-type nouns
    "page", "pages", "site", "sites", "website", "websites",
    "document", "documents", "doc", "docs", "file", "files",
    "folder", "folders", "window", "windows", "screen", "screens",
    "browser", "browsers", "tabs", "applications",
    "application", "apps", "tool", "tools",
    "email", "emails", "mail", "inbox", "outbox",
    "chat", "chats", "message", "messages", "messaging",
    "meeting", "meetings", "call", "calls", "video", "audio",
    "spreadsheet", "spreadsheets", "presentation",
    "presentations", "slides", "slide", "code", "source",
    "notes", "note", "notebook", "notebooks", "links", "link",
    "items", "item", "entries", "entry",
    # ordinals / size words
    "first", "second", "third", "fourth", "fifth",
    "next", "last", "main", "core", "much", "long", "short",
    "quick", "small", "large", "huge", "tiny",
    # generic markers
    "etc", "topic", "topics", "subject", "subjects",
    "content", "context", "session", "sessions",
    "snapshot", "summary", "summaries", "details", "detail",
    "things", "stuff", "task", "tasks", "activity",
    "activities",
}


def _tokenize(s):
    """Yield lowercase >= 4-char alnum tokens, like the C++ code."""
    cur = []
    for c in s:
        if c.isalnum():
            cur.append(c.lower())
        else:
            if len(cur) >= 4:
                yield "".join(cur)
            cur = []
    if len(cur) >= 4:
        yield "".join(cur)


def build_input_tokens(items, existing):
    """items: list of (app, title) tuples; existing: list of str."""
    toks = set()
    for app, title in items:
        for s in (app, title, title):  # rawTitle ~= title in tests
            for t in _tokenize(s):
                toks.add(t)
    for line in existing:
        for t in _tokenize(line):
            toks.add(t)
    return toks


def is_hallucination(line, input_tokens):
    """Returns True if any topic-bearing token in `line` is ungrounded."""
    for tok in _tokenize(line):
        if tok in DISCOURSE_STOPS:
            continue
        if tok in input_tokens:
            continue
        # Fuzzy: prefix/suffix match against any input token >= 4 chars
        fuzzy_ok = False
        for itok in input_tokens:
            if len(itok) >= 4 and (itok in tok or tok in itok):
                fuzzy_ok = True
                break
        if fuzzy_ok:
            continue
        return True
    return False


CASES = [
    # ----------------------------------------------------------------
    # MUST REJECT (the bug the user reported + variants)
    # ----------------------------------------------------------------
    # The exact byte-for-byte few-shot leak the user reported.
    {
        "name": "indexer reliability leak with Outlook+Teams input",
        "line": "User is reading about indexer reliability across "
                "their emails and chats (Outlook, Microsoft Teams).",
        "items": [("Outlook", "Inbox - Suman.Ghosh@microsoft.com"),
                  ("Microsoft Teams", "Activity")],
        "existing": ["Reading email in Outlook (across Outlook & Teams)"],
        "expect_reject": True,
    },
    # Same leak, with extra trailing paren (literal user-reported text).
    {
        "name": "indexer reliability leak with stray paren",
        "line": "User is reading about indexer reliability across "
                "their emails and chats (Outlook, Microsoft Teams)).",
        "items": [("Outlook", "Inbox"),
                  ("Microsoft Teams", "Chat")],
        "existing": ["Reading email in Outlook"],
        "expect_reject": True,
    },
    # Old auth-refactor few-shot leak.
    {
        "name": "auth refactor leak with VS Code input",
        "line": "User is reviewing the auth refactor PR in their "
                "browser and editing the related source file in "
                "Visual Studio.",
        "items": [("Microsoft Edge", "Google Search"),
                  ("Visual Studio Code", "settings.json")],
        "existing": ["Searching in browser"],
        "expect_reject": True,
    },
    # Old React-hooks few-shot leak.
    {
        "name": "React hooks leak with unrelated browsing",
        "line": "User is exploring various websites about React "
                "hooks and state management.",
        "items": [("Microsoft Edge", "Azure Portal - Resource group")],
        "existing": ["Browsing Azure Portal"],
        "expect_reject": True,
    },
    # New v5.15 examples being copied for unrelated scenarios.
    {
        "name": "Q3 budget leak with unrelated documents",
        "line": "User is reviewing the Q3 budget across their "
                "spreadsheets and a status email.",
        "items": [("Word", "Customer interview notes.docx"),
                  ("Outlook", "Inbox - Re: Customer follow-up")],
        "existing": ["Editing customer notes"],
        "expect_reject": True,
    },
    {
        "name": "customer onboarding leak with unrelated docs",
        "line": "User is working on a customer onboarding flow in "
                "their browser and a related design document.",
        "items": [("Excel", "Quarterly sales data.xlsx"),
                  ("Outlook", "Inbox - Sales numbers Q4")],
        "existing": ["Reviewing sales numbers"],
        "expect_reject": True,
    },
    # Other potential hallucination patterns.
    {
        "name": "invented project name not in input",
        "line": "User is drafting the Project Phoenix specification "
                "in their notes app.",
        "items": [("OneNote", "Untitled section")],
        "existing": ["Writing notes in OneNote"],
        "expect_reject": True,
    },
    {
        "name": "invented technical noun (kubernetes)",
        "line": "User is debugging kubernetes deployment issues in "
                "their terminal.",
        "items": [("Windows Terminal", "PowerShell - ls"),
                  ("Visual Studio Code", "main.py")],
        "existing": ["Working in terminal"],
        "expect_reject": True,
    },

    # ----------------------------------------------------------------
    # MUST ACCEPT (legitimate outputs)
    # ----------------------------------------------------------------
    {
        "name": "Q3 budget LEGITIMATELY in input",
        "line": "User is reviewing the Q3 budget across their "
                "spreadsheets and a status email.",
        "items": [("Excel", "Q3 budget v2 - draft.xlsx"),
                  ("Outlook", "Re: Q3 budget status update")],
        "existing": ["Reviewing Q3 budget (Excel & Outlook)"],
        "expect_reject": False,
    },
    {
        "name": "indexer reliability LEGITIMATELY in input",
        "line": "User is reading about indexer reliability across "
                "their emails and chats.",
        "items": [("Outlook", "Indexer reliability rollout - status"),
                  ("Microsoft Teams", "Indexer reliability war room")],
        "existing": ["Reading about Indexer Reliability"],
        "expect_reject": False,
    },
    {
        "name": "natural rewrite of existing topic hint",
        "line": "User is reviewing the authentication refactor in "
                "Visual Studio Code.",
        "items": [("Visual Studio Code", "auth_refactor.cpp"),
                  ("Visual Studio Code", "authentication.h")],
        "existing": ["Editing auth refactor (Visual Studio Code)"],
        "expect_reject": False,
    },
    {
        "name": "compound word fuzzy match (m365copilot vs m365 + copilot)",
        "line": "User is working in M365Copilot.",
        "items": [("M365Copilot", "Untitled chat")],
        "existing": ["Working in M365Copilot"],
        "expect_reject": False,
    },
    {
        "name": "all-stops sentence is not hallucination",
        "line": "User is reading emails and chats.",
        "items": [("Outlook", "Inbox"),
                  ("Microsoft Teams", "Activity")],
        "existing": ["Reading email"],
        "expect_reject": False,
    },
    {
        "name": "topic from existing only (model rephrased the hint)",
        "line": "User is summarizing telemetry data in their docs.",
        "items": [("Word", "telemetry_data_notes.docx")],
        "existing": ["Working on telemetry data summary (Word)"],
        "expect_reject": False,
    },
    # An over-eager rewrite that genuinely invents a word should be
    # rejected -- this is a *desired* false-positive trade-off.  False
    # positives drop us back to the algorithmic summary (still correct,
    # just less natural); false negatives would show invented content
    # to the user, which is worse.
    {
        "name": "rewrite that invents an ungrounded word (design choice: reject)",
        "line": "User is reviewing the authentication refactor in "
                "their IDE.",
        "items": [("Visual Studio Code", "auth.cpp"),
                  ("Visual Studio Code", "authentication.h")],
        "existing": ["Editing auth (Visual Studio Code)"],
        "expect_reject": True,
    },
]


def main():
    fails = 0
    for c in CASES:
        ti = build_input_tokens(c["items"], c["existing"])
        got = is_hallucination(c["line"], ti)
        if got != c["expect_reject"]:
            print(f"FAIL: {c['name']}")
            print(f"   line: {c['line']!r}")
            print(f"   expect_reject={c['expect_reject']} got={got}")
            fails += 1
        else:
            verdict = "REJECT" if got else "ACCEPT"
            print(f"OK   ({verdict}): {c['name']}")
    print(f"\n{len(CASES) - fails}/{len(CASES)} passed")
    if fails:
        sys.exit(1)


if __name__ == "__main__":
    main()

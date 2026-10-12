"""
_warp_guards.py — pure-Python ports of WARP's safety / scoring logic.

These ports exist for testing. The C++ implementations in
`LlmSummarizer.cpp`, `EntityStore.cpp`, `ActivityStore.h` and
`ContextInference.cpp` remain the source of truth; the Python ports are
kept bit-identical so the test suite can exercise the logic without a
C++ test harness (the C++ harness lives in tests/cpp).

Drift between the C++ and Python implementations is caught by
`tests/python/test_discourse_stops_sync.py` and
`tests/python/test_recency_constants_sync.py`.

Symbols mirrored here:

* `DISCOURSE_STOPS`           ← LlmSummarizer.cpp::DiscourseStops()
* `is_hallucination`          ← LlmSummarizer.cpp::IsHallucination()
* `is_near_copy`              ← LlmSummarizer.cpp::IsNearCopyOfExisting()
* `is_prompt_echo`            ← LlmSummarizer.cpp::IsPromptEcho()
* `strip_think_blocks`        ← inline `<think>...</think>` stripper
* `strip_im_markers`          ← inline `<|im_*|>` stripper
* `compute_recency_score`     ← EntityStore.cpp::LegacyRecencyScore()
* `decay_add` / `decay_at` / `decay_key` / `blended_prior`
                              ← EntityStore.cpp (DecayAdd, DecayAt, PriorFor) + StoreUtil::DecayKey
* `apply_inference_boost`     ← ContextInference.cpp::ApplyInferenceBoost()
"""

from __future__ import annotations

import math
import re
from typing import Iterable, List, Sequence

# =====================================================================
# DiscourseStops — keep in lockstep with LlmSummarizer.cpp
# =====================================================================
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


# =====================================================================
# Tokenizer (matches the inline tokenization in LlmSummarizer.cpp)
# =====================================================================
def tokenize(text: str) -> List[str]:
    """Yield lowercase >= 4-char alnum runs, matching the C++ behaviour."""
    out: List[str] = []
    cur: List[str] = []
    for c in text:
        if c.isalnum():
            cur.append(c.lower())
        else:
            if len(cur) >= 4:
                out.append("".join(cur))
            cur = []
    if len(cur) >= 4:
        out.append("".join(cur))
    return out


def build_input_tokens(
    items: Sequence[tuple],  # list of (app, title)
    existing: Sequence[str],
) -> set:
    """Build the lower-cased token set that `is_hallucination` uses as
    the grounding vocabulary.  Matches the C++ assembly:
        for it in items: addLowerTokens(it.app); addLowerTokens(it.title); ...
        for l in existing: addLowerTokens(l);
    """
    toks: set = set()
    for app, title in items:
        for s in (app, title, title):  # rawTitle ~= title in tests
            toks.update(tokenize(s))
    for line in existing:
        toks.update(tokenize(line))
    return toks


# =====================================================================
# IsHallucination — every topic-bearing token must be grounded
# =====================================================================
def is_hallucination(line: str, input_tokens: set) -> bool:
    """Returns True if `line` contains a >= 4-char topic-bearing token
    that does NOT appear in `input_tokens` (and is not in
    `DISCOURSE_STOPS`, and is not a prefix/suffix of an input token).
    """
    for tok in tokenize(line):
        if tok in DISCOURSE_STOPS:
            continue
        if tok in input_tokens:
            continue
        fuzzy_ok = False
        for itok in input_tokens:
            if len(itok) >= 4 and (itok in tok or tok in itok):
                fuzzy_ok = True
                break
        if fuzzy_ok:
            continue
        return True
    return False


# =====================================================================
# IsNearCopyOfExisting — reject when the model parrots back its hint
# =====================================================================
_NEAR_COPY_OPENERS = (
    "user is ", "they are ", "the user is ",
    "currently ", "right now ",
)


def _normalize_for_compare(s: str) -> str:
    """Lowercase + strip non-alnum + collapse whitespace."""
    return re.sub(r"\s+", " ",
                  re.sub(r"[^A-Za-z0-9]+", " ", s.lower())).strip()


def _strip_opener(s: str) -> str:
    for op in _NEAR_COPY_OPENERS:
        if s.startswith(op):
            return s[len(op):]
    return s


def is_near_copy(line: str, existing: Sequence[str]) -> bool:
    nl = _normalize_for_compare(line)
    if not nl:
        return False
    nlo = _strip_opener(nl)
    for e in existing:
        ne = _normalize_for_compare(e)
        if not ne:
            continue
        if nl == ne:
            return True
        if nlo == ne:
            return True
    return False


# =====================================================================
# IsPromptEcho — reject obvious instruction-text echoes
# =====================================================================
_PROMPT_ECHO_PHRASES = (
    "(1-3 lines", "(1 to 3 sentence", "(1 to 3 lines",
    "refined summary", "candidate summary", "draft notes",
    "polished prose", "most-used first", "most-focused first",
    "activity facts", "apps and windows open right now",
    "% of focus", "[major]", "[present]", "[minor]",
)


def is_prompt_echo(line: str) -> bool:
    lc = line.lower()
    for phrase in _PROMPT_ECHO_PHRASES:
        if phrase in lc:
            return True
    # "app=..." AND "title=..." both present -> structured-item echo
    if "app=" in lc and "title=" in lc:
        return True
    return False


# =====================================================================
# Post-processing strippers (matches LlmSummarizer.cpp post-decode)
# =====================================================================
def strip_think_blocks(decoded: str) -> str:
    """Strip <think>...</think> blocks, mirroring the C++ paired+dangling+orphan logic."""
    # paired form first
    while True:
        op = decoded.find("<think>")
        if op == -1:
            break
        cl = decoded.find("</think>", op)
        if cl == -1:
            # dangling <think> -- drop everything from here
            decoded = decoded[:op]
            break
        decoded = decoded[:op] + decoded[cl + len("</think>"):]
    # orphan closing tag
    orphan = decoded.find("</think>")
    if orphan != -1:
        decoded = decoded[orphan + len("</think>"):]
    return decoded


def strip_im_markers(decoded: str) -> str:
    """Strip from the first <|im_end|> / <|im_start|> marker onwards."""
    im_end = decoded.find("<|im_end|>")
    if im_end != -1:
        decoded = decoded[:im_end]
    im_start = decoded.find("<|im_start|>")
    if im_start != -1:
        decoded = decoded[:im_start]
    return decoded


# =====================================================================
# Legacy v1 recency_score — keep constants in lockstep with EntityStore.cpp
# (evaluated at QUERY time since v6; it used to be frozen at write time)
# =====================================================================
TAU_SECONDS = 172800.0     # 2 days
MAX_SCORE   = 200.0
SCORE_CAP   = 255.0


def compute_recency_score(
    now: int,
    last_open_ts: int,
    open_count_7d: float,
) -> float:
    """Pure-Python port of EntityStore::LegacyRecencyScore."""
    if last_open_ts <= 0:
        return 0.0
    delta = max(0, now - last_open_ts)   # C++ clamps negative clock skew too
    decay = math.exp(-delta / TAU_SECONDS)
    score = MAX_SCORE * decay
    score += math.log(1.0 + max(0.0, open_count_7d)) * 5.0
    if score > SCORE_CAP:
        score = SCORE_CAP
    if score < 0.0:
        score = 0.0
    return score


# =====================================================================
# Decayed engagement (RANK-1 / RANK-2) — keep in lockstep with
# ActivityStore.h (StoreUtil constants) and EntityStore.cpp.
#
# Each entity keeps an exponentially-decayed accumulator (S, t0) per
# horizon.  value(now) = S * exp(-(now - t0) / tau); the stored ranking
# key ln(S) + t0/tau orders entities identically for EVERY `now`.
# =====================================================================
TAU_SHORT_SECONDS = 3.0 * 86400.0
TAU_LONG_SECONDS  = 30.0 * 86400.0
ENGAGEMENT_WEIGHTS = {
    "kWOpenPassive": 0.3,
    "kWOpenExplicit": 1.5,
    "kWEdit": 3.0,
    "kWActivePerMin": 1.0,
    "kWVisit": 0.5,
    "kWNewDay": 2.0,
    "kMaxActiveMinutesPerSession": 60.0,
}


def decay_add(s: float, t0: int, w: float, t: int, tau: float):
    """Port of EntityStore.cpp::DecayAdd -> returns the new (s, t0)."""
    if w <= 0.0:
        return s, t0
    if s <= 0.0 or t0 == 0:
        return w, t
    if t >= t0:
        return s * math.exp(-(t - t0) / tau) + w, t
    return s + w * math.exp(-(t0 - t) / tau), t0


def decay_at(s: float, t0: int, now: int, tau: float) -> float:
    """Port of EntityStore.cpp::DecayAt."""
    if s <= 0.0 or t0 == 0:
        return 0.0
    return s * math.exp(-max(0, now - t0) / tau)


def decay_key(s: float, t0: int, tau: float) -> float:
    """Port of StoreUtil::DecayKey (time-independent ordering key)."""
    if s <= 0.0:
        return -1.0e300
    return math.log(s) + t0 / tau


def blended_prior(short_term: float, long_term: float) -> float:
    """Port of EntityStore.cpp::PriorFor("blend")."""
    return 0.7 * short_term + 0.3 * long_term * (TAU_SHORT_SECONDS / TAU_LONG_SECONDS)


def percentile(sorted_priors: Sequence[float], prior: float) -> float:
    """Port of EntityStore::Percentile (rank / n, 0 for no engagement)."""
    if prior <= 0.0:
        return 0.0
    if not sorted_priors:
        return 0.5
    import bisect
    return min(1.0, max(0.0, bisect.bisect_right(sorted_priors, prior) / len(sorted_priors)))


# =====================================================================
# ApplyInferenceBoost — bounded tie-breaker (INTEL-4):
#   weighted = focus * (1 + 0.5 * short-horizon percentile)   in [1x, 1.5x]
# Matches ContextInference.cpp::ApplyInferenceBoost.
# =====================================================================
def compute_boost(score: float) -> float:
    return 1.0 + 0.5 * min(1.0, max(0.0, score))


def apply_inference_boost(
    bag: List[dict],
    lookup,                  # callable(key) -> dict | None  ({"found": bool, "score": float})
    key_of,                  # callable(entry) -> str | ""
) -> None:
    """In-place boost.  Each entry must have `totalFocusSecs` (int).
    Sets `weightedFocusSecs` (float).
    """
    # Seed weighted = raw for every entry.
    for entry in bag:
        entry["weightedFocusSecs"] = float(entry["totalFocusSecs"])

    if lookup is None or not bag:
        return

    for entry in bag:
        key = key_of(entry)
        if not key:
            continue
        rec = lookup(key)
        if rec is None or not rec.get("found"):
            continue
        entry["weightedFocusSecs"] = float(entry["totalFocusSecs"]) * compute_boost(rec.get("score", 0.0))


# =====================================================================
# Convenience: re-export normalize for downstream tests
# =====================================================================
normalize_for_compare = _normalize_for_compare

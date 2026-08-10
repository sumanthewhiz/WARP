"""
_rich_listing_ref.py — Python port of the rich deterministic listing
composer added to ContextInference.cpp in v5.17 (CleanPhrase +
ComposeRichListing).

The C++ implementation in ContextInference.cpp is the source of truth;
this port is kept faithful so the test suite can pin the behaviour
without a C++ harness.  Verified bit-for-bit against the C++ via a
standalone g++ build during development.
"""
from __future__ import annotations

import re
from typing import List

_EXT = {
    "cpp","h","hpp","cc","cxx","cs","java","kt","py","rb","pl","php","js",
    "mjs","cjs","jsx","ts","tsx","go","rs","swift","m","mm","dart","sh",
    "bash","zsh","ps1","psm1","html","htm","css","scss","sass","less","xml",
    "json","yaml","yml","toml","ini","cfg","conf","sql","graphql","vue",
    "svelte","r","jl","lua","ex","exs","erl","hs","clj","docx","doc","xlsx",
    "xls","pptx","ppt","pdf","txt","md","rst","tex","one","csv","tsv","sln",
    "vcxproj","png","jpg","jpeg","gif","svg",
}
_LEAD_DROP = {"a", "an", "the"}
_DANGLE = {
    "to","the","a","an","of","for","in","on","and","or","with","from","f",
    "vs","ch","p99","v1","v2","v3","v4","v5","draft","is","at","by",
}
_LEAD_PUNCT = set("#@*:>|- ")


def _strip_trailing_ext(s: str) -> str:
    dot = s.rfind(".")
    if dot <= 0:
        return s
    if s[dot + 1:].lower() in _EXT:
        return s[:dot]
    return s


def clean_phrase(title: str, max_words: int = 4) -> str:
    t = _strip_trailing_ext(title)
    # snake/kebab/path -> space; camelCase boundary -> space
    spaced = []
    for i, c in enumerate(t):
        if c in "_-/\\":
            spaced.append(" ")
        else:
            if i > 0 and c.isupper() and t[i - 1].islower():
                spaced.append(" ")
            spaced.append(c)
    s = "".join(spaced)
    # strip leading re:/fwd:/fw:
    lo = s.lower()
    for m in ("re:", "fwd:", "fw:"):
        if lo.startswith(m):
            s = s[len(m):]
            break
    # strip leading punctuation noise
    p = 0
    while p < len(s) and s[p] in _LEAD_PUNCT:
        p += 1
    s = s[p:]
    # tokenize on whitespace
    words = s.split()
    # drop leading article / punctuation-only tokens
    while words:
        stripped = re.match(r"[A-Za-z0-9]*", words[0]).group(0)
        if not stripped or stripped.lower() in _LEAD_DROP:
            words = words[1:]
        else:
            break
    words = words[:max_words]
    # drop dangling trailing connector/version tokens
    while words and words[-1].lower() in _DANGLE:
        words = words[:-1]
    return " ".join(words).strip()


def compose_rich_listing(bag: List[dict], category: str,
                         theme_hint: List[str]) -> List[str]:
    """bag: list of {friendlyName, bestTitle, rawTitle}."""
    if not bag:
        return list(theme_hint)

    phrases, lowers = [], []
    for a in bag:
        src = a.get("bestTitle") or a.get("rawTitle") or ""
        p = clean_phrase(src)
        if len(p) < 2:
            continue
        lo = p.lower()
        if any(lo in s or s in lo for s in lowers):
            continue
        phrases.append(p)
        lowers.append(lo)
        if len(phrases) >= 3:
            break

    if not phrases:
        return list(theme_hint)

    verb = ("researching" if category == "websites"
            else "working across" if category == "apps"
            else "working on")

    if len(phrases) == 1:
        body = phrases[0]
    elif len(phrases) == 2:
        body = f"{phrases[0]} and {phrases[1]}"
    else:
        body = f"{phrases[0]}, {phrases[1]} and {phrases[2]}"

    apps = []
    for a in bag:
        fn = a.get("friendlyName", "")
        if fn and fn not in apps:
            apps.append(fn)
        if len(apps) >= 2:
            break
    if len(apps) == 1:
        app_clause = f" in {apps[0]}"
    elif len(apps) >= 2:
        app_clause = f" across {apps[0]} and {apps[1]}"
    else:
        app_clause = ""

    return [f"User is {verb} {body}{app_clause}"]

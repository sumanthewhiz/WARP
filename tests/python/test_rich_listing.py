"""
Tests for the v5.17 rich deterministic listing composer
(ContextInference.cpp::ComposeRichListing + CleanPhrase).

The expected strings here were validated bit-for-bit against the C++
implementation via a standalone g++ build of the real source during
development; this suite pins them as a regression guard and documents
the intended behaviour.

Also includes a lightweight source-presence check that fails loudly if
the C++ function is renamed/removed without updating this port.
"""
import pathlib

import pytest

from _rich_listing_ref import clean_phrase, compose_rich_listing


# ---------------------------------------------------------------------
# clean_phrase: title -> short human-readable phrase
# ---------------------------------------------------------------------
class TestCleanPhrase:
    @pytest.mark.parametrize("raw,expected", [
        ("auth_middleware.cpp", "auth middleware"),
        ("login_handler.ts", "login handler"),
        ("session_store.go", "session store"),
        ("ContextInference.cpp", "Context Inference"),
        ("msbuild WARP.sln", "msbuild WARP"),
        ("Performance Review - Self Assessment", "Performance Review Self Assessment"),
        ("Re: Q3 OKR sign-off needed", "Q3 OKR sign off"),
        ("#incidents - prod alert ack", "incidents prod alert ack"),
        ("Search Relevance Spec v4", "Search Relevance Spec"),   # trailing v4 dropped
        ("kafka_consumer.go", "kafka consumer"),
    ])
    def test_clean_phrase(self, raw, expected):
        assert clean_phrase(raw) == expected

    def test_caps_at_four_words(self):
        assert clean_phrase("one two three four five six") == "one two three four"

    def test_drops_leading_article(self):
        assert clean_phrase("The Rust Book") == "Rust Book"

    def test_empty(self):
        assert clean_phrase("") == ""


# ---------------------------------------------------------------------
# compose_rich_listing: bag -> "User is <verb> <p1>, <p2> and <p3> ..."
# ---------------------------------------------------------------------
def _bag(*triples):
    return [{"friendlyName": fn, "bestTitle": bt, "rawTitle": rt}
            for fn, bt, rt in triples]


VALIDATED = [
    (
        "code-auth", "files",
        _bag(("Visual Studio Code", "auth_middleware.cpp", "x"),
             ("Visual Studio Code", "login_handler.ts", "x"),
             ("Visual Studio Code", "session_store.go", "x"),
             ("Visual Studio Code", "jwt_validator.py", "x")),
        "User is working on auth middleware, login handler and session store in Visual Studio Code",
    ),
    (
        "research-vector", "websites",
        _bag(("Microsoft Edge", "FAISS vs HNSW benchmark", "x"),
             ("Microsoft Edge", "pgvector documentation", "x"),
             ("Microsoft Edge", "Qdrant vs Milvus comparison", "x")),
        "User is researching FAISS vs HNSW benchmark, pgvector documentation and Qdrant vs Milvus comparison in Microsoft Edge",
    ),
    (
        "comms", "apps",
        _bag(("Outlook", "Re: Q3 OKR sign-off needed", "x"),
             ("Microsoft Teams", "Leads sync - action items", "x"),
             ("Slack", "#incidents - prod alert ack", "x")),
        "User is working across Q3 OKR sign off, Leads sync action items and incidents prod alert ack across Outlook and Microsoft Teams",
    ),
    (
        "microservice", "files",
        _bag(("Visual Studio Code", "kafka_consumer.go", "x"),
             ("Visual Studio Code", "rabbitmq_config.yaml", "x"),
             ("Visual Studio Code", "sqs_publisher.py", "x")),
        "User is working on kafka consumer, rabbitmq config and sqs publisher in Visual Studio Code",
    ),
    (
        "single-doc", "files",
        _bag(("Word", "Performance Review - Self Assessment", "x")),
        "User is working on Performance Review Self Assessment in Word",
    ),
]


@pytest.mark.parametrize("name,cat,bag,expected",
                         VALIDATED, ids=[c[0] for c in VALIDATED])
def test_compose_rich_listing(name, cat, bag, expected):
    out = compose_rich_listing(bag, cat, ["(theme hint)"])
    assert out == [expected], f"{name}: got {out!r}"


def test_empty_bag_returns_theme_hint():
    assert compose_rich_listing([], "all", ["Working on X"]) == ["Working on X"]


def test_dedup_contained_phrases():
    # "Search Relevance" is contained in "Search Relevance Spec" -> keep one
    bag = _bag(("Word", "Search Relevance Spec", "x"),
               ("Word", "Search Relevance", "x"))
    out = compose_rich_listing(bag, "files", ["hint"])
    assert "search relevance spec" in out[0].lower()
    # the second (contained) phrase must not appear as a separate item
    assert out[0].lower().count("search relevance") == 1


def test_verb_by_category():
    bag = _bag(("Edge", "Some Page", "x"))
    assert compose_rich_listing(bag, "websites", ["h"])[0].startswith("User is researching")
    assert compose_rich_listing(bag, "apps", ["h"])[0].startswith("User is working across")
    assert compose_rich_listing(bag, "files", ["h"])[0].startswith("User is working on")


# ---------------------------------------------------------------------
# Source-presence guard: the C++ function must still exist.
# ---------------------------------------------------------------------
def test_cpp_function_present():
    repo = pathlib.Path(__file__).parent.parent.parent
    cpp = (repo / "ContextInference.cpp").read_text(encoding="utf-8", errors="replace")
    assert "ComposeRichListing(" in cpp, (
        "ComposeRichListing not found in ContextInference.cpp -- if it was "
        "renamed, update tests/python/_rich_listing_ref.py and this test."
    )
    assert "CleanPhrase(" in cpp, "CleanPhrase helper missing from ContextInference.cpp"

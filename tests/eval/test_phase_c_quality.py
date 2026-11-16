import json

import pytest

from .make_phase_c_corpus import build
from .phase_c_quality import CORPUS, GATES, bootstrap_group_delta, independent_model_check, load_jsonl, metrics


def test_corpus_is_reproducible_synthetic_and_group_disjoint():
    cases = load_jsonl(CORPUS)
    assert cases == build()
    tune = {case["group"] for case in cases if case["split"] == "tune"}
    heldout = {case["group"] for case in cases if case["split"] == "heldout"}
    assert tune and len(heldout) >= 10 and not tune.intersection(heldout)
    for case in cases:
        assert case["synthetic"]
        ids = {candidate["id"] for candidate in case["candidates"]}
        assert len(ids) == len(case["candidates"]) and ids == set(case["labels"]["relevance"])
        assert any(case["labels"]["relevance"].values())
        assert all(candidate["key"].startswith("C:\\WarpSynthetic\\") for candidate in case["candidates"])


def test_metrics_penalize_missed_unseen_relevant_candidates():
    scores = metrics(["irrelevant", "seen"], {"seen": 1, "unseen": 3})
    assert scores["mrr10"] == .5
    assert scores["recall10"] == .5
    assert 0 < scores["ndcg10"] < .2
    assert metrics(["unseen", "seen"], {"seen": 1, "unseen": 3})["ndcg10"] == 1


def test_paired_resampling_counts_projects_not_repeated_events():
    gates = json.loads(GATES.read_text(encoding="utf-8"))
    rows = [{"group": "same-session", "baseline": {"ndcg10": 0.3}, "context": {"ndcg10": 0.5}}] * 100
    result = bootstrap_group_delta(rows, "baseline", "context", gates)
    assert result["independent_groups"] == 1
    assert result["mean"] == pytest.approx(.2)
    assert bootstrap_group_delta([], "baseline", "context", gates)["mean"] is None


def test_independent_grounding_audit_rejects_an_accepted_invented_id():
    observed = {
        "model_accepted": True,
        "model_raw": '{"abstain":false,"tasks":[{"entities":["e999"],"label_entity":"e999","evidence_refs":["v999"],"roles":[]}]}',
        "context": {"entities": [], "tasks": []},
    }
    assert independent_model_check(observed) > 0
    observed["model_raw"] = '{"abstain":true,"tasks":[]}'
    assert independent_model_check(observed) == 0

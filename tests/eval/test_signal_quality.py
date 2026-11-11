import copy
import math
import json

import pytest

from .signal_quality import (
    aggregate, by_id, evaluate, load_rows, DEFAULT_CORPUS, matched_boundaries,
    membership_pairs, paired_comparison, percentile, retrieval_scores, snapshot_valid,
    main,
)

pytestmark = pytest.mark.l3


def snapshot(**overrides):
    value = {
        "timestamp": 1000, "window_seconds": 900, "summary": [], "summary_files": [],
        "summary_websites": [], "summary_apps": [], "current_entities": [],
        "focus_seconds": 0, "foreground_exposure_seconds": 0, "passive_seconds": 0,
        "idle_seconds": 0, "unknown_attention_seconds": 0,
    }
    value.update(overrides)
    return value


def test_default_corpus_is_frozen_labeled_and_unique():
    cases = load_rows(DEFAULT_CORPUS)
    assert len(by_id(cases)) == len(cases) >= 20
    for case in cases:
        assert case["split"] == "test" and case["group_id"]
        ids = {signal["id"] for signal in case["signals"] if signal["type"] not in {"purge", "clear"}}
        assert ids == case["expected"]["accepted"].keys()


def test_retrieval_uses_relevance_grades_and_full_relevant_set():
    relevance = {"a": 3, "b": 1, "unseen": 2}
    perfect = retrieval_scores(["a", "unseen", "b"], relevance, 2)
    assert perfect["ndcg_at_10"] == pytest.approx(1)
    assert perfect["mrr_at_10"] == 1
    assert perfect["recall_at_k"] == pytest.approx(2 / 3)
    wrong = retrieval_scores(["noise", "b"], relevance)
    assert wrong["mrr_at_10"] == .5
    assert wrong["recall_at_k"] == pytest.approx(1 / 3)
    assert wrong["ndcg_at_10"] < .2


def test_no_labels_is_unmeasured_and_duplicates_cannot_inflate_search():
    assert all(v is None for v in retrieval_scores(["a"], {"a": 0}).values())
    with pytest.raises(ValueError, match="duplicate"):
        retrieval_scores(["a", "a"], {"a": 3})
    for invalid in (math.nan, math.inf, -1, True):
        with pytest.raises(ValueError):
            retrieval_scores(["a"], {"a": invalid})


def test_latency_percentiles_and_micro_averaged_ratios():
    assert percentile([10, 20, 30, 40], .5) == 25
    assert percentile([10, 20, 30, 40], .95) == pytest.approx(38.5)
    assert aggregate("capture.action_precision", [(1, 1), (0, 9)]) == .1
    assert aggregate("capture.action_precision", []) is None


def test_window_schema_rejects_fabricated_time_and_wrong_types():
    assert snapshot_valid(snapshot())
    assert not snapshot_valid(snapshot(focus_seconds=600, foreground_exposure_seconds=0))
    assert not snapshot_valid(snapshot(focus_seconds=True, foreground_exposure_seconds=1))
    assert not snapshot_valid(snapshot(unknown_attention_seconds=-1))


def test_missing_observations_and_unlabeled_decisions_fail_closed():
    case = {"id": "a", "as_of": 1000, "expected": {"accepted": {"x": True}}}
    with pytest.raises(ValueError, match="case mismatch"):
        evaluate([case], [])
    with pytest.raises(ValueError, match="every non-control"):
        evaluate([case], [{"id": "a", "decisions": []}])
    with pytest.raises(ValueError, match="duplicate"):
        by_id([case, case])


def test_unmeasured_metrics_do_not_become_perfect_scores():
    result = evaluate([{"id": "a", "as_of": 1000}], [{"id": "a", "snapshot": snapshot()}])
    assert result["status"] == "incomplete"
    assert result["metrics"]["search.ndcg_at_10"]["value"] is None
    assert result["metrics"]["grounding.supported_claim_rate"]["status"] == "not_measured"
    assert result["metrics"]["cost.battery_mwh"]["status"] == "not_measured"


def test_time_identity_action_and_privacy_regressions_are_failures():
    case = {"id": "a", "as_of": 1000, "expected": {
        "accepted": {"x": False}, "resolved_keys": {"x": ""},
        "cross_user_ids": ["x"], "excluded_ids": ["x"], "allowed_entity_keys": ["right"],
        "allowed_actions": {"right": ["open"]}, "durations": {"focus_seconds": 0},
        "session_attention_seconds": 600,
    }}
    observation = {"id": "a", "decisions": [{"id": "x", "accepted": True, "resolved_key": "wrong"}],
                   "snapshot": snapshot(focus_seconds=600, foreground_exposure_seconds=600,
                                        current_entities=[{"entity_key": "wrong", "entity_type": "file",
                                                           "active_secs_in_window": 600, "observed_action": "edit"}]),
                   "sessions": [{"active_secs": 1200, "passive_secs": 0}]}
    result = evaluate([case], [observation])
    assert result["status"] == "failed"
    assert result["metrics"]["identity.false_joins"]["value"] == 1
    assert result["metrics"]["temporal.overlap_excess_seconds"]["value"] == 600
    assert result["metrics"]["grounding.action_claim_errors"]["value"] == 1
    assert result["metrics"]["privacy.cross_user_inclusions"]["value"] == 1


def test_membership_pairs_and_boundary_matching_are_one_to_one():
    assert membership_pairs({"a": 1, "b": 1, "c": 2}) == {("a", "b")}
    assert matched_boundaries([9, 10, 11], [10], 2) == 1
    assert matched_boundaries([10, 30], [10, 20], 1) == 1


def test_model_claims_require_independent_support_labels():
    case = {"id": "a", "as_of": 1000, "expected": {
        "allowed_entity_keys": ["e1"], "required_evidence_ids": ["ev1", "ev2"],
        "claim_labels": {"c1": True, "c2": False},
    }}
    observation = {"id": "a", "model": {"entity_ids": ["e1", "invented"], "evidence_ids": ["ev1"],
                                      "claims": [{"id": "c1", "kind": "task"}, {"id": "c2", "kind": "action"}]}}
    result = evaluate([case], [observation])
    assert result["metrics"]["grounding.supported_claim_rate"]["value"] == .5
    assert result["metrics"]["grounding.unsupported_action_rate"]["value"] == 1
    assert result["metrics"]["grounding.invalid_entity_ids"]["value"] == 1
    assert result["metrics"]["grounding.required_evidence_recall"]["value"] == .5
    del case["expected"]["claim_labels"]["c2"]
    with pytest.raises(ValueError, match="every emitted claim"):
        evaluate([case], [observation])


def test_search_cohorts_and_exact_match_gate():
    case = {"id": "a", "group_id": "project1", "split": "test", "as_of": 1000,
            "cohort": {"query": "exact", "seen": False, "language": "en", "application": "Word"},
            "expected": {"search": {"relevance": {"exact": 3}, "exact_match_ids": ["exact"]}}}
    result = evaluate([case], [{"id": "a", "search": {"ranking": ["noise", "exact"], "task_success": False}}])
    assert result["gates"]["search.exact_match_overrides"] == "failed"
    assert result["metrics"]["search.task_success"]["value"] == 0
    assert result["cohorts"]["seen=False"]["queries"] == 1
    assert result["metrics"]["search.mrr_at_10"]["value"] == .5


def test_paired_uncertainty_resamples_groups_not_events():
    def report(value):
        return {"corpus_sha256": "same", "per_case": [
            {"id": str(i), "group_id": "project-a" if i < 10 else "project-b", "split": "test",
             "metrics": {"search.ndcg_at_10": value}} for i in range(11)]}
    comparison = paired_comparison(report(.8), report(.6), iterations=100)
    assert comparison["groups"] == 2
    assert comparison["mean"] == pytest.approx(.2)
    assert comparison["ci95"] == pytest.approx([.2, .2])
    bad = report(.6)
    bad["corpus_sha256"] = "different"
    with pytest.raises(ValueError, match="identical frozen corpus"):
        paired_comparison(report(.8), bad)
    one = report(.8)
    one["per_case"] = one["per_case"][:10]
    one_baseline = report(.6)
    one_baseline["per_case"] = one_baseline["per_case"][:10]
    assert paired_comparison(one, one_baseline)["status"] == "not_measured"
    with pytest.raises(ValueError, match="identical case set"):
        paired_comparison(one, report(.6))


def test_repeats_detect_instability_and_lifecycle_survivors():
    case = {"id": "a", "as_of": 1000, "expected": {"deleted_keys": ["secret"], "reset_required": True}}
    observation = {"id": "a", "snapshot": snapshot(), "lifecycle": {"visible_keys": ["secret"], "reset": False}}
    repeated = copy.deepcopy(observation)
    repeated["snapshot"]["summary"] = ["Changed"]
    result = evaluate([case], [observation], [[repeated]])
    assert result["metrics"]["grounding.stability"]["value"] == 0
    assert result["gates"]["lifecycle.deleted_evidence_survivors"] == "failed"
    assert result["gates"]["lifecycle.resync_correctness"] == "failed"


def test_correlation_and_product_cost_metrics_use_supplied_measurements():
    case = {"id": "a", "as_of": 1000, "expected": {"correlation": {
        "boundaries": [10, 20], "tolerance_seconds": 1,
        "membership": {"a": 1, "b": 1, "c": 2}, "co_use": [["a", "b"]],
        "task_changes": [{"task": "x", "timestamp": 100}, {"task": "y", "timestamp": 150}],
        "abstain": True,
    }}}
    observation = {"id": "a", "correlation": {
        "boundaries": [10, 30], "membership": {"a": 1, "b": 1, "c": 1},
        "co_use": [["a", "b"], ["a", "c"]], "task_changes": [{"task": "x", "timestamp": 125}],
        "abstained": False,
    }, "measurements": {"capture_cpu_percent": 12.5, "model_cpu_ms": 3.2,
                        "working_set_bytes": 4096, "battery_mwh": .5, "package_bytes": 123,
                        "database_growth_bytes": 1024, "search_api_latency_ms": [1, 2, 3, 100],
                        "timeouts": [False, True], "fallbacks": [False, False],
                        "checkpoint_crash_loss_seconds": 2}}
    metrics = evaluate([case], [observation])["metrics"]
    assert metrics["correlation.task_boundary_f1"]["value"] == .5
    assert metrics["correlation.membership_precision"]["value"] == pytest.approx(1 / 3)
    assert metrics["correlation.membership_recall"]["value"] == 1
    assert metrics["correlation.false_co_use_links"]["value"] == 1
    assert metrics["correlation.task_switch_recovery_ms"]["value"] == 25000
    assert metrics["correlation.task_switch_recovery_rate"]["value"] == .5
    assert metrics["correlation.correct_abstention_rate"]["value"] == 0
    assert metrics["cost.search_api_p50_ms"]["value"] == 2.5
    assert metrics["cost.search_api_p95_ms"]["value"] == pytest.approx(85.45)
    assert metrics["cost.timeout_rate"]["value"] == .5
    assert metrics["cost.fallback_rate"]["value"] == 0
    assert metrics["temporal.checkpoint_crash_loss_seconds"]["value"] == 2


def test_held_out_groups_cannot_leak_across_splits():
    cases = [{"id": "train", "group_id": "project1", "split": "train"},
             {"id": "test", "group_id": "project1", "split": "test"}]
    with pytest.raises(ValueError, match="must not cross"):
        evaluate(cases, [{"id": "train"}, {"id": "test"}])


@pytest.mark.parametrize("labels", [{"durations": {"focus_seconds": 10}}, {"search": {"relevance": {"x": 1}}},
                                    {"deleted_keys": ["x"]}, {"claim_labels": {"c": True}}])
def test_missing_labeled_outputs_cannot_selectively_disappear(labels):
    with pytest.raises(ValueError, match="output is missing"):
        evaluate([{"id": "a", "as_of": 1000, "expected": labels}], [{"id": "a"}])


def test_failed_rerun_cannot_leave_a_previous_passing_scorecard(tmp_path):
    corpus = tmp_path / "labels.jsonl"
    corpus.write_text('{"id":"a","as_of":1000}\n', encoding="utf-8")
    observed = tmp_path / "observations.jsonl"
    observed.write_text("invalid json", encoding="utf-8")
    output = tmp_path / "results"
    output.mkdir()
    (output / "scorecard.json").write_text('{"status":"passed"}', encoding="utf-8")
    assert main(["--corpus", str(corpus), "--observations", str(observed), "--output-dir", str(output)]) == 1
    assert json.loads((output / "scorecard.json").read_text())["status"] == "failed"


def test_output_directory_cannot_overwrite_baseline_or_labels(tmp_path):
    labels = tmp_path / "scorecard.json"
    labels.write_text("important input", encoding="utf-8")
    with pytest.raises(SystemExit) as exit_info:
        main(["--corpus", str(labels), "--output-dir", str(tmp_path)])
    assert exit_info.value.code == 2
    assert labels.read_text() == "important input"


def test_measured_profile_does_not_claim_missing_metrics_passed():
    cases = [{"id": "a", "as_of": 1000, "expected": {"durations": {"focus_seconds": 0}}}]
    report = evaluate(cases, [{"id": "a", "snapshot": snapshot()}], gate_profile="measured")
    assert report["gate_profile"] == "measured"
    assert report["status"] == "passed"
    assert "capture.action_precision" not in report["gates"]
    assert report["metrics"]["capture.action_precision"]["status"] == "not_measured"
    assert evaluate([{"id": "a"}], [{"id": "a"}], gate_profile="measured")["status"] == "incomplete"

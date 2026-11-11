"""Local, labeled signal/context evaluation. Missing evidence is never a passing score."""

from __future__ import annotations

import argparse
from collections import defaultdict
from datetime import datetime, timezone
import hashlib
import itertools
import json
import math
from pathlib import Path
import random
import statistics
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[2]
DEFAULT_CORPUS = ROOT / "tests" / "fixtures" / "signal_quality.jsonl"

# name -> (unit, aggregation). Ratios aggregate numerators/denominators, not averages
# of percentages. Search scores are macro-averaged per labeled query.
METRICS = {
    "capture.action_precision": ("ratio", "ratio"),
    "capture.action_recall": ("ratio", "ratio"),
    "identity.resolution_precision": ("ratio", "ratio"),
    "identity.resolution_coverage": ("ratio", "ratio"),
    "identity.ambiguity_rate": ("ratio", "ratio"),
    "identity.false_joins": ("count", "sum"),
    "temporal.duration_mae_seconds": ("seconds", "mean"),
    "temporal.overlap_excess_seconds": ("seconds", "sum"),
    "temporal.boundary_mae_ms": ("milliseconds", "mean"),
    "temporal.checkpoint_crash_loss_seconds": ("seconds", "mean"),
    "temporal.stale_context_age_seconds": ("seconds", "mean"),
    "correlation.task_boundary_f1": ("ratio", "ratio"),
    "correlation.membership_precision": ("ratio", "ratio"),
    "correlation.membership_recall": ("ratio", "ratio"),
    "correlation.false_co_use_links": ("count", "sum"),
    "correlation.task_switch_recovery_ms": ("milliseconds", "mean"),
    "correlation.task_switch_recovery_rate": ("ratio", "ratio"),
    "correlation.correct_abstention_rate": ("ratio", "ratio"),
    "grounding.supported_claim_rate": ("ratio", "ratio"),
    "grounding.invalid_entity_ids": ("count", "sum"),
    "grounding.unsupported_relationship_rate": ("ratio", "ratio"),
    "grounding.unsupported_action_rate": ("ratio", "ratio"),
    "grounding.action_claim_errors": ("count", "sum"),
    "grounding.required_evidence_recall": ("ratio", "ratio"),
    "grounding.schema_validity": ("ratio", "ratio"),
    "grounding.stability": ("ratio", "ratio"),
    "search.ndcg_at_10": ("score", "mean"),
    "search.mrr_at_10": ("score", "mean"),
    "search.recall_at_k": ("ratio", "mean"),
    "search.task_success": ("ratio", "ratio"),
    "search.exact_match_overrides": ("count", "sum"),
    "cost.capture_cpu_percent": ("percent", "mean"),
    "cost.model_cpu_ms": ("milliseconds", "mean"),
    "cost.working_set_bytes": ("bytes", "mean"),
    "cost.battery_mwh": ("mWh", "mean"),
    "cost.package_bytes": ("bytes", "mean"),
    "cost.database_growth_bytes": ("bytes", "mean"),
    "cost.search_api_p50_ms": ("milliseconds", "p50"),
    "cost.search_api_p95_ms": ("milliseconds", "p95"),
    "cost.timeout_rate": ("ratio", "ratio"),
    "cost.fallback_rate": ("ratio", "ratio"),
    "privacy.cross_user_inclusions": ("count", "sum"),
    "privacy.excluded_inclusions": ("count", "sum"),
    "lifecycle.deleted_evidence_survivors": ("count", "sum"),
    "lifecycle.resync_correctness": ("ratio", "ratio"),
}
PHASE_A_GATES = {
    "capture.action_precision": 1,
    "capture.action_recall": 1,
    "identity.resolution_precision": 1,
    "identity.false_joins": 0,
    "temporal.duration_mae_seconds": 0,
    "temporal.overlap_excess_seconds": 0,
    "grounding.invalid_entity_ids": 0,
    "grounding.action_claim_errors": 0,
    "grounding.schema_validity": 1,
    "privacy.cross_user_inclusions": 0,
    "privacy.excluded_inclusions": 0,
    "lifecycle.deleted_evidence_survivors": 0,
    "lifecycle.resync_correctness": 1,
}


def number(value):
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value):
        raise ValueError(f"expected a finite number, got {value!r}")
    return value


def load_json(path):
    def reject(value):
        raise ValueError(f"non-finite JSON number: {value}")

    return json.loads(Path(path).read_text(encoding="utf-8-sig"), parse_constant=reject)


def load_rows(path):
    rows = []
    for line in Path(path).read_text(encoding="utf-8-sig").splitlines():
        if line.strip():
            rows.append(json.loads(line, parse_constant=lambda value: number(value)))
    if not rows:
        raise ValueError("empty corpus/observations")
    return rows


def by_id(rows):
    result = {}
    for row in rows:
        if not isinstance(row, dict) or not isinstance(row.get("id"), str) or not row["id"]:
            raise ValueError("each row needs a nonempty string id")
        if row["id"] in result:
            raise ValueError(f"duplicate id: {row['id']}")
        result[row["id"]] = row
    return result


def percentile(values, probability):
    if not values:
        return None
    ordered = sorted(number(v) for v in values)
    position = (len(ordered) - 1) * probability
    low, high = math.floor(position), math.ceil(position)
    return ordered[low] + (ordered[high] - ordered[low]) * (position - low)


def retrieval_scores(ranking, relevance, k=10):
    if len(ranking) != len(set(ranking)):
        raise ValueError("ranking contains duplicate candidate IDs")
    if not isinstance(k, int) or isinstance(k, bool) or k < 1:
        raise ValueError("recall_k must be a positive integer")
    for grade in relevance.values():
        if not 0 <= number(grade) <= 5:
            raise ValueError("relevance grades must be in [0,5]")
    relevant = {key for key, grade in relevance.items() if grade > 0}
    if not relevant:
        return {"ndcg_at_10": None, "mrr_at_10": None, "recall_at_k": None}

    def dcg(grades):
        return sum((2 ** grade - 1) / math.log2(i + 2) for i, grade in enumerate(grades))

    ideal = dcg(sorted(relevance.values(), reverse=True)[:10])
    return {
        "ndcg_at_10": dcg([relevance.get(key, 0) for key in ranking[:10]]) / ideal,
        "mrr_at_10": next((1 / (i + 1) for i, key in enumerate(ranking[:10]) if key in relevant), 0),
        "recall_at_k": len(set(ranking[:k]) & relevant) / len(relevant),
    }


def membership_pairs(membership):
    if len(membership) > 256:
        raise ValueError("membership labels exceed 256 entities")
    return {
        tuple(sorted((a, b)))
        for a, b in itertools.combinations(membership, 2)
        if membership[a] is not None and membership[a] == membership[b]
    }


def matched_boundaries(actual, expected, tolerance):
    actual, expected = sorted(map(number, actual)), sorted(map(number, expected))
    i = j = matches = 0
    while i < len(actual) and j < len(expected):
        if abs(actual[i] - expected[j]) <= tolerance:
            matches += 1
            i += 1
            j += 1
        elif actual[i] < expected[j]:
            i += 1
        else:
            j += 1
    return matches


def snapshot_valid(snapshot):
    if not isinstance(snapshot, dict):
        return False
    for key in ("summary", "summary_files", "summary_websites", "summary_apps"):
        if not isinstance(snapshot.get(key), list) or not all(isinstance(s, str) for s in snapshot[key]):
            return False
    fields = ("focus_seconds", "foreground_exposure_seconds", "passive_seconds",
              "idle_seconds", "unknown_attention_seconds", "window_seconds")
    if not all(type(snapshot.get(k)) is int and snapshot[k] >= 0 for k in fields):
        return False
    if sum(snapshot[k] for k in (fields[0], fields[2], fields[3], fields[4])) != snapshot[fields[1]]:
        return False
    if snapshot[fields[1]] > snapshot["window_seconds"]:
        return False
    if not isinstance(snapshot.get("current_entities"), list):
        return False
    return all(isinstance(e, dict) and isinstance(e.get("entity_key"), str) and e["entity_key"]
               and e.get("entity_type") in {"file", "app", "url"}
               and type(e.get("active_secs_in_window")) is int and e["active_secs_in_window"] >= 0
               for e in snapshot["current_entities"])


def stable_projection(observation):
    if "model" in observation:
        return observation["model"]
    snapshot = observation.get("snapshot", {})
    return {key: snapshot.get(key) for key in (
        "summary", "summary_files", "summary_websites", "summary_apps", "current_entities",
        "focus_seconds", "foreground_exposure_seconds", "passive_seconds", "idle_seconds",
        "unknown_attention_seconds",
    )}


def evaluate(corpus, observations, repeated=None, gate_profile="phase-a"):
    cases, observed = by_id(corpus), by_id(observations)
    if cases.keys() != observed.keys():
        raise ValueError(f"case mismatch: missing={sorted(cases.keys() - observed.keys())}, extra={sorted(observed.keys() - cases.keys())}")
    repeats = [by_id(rows) for rows in (repeated or [])]
    if any(rows.keys() != cases.keys() for rows in repeats):
        raise ValueError("repeat runs must cover the identical case set")
    samples = defaultdict(list)
    per_case = []
    modes = sorted({row.get("mode", "unspecified") for row in observations})
    splits = defaultdict(set)
    for case in corpus:
        if case.get("group_id"):
            splits[case["group_id"]].add(case.get("split"))
    if any("test" in values and ({"train", "tune"} & values) for values in splits.values()):
        raise ValueError("user/task groups must not cross train/tune and held-out test splits")
    for case_id, case in cases.items():
        observation = observed[case_id]
        gold = case.get("expected", {})
        local = defaultdict(list)
        if any(key in gold for key in ("durations", "window", "allowed_actions", "required_entity_keys", "forbidden_entity_keys")) and "snapshot" not in observation:
            raise ValueError(f"{case_id}: labeled context output is missing")
        if "allowed_entity_keys" in gold and "snapshot" not in observation and "model" not in observation:
            raise ValueError(f"{case_id}: labeled identity output is missing")
        for label, output in (("search", "search"), ("correlation", "correlation"),
                              ("claim_labels", "model"), ("required_evidence_ids", "model"),
                              ("deleted_keys", "lifecycle"), ("reset_required", "lifecycle")):
            if label in gold and output not in observation:
                raise ValueError(f"{case_id}: labeled {output} output is missing; use explicit empty predictions for failed requests")

        def add(name, value, denominator=1):
            if value is None or denominator == 0:
                return
            value, denominator = number(value), number(denominator)
            samples[name].append((value, denominator))
            local[name].append((value, denominator))

        decisions = by_id(observation.get("decisions", []))
        if "accepted" in gold:
            labels = gold["accepted"]
            captured = {key: value for key, value in decisions.items() if value.get("type") not in {"purge", "clear"}}
            if set(labels) != captured.keys() or not all(type(v) is bool for v in labels.values()):
                raise ValueError(f"{case_id}: accepted labels must cover every non-control decision")
            if not all(type(d.get("accepted")) is bool for d in decisions.values()):
                raise ValueError(f"{case_id}: decisions need boolean accepted")
            positives = {key for key, value in labels.items() if value}
            predicted = {key for key, value in captured.items() if value["accepted"]}
            add("capture.action_precision", len(positives & predicted), len(predicted))
            add("capture.action_recall", len(positives & predicted), len(positives))
        if "resolved_keys" in gold:
            known = assigned = correct = false = ambiguous = 0
            for key, expected in gold["resolved_keys"].items():
                if key not in decisions or "resolved_key" not in decisions[key]:
                    raise ValueError(f"{case_id}: missing resolution observation for {key}")
                actual = decisions[key]["resolved_key"]
                assigned += bool(actual)
                known += bool(expected)
                correct += bool(actual) and actual == expected
                false += bool(actual) and actual != expected
                ambiguous += decisions[key].get("ambiguous") is True
            add("identity.resolution_precision", correct, assigned)
            add("identity.resolution_coverage", correct, known)
            add("identity.ambiguity_rate", ambiguous, len(gold["resolved_keys"]))
            add("identity.false_joins", false)

        snapshot = observation.get("snapshot")
        if snapshot is not None:
            add("grounding.schema_validity", int(snapshot_valid(snapshot)))
            if "timestamp" in snapshot:
                add("temporal.stale_context_age_seconds", max(0, number(case["as_of"]) - number(snapshot["timestamp"])))
            for key, expected in gold.get("durations", {}).items():
                if key not in snapshot:
                    raise ValueError(f"{case_id}: missing duration {key}")
                add("temporal.duration_mae_seconds", abs(number(snapshot[key]) - number(expected)))
            entities = snapshot.get("current_entities", [])
            keys = {entity.get("entity_key") for entity in entities}
            if "allowed_entity_keys" in gold:
                add("grounding.invalid_entity_ids", len(keys - set(gold["allowed_entity_keys"])))
            if "required_entity_keys" in gold:
                required = set(gold["required_entity_keys"])
                add("grounding.required_evidence_recall", len(keys & required), len(required))
            if "allowed_actions" in gold:
                errors = sum(e.get("observed_action") not in gold["allowed_actions"].get(e.get("entity_key"), [])
                             for e in entities)
                text = "\n".join(snapshot.get("summary", []))
                errors += sum(token.casefold() in text.casefold() for token in gold.get("forbidden_summary_substrings", []))
                add("grounding.action_claim_errors", errors)
            for key, expected in gold.get("window", {}).items():
                add("temporal.boundary_mae_ms", abs(number(snapshot[key]) - number(expected)) * 1000)
        if "session_attention_seconds" in gold:
            sessions = observation.get("sessions")
            if not isinstance(sessions, list):
                raise ValueError(f"{case_id}: missing session output")
            actual = sum(number(s["active_secs"]) + number(s["passive_secs"]) for s in sessions)
            expected = number(gold["session_attention_seconds"])
            add("temporal.duration_mae_seconds", abs(actual - expected))
            add("temporal.overlap_excess_seconds", max(0, actual - expected))
        for name in ("cross_user_inclusions", "excluded_inclusions"):
            field = "cross_user_ids" if name.startswith("cross_user") else "excluded_ids"
            if field in gold:
                if any(key not in decisions for key in gold[field]):
                    raise ValueError(f"{case_id}: missing privacy decisions")
                add("privacy." + name, sum(decisions[key]["accepted"] for key in gold[field]))
        if "forbidden_entity_keys" in gold and snapshot is not None:
            keys = {e["entity_key"] for e in snapshot["current_entities"]}
            add("privacy.excluded_inclusions", len(keys & set(gold["forbidden_entity_keys"])))
        if "deleted_keys" in gold and "lifecycle" in observation:
            add("lifecycle.deleted_evidence_survivors", len(set(gold["deleted_keys"]) & set(observation["lifecycle"]["visible_keys"])))
        if "reset_required" in gold and "lifecycle" in observation:
            add("lifecycle.resync_correctness", int(observation["lifecycle"].get("reset") is gold["reset_required"]))

        correlation = observation.get("correlation", {})
        labels = gold.get("correlation", {})
        for field in ("boundaries", "membership", "co_use", "task_changes"):
            if field in labels and field not in correlation:
                raise ValueError(f"{case_id}: missing labeled correlation field {field}")
        if "abstain" in labels and "abstained" not in correlation:
            raise ValueError(f"{case_id}: missing abstention decision")
        if "boundaries" in labels and "boundaries" in correlation:
            matches = matched_boundaries(correlation["boundaries"], labels["boundaries"], labels.get("tolerance_seconds", 5))
            add("correlation.task_boundary_f1", 2 * matches, len(correlation["boundaries"]) + len(labels["boundaries"]))
        if "membership" in labels and "membership" in correlation:
            actual, expected = membership_pairs(correlation["membership"]), membership_pairs(labels["membership"])
            add("correlation.membership_precision", len(actual & expected), len(actual))
            add("correlation.membership_recall", len(actual & expected), len(expected))
        if "co_use" in labels and "co_use" in correlation:
            actual = {tuple(sorted(pair)) for pair in correlation["co_use"]}
            expected = {tuple(sorted(pair)) for pair in labels["co_use"]}
            add("correlation.false_co_use_links", len(actual - expected))
        if "task_changes" in labels and "task_changes" in correlation:
            for change in labels["task_changes"]:
                delays = [number(p["timestamp"]) - number(change["timestamp"]) for p in correlation["task_changes"]
                          if p["task"] == change["task"] and p["timestamp"] >= change["timestamp"]]
                add("correlation.task_switch_recovery_rate", int(bool(delays)))
                if delays:
                    add("correlation.task_switch_recovery_ms", min(delays) * 1000)
        if "abstain" in labels and "abstained" in correlation:
            add("correlation.correct_abstention_rate", int(correlation["abstained"] is labels["abstain"]))

        model = observation.get("model")
        if model is not None:
            if "allowed_entity_keys" in gold:
                add("grounding.invalid_entity_ids", len(set(model["entity_ids"]) - set(gold["allowed_entity_keys"])))
            claim_labels = gold.get("claim_labels")
            if claim_labels is not None:
                claims = by_id(model["claims"])
                if not claims.keys() <= claim_labels.keys():
                    raise ValueError(f"{case_id}: every emitted claim must have an independent support label")
                for key, claim in claims.items():
                    supported = claim_labels[key]
                    if type(supported) is not bool:
                        raise ValueError("claim support labels must be booleans")
                    add("grounding.supported_claim_rate", int(supported))
                    if claim["kind"] in {"relationship", "action"}:
                        add("grounding.unsupported_" + claim["kind"] + "_rate", int(not supported))
            if "required_evidence_ids" in gold:
                required = set(gold["required_evidence_ids"])
                add("grounding.required_evidence_recall", len(required & set(model["evidence_ids"])), len(required))
            valid = (isinstance(model.get("entity_ids"), list) and isinstance(model.get("evidence_ids"), list)
                     and isinstance(model.get("claims"), list)
                     and all(isinstance(v, str) for v in model["entity_ids"] + model["evidence_ids"])
                     and all(isinstance(c, dict) and isinstance(c.get("id"), str) and c["id"]
                             and c.get("kind") in {"action", "relationship", "task"} for c in model["claims"]))
            if valid:
                valid = len({c["id"] for c in model["claims"]}) == len(model["claims"])
            add("grounding.schema_validity", int(valid))
        for repeat in repeats:
            add("grounding.stability", int(stable_projection(observation) == stable_projection(repeat[case_id])))

        if "search" in gold and "search" in observation:
            if not case.get("group_id") or case.get("split") not in {"train", "tune", "test"}:
                raise ValueError("search evaluation needs explicit user/task group_id and split")
            search, truth = observation["search"], gold["search"]
            ranking = search["ranking"]
            for name, value in retrieval_scores(ranking, truth["relevance"], truth.get("recall_k", 10)).items():
                add("search." + name, value)
            if "task_success" in search:
                if type(search["task_success"]) is not bool:
                    raise ValueError("task_success must be a measured boolean outcome")
                add("search.task_success", int(search["task_success"]))
            if "exact_match_ids" in truth:
                overrides = 0
                for key in truth["exact_match_ids"]:
                    if key not in ranking or any(truth["relevance"].get(other, 0) == 0 for other in ranking[:ranking.index(key)]):
                        overrides += 1
                add("search.exact_match_overrides", overrides)
        measured = observation.get("measurements", {})
        for field in ("capture_cpu_percent", "model_cpu_ms", "working_set_bytes", "battery_mwh", "package_bytes", "database_growth_bytes"):
            if field in measured:
                add("cost." + field, measured[field])
        for latency in measured.get("search_api_latency_ms", []):
            add("cost.search_api_p50_ms", latency)
            add("cost.search_api_p95_ms", latency)
        for field, metric in (("timeouts", "timeout_rate"), ("fallbacks", "fallback_rate")):
            for flag in measured.get(field, []):
                if type(flag) is not bool:
                    raise ValueError(f"{field} must contain boolean outcomes")
                add("cost." + metric, int(flag))
        if "checkpoint_crash_loss_seconds" in measured:
            add("temporal.checkpoint_crash_loss_seconds", measured["checkpoint_crash_loss_seconds"])
        per_case.append({"id": case_id, "group_id": case.get("group_id", case_id),
                         "split": case.get("split", "unspecified"), "cohort": case.get("cohort", {}),
                         "metrics": {key: aggregate(key, values) for key, values in local.items()}})

    metrics = {}
    for name, (unit, _) in METRICS.items():
        values = samples[name]
        metrics[name] = {"status": "measured" if values else "not_measured",
                         "value": aggregate(name, values), "unit": unit, "samples": len(values)}
        if not values:
            metrics[name]["reason"] = "No eligible labeled observation/measurement supplied; not a pass."
    gates = {key: ("not_measured" if metrics[key]["value"] is None else
                   "passed" if abs(metrics[key]["value"] - expected) < 1e-9 else "failed")
             for key, expected in PHASE_A_GATES.items()
             if gate_profile == "phase-a" or metrics[key]["value"] is not None}
    for name in ("search.exact_match_overrides", "grounding.unsupported_action_rate", "grounding.unsupported_relationship_rate"):
        if metrics[name]["value"] is not None:
            gates[name] = "passed" if metrics[name]["value"] == 0 else "failed"
    status = "failed" if "failed" in gates.values() else "incomplete" if not gates or "not_measured" in gates.values() else "passed"
    return {"schema_version": 1, "status": status, "scope": modes, "metrics": metrics,
            "gate_profile": gate_profile, "gates": gates, "per_case": per_case, "cohorts": cohort_scores(per_case)}


def aggregate(name, values):
    if not values:
        return None
    mode = METRICS[name][1]
    if mode == "ratio":
        return sum(v for v, _ in values) / sum(n for _, n in values)
    raw = [v for v, _ in values]
    if mode == "sum":
        return sum(raw)
    if mode in {"p50", "p95"}:
        return percentile(raw, 0.5 if mode == "p50" else 0.95)
    return statistics.fmean(raw)


def cohort_scores(cases):
    groups = defaultdict(list)
    for case in cases:
        score = case["metrics"].get("search.ndcg_at_10")
        if score is not None:
            for field, value in case["cohort"].items():
                groups[f"{field}={value}"].append(score)
    return {key: {"queries": len(values), "ndcg_at_10": statistics.fmean(values)} for key, values in sorted(groups.items())}


def paired_comparison(current, baseline, iterations=2000):
    if not current.get("corpus_sha256") or current.get("corpus_sha256") != baseline.get("corpus_sha256"):
        raise ValueError("paired comparison requires the identical frozen corpus")
    old = by_id(baseline["per_case"])
    if set(old) != {row["id"] for row in current["per_case"]}:
        raise ValueError("paired reports must contain the identical case set")
    clusters = defaultdict(list)
    for row in current["per_case"]:
        previous = old.get(row["id"])
        if previous is None or previous["group_id"] != row["group_id"]:
            raise ValueError("paired cases/groups differ")
        a = row["metrics"].get("search.ndcg_at_10")
        b = previous["metrics"].get("search.ndcg_at_10")
        if a is not None and b is not None and row["split"] == previous["split"] == "test":
            clusters[row["group_id"]].append(a - b)
    if len(clusters) < 2:
        return {"status": "not_measured", "reason": "Need at least two independent held-out user/task groups."}
    deltas = [statistics.fmean(values) for values in clusters.values()]
    rng = random.Random(0)
    boot = [statistics.fmean(rng.choices(deltas, k=len(deltas))) for _ in range(iterations)]
    return {"status": "measured", "metric": "group_macro_ndcg_at_10_delta", "groups": len(deltas),
            "mean": statistics.fmean(deltas), "ci95": [percentile(boot, .025), percentile(boot, .975)]}


def markdown(report):
    dirty = " (working tree modified)" if report.get("working_tree_dirty") else ""
    label = "Phase A fixture gates" if report.get("gate_profile", "phase-a") == "phase-a" else "Measured-only gates (partial evaluation)"
    lines = ["# WARP signal quality validation", "", f"**{label}: {report['status'].upper()}**",
             f"Scope: {', '.join(report['scope'])}. Revision: `{report.get('revision', 'unknown')}`{dirty}.",
             f"Model modes: {', '.join(report.get('model_modes', ['unspecified']))}.", "",
             "Synthetic replay tests source rules, storage and deterministic inference, not Windows provider recall.",
             "SLM, search, crash-loss, battery and live-capture results require corresponding labeled observations.",
             "A missing measurement is NOT a passing score; this is not a single composite AI-quality score.", "",
             "| Metric | Value | Samples | Status |", "|---|---:|---:|---|"]
    for name, item in report["metrics"].items():
        value = "N/A" if item["value"] is None else f"{item['value']:.6g}"
        lines.append(f"| `{name}` | {value} {item['unit']} | {item['samples']} | {item['status']} |")
    lines.extend(["", "## Gates", ""])
    lines.extend(f"- `{name}`: **{state}**" for name, state in report["gates"].items())
    if "native_tests" in report:
        tests = report["native_tests"]
        lines.extend(["", f"Native implementation regressions: {tests['run']} run, {tests['failed']} failed."])
    if "comparison" in report:
        lines.extend(["", "## Paired held-out comparison", "", "```json", json.dumps(report["comparison"], indent=2), "```"])
    if report["cohorts"]:
        lines.extend(["", "## Search cohorts", "", "```json", json.dumps(report["cohorts"], indent=2), "```"])
    return "\n".join(lines) + "\n"


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native-bin", type=Path)
    parser.add_argument("--models-dir", type=Path, help="Optional Granite directory parent; Qwen is not enabled.")
    parser.add_argument("--corpus", type=Path, default=DEFAULT_CORPUS)
    parser.add_argument("--observations", type=Path, help="Score labeled live/model/search observations instead of native replay.")
    parser.add_argument("--repeated-observations", type=Path, action="append", default=[])
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--repeat", type=int, default=2)
    parser.add_argument("--baseline", type=Path)
    parser.add_argument("--min-ndcg-gain", type=float)
    parser.add_argument("--max-cohort-regression", type=float)
    parser.add_argument("--require-metric", choices=sorted(METRICS), action="append", default=[])
    parser.add_argument("--gate-profile", choices=("phase-a", "measured"), default="phase-a")
    args = parser.parse_args(argv)
    if args.models_dir and args.observations:
        parser.error("--models-dir applies to native replay, not precomputed observations")
    if not 1 <= args.repeat <= 10:
        parser.error("--repeat must be 1..10")
    if args.min_ndcg_gain is not None and (not args.baseline or not math.isfinite(args.min_ndcg_gain) or args.min_ndcg_gain < 0):
        parser.error("--min-ndcg-gain needs --baseline and a finite preregistered threshold")
    if args.max_cohort_regression is not None and (not args.baseline or not math.isfinite(args.max_cohort_regression) or args.max_cohort_regression < 0):
        parser.error("--max-cohort-regression needs --baseline and a finite nonnegative threshold")
    directory = args.output_dir or ROOT / "tests" / "eval" / "results" / ("signal-quality-" + datetime.now().strftime("%Y%m%d-%H%M%S-%f"))
    directory.mkdir(parents=True, exist_ok=True)
    inputs = [p for p in (args.corpus, args.observations, args.baseline, *args.repeated_observations) if p]
    outputs = [directory / name for name in ("scorecard.json", "scorecard.md")]
    if not args.observations:
        outputs += [directory / name for name in ("native-tests.json", "native-tests.log")]
        outputs += [directory / f"observations-{i + 1}.jsonl" for i in range(args.repeat)]
    for output in outputs:
        for source in inputs:
            if output.resolve() == source.resolve() or (output.exists() and source.exists() and output.samefile(source)):
                parser.error("output artifacts must not overwrite corpus, observations or baseline inputs; choose a different output directory")
    pending = {"schema_version": 1, "status": "running", "started_utc": datetime.now(timezone.utc).isoformat()}
    (directory / "scorecard.json").write_text(json.dumps(pending) + "\n", encoding="utf-8")
    (directory / "scorecard.md").write_text("# WARP signal quality validation\n\nRUNNING - not a passing result.\n", encoding="utf-8")
    try:
        return run_evaluation(args, directory)
    except (ValueError, KeyError, TypeError, OSError, subprocess.SubprocessError) as error:
        failure = {**pending, "status": "failed", "error": str(error)}
        (directory / "scorecard.json").write_text(json.dumps(failure, indent=2) + "\n", encoding="utf-8")
        (directory / "scorecard.md").write_text("# WARP signal quality validation\n\nFAILED - no valid scorecard.\n\n" + str(error) + "\n", encoding="utf-8")
        print(f"Quality validation failed: {error}", file=sys.stderr)
        return 1


def run_evaluation(args, directory):
    corpus = load_rows(args.corpus)
    repeats = [load_rows(path) for path in args.repeated_observations]
    native_tests = None
    native_rc = 0
    if args.observations:
        observations = load_rows(args.observations)
    else:
        if not args.native_bin or not args.native_bin.is_file():
            raise ValueError("provide a built --native-bin, or use tests\\run_signal_quality.ps1")
        unit_path = directory / "native-tests.json"
        with (directory / "native-tests.log").open("w", encoding="utf-8") as log:
            run = subprocess.run([str(args.native_bin), "/timeout", "900", "/report", str(unit_path)], stdout=log, stderr=subprocess.STDOUT, timeout=960)
        native_rc = run.returncode
        native_tests = load_json(unit_path) if unit_path.exists() else {"run": 0, "failed": 1}
        observations = None
        for iteration in range(args.repeat):
            path = directory / f"observations-{iteration + 1}.jsonl"
            command = [str(args.native_bin), "/replay", str(args.corpus), "/output", str(path)]
            if args.models_dir:
                command += ["/models", str(args.models_dir)]
            subprocess.run(command, check=True, timeout=900)
            rows = load_rows(path)
            if observations is None:
                observations = rows
            else:
                repeats.append(rows)
    result = evaluate(corpus, observations, repeats, args.gate_profile)
    result["corpus_sha256"] = hashlib.sha256(args.corpus.read_bytes()).hexdigest()
    result["generated_utc"] = datetime.now(timezone.utc).isoformat()
    result["revision"] = subprocess.run(["git", "-C", str(ROOT), "rev-parse", "HEAD"], capture_output=True, text=True, check=True).stdout.strip()
    result["working_tree_dirty"] = bool(subprocess.run(["git", "-C", str(ROOT), "status", "--porcelain", "--untracked-files=no"],
                                                       capture_output=True, text=True, check=True).stdout.strip())
    result["evaluation_rule_version"] = "phase-a-1"
    result["metrics_code_sha256"] = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
    if args.native_bin and not args.observations:
        with args.native_bin.open("rb") as stream:
            result["native_binary_sha256"] = hashlib.file_digest(stream, "sha256").hexdigest()
    result["model_modes"] = sorted({row.get("model_mode", "externally_supplied") for row in observations})
    if args.models_dir and not args.observations:
        result["model_inputs"] = {}
        for path in sorted((args.models_dir / "granite").glob("*")):
            if path.is_file():
                with path.open("rb") as stream:
                    digest = hashlib.file_digest(stream, "sha256").hexdigest()
                result["model_inputs"][path.name] = {"sha256": digest, "bytes": path.stat().st_size}
    if native_tests is not None:
        result["native_tests"] = native_tests
        result["gates"]["native_regressions"] = "failed" if native_rc or native_tests["failed"] or not native_tests["run"] else "passed"
    for metric in args.require_metric:
        result["gates"]["required:" + metric] = "passed" if result["metrics"][metric]["status"] == "measured" else "not_measured"
    if args.baseline:
        baseline = load_json(args.baseline)
        result["comparison"] = paired_comparison(result, baseline)
        result["cohort_deltas"] = {
            key: row["ndcg_at_10"] - baseline["cohorts"][key]["ndcg_at_10"]
            for key, row in result["cohorts"].items() if key in baseline.get("cohorts", {})
        }
        if args.min_ndcg_gain is not None:
            comparison = result["comparison"]
            result["gates"]["held_out_search_gain"] = (
                "not_measured" if comparison["status"] != "measured" else
                "passed" if comparison["ci95"][0] >= args.min_ndcg_gain else "failed")
        if args.max_cohort_regression is not None:
            deltas = result["cohort_deltas"]
            result["gates"]["search_cohort_regressions"] = "not_measured" if not deltas else (
                "passed" if min(deltas.values()) >= -args.max_cohort_regression else "failed")
    states = result["gates"].values()
    result["status"] = "failed" if "failed" in states else "incomplete" if not result["gates"] or "not_measured" in states else "passed"
    (directory / "scorecard.json").write_text(json.dumps(result, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    (directory / "scorecard.md").write_text(markdown(result), encoding="utf-8")
    print(f"{result['status'].upper()}: {directory / 'scorecard.md'}")
    return {"passed": 0, "failed": 1, "incomplete": 2}[result["status"]]


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (ValueError, KeyError, TypeError, OSError, subprocess.SubprocessError) as error:
        print(f"Quality validation failed: {error}", file=sys.stderr)
        raise SystemExit(1)

"""Evaluate real WARP API/ranking code on a fixed, synthetic labeled candidate corpus."""
import argparse
import hashlib
import json
import math
import platform
import random
import statistics
import subprocess
from pathlib import Path

HERE = Path(__file__).resolve().parent
CORPUS = HERE.parent / "fixtures" / "phase_c_search.jsonl"
GATES = HERE / "phase_c_gates.json"


def load_jsonl(path):
    rows = [json.loads(line) for line in path.read_text(encoding="utf-8-sig").splitlines() if line.strip()]
    ids = [row["id"] for row in rows]
    if len(set(ids)) != len(ids):
        raise ValueError(f"duplicate IDs in {path}")
    return rows


def metrics(order, relevance, k=10):
    gains = [2 ** relevance.get(key, 0) - 1 for key in order[:k]]
    dcg = sum(gain / math.log2(rank + 2) for rank, gain in enumerate(gains))
    ideal = sorted((2 ** value - 1 for value in relevance.values()), reverse=True)[:k]
    idcg = sum(gain / math.log2(rank + 2) for rank, gain in enumerate(ideal))
    positive = sum(value > 0 for value in relevance.values())
    return {
        "ndcg10": dcg / idcg if idcg else 0.0,
        "mrr10": next((1.0 / (i + 1) for i, key in enumerate(order[:k]) if relevance.get(key, 0) > 0), 0.0),
        "recall10": sum(relevance.get(key, 0) > 0 for key in order[:k]) / positive if positive else 0.0,
        "task_success1": float(bool(order) and relevance.get(order[0], 0) > 0),
    }


def bootstrap_group_delta(rows, left, right, gates):
    grouped = {}
    for row in rows:
        grouped.setdefault(row["group"], []).append(row[right]["ndcg10"] - row[left]["ndcg10"])
    means = [statistics.mean(group) for group in grouped.values()]
    if not means:
        return {"mean": None, "lower": None, "upper": None, "independent_groups": 0}
    rng = random.Random(gates["bootstrap_seed"])
    samples = sorted(statistics.mean(rng.choices(means, k=len(means))) for _ in range(gates["bootstrap_samples"]))
    tail = (1 - gates["bootstrap_confidence"]) / 2
    return {"mean": statistics.mean(means), "lower": samples[int(len(samples) * tail)],
            "upper": samples[min(len(samples) - 1, int(len(samples) * (1 - tail)))],
            "independent_groups": len(means)}


def mean_metrics(rows, arm):
    return {key: statistics.mean(row[arm][key] for row in rows) if rows else None
            for key in ("ndcg10", "mrr10", "recall10", "task_success1")}


def independent_model_check(observation):
    """Audit accepted claims separately from the native validator."""
    if not observation.get("model_accepted"):
        return 0
    try:
        model = json.loads(observation.get("model_validated_output") or observation["model_raw"])
        if model.get("abstain"):
            return int(bool(model.get("tasks")))
        cards = {card["id"]: card for card in observation["context"]["entities"]}
        owners = {}
        for task in observation["context"]["tasks"]:
            for key in task["entities"]:
                owners[key] = task["id"]
        faults = 0
        claimed = set()
        for task in model["tasks"]:
            members = task["entities"]
            if not members or task["label_entity"] not in members:
                faults += 1
            parent = set()
            for entity in members:
                if entity not in cards or not cards[entity]["eligible"] or entity in claimed:
                    faults += 1
                    continue
                claimed.add(entity)
                parent.add(owners.get(cards[entity]["entity_key"]))
                if "v" + entity[1:] not in task["evidence_refs"]:
                    faults += 1
            if len(parent) != 1 or None in parent:
                faults += 1
            for role in task["roles"]:
                card = cards.get(role["entity"], {})
                if role["entity"] not in members or "v" + role["entity"][1:] not in role["evidence_refs"]:
                    faults += 1
                if role["role"] == "primary_artifact" and card.get("active_ms", 0) < 30000:
                    faults += 1
                if role["role"] not in ("primary_artifact", "possible_reference", "observed"):
                    faults += 1
        if claimed != {key for key, card in cards.items() if card["eligible"]}:
            faults += 1
        return faults
    except (KeyError, TypeError, ValueError):
        return 1


def evaluate(cases, predictions, gates, with_slm=False):
    indexed = {row["id"]: row for row in predictions}
    if set(indexed) != {case["id"] for case in cases}:
        raise ValueError("prediction IDs must exactly match the requested corpus")
    partitions = {split: {case["group"] for case in cases if case["split"] == split} for split in ("tune", "heldout")}
    if partitions["tune"] & partitions["heldout"]:
        raise ValueError("project/session leakage between tune and heldout")
    rows, failures = [], []
    counters = dict(exact_regressions=0, unseen_recall_regressions=0, fallback_order_changes=0,
                    unsupported_accepted_claims=0, task_boundary_failures=0, task_switch_failures=0,
                    unexpected_api_errors=0, invalid_candidate_sets=0)
    arms = ("baseline", "engagement_only", "deterministic", "slm")
    for case in cases:
        if not case.get("synthetic"):
            raise ValueError("this evaluator only accepts the labeled synthetic corpus")
        observed = indexed[case["id"]]
        row = {"id": case["id"], "group": case["group"], "split": case["split"], "cohorts": case["cohorts"]}
        expected = {candidate["id"] for candidate in case["candidates"]}
        for arm in arms:
            order = observed[arm]
            if len(order) != len(expected) or set(order) != expected:
                counters["invalid_candidate_sets"] += 1
            row[arm] = metrics(order, case["labels"]["relevance"])
        if "exact_match" in case["cohorts"] and row["deterministic"]["mrr10"] < row["baseline"]["mrr10"]:
            counters["exact_regressions"] += 1
        if "unseen_relevant" in case["cohorts"] and row["deterministic"]["recall10"] < row["baseline"]["recall10"]:
            counters["unseen_recall_regressions"] += 1
        if case["labels"]["fallback_order_unchanged"] and observed["baseline"] != observed["deterministic"]:
            counters["fallback_order_changes"] += 1
            failures.append(case["id"] + ": fallback changed order")
        if not observed["features"].get("ok") and case["scenario"] != "purge":
            counters["unexpected_api_errors"] += 1
            failures.append(case["id"] + ": unexpected API error")
        tasks = observed["context"]["tasks"]
        pair = [key.casefold() for key in case["labels"]["forbidden_strong_pair"]]
        if len(tasks) < case["labels"]["minimum_tasks"] or (pair and any(
                all(key in {member.casefold() for member in task["entities"]} for key in pair) for task in tasks)):
            counters["task_boundary_failures"] += 1
        primary = case["labels"]["primary_key"].casefold()
        if primary and not any(task["primary"] and primary in {key.casefold() for key in task["entities"]} for task in tasks):
            counters["task_switch_failures"] += 1
        counters["unsupported_accepted_claims"] += independent_model_check(observed)
        row["model_evaluated"] = observed.get("model_evaluated", False)
        rows.append(row)
    heldout = [row for row in rows if row["split"] == "heldout"]
    gain = bootstrap_group_delta(heldout, "baseline", "deterministic", gates)
    model_rows = [row for row in heldout if row["model_evaluated"]]
    model_gain = bootstrap_group_delta(model_rows, "deterministic", "slm", gates)
    model_predictions = [row for row in predictions if row.get("model_evaluated")]
    if with_slm and {case["id"] for case in cases if case["model_eval"]} != {row["id"] for row in model_predictions}:
        failures.append("The real-model arm omitted a prespecified case.")
    if with_slm and (not model_predictions or not all(row.get("model_loaded") for row in model_predictions)):
        failures.append("The requested real-model arm did not load Qwen for every evaluated case.")
    if any(counters.values()):
        failures.extend(f"{name}={value}" for name, value in counters.items() if value)
    if heldout and (gain["mean"] < gates["minimum_heldout_ndcg10_gain"] or gain["lower"] <= 0):
        failures.append("Held-out deterministic gain did not clear the predeclared minimum and paired group confidence bound.")
    latencies = sorted(row["api_latency_ms"] for row in predictions)
    cohorts = {}
    for name in sorted({name for row in heldout for name in row["cohorts"]}):
        subset = [row for row in heldout if name in row["cohorts"]]
        cohorts[name] = {"cases": len(subset), **{arm: mean_metrics(
            [row for row in subset if row["model_evaluated"]] if arm == "slm" else subset, arm) for arm in arms}}
    model_latencies = sorted(row["model_latency_ms"] for row in model_predictions)
    model_p95 = model_latencies[min(len(model_latencies) - 1, math.ceil(len(model_latencies) * .95) - 1)] if model_latencies else None
    return {
        "schema": "warp.phase-c-scorecard.v1", "passed": not failures, "failures": failures,
        "scope": "Synthetic frozen candidate reranking; not measured real-user relevance or full-disk retrieval recall.",
        "heldout_gate_evaluated": bool(heldout),
        "tune_arms": {arm: mean_metrics([row for row in rows if row["split"] == "tune" and
                                        (arm != "slm" or row["model_evaluated"])], arm) for arm in arms},
        "cases": len(rows), "heldout_cases": len(heldout), "groups": {key: sorted(value) for key, value in partitions.items()},
        "arms": {arm: mean_metrics(heldout, arm) for arm in arms if arm != "slm"},
        "paired_project_bootstrap_ndcg10_gain": gain, "invariants": counters, "cohorts": cohorts,
        "slm": {
            "evaluated_cases": len(model_predictions), "heldout_evaluated_cases": len(model_rows),
            "accepted": sum(row.get("model_accepted", False) for row in model_predictions),
            "abstained": sum(row.get("model_abstained", False) for row in model_predictions),
            "non_abstaining_accepted": sum(row.get("model_accepted", False) and not row.get("model_abstained", False)
                                          for row in model_predictions),
            "incremental_gain": model_gain,
            "paired_subset": {arm: mean_metrics(model_rows, arm) for arm in ("baseline", "deterministic", "slm")},
            "total_latency_p95_ms": model_p95,
            "enable_default": bool(model_gain["mean"] is not None and
                                   model_gain["mean"] >= gates["minimum_slm_incremental_ndcg10_gain_to_enable"] and
                                   model_gain["lower"] > 0 and model_p95 is not None and
                                   model_p95 <= gates["maximum_slm_p95_total_ms_to_enable"] and not failures),
            "not_evaluated_rows": "SLM order equals deterministic fallback, not an actual model measurement.",
        },
        "cost": {"api_p50_ms": statistics.median(latencies),
                 "api_p95_ms": latencies[min(len(latencies) - 1, math.ceil(len(latencies) * .95) - 1)],
                 "measurement": "In-process context+feature API on synthetic scratch databases; excludes IPC and index retrieval."},
        "per_case": rows,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native-bin", type=Path, required=True)
    parser.add_argument("--corpus", type=Path, default=CORPUS)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--models-dir", type=Path)
    parser.add_argument("--with-slm", action="store_true")
    parser.add_argument("--split", choices=("all", "tune", "heldout"), default="all")
    parser.add_argument("--timeout", type=int, default=1800)
    args = parser.parse_args()
    cases = load_jsonl(args.corpus)
    if args.split != "all":
        cases = [case for case in cases if case["split"] == args.split]
    args.output_dir.mkdir(parents=True, exist_ok=True)
    replay = args.output_dir / "inputs.jsonl"
    replay.write_text("".join(json.dumps(case, ensure_ascii=False) + "\n" for case in cases), encoding="utf-8")
    predictions = args.output_dir / "observations.jsonl"
    command = [str(args.native_bin.resolve()), "--search-replay", str(replay.resolve()),
               "--output", str(predictions.resolve()), "--timeout", str(args.timeout)]
    if args.with_slm:
        if not args.models_dir:
            parser.error("--with-slm requires --models-dir")
        command.extend(["--models", str(args.models_dir.resolve()), "--model-eval"])
    with (args.output_dir / "native.log").open("w", encoding="utf-8") as log:
        completed = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, timeout=args.timeout + 30)
    if completed.returncode:
        raise SystemExit(f"Native replay failed ({completed.returncode}); see {args.output_dir / 'native.log'}")
    gates = json.loads(GATES.read_text(encoding="utf-8"))
    report = evaluate(cases, load_jsonl(predictions), gates, args.with_slm)
    sha = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
    report["provenance"] = {"corpus_sha256": sha(args.corpus), "gate_sha256": sha(GATES),
                            "native_binary_sha256": sha(args.native_bin), "platform": platform.platform()}
    (args.output_dir / "scorecard.json").write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(json.dumps({key: report[key] for key in ("passed", "cases", "heldout_cases", "arms",
                                                  "paired_project_bootstrap_ndcg10_gain", "invariants", "slm", "cost", "failures")}, indent=2))
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())

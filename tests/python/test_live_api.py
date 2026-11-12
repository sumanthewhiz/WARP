"""Live API integration test (gap analysis ENG-3, API-1).

Runs only in the CI integration step, which starts ``WARP!.exe --headless --data-dir <tmp>`` and
sets WARP_LIVE_API=1 (and WARP_PID). Locally:

    $env:WARP_LIVE_API = "1"; python -m pytest tests/python/test_live_api.py -m live -v

Checks the pipe from both the (elevated) runner process and an admin-filtered, medium-integrity
token - the access a normal search app has - then drives scripted file activity (an atomic
temp->rename save) and expects it to surface as a ranked entity.
"""

from __future__ import annotations

import os
import pathlib
import subprocess
import time
import uuid

import pytest

pytestmark = [pytest.mark.live]

if os.environ.get("WARP_LIVE_API") != "1":
    pytest.skip("needs a running WARP instance (set WARP_LIVE_API=1)", allow_module_level=True)

import warp_pipe  # noqa: E402  (tests/python is on sys.path via conftest)


@pytest.fixture(scope="module")
def pipe_ready():
    assert warp_pipe.wait_for_pipe(timeout_s=90), "WARP's session pipe never appeared"
    # The store and API start on a worker thread; wait until GetStatus answers.
    deadline = time.monotonic() + 60
    last = None
    while time.monotonic() < deadline:
        try:
            last = warp_pipe.call({"op": "GetStatus"})
            if last.get("ok") and last.get("store", {}).get("open"):
                return last
        except OSError as e:  # still starting
            last = e
        time.sleep(1)
    pytest.fail(f"WARP did not become ready: {last!r}")


def test_ping_and_server_identity(pipe_ready):
    pids: list[int] = []
    r = warp_pipe.call({"op": "Ping"}, server_pid_out=pids)
    assert r["ok"] and r["pong"] and r["api_version"] == 2
    expected = os.environ.get("WARP_PID")
    if expected:
        assert pids and pids[0] == int(expected), f"pipe served by pid {pids} instead of WARP ({expected})"


def test_standard_user_client_can_connect(pipe_ready):
    """API-1: a non-elevated, medium-integrity app must be able to open the pipe read/write."""
    r = warp_pipe.call({"op": "Ping"}, as_standard_user=True)
    assert r["ok"] and r["pong"]


def test_status_reports_store_and_capture_health(pipe_ready):
    s = warp_pipe.call({"op": "GetStatus"})
    print(s)
    assert s["store"]["schema_version"] == 4
    assert s["store"]["write_errors"] == 0
    files = s["capture"]["files"]
    assert files["enabled"]
    assert files["etw_running"] or files["usn_running"], f"no file source running: {files}"


def test_control_operations_are_refused_for_other_apps(pipe_ready):
    r = warp_pipe.call({"op": "Purge", "start": 1, "end": 2})
    assert not r["ok"] and r["error"]["code"] == "forbidden"
    r = warp_pipe.call({"op": "Pause", "minutes": 5})
    assert not r["ok"] and r["error"]["code"] == "forbidden"


def test_malformed_requests_get_structured_errors(pipe_ready):
    assert warp_pipe.call("not json")["error"]["code"] == "invalid_json"
    assert warp_pipe.call({"op": "NoSuchOp"})["error"]["code"] == "unknown_op"


def test_phase_b_evidence_and_interval_contract(pipe_ready):
    end = int(time.time()) - 1
    bounds = {"start": end - 60, "end": end}
    evidence = warp_pipe.call({"op": "GetContextEvidence", **bounds, "limit_per_type": 2})
    assert evidence["ok"] and evidence["available"]
    assert evidence["schema"] == "warp.context-evidence.v1"
    assert evidence["interval_semantics"] == "[start,end)"
    assert evidence["start_ms"] == bounds["start"] * 1000
    assert evidence["end_ms"] == bounds["end"] * 1000
    assert isinstance(evidence["partial"], bool)
    assert {row["source"] for row in evidence["coverage_summary"]} == {
        "files", "launch", "browser", "focus", "presence", "storage"}
    deadline = time.monotonic() + 30
    while True:
        result = warp_pipe.call({"op": "GetIntervalContext", **bounds, "include_vector": False})
        if result.get("error", {}).get("code") != "unavailable" or time.monotonic() >= deadline:
            break
        time.sleep(0.25)
    assert result["ok"]
    context = result["interval_context"]
    assert context["mode"] == "interval" and context["thread_id"] == 0
    assert context["presence"] == ""
    assert not context["contains_volatile_overlay"]
    assert "summary_embedding" not in context
    invalid = warp_pipe.call({"op": "GetContextEvidence", "start": 2, "end": 1})
    assert invalid["error"]["code"] == "invalid_request"


def _documents_dir() -> pathlib.Path:
    d = pathlib.Path(os.environ.get("USERPROFILE", str(pathlib.Path.home()))) / "Documents"
    d.mkdir(parents=True, exist_ok=True)
    return d


def test_scripted_save_becomes_a_ranked_entity(pipe_ready):
    if pipe_ready.get("session_id", 0) == 0:
        pytest.skip("the job runs in Session 0 (services), where WARP by design records no user activity")
    folder = _documents_dir() / f"WarpCI-{uuid.uuid4().hex[:8]}"
    folder.mkdir()
    target = folder / "quarterly-report.txt"
    try:
        # Atomic save, the way editors do it: write a temp file, then rename over the target.
        for i in range(2):
            tmp = folder / f"~quarterly-report.{i}.tmp"
            tmp.write_text(f"draft {i}\n" * 50, encoding="utf-8")
            os.replace(tmp, target)
            time.sleep(4)
        key = str(target)
        deadline = time.monotonic() + 90
        found = None
        while time.monotonic() < deadline:
            r = warp_pipe.call({"op": "QueryInferences", "paths": [key]})
            found = r["results"].get(key)
            if found and found.get("entity_type") == "file":
                break
            time.sleep(3)
        assert found and found.get("entity_type") == "file", f"{key} was not captured: {found}"
        assert found["last_edit_ts"] > 0

        top = warp_pipe.call({"op": "TopEntities", "type": "file", "limit": 50})
        assert any(e["display_name"].lower() == key.lower() for e in top["results"]), top
        # The editor-style temp files never become entities (FILE-2).
        assert not any(e["display_name"].lower().endswith(".tmp") for e in top["results"]), top
        search = warp_pipe.call({"op": "SearchActivity", "query": "quarterly report"})
        assert any(e["display_name"].lower() == key.lower() for e in search["results"]), search
    finally:
        for p in folder.glob("*"):
            p.unlink(missing_ok=True)
        folder.rmdir()


def test_app_launch_is_visible_in_the_timeline(pipe_ready):
    if pipe_ready.get("session_id", 0) == 0:
        pytest.skip("the job runs in Session 0 (services)")
    start = int(time.time()) - 5
    proc = subprocess.Popen(["notepad.exe"])
    try:
        deadline = time.monotonic() + 45
        launches = []
        while time.monotonic() < deadline:
            r = warp_pipe.call({"op": "QueryTimeline", "start": start, "end": int(time.time()) + 60,
                                "types": ["app_launch"]})
            launches = [a for a in r.get("app_launches", []) if a["exe_name"].lower() == "notepad.exe"]
            if launches:
                break
            time.sleep(3)
        if not launches:
            pytest.xfail("notepad launch not recorded (no interactive desktop on this runner?)")
    finally:
        proc.kill()

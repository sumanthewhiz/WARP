"""
Cross-platform test runner for WARP.

Usage:
    python tests/run_tests.py                # all runnable tests
    python tests/run_tests.py --layer l1     # L1 only
    python tests/run_tests.py --layer l2     # L2 only (contract + adversarial)
    python tests/run_tests.py --layer l3     # L3 only (eval)
    python tests/run_tests.py --layer l1_property
    python tests/run_tests.py --tier python  # only pytest-driven tests
    python tests/run_tests.py --tier scripts # only the standalone scripts
    python tests/run_tests.py --list         # list all discovered tests
    python tests/run_tests.py -v             # verbose (passed through to pytest)

Exits non-zero if any test fails or is missing.
"""

from __future__ import annotations

import argparse
import pathlib
import subprocess
import sys


TESTS_DIR = pathlib.Path(__file__).parent.resolve()
REPO_ROOT = TESTS_DIR.parent.resolve()


# Standalone (non-pytest) scripts that should still run as part of
# the full suite for backward compatibility.  Each entry is
# (display_name, command_argv).
STANDALONE_SCRIPTS = [
    # The pytest tests in tests/python/ supersede these; we keep them
    # invocable for legacy users + as a smoke check that the original
    # scripts still work standalone.
    ("scripts/test_hallucination_guard.py",
     [sys.executable, str(REPO_ROOT / "scripts" / "test_hallucination_guard.py")]),
    ("scripts/test_near_copy.py",
     [sys.executable, str(REPO_ROOT / "scripts" / "test_near_copy.py")]),
]


def _run_pytest(layer: str | None, extra: list[str]) -> int:
    cmd = [sys.executable, "-m", "pytest", "-c", str(TESTS_DIR / "pytest.ini")]
    cmd += ["--rootdir", str(TESTS_DIR)]
    if layer:
        cmd += ["-m", layer]
    cmd += extra
    print(f"[run_tests] {' '.join(cmd)}", flush=True)
    return subprocess.call(cmd, cwd=str(TESTS_DIR))


def _run_standalone() -> int:
    overall = 0
    for name, argv in STANDALONE_SCRIPTS:
        print(f"[run_tests] running standalone: {name}", flush=True)
        rc = subprocess.call(argv, cwd=str(REPO_ROOT))
        print(f"[run_tests] {name} -> exit {rc}", flush=True)
        if rc != 0:
            overall = rc
    return overall


def _list_tests() -> int:
    cmd = [sys.executable, "-m", "pytest",
           "-c", str(TESTS_DIR / "pytest.ini"),
           "--rootdir", str(TESTS_DIR),
           "--collect-only", "-q"]
    print(f"[run_tests] {' '.join(cmd)}", flush=True)
    return subprocess.call(cmd, cwd=str(TESTS_DIR))


def main(argv: list[str]) -> int:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--layer", choices=["l1", "l1_property", "l2", "l3"],
                   default=None)
    p.add_argument("--tier", choices=["python", "scripts", "all"],
                   default="all")
    p.add_argument("--list", action="store_true",
                   help="list all discovered pytest tests, don't run")
    args, extra = p.parse_known_args(argv)

    if args.list:
        return _list_tests()

    overall = 0
    if args.tier in ("python", "all"):
        rc = _run_pytest(args.layer, extra)
        if rc != 0:
            overall = rc

    # Standalone scripts only run when no layer filter is set
    # (they're not tagged by layer).
    if args.tier in ("scripts", "all") and args.layer is None:
        rc = _run_standalone()
        if rc != 0:
            overall = rc

    return overall


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

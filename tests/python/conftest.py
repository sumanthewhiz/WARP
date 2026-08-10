"""Shared pytest fixtures and configuration."""

import os
import sys
import pathlib

import pytest

# Make `tests/python` importable for sibling test files.
TESTS_PYTHON_DIR = pathlib.Path(__file__).parent.resolve()
if str(TESTS_PYTHON_DIR) not in sys.path:
    sys.path.insert(0, str(TESTS_PYTHON_DIR))

REPO_ROOT = TESTS_PYTHON_DIR.parent.parent.resolve()


@pytest.fixture(scope="session")
def repo_root() -> pathlib.Path:
    """Absolute path to the repository root."""
    return REPO_ROOT


@pytest.fixture(scope="session")
def cpp_source(repo_root) -> dict:
    """Read the key C++ source files once per session.  Used by the
    sync tests that pin Python ports against the C++ source of truth.
    """
    files = {
        "LlmSummarizer.cpp":  repo_root / "LlmSummarizer.cpp",
        "InferenceEngine.cpp": repo_root / "InferenceEngine.cpp",
        "ContextInference.cpp": repo_root / "ContextInference.cpp",
    }
    out = {}
    for name, path in files.items():
        if not path.exists():
            pytest.skip(f"{name} not found at {path}")
        out[name] = path.read_text(encoding="utf-8", errors="replace")
    return out


@pytest.fixture(scope="session")
def models_dir(repo_root) -> pathlib.Path | None:
    """Return the models directory if it exists; else None.  Tests
    that need the actual ONNX files skip when this is None.
    """
    candidates = [
        repo_root / "models" / "granite",
        repo_root / "x64" / "Debug" / "models" / "granite",
        repo_root / "x64" / "Release" / "models" / "granite",
        repo_root / "WARP!" / "x64" / "Debug" / "models" / "granite",
    ]
    for c in candidates:
        if (c / "model_quantized.onnx").exists():
            return c
    return None


def pytest_collection_modifyitems(config, items):
    """Auto-mark tests by their directory so users can filter by layer.
    `pytest -m l1` runs only L1 tests, etc.
    """
    for item in items:
        path = str(item.fspath)
        if "/property/" in path or "\\property\\" in path:
            item.add_marker(pytest.mark.l1_property)
        elif "/contract/" in path or "\\contract\\" in path:
            item.add_marker(pytest.mark.l2)
        elif "/adversarial/" in path or "\\adversarial\\" in path:
            item.add_marker(pytest.mark.l2)
        elif "/eval/" in path or "\\eval\\" in path:
            item.add_marker(pytest.mark.l3)
        else:
            item.add_marker(pytest.mark.l1)


def pytest_configure(config):
    config.addinivalue_line("markers", "l1: layer 1 deterministic tests")
    config.addinivalue_line("markers", "l1_property: layer 1 property-based tests")
    config.addinivalue_line("markers", "l2: layer 2 model-contract tests")
    config.addinivalue_line("markers", "l3: layer 3 eval / quality tests")
    config.addinivalue_line("markers", "needs_models: requires the ONNX models on disk")

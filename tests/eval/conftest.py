"""Shared pytest config for the eval layer.  Adds the eval directory
to sys.path so tests in this directory can import sibling modules
like `metrics` without a package-relative prefix."""

import pathlib
import sys

EVAL_DIR = pathlib.Path(__file__).parent.resolve()
if str(EVAL_DIR) not in sys.path:
    sys.path.insert(0, str(EVAL_DIR))

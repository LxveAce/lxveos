"""Host-C test-wiring guard: every tests/host_c/test_*.c must be compiled by tests/host_c/run.sh, and every
source path run.sh references must exist on disk.

The host-C suite is a hand-maintained shell script (run.sh) with one compile+run block per pure core. A new
pure core is easy to add a test_*.c for and then forget to wire into run.sh -- in which case the test silently
never runs and CI's `validate` job stays green on a test nobody executes. This guard fails when a test file
isn't referenced (so a forgotten core is caught), and when run.sh points at a source/test path that no longer
exists (a typo/rename that would break the build). Pure Python: `python -m pytest tests/`, no toolchain.
"""
from __future__ import annotations

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
HOSTC = ROOT / "tests/host_c"
RUN_SH = (HOSTC / "run.sh").read_text(encoding="utf-8")


def test_every_hostc_test_is_wired_into_run_sh() -> None:
    """Each tests/host_c/test_*.c is referenced (compiled + run) by run.sh -- none silently skipped."""
    test_files = sorted(p.name for p in HOSTC.glob("test_*.c"))
    assert test_files, "no host_c test files found -- the glob or path is wrong"
    missing = [name for name in test_files if name not in RUN_SH]
    assert not missing, (
        "host_c test file(s) not compiled by run.sh (they would silently never run): " + ", ".join(missing)
    )


def test_run_sh_source_paths_exist() -> None:
    """Every `$here/...` and `$root/...` .c source path run.sh compiles actually exists on disk (no typo/rename)."""
    # run.sh feeds $CC quoted args; $here = tests/host_c, $root = repo root. Match only the .c source args
    # (the -I"$root/.../include" dirs end in a directory, not `.c`, so they're skipped).
    refs = re.findall(r'"\$(here|root)/([^"]+\.c)"', RUN_SH)
    assert refs, "no compiled .c source paths parsed from run.sh -- the pattern or run.sh format changed"
    base = {"here": HOSTC, "root": ROOT}
    missing = sorted({f"${k}/{rel}" for k, rel in refs if not (base[k] / rel).is_file()})
    assert not missing, "run.sh references source file(s) that do not exist: " + ", ".join(missing)

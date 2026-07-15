#!/usr/bin/env python3
"""Managed-dependency drift gate for LxveOS.

The ESP-IDF Component Manager resolves the caret version ranges in each component's idf_component.yml
(e.g. lvgl "^9") into concrete versions and writes them to dependencies.lock at build time. That lock is
NOT committed (it's git-ignored and regenerated every build), so without a check a new upstream minor/patch
can silently enter a security-firmware build. This gate reads the freshly generated dependencies.lock and
fails the build if any resolved version drifts from the reviewed pins in scripts/expected_deps.txt. Adopting
an upgrade is then a conscious, in-commit bump of the pin rather than an invisible resolution change.

Only registry components (namespace/name) are gated; the `idf` pseudo-dependency and the lock's own
format-version field are ignored. A lock we can't parse at all (format change) is a hard error, not a pass.

Pure helpers (parse_lock_deps / parse_expected / evaluate) are unit-tested off a fixture in
tests/test_check_deps.py. No ESP-IDF required to run.

Usage:
  check_deps.py [--lock dependencies.lock] [--expected scripts/expected_deps.txt] [--board <id>]
"""
from __future__ import annotations

import argparse
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def parse_lock_deps(text: str) -> dict[str, str]:
    """Extract {component_name: resolved_version} from a dependencies.lock (ESP-IDF 2.x YAML format).

    Scoped to the top-level `dependencies:` block: a 2-space-indented `name:` key starts a dependency and
    its 4-space-indented `version:` child gives the resolved version. Top-level keys (manifest_hash, target,
    the lock's own `version:`) and nested `source:` fields are ignored. Returns {} on an unrecognisable file
    so the caller can fail loudly instead of silently passing."""
    deps: dict[str, str] = {}
    in_deps = False
    current: str | None = None
    for raw in text.splitlines():
        if not raw.strip() or raw.lstrip().startswith("#"):
            continue
        indent = len(raw) - len(raw.lstrip(" "))
        if indent == 0:
            in_deps = raw.strip() == "dependencies:"
            current = None
            continue
        if not in_deps:
            continue
        stripped = raw.strip()
        # A dependency name: a 2-indent key with no inline value ("espressif/esp_lcd_ili9341:").
        if indent == 2 and stripped.endswith(":") and " " not in stripped[:-1]:
            current = stripped[:-1]
            continue
        # That dependency's resolved version (its direct 4-indent child).
        if current and indent == 4 and stripped.startswith("version:"):
            val = stripped[len("version:"):].strip().strip('"').strip("'")
            if val and current not in deps:
                deps[current] = val
    return deps


def parse_expected(text: str) -> dict[str, str]:
    """Parse scripts/expected_deps.txt: one `namespace/component  exact-version` per line, # comments and
    blank lines skipped."""
    expected: dict[str, str] = {}
    for line in text.splitlines():
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        parts = line.split()
        if len(parts) != 2:
            raise ValueError(f"malformed expected-deps line (want '<name> <version>'): {line!r}")
        expected[parts[0]] = parts[1]
    return expected


def evaluate(locked: dict[str, str], expected: dict[str, str]) -> tuple[list[tuple[str, str, str]], bool]:
    """Compare resolved lock versions against the expected pins. Returns (rows, ok): rows is a list of
    (name, status, detail) with status in {ok, DRIFT, MISSING, UNEXPECTED}; ok is True iff nothing drifted.

    A pinned dep that is absent from the lock is MISSING; a version mismatch is DRIFT. Any *registry* dep
    (namespace/name) present in the lock but not pinned is UNEXPECTED (a new dependency slipped in). Non-slash
    entries (the `idf` pseudo-dep) are never flagged as unexpected."""
    rows: list[tuple[str, str, str]] = []
    ok = True
    for name in sorted(expected):
        want = expected[name]
        got = locked.get(name)
        if got is None:
            rows.append((name, "MISSING", f"pinned {want}, absent from lock"))
            ok = False
        elif got != want:
            rows.append((name, "DRIFT", f"pinned {want}, resolved {got}"))
            ok = False
        else:
            rows.append((name, "ok", got))
    for name in sorted(locked):
        if "/" in name and name not in expected:
            rows.append((name, "UNEXPECTED", f"resolved {locked[name]}, not in pins"))
            ok = False
    return rows, ok


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="LxveOS managed-dependency drift gate")
    ap.add_argument("--lock", default=os.path.join(ROOT, "dependencies.lock"))
    ap.add_argument("--expected", default=os.path.join(ROOT, "scripts", "expected_deps.txt"))
    ap.add_argument("--board", default=None, help="label for the report line (matrix board id)")
    args = ap.parse_args(argv)

    if not os.path.isfile(args.lock):
        print(f"[deps:ERROR] lock file not found: {args.lock} (was the board built first?)", file=sys.stderr)
        return 2
    locked = parse_lock_deps(open(args.lock, encoding="utf-8").read())
    if not locked:
        print(f"[deps:ERROR] parsed no dependencies from {args.lock} - lock format may have changed",
              file=sys.stderr)
        return 2
    expected = parse_expected(open(args.expected, encoding="utf-8").read())

    rows, ok = evaluate(locked, expected)
    tag = args.board or "lock"
    for name, status, detail in rows:
        if status != "ok":
            print(f"[deps:{tag}] {status}: {name} - {detail}")
    if ok:
        pins = ", ".join(f"{n}={expected[n]}" for n in sorted(expected))
        print(f"[deps:{tag}] OK - {len(expected)} pins match ({pins})")
        return 0
    print(f"[deps:{tag}] FAIL - managed dependencies drifted from scripts/expected_deps.txt "
          f"(bump the pin in-commit to adopt an upgrade)")
    return 1


if __name__ == "__main__":
    sys.exit(main())

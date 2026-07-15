#!/usr/bin/env python3
"""Flash-headroom report + gate for a built LxveOS board.

After a board is built, this measures the app image against the app partition slot it has to fit (read from
the board's partition CSV, resolved via cyd_boards.json) and prints a one-line headroom report. It exits
non-zero if the app OVERFLOWS its slot — a clear, early signal of the failure that otherwise only bites at
flash/OTA time — and warns (without failing) when the app crosses a soft budget fraction, so creeping binary
bloat shows up in the CI log of every build before it hits the wall.

IRAM/DRAM overflow is already a hard link error (the build itself fails), so this deliberately focuses on the
flash/app-slot headroom the linker does not police.

Pure parsing/eval helpers (parse_size / parse_partitions / app_slot_size / evaluate) are unit-tested off any
build in tests/test_size_report.py. No ESP-IDF required to run.

Usage:
  size_report.py --board <id> --build-dir build/<id> [--manifest cyd_boards.json]
                 [--project lxveos] [--budget 0.90] [--app-bin path]
"""
from __future__ import annotations

import argparse
import glob
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# .bin files in a build dir that are NOT the app image (bootloader / partition table / OTA seed / the merged
# flashable). Anything else is a candidate app image.
NON_APP_BINS = ("bootloader.bin", "partition-table.bin", "partition_table.bin", "ota_data_initial.bin")


def parse_size(tok: str) -> int:
    """A partition size/offset token -> bytes. Accepts hex (0x...), decimal, and a K/M suffix (1024-based),
    the same forms ESP-IDF's gen_esp32part.py accepts."""
    tok = tok.strip()
    if not tok:
        raise ValueError("empty size token")
    mult = 1
    if tok[-1] in "kK":
        mult, tok = 1024, tok[:-1]
    elif tok[-1] in "mM":
        mult, tok = 1024 * 1024, tok[:-1]
    return int(tok, 0) * mult


def parse_partitions(text: str) -> list[dict]:
    """Parse a partition CSV (Name,Type,SubType,Offset,Size) into rows, skipping comments/blank lines."""
    parts = []
    for line in text.splitlines():
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        cols = [c.strip() for c in line.split(",")]
        if len(cols) < 5:
            continue
        parts.append({"name": cols[0], "type": cols[1], "subtype": cols[2], "size": parse_size(cols[4])})
    return parts


def app_slot_size(parts: list[dict]) -> int | None:
    """Bytes the app image must fit. The smallest app-type slot bounds it: with OTA the app has to fit BOTH
    ota_0 and ota_1 (equal), and factory-only has a single slot. None if the table declares no app slot."""
    apps = [p["size"] for p in parts if p["type"] == "app"]
    return min(apps) if apps else None


def evaluate(app_bytes: int, slot_bytes: int, budget: float) -> tuple[str, float, str]:
    """Classify the app against its slot. Returns (level, used_pct, message) where level is
    'fail' (overflows the slot), 'warn' (fits but over the soft budget fraction) or 'ok'."""
    used_pct = 100.0 * app_bytes / slot_bytes
    free = slot_bytes - app_bytes
    if app_bytes > slot_bytes:
        return ("fail", used_pct,
                f"app {app_bytes:,} B OVERFLOWS app slot {slot_bytes:,} B by {-free:,} B ({used_pct:.1f}%)")
    if app_bytes > budget * slot_bytes:
        return ("warn", used_pct,
                f"app {app_bytes:,} B is {used_pct:.1f}% of {slot_bytes:,} B - over the {budget*100:.0f}% "
                f"soft budget ({free:,} B free)")
    return ("ok", used_pct, f"app {app_bytes:,} B of {slot_bytes:,} B ({used_pct:.1f}%), {free:,} B free")


def find_app_bin(build_dir: str, project: str) -> str | None:
    """Locate the app image in a build dir: prefer <project>.bin, else the largest .bin that isn't a
    bootloader / partition table / OTA seed / merged image."""
    preferred = os.path.join(build_dir, f"{project}.bin")
    if os.path.isfile(preferred):
        return preferred
    cands = []
    for p in glob.glob(os.path.join(build_dir, "*.bin")):
        base = os.path.basename(p)
        if base in NON_APP_BINS or base.endswith("-merged.bin") or base.endswith("_merged.bin"):
            continue
        cands.append(p)
    return max(cands, key=os.path.getsize) if cands else None


def _partition_csv_for(board: str, manifest: str) -> str:
    boards = json.loads(open(manifest, encoding="utf-8").read())["boards"]
    if board not in boards:
        raise SystemExit(f"size_report: board '{board}' not in {manifest}")
    csv = boards[board].get("build", {}).get("partition_csv")
    if not csv:
        raise SystemExit(f"size_report: board '{board}' has no build.partition_csv")
    return csv if os.path.isabs(csv) else os.path.join(os.path.dirname(os.path.abspath(manifest)), csv)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="LxveOS flash-headroom report + overflow gate")
    ap.add_argument("--board", required=True)
    ap.add_argument("--build-dir", required=True)
    ap.add_argument("--manifest", default=os.path.join(ROOT, "cyd_boards.json"))
    ap.add_argument("--project", default="lxveos")
    ap.add_argument("--app-bin", default=None, help="override the app image path")
    ap.add_argument("--budget", type=float, default=0.90, help="soft budget fraction (warn above; default 0.90)")
    args = ap.parse_args(argv)

    csv_path = _partition_csv_for(args.board, args.manifest)
    parts = parse_partitions(open(csv_path, encoding="utf-8").read())
    slot = app_slot_size(parts)
    if slot is None:
        raise SystemExit(f"size_report: {csv_path} declares no app partition")

    app_bin = args.app_bin or find_app_bin(args.build_dir, args.project)
    if not app_bin or not os.path.isfile(app_bin):
        raise SystemExit(f"size_report: no app image found in {args.build_dir} (project '{args.project}')")

    level, _pct, msg = evaluate(os.path.getsize(app_bin), slot, args.budget)
    tag = {"ok": "OK", "warn": "WARN", "fail": "FAIL"}[level]
    print(f"[size:{tag}] {args.board}: {msg}")
    return 1 if level == "fail" else 0


if __name__ == "__main__":
    sys.exit(main())

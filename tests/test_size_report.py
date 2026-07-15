"""Host-side tests for scripts/size_report.py — the flash-headroom report + overflow gate. Exercises the
pure helpers (no ESP-IDF, no real build) plus the app-image resolution against a synthetic build dir."""
from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "scripts"))
import size_report as s  # noqa: E402


def test_parse_size():
    assert s.parse_size("0x2D0000") == 0x2D0000
    assert s.parse_size("0x6000") == 24576
    assert s.parse_size("1024") == 1024
    assert s.parse_size("16k") == 16 * 1024
    assert s.parse_size("3M") == 3 * 1024 * 1024
    assert s.parse_size(" 0x1000 ") == 0x1000


def test_parse_partitions_and_app_slot_real_csvs():
    # The committed 4 MB factory-only table: one app slot at 0x2D0000.
    text = (ROOT / "partitions" / "partitions_4mb_single.csv").read_text(encoding="utf-8")
    parts = s.parse_partitions(text)
    assert {p["name"] for p in parts} == {"nvs", "phy_init", "factory", "storage"}
    assert s.app_slot_size(parts) == 0x2D0000
    # comments and the header line are skipped (no partition named "Name").
    assert all(p["name"] != "Name" for p in parts)


def test_app_slot_is_smallest_app_partition():
    # An OTA table: the image must fit BOTH ota slots, so the slot size is the min of the app partitions.
    csv = (
        "nvs, data, nvs, 0x9000, 0x6000\n"
        "otadata, data, ota, 0xf000, 0x2000\n"
        "ota_0, app, ota_0, 0x20000, 0x180000\n"
        "ota_1, app, ota_1, 0x1A0000, 0x180000\n"
    )
    parts = s.parse_partitions(csv)
    assert s.app_slot_size(parts) == 0x180000
    # No app partition -> None (caller errors out rather than dividing by nothing).
    assert s.app_slot_size(s.parse_partitions("nvs, data, nvs, 0x9000, 0x6000\n")) is None


def test_evaluate_levels():
    slot = 0x2D0000  # 2,949,120 B
    # Comfortably under budget -> ok.
    level, pct, _ = s.evaluate(1_500_000, slot, 0.90)
    assert level == "ok" and 50 < pct < 55
    # Fits but over the 90% soft budget -> warn (does NOT fail the build).
    level, _, _ = s.evaluate(int(slot * 0.95), slot, 0.90)
    assert level == "warn"
    # Larger than the slot -> fail (the gate).
    level, _, msg = s.evaluate(slot + 1, slot, 0.90)
    assert level == "fail" and "OVERFLOW" in msg
    # Exactly full is not an overflow.
    assert s.evaluate(slot, slot, 0.90)[0] != "fail"


def test_find_app_bin(tmp_path):
    bd = tmp_path
    (bd / "bootloader.bin").write_bytes(b"\x00" * 20_000)
    (bd / "partition-table.bin").write_bytes(b"\x00" * 3_000)
    (bd / "board-merged.bin").write_bytes(b"\x00" * 900_000)
    (bd / "lxveos.bin").write_bytes(b"\x00" * 800_000)
    # Prefers the project image by name even though the merged image is larger.
    assert s.find_app_bin(str(bd), "lxveos") == str(bd / "lxveos.bin")
    # Without the named image, falls back to the largest non-excluded .bin (never bootloader/merged).
    (bd / "lxveos.bin").unlink()
    (bd / "someapp.bin").write_bytes(b"\x00" * 700_000)
    assert s.find_app_bin(str(bd), "lxveos") == str(bd / "someapp.bin")
    # Empty dir -> None.
    assert s.find_app_bin(str(tmp_path / "nope"), "lxveos") is None

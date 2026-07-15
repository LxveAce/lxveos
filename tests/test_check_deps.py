"""Host-side tests for scripts/check_deps.py — the managed-dependency drift gate. Exercises the pure
parsing/compare helpers against a realistic dependencies.lock fixture (no ESP-IDF, no real build) and
confirms the committed scripts/expected_deps.txt is self-consistent."""
from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "scripts"))
import check_deps as c  # noqa: E402

# A realistic ESP-IDF 2.x dependencies.lock: the registry deps LxveOS actually resolves (including the
# transitive `espressif/cmake_utilities` and the `~1` repackage revision on esp_lvgl_port), the `idf`
# pseudo-dependency, a nested source: block (whose fields must NOT be read as versions), and the lock's own
# top-level `version:` field (must NOT be read as a dependency version).
LOCK = """\
dependencies:
  atanisoft/esp_lcd_touch_xpt2046:
    component_hash: deadc0de
    source:
      type: service
    version: 1.0.6
  espressif/cmake_utilities:
    component_hash: 1122aabb
    source:
      type: service
    version: 0.5.3
  espressif/esp_lcd_ili9341:
    component_hash: 4f2a1c0deadbeef
    dependencies: []
    source:
      registry_url: https://components.espressif.com/
      type: service
    version: 2.0.2
  espressif/esp_lcd_touch:
    component_hash: beef4444
    source:
      type: service
    version: 1.2.1
  espressif/esp_lvgl_port:
    component_hash: aa11bb22
    source:
      type: service
    version: 2.8.0~1
  idf:
    source:
      type: idf
    version: 6.0.2
  lvgl/lvgl:
    component_hash: 99ffee
    source:
      type: service
    version: 9.5.0
direct_dependencies:
- espressif/esp_lcd_ili9341
- espressif/esp_lvgl_port
- lvgl/lvgl
manifest_hash: c0ffee1234
target: esp32
version: 2.0.0
"""


def test_parse_lock_deps():
    got = c.parse_lock_deps(LOCK)
    assert got == {
        "atanisoft/esp_lcd_touch_xpt2046": "1.0.6",
        "espressif/cmake_utilities": "0.5.3",
        "espressif/esp_lcd_ili9341": "2.0.2",
        "espressif/esp_lcd_touch": "1.2.1",
        "espressif/esp_lvgl_port": "2.8.0~1",
        "idf": "6.0.2",
        "lvgl/lvgl": "9.5.0",
    }
    # The lock's own top-level format version (2.0.0) is never mistaken for a dependency version.
    assert "2.0.0" not in got.values()


def test_parse_lock_deps_unrecognisable_is_empty():
    # A file we can't parse yields {} so main() can fail loudly rather than silently pass.
    assert c.parse_lock_deps("") == {}
    assert c.parse_lock_deps("just some junk\nno yaml here\n") == {}


def test_parse_expected():
    exp = c.parse_expected("# comment\nespressif/esp_lvgl_port 2.8.0~1\n\nlvgl/lvgl 9.5.0  # inline\n")
    assert exp == {"espressif/esp_lvgl_port": "2.8.0~1", "lvgl/lvgl": "9.5.0"}


def test_evaluate_all_match():
    locked = c.parse_lock_deps(LOCK)
    expected = {
        "atanisoft/esp_lcd_touch_xpt2046": "1.0.6",
        "espressif/cmake_utilities": "0.5.3",
        "espressif/esp_lcd_ili9341": "2.0.2",
        "espressif/esp_lcd_touch": "1.2.1",
        "espressif/esp_lvgl_port": "2.8.0~1",
        "lvgl/lvgl": "9.5.0",
    }
    rows, ok = c.evaluate(locked, expected)
    assert ok is True
    assert all(status == "ok" for _n, status, _d in rows)
    # The `idf` pseudo-dep (no slash) present in the lock but not pinned is NOT flagged unexpected.
    assert not any(name == "idf" for name, _s, _d in rows)


def test_evaluate_drift_missing_unexpected():
    locked = {
        "espressif/esp_lcd_ili9341": "2.0.3",       # drifted
        "lvgl/lvgl": "9.5.0",                        # matches
        "espressif/esp_new_thing": "1.0.0",         # a registry dep that slipped in
        "idf": "6.0.2",                             # non-slash pseudo-dep, must be ignored
    }
    expected = {
        "espressif/esp_lcd_ili9341": "2.0.2",
        "espressif/esp_lvgl_port": "2.8.0~1",       # pinned but absent -> MISSING
        "lvgl/lvgl": "9.5.0",
    }
    rows, ok = c.evaluate(locked, expected)
    assert ok is False
    status = {name: st for name, st, _d in rows}
    assert status["espressif/esp_lcd_ili9341"] == "DRIFT"
    assert status["espressif/esp_lvgl_port"] == "MISSING"
    assert status["lvgl/lvgl"] == "ok"
    assert status["espressif/esp_new_thing"] == "UNEXPECTED"
    assert "idf" not in status


def test_committed_expected_deps_is_consistent():
    # The shipped pin file parses and pins exactly the three managed deps declared in the idf_component.yml
    # files (guards against a stray edit / a new dep added to a manifest without a pin here).
    expected = c.parse_expected((ROOT / "scripts" / "expected_deps.txt").read_text(encoding="utf-8"))
    assert set(expected) == {
        "atanisoft/esp_lcd_touch_xpt2046",
        "espressif/cmake_utilities",
        "espressif/esp_lcd_ili9341",
        "espressif/esp_lcd_touch",
        "espressif/esp_lvgl_port",
        "lvgl/lvgl",
    }
    # A clean lock matching the pins passes the gate end-to-end.
    rows, ok = c.evaluate(c.parse_lock_deps(LOCK), expected)
    assert ok is True and rows

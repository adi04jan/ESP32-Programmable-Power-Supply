"""Cross-surface contract tests — static, no device, no simulator.

The v2.1 firmware serialises its status frame TWICE by hand:

    vb_net.cpp      fill_status()      ArduinoJson  -> web UI + PC net transport
    vb_console.cpp  fmt_state_json()   snprintf     -> serial/telnet transport

They are independent code paths that must describe the same device. Nothing at
runtime forces them to agree, and a field added to one and forgotten in the
other would show up as "the web UI has it but my script doesn't" weeks later.
These tests fail the moment they diverge.

The serial path is snprintf rather than ArduinoJson deliberately (spec §2.2):
it must work with an exhausted heap, which is exactly when you need it.
"""
import re
from pathlib import Path

import pytest

from voltbench.model import ChannelState, Status

FW = Path(__file__).resolve().parents[2] / "Voltbench_v2"


def _read(name: str) -> str:
    p = FW / name
    if not p.exists():
        pytest.skip(f"{name} not present")
    return p.read_text(encoding="utf-8", errors="replace")


def _section(src: str, start: str) -> str:
    """Body of the function beginning at `start`, to the next top-level '}'."""
    i = src.index(start)
    depth, j, started = 0, i, False
    while j < len(src):
        if src[j] == "{":
            depth += 1
            started = True
        elif src[j] == "}":
            depth -= 1
            if started and depth == 0:
                return src[i:j + 1]
        j += 1
    return src[i:]


def net_channel_keys() -> set[str]:
    body = _section(_read("vb_net.cpp"), "static void fill_status")
    # o["en"] = ... inside the per-channel loop
    return set(re.findall(r'\bo\["(\w+)"\]\s*=', body))


def net_top_keys() -> set[str]:
    body = _section(_read("vb_net.cpp"), "static void fill_status")
    return set(re.findall(r'\bd\["(\w+)"\]', body))


def console_keys() -> set[str]:
    body = _section(_read("vb_console.cpp"), "static int fmt_state_json")
    return set(re.findall(r'\\"(\w+)\\"\s*:', body))


class TestStatusFrameContract:
    def test_both_serializers_agree_on_channel_fields(self):
        net = net_channel_keys()
        con = console_keys()
        # Channel-level fields the console emits inside the ch[] objects.
        channel_fields = {"en", "v", "i", "w", "vset", "iset",
                          "mode", "prot", "reason", "cc_mode", "ovp", "ocp"}
        missing_net = channel_fields - net
        missing_con = channel_fields - con
        assert not missing_net, f"vb_net.cpp omits channel fields: {sorted(missing_net)}"
        assert not missing_con, f"vb_console.cpp omits channel fields: {sorted(missing_con)}"

    def test_spec_mandated_fields_are_all_emitted_by_the_web_path(self):
        """Spec §2.4 froze this shape; the web UI and PC client both read it."""
        required = {"ts", "up", "api", "ch", "temp", "temp_src", "seq", "log", "cal"}
        top = net_top_keys()
        missing = required - top
        assert not missing, f"vb_net.cpp fill_status omits: {sorted(missing)}"

    def test_spec_mandated_fields_are_all_emitted_by_the_serial_path(self):
        required = {"ts", "up", "api", "ch", "temp", "temp_src", "seq", "log", "cal"}
        missing = required - console_keys()
        assert not missing, f"vb_console.cpp fmt_state_json omits: {sorted(missing)}"

    def test_python_model_covers_every_channel_field_the_firmware_sends(self):
        """A field the firmware sends and the client silently drops is a feature
        that looks broken from the outside."""
        model_fields = set(ChannelState.__dataclass_fields__)
        unmodelled = net_channel_keys() - model_fields
        assert not unmodelled, f"model.ChannelState has no field for: {sorted(unmodelled)}"

    def test_python_model_covers_every_top_level_field(self):
        model_fields = set(Status.__dataclass_fields__)
        # rev_set/hw are extras the web path adds for the UI; not in the frozen
        # frame, so the model is allowed to ignore them.
        extras = {"rev_set", "hw"}
        unmodelled = net_top_keys() - model_fields - extras
        assert not unmodelled, f"model.Status has no field for: {sorted(unmodelled)}"


class TestApiVersionPinning:
    def test_firmware_and_client_agree_on_the_api_version(self):
        cfg = _read("vb_config.h")
        m = re.search(r"#define\s+VB_API_VERSION\s+(\d+)", cfg)
        assert m, "VB_API_VERSION not found in vb_config.h"
        from voltbench.model import API_VERSION
        assert int(m.group(1)) == API_VERSION, (
            f"firmware speaks api {m.group(1)}, python client speaks {API_VERSION}")


class TestSafetyInvariants:
    """Static checks on the properties that keep a DUT alive. These are cheap,
    and each one encodes a mistake that is easy to reintroduce during a refactor."""

    def test_enable_pins_are_driven_low_before_anything_else_in_setup(self):
        ino = _read("Voltbench_v2.ino")
        body = _section(ino, "void setup()")
        first_call = re.search(r"^\s*(\w+)\s*\(", body[body.index("{"):], re.M)
        assert first_call and first_call.group(1) == "hal_gpio_init", (
            "hal_gpio_init() must be the first statement in setup(): the CV "
            "mapping is inverted, so any path that runs before the pins are "
            f"pulled low is a 20 V path. Found {first_call.group(1) if first_call else None!r}")

    def test_dac_safe_code_is_maximum_not_zero(self):
        """DAC code 0 is 20.18 V. Getting this backwards is the single worst
        bug available in this design."""
        cfg = _read("vb_config.h")
        assert re.search(r"#define\s+CV_CODE_SAFE\s+4095", cfg), (
            "CV_CODE_SAFE must be 4095 (minimum volts) — code 0 is 20.18 V")
        assert re.search(r"#define\s+CC_CODE_SAFE\s+0", cfg)

    def test_below_spec_dac_region_is_excluded(self):
        cfg = _read("vb_config.h")
        assert re.search(r"#define\s+CV_CODE_MAX\s+4069", cfg), (
            "CV_CODE_MAX must exclude the below-spec 4070-4095 region")

    def test_no_i2c_outside_the_control_task_owner(self):
        """ctrl is the sole owner of the bus; that is what removes every mutex."""
        offenders = [p.name for p in FW.glob("*.cpp")
                     if "Wire." in p.read_text(encoding="utf-8", errors="replace")
                     and p.name not in ("vb_hal.cpp", "vb_ui.cpp")]
        assert not offenders, f"I2C access outside the ctrl-owned modules: {offenders}"

    def test_no_preferences_outside_the_nvs_module(self):
        """A Preferences write is a 10-40 ms flash erase; on the AsyncTCP task it
        stalls the whole network stack."""
        offenders = [p.name for p in FW.glob("*.cpp")
                     if "vbPrefs." in p.read_text(encoding="utf-8", errors="replace")
                     and p.name != "vb_nvs.cpp"]
        assert not offenders, f"NVS access outside vb_nvs.cpp: {offenders}"

    def test_websocket_broadcasts_are_guarded_against_a_stalled_client(self):
        """v1's load-bearing workaround: drop the frame, never queue it."""
        for name in ("vb_net.cpp", "vb_console.cpp"):
            src = _read(name)
            for call in re.finditer(r"\.textAll\(", src):
                window = src[max(0, call.start() - 400):call.start()]
                assert "availableForWriteAll()" in window, (
                    f"{name}: a textAll() is not guarded by availableForWriteAll()")

    def test_control_task_outranks_asynctcp(self):
        """An over-current trip must not queue behind a WebSocket burst."""
        cfg = _read("vb_config.h")
        prio = int(re.search(r"#define\s+PRIO_CTRL\s+(\d+)", cfg).group(1))
        assert prio > 3, f"PRIO_CTRL is {prio}; AsyncTCP runs at 3"

    def test_every_translation_unit_reaches_the_optimisation_pragma(self):
        """#pragma GCC optimize("Os") is per-TU and lives in vb_config.h.
        A .cpp that misses it compiles unoptimised; losing it across the board
        costs ~60 KB of flash."""
        assert '#pragma GCC optimize("Os")' in _read("vb_config.h")
        for p in FW.glob("*.cpp"):
            head = p.read_text(encoding="utf-8", errors="replace")[:600]
            assert re.search(r'#include\s+"vb_\w+\.h"', head), (
                f"{p.name} includes no vb_*.h header, so it never sees the -Os pragma")

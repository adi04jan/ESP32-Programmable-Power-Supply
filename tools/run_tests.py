#!/usr/bin/env python3
"""Run every automated test in the project. One command, no hardware.

    python tools/run_tests.py           # everything
    python tools/run_tests.py --quick   # skip the firmware compile check

Four layers, cheapest first:

  1. PURE unit tests    g++ -Werror over the firmware's Arduino-free headers.
                        -Werror is what enforces the PURE constraint: drag in
                        Arduino.h and this stops compiling.
  2. Contract tests     static cross-check that the firmware's TWO hand-written
                        status serializers (ArduinoJson for web, snprintf for
                        serial) still describe the same device, and that the
                        safety invariants hold.
  3. Simulation tests   the real firmware logic, linked into vb_sim, driven
                        through the real serial protocol against a simulated
                        analog plant. Time is injected, so a 4-hour bench wait
                        costs microseconds.
  4. Firmware build     arduino-cli compile — slow, so it runs last.

Layers 1-3 need no ESP32 and no v2.1 board. That is the point: when the
hardware finally arrives, a failure is evidence about the hardware instead of
one more suspect in a list of two.
"""
from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SIM_SRC = ROOT / "tools" / "sim" / "vb_sim.cpp"
SIM_BIN = ROOT / "tools" / "sim" / ("vb_sim.exe" if sys.platform == "win32" else "vb_sim")
PURE_SRC = ROOT / "tools" / "host_test" / "test_pure_v2.cpp"
PURE_BIN = ROOT / "tools" / "host_test" / ("tpv2.exe" if sys.platform == "win32" else "tpv2")
V1_SRC = ROOT / "tools" / "host_test" / "test_control_math.cpp"

GXX = ["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-O2"]

results: list[tuple[str, bool, float, str]] = []


def step(name: str, fn) -> bool:
    t0 = time.monotonic()
    try:
        ok, detail = fn()
    except Exception as e:                      # a broken step is a failed step
        ok, detail = False, f"{type(e).__name__}: {e}"
    dt = time.monotonic() - t0
    results.append((name, ok, dt, detail))
    print(f"{'PASS' if ok else 'FAIL'}  {name:<34} {dt:6.2f}s  {detail}")
    return ok


def run(cmd, cwd=None):
    return subprocess.run(cmd, cwd=cwd, capture_output=True, text=True)


def build_pure():
    r = run(GXX + ["-o", str(PURE_BIN), str(PURE_SRC)])
    return r.returncode == 0, "" if r.returncode == 0 else r.stderr.strip()[-1500:]


def run_pure():
    r = run([str(PURE_BIN)])
    last = r.stdout.strip().splitlines()[-1] if r.stdout.strip() else ""
    return r.returncode == 0, last


def run_v1_pure():
    if not V1_SRC.exists():
        return True, "skipped (v1 test absent)"
    out = V1_SRC.with_name("tcm.exe" if sys.platform == "win32" else "tcm")
    r = run(["g++", "-std=c++17", "-o", str(out), str(V1_SRC)])
    if r.returncode != 0:
        return False, r.stderr.strip()[-800:]
    r = run([str(out)])
    last = r.stdout.strip().splitlines()[-1] if r.stdout.strip() else ""
    return r.returncode == 0, last


def run_v1_proto():
    # -Werror like the v2 PURE headers: vb_proto.h must stay Arduino-free.
    src = V1_SRC.with_name("test_vb_proto.cpp")
    out = V1_SRC.with_name("tvp.exe" if sys.platform == "win32" else "tvp")
    r = run(GXX + ["-o", str(out), str(src)])
    if r.returncode != 0:
        return False, r.stderr.strip()[-800:]
    r = run([str(out)])
    last = r.stdout.strip().splitlines()[-1] if r.stdout.strip() else ""
    return r.returncode == 0, last


def build_sim():
    # No -Werror here: the simulator is a test harness, not shipped code.
    r = run(["g++", "-std=c++17", "-Wall", "-Wextra", "-O2",
             "-o", str(SIM_BIN), str(SIM_SRC)])
    return r.returncode == 0, "" if r.returncode == 0 else r.stderr.strip()[-1500:]


def run_pytest(expr: str | None, label: str):
    cmd = [sys.executable, "-m", "pytest", "-q", "tests/"]
    if expr:
        cmd += ["-k", expr]
    r = run(cmd, cwd=ROOT / "pc")
    tail = [l for l in r.stdout.strip().splitlines() if "passed" in l or "failed" in l]
    detail = tail[-1] if tail else r.stdout.strip()[-400:]
    if r.returncode != 0:
        detail += "\n" + "\n".join(r.stdout.strip().splitlines()[-25:])
    return r.returncode == 0, detail


def build_firmware():
    if not shutil.which("arduino-cli"):
        return True, "skipped (arduino-cli not on PATH)"
    r = run(["arduino-cli", "compile", "-b",
             "esp32:esp32:esp32c3:CDCOnBoot=cdc,FlashSize=4M,PartitionScheme=min_spiffs",
             "--build-path", str(ROOT / "build" / "v2_test"),
             str(ROOT / "Voltbench_v2")])
    if r.returncode != 0:
        errs = [l for l in r.stdout.splitlines() + r.stderr.splitlines() if "error:" in l]
        return False, "\n".join(errs[:15]) or r.stderr.strip()[-1200:]
    size = [l for l in r.stdout.splitlines() if "Sketch uses" in l or "Global var" in l]
    return True, " | ".join(s.strip() for s in size)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--quick", action="store_true",
                    help="skip the arduino-cli firmware compile (the slow one)")
    a = ap.parse_args()

    # ASCII only: the Windows console renders a UTF-8 em dash as a mojibake box.
    print("Voltbench automated tests - no hardware required\n" + "=" * 66)

    ok = True
    ok &= step("pure headers: compile (-Werror)", build_pure)
    ok &= step("pure headers: assertions",        run_pure)
    ok &= step("v1 control_math",                 run_v1_pure)
    ok &= step("v1 console/OTA parsing (-Werror)", run_v1_proto)
    ok &= step("simulator: build",                build_sim)
    ok &= step("contract + safety invariants",    lambda: run_pytest("contract or Safety or ApiVersion", "contract"))
    ok &= step("firmware behaviour (simulated)",  lambda: run_pytest("test_firmware", "firmware"))
    ok &= step("PC library",                      lambda: run_pytest("test_pc", "pc"))
    if not a.quick:
        ok &= step("firmware: arduino-cli compile", build_firmware)

    print("=" * 66)
    total = sum(r[2] for r in results)
    failed = [r[0] for r in results if not r[1]]
    if failed:
        print(f"FAILED after {total:.1f}s: {', '.join(failed)}")
        for name, good, _, detail in results:
            if not good:
                print(f"\n--- {name} ---\n{detail}")
        return 1
    print(f"All {len(results)} steps passed in {total:.1f}s")
    return 0


if __name__ == "__main__":
    sys.exit(main())

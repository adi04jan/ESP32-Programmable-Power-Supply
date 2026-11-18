# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

Voltbench — an ESP32-C3 bench power supply with a Wi-Fi web UI, REST/WebSocket API, MQTT bridge, and
two OTA paths. Firmware is Arduino-framework C++ in a sketch folder per hardware generation. There is
no package manager and no CI, but there **is** an automated test suite that needs no hardware — see
"Testing" below. Run it before claiming anything works.

Two hardware generations coexist in this repo:

| | v1 (shipping) | v2.x (design only) |
|---|---|---|
| Branch | `fw-3.1` (3.1.4, latest) · `release_v2` (3.0.1) · `main` | `v2-hardware` |
| Topology | LM2596 + MCP4017 digipot, 3 outputs, CV only | MP1584 + MCP4728 DAC + LM358, 2 channels, hardware CC+CV, USB-C PD in |
| Firmware | `ESP32_power_supply/` | Phase 0 done (module split); Phase 1+ not started |
| Reference | `README.md`, `AUTOMATION.md` | `Hardware/HARDWARE-DESIGN-V2.md`, `Hardware/Voltbench_v22.sch`, `docs/superpowers/specs/2026-08-11-v2.1-firmware-and-software-design.md`, next board: `docs/superpowers/plans/2026-11-18-hardware-v2.3.md` |

`fw-3.1` carries everything since 3.0.1 and is not merged yet; `release_v2` is still at 3.0.1.
`v2-hardware` holds the v2.1 design docs plus the Phase 0 refactor; its control path is still v1
behaviour, so do not assume code there matches the v2 design.

## Commands

**Build** — arduino-cli is installed and works; use it to verify changes without opening the IDE:
```sh
arduino-cli compile -b "esp32:esp32:esp32c3:CDCOnBoot=cdc,FlashSize=4M,PartitionScheme=min_spiffs" \
  --build-path /tmp/vb_build ESP32_power_supply
# 3.1.4 for comparison: flash 1,345,714 B (68%) · static RAM 45,200 B (13%)
```
A change that moves those numbers by more than a few hundred bytes changed something real. In
particular, a sudden **+60 KB** means a translation unit lost `#pragma GCC optimize("Os")`.

**Flash** — Arduino IDE 2.x (no PlatformIO config exists). Board `ESP32C3 Dev Module`, 4 MB flash,
**Partition Scheme: Custom → `ESP32_power_supply/partitions.csv`** (the default is too small — the
sketch is ~1.3 MB), and **USB CDC On Boot: Enabled** (required for the serial console;
`Serial.setTxTimeoutMs(0)` is set so CDC never blocks the loop). arduino-cli does *not* pick up the
sketch's `partitions.csv`, so the command above uses `min_spiffs` (1.875 MB app) purely as a
size-check proxy; it is close enough to the real 1.75 MB slot to catch overflow.

**Flash without USB** — web-upload OTA works from any machine on the LAN:
```sh
curl -H "X-Auth: <token>" -F "f=@ESP32_power_supply.ino.bin" http://<ip>/api/ota/upload
```
curl exiting 28 or 56 is normal — the connection drops when the device reboots. Confirm with
`/api/info` → `fw`. A reboot cuts every output for about a second (see gotchas).

**Regenerate the web UI** — `web_page.h` is generated, never hand-edited. Source of truth is
`new design/Power Supply.html`. Run `python tools/gen_web_page.py` — deterministic gzip, and it
decompresses its own output to prove the header matches the HTML (the old PowerShell snippet in
`README.md` once wrote a stale header while exiting 0). After flashing, hard-refresh the browser
(`Cache-Control: no-cache` still permits a stale copy).

**Git identity — always adi04jan.** Every commit in this repo is authored as `adi04jan` with that
account's email (set in the repo's own `.git/config`; don't override it with `-c`, env vars or
`--author`). Push only through `origin`, whose SSH host alias maps to adi04jan's key: check with
`ssh -T git@aditya_github.com` → `Hi adi04jan!`. Plain `git@github.com` authenticates as a
different account on this machine, so never repoint `origin` at it, and the HTTPS credential is
pull-only.

**Release** — bump `CURRENT_FIRMWARE_VERSION` in `vb_config.h`, build, then attach **both**
`version.txt` (containing exactly the version string) and `firmware_<version>.bin` to a GitHub
release (web UI or an adi04jan PAT). `/releases/latest/download/` resolves automatically. Both files
are gitignored build outputs in the repo root.

For driving a live device from a session (auth flow, endpoint table, console protocol, safety rules),
read `AUTOMATION.md` rather than re-deriving it.

## Testing

**One command, no hardware, ~30 s** (add the arduino-cli compile by dropping `--quick`, ~4 min):
```sh
python tools/run_tests.py --quick
```

| Layer | What | Where |
|---|---|---|
| v1 control math | fixed map, step choice, overvolt ceiling, correction guard | `tools/host_test/test_control_math.cpp` (`control_math.h`) |
| v1 parsing | console arguments + semver, g++ `-Werror` | `tools/host_test/test_vb_proto.cpp` (`vb_proto.h`) |
| v2 PURE unit tests | g++ `-Werror` over the Arduino-free v2.1 headers | `tools/host_test/test_pure_v2.cpp` |
| v2 contract + safety | static cross-check of the two status serializers and the safety invariants | `pc/tests/test_contract.py` |
| v2 firmware behaviour | real firmware logic vs a simulated analog plant | `pc/tests/test_firmware.py` |
| v2 PC library | model/transport/device | `pc/tests/test_pc.py` |

**`tools/sim/vb_sim.cpp` links the v2 firmware's own PURE headers** — `vb_cal.h`, `vb_protect.h`,
`vb_seq.h`, `vb_log.h`, `vb_cmd.h` — against a modelled MP1584 + CC loop + load, and speaks the real
serial protocol on stdio. So the protection FSM, clamp chain, and tokeniser under test *are* the
shipping code; a separately written simulator would drift. **Time is injected, never read from a
clock** (`SIM:TICK <ms>`), so a `timeout="4h"` wait runs in microseconds. Fault injection:
`SIM:LOAD/SHORT/SENSE/TEMP/HW/FORCEV/NOISE/REBANNER`, plus `SIM:CODES?` for white-box DAC assertions.

Two rules that keep this honest:
- **`-Werror` on the PURE tests enforces the PURE constraint.** Drag `Arduino.h`, `String`, or
  FreeRTOS into one of those headers and the suite stops compiling — which is the point.
- **`conftest.py` rebuilds `vb_sim` when any `Voltbench_v2/vb_*.h` is newer.** A stale binary would
  silently turn these into tests of last week's firmware.

Put any new pure logic behind a PURE header with assertions; anything needing Arduino headers can't
be host-tested.

**PC library** — `pc/voltbench/`. `connect("sim://")` returns a working `PowerSupply` with no sockets
and no hardware; `connect("COM7")` is the same object over serial (untested — no v2 board exists).

## Architecture (v1, `ESP32_power_supply/`)

**Multiple translation units — order does not matter** (since Phase 0, commit `0da524d`). Each module
is a separate `.cpp`; the old mid-file `firmware-additions.h` include is now `vb_net.cpp`.

| File | Owns |
|---|---|
| `ESP32_power_supply.ino` | `setup()` / `loop()` / task creation only |
| `vb_config.h` | pins, addresses, version, setpoint limits, and the **per-TU `-Os` pragma** |
| `vb_state.h/.cpp` | globals shared across modules |
| `vb_debug.h/.cpp` | `DbgPrint` ring → Serial + telnet + `/logws` |
| `vb_control.h/.cpp` | digipot, ADC, deadbeat controller, output switching, change log — **sole I²C + ADC owner** |
| `vb_console.h/.cpp` | bench console (USB + telnet), log-ring drain |
| `vb_nvs.h/.cpp` | Preferences: settings, Wi-Fi slots, restore state |
| `vb_ota.h/.cpp` | pull-OTA from GitHub release assets |
| `vb_net.h/.cpp` | HTTP routes, both WebSockets, WiFi manager, captive AP, MQTT |
| `control_math.h` | pure, Arduino-free, host-tested: fixed map, step choice, correction. Keep new math here |
| `vb_proto.h` | pure console-argument + semver parsing, host-tested with `-Werror` |

Three Arduino-IDE rules this structure depends on:

- **`.cpp` files in a sketch folder get no implicit `#include <Arduino.h>`** (unlike `.ino`).
  `vb_config.h` provides it, which is one reason every TU must include that header first.
- **`#pragma GCC optimize("Os")` is per-TU.** It lives in `vb_config.h`. A `.cpp` that forgets to
  include it compiles unoptimized; losing it across the board costs ~60 KB of flash.
- **Files in subdirectories are not compiled** unless the directory is named `src/`. Keep the sketch
  folder flat — flat files also get IDE tabs.

**Concurrency model — one owner per resource.** `voltageControlTask` is the *only* thing that touches
the ADC and the I²C pot. Async web handlers never do hardware I/O; they read `volatile` globals and
write `g_target_mV`. Preserve this when adding features — an I²C write from an AsyncTCP callback will
race the control task. NVS writes from either side must take `g_nvs_mux`.

**One dispatcher for five input paths.** WebSocket, `/api/control`, MQTT, telnet, and the USB console
all funnel into `apply_command(action, ch, val, src)`. Add commands there, not in individual handlers,
or the other four transports silently lack the feature. It returns false on anything invalid — every
transport must surface that (HTTP 400, WS `err`, console `ERR`), never claim success. `src` feeds the
change log (`/api/log`, console `log?`). The v1 signature hardcodes `ch == 1` for `set_voltage`.

**No transport may switch an output on as a side effect.** `set_voltage` / console `v` change the
setpoint only. Deferred switch-ons (`cycle`, restore-after-reboot) all go through one pending
mechanism serviced by the control task, which switches a channel on only once its rail checks out,
and an explicit toggle/all_off cancels it under the same lock. Don't add a second path.

**The console protocol is a contract** (`AUTOMATION.md` §3a): whole tokens, exactly one `OK`/`ERR`
line per command, USB replies through `UsbReply` (never `Dbg`, which drops when the CDC ring is
full). External automation scripts parse it. The old first-character parser ran a sweep on `meas`
and switched output 1 on for `version` — keep matching whole tokens. Append new `s` fields at the
end; never reorder.

**Controller is a deadbeat, not a loop.** Predict the wiper step from the map → 300 ms settle → one
~200 ms filtered verify sample → at most one ±1-step correction → freeze. States: idle → settling →
verifying → frozen. This deliberately replaced a PID that chased ADC noise and dithered the output.
Do not reintroduce continuous feedback, and do not dither the pot between steps.

**The step→mV map is fixed, not swept** (3.1.1). `cm_fixed_map()` in `control_math.h` computes all 128
entries from `V = A + B/(step + C)`, three constants fitted to two full sweeps of this board (rms
103 mV = sweep noise). The removed on-device sweep stored raw ADC readings, whose noise smoothed into
flat runs of up to 18 steps and broke the +300 mV overvolt ceiling. Don't bring a per-step stored map
back; refit the three constants if the pot, feedback resistor, LM2596 or `VV_ADC_SCALE` change. Old
NVS keys `calmap`/`calok` may linger on upgraded units; nothing reads them. The map is also the best
voltage readout: `vb_expected1_mv()` → `/api/status.expected1` / `s exp1_mv=` (3.1.3), checked against
a load's own input ADC to ±0.03 V (plus that load's constant input drop) across 10.8–16.6 V.

**The web UI mirrors the firmware's step choice.** `predictLanding()` in `Power Supply.html` is a
copy of `cm_best_step()` (nearest step not above target + 300 mV). Every CH1 control — slider, dial,
▲/▼, presets — snaps through it (3.1.4), so what the page shows is what the supply sets. Change one,
change both.

## Load-bearing gotchas

These cost hours to rediscover. Do not "clean up" the workarounds.

- **CH1 is quantized, unevenly.** 128 pot steps: ~13 mV apart near 3 V, 0.9–1.2 V apart at 13–17 V
  (only 13.42 / 14.33 / 15.38 / 16.61 V there). Setpoints are 2.5–17 V (`VB_VMIN_MV`/`VB_VMAX_MV`);
  17 V admits step 9 (16.6 V). **Steps 0–8 must stay unselectable** — the LM2596 is in dropout there
  and outputs the input adapter minus ~1 V, i.e. whatever adapter is plugged in. Finer steps need
  hardware (the v2 DAC); see the v2.3 plan.
- **Every ADC divider taps the regulator BEFORE its output P-FET** (schematic net N$1; CH3 switched
  off reads 3.35 V). Readings are valid with the output off (restore checks the rail before switching
  on) and say nothing about whether a load is powered — only `out=` does.
- **The CH1 sense reads 0.6–1.25 V LOW and noisy under load.** Its 47k:1k divider multiplies any
  ground offset ×47; a ~30 mV shift (suspected USB ground loop when the ESP32's USB and the load share
  a host) becomes ~1.4 V. Trust `expected1`, never `voltage1`/`rail1_mv`, for a loaded CH1. Never
  "self-heal" the map or step the controller UP from a loaded reading (`cm_correction_step` refuses
  >3 steps low).
- **A deployed unit may be powering other hardware.** Any reboot/OTA cuts every output for ~1 s;
  with restore on (`restore 1`) they come back once each rail checks out. Coordinate with whoever
  owns the load before flashing a unit in service (see project memory).
- **HWCDC wedge.** arduino-esp32 3.2.1 `HWCDC::write()` with `setTxTimeoutMs(0)`: a stale-connected CDC
  session (exactly what an esptool flash leaves behind) plus a full TX ring underflows the retry
  counter and blocks `loopTask` indefinitely. Symptom: WebSocket/telnet/console dead while HTTP and the
  control task stay alive. Mitigation is in `DbgPrint` — never write more than `availableForWrite()`
  bytes. Similarly, every WS `textAll` is guarded by `availableForWriteAll()` (drop the frame, don't
  queue it), and the UI has a 3.5 s silent-WS watchdog that falls back to polling.
- **OTA downgrade footgun — fixed in 3.1.0, still live on any unit running ≤3.0.1.** Those compare
  `latest != CURRENT_FIRMWARE_VERSION` and downgrade a build ahead of the GitHub release; disable
  `ota_auto` before web-uploading onto them. 3.1.0+ uses `vp_semver_newer` (strictly newer only).
- **ArduinoOTA does not work here.** `ArduinoOTA.begin()` deadlocks alongside AsyncTCP even with mDNS
  disabled. It was tried and removed. Use web-upload or pull-OTA.
- **Reading USB serial on the C3 has two independent traps; expect silence until both are handled.**
  1. **HWCDC discards output until the host writes first.** `HWCDC::isCDC_Connected()` only becomes
     true when `connected = true` is set inside the *RX* interrupt (`HWCDC.cpp` ~line 129), and when it
     is false `write()` throws the data away (~line 425). A terminal that only listens sees
     **nothing at all**. Send any byte (even a bare newline) to wake it.
  2. **Opening the port can knock the chip into ROM download mode.** DTR drives the boot pin on the
     USB-Serial-JTAG: DTR low through a reset selects download, and the ROM then prints
     `boot:0x5 (DOWNLOAD)` / `waiting for download` and runs nothing. Open with `DtrEnable = true`
     (verified reset-safe) rather than the .NET default of false.

  To tell "app running" from "parked in the bootloader" without guessing, ask esptool to talk to the
  chip **without** resetting it: `esptool --before no_reset --after no_reset chip_id`. If that syncs
  ("Stub flasher running"), the app is not running. If it fails to sync, the app is running and any
  silence is a serial-plumbing problem. The ROM bootloader presents the *same* composite CDC + JTAG
  USB device as a running app, so enumeration proves nothing on its own.
- **Pull-OTA uses `setInsecure()` deliberately.** The core's embedded CA bundle symbol isn't in the
  indexed format `esp_crt_bundle_set()` needs, so verification fails with "No CA Chain is set". This is
  an accepted trade-off, not an oversight — don't "fix" it without a build-time bundle or firmware signing.
- **Auth token rotates on every `/api/login`.** A second login (another script, the web UI) invalidates
  yours: on a 401, log in again rather than failing.
- **`/api/status` needs `Cache-Control: no-store`** or browsers heuristically cache it and the
  displayed voltage appears frozen.
- **Deleted on purpose — don't re-add:** PID/autotune and `/api/pid/*`, `ctrl_mode`, OCP/OTP stubs,
  fake current/CC/trip fields, `set_ilimit`/`clear_trip`, OTA URL/SSL settings (pinned), login
  username, the calibration sweep (`/api/cal/sweep`, `/api/cal/clear`, console `m!`/`c`).
- **`credential.h` is gitignored but exists locally with real Wi-Fi credentials.** It only seeds NVS
  when every SSID slot is empty. Don't print it, commit it, or paste its contents.
- **`new design/` contains stale gitignored firmware drafts that will mislead you.** Only
  `new design/Power Supply.html` is tracked — that one *is* the canonical web UI source. The
  gitignored `new design/ESP32_power_supply.ino` and `new design/firmware-additions.h` still contain
  the deleted fake telemetry (`vb_read_current()` returning hardcoded 0.08 / 0.14 / 0.05 A, plus
  `cc1..3` / `trip1..3` / `ilim[3]` derived from it), which suggests current sensing exists. It does
  not: the real code is `ESP32_power_supply/vb_net.cpp`, with **zero** of these fields.

## Conventions

- Voltage is integer millivolts (`uint32_t`) internally; floats only at API boundaries.
- Logging goes through `Dbg` (a `DbgPrint : Print`), which fans out to USB serial, telnet, and
  `/logws` via a shared 3 KB ring. Never `Serial.print` directly in the control path.
- Async endpoints that can't answer immediately return `202` and are polled (see `/api/ota/check`).
- Test images and screenshots are gitignored (`*.png`) — don't fight it by renaming.
- Design docs live in `docs/superpowers/specs/` and `docs/superpowers/plans/`; the rest of `docs/` is
  gitignored.
- No personal, credential or other-project details in tracked files; those belong in local memory.

## Hardware notes that contradict the BOM

The BOM lists a 503E/LT (50 K) digital pot, but the **installed part behaves as ~10 K** — verified by
sweeping wiper steps against measured output. Firmware's `maxRangeOhms = 10000` is correct; leave it.

`VV_ADC_SCALE` is 47.40, DMM-trimmed from a nominal 48.0 (47K:1K divider). It's a per-board value —
re-derive from a ≥4-point DMM fit if the divider is reworked, and refit `CM_FIX_*` in
`control_math.h` afterward since the map is in scaled millivolts.

The CH1 sense is noisy (~600 mV pk-pk raw) because of that ×47 gain. Controller samples are a 50-read
mean; display values are median-of-5. Treat the noise as a hardware floor, not a firmware bug.

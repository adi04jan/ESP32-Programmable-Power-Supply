# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

Voltbench — an ESP32-C3 bench power supply with a Wi-Fi web UI, REST/WebSocket API, MQTT bridge, and
two OTA paths. Firmware is Arduino-framework C++ in a single sketch folder. There is no package
manager, no CI, and no test framework beyond one g++ self-check.

Two hardware generations coexist in this repo:

| | v1 (shipping) | v2.1 (design only) |
|---|---|---|
| Branch | `main`, `release_v2` | `v2-hardware` |
| Topology | LM2596 + MCP4017 digipot, 3 outputs, CV only | MP1584 + MCP4728 + LM358, 2 channels, hardware CC+CV |
| Firmware | `ESP32_power_supply/` (v3.0.1) | not written yet |
| Reference | `README.md` | `Hardware/HARDWARE-DESIGN-V2.md` + `docs/superpowers/specs/2026-08-11-v2.1-firmware-and-software-design.md` |

`release_v2` is the live firmware line. `v2-hardware` currently holds only the redesign document —
do not assume code there matches it.

## Commands

**Build/flash** — Arduino IDE 2.x only (no arduino-cli config, no PlatformIO). Board `ESP32C3 Dev
Module`, 4 MB flash, **Partition Scheme: Custom → `ESP32_power_supply/partitions.csv`** (the default
scheme is too small — the sketch is ~1.3 MB), and **USB CDC On Boot: Enabled** (required for the
serial console; `Serial.setTxTimeoutMs(0)` is set so CDC never blocks the loop).

**Host test** — the only runnable test. Covers `control_math.h`:
```sh
cd tools/host_test && g++ -std=c++17 -o tcm test_control_math.cpp && ./tcm
# → "control_math: all checks passed"
```
Add assertions here for any pure logic; anything needing Arduino headers can't be tested this way.

**Regenerate the web UI** — `web_page.h` is generated, never hand-edited. Source of truth is
`new design/Power Supply.html`. The PowerShell snippet in `README.md` §"Regenerating the Web UI"
gzips it into a PROGMEM byte array. **This snippet has silently produced a stale `web_page.h` while
exiting 0** — after running it, confirm `index_html_gz_len` changed and that the device serves the new
markup (hard-refresh; `Cache-Control: no-cache` still permits stale-without-validator). Writing a
checked-in build script instead is a known outstanding task.

**Flash without USB** — web-upload OTA works from any machine on the LAN:
```sh
curl -H "X-Auth: <token>" -F "f=@ESP32_power_supply.ino.bin" http://<ip>/api/ota/upload
```
curl exiting 28 is normal — the connection drops when the device reboots. That is success, not failure.

**Release** — bump `CURRENT_FIRMWARE_VERSION` in the `.ino`, build, then attach **both**
`version.txt` (containing exactly the version string) and `firmware_<version>.bin` to a GitHub
release. `/releases/latest/download/` resolves automatically. Note the git identity `adi04jan` differs
from the stored Windows credential (a pull-only account), so releases must be published via the web UI
or a PAT; SSH push works.

For driving a live device from a session (auth flow, endpoint table, safety rules), read
`AUTOMATION.md` rather than re-deriving it.

## Architecture

**Two files, one translation unit, order-dependent.** `ESP32_power_supply.ino` includes
`firmware-additions.h` **at line 457 — in the middle of the file, not the top**. The header depends on
sketch globals and functions (`psState`, `setOutput`, `setVoltage`, `apply_command`, the `g_*`
volatiles) already being declared. Moving that include, or adding a header that reorders it, breaks the
build in confusing ways. Split of responsibility:

- `.ino` — hardware: pin/I²C definitions, MCP4017 wiper writes, ADC sampling, the control task,
  calibration sweep, pull-OTA, the `DbgPrint` ring logger, telnet pump, `setup()`/`loop()`.
- `firmware-additions.h` — everything networked: NVS/Preferences, auth, all `/api/*` routes
  (`vb_register_routes`), both WebSockets, Wi-Fi manager + captive portal, MQTT, upload-OTA.
- `control_math.h` — pure, Arduino-free, host-tested. Keep new math here.

**Concurrency model — one owner per resource.** `voltageControlTask` is the *only* thing that touches
the ADC and the I²C pot. Async web handlers never do hardware I/O; they read `volatile` globals and
write `g_target_mV`. Preserve this when adding features — an I²C write from an AsyncTCP callback will
race the control task. NVS writes from either side must take `g_nvs_mux`.

**One dispatcher for five input paths.** WebSocket, `/api/control`, MQTT, telnet, and the USB console
all funnel into `apply_command(action, ch, val)`. Add commands there, not in individual handlers,
or the other four transports silently lack the feature. (The v1 signature hardcodes `ch == 1` for
`set_voltage`; v2.1's two variable channels will require widening it.)

**Controller is a deadbeat, not a loop.** Predict the wiper step from the calibration map → 300 ms
settle → one ~200 ms filtered verify sample → at most one ±1-step correction → freeze. States:
idle → settling → verifying → frozen (plus busy during a sweep). This deliberately replaced a PID that
chased ADC noise and dithered the output. Do not reintroduce continuous feedback.

**Calibration is a 128-entry map**, PAVA/isotonic-smoothed (essential — raw is noisier than the step
size at low voltage) and persisted to NVS keys `calmap`/`calok`, surviving OTA. This whole model is an
artifact of the digipot; the v2.1 linear DAC replaces it with a 2-point gain/offset fit.

## Load-bearing gotchas

These cost hours to rediscover. Do not "clean up" the workarounds.

- **HWCDC wedge.** arduino-esp32 3.2.1 `HWCDC::write()` with `setTxTimeoutMs(0)`: a stale-connected CDC
  session (exactly what an esptool flash leaves behind) plus a full TX ring underflows the retry
  counter and blocks `loopTask` indefinitely. Symptom: WebSocket/telnet/console dead while HTTP and the
  control task stay alive. Mitigation is in `DbgPrint` — never write more than `availableForWrite()`
  bytes. Similarly, every WS `textAll` is guarded by `availableForWriteAll()` (drop the frame, don't
  queue it), and the UI has a 3.5 s silent-WS watchdog that falls back to polling.
- **OTA downgrade footgun (unfixed, present on every deployed device).** The "newer" check is
  `latest != CURRENT_FIRMWARE_VERSION`, so a device running *ahead* of the published GitHub release
  will downgrade itself on the next auto-check. Before web-uploading a build newer than what's
  published, disable `ota_auto`. The real fix is a semver `>` comparison.
- **ArduinoOTA does not work here.** `ArduinoOTA.begin()` deadlocks alongside AsyncTCP even with mDNS
  disabled. It was tried and removed. Use web-upload or pull-OTA.
- **Pull-OTA uses `setInsecure()` deliberately.** The core's embedded CA bundle symbol isn't in the
  indexed format `esp_crt_bundle_set()` needs, so verification fails with "No CA Chain is set". This is
  an accepted trade-off, not an oversight — don't "fix" it without a build-time bundle or firmware signing.
- **Auth token rotates on every `/api/login`.** Log in once per automation run and reuse the token;
  a second login invalidates the first.
- **`/api/status` needs `Cache-Control: no-store`** or browsers heuristically cache it and the
  displayed voltage appears frozen.
- **Deleted on purpose — don't re-add:** PID/autotune and `/api/pid/*`, `ctrl_mode`, OCP/OTP stubs,
  fake current/CC/trip fields, `set_ilimit`/`clear_trip`, OTA URL/SSL settings (pinned), login username.
- **`credential.h` is gitignored but exists locally with real Wi-Fi credentials.** It only seeds NVS
  when every SSID slot is empty. Don't print it, commit it, or paste its contents.
- **`new design/` contains stale gitignored firmware drafts that will mislead you.** Only
  `new design/Power Supply.html` is tracked — that one *is* the canonical web UI source. But
  `new design/ESP32_power_supply.ino` and `new design/firmware-additions.h` are gitignored old drafts
  that still contain the deleted fake telemetry: `vb_read_current()` there returns hardcoded constants
  (0.08 A / 0.14 A / 0.05 A regardless of load), and `cc1..3` / `trip1..3` / `ilim[3]` are derived from
  that fiction. The canonical `ESP32_power_supply/firmware-additions.h` has **zero** of these fields.
  Reading the draft leads to the false conclusion that current sensing and CC/trip plumbing already
  exist. Always confirm which of the two headers you are reading; the real one lives in
  `ESP32_power_supply/`.

## Conventions

- Voltage is integer millivolts (`uint32_t`) internally; floats only at API boundaries.
- Logging goes through `Dbg` (a `DbgPrint : Print`), which fans out to USB serial, telnet, and
  `/logws` via a shared 3 KB ring. Never `Serial.print` directly in the control path.
- Async endpoints that can't answer immediately return `202` and are polled (see `/api/ota/check`).
- Test images and screenshots are gitignored (`*.png`) — don't fight it by renaming.
- Design docs live in `docs/superpowers/specs/` and `docs/superpowers/plans/`; the rest of `docs/` is
  gitignored.

## Hardware notes that contradict the BOM

The BOM lists a 503E/LT (50 K) digital pot, but the **installed part behaves as ~10 K** — verified by
sweeping wiper steps against measured output. Firmware's `maxRangeOhms = 10000` is correct; leave it.

`VV_ADC_SCALE` is 47.40, DMM-trimmed from a nominal 48.0 (47K:1K divider). It's a per-board value —
re-derive from a ≥4-point DMM fit if the divider is reworked, and re-run the calibration sweep
afterward since the map stores scaled millivolts.

The CH1 ADC divider is high-impedance and genuinely noisy (~600 mV pk-pk before filtering). Display
values are median-of-5. Treat measurement noise as a hardware floor, not a firmware bug.

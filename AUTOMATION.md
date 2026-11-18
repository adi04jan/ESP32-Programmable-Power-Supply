# Voltbench — Automation & Test Reference (firmware v3.1.3)

A self-contained guide for driving a Voltbench ESP32-C3 programmable power supply
over its HTTP/WebSocket API — for automated testing, CI rigs, or another agent
session. Written against firmware **3.1.3** (console protocol, errors, restore and
cycle are new in 3.1.0; the fixed CH1 map replaced the calibration sweep in 3.1.1 —
older units answer differently).

---

## 1. What the device is

A 3-channel bench supply with a Wi-Fi web UI:

| Channel | Rail | Control |
|---------|------|---------|
| **1 (VAR)** | Variable **2.5–17 V** | Software-set via an MCP4017 digital pot in the LM2596 feedback loop |
| 2 | Fixed **5 V** | On/off only |
| 3 | Fixed **3.3 V** | On/off only |

Only **channel 1** takes a voltage command. Channels 2/3 are on/off.

**Reach it at** `http://<ip>/` or `http://<mdns>.local/` (default mdns `voltbench`).
Find the IP from `/api/info` (`ip` field, when authed) or your router/mDNS.

---

## 2. Auth — read this first (three gotchas)

- Most `/api/*` endpoints require a **bearer token** in an `X-Auth` header. Get one
  from `POST /api/login` with the panel password (default **`admin`**).
- **The token ROTATES on every `/api/login` call.** A new login invalidates the
  previous token. In a test script, **log in once, reuse that token** for the whole
  run. Logging in again mid-run 401s every request still using the old token.
- `require_login` may be **false** on some units — then auth isn't enforced and any
  token (or none) works. Check `/api/info`'s `require_login`. Write scripts to send
  the token when they have one and tolerate `require_login:false`.
- **`/api/info` is the one exception:** unauthenticated it returns only
  `{fw, mdns, require_login, captive}` (HTTP 200, *not* 401) — enough to bootstrap.
  With a valid token it returns the full object. Use the unauth call to discover
  `require_login` before deciding whether you need to log in.

```bash
# Login once, capture token (bash)
TOKEN=$(curl -s -X POST -d "pass=admin" http://$IP/api/login \
        | python -c "import sys,json;print(json.load(sys.stdin)['token'])")
AUTH="-H X-Auth:$TOKEN"
```

---

## 3. Endpoint reference

Auth column: **Y** = 401 without a valid token when `require_login` is on.
`/api/info` is **partial** (see §2).

| Method | Path | Auth | Purpose / body |
|--------|------|------|----------------|
| GET  | `/` | – | gzip web UI (`Content-Encoding: gzip`, `Cache-Control: no-cache`) |
| POST | `/api/login` | – | body `pass=<pw>` → `{ok, token}` (**rotates token**) |
| GET  | `/api/info` | partial | device/network/OTA/mqtt/exp info + `vmax`; minimal unauth |
| GET  | `/api/status` | Y | live telemetry (see §4) |
| POST | `/api/control` | Y | form: `action`, `output`, `value` (see §5) |
| POST | `/api/settings` | Y | **JSON body** — nested settings patch (see §6) |
| GET  | `/api/log` | Y | `{n, lines:[…]}` last 32 output/setpoint changes with source |
| GET  | `/api/cal/status` | Y | `{valid, fixed, mv:[128]}` — CH1's fixed step→mV map |
| GET  | `/api/wifi/scan` | Y | `{scanning:true}` (202) then `{networks:[…]}` — **poll it** |
| POST | `/api/wifi/connect` | Y | form `ssid`, `pass` (async; may 409 if busy) |
| POST | `/api/wifi/forget` | Y | form `ssid` |
| GET  | `/api/ota/check` | Y | `{checking:true}` (202) then `{current, latest, newer}` — **poll it** |
| POST | `/api/ota/update` | Y | pull-OTA from GitHub **if newer** (see §8 footgun) |
| POST | `/api/ota/force` | Y | pull-OTA regardless of version |
| POST | `/api/ota/upload` | Y | multipart `.bin` upload → flash → reboot (field name any) |
| GET  | `/api/mqtt/test` | Y | test broker reachability |
| POST | `/api/reboot` | Y | reboot (was GET in ≤3.0.0 — now **POST**) |
| POST | `/api/factory_reset` | Y | wipe NVS + reboot |
| WS   | `/ws` | token in msg | live status push (4 Hz) + command channel (see §7) |
| WS   | `/logws` | – | view-only device log stream (same as telnet :23) |
| TCP  | `:23` telnet | password | log stream + console (see §3a) |
| USB  | CDC serial | – | same console (see §3a) |

Async endpoints (`/api/wifi/scan`, `/api/ota/check`) return **202 `{…:true}`** while
working and the real payload once ready — poll every ~1.5–2 s, cap the loop.

---

## 3a. USB serial / telnet console (3.1.0)

Same commands on both: USB CDC at 115200 8N1 (`/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_<MAC>-if00`
on Linux; the ttyACM number moves) and telnet :23 (password = panel password).

**Protocol.** Whole lower-cased tokens only. Every command gets **exactly one** reply line
starting `OK` or `ERR <code>` (`log?` sends `> ` rows first, then its `OK`). Anything else on
the stream (`[tel]` telemetry, `[chg]`, `[VCtrl]`, Wi-Fi logs) is log output; **ignore lines
not starting `OK`/`ERR`.** USB replies are never dropped (≤100 ms wait for buffer room).

**Opening the port (C3 traps):** the chip discards USB output until the host writes first,
and the TX buffer may hold stale log lines. Send `\n`, drain/discard input for ~0.3 s, then
send `quiet 1` (stops the 1 Hz `[tel]` line) and your commands.

| Command | Reply | Notes |
|---|---|---|
| `s` | `OK STAT out=1,1,0 set_mv=13500 rail1_mv=13420 rail2_mv=5010 rail3_mv=3300 ctl=frozen cal=1 pend=0 restore=1 up_s=123 exp1_mv=13423` | fixed format — parse this one. `exp1_mv` (3.1.3) = map value for the current pot step, see §4 |
| `?` | `OK set=13500mV meas=… disp=… state=3 R=952ohm cal=1 out=110` | legacy fields kept for old scripts. `R=` is the pot wiper, `state=` the controller (3 = frozen) — **neither describes the load** |
| `id` / `*idn?` | `OK ID voltbench fw=3.1.3 mac=… ip=… mdns=… up_s=… reset=sw` | `reset` = why it last rebooted |
| `v13.5` · `v13500mV` | `OK set_mv=13500 out=…` | **setpoint only — never switches an output.** `ERR arg` for garbage, `ERR range` outside 2.5–17 V |
| `o1 1` / `o2 0` / `o3 1` | `OK out=1,0,1` | switch one output (`o0`/`o1` still work for CH1) |
| `off` | `OK out=0,0,0` | all outputs off |
| `cycle <ch> <sec>` | `OK cycle ch=1 off_s=15 …` | off now, back on after `sec` (1–3600) **only if the rail checks out**; done by the firmware, so it survives the host dying. Any explicit switch command on that channel cancels it |
| `log?` | `> 123.456 usb set ch=1 val=13500` … `OK LOG n=N` | last 32 output/setpoint changes with source (`usb telnet http ws mqtt sys`) |
| `quiet 0\|1` | `OK quiet=1` | suppress the periodic `[tel]` line (not persisted) |
| `restore 0\|1` · `restore?` | `OK restore=1 …` | restore-after-reboot (see §8.7) |
| `help` | `OK cmds: …` | |

**All `railN_mv` / `meas` values are measured BEFORE the output switch.** They show the
regulator voltage even when the output is off, and the high-impedance CH1 divider reads
**low under load** (≈12.2 V shown at a 13.5 V setpoint while the load saw ~12.9 V). Judge a
load by the load, never by these numbers. There is **no current sensing**.

---

## 4. `/api/status` shape

```json
{"output1":false,"output2":false,"output3":false,
 "voltage1":4.992,"expected1":5.027,"voltage2":5.01,"voltage3":3.30,
 "set1":5.00,"cal":true,"state":"frozen","ts":123456}
```

- `voltage1/2/3` — measured volts **before each output switch** (non-zero when off; CH1
  reads low under load). CH1 is a median-of-5.
- `expected1` (3.1.3) — CH1 regulator voltage the fixed map predicts for the current pot
  step; `0` until the pot is written after boot. **Under load, trust this over
  `voltage1`.** Farm run 72 (2026-11-18) checked it against the build-box gateway's own
  input ADC at 7 steps, 10.8–15.4 V: the gateway read `expected1 − 0.52 V` (±0.03, its
  input drop), while `voltage1` read 0.6–1.25 V low and ~10× noisier.
- `set1` — CH1 commanded set-point.
- `cal` — step→mV map loaded (always `true` since 3.1.1: the map is fixed in firmware).
- `state` — controller state: **`idle` `settling` `verifying` `frozen`**.
- `ts` — `millis()`; wraps every ~49 days (harmless).
- **No** `current*` / `cc*` / `trip*` / `pid*` — that hardware/feature does not exist.

**Waiting for a set-point to land:** poll `/api/status` until `state == "frozen"`,
then wait ~1.5 s for the median display to settle before reading `voltage1`. Since 3.1.0
the state flips to `settling` the moment a new setpoint is accepted, so you can never see
a stale `frozen` from the previous target.

---

## 5. Control (`POST /api/control`, form-encoded)

| action | params | effect |
|--------|--------|--------|
| `toggle` | `output=1..3`, `value=1|0` | enable/disable a channel |
| `set_voltage` | `output=1`, `value=<volts>` | set CH1 (CH1 only) |
| `all_off` | — | disable all channels |
| `cycle` | `output=1..3`, `value=<seconds 1-3600>` | off now, back on after N s if the rail checks out |

- `value` accepts `1/0`, `true/false`, `on/off` or a number.
- **Errors are real (3.1.0):** an unknown action, bad `output`, or `set_voltage` outside
  **2.5–17 V** returns **400** `{"ok":false,"err":…}` and changes nothing (it used to clamp
  silently and always answer `ok`).
- `set_voltage` never switches an output on.

```bash
curl -s $AUTH -X POST -d "action=toggle&output=1&value=1" http://$IP/api/control
curl -s $AUTH -X POST -d "action=set_voltage&output=1&value=9.0" http://$IP/api/control
curl -s $AUTH -X POST -d "action=all_off" http://$IP/api/control
```

### The quantization reality (important for test assertions)

CH1 is set by a **128-step** digital pot in a `1/R` feedback divider, so achievable
voltages are a **non-uniform ladder**: ~30 mV/step near 3 V but **~0.9 V/step near
13–15 V**. The controller lands on the **nearest achievable step that doesn't exceed
target + 300 mV**, then freezes. Consequences for tests:

- **Do not assert exact equality.** A request for 13.2 V may legitimately settle at
  ~12.8 V (the nearest rung) — that is correct behavior, not a bug.
- Assertion tolerance should be **half the local step**: ~±50 mV below 6 V, growing to
  **~±0.5 V near the top**. Or read the map (`/api/cal/status.mv`) and assert the
  device landed on the nearest non-overvolt rung.
- Absolute accuracy also depends on a **per-board ADC scale** (`VV_ADC_SCALE`, compile
  time). Unless a board was DMM-trimmed, expect a few-% offset vs a real meter. Test
  against the device's own reported `voltage1`, not against a DMM value, unless you
  have trimmed that specific unit.

---

## 6. `/api/settings` (JSON body, nested patch — only send what you change)

```json
{
  "mdns": "voltbench",
  "require_login": true,
  "password": "newpass",
  "ota":  { "auto": true },
  "exp":  { "fast": true, "smooth": false, "dial": false },
  "mqtt": { "enabled": false, "host": "", "port": 1883, "user": "", "pass": "", "topic": "voltbench" }
}
```

- `exp.fast` — 4 Hz vs 1 Hz status push. `exp.smooth` — **extra client-side EMA on the
  readout; leave OFF** (the firmware already medians CH1; enabling it double-filters and
  adds seconds of display lag). `exp.dial` — dial input widget.
- The OTA **base URL is pinned** to the project's GitHub releases and cannot be changed
  via settings in 3.0.1 (only `ota.auto` is exposed). `ota.ssl` is fixed insecure.

---

## 7. WebSocket `/ws` (preferred for live monitoring)

Server pushes the §4 status object at 4 Hz (or 1 Hz if `exp.fast=false`). It **only
pushes to clients whose send queue is drained** — a slow/hung reader is skipped, never
backs up the device. Send commands as JSON text frames:

```json
{"action":"toggle","output":1,"value":true,"token":"<TOKEN>"}
{"action":"set_voltage","output":1,"value":5.0,"token":"<TOKEN>"}
{"action":"all_off","token":"<TOKEN>"}
```

Include `token` in every command frame when `require_login` is on. `value` accepts a
bool, number or numeric string. A rejected command replies with the status object plus
`"err":"rejected"`. `cycle` works here too. **Client watchdog rule:** if the socket is open but no
frame arrives for >3.5 s, close it and fall back to polling `/api/status` — the built-in
UI does exactly this so a half-dead socket never freezes the display.

---

## 8. Known gotchas & footguns (will bite automation)

1. **OTA downgrade footgun — fixed in 3.1.0.** Units on ≤3.0.1 compare `latest != current`
   and will **downgrade** a build that is ahead of GitHub's release; disable `ota.auto`
   before web-uploading onto them. 3.1.0+ only installs a strictly semver-newer release.
2. **Token rotation** — §2. One login per run.
3. **Cached UI after OTA** — `/` is `no-cache` but that permits stale-without-validator;
   a browser can show the *old* UI after an update. Hard-reload, or verify the served
   HTML directly (`curl / | gunzip | grep <marker>`) rather than trusting the tab.
4. **`/api/ota/upload` reboots on success**, dropping the connection mid-request — your
   HTTP client will see a connection error / "context destroyed". That is **success**,
   not failure; confirm by polling `/api/info.fw` after ~10 s.
5. **No calibration sweep since 3.1.1.** CH1 uses a fixed step→mV map fitted to this
   board (`control_math.h`), so nothing ever ramps or interrupts output 1. `/api/cal/sweep`,
   `/api/cal/clear` and the console `m!`/`c` commands are gone (404 / `ERR unknown`).
6. **Reboot after any control** — none needed; changes are live. But `/api/reboot`,
   `/api/factory_reset`, OTA, and mdns-change **do** reboot; expect ~10–15 s downtime, and
   **every output drops**.
7. **Restore after reboot (opt-in, `restore 1` / settings `restore:true`).** The setpoint
   and output mask are saved 2 s after they stop changing. At boot, the saved setpoint is
   applied first; each saved output is switched on only once its rail checks out (CH1:
   controller frozen and within 500 mV; CH2 4.5–5.5 V; CH3 3.0–3.6 V), else it stays off
   and `auto_on_fail` is logged. Off by default: everything stays off after a reboot.
8. **Wi-Fi in service (3.1.0):** if the link drops, the device retries the saved networks
   every 15 s and **never** opens its (open) setup AP; that only happens at power-up with no
   reachable network. `/api/info.wifi_drop` and `reset` explain the last outage/reboot.

---

## 9. Ready-to-use recipes

### A. Sweep set-points and check landing (bash)
```bash
IP=<device-ip>
TOKEN=$(curl -s -X POST -d "pass=admin" http://$IP/api/login | python -c "import sys,json;print(json.load(sys.stdin)['token'])")
A="-H X-Auth:$TOKEN"
curl -s $A -X POST -d "action=toggle&output=1&value=1" http://$IP/api/control >/dev/null
for V in 3.3 5 9 12; do
  curl -s $A -X POST -d "action=set_voltage&output=1&value=$V" http://$IP/api/control >/dev/null
  # wait for frozen
  for i in $(seq 1 30); do
    st=$(curl -s $A http://$IP/api/status | python -c "import sys,json;print(json.load(sys.stdin)['state'])")
    [ "$st" = frozen ] && break; sleep 0.3
  done
  sleep 1.5
  curl -s $A http://$IP/api/status | python -c "import sys,json;d=json.load(sys.stdin);print(f'set $V -> {d[\"voltage1\"]:.3f} V ({(d[\"voltage1\"]-$V)*1000:+.0f} mV)')"
done
curl -s $A -X POST -d "action=all_off" http://$IP/api/control >/dev/null
```

### B. Poll an async endpoint (wifi scan)
```bash
for i in $(seq 1 15); do
  r=$(curl -s $A http://$IP/api/wifi/scan)
  echo "$r" | grep -q '"networks"' && { echo "$r"; break; }
  sleep 2
done
```

### C. Browser / page-context (Playwright `evaluate`) — same-origin, uses the tab's fetch
```js
async () => {
  const lr = await fetch('/api/login', {method:'POST',
      headers:{'Content-Type':'application/x-www-form-urlencoded'}, body:'pass=admin'}).then(r=>r.json());
  const H = {'X-Auth': lr.token};
  await fetch('/api/control', {method:'POST',
      headers:{...H,'Content-Type':'application/x-www-form-urlencoded'},
      body:'action=set_voltage&output=1&value=5'});
  let s; for (let i=0;i<30;i++){ s=await fetch('/api/status',{headers:H}).then(r=>r.json());
      if (s.state==='frozen') break; await new Promise(r=>setTimeout(r,300)); }
  return s;   // {voltage1, set1, state, ...}
}
```
*(Note: driving the device over curl from a shell may be blocked by some agent
sandboxes as an external state-changing POST; the browser page-context path above,
or an explicitly allowed host, avoids that.)*

---

## 10. Copy-paste agent prompt

> You are testing a Voltbench ESP32-C3 programmable power supply at `http://<IP>/`
> (firmware 3.1.0). Read its automation contract before issuing any call: the API is
> summarized here — [paste this file or its URL].
>
> Rules: (1) `GET /api/info` unauthenticated first to read `require_login`; if true,
> `POST /api/login {pass:"admin"}` **once** and reuse the returned `token` in an
> `X-Auth` header for every call — logging in again rotates and invalidates the old
> token. (2) Only channel 1 is variable; set it with `POST /api/control
> action=set_voltage output=1 value=<volts>` (2.5–17 V; outside → HTTP 400). (3) After a set,
> poll `/api/status` until `state=="frozen"`, wait ~1.5 s, then read `voltage1`.
> (4) Do **not** assert exact voltage — output is quantized to a non-uniform pot ladder
> (~30 mV/step low, ~0.9 V/step near 15 V); assert within half a local step, or against
> the nearest rung in `/api/cal/status.mv`. (5) Never trigger `/api/ota/*` or
> `/api/factory_reset` unless the test is specifically about OTA — any reboot drops every
> output. (6) Async endpoints (`/api/wifi/scan`,
> `/api/ota/check`) return `202 {…:true}` first — poll until the real payload.
> Turn output off (`all_off`) when done.

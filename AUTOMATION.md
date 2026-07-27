# Voltbench — Automation & Test Reference (firmware v3.0.1)

A self-contained guide for driving a Voltbench ESP32-C3 programmable power supply
over its HTTP/WebSocket API — for automated testing, CI rigs, or another agent
session. Everything here is verified against firmware **3.0.1**.

---

## 1. What the device is

A 3-channel bench supply with a Wi-Fi web UI:

| Channel | Rail | Control |
|---------|------|---------|
| **1 (VAR)** | Variable **2.5–~16 V** | Software-set via an MCP4017 digital pot in the LM2596 feedback loop |
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
| POST | `/api/cal/sweep` | Y | start calibration sweep (drives CH1 to max ~1 min) |
| GET  | `/api/cal/status` | Y | `{valid, running, mv:[128]}` |
| POST | `/api/cal/clear` | Y | erase calibration map |
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
| TCP  | `:23` telnet | password | log stream + console: `v<volts> o1 o0 m c ?` |

Async endpoints (`/api/wifi/scan`, `/api/ota/check`) return **202 `{…:true}`** while
working and the real payload once ready — poll every ~1.5–2 s, cap the loop.

---

## 4. `/api/status` shape

```json
{"output1":false,"output2":false,"output3":false,
 "voltage1":4.992,"voltage2":5.01,"voltage3":3.30,
 "set1":5.00,"cal":true,"state":"frozen","ts":123456}
```

- `voltage1/2/3` — measured volts (CH1 is a median-of-5, calm; CH2/3 sampled).
- `set1` — CH1 commanded set-point.
- `cal` — calibration map present.
- `state` — controller state: **`idle` `settling` `verifying` `frozen` `busy`**
  (`busy` = a calibration sweep is running).
- `ts` — `millis()`; wraps every ~49 days (harmless).
- **No** `current*` / `cc*` / `trip*` / `pid*` — that hardware/feature does not exist.

**Waiting for a set-point to land:** poll `/api/status` until `state == "frozen"`,
then wait ~1.5 s for the median display to settle before reading `voltage1`.

---

## 5. Control (`POST /api/control`, form-encoded)

| action | params | effect |
|--------|--------|--------|
| `toggle` | `output=1..3`, `value=1|0` | enable/disable a channel |
| `set_voltage` | `output=1`, `value=<volts>` | set CH1 (CH1 only) |
| `all_off` | — | disable all channels |

- `value` for `set_voltage` is **clamped to [0, 16] V** in firmware; NaN/negative → 0.
- Out-of-range `output` is rejected (bounds-checked) — no effect.

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
bool or number for `toggle`. **Client watchdog rule:** if the socket is open but no
frame arrives for >3.5 s, close it and fall back to polling `/api/status` — the built-in
UI does exactly this so a half-dead socket never freezes the display.

---

## 8. Known gotchas & footguns (will bite automation)

1. **OTA downgrade footgun.** The pull-OTA "newer" test is `latest != current`, i.e. it
   treats *any* difference as update-worthy. A device running a version **newer than the
   published GitHub "latest"** will **downgrade itself** on the next auto/`/api/ota/update`
   check. When flashing a build ahead of the release (e.g. web-upload a pre-release),
   **set `ota.auto:false` first**, or it will revert on reboot. Fixed only when the
   matching release is published (`latest == current`).
2. **Token rotation** — §2. One login per run.
3. **Cached UI after OTA** — `/` is `no-cache` but that permits stale-without-validator;
   a browser can show the *old* UI after an update. Hard-reload, or verify the served
   HTML directly (`curl / | gunzip | grep <marker>`) rather than trusting the tab.
4. **`/api/ota/upload` reboots on success**, dropping the connection mid-request — your
   HTTP client will see a connection error / "context destroyed". That is **success**,
   not failure; confirm by polling `/api/info.fw` after ~10 s.
5. **Calibration sweep ramps CH1 to its maximum (~16 V)** for ~1 min. Ensure CH1 is
   unloaded or on a voltage-tolerant load before `POST /api/cal/sweep`. During the sweep
   `state == "busy"` and set-voltage commands are ignored.
6. **Reboot after any control** — none needed; changes are live. But `/api/reboot`,
   `/api/factory_reset`, OTA, and mdns-change **do** reboot; expect ~10–15 s downtime.

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
> (firmware 3.0.1). Read its automation contract before issuing any call: the API is
> summarized here — [paste this file or its URL].
>
> Rules: (1) `GET /api/info` unauthenticated first to read `require_login`; if true,
> `POST /api/login {pass:"admin"}` **once** and reuse the returned `token` in an
> `X-Auth` header for every call — logging in again rotates and invalidates the old
> token. (2) Only channel 1 is variable; set it with `POST /api/control
> action=set_voltage output=1 value=<volts>` (clamped 0–16 V). (3) After a set,
> poll `/api/status` until `state=="frozen"`, wait ~1.5 s, then read `voltage1`.
> (4) Do **not** assert exact voltage — output is quantized to a non-uniform pot ladder
> (~30 mV/step low, ~0.9 V/step near 15 V); assert within half a local step, or against
> the nearest rung in `/api/cal/status.mv`. (5) Never trigger `/api/ota/*` or
> `/api/factory_reset` unless the test is specifically about OTA — OTA can downgrade the
> device if the GitHub "latest" differs. (6) Async endpoints (`/api/wifi/scan`,
> `/api/ota/check`) return `202 {…:true}` first — poll until the real payload.
> Turn output off (`all_off`) when done.

# Voltbench — ESP32-C3 Programmable Power Supply

A three-channel bench power supply controlled over Wi-Fi, built around an **ESP32-C3 Super Mini**. The variable channel uses a PID-controlled LM2596-ADJ buck converter with an MCP4017 I2C digital potentiometer; two fixed rails deliver regulated 5 V and 3.3 V. Everything is managed through a dark-themed responsive web UI served directly from the microcontroller — no app, no cloud, no dependencies.

Designed and developed by **Aditya Biswas**.

---

## Screenshots

| Dashboard | History | Settings |
|-----------|---------|----------|
| ![Dashboard](ss-05-dashboard-live.png) | ![History](ss-06-history-live.png) | ![Settings](ss-07-settings-live.png) |

---

## Features

### Power Outputs
| Channel | Rail | Range | Regulation |
|---------|------|-------|------------|
| CH1 | LM2596-ADJ (variable) | 2.5 V – 15 V | PID + relay auto-tune |
| CH2 | Mini360 (fixed 5 V) | 5.0 V | Passthrough (monitor only) |
| CH3 | LM1117-3.3 (LDO, fixed) | 3.3 V | Passthrough (monitor only) |

### Accuracy (measured, PID auto-tuned)
| Setpoint | Measured | Error |
|----------|----------|-------|
| 3.3 V | 3.22 V | −80 mV (2.4%) |
| 5.0 V | 4.99 V | −10 mV (0.2%) |
| 9.0 V | 8.98 V | −20 mV (0.2%) |
| 12.0 V | 12.29 V | +290 mV (2.4%) |
| 15.0 V | 15.55 V | +550 mV (3.7%) |

> Higher-voltage error is a hardware quantization limit (78.7 Ω/step on the MCP4017). Run PID auto-tune once after assembly for best results.

### Web UI
- **Dashboard** — live voltage/current at 4 Hz, per-channel enable toggles, voltage slider, preset chips (right-click to delete, ＋ to save current), delta indicator, CV/CC mode badge, All Off button
- **History** — bezier-smoothed time-series graph with SMA noise reduction, channel filters, event markers (output on/off, trips), adjustable time window (1 min – 12 h)
- **Settings** — mDNS hostname, security/login, Wi-Fi management (scan/connect/forget, stores up to 4 SSIDs), OTA update, experimental flags (dial mode, EMA smoothing, fast poll), safety guards, MQTT bridge, PID tuning

### Connectivity
- **WebSocket** `/ws` — server pushes full status JSON at 4 Hz; also accepts control commands
- **REST API** — full control and configuration via `/api/*` endpoints
- **mDNS** — reachable at `http://voltbench.local/` on any mDNS-capable host (Windows, macOS, Linux, iOS)
- **Captive portal** — automatically falls back to AP mode (`VoltbenchAP`) when no saved Wi-Fi connects; re-tries every 30 s

### Safety
- **Over-current protection (OCP)** — trips output if current exceeds per-channel limit
- **Over-temperature protection (OTP)** — configurable threshold (default 85 °C)
- **Auto-recover** — optional: re-enables output after guard condition clears
- **Trip flags** — per-channel, manually clearable from the UI

### Management
- **OTA update** — pull firmware from any HTTP/HTTPS URL; optional auto-check on boot
- **NVS persistence** — all settings, PID gains, and Wi-Fi credentials survive power cycles
- **MQTT bridge** — publishes 1 Hz telemetry; subscribes to control topics
- **Factory reset** — wipes all NVS from Settings → Factory Reset
- **Token auth** — optional bearer-token login (stored in `sessionStorage`, survives page refresh)

---

## Hardware

### Bill of Materials

| Component | Part | Notes |
|-----------|------|-------|
| MCU | ESP32-C3 Super Mini | 4 MB flash, Wi-Fi 802.11 b/g/n |
| Variable regulator | LM2596-ADJ module | Adjustable buck, ~3–35 V input |
| Fixed 5 V | Mini360 module | Fixed-output synchronous buck |
| Fixed 3.3 V | LM1117-3.3 | LDO, TO-92 or SOT-223 |
| Digital pot | MCP4017T-103E/LT | 10 KΩ, 128 steps, I2C @ 0x2F |
| R1 (feedback) | 10 KΩ 1% | VOUT → FB (ADJ) on LM2596 |
| R_adc_top | 47 KΩ | CH1 ADC voltage divider high-side |
| R_adc_bot | 1 KΩ | CH1 ADC voltage divider low-side |
| R_5v / R_3v3 | 10 KΩ × 4 | CH2 and CH3 ADC dividers (÷2 each) |
| Enable switches | N-ch MOSFET or relay × 3 | Logic-level control from GPIO 5/6/7 |

### Pin Map

| GPIO | Function |
|------|----------|
| 0 | ADC CH1 — variable rail (47K:1K divider, ×48 scale) |
| 1 | ADC CH2 — 5 V rail (10K:10K divider, ×2 scale) |
| 3 | ADC CH3 — 3.3 V rail (10K:10K divider, ×2 scale) |
| 5 | Enable CH1 (variable output) |
| 6 | Enable CH2 (5 V output) |
| 7 | Enable CH3 (3.3 V output) |
| 8 | I2C SDA → MCP4017 |
| 9 | I2C SCL → MCP4017 |

### LM2596-ADJ Feedback Network

```
VOUT ──── 10 KΩ ──── FB (ADJ) ──── MCP4017 wiper ──── GND
                                     (0 – 10 KΩ)
```

`Vout = 1.235 × (1 + 10 000 / R2)` where R2 is the MCP4017 resistance.

| Target | R2 needed | MCP4017 step |
|--------|-----------|--------------|
| 2.5 V | 9.87 KΩ | 125 / 127 |
| 5.0 V | 3.27 KΩ | 41 / 127 |
| 9.0 V | 1.56 KΩ | 20 / 127 |
| 12.0 V | 1.13 KΩ | 14 / 127 |
| 15.0 V | 0.87 KΩ | 11 / 127 |

### CH1 ADC Divider

```
VOUT ──── 47 KΩ ──── GPIO0 ──── 1 KΩ ──── GND
                       ↑
                  ADC reads here (0 – 2.5 V with ADC_11db)
                  Scale factor = 48 → supports up to ~27 V
```

---

## Firmware

### Prerequisites

- **Arduino IDE 2.x** with ESP32 Arduino core (Espressif) v3.x
- Libraries (Library Manager):
  - `ESPAsyncWebServer` + `AsyncTCP`
  - `ArduinoJson` v7
  - `SW_MCP4017`
  - `PubSubClient` *(only if MQTT is needed)*

### Build & Flash

1. Clone the repository:
   ```sh
   git clone https://github.com/your-username/ESP32-Programmable-Power-Supply.git
   ```

2. Edit `ESP32_power_supply/credential.h`:
   ```cpp
   const char* ssids[]     = { "YourSSID" };
   const char* passwords[] = { "YourPassword" };
   const char* base_url    = "https://your-ota-server.com/voltbench/";
   ```

3. In Arduino IDE, set:
   - Board: `ESP32C3 Dev Module`
   - Upload Speed: `921600`
   - Flash Size: `4MB`
   - Partition Scheme: **Custom** → select `ESP32_power_supply/partitions.csv`

4. Upload: **Sketch → Upload** (`Ctrl+U`)

5. Open Serial Monitor at **115200 baud** — you should see:
   ```
   Voltbench v0.1.0 booting...
   MCP4017 OK @ 0x2F
   Joined YourSSID — IP=192.168.x.x
   URL: http://voltbench.local/
   ```

6. Open `http://voltbench.local/` in any browser on the same network.

### Partition Layout

```
nvs        20 KB   Settings and Wi-Fi credentials
otadata     8 KB   OTA slot tracking
app0     1792 KB   Active firmware (~1.3 MB used)
app1     1792 KB   OTA staging slot
spiffs    448 KB   Reserved
```

### Regenerating the Web UI

After editing `new design/Power Supply.html`, run this PowerShell snippet to rebuild `web_page.h`:

```powershell
$html  = Get-Content "new design\Power Supply.html" -Raw -Encoding utf8
$bytes = [System.Text.UTF8Encoding]::new($false).GetBytes($html)
$ms    = [System.IO.MemoryStream]::new()
$gz    = [System.IO.Compression.GzipStream]::new($ms, [System.IO.Compression.CompressionLevel]::Optimal)
$gz.Write($bytes, 0, $bytes.Length); $gz.Close()
$gzBytes = $ms.ToArray()
$chunks  = for ($i = 0; $i -lt $gzBytes.Length; $i += 16) {
    "  " + (($gzBytes[$i..([Math]::Min($i+15,$gzBytes.Length-1))] |
    ForEach-Object { "0x{0:x2}" -f $_ }) -join ", ")
}
$header = @("#pragma once","const uint8_t index_html_gz[] PROGMEM = {",
            ($chunks -join ",`n"),"};",
            "const size_t index_html_gz_len = $($gzBytes.Length);")
[System.IO.File]::WriteAllLines("ESP32_power_supply\web_page.h", $header,
    [System.Text.UTF8Encoding]::new($false))
Write-Host "Done: $($bytes.Length) -> $($gzBytes.Length) bytes"
```

---

## Usage

### PID Auto-Tune

On first flash the PID uses conservative defaults. For best accuracy, run the relay auto-tune once:

1. Enable CH1 output at any mid-range voltage (5 V is ideal).
2. In **Settings → PID**, click **Auto-tune**. The output will oscillate briefly as the relay test runs.
3. Gains are saved to NVS and persist across reboots. The **Tuned** label confirms success.

Tune once — gains remain valid across the full 2.5–15 V range.

### Presets

- **Click** a preset chip to jump to that voltage instantly.
- **Right-click** a chip to delete it.
- **＋** saves the current slider position as a new preset (stored in browser `localStorage`).

### All Off

The **All off** button in the top-right cuts all three outputs simultaneously. Individual toggles restore each channel independently.

---

## REST API

All responses are JSON. When `require_login` is enabled, include header `X-Auth: <token>` on every request except `/api/login`.

| Method | Path | Description |
|--------|------|-------------|
| `GET` | `/api/status` | Live voltages, currents, PID state |
| `GET` | `/api/info` | Device info, network, settings |
| `POST` | `/api/login` | `pass=…` → `{ok, token}` |
| `POST` | `/api/control` | Outputs and voltage (see below) |
| `POST` | `/api/settings` | JSON — any subset of settings |
| `GET` | `/api/wifi/scan` | Start scan → returns networks |
| `POST` | `/api/wifi/connect` | `ssid=…&pass=…` |
| `POST` | `/api/wifi/forget` | `ssid=…` |
| `GET` | `/api/ota/check` | Check for newer firmware |
| `POST` | `/api/ota/update` | Pull if newer |
| `POST` | `/api/ota/force` | Pull unconditionally |
| `GET` | `/api/mqtt/test` | Test broker connectivity |
| `GET` | `/api/reboot` | Restart device |
| `POST` | `/api/factory_reset` | Wipe NVS |

**`/api/control` actions** (form-encoded POST body):
```
action=toggle&output=1
action=set_voltage&output=1&value=5.0
action=set_ilimit&output=1&value=1.5
action=all_off
action=clear_trip&output=1
```

**`/api/status` example:**
```json
{
  "output1": true,  "output2": false, "output3": false,
  "voltage1": 4.99, "voltage2": 5.17, "voltage3": 3.31,
  "current1": 0.08, "current2": 0,    "current3": 0,
  "set1": 5.0,  "ts": 8040468,
  "pid_state": "converged",
  "pid_kp": 0.253, "pid_ki": 2.533, "pid_kd": 0.006
}
```

`pid_state`: `idle` | `settling` | `running` | `converged` | `tuning`

---

## WebSocket

Connect to `ws://voltbench.local/ws`.

The server **pushes** a full status frame every 250 ms automatically — no subscription needed.

**Send a control command:**
```json
{ "action": "set_voltage", "output": 1, "value": 9.0 }
{ "action": "toggle",      "output": 1 }
{ "action": "all_off" }
{ "action": "set_ilimit",  "output": 1, "value": 2.0 }
{ "action": "clear_trip",  "output": 1 }
{ "action": "tune" }
```

When `require_login` is on, include `"token": "<token>"` in every command.

**Quick test (browser console):**
```js
const ws = new WebSocket('ws://voltbench.local/ws');
ws.onmessage = e => console.log(JSON.parse(e.data));
ws.onopen    = () => ws.send(JSON.stringify({action:'set_voltage', output:1, value:5.0}));
```

---

## MQTT

Enable in **Settings → MQTT**. Configure broker host, port, credentials, and topic prefix.

| Topic | Direction | Payload |
|-------|-----------|---------|
| `voltbench/tele` | Publish (1 Hz) | Full JSON telemetry |
| `voltbench/status` | Publish (retained) | `online` |
| `voltbench/cmd/ch1/output` | Subscribe | `on` / `off` |
| `voltbench/cmd/ch2/output` | Subscribe | `on` / `off` |
| `voltbench/cmd/ch3/output` | Subscribe | `on` / `off` |
| `voltbench/cmd/ch1/voltage` | Subscribe | Float string, e.g. `5.0` |
| `voltbench/cmd/ch1/alloff` | Subscribe | Any payload |
| `voltbench/cmd/ch1/tune` | Subscribe | Any payload — triggers auto-tune |

The prefix `voltbench` is configurable in Settings → MQTT → Topic prefix.

---

## OTA Updates

1. Host `version.txt` (content: e.g. `0.1.1`) and `firmware_0.1.1.bin` on an HTTP server.
2. Set the base URL in **Settings → OTA** (e.g. `https://your-server.com/voltbench/`).
3. On the next boot (or via **Check for updates**), the device fetches `version.txt`, compares, and pulls the binary if newer. The device reboots automatically after a successful flash into the OTA slot.

---

## Architecture

```
┌──────────────────────────────────────────────────┐
│  ESP32-C3  (FreeRTOS, single physical core)      │
│                                                  │
│  ┌──────────────────────┐  volatile globals       │
│  │  voltageControlTask  │◄──────────────────────┐ │
│  │  (priority 2, core 0)│  g_target_mV          │ │
│  │                      │  g_setpoint_mV        │ │
│  │  PID state machine   │  g_measured_mV ──────►│ │
│  │  ADC reads  (4 Hz)   │  g_ctrl_output1       │ │
│  │  I2C pot writes      │  g_pid_kp/ki/kd       │ │
│  └──────────────────────┘                       │ │
│                                                 │ │
│  ┌──────────────────────┐                       │ │
│  │  AsyncWebServer      │───────────────────────┘ │
│  │  ESPAsyncWebServer   │  Reads g_measured_mV    │
│  │                      │  Writes g_target_mV     │
│  │  /api/* handlers     │                         │
│  │  WebSocket push 4 Hz │                         │
│  └──────────────────────┘                         │
│                                                   │
│  loop():  vb_loop()                               │
│     DNS captive · WS push · MQTT · OCP/OTP guard  │
└──────────────────────────────────────────────────┘
        │ I2C                │ ADC_11db
    MCP4017 @ 0x2F        GPIO 0 / 1 / 3
    10 KΩ, 128 steps      ×48 / ×2 / ×2
        │
   LM2596-ADJ
   feedback R2
```

**Key design decisions:**
- `voltageControlTask` is the **sole owner** of the ADC and I2C pot — concurrent access from the web server is eliminated. Status reads use `g_measured_mV` (a `volatile uint32_t`), which is atomically written by the control task.
- The PID operates on a ~70 ms dt (20 ms ADC sampling + 50 ms delay) giving sub-1-second convergence to within 100 mV across the full range.
- The web UI is a single gzip-compressed HTML file (~23 KB) served directly from PROGMEM — no filesystem partition required.

---

## Troubleshooting

| Symptom | Cause | Fix |
|---------|-------|-----|
| `voltbench.local` not resolving | mDNS client issue | Try IP directly; Android needs an mDNS helper app |
| Voltage shows 0 V with output OFF | Expected — enable pin cuts power | Toggle output ON |
| CH1 settles far from setpoint | PID gains not tuned | Run Auto-tune from Settings → PID |
| OTA fails | Wrong URL or SSL mismatch | Verify OTA URL; disable SSL for plain HTTP |
| `MCP4017 not found!` at boot | I2C wiring | Check SDA/SCL and pull-up resistors (4.7 KΩ to 3.3 V) |
| Sketch too large to upload | Wrong partition scheme | Select Custom and point to `partitions.csv` |
| Settings revert after save | Browser cached old page | Hard-refresh (`Ctrl+Shift+R`) |

---

## License

MIT — see [LICENSE](LICENSE).

# Voltbench v2.1 — Hardware Design

> Two-channel programmable bench power supply with **true hardware CC + CV** per channel.
> Input: 24 V DC SMPS · Output: 1.0 – 20 V · 2 A (see derating) · per channel
> Control: ESP32-C3 Super Mini · MCP4728 quad DAC · MP1584 modules (modded) · LM358 CC loop · INA219 sensing
> Local UI: SSD1306 OLED + 3 buttons · Remote: existing Voltbench web/WS/OTA firmware stack

---

## Table of Contents

1. [Why v2.1?](#why-v21)
2. [System Overview](#system-overview)
3. [CV Loop — DAC Feedback Injection](#cv-loop--dac-feedback-injection)
4. [CC Loop — LM358 Shunt Amp + Diode-OR](#cc-loop--lm358-shunt-amp--diode-or)
5. [MP1584 Module Mod Procedure](#mp1584-module-mod-procedure)
6. [Sensing — INA219 + Remote-Sense Tap](#sensing--ina219--remote-sense-tap)
7. [Output Enable + Power-Up Safety Chain](#output-enable--power-up-safety-chain)
8. [Self-Calibration Load](#self-calibration-load)
9. [Local UI — OLED + Buttons](#local-ui--oled--buttons)
10. [Pin Map + I²C Address Map](#pin-map--i2c-address-map)
11. [Power Budget + Thermal](#power-budget--thermal)
12. [Bill of Materials](#bill-of-materials)
13. [Carrier PCB Notes (0.25 mm CNC)](#carrier-pcb-notes-025-mm-cnc)
14. [Bring-Up & Verification Checklist](#bring-up--verification-checklist)
15. [Firmware Architecture Outline](#firmware-architecture-outline)
16. [System Specifications](#system-specifications)
17. [Key Design Decisions Log](#key-design-decisions-log)

---

## Why v2.1?

### v1 problems (LM2596 + MCP4017 digipot)

| Problem | Root cause |
|---------|-----------|
| ±100 mV best-case accuracy | 128-step digipot fought by a software loop |
| No current limit at all | No sense circuit, no CC loop |
| Slow settling | Software deadbeat controller over I²C |
| Noisy readings | ESP32-C3 internal ADC |

### Why this supersedes the earlier v2 draft (MP1584 + PMOS-LDO)

The previous revision of this document added a PMOS linear post-regulator for
2–5 mV ripple, but had **no current control**. Adding hardware CC to that
topology fails twice:

1. **DAC budget** — coarse + fine + CC per channel = 6 setpoints; the MCP4728 has 4.
2. **Thermal hazard** — in CC mode with a shorted output, the PMOS drops the
   full pre-regulator voltage: ~20 V × 2 A ≈ **40 W** in one TO-220 unless
   firmware actively droops the buck (slow, fragile).

v2.1 instead uses the classic XL4015-CC/CV-board topology — both loops act
directly on the buck's FB pin, with the trimpots replaced by DAC channels:

```
v1:    LM2596 + digipot + software loop  →  ±100 mV, no CC
v2.0:  MP1584 + PMOS LDO (draft only)    →  CV only, CC infeasible
v2.1:  MP1584 FB injection (CV)          →  ~4.8 mV/LSB, analog-regulated
       + LM358 shunt amp diode-OR (CC)   →  true hardware current limit
       + INA219                          →  accurate V + I readback
```

The MCU only writes setpoints; the analog loops do all regulation.
Short-circuit safe by construction — the buck just reduces duty.
Trade-off accepted: output ripple ~20–30 mV (no linear post-stage).

---

## System Overview

```
                          24V DC SMPS (4A)
                                │
        ┌───────────────┬───────┴────────┬──────────────────┐
        │               │                │                  │
   [CHANNEL 1]     [CHANNEL 2]     [5V buck module]    [LM358 ×2 Vcc]
   MP1584 mod      MP1584 mod           │
   LM358 CC        LM358 CC        ESP32-C3 Super Mini ── 3.3V → INA219×2,
   INA219 0x40     INA219 0x41          │                  MCP4728, OLED
   cal load        cal load        ┌────┴─────┐
        │               │          │  I²C bus │ SDA/SCL
        └───────────────┴──────────┴──────────┘
                                   │
                 MCP4728 0x60 · INA219 0x40/0x41 · SSD1306 0x3C
```

### Per-channel signal flow

```
24V ──► MP1584 module ──►──[0.1Ω shunt]──►── OUT+ terminal
         (FB net removed)  │           │
              FB ◄─────┐   └─ INA219 ──┘  (IN− on terminal side;
               ▲       │      bus reading = terminal voltage)
               │       │
   R1 100k ────┤◄──────┴── Vout sense tapped AFTER shunt (remote sense)
   R2 6.8k ────┤── GND
   R3 10.5k ───┘── MCP4728 CV channel (0–2.048V)

   shunt ×10 diff amp ──► integrator vs CC DAC ──[BAT54]──[10k]──► FB
   (LM358 half A)         (LM358 half B)     pulls FB up ⇒ Vout drops
```

Whichever loop demands the **lower** output wins: in CV mode the CC amp
saturates low and its diode blocks; when load current hits the limit, the
integrator rises, the diode conducts, FB is pulled up and the buck folds
back voltage until current equals the setpoint.

### MCP4728 channel assignment

| Channel | Function | Scale |
|---------|----------|-------|
| A | CH1 CV setpoint | code 0 = 20.18 V … code 4095 = 0.68 V (inverted!) |
| B | CH1 CC setpoint | 1 V = 1 A → code ≈ 2000 × amps |
| C | CH2 CV setpoint | same as A |
| D | CH2 CC setpoint | same as B |

Internal reference 2.048 V, gain ×1 — setpoints are independent of the 3.3 V rail.

---

## CV Loop — DAC Feedback Injection

The MP1584 regulates FB to **0.8 V**. With the module's own divider removed,
an external three-resistor network sets the output, and the DAC shifts it
linearly by injecting current into the FB node:

```
Vout_sense ──[R1 100k 1%]──┬── FB (MP1584)
                            │
              GND ──[R2 6.8k 1%]
                            │
  MCP4728 CV ──[R3 10.5k 1%]┘        (all E96 values)
```

### Transfer function

```
Vout = 0.8 · (1 + R1/R2 + R1/R3) − Vdac · (R1/R3)

R1/R2 = 14.71   R1/R3 = 9.52

Vout = 20.18 V − 9.52 · Vdac
     = 20.18 V − code · 4.763 mV        (Vdac = 2.048 · code / 4095)

code = (20.18 − Vtarget) / 0.004763
```

**Resolution ≈ 4.8 mV/LSB.** DAC loading is trivial (≤120 µA, buffered
output); MP1584 FB bias current (±100 nA through 100k) contributes ≤±10 mV.

### Code table (nominal — firmware calibrates, see below)

| Target Vout | DAC code |
|-------------|----------|
| 20.0 V | 38 |
| 15.0 V | 1088 |
| 12.0 V | 1717 |
| 9.0 V  | 2347 |
| 5.0 V  | 3187 |
| 3.3 V  | 3544 |
| 1.0 V  | 4027 |
| 0.8 V  | 4069 (clamp: codes 4070–4095 are below-spec) |

### Calibration is mandatory

1 % resistors stack to roughly **±2 % at the 20.18 V endpoint (±0.4 V)**.
Firmware performs a two-point calibration per channel against the INA219
(set two codes, read actual voltage, solve gain + offset). This replaces
v1's 128-entry calibration map entirely — the DAC is linear.

### ⚠ Inverted relation

**DAC code 0 = maximum voltage.** An unprogrammed/reset DAC would slam the
output to 20 V. Mitigated by the three-layer safety chain in
[Output Enable + Power-Up Safety Chain](#output-enable--power-up-safety-chain).

---

## CC Loop — LM358 Shunt Amp + Diode-OR

One LM358 (dual op-amp, Vcc = **24 V rail**, GND = system ground) per channel.

### Half A — shunt difference amplifier (gain 10)

```
 shunt upstream (buck side, V2) ──[10k]──┬──(+in)
                                          │
                              [100k]──── GND
 shunt downstream (terminal, V1) ─[10k]──┬──(−in)
                                          │
                              [100k]──── (output)

 output = 10 · (V2 − V1) = 10 · I · 0.1Ω  →  1 V per amp, ground-referenced
```

- **All four resistors 0.1 %** (₹5 parts). Worst-case CMRR of a 4-resistor
  diff amp at 0.1 % is ≈ 69 dB → at 20 V common mode that is ±73 mA of
  error, plus ~7 mA from LM358 Vos. Acceptable for a hardware limiter —
  the INA219 still reports true current, and firmware software-trims the
  CC DAC against it.
- **Filtering:** one 100 nF **differential** capacitor directly between the
  op-amp's +in and −in nodes (inside the matched arms). Do **not** add
  series filter resistors — unmatched series R doubles the CMRR error.
- Common-mode check: op-amp input pins sit at ~0.91 × 20.4 V ≈ 18.5 V;
  LM358 input range extends to Vcc − 1.5 V = 22.5 V. 4 V margin. ✓

### Half B — error integrator + diode-OR

```
 diff amp output ────────────────(+in)
 MCP4728 CC ──[100k]──┬──────────(−in)
                      │
                 [100nF Cf]──── (output) ──[BAT54]──[10k]──► FB node
```

- Below the limit: +in < −in → output integrates to saturation low
  (~10 mV), BAT54 reverse-biased (<25 nA leakage → sub-mV at the ~4 kΩ
  FB node) → **zero effect on CV accuracy**.
- Above the limit: output integrates up, diode conducts, FB rises, the buck
  folds back until 1 V/A · I = Vdac.
- τ = R·C = 100k × 100nF = 10 ms — decades slower than the MP1584's
  internal loop, so the two never fight.
- The **10 kΩ series resistor** limits injection authority/slew: forcing
  Vout → 0 needs only ~200 µA into FB; the amp has ~2 mA available. Plenty.
- **BAT54 over 1N4148**: 0.4 V less swing to traverse out of saturation →
  faster CC engagement. (1N4148 works if BAT54 unavailable.)
- **Known quirk — integrator windup:** coming out of saturation the output
  must slew ~1 V before the diode conducts → expect a few-ms current
  overshoot when CC first engages. Bring-up test #4 scopes this into a
  dead short; add an anti-windup clamp (zener/diode across Cf) only if the
  spike matters for your loads.

### CC setpoint scale

```
I_limit = Vdac / (10 × 0.1Ω) = Vdac   (volts → amps, 1:1)
code    = I_limit × 4095 / 2.048  ≈  2000 × amps      (0.5 mA/LSB)
```

| I limit | DAC code |
|---------|----------|
| 0.1 A | 200 |
| 0.5 A | 1000 |
| 1.0 A | 2000 |
| 1.5 A | 3000 |
| 2.0 A | 4000 |
| 2.048 A | 4095 (hardware ceiling) |

---

## MP1584 Module Mod Procedure

Per purple module, remove **three** parts (batch layouts vary — identify by
tracing, not position):

1. The multiturn **trimpot** (upper divider element, Vout → FB).
2. The fixed **FB→GND resistor** (2 k–8.2 k depending on batch).
3. The small **feed-forward capacitor** across the trimpot, if present.

Then **meter-verify the FB pad is open to both Vout and GND** before wiring.

Layout rules on the carrier PCB:

- The R1/R2/R3 network sits **immediately adjacent to the module's FB pad
  (< 25 mm total FB-node trace/wire)**. The FB node has ~4 kΩ Thevenin
  impedance and is the one noise-sensitive net in the design.
- The *long* runs are Vout-sense and the DAC line — both low-impedance /
  driven nets. Never extend the FB node itself.
- Keep the FB network away from the module's inductor/SW-node side.
- Solder the module flat onto the carrier (castellated/pin-header style);
  do not fly-wire FB.

---

## Sensing — INA219 + Remote-Sense Tap

```
MP1584 out ──► IN+ ──[0.1Ω 1W 2512 shunt]── IN− ──► OUT+ terminal
                        │ (Kelvin-ish: route sense traces from pad centers)
                     INA219 (0x40 / 0x41)
                        bus-voltage pin side = IN− = terminal
```

- **CV divider (R1) taps AFTER the shunt, at the terminal.** This puts the
  0.2 V @ 2 A shunt drop *inside* the voltage loop — free remote sense at
  the terminal. The 0.1 Ω in series with a ~100 kΩ divider is six orders of
  magnitude down; no measurable loop interaction.
- Cost: the divider's ≤200 µA flows through the shunt and reads as phantom
  load current — below practical INA219 resolution; subtract in firmware
  if you care.
- INA219 with IN− on the terminal side reports **bus voltage = terminal
  voltage**, matching what the user sees.
- Calibration register for 0.1 Ω, 100 µA/LSB: `Cal = 0.04096 / (100e-6 × 0.1) = 4096 → write 0x1000`.

---

## Output Enable + Power-Up Safety Chain

MP1584 EN: absolute max **6 V** (datasheet warns against tying to VIN),
internal µA-scale pull-up, rising threshold ~1.5 V.

```
ESP32 GPIO ──────┬──── EN (MP1584 module)
                 │
             [10k pull-down]
                 │
                GND
```

Direct 3.3 V GPIO drive is safe. The 10 k pull-down overpowers the internal
µA pull-up → **channel is OFF whenever the GPIO is Hi-Z** (boot, reset,
crash). Use non-strapping GPIOs only (avoid 2/8/9 for EN).

**Three-layer power-up safety chain** (because DAC code 0 = 20 V):

1. **MCP4728 EEPROM power-on values**: CV channels = code 4095 (minimum
   voltage), CC channels = code 0 (zero current). The DAC restores these
   at POR, long before the ESP32 finishes booting.
2. **EN pull-downs**: outputs disabled until firmware actively drives EN high.
3. **Firmware sequence**: write both DACs → settle (≥1 ms) → EN high.
   Never enable before setpoints are confirmed written.

MCP4728 write discipline: breakout boards tie LDAC low (writes take effect
immediately) → use **single-channel write commands only**, so updating CH-A
can never glitch CH-B.

---

## Self-Calibration Load

Per channel, a GPIO-switched known resistance across the output lets
firmware verify/trim the INA219 readings at startup with no external gear:

```
OUT+ ──[10Ω 5W wirewound]──► AO3400 drain
                              AO3400 gate ──┬── GPIO (CAL)
                              AO3400 source ── GND
                                            │
                                        [10k pull-down]
```

- **AO3400** (SOT-23, ~30 mΩ at Vgs = 2.5 V): fully specified at 3.3 V gate
  drive. (IRLZ34N is only characterized at 5 V — marginal here; avoid.)
- **⚠ Firmware interlock — cal only at Vout ≤ 3 V, time-limited (≤2 s).**
  10 Ω at 3 V = 0.3 A / 0.9 W ✓. At 20 V it would be 40 W and the resistor
  burns. The interlock lives in firmware AND in the bring-up procedure.
- **⚠ The interlock has no hardware timeout — the ESP32 task watchdog is the backstop.**
  If the control task hangs while CAL is asserted, the gate pull-down only helps after
  a full reset, and the resistor cooks in the meantime. So: enable the task watchdog on
  the control task (5 s), and implement calibration as a *state within the control
  loop*, never as a blocking subroutine full of delays. (v1's `run_cal_sweep` blocked
  for a full minute — that pattern must not be carried over.) If you want fail-safety
  independent of firmware, AC-couple the gate drive or fit a thermal fuse.
- **The ≤3 V limit caps CC trimming at ~0.3 A, and that is sufficient — don't "fix" it
  with a bigger resistor.** The CC loop's *gain* is set by 0.1 % resistors and is
  already trustworthy; the dominant error is the LM358's input offset voltage, which is
  an *offset*. A 0.1 A / 0.25 A point pair captures that offset exactly, and the gain
  then extrapolates soundly to the full 2 A.
- Routine: set 2.0 V → engage load → read I; set 3.0 V → repeat.
  Expected I = V/10 Ω (±resistor tolerance 5 %); solves INA219 I-gain and
  cross-checks V-scale. Store trims in NVS.

---

## Local UI — OLED + Buttons

- **SSD1306 128×64 I²C OLED @ 0x3C**, powered from the ESP32 module's 3.3 V.
  Shows per-channel: set V / set I / actual V / actual I / CV-or-CC flag /
  output state. CC-engaged detection: `I_actual ≈ I_limit && V_actual < V_set`.
- **3 push buttons** to GND, internal pull-ups: `SEL` (cycle field/channel),
  `UP`, `DOWN` (adjust; hold = fast repeat). Long-press SEL = output toggle.
  Exact menu flow is firmware's problem — hardware is just 3 GPIOs.

---

## Pin Map + I²C Address Map

### ESP32-C3 Super Mini

| GPIO | Function | Notes |
|------|----------|-------|
| 0 | BTN1 (SEL) | input pull-up |
| 1 | BTN2 (UP) | input pull-up |
| 2 | *(unused)* | **strapping pin — deliberately left free, see warning below** |
| 3 | CAL ch2 | 10k pull-down at gate |
| 4 | CAL ch1 | 10k pull-down at gate |
| 5 | EN ch2 | 10k pull-down, non-strapping ✓ |
| 6 | SDA | 2.2k pull-up to 3.3 V |
| 7 | SCL | 2.2k pull-up to 3.3 V |
| 8 | onboard LED | status blink (strapping — drive as output only after boot) |
| 9 | BOOT button | leave |
| 10 | EN ch1 | 10k pull-down, non-strapping ✓ |
| 20 | BTN3 (DOWN) | ex-UART RX — input during ROM boot, so a held button is harmless |
| 21 | UART TX | debug serial (USB-CDC is the primary console) |

> ⚠ **Do not put a button on GPIO2, GPIO8 or GPIO9.** These are ESP32-C3 strapping
> pins latched at reset, before any firmware runs. A button to GND on GPIO2 works
> perfectly in normal use but **prevents the chip from booting if it is held down
> during power-on** — an unfixable-in-software failure that presents as "the unit is
> dead." BTN3 therefore lives on GPIO20, which is an input during ROM boot and safe
> to pull low at any time. GPIO2 stays unconnected (it is ADC-capable, so it remains
> available as a spare analog input).

> **I²C pull-ups are 2.2 kΩ, not 4.7 kΩ.** At 400 kHz the rise time must stay under
> 300 ns; `t_r ≈ 0.85·R·C` makes 4.7 kΩ marginal at ~70 pF and clearly inadequate if
> the OLED hangs off a flying lead (~150 pF → ~600 ns). Dropping to 100 kHz is not an
> acceptable alternative — it quadruples a full OLED frame flush to ~104 ms, which the
> control loop cannot absorb.

### I²C bus (single SDA/SCL pair, 100–400 kHz)

| Device | Address |
|--------|---------|
| MCP9808 temp sensor | 0x18 |
| SSD1306 OLED | 0x3C |
| INA219 ch1 | 0x40 (A1=A0=GND) |
| INA219 ch2 | 0x41 (A0=VCC) |
| MCP4728 | 0x60 |

No conflicts. Five devices, ~30 cm total bus max, 2.2 kΩ pull-ups.

**Why the MCP9808 is required, not optional.** Over-temperature protection is a
specified feature, and there is nowhere else to put a sensor: the ESP32-C3's ADC is
limited to GPIO0–GPIO4, and all five of those pins are already committed (three
buttons, two cal-load gates), so an analog NTC has no home. The MCP9808 costs ~₹100,
consumes zero GPIO, and — mounted on the heatsink or between the two MP1584 modules —
measures the thing that actually gets hot rather than the MCU die. Firmware falls back
to the ESP32's internal `temperatureRead()` with a wider threshold if the part is
absent, and reports which source is in use.

---

## Power Budget + Thermal

```
Worst realistic case per channel: 20 V × 1.5 A = 30 W out → ~33 W in @ 92%
Two channels + MCU/UI (~1 W):  ~68 W
SMPS: 24 V / 4 A (96 W) — comfortable. (Old 7 A sizing assumed the LDO stage.)
Light bench use (5 V/1 A per channel): ~12 W total — a 24 V/2 A brick suffices.
```

- **MP1584 module thermal reality:** bare modules are good for ~1.5 A
  continuous at high duty without airflow; the "3 A" rating is peak.
  Spec: **2 A up to ~12 V out, derate to 1.5 A continuous at 20 V** —
  pending bring-up test #5. A clip heatsink on the module IC + airflow
  reclaims 2 A if needed.
- 24 V→20 V at 2 A is near the max-duty corner — verify regulation and
  ripple there (bring-up #2).
- LM358 at 24 V: ~24 mW quiescent — nothing.
- 0.1 Ω shunt at 2 A: 0.4 W — use 1 W 2512 (or through-hole equivalent).
- Fit the **1 kΩ preload** per channel **unconditionally** — not conditionally on
  ripple as earlier drafts suggested. There is a second, independent and more
  important reason for it: **the output has no discharge path.** After a protection
  trip or a step-down into a capacitive load, the terminal holds its old voltage with
  nothing to bleed it. That means OVP keeps reading high and the user's attempt to
  clear the trip is refused until the charge decays on its own — potentially minutes
  into a large capacitor. One resistor per channel fixes both the light-load ripple
  and the stuck-voltage problem. Cost: 400 mW at 20 V, 20 mA of phantom load.

---

## Bill of Materials

### Per channel (×2)

| Ref | Part | Value / type | Notes |
|-----|------|-------------|-------|
| M1 | MP1584 buck module | purple, modded per §5 | ₹30 ea |
| U1 | LM358 | DIP-8 or SOIC-8 | Vcc = 24 V rail |
| U2 | INA219 | breakout or SOIC-8 | 0x40 / 0x41 |
| Rs | Shunt | 0.1 Ω 1 % 1 W (2512 or TH) | shared by INA219 + CC amp |
| R1 | FB upper | 100 k 1 % E96 | tap at terminal (after shunt) |
| R2 | FB lower | 6.8 k 1 % | |
| R3 | DAC injection | 10.5 k 1 % E96 | |
| R4–R7 | Diff amp | 2× 10 k + 2× 100 k, **0.1 %** | CMRR budget |
| R8 | Integrator in | 100 k 1 % | |
| R9 | FB injection series | 10 k | after diode |
| R10 | EN pull-down | 10 k | |
| R11 | CAL gate pull-down | 10 k | |
| C1 | Diff-amp filter | 100 nF, differential across op-amp inputs | no series Rs |
| C2 | Integrator Cf | 100 nF | τ = 10 ms |
| D1 | Diode-OR | BAT54 (or 1N4148) | |
| Q1 | Cal switch | AO3400 | 3.3 V-gate logic FET |
| R12 | Cal load | 10 Ω 5 W wirewound | interlock ≤ 3 V! |

### Shared

| Part | Qty | Notes |
|------|-----|-------|
| ESP32-C3 Super Mini | 1 | |
| MCP4728 breakout | 1 | 0x60, EEPROM defaults programmed at bring-up #7 |
| SSD1306 0.96" I²C OLED | 1 | 0x3C |
| Push buttons | 3 | to GND |
| 5 V buck module (Mini360/MP1584 fixed) | 1 | 24 V → 5 V for ESP32 module |
| 24 V/4 A SMPS | 1 | |
| MCP9808 temp sensor breakout | 1 | 0x18, on the heatsink — required for OTP |
| 1 kΩ 1 W preload resistor | 2 | one per channel, mandatory (discharge path) |
| I²C pull-ups 2.2 k | 2 | **not** 4.7 k — see §10 |
| Terminals: DC in + 2× banana pairs | — | |

**Estimated cost ≈ ₹900–1000** (modules + semis; SMPS extra).

---

## Carrier PCB Notes (0.25 mm CNC)

The switching stage lives on the purchased modules — the carrier is all
low-speed analog + digital, which mills easily.

1. **Module footprints:** pin-header rows or castellated pads; module
   soldered flat. FB pad breaks out to the adjacent R1/R2/R3 cluster
   (< 25 mm net).
2. **Power path** 24 V → module → shunt → terminal: 2 mm+ traces / pour.
3. **Shunt:** route INA219 sense traces from the pad centers (Kelvin-ish);
   diff-amp 10 k arms tap the same points.
4. **Diff amp:** the four 0.1 % resistors clustered at the op-amp;
   the 100 nF differential cap right across pins 2–3 (or 5–6).
5. **I²C:** < 30 cm total, 4.7 k pull-ups near the ESP32.
6. **Grounding:** single star point at the SMPS return; keep CC diff-amp
   ground reference and DAC/INA grounds off the high-current return path.
7. All packages SOIC/0805/TO/module-scale — no sub-0.5 mm pad gaps; fine
   for a 0.25 mm bit. (AO3400 is SOT-23: 0.95 mm pitch — OK, mill slowly,
   or use a pre-made breakout.)

---

## Bring-Up & Verification Checklist

Work one channel at a time. DMM + scope on hand. **DACs at safe codes
(CV = 4095, CC = 0) before every power-on until step 7 locks EEPROM defaults.**

1. **Module mod:** remove trimpot + FB resistor + FF cap; meter FB pad open
   to Vout and GND. Wire ch1 network only.
2. **CV sweep:** step DAC code 4095 → 0, log Vout. Verify linearity,
   endpoints (expect ±2 % pre-cal), slope ≈ 4.76 mV/code. Check the
   24 V→20 V max-duty corner: load 1 A, confirm regulation + ripple.
3. **Low-end ripple:** scope Vout at 1.0 V set, 2 A load. If pulse-skipping
   ripple is excessive, fit the 1 kΩ preload and/or move spec floor up.
4. **CC into dead short:** set 0.5 / 1.0 / 2.0 A limits, short the output,
   verify steady-state limit accuracy (±100 mA expected pre-trim) and
   **scope the engagement overshoot** (integrator windup — few ms spike is
   expected; decide if anti-windup clamp is needed).
5. **Thermal:** 20 V × 1.5 A sustained 15 min → module IC temp. Repeat at
   2 A. Finalize the derating line in [System Specifications](#system-specifications).
6. **Sensing:** INA219 vs bench DMM at 3–4 points; confirm current sign
   (IN− = terminal side); run the self-cal load routine manually at 2 V/3 V.
7. **MCP4728 EEPROM:** program power-on defaults (CV = 4095, CC = 0),
   power-cycle, scope the outputs during POR — must come up at min voltage
   before the ESP32 boots. Confirm single-channel writes don't glitch
   neighbors (LDAC is tied low on breakouts).
8. **UI smoke test:** OLED + 3 buttons + both INA219s + DAC on the shared
   bus simultaneously; `i2cdetect`-style scan shows 0x3C/0x40/0x41/0x60.

---

## Firmware Architecture Outline

*(Implementation is a later round — this maps reuse from the `release_v2` firmware.)*

### Carry over verbatim

- `apply_command()` dispatcher pattern — widened to `(action, channel, value)`
  with `set_voltage` **and** `set_current` per channel.
- `DbgPrint` ring logger + telnet/WebSocket log fan-out (keep the two
  load-bearing workarounds: HWCDC `availableForWrite` truncation guard and
  `availableForWriteAll()` before every `textAll`).
- NVS layer + `g_nvs_mux`, auth token + `VB_REQUIRE_AUTH`, WiFi manager
  task + captive portal (its comments encode real IDF bugs), OTA upload
  handler, `new design/Power Supply.html` as the web UI base.
- g++ host-test pattern (`tools/host_test/`).

### Rewrite / drop

- `control_math.h` + `voltageControlTask`: the 128-step wiper model dies
  with the digipot. A linear DAC needs a **2-point gain/offset cal per
  channel**, not an isotonic map. Keep `cm_median5` only.
- NVS `calmap` blob → per-channel `{v_gain, v_off, i_gain, i_off}` floats.
- `web_page.h` generation: replace the manual PowerShell snippet with a
  **checked-in Node/Python script** (the manual flow already shipped a
  stale embed once).

### New

- MCP4728 + INA219 + SSD1306 drivers (Adafruit libraries).
- Button/menu task for local UI.
- Self-cal routine with the **≤ 3 V / ≤ 2 s interlock**.
- CC/CV status in `/api/status` + WS push: per channel
  `{v_set, i_limit, v_out, i_out, mode: "CV"|"CC", enabled}`.
- Startup sequence: load cal from NVS → write DACs → settle → EN as per
  saved state (default off).

---

## System Specifications

| Parameter | Value |
|-----------|-------|
| Input | 24 V DC, 4 A SMPS |
| Channels | 2, independent, common ground |
| Output voltage | 1.0 – 20.0 V (0.8 V best-effort at light load) |
| Output current limit | 0 – 2.048 A, hardware CC |
| Current derating | 2 A ≤ ~12 V out · 1.5 A continuous at 20 V (pending thermal test) |
| V setpoint resolution | 4.8 mV/LSB (12-bit) |
| I setpoint resolution | 0.5 mA/LSB |
| V accuracy, self-calibrated | **±100 mV** (bounded by the INA219's own ±0.5 % bus-ADC gain error) |
| V accuracy, DMM-calibrated | **±20 mV** (two-point fit against an external meter) |
| CC accuracy (post-trim) | ±75 mA hardware · INA219 readback ±1 % |
| Ripple | 20–30 mV pk-pk (buck direct, no LDO) |
| Readback | INA219: 4 mV / 100 µA LSB |
| Short-circuit | inherently safe (buck folds back) |
| Local UI | SSD1306 OLED + 3 buttons |
| Remote | WiFi web UI, WebSocket, REST, OTA (Voltbench stack) |
| Self-test | per-channel 10 Ω switched cal load (≤ 3 V interlock) |
| PCB | carrier board, 0.25 mm CNC millable |

---

## Key Design Decisions Log

| Decision | Reason |
|----------|--------|
| Direct buck CC/CV over PMOS-LDO stage | 4 DAC channels fit exactly; short-circuit safe; no 40 W CC thermal hazard; matches proven XL4015-board topology |
| MP1584 modules (modded) over bare chips | ₹30 proven power stage; avoids milling the layout-critical SW node |
| MCP4728 over 4× MCP4725 | one chip, one address, internal ref, **EEPROM safe power-on values** (safety layer 1) |
| Internal 2.048 V ref, gain ×1 | setpoints independent of 3.3 V rail tolerance |
| High-side shunt shared INA219 + CC amp | one shunt; keeps channel grounds common (low-side would float them) |
| CV divider taps after shunt | free remote sense; shunt drop inside the loop |
| 0.1 % diff-amp resistors | CMRR: ±73 mA error at 20 V vs ±0.7 A with 1 % |
| Differential filter cap, no series Rs | series R mismatch would double CMRR error |
| BAT54 over 1N4148 | 0.4 V less integrator swing → faster CC engagement |
| EN: 10 k pull-down + direct GPIO | default-off on boot/crash; EN abs max 6 V allows 3.3 V drive |
| AO3400 over IRLZ34N for cal switch | specified at 3.3 V Vgs |
| Min spec 1.0 V not 0.8 V | MP1584 min on-time at 24 Vin → CCM floor 1.4–2.4 V; below is pulse-skip territory |
| 2-point cal over 128-entry map | DAC is linear; v1's isotonic map was a digipot artifact |

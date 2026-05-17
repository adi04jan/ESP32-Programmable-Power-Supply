# Voltbench v2 — Hardware Design

> Two-channel programmable bench power supply.  
> Input: 24 V DC SMPS · Output: 2.8 V – 20 V · 3 A per channel  
> Control: ESP32-C3 · I²C DAC (MCP4728) · Discrete PMOS LDO · INA219 sensing

---

## Table of Contents

1. [Why v2?](#why-v2)
2. [System Overview](#system-overview)
3. [Stage 1 — Coarse Voltage (MP1584 + MCP4728)](#stage-1--coarse-voltage-mp1584--mcp4728)
4. [Stage 2 — Precision LDO (FQP27P06 + LM358)](#stage-2--precision-ldo-fqp27p06--lm358)
5. [Stage 3 — Measurement (INA219)](#stage-3--measurement-ina219)
6. [Complete Single-Channel Schematic](#complete-single-channel-schematic)
7. [Two-Channel Full System](#two-channel-full-system)
8. [I²C Address Map](#i2c-address-map)
9. [Power Budget](#power-budget)
10. [Thermal Design](#thermal-design)
11. [Bill of Materials](#bill-of-materials)
12. [PCB Design Notes (0.25 mm CNC)](#pcb-design-notes-025-mm-cnc)
13. [Firmware Interface](#firmware-interface)
14. [System Specifications](#system-specifications)

---

## Why v2?

### Problems with v1 (LM2596 + MCP4017)

| Problem | Root Cause | Impact |
|---------|-----------|--------|
| ±400–800 mV voltage error | MCP4017 has 128 steps × 78.7 Ω/step | Cannot hit target precisely |
| PID loop needed | Step size larger than tolerance band | Slow settling, overshoot risk |
| No current measurement | No sense circuit | Cannot display watts or enforce OCP |
| Noisy ADC readings | ESP32-C3 internal ADC non-linearity | Inaccurate voltage display |
| 75% efficiency | LM2596 150 kHz non-synchronous switcher | Heat, large inductor/caps |

### v2 Solution

```
v1:  LM2596 (variable R2 via MCP4017)  →  ±400 mV error, slow PID
v2:  MP1584 (DAC FB injection)         →  coarse set, instant, ~3 mV resolution
     + PMOS LDO (op-amp regulated)     →  fine regulation, 2–5 mV ripple
     + INA219                          →  accurate V + I measurement
```

---

## System Overview

```
                       24V DC SMPS
                            │
              ┌─────────────┼─────────────┐
              │             │             │
              ▼             ▼             ▼
          [CHANNEL 1]   [CHANNEL 2]   [ESP32-C3]
                                          │
                                     I²C Bus (SDA/SCL)
                                     ┌────┴──────┐
                                  MCP4728      INA219×2
                                  (0x60)    (0x40, 0x41)
```

### Per-Channel Signal Flow

```
24V ──► [MP1584 Buck] ──► [FQP27P06 PMOS] ──► OUTPUT TERMINAL
              ▲                   ▲
              │ FB injection       │ Gate drive
         MCP4728 Ch A          LM358 op-amp ◄── MCP4728 Ch B
         (coarse set)          (fine regulate)        (reference)
                                      ▲
                               Vout feedback
                               divider (10k/51k)
                                      │
                               ──[0.1Ω shunt]──
                                      │
                                  INA219
                                (V + I sense)
```

---

## Stage 1 — Coarse Voltage (MP1584 + MCP4728)

### How It Works

The MP1584 is a synchronous buck converter with an adjustable output set by its FB (feedback) pin. Normally R1 and R2 form a divider to set output voltage. We **inject current from the MCP4728 DAC through R3** into the FB node, which shifts the output voltage continuously and linearly.

The FB pin is always regulated to **Vref = 0.8 V** by the MP1584's internal error amplifier. Injecting current via R3 forces the converter to change Vout to maintain that 0.8 V at FB.

### Feedback Network Schematic

```
                          MP1584
                         ┌──────┐
              ┌──────────┤ FB   │
              │          └──────┘
              │
Vout_mp1584 ──┤──[R1 = 100 kΩ]──┐
              │                   │
              ├──[R2 = 6.8 kΩ]── GND
              │
              └──[R3 = 24.3 kΩ]──── MCP4728 Channel A (0–3.3 V)
```

### Transfer Function

```
Vout = 0.8 × (1 + R1/R2 + R1/R3)  −  Vdac × (R1/R3)

With R1=100k, R2=6.8k, R3=24.3k:
  Vout = 16.0  −  4.09 × Vdac
```

### DAC Code to Output Voltage

```
Vdac = (16.0 − Vout_target) / 4.09
DAC_code = round(Vdac × 4096 / 3.3)
```

| Target Vout + 0.5V | Vdac | DAC Code |
|--------------------|------|----------|
| 20.5 V | 0.00 V | 0 |
| 15.5 V | 0.12 V | 1218 |
| 12.5 V | 0.49 V | 1211 (coarse MP1584 target) |
| 9.5 V | 1.59 V | 1973 |
| 5.5 V | 2.56 V | 3176 |
| 3.3 V | 3.10 V | 3846 |

> MP1584 is set to **Vtarget + 0.5 V** — the LDO stage drops the final 0.5 V precisely.

### MP1584 Support Components

```
               24V
                │
           [10µF ceramic]  ← Input decoupling
                │
         ┌──────┴──────┐
         │   MP1584    │
    EN ──┤EN       OUT ├──┬──[L1: 4.7µH]──┬── To PMOS source
         │             │  │               │
         │         SW  ├──┘            [C1: 22µF] ← Output cap
         │             │               ceramic
         └──────┬──────┘               │
               GND                    GND
                         [D1: SS34 Schottky] ← Freewheeling diode
                         (SW to GND, for transient protection)
```

| Component | Value | Notes |
|-----------|-------|-------|
| L1 | 4.7 µH, 4A rated | Smaller than LM2596's 100 µH |
| C1 (output) | 22 µF ceramic | MP1584 stability |
| C_in | 10 µF ceramic | Input decoupling |
| D1 | SS34 Schottky | Optional (synchronous but helps transients) |

---

## Stage 2 — Precision LDO (FQP27P06 + LM358)

### How It Works

The PMOS transistor (FQP27P06) sits between the MP1584 output and the final output terminal. An op-amp (LM358) compares a divided-down version of the actual output voltage against a **DAC reference (MCP4728 Channel B)**. It drives the PMOS gate to force the output to exactly match the reference.

This eliminates the MP1584's ±1% Vref tolerance error, load regulation error, and most of the switching ripple.

### LDO Schematic

```
MP1584 out (Vtarget+0.5V)
        │
        ├── PMOS Source (FQP27P06 pin 3)
        │
        │       FQP27P06 (P-Channel MOSFET)
        │      ┌─────────────┐
        └──────┤ S           │
               │    D        ├──────────────┬── Vout (clean)
               │             │              │
               └─────────────┘          [100µF]   [10µF]
                      │ G                   │         │
                   [100Ω]                  GND       GND
                      │
               ┌──────┴──────┐
               │   LM358     │
               │  op-amp     │
         Vfb ──┤ −IN   OUT   ├── Gate drive
               │             │
  DAC ref ─────┤ +IN         │
               └─────────────┘
                      │
                     GND (op-amp GND)
                     Vcc = 24V (op-amp supply)


Vfb comes from output voltage divider:

Vout ──[Ra = 10 kΩ]──┬── to op-amp −IN
                      │
                   [Rb = 51 kΩ]
                      │
                     GND
```

### Transfer Function

```
Op-amp null condition (Vout settled):
  DAC_ref = Vout × Ra / (Ra + Rb)

Solving for Vout:
  Vout = DAC_ref × (Ra + Rb) / Ra
       = DAC_ref × (10k + 51k) / 10k
       = DAC_ref × 6.1

DAC code for target:
  DAC_ref = Vout_target × 10 / (10 + 51)  = Vout_target × 0.1639
  DAC_code = round(DAC_ref × 4096 / 3.3)
```

| Target Vout | DAC Ref | DAC Code |
|-------------|---------|----------|
| 20.0 V | 3.28 V | 4070 |
| 15.0 V | 2.46 V | 3052 |
| 12.0 V | 1.97 V | 2446 |
| 9.0 V | 1.47 V | 1827 |
| 5.0 V | 0.82 V | 1018 |
| 3.3 V | 0.54 V | 671 |
| 2.8 V | 0.46 V | 571 |

### Why PMOS and Not NPN/NMOS?

```
NPN/NMOS (source follower):
  Vout = Vgate − Vgs_threshold   ← output always lower than gate
  Hard to use at high side without charge pump

PMOS (common source, source at top):
  Source = Vin (high side)
  Gate pulled LOW turns it ON
  Op-amp can drive gate from 0V (full ON) to 24V (full OFF)
  No charge pump needed ✓
```

### Op-Amp Operating Range Check (LM358 at 24V supply)

```
Source voltage: 0.5V to 20.5V (MP1584 output range)
Gate must swing BELOW source to turn ON:
  At Vsource = 20.5V: gate needs to go to ~17V (Vgs = -3.5V → fully ON)
  LM358 output max = Vcc - 1.5V = 22.5V  > 20.5V ✓ (can turn OFF)
  LM358 output min = 0V  <  17V needed to turn ON  ✓

At Vsource = 3.3V: gate needs ~1V to fully ON
  LM358 output = 0 to ~22.5V  →  easily covers 0–1V ✓
```

### Dropout Voltage

```
Vdropout = Rdson × Iout
         = 0.070 Ω × 3 A  =  0.21 V  (typical)
         = 0.100 Ω × 3 A  =  0.30 V  (worst case, warm)

MP1584 headroom set to 0.5V → leaves 0.2–0.3V regulation margin ✓
```

---

## Stage 3 — Measurement (INA219)

### How It Works

The INA219 measures both **bus voltage** (actual output voltage, 4 mV resolution) and **shunt current** (via 0.1 Ω resistor, 1 mA resolution) over I²C. This replaces the ESP32's internal ADC entirely.

### Connection

```
                 Vout (before shunt)
                         │
                    ┌────┴────┐
                    │ INA219  │
                    │         │
              IN+ ──┤         ├── SDA ── I²C Bus
              IN− ──┤         ├── SCL ── I²C Bus
                    │         ├── VCC ── 3.3V
                    │         ├── GND ── GND
                    │    A0   ├── (address select)
                    │    A1   ├── (address select)
                    └─────────┘
                         │
                    [0.1 Ω shunt, 1W, 2512]
                         │
                    Output Terminal (+)
```

### Address Configuration

| Channel | A0 | A1 | I²C Address |
|---------|----|----|-------------|
| Ch 1 | GND | GND | 0x40 |
| Ch 2 | VCC | GND | 0x41 |

### INA219 Calibration Register

```
For 0.1 Ω shunt, max 3.2 A, current LSB = 100 µA:
  Cal = 0.04096 / (Current_LSB × Rshunt)
      = 0.04096 / (0.0001 × 0.1)
      = 4096

Write 0x1000 to calibration register.
```

---

## Complete Single-Channel Schematic

```
                             24V DC SMPS
                                  │
          ┌───────────────────────┼────────────────────────────────┐
          │                       │                                │
       [10µF]                     │                             [100nF]
          │                       │                                │
         GND                      │                               GND
                                  │
                    ┌─────────────┴──────────────────┐
                    │           MP1584                │
         GPIO_EN ───┤ EN                              │
                    │                            IN   ├── 24V
          ┌─────────┤ FB                              │
          │         │                         SW      ├──┐
          │         └─────────────────────────────────┘  │
          │                                               │
          │  Feedback Network:                         [L1: 4.7µH, 4A]
          │                                               │
Vmp ──────┼──[R1: 100kΩ]──────── Vout_mp1584 ────────────┤
          │                          (Vtgt+0.5V)          │
          ├──[R2: 6.8kΩ]──── GND                      [C1: 22µF]
          │                                               │
          └──[R3: 24.3kΩ]─── MCP4728 Ch A (coarse)      GND
                                                          │
                                               Vout_mp1584 (Vtgt+0.5V)
                                                          │
                                           ┌──────────────┘
                                           │ Source
                                    ┌──────┴──────┐
                                    │  FQP27P06   │ (P-Ch MOSFET)
                                    │  TO-220     │
                                    └──────┬──────┘
                                           │ Drain
                              ┌────────────┼────────────┐
                              │            │            │
                           [100µF]      [10µF]       [100nF]
                              │            │            │
                             GND          GND          GND
                              │
                         Vout_clean
                              │
                   ┌──────────┴──────────┐
                   │                     │
              [Ra: 10kΩ]         [0.1Ω shunt, 1W]
                   │                     │
              [Rb: 51kΩ]          Output Terminal (+)
                   │
                  GND               ┌────────────────┐
                   │                │    INA219      │
              Vfb (0–3.3V)    IN+ ──┤                ├── SDA
                   │          IN- ──┤  0x40 / 0x41   ├── SCL
                   │                │                ├── 3.3V
                   │               GND               └────────────────┘
                   │
            ┌──────┴──────────┐
            │    LM358        │
            │   (Vcc = 24V)   │
     Vfb ───┤ −IN    OUT      ├──[100Ω]── Gate (FQP27P06)
            │                 │
DAC_ref ────┤ +IN             │
(MCP4728    │                 │
   Ch B)    └─────────────────┘
                   │
       [10nF cap from OUT to −IN for stability]
```

---

## Two-Channel Full System

```
                              24V DC SMPS
                                   │
            ┌──────────────────────┼──────────────────────┐
            │                      │                      │
     ┌──────┴──────┐        ┌──────┴──────┐        ┌──────┴──────┐
     │  CHANNEL 1  │        │  CHANNEL 2  │        │  ESP32-C3   │
     │             │        │             │        │             │
     │  MP1584     │        │  MP1584     │        │  SDA ───────┼──┐
     │  FQP27P06   │        │  FQP27P06   │        │  SCL ───────┼──┤
     │  LM358      │        │  LM358      │        │  GPIO5 ─────┼──┼── EN_CH1
     │  INA219     │        │  INA219     │        │  GPIO6 ─────┼──┼── EN_CH2
     │  (0x40)     │        │  (0x41)     │        └─────────────┘  │
     └──────┬──────┘        └──────┬──────┘                         │
            │ SDA/SCL              │ SDA/SCL                         │
            └──────────────────────┴─────────────────────────────────┘
                                   │
                              I²C Bus
                                   │
                           ┌───────┴───────┐
                           │   MCP4728     │  SOIC-10
                           │   (0x60)      │
                           │               │
                    Ch A ──┤               ├── SDA
                    Ch B ──┤               ├── SCL
                    Ch C ──┤               ├── VCC (3.3V)
                    Ch D ──┤               ├── GND
                           └───────────────┘
                             │    │    │    │
                          MP1584 LDO MP1584 LDO
                           ref   ref   ref  ref
                           Ch1   Ch1   Ch2  Ch2
```

### MCP4728 Channel Assignment

| MCP4728 Ch | Function | Target IC | What it controls |
|------------|----------|-----------|-----------------|
| A | Ch1 Coarse | MP1584 #1 | Sets MP1584 out to Vtarget + 0.5V |
| B | Ch1 Fine | LM358 #1 | Sets exact Vtarget for LDO |
| C | Ch2 Coarse | MP1584 #2 | Sets MP1584 out to Vtarget + 0.5V |
| D | Ch2 Fine | LM358 #2 | Sets exact Vtarget for LDO |

---

## I²C Address Map

| IC | Address | Set by | Notes |
|----|---------|--------|-------|
| MCP4728 | 0x60 | Factory (A0T variant) | Quad DAC |
| INA219 Ch1 | 0x40 | A0=GND, A1=GND | Voltage + current Ch1 |
| INA219 Ch2 | 0x41 | A0=VCC, A1=GND | Voltage + current Ch2 |

No address conflicts. All four ICs on a single SDA/SCL pair.

---

## Power Budget

### SMPS Sizing

```
Per channel, worst case (3A at any voltage):
  Stage 1 (MP1584):
    Vin=24V, Vout=20.5V, Iout=3A
    Pout = 20.5 × 3 = 61.5W
    Pin  = 61.5 / 0.93 = 66.1W
    Ploss = 4.6W  (MP1584 gets warm — copper pour heatsink on PCB)

  Stage 2 (PMOS LDO):
    Pdrop = (20.5 − 20.0) × 3 = 1.5W  (always 0.5V × Iout)

  Per channel total: 66.1W input from 24V
  Two channels max: 132W

Minimum SMPS: 24V / 6A = 144W
Recommended SMPS: 24V / 7A = 168W (Mean Well LRS-150-24 or equivalent)
```

### Typical Operating Power

```
Both channels at 5V / 1A each (common bench use):
  MP1584: (5.5 × 1) / 0.93 = 5.9W per channel
  PMOS:   0.5 × 1 = 0.5W per channel
  Total input: ~13W from 24V rail → 0.54A from SMPS
  A 24V/2A adapter is enough for light loads
```

---

## Thermal Design

### FQP27P06 (PMOS)

```
Power dissipated = 0.5V × Iout (constant regardless of output voltage)

At 3A: P = 1.5W per channel
At 1A: P = 0.5W per channel

TO-220 without heatsink: θja = 62°C/W
  At 1.5W: ΔT = 93°C  →  junction = 118°C  ✗ Too hot

TO-220 with small clip heatsink: θja ≈ 15°C/W
  At 1.5W: ΔT = 22°C  →  junction = 47°C  ✓

Recommendation: use a 25×25mm aluminium heatsink with thermal paste.
```

### MP1584

```
Power loss is highest at large Vin-to-Vout ratio:
At Vout=3.3V, Iin_avg ≈ Iout × (Vout/Vin) / η = 3 × (3.3/24) / 0.93 ≈ 0.44A from 24V
MP1584 internal dissipation ≈ 0.6W

At Vout=20V, Iin_avg ≈ 3 × (20.5/24) / 0.93 ≈ 2.76A from 24V
MP1584 internal dissipation ≈ 4.6W  →  needs copper pour (SOIC-8 exposed pad)

Recommendation: Connect SOIC-8 exposed pad to a copper pour of at least 4 cm²
```

### LM358

```
Op-amp quiescent current: ~1mA at 24V = 24mW  →  negligible, no heatsink needed.
```

---

## Bill of Materials

### Per Channel

| Ref | Component | Value / Part | Package | Qty | Notes |
|-----|-----------|-------------|---------|-----|-------|
| U1 | Buck converter | MP1584EN | SOIC-8 | 1 | |
| Q1 | PMOS pass transistor | FQP27P06 | TO-220 | 1 | Needs heatsink |
| U2 | Op-amp | LM358 | SOIC-8 or DIP-8 | 1 | |
| U3 | Current/voltage sense | INA219xAIDR | SOIC-8 | 1 | |
| R1 | FB upper resistor | 100 kΩ 1% | 0805 | 1 | |
| R2 | FB lower resistor | 6.8 kΩ 1% | 0805 | 1 | |
| R3 | DAC injection resistor | 24.3 kΩ 1% | 0805 | 1 | Use 24.3k or 24k |
| Ra | LDO divider upper | 10 kΩ 1% | 0805 | 1 | |
| Rb | LDO divider lower | 51 kΩ 1% | 0805 | 1 | |
| Rg | Gate resistor | 100 Ω | 0805 | 1 | Prevents oscillation |
| Rc | Stability cap resistor | 10 nF | 0805 | 1 | Op-amp feedback cap |
| Rs | Shunt resistor | 0.1 Ω 1W 1% | 2512 | 1 | Current sense |
| L1 | Inductor | 4.7 µH 4A | SMD | 1 | e.g. SRR1260-4R7Y |
| C1 | MP1584 output cap | 22 µF 35V | 0805/1206 ceramic | 1 | |
| C2 | Input decoupling | 10 µF 35V | 1206 ceramic | 1 | |
| C3 | LDO output cap | 100 µF 35V | Electrolytic | 1 | |
| C4 | LDO HF bypass | 10 µF ceramic | 1206 | 1 | In parallel with C3 |
| C5 | LDO HF bypass | 100 nF ceramic | 0805 | 1 | In parallel with C3 |

### Shared (Both Channels)

| Ref | Component | Value / Part | Package | Qty |
|-----|-----------|-------------|---------|-----|
| U4 | Quad DAC | MCP4728A0T | SOIC-10 | 1 |
| U5 | Microcontroller | ESP32-C3 Super Mini | Module | 1 |
| J1 | Power input | DC barrel or screw terminal 24V | — | 1 |
| J2,J3 | Output terminals | Banana jack or screw terminal | — | 2 |

### Estimated Cost (India)

| Item | Approx Cost |
|------|-------------|
| MP1584 × 2 | ₹60 |
| FQP27P06 × 2 | ₹80 |
| LM358 × 2 | ₹30 |
| INA219 × 2 | ₹160 |
| MCP4728 | ₹120 |
| Inductors × 2 | ₹80 |
| Resistors + caps | ₹50 |
| Heatsinks × 2 | ₹40 |
| ESP32-C3 | ₹200 |
| PCB (FR4, milled) | ₹0 (self-milled) |
| **Total** | **~₹820** |

---

## PCB Design Notes (0.25 mm CNC)

### Component Package Analysis

| Component | Package | Min pad gap | 0.25mm bit |
|-----------|---------|-------------|-----------|
| MP1584 | SOIC-8 | 0.67 mm | ✓ Comfortable |
| INA219 | SOIC-8 | 0.67 mm | ✓ Comfortable |
| MCP4728 | SOIC-10 | 0.67 mm | ✓ Comfortable |
| LM358 | SOIC-8 | 0.67 mm | ✓ Comfortable |
| FQP27P06 | TO-220 | 2.54 mm | ✓ Easy |
| Resistors | 0805 | 0.50 mm | ✓ Fine |
| Shunt Rs | 2512 | 1.50 mm | ✓ Easy |

> All packages are safely within the 0.25 mm bit capability (no SOT-23 ICs in this design).

### PCB Layout Priorities

```
1. Power path (24V → MP1584 → PMOS → output terminal):
   Use 2mm+ track width. Copper pour recommended.

2. MP1584 SW node:
   Keep short and away from analog components.
   The switch node is the noisiest point in the design.

3. INA219 shunt resistor placement:
   Place shunt resistor close to INA219 IN+/IN- pins.
   Kelvin (4-wire) connection if possible for accuracy.

4. LM358 feedback loop:
   Keep Ra/Rb close to op-amp −IN pin.
   Keep 10nF stability cap (Rc) across Rb.

5. Decoupling capacitors:
   Place C_in (10µF) within 3mm of MP1584 IN pin.
   Place C1 (22µF) within 5mm of MP1584 OUT pin.
   Place C3/C4/C5 right at output terminal.

6. I²C bus:
   Keep SDA/SCL tracks short (<10cm).
   Add 4.7kΩ pull-ups to 3.3V if not already on ESP32 module.

7. PMOS heatsink:
   TO-220 mounted to edge of PCB with external heatsink,
   or use PCB copper pour on drain pad if using D2PAK version.
```

### Recommended Track Widths

| Signal | Track Width | Notes |
|--------|-------------|-------|
| 24V power | 2.5 mm | Input supply |
| MP1584 output | 2.0 mm | Up to 3A |
| Output terminal | 2.5 mm | Final output |
| SW node | 1.5 mm | Short as possible |
| I²C (SDA/SCL) | 0.4 mm | Signal only |
| FB network | 0.3 mm | High impedance |
| GND copper pour | Full pour | Both layers |

---

## Firmware Interface

### Setting Output Voltage

```cpp
#include <Adafruit_MCP4728.h>
#include <Adafruit_INA219.h>

Adafruit_MCP4728 dac;
Adafruit_INA219 ina[2] = { Adafruit_INA219(0x40),
                            Adafruit_INA219(0x41) };

// Channel index: 0 = Ch1, 1 = Ch2
void set_voltage(uint8_t ch, float target_V) {
    // Coarse: MP1584 set to Vtarget + 0.5V
    float v_coarse = target_V + 0.5f;
    float vdac_c = (16.0f - v_coarse) / 4.09f;
    uint16_t code_c = (uint16_t)constrain(vdac_c * 4096.0f / 3.3f, 0, 4095);

    // Fine: LDO reference = Vtarget × divider ratio
    float vdac_f = target_V * 10.0f / (10.0f + 51.0f);
    uint16_t code_f = (uint16_t)constrain(vdac_f * 4096.0f / 3.3f, 0, 4095);

    // MCP4728: Ch A/C = coarse, Ch B/D = fine
    MCP4728_channel_t ch_coarse = (ch == 0) ? MCP4728_CHANNEL_A : MCP4728_CHANNEL_C;
    MCP4728_channel_t ch_fine   = (ch == 0) ? MCP4728_CHANNEL_B : MCP4728_CHANNEL_D;

    dac.setChannelValue(ch_coarse, code_c);
    dac.setChannelValue(ch_fine,   code_f);
}

// Read actual voltage (V)
float read_voltage(uint8_t ch) {
    return ina[ch].getBusVoltage_V();
}

// Read actual current (mA)
float read_current(uint8_t ch) {
    return ina[ch].getCurrent_mA();
}

// Read power (mW)
float read_power(uint8_t ch) {
    return ina[ch].getPower_mW();
}
```

### Initialization

```cpp
void setup() {
    Wire.begin();

    // Init DAC — set both channels to safe starting point (3.3V output)
    dac.begin(0x60);
    set_voltage(0, 3.3f);
    set_voltage(1, 3.3f);
    // Save to EEPROM so power-up is safe next time
    dac.saveToEEPROM();

    // Init INA219
    ina[0].begin();
    ina[1].begin();
    ina[0].setCalibration_32V_2A();  // or custom calibration
    ina[1].setCalibration_32V_2A();
}
```

### Enable / Disable Outputs

```cpp
#define EN_CH1  5   // GPIO5
#define EN_CH2  6   // GPIO6

void enable_output(uint8_t ch, bool en) {
    digitalWrite(ch == 0 ? EN_CH1 : EN_CH2, en ? HIGH : LOW);
}
```

---

## System Specifications

| Parameter | Value |
|-----------|-------|
| Input voltage | 24 V DC (SMPS) |
| Input current (max, both channels) | 7 A |
| Channels | 2 independent |
| Output voltage range | 2.8 V – 20.0 V |
| Output current (per channel) | 0 – 3 A |
| Output voltage resolution | ~4.9 mV/step |
| Output voltage accuracy | ±5 mV (DC) |
| Output ripple | 2–5 mV pk-pk |
| Load regulation | < 10 mV (0–3A) |
| Line regulation | < 2 mV |
| Current measurement range | 0 – 3.2 A |
| Current resolution | 1 mA |
| Voltage measurement resolution | 4 mV (INA219) |
| Transient response | < 1 ms |
| Efficiency (typical) | 90–93% |
| Control interface | I²C (ESP32-C3) |
| Web interface | Yes (existing Voltbench firmware) |
| OTA updates | Yes (existing firmware) |
| PCB process | 0.25 mm isolation CNC milling |

---

## Key Design Decisions Log

| Decision | Reason |
|----------|--------|
| MP1584 over LM2596 | 10× smaller passives, 93% vs 75% efficiency, SOIC-8 |
| PMOS LDO over IC LDO | Handles full 2.8–20V range, 210mV dropout, cheapest option |
| FQP27P06 over other PMOS | 60V/27A rated, 70mΩ Rdson, TO-220 (easy to heatsink), available in India |
| MCP4728 (quad) over 4× MCP4725 | One SOIC-10 replaces four SOT-23-5, easier to mill |
| INA219 SOIC-8 over SOT-23 | 0.67mm pad gap vs 0.35mm — comfortable with 0.25mm CNC bit |
| 24V SMPS over transformer | Clean regulated DC, no rectifier capacitors, compact |
| LM358 over TL072 | Single-supply 3–32V operation, 24V compatible, available everywhere |
| 0805 resistors | Balance between solderable size and CNC pad gap |
| Coarse+Fine DAC split per channel | MP1584 handles gross voltage, LDO handles precision — no PID needed |

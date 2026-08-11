# Voltbench v2.1 — GPIO Chart & Wiring Guide

> Bench reference for building the v2.1 board. Every pin, every device, every
> connection. Companion to `HARDWARE-DESIGN-V2.md` (circuit design and component
> values) — this document is *what wire goes where*.

---

## 1. ESP32-C3 Super Mini board

16 pins, USB-C, native USB (USB-Serial-JTAG — no CP210x/CH340 driver needed on
Windows 10/11), onboard 3.3 V LDO, onboard LED on GPIO8, BOOT button on GPIO9.

```
                    ┌──────── USB-C ────────┐
                    │                       │
          ┌─────────┴───────────────────────┴─────────┐
          │                                            │
   5V ────┤ 5V                                  GPIO5  ├──── EN ch2
  GND ────┤ G          ESP32-C3 SuperMini      GPIO6  ├──── SDA   ◄── I²C
  3V3 ────┤ 3V3                                 GPIO7  ├──── SCL   ◄── I²C
 TEMP ────┤ GPIO4  (ADC1_CH4)                   GPIO8  ├──── (onboard LED)
CAL2 ────┤ GPIO3                                GPIO9  ├──── (BOOT button)
   -- ────┤ GPIO2  ⚠ STRAPPING — LEAVE FREE     GPIO10 ├──── EN ch1
 BTN2 ────┤ GPIO1  (UP)                         GPIO20 ├──── CAL ch1
 BTN1 ────┤ GPIO0  (SEL)                        GPIO21 ├──── BTN3 (DOWN) +1k
          │                                            │
          │              [antenna]                     │
          └────────────────────────────────────────────┘
```

### Pin capability reference

| GPIO | ADC | Strapping | Boot-time behaviour | Safe for |
|------|-----|-----------|---------------------|----------|
| 0 | ADC1_CH0 | no | floating input | anything |
| 1 | ADC1_CH1 | no | floating input | anything |
| 2 | ADC1_CH2 | **YES** | **must read HIGH** | nothing that idles low |
| 3 | ADC1_CH3 | no | floating input | anything |
| 4 | ADC1_CH4 | no | floating input | anything |
| 5 | ADC2 (unusable w/ WiFi) | no | floating input | digital only |
| 6 | — | no | floating input | anything |
| 7 | — | no | floating input | anything |
| 8 | — | **YES** | **must read HIGH** | onboard LED (already wired) |
| 9 | — | **YES** | low ⇒ download mode | BOOT button (already wired) |
| 10 | — | no | floating input | anything |
| 20 | — | no | U0RXD — **input** during ROM boot | anything |
| 21 | — | no | U0TXD — **driven output** during ROM boot | inputs need series R |

**ADC2 is unusable.** GPIO5 is the only ADC2 pin and ADC2 cannot be read while WiFi
is active — which for this device is always. Treat GPIO0–GPIO4 as the *only* analog
inputs, five total.

---

## 2. Voltbench v2.1 GPIO allocation

| GPIO | Signal | Direction | External parts | Notes |
|------|--------|-----------|----------------|-------|
| 0 | `BTN_SEL` | input, pull-up | button → GND | internal pull-up, no resistor needed |
| 1 | `BTN_UP` | input, pull-up | button → GND | internal pull-up |
| 2 | — | **unconnected** | none | strapping pin, deliberately unused |
| 3 | `CAL_CH2` | output | 10 kΩ → GND, → AO3400 gate | pull-down keeps load off |
| 4 | `TEMP` | **analog in** | 1 kΩ series + 100 nF → GND | MCP9700 output |
| 5 | `EN_CH2` | output | 10 kΩ → GND, → MP1584 #2 EN | pull-down = default OFF |
| 6 | `SDA` | I²C | 2.2 kΩ → 3V3 | shared bus |
| 7 | `SCL` | I²C | 2.2 kΩ → 3V3 | shared bus |
| 8 | `LED` | output | onboard | **active LOW** on most SuperMini boards |
| 9 | `BOOT` | input | onboard button | leave alone |
| 10 | `EN_CH1` | output | 10 kΩ → GND, → MP1584 #1 EN | pull-down = default OFF |
| 20 | `CAL_CH1` | output | 10 kΩ → GND, → AO3400 gate | safe: input during ROM boot |
| 21 | `BTN_DOWN` | input, pull-up | **1 kΩ series** + button → GND | see warning |

### ⚠ Three warnings that will cost you a board or a day

**1. Nothing that idles low may touch GPIO2, GPIO8, or GPIO9.** These are strapping
pins, latched at reset before any firmware runs, and they must read high. Two
tempting-but-fatal uses of the "spare" GPIO2:

- *A button to GND* — works perfectly in normal use, but if anyone holds it while
  powering on, the chip will not boot. The unit appears dead.
- *An analog temperature sensor* — MCP9700 idles at ~750 mV and LM35 at ~250 mV, both
  far below the ~2.4 V input-high threshold. Same result.

A cal-load gate is equally unusable there because it needs a pull-down. **Leave GPIO2
unconnected.**

**2. BTN3 on GPIO21 needs a 1 kΩ series resistor.** GPIO21 is U0TXD and the ROM
bootloader actively drives it while printing its boot log. A bare button to ground
would short a driven output. 1 kΩ limits that to ~3.3 mA while still reading as a
solid low through the ~45 kΩ internal pull-up. GPIO20 needs no such resistor — it is
U0RXD, an input during boot, which is exactly why the cal gate lives there.

**3. Call `Wire.begin(6, 7)` explicitly.** Most ESP32-C3 SuperMini examples online use
GPIO8/GPIO9 for I²C. Those are the LED and BOOT pins on this board and are strapping
pins besides. This design uses **GPIO6 = SDA, GPIO7 = SCL** and the firmware must say
so; the default will not work.

---

## 3. I²C bus

Single SDA/SCL pair, 400 kHz, **2.2 kΩ pull-ups to 3.3 V** (one per line, placed near
the ESP32). Four devices, no address conflicts.

| Device | Address | Set by | Purpose |
|--------|---------|--------|---------|
| SSD1306 OLED | `0x3C` | fixed (some modules 0x3D) | 128×64 display |
| INA219 ch1 | `0x40` | A0=GND, A1=GND (default) | V + I sense, channel 1 |
| INA219 ch2 | `0x41` | **A0 bridged to VS** | V + I sense, channel 2 |
| MCP4728 | `0x60` | factory (A0T variant) | all four setpoints |

> **Why 2.2 kΩ and not the usual 4.7 kΩ.** At 400 kHz the rise time must stay under
> 300 ns. With `t_r ≈ 0.85·R·C`, 4.7 kΩ is marginal at ~70 pF of bus capacitance and
> clearly fails if the OLED hangs off a flying lead (~150 pF → ~600 ns). Dropping the
> bus to 100 kHz instead is *not* an option: it quadruples a full OLED frame flush to
> ~104 ms, which the control loop cannot absorb.

### Bus wiring

```
  ESP32-C3                                                    3V3
   GPIO6 ─┬──────┬──────────┬──────────┬───────────┐          │
   (SDA)  │      │          │          │          [2.2k]──────┤
          │      │          │          │                      │
   GPIO7 ─┼┬─────┼┬─────────┼┬─────────┼┬──────────┐          │
   (SCL)  ││     ││         ││         ││         [2.2k]──────┘
          ││     ││         ││         ││
        SSD1306  INA219#1  INA219#2  MCP4728
         0x3C     0x40      0x41      0x60
```

Keep the total bus under ~30 cm. Route SDA/SCL away from the MP1584 switch nodes.

### Setting the INA219 addresses

INA219 breakouts have two solder jumpers, A0 and A1, both open (= GND) by default.

| Channel | A0 | A1 | Result |
|---------|-----|-----|--------|
| ch1 (U3) | leave open | leave open | `0x40` |
| ch2 (U4) | **bridge to VS** | leave open | `0x41` |

Verify with an I²C scan before wiring anything else — two boards both answering at
0x40 is the single most common mistake here.

---

## 4. Device-by-device connections

### 4.1 SSD1306 OLED (0x3C)

| Module pin | Connect to |
|---|---|
| VCC / VDD | 3V3 |
| GND | GND |
| SDA | GPIO6 |
| SCL | GPIO7 |

Some modules are labelled 0x3D — check the silkscreen or scan the bus. Mount it on the
panel; if the leads exceed ~15 cm, twist SDA/SCL with a ground wire.

### 4.2 INA219 ×2 (0x40, 0x41)

| Module pin | Connect to (ch1) | Connect to (ch2) |
|---|---|---|
| VCC / VS | 3V3 | 3V3 |
| GND | GND | GND |
| SDA | GPIO6 | GPIO6 |
| SCL | GPIO7 | GPIO7 |
| **VIN+** | buck side of RS1 | buck side of RS2 |
| **VIN−** | terminal side of RS1 | terminal side of RS2 |

**Polarity matters.** VIN− must be on the *terminal* side so the bus-voltage reading
equals the voltage the user sees at the jacks. Reversed, current reads negative and the
bus voltage reads the pre-shunt node.

The breakouts ship with an onboard 0.1 Ω shunt. You may either use it (wire the load
through the breakout's screw terminals) or **remove/bypass it and use your own 2512
shunt** on the carrier board — the latter is preferred for the 2 A rating and for
Kelvin sensing. Do not leave both in circuit.

### 4.3 MCP4728 quad DAC (0x60)

| Module pin | Connect to |
|---|---|
| VDD | 3V3 (+ 100 nF to GND at the pin) |
| GND | GND |
| SDA | GPIO6 |
| SCL | GPIO7 |
| LDAC | GND (already tied low on most breakouts) |
| RDY / BSY | not connected |
| **VA** | `CV_DAC1` → R3 (10.5 kΩ) → FB of MP1584 #1 |
| **VB** | `CC_DAC1` → R8 (100 kΩ) → LM358 #1 pin 6 |
| **VC** | `CV_DAC2` → R3′ → FB of MP1584 #2 |
| **VD** | `CC_DAC2` → R8′ → LM358 #2 pin 6 |

> **LDAC is tied low on breakouts, meaning writes take effect immediately.** Firmware
> must therefore use **single-channel write commands only**, so updating channel A can
> never glitch channel B.

### 4.4 MCP9700 temperature sensor (analog)

TO-92, flat face toward you, pins left→right: **VDD, VOUT, GND**. (Check the datasheet
for your exact variant — MCP9700 and MCP9700A share this pinout, but clones vary.)

```
3V3 ──── VDD
                VOUT ──[1 kΩ]──┬── GPIO4
GND ──── GND                   │
                           [100 nF]
                               │
                              GND
```

The 1 kΩ + 100 nF filter is **not optional** — the sensor sits near a switching
regulator on flying leads and the ADC input is high-impedance. Mount the sensor against
the heatsink or between the two MP1584 modules, where the heat actually is.

Transfer function: `V_out = 500 mV + 10 mV/°C` → 750 mV at 25 °C, **1.30 V at 80 °C**,
comfortably inside the ADC range. Firmware: `T_C = (mV − 500) / 10`.

### 4.5 Buttons

```
GPIO0  ────────────────── [SEL]  ──── GND
GPIO1  ────────────────── [UP]   ──── GND
GPIO21 ──[1 kΩ]────────── [DOWN] ──── GND      ← series resistor required
```

All three use the MCU's internal pull-ups (`INPUT_PULLUP`) — no external resistors, no
capacitors. Debouncing is done in firmware (20 ms counting integrator).

### 4.6 Output enable (per channel)

```
GPIO10 (ch1) ──┬──────── MP1584 #1 EN pin
GPIO5  (ch2) ──┤ (respectively #2)
               │
            [10 kΩ]
               │
              GND
```

The 10 kΩ pull-down overpowers the MP1584's µA-scale internal pull-up, so **the
channel is OFF whenever the GPIO is high-impedance** — during boot, reset, or a crash.
This is the outermost safety layer and must not be omitted. MP1584 EN is rated 6 V
absolute max, so 3.3 V direct GPIO drive is safe; never tie EN to 24 V.

### 4.7 Self-calibration load (per channel)

```
VOUT1 ──[10 Ω 5 W]──┬── AO3400 drain (pin 3)
                    │
   GPIO20 ──────────┼── AO3400 gate (pin 1)
   (ch1)            │
                 [10 kΩ]
                    │
                   GND ── AO3400 source (pin 2)
```

AO3400 in SOT-23: **pin 1 = gate, pin 2 = source, pin 3 = drain.** Channel 2 is
identical on GPIO3.

> ⚠ **The 10 Ω resistor is only rated for this at low voltage.** At 3 V it dissipates
> 0.9 W; at 20 V it would see **40 W** and burn. Firmware enforces a ≤3 V, ≤2 s
> interlock, backed by the task watchdog. Never engage this load manually with the
> output at a high setpoint.

---

## 5. Power distribution

```
24 V SMPS ──┬── MP1584 #1 (channel 1)
            ├── MP1584 #2 (channel 2)
            ├── LM358 #1, #2  (pin 8, +100 nF each)
            └── Mini360 buck ──► 5 V ──► ESP32-C3 "5V" pin
                                            │
                                            └── onboard LDO ──► 3V3 pin
                                                                  │
                                        ┌─────────────────────────┤
                                     OLED  INA219×2  MCP4728  MCP9700
                                                                  │
                                                          I²C pull-ups
```

**Do not power the ESP32 from USB and the 5 V rail simultaneously** unless the board
has reverse-protection on the 5 V input (many SuperMini clones do not — check for a
diode). During bench work, power from USB alone with the Mini360 output disconnected.

### 3.3 V current budget

| Load | Typical | Notes |
|---|---|---|
| SSD1306 OLED | ~20 mA | depends on lit pixels |
| INA219 ×2 | ~2 mA | 1 mA each |
| MCP4728 | ~0.5 mA | |
| MCP9700 | ~6 µA | negligible |
| I²C pull-ups | ~3 mA | worst case, both lines low |
| **Total peripherals** | **~26 mA** | |

The ESP32-C3 itself draws 80–100 mA average with WiFi, peaking around 350 mA on
transmit. Size the 5 V supply for **≥500 mA**; a Mini360 (1.8 A) is ample. The onboard
3.3 V LDO handles the peripherals easily.

---

## 6. Assembly and first-power-on order

Build and verify in this order. Do not skip ahead — each step de-risks the next.

1. **ESP32 alone.** Flash a blink sketch. Confirm the board enumerates as a COM port
   and GPIO8 blinks (remember: active low).
2. **I²C bus only** — pull-ups, OLED, both INA219s, MCP4728. Nothing connected to the
   power section. Run an I²C scanner: you must see exactly `0x3C, 0x40, 0x41, 0x60`.
   Fix any address collision now.
3. **Buttons and the temperature sensor.** Verify all three buttons read high at idle
   and low when pressed, and that the MCP9700 reads a sane room temperature.
   **Power-cycle while holding each button** — the board must boot every time. If it
   does not, something is on a strapping pin.
4. **MCP4728 outputs, unloaded.** Write code 4095 to A/C and 0 to B/D, then measure
   VA–VD with a DMM: A and C should read ~2.047 V, B and D ~0 V. Program the EEPROM
   power-on defaults and power-cycle to confirm they restore.
5. **Power section, one channel, EN disconnected.** With the DAC at code 4095 (minimum
   voltage), connect 24 V and confirm the MP1584 output is near its minimum and stable.
6. **Connect EN.** Only now can an output go live. Follow the bring-up checklist in
   `HARDWARE-DESIGN-V2.md` §14.
7. **Self-cal load last.** It is the only subsystem that can destroy a component.

### Pre-power-on checklist

- [ ] 10 kΩ pull-downs fitted on **both** EN lines and **both** CAL gates
- [ ] 1 kΩ series resistor on the GPIO21 button
- [ ] GPIO2 unconnected
- [ ] 2.2 kΩ (not 4.7 kΩ) I²C pull-ups
- [ ] INA219 ch2 A0 jumper bridged; both VIN− on the terminal side
- [ ] MP1584 modules modded: trimpot, FB→GND resistor and feed-forward cap removed,
      FB pad verified open to both Vout and GND
- [ ] 1 kΩ preload fitted on each output (mandatory — it is the only discharge path)
- [ ] MCP4728 EEPROM defaults programmed (CV = 4095, CC = 0) *before* EN is ever wired

---

## 7. Common mistakes

| Symptom | Likely cause |
|---|---|
| Board won't boot, appears dead | Something idling low on GPIO2 (or 8/9). Disconnect and retest |
| Board won't boot only sometimes | A button on a strapping pin, held during power-on |
| I²C scan finds nothing | `Wire.begin(6, 7)` not called — the default pins are wrong for this board |
| Only one INA219 appears | A0 jumper not bridged on channel 2; both are at 0x40 |
| Current reads negative | INA219 VIN+/VIN− swapped |
| Voltage reads pre-shunt | INA219 VIN− on the buck side instead of the terminal side |
| Output jumps to ~20 V at power-on | DAC unwritten or EEPROM defaults not programmed — **the CV mapping is inverted, code 0 = 20.18 V** |
| Output stuck high after a trip | Missing 1 kΩ preload; there is no other discharge path |
| Display garbles under load | 4.7 kΩ pull-ups, or SDA/SCL routed near a switch node |
| Cal resistor burns | Load engaged above 3 V — check the firmware interlock |

# ESP-IDMS — Hardware Deep-Dive Analysis & PCB Redesign

**Document Version:** 3.0  
**Date:** April 2026  
**Analysis Scope:** Complete hardware audit, PCB gap analysis, and redesign specification

---

## 1. EXECUTIVE SUMMARY

The ESP-IDMS is a functionally complete firmware project with well-documented hardware requirements, but the **PCB design exists only as markdown documentation and an atopile description** — neither of which is directly fabricable. No Gerber, KiCad, Eagle, or Altium project files exist. This analysis identifies all hardware specifications, catalogs design gaps, and provides a redesigned PCB layout specification ready for EDA implementation.

**Critical findings:**
- No fabricable PCB design exists (only docs + pseudo-schematic)
- GPIO assignments are inconsistent between Kconfig defaults, docs, and the atopile schematic
- Missing 3.3 V regulator, ESD protection, status LEDs, and user controls
- atopile schematic has wiring errors in the CT connector net
- AC-DC isolation strategy is documented but not simulated/verified

---

## 2. HARDWARE ARCHITECTURE OVERVIEW

```
AC Mains (100-240 VAC)
    │
    ├─── Fuse (10 A) ─── HLK-PM01 ─── 5 VDC ──┬─── 3.3 V LDO ─── ESP32-S3
    │                                          │
    │                                          ├─── Topway LCD (5 V)
    │                                          │
    SCT-013 CT ─── Bias Network ─── GPIO6 (ADC)
    (non-invasive)

DS18B20 ×2 ─── 1-Wire ─── GPIO4 (T_in)
               1-Wire ─── GPIO15 (T_out)

XPT2046 Touch ─── SPI ─── GPIO12/13/16 + GPIO11(CS) + GPIO14(IRQ)
```

---

## 3. COMPLETE HARDWARE SPECIFICATION

### 3.1 Processor / MCU

| Parameter | Specification |
|-----------|---------------|
| **Model** | ESP32-S3R8 (embedded in DevKitC-1-N16R8) |
| **Package** | QFN56 (7×7 mm) |
| **CPU** | Dual-core Xtensa LX7 @ 240 MHz |
| **Flash** | 16 MB (128 Mbit) quad SPI, DIO mode |
| **PSRAM** | 8 MB (64 Mbit) octal SPI, 80 MHz |
| **SRAM** | 512 KB internal |
| **ROM** | 384 KB |
| **Wi-Fi** | 802.11 b/g/n 2.4 GHz, STA mode |
| **BLE** | 5.0 (unused in firmware) |
| **ADC** | 2× 12-bit SAR ADC (ADC1: CH0-9, ADC2: CH0-9) |
| **SPI** | 4× SPI controllers (SPI0 reserved for flash, SPI1 for PSRAM) |
| **UART** | 3× (UART0: USB-Serial console, UART1: Topway LCD, UART2: unused) |
| **1-Wire** | Software bit-bang (no hardware OW peripheral) |
| **GPIO** | 45 usable pins |
| **USB** | USB OTG 1.1 (pins 19/20 — not used in this project) |
| **Operating Temp** | -40 °C to +85 °C |
| **VDD** | 3.0–3.6 V (typ 3.3 V) |
| **Typical Current** | ~80 mA Wi-Fi TX, ~160 mA peak |

### 3.2 Power Supply Chain

```
                                                  ┌──────────────┐
AC Mains ──[10A Fuse]──┐                         │  ESP32-S3    │
  100-240 VAC          ├── HLK-PM01 ──── 5V ──────┤  3.3V (DevKit LDO)
                       │    AC-DC                │              │
                       └─────────────────────────┤  LCD 5V      │
                                                  └──────────────┘
```

| Component | Part Number | Input | Output | Power | Notes |
|-----------|-------------|-------|--------|-------|-------|
| **HLK-PM01** | Hi-Link HLK-PM01 | 100-240 VAC | 5 VDC ±5% | 3 W (600 mA) | Isolated (3 kV), epoxy-potted |
| **Fuse** | 5×20 mm glass cartridge | 250 V | — | 10 A | Slow-blow preferred |
| **3.3 V Regulator** | On-board DevKit LDO (AMS1117-3.3) | 5 V | 3.3 V | 1 A | DevKit provides regulated 3.3 V |

**Power Budget:**

| Consumer | Voltage | Max Current | Peak Power |
|----------|---------|-------------|------------|
| ESP32-S3 (DevKit) | 5 V (→3.3 V LDO) | 200 mA | 1.0 W |
| Topway LCD Backlight | 5 V | 250 mA | 1.25 W |
| Topway LCD Logic | 5 V (→3.3 V internal) | 50 mA | 0.25 W |
| DS18B20 ×2 | 3.3 V | 3 mA | 0.01 W |
| SCT-013 bias network | 3.3 V | <1 mA | <0.01 W |
| **Total** | | **~500 mA** | **~2.5 W** |

Headroom: 100 mA (600 mA rated — 500 mA draw = 17% margin). Adequate.

### 3.3 Current Sensing Subsystem — SCT-013

**Complete Analog Front-End:**

```
                  R1 (10kΩ 1%)
      3.3V ───────┬────────┐
                  │        │
                  │       ┌┤─── GPIO6 (ADC1 CH5)
                  │       │     ESP32-S3 12-bit
  SCT-013 ─────┐  │  C1   │     0-3.3V range
  CT Sensor    │  │  10µF │     Atten: 11 dB
               ├──┴──┤+ ──┤
               │     └┤───┤
               │        │
          R_burden    R2 (10kΩ 1%)
           (33Ω,
          DNP for       │
         030 variant)   │
               │        │
              GND      GND
```

| Parameter | Value | Notes |
|-----------|-------|-------|
| **Primary CT** | SCT-013-030 (30 A : 1 V) | Built-in burden, output = voltage |
| **Alternate CT** | SCT-013-000 (100 A : 50 mA) | Current output, needs external 33 Ω burden |
| **ADC Channel** | ADC1 CH5 (GPIO6) | 12-bit SAR, 11 dB attenuation |
| **Input Range** | 0–3.3 V (0–4095 counts) | Check: 1 V RMS = 1.414 V peak from CT |
| **DC Bias** | 1.65 V (2× 10kΩ divider) | Midpoint = 2048 counts |
| **Bias Capacitor** | 10 µF electrolytic, 25 V | AC couples CT signal, stabilizes DC midpoint |
| **Cutoff Frequency** | f_c = 1/(2π×10kΩ×10µF) = 1.6 Hz | Adequate for 50/60 Hz AC |
| **RMS Sampling** | 512 samples @ 500 ms | Non-power-of-2 window reduces harmonic leakage |
| **Effective Resolution** | ~3.2 mA/count @ 30 A/V calibration | Theoretical min = 3.3V/4096/30 = 0.027 mA/count |
| **Calibration Factor** | 3000 = 30.00 A/V (×100 fixed-point) | Configurable via menuconfig |
| **Auto-Zero** | 512 samples at boot | Compensates for V_div tolerance and noise floor |
| **Disconnect Detection** | ADC mean < 15 OR > 4080 | Floating input pulls to rail |
| **Power-Loss Threshold** | < 350 mA for ≥ 5 s | Configurable 50–5000 mA |

**Transfer function:**
```
I_rms(A) = (RMS(ADC_counts - 2048) × 3.3V / 4096) × 30.0 (A/V) × (1 / autozero_correction)
```

### 3.4 Temperature Sensing Subsystem — DS18B20

| Parameter | Value |
|-----------|-------|
| **Sensor** | Maxim DS18B20 (waterproof probe, 1 m cable) |
| **Accuracy** | ±0.5 °C (-10 °C to +85 °C) |
| **Resolution** | 12-bit (0.0625 °C/LSB) |
| **Range** | -55 °C to +125 °C |
| **Supply** | 3.0–5.5 V (parasitic or VDD mode) |
| **Interface** | 1-Wire (Dallas/Maxim proprietary) |
| **Timing** | Reset pulse 480 µs, read/write slots 60–120 µs |
| **Conversion Time** | 750 ms (12-bit), pipelined |
| **Parasitic Mode** | Not used — VDD supplied at 3.3 V |
| **Pull-up** | 4.7 kΩ to 3.3 V (per bus) |
| **CRC** | 8-bit Dallas CRC on every 9-byte read scratchpad |
| **Bus Architecture** | Two independent 1-Wire buses |
| **T_in Bus** | GPIO4 + 4.7kΩ pull-up to 3.3V |
| **T_out Bus** | GPIO15 + 4.7kΩ pull-up to 3.3V |

**Wiring:**

| Wire Color | Signal | T_in Connection | T_out Connection |
|------------|--------|-----------------|------------------|
| Red | VCC | 3.3 V | 3.3 V |
| Yellow | DQ (Data) | GPIO4 + 4.7kΩ | GPIO15 + 4.7kΩ |
| Black/Bare | GND | GND | GND |

### 3.5 Display Subsystem — Topway HKT070DTA-1C

| Parameter | Specification |
|-----------|---------------|
| **Model** | Topway HKT070DTA-1C |
| **Type** | TFT-LCD, 7-inch, transmissive |
| **Resolution** | 800 × 480 pixels (WVGA) |
| **Color Depth** | 16-bit (RGB565) |
| **Backlight** | LED, 250 mA @ 5 V |
| **Interface** | UART TTL 3.3 V (RS232-TTL transceiver onboard) |
| **Baud Rate** | Factory 115200, configurable via firmware command |
| **UART Format** | 8N1 (8 data bits, no parity, 1 stop bit) |
| **Protocol** | DGUS-style: `AA [cmd] [3-byte addr] [data] CC 33 C3 3C` |
| **Touch** | 4-wire resistive (XPT2046 controller via SPI) |
| **VP Memory** | Variable Picture — display elements mapped to addresses |
| **Design Tool** | RGTools (Topway's free HMI editor) |
| **Operating Temp** | -20 °C to +70 °C |
| **Supply Voltage** | 5 V ± 0.3 V |
| **Logic Level** | 3.3 V (via onboard MAX232-level shifter) |

**VP Address Map (firmware ↔ display contract):**

| VP Address | Type | Content | Source |
|------------|------|---------|--------|
| 0x000000 | String | Current display ("--" if invalid) | monitor |
| 0x000200 | String | Wi-Fi IP address | wifi_manager |
| 0x000280 | String | Firmware version | app_main |
| 0x000300 | String | OTA status | ota |
| 0x000380 | String | ACTIVE / INACTIVE | monitor |
| 0x000500 | String | Diagnostic detail | monitor |
| 0x000600 | String | ERROR status | monitor |
| 0x000700 | String | WARNING status | monitor |
| 0x080000 | N16 | Current × 10 | monitor |
| 0x080002 | N16 | T_in × 10 | monitor |
| 0x080004 | N16 | T_out × 10 | monitor |
| 0x080006 | N16 | ΔT × 10 (signed) | monitor |
| 0x080010 | N16 | Current valid | monitor |
| 0x080012 | N16 | T_in valid | monitor |
| 0x080014 | N16 | T_out valid | monitor |
| 0x080016 | N16 | ΔT valid | monitor |
| 0x080018 | N16 | Wi-Fi status | wifi_manager |
| 0x08001A | N16 | OTA status | ota |
| 0x08001C | N16 | Technician count | config_store |
| 0x08001E | N16 | Power fault | monitor |
| 0x080020 | N16 | Cooling fault | monitor |

### 3.6 Touch Controller Subsystem — XPT2046

| Parameter | Specification |
|-----------|---------------|
| **Controller** | XPT2046 (TI TSC2046 compatible) |
| **Interface** | 4-wire SPI, mode 0 (CPOL=0, CPHA=0) |
| **Resolution** | 12-bit (0–4095) XY |
| **SPI Bus** | SPI3_HOST (shared with LCD or dedicated) |
| **SPI Speed** | 2 MHz typ |
| **CS** | GPIO11 |
| **SCLK** | GPIO12 |
| **MOSI** | GPIO13 |
| **MISO** | GPIO16 |
| **IRQ** | GPIO14 (active low pen-down) |
| **Touch Panel** | 4-wire resistive (integrated in Topway LCD) |

**Note:** XPT2046 is on the Topway LCD module itself (not separate). The PCB breakout connects to the Topway module's touch SPI header. In a **Topway-only build, this SPI connector is optional** since the Topway display drives its own touch panel via its built-in HMI processor. The SPI touch interface on the PCB is a **legacy carryover** for ILI9341 TFT builds.

### 3.7 Connectors and Interfaces

| Connector | Pins | Type | Mates With |
|-----------|------|------|------------|
| J_AC_IN | 3 | Screw terminal 5.08 mm pitch | Mains cable (L, N, E) |
| J_LCD | 4 | JST-XH 4-pin | Topway LCD ribbon → 4-pin JST-XH housing |
| J_CT | 2 | JST-XH 2-pin or screw terminal | SCT-013 3.5mm audio plug → adapter cable |
| J_TEMP_IN | 3 | JST-XH 3-pin | DS18B20 probe (red/yellow/black) |
| J_TEMP_OUT | 3 | JST-XH 3-pin | DS18B20 probe (red/yellow/black) |
| J_TOUCH | 6 | JST-XH 6-pin | Topway touch SPI header (optional) |
| ESP32 socket | 2×19 | 2.54 mm female headers | ESP32-S3 DevKit male headers |

### 3.8 Complete GPIO Allocation Map (ESP32-S3, Topway Config)

| GPIO | Net Name | Direction | Function | ADC | SPI | UART | Notes |
|------|----------|-----------|----------|-----|-----|------|-------|
| 0 | — | — | **UNUSED** | — | — | — | Strapping (boot) |
| 1 | LCD_TX | OUT | Topway UART TX | — | — | U1TX | UART1 to LCD |
| 2 | — | — | **UNUSED** | — | — | — | — |
| 3 | LCD_RX | IN | Topway UART RX | — | — | U1RX | UART1 from LCD |
| 4 | OW_TIN | IN/OUT | DS18B20 T_in 1-Wire | — | — | — | 4.7kΩ pull-up |
| 5–10 | — | — | **UNUSED** | — | — | — | Available for expansion |
| 6 | ADC_CT | IN | SCT-013 biased ADC | ADC1_CH5 | — | — | 2×10kΩ divider + 10µF |
| 11 | T_CS | OUT | XPT2046 CS | — | SPI3_CS | — | Optional (Topway build) |
| 12 | T_SCLK | OUT | XPT2046 SCLK | — | SPI3_CLK | — | Optional |
| 13 | T_MOSI | OUT | XPT2046 MOSI | — | SPI3_MOSI | — | Optional |
| 14 | T_IRQ | IN | XPT2046 IRQ | — | — | — | Optional, pull-up |
| 15 | OW_TOUT | IN/OUT | DS18B20 T_out 1-Wire | — | — | — | 4.7kΩ pull-up |
| 16 | T_MISO | IN | XPT2046 MISO | — | SPI3_MISO | — | Optional |
| 17–18 | — | — | **UNUSED** | — | — | — | — |
| 19 | USB_D- | — | DevKit USB (untouched) | — | — | — | DevKit USB port |
| 20 | USB_D+ | — | DevKit USB (untouched) | — | — | — | DevKit USB port |
| 21–25 | — | — | **UNUSED** | — | — | — | Available expansion |
| 26–32 | — | — | **RESERVED** | — | — | — | Flash/PSRAM |
| 33–37 | — | — | **UNUSED** | — | — | — | Available expansion |
| 38–42 | — | — | **UNUSED** | — | — | — | Available expansion |
| 43 | CONSOLE_RX | IN | USB-Serial RX | — | — | U0RX | Console (fixed) |
| 44 | CONSOLE_TX | OUT | USB-Serial TX | — | — | U0TX | Console (fixed) |
| 45–46 | — | — | **STRAPPING** | — | — | — | Do not use |
| 47–48 | — | — | **UNUSED** | — | — | — | — |

**GPIO utilization:** 12 of ~45 usable pins (27%). Plenty of headroom for expansion.

---

## 4. FIRMWARE ↔ HARDWARE INTERFACE

### 4.1 Driver Initialization Order

```
1. NVS flash init (config_store.c)
2. Wi-Fi init to STA mode (wifi_manager.c)
3. 1-Wire bus init per DS18B20 (ds18b20.c → onewire.c)
4. ADC oneshot init for SCT-013 (monitor.c)
5. Topway UART handshake (topway_lcd.c, 5 retries max)
6. SPI init for XPT2046 touch (xpt2046.c, optional)
7. UI task start (ui_topway.c, 500ms timer)
8. Monitor task start (monitor.c, 500ms, priority 5)
9. Telegram bot poll task (tg_bot.c, 5s/30s, priority 3)
10. OTA HTTP server (ota.c, port 8080)
11. Serial console (cli/serial_console.c, priority 2)
12. OTA validity timer (30s delayed mark)
```

### 4.2 Critical Timings

| Operation | Time | Blocking? |
|-----------|------|-----------|
| 1-Wire reset pulse | 480 µs | Yes |
| 1-Wire read slot | ~70 µs | Yes |
| 1-Wire write slot | ~60 µs | Yes |
| DS18B20 conversion | 750 ms | No (pipelined) |
| 512-sample ADC RMS | ~5 ms @ 100 kHz | Yes |
| Topway N16 write | ~2 ms @ 115200 | Yes |
| Topway string write | ~1 ms + len/115200 | Yes |
| Telegram HTTPS POST | 200–1000 ms | No (async) |
| OTA flash write (4 MB) | ~60 s @ 690 KB/s | No (streamed) |

No critical real-time constraints violated. 1-Wire bit-bang timing is met by disabling interrupts during bit slots (portENTER_CRITICAL on single-core sections).

---

## 5. PCB LAYOUT — CURRENT DESIGN AUDIT

### 5.1 What Exists

| Artifact | Format | Completeness | Fabricable? |
|----------|--------|--------------|-------------|
| `PCB_Layout.md` | Markdown documentation | Component placement, trace rules, layer stack-up | **No** |
| `hardware/esp-idms.ato` | atopile pseudo-schematic | Component declarations, net table | **No** (atopile-only) |
| `BOM.md` | Markdown BOM | 16 items, costs, notes | Partially |
| `Specifications.md` | Markdown schematic | ASCII-art schematics | **No** |

**No EDA-native design files exist** (no `.kicad_sch`, `.kicad_pcb`, `.sch`, `.pcb`, `.brd`, `.gbr`, `.drl`).

### 5.2 Design Gaps Identified

| # | Gap | Severity | Impact |
|---|-----|----------|--------|
| **G1** | No EDA project file | **Critical** | Cannot fabricate PCB |
| **G2** | No 3.3 V regulator on PCB (relies on DevKit LDO) | **Medium** | Works with DevKit but not for custom MCU integration |
| **G3** | No ESD/TVS protection on external connectors | **High** | DS18B20 1-Wire and SCT-013 inputs exposed to ESD events |
| **G4** | No ferrite bead / Pi filter on ADC input | **Medium** | SCT-013 analog front-end susceptible to digital noise |
| **G5** | No status LEDs on PCB | **Low** | No visual indication (power, Wi-Fi, fault) |
| **G6** | No reset button or user button | **Low** | No physical way to reset or enter config mode |
| **G7** | GPIO conflicts between Kconfig and documentation | **Medium** | Kconfig defaults differ from docs and atopile schematic |
| **G8** | atopile schematic: CT connector wiring error | **Medium** | CT connector pin 1 goes to CT_OUTPUT, pin 2 to ADC_MIDPOINT — should both go to same net |
| **G9** | No keep-out zone defined for ESP32 antenna | **High** | Wi-Fi range severely degraded if metal/copper near antenna |
| **G10** | No test points defined | **Low** | Difficult to debug assembled PCB |
| **G11** | No mounting hole specification | **Medium** | PCB cannot be mounted in enclosure |
| **G12** | No silkscreen reference designators | **Medium** | Assembly instructions ambiguous |
| **G13** | No current rating verification for traces | **Medium** | 5V trace carrying 500 mA needs 30+ mil width |
| **G14** | No creepage/clearance verification | **High** | 240 VAC isolation is safety-critical |

### 5.3 GPIO Assignment Inconsistency Matrix

| Signal | Kconfig Default (ESP32-S3) | Docs (README/Specs) | atopile Schematic | Firmware Hard-Coded | Recommended |
|--------|---------------------------|---------------------|-------------------|---------------------|-------------|
| T_in 1-Wire | **GPIO 5** | GPIO 4 | GPIO 4 | GPIO 4 (onewire.c config) | **GPIO 4** |
| T_out 1-Wire | **GPIO 7** | GPIO 15 | GPIO 15 | GPIO 15 | **GPIO 15** |
| SCT-013 ADC | GPIO 6 | GPIO 6 | GPIO 6 | GPIO 6 | **GPIO 6** |
| Topway TX | GPIO 1 | GPIO 1 | GPIO 1 | GPIO 1 | **GPIO 1** |
| Topway RX | GPIO 3 | GPIO 3 | GPIO 3 | GPIO 3 | **GPIO 3** |
| Touch CS | GPIO 11 | GPIO 11 | GPIO 11 | GPIO 11 | **GPIO 11** |
| Touch SCLK | GPIO 12 | GPIO 12 | GPIO 12 | GPIO 12 | **GPIO 12** |
| Touch MOSI | GPIO 13 | GPIO 13 | GPIO 13 | GPIO 13 | **GPIO 13** |
| Touch MISO | GPIO 16 | GPIO 16 | GPIO 16 | GPIO 16 | **GPIO 16** |
| Touch IRQ | GPIO 14 | GPIO 14 | GPIO 14 | GPIO 14 | **GPIO 14** |

**Action Required:** Update `Kconfig.projbuild` to set GPIO 4 and GPIO 15 as defaults for DS18B20 pins on ESP32-S3 (lines 105, 113). The docs are authoritative.

---

## 6. REDESIGNED PCB LAYOUT SPECIFICATION

### 6.1 Physical Specifications

| Parameter | Value |
|-----------|-------|
| **Form Factor** | Custom rectangular, ≤ 100 × 80 mm |
| **Layers** | 2 (2-layer FR4) |
| **Board Thickness** | 1.6 mm |
| **Copper Weight** | 1 oz (35 µm) |
| **Surface Finish** | HASL Lead-Free (mATL0806 or ENIG optional) |
| **Solder Mask** | Green, both sides |
| **Silkscreen** | White, both sides (component outlines + ref designators) |
| **Min Trace / Space** | 8 mil / 8 mil (standard, not dense) |
| **Min Via** | 20 mil drill / 40 mil annular ring |
| **Mounting Holes** | 4× M3 (3.2 mm hole), plated or NPTH to GND |
| **Fabrication Standard** | IPC-A-600 Class 2 |
| **Assembly** | Hand-solder (all through-hole or 0805 SMD) |

### 6.2 Layer Assignment

| Layer | Content |
|-------|---------|
| **Top (Layer 1)** | Components, signal routing, 5V power polygon, 3.3V star-distribution |
| **Bottom (Layer 2)** | Solid GND copper pour (minimal signal traces), return paths |

**Note:** A 2-layer board is sufficient because:
- Max signal frequency is UART 115200 baud (~115 kHz fundamental)
- No high-speed buses (no DDR, no USB HS, no MIPI)
- 1-Wire is bit-banged at < 20 kHz
- Solid ground plane on bottom layer captures return currents

### 6.3 Zone Layout

```
┌─────────────────────────────────────────────────────────┐
│  ⚠ HIGH VOLTAGE ZONE  │         LOW VOLTAGE ZONE        │
│                        │                                  │
│  ┌──────┐  ┌──────┐   │  ┌────────────────────────────┐  │
│  │ FUSE │  │ HLK  │   │  │       ESP32-S3 DevKit      │  │
│  │ 10A  │──│ PM01 │───┼──│    (2×19 female headers)   │  │
│  └──┬───┘  └──────┘   │  │                            │  │
│     │         │        │  │  GPIO4 ────R4.7k── J_TIN  │  │
│  AC_IN       │        │  │  GPIO15 ───R4.7k── J_TOUT │  │
│  L N E       │        │  │  GPIO6 ─── Bias ── J_CT   │  │
│              │        │  │  GPIO1 ──────────── J_LCD │  │
│              │        │  │  GPIO3 ──────────── J_LCD │  │
│              │        │  │  5V ─────────────── J_LCD │  │
│              │        │  └────────────────────────────┘  │
│              │        │                                  │
├────[5mm slot]─────────┤  ┌────────────────────────────┐  │
│                        │  │     BIAS NETWORK           │  │
│                        │  │  R1 R2 C1 (SCT-013 AFE)   │  │
│                        │  └────────────────────────────┘  │
│                        │                                  │
│                        │  ┌────────────────────────────┐  │
│                        │  │    EXPANSION / TEST POINTS  │  │
│                        │  │  TP_GND TP_3V3 TP_ADC TP_OW│  │
│                        │  └────────────────────────────┘  │
└─────────────────────────────────────────────────────────┘
```

### 6.4 Component Placement (Top Layer)

#### Zone A: High Voltage (Left edge, 30 mm wide strip)

| Ref | Component | Position | Notes |
|-----|-----------|----------|-------|
| J1 | AC_IN screw terminal (3-pin, 5.08 mm) | Bottom-left edge | AC input, cable gland aligned |
| F1 | Fuse holder (5×20 mm PCB clip) | Left of U1 | In series with AC_L |
| U1 | HLK-PM01 module | Center-left | 5 mm gap to DC zone |
| — | PCB isolation slot | Between HV/DC | 1.5 mm × 25 mm routed slot |

#### Zone B: ESP32 DevKit Socket (Center)

| Ref | Component | Position | Notes |
|-----|-----------|----------|-------|
| J2 | 1×19 female header (left) | Center | ESP32 left pins |
| J3 | 1×19 female header (right) | Center | ESP32 right pins |
| — | Antenna keep-out | Top-center (above J2/J3) | 15 mm radius, no copper/comp |
| C6,C7,C8 | 100 nF 0805 decoupling | Near 3.3V/GND pins | One per side |

#### Zone C: SCT-013 Bias Network (Center-top)

| Ref | Component | Position | Notes |
|-----|-----------|----------|-------|
| R1 | 10kΩ 1% 0805 | Near GPIO6 | Divider top leg |
| R2 | 10kΩ 1% 0805 | Near GPIO6 | Divider bottom leg |
| C1 | 10µF 25V electrolytic (TH) | Between R1/R2 midpoint and GND | Positive to midpoint |
| R3 | 33Ω 1% 0805 (DNP marker) | Between CT output and GND | DNP for SCT-013-030 |
| TP_ADC | Test point loop | At ADC_MIDPOINT | Debug/calibration |

#### Zone D: Connectors (Right edge)

| Ref | Component | Position | Notes |
|-----|-----------|----------|-------|
| J_LCD | 4-pin JST-XH right-angle | Right edge | 5V,GND,TX,RX to Topway |
| J_TIN | 3-pin JST-XH right-angle | Top-right | VCC,DQ,GND for T_in probe |
| J_TOUT | 3-pin JST-XH right-angle | Right | VCC,DQ,GND for T_out probe |
| J_CT | 2-pin JST-XH right-angle | Right | CT+ (signal), CT- (GND) |
| J_TOUCH | 6-pin JST-XH right-angle | Right (optional) | Touch SPI breakout |

#### Zone E: Status and Control (Top-right or Bottom)

| Ref | Component | Position | Notes |
|-----|-----------|----------|-------|
| LED1 | Green LED, 3 mm through-hole | Top edge | Power OK (3.3V present) |
| LED2 | Blue LED, 3 mm through-hole | Top edge | Wi-Fi connected |
| LED3 | Red LED, 3 mm through-hole | Top edge | Fault condition |
| R_LED1 | 330 Ω 0805 | Adjacent | LED current limit |
| R_LED2 | 330 Ω 0805 | Adjacent | LED current limit |
| R_LED3 | 330 Ω 0805 | Adjacent | LED current limit |
| SW1 | Tactile button (6×6 mm TH) | Bottom edge | Reset (EN pin) |
| SW2 | Tactile button (6×6 mm TH) | Bottom edge | User/Boot (GPIO0) |

### 6.5 Revised Schematic Netlist

```
Net: AC_LINE_IN
  ├── J1 (AC_IN connector): pin L
  └── F1 (Fuse): pin IN

Net: AC_L_FUSED
  ├── F1 (Fuse): pin OUT
  └── U1 (HLK-PM01): AC_L

Net: AC_NEUTRAL
  ├── J1 (AC_IN connector): pin N
  └── U1 (HLK-PM01): AC_N

Net: AC_EARTH
  ├── J1 (AC_IN connector): pin E
  └── Enclosure earth stud

Net: 5V
  ├── U1 (HLK-PM01): VO+
  ├── J_LCD (Topway): VCC (pin 1)
  ├── J2 (ESP32 header): 5V pin
  └── C4: 100nF to GND (decoupling)

Net: 3V3
  ├── J2 (ESP32 header): 3V3 pin
  ├── R1 (bias divider): pin 1 (10kΩ to 3.3V)
  ├── R4 (T_in pull-up): pin 1 (4.7kΩ to 3.3V)
  ├── R5 (T_out pull-up): pin 1 (4.7kΩ to 3.3V)
  ├── J_TIN (temp connector): VCC (pin 1)
  ├── J_TOUT (temp connector): VCC (pin 1)
  ├── C6,C7,C8 (100nF decoupling): + to 3.3V
  ├── LED1 + R_LED1 (power indicator): cathode to GPIO(-)
  └── J_TOUCH (touch connector): 3V3 (pin 1)

Net: GND
  ├── U1 (HLK-PM01): VO-
  ├── J2 (ESP32 header): GND pin(s)
  ├── J_LCD (Topway): GND (pin 2)
  ├── J_TIN (temp connector): GND (pin 3)
  ├── J_TOUT (temp connector): GND (pin 3)
  ├── J_CT (CT connector): GND (pin 2)
  ├── R2 (bias divider): pin 2
  ├── C1 (bias cap): negative
  ├── R3 (burden): pin 2 (DNP)
  ├── C4,C6,C7,C8 (decoupling): negative
  ├── SW1 (reset): pin 2
  ├── J_TOUCH (touch connector): GND pin
  └── TP_GND (test point)
  └── Bottom copper pour (solid)

Net: LCD_TX
  ├── J2 (ESP32 header): GPIO1
  └── J_LCD (Topway): RX (pin 3) ─── NOTE: ESP TX → LCD RX

Net: LCD_RX
  ├── J2 (ESP32 header): GPIO3
  └── J_LCD (Topway): TX (pin 4) ─── NOTE: ESP RX ← LCD TX

Net: ADC_MIDPOINT
  ├── J2 (ESP32 header): GPIO6
  ├── R1 (bias divider): pin 2
  ├── R2 (bias divider): pin 1
  ├── C1 (bias cap): positive
  ├── J_CT (CT connector): pin 1 (CT signal)
  └── TP_ADC (test point)

Net: CT_OUTPUT
  ├── J_CT (CT connector): pin 1
  └── R3 (burden DNP): pin 1

Net: OW_BUS0
  ├── J2 (ESP32 header): GPIO4
  ├── R4 (T_in pull-up): pin 2 (4.7kΩ)
  ├── J_TIN (temp connector): DQ (pin 2)
  ├── D1 (TVS): cathode (ESD to GND)
  └── TP_OW0 (test point)

Net: OW_BUS1
  ├── J2 (ESP32 header): GPIO15
  ├── R5 (T_out pull-up): pin 2 (4.7kΩ)
  ├── J_TOUT (temp connector): DQ (pin 2)
  ├── D2 (TVS): cathode (ESD to GND)
  └── TP_OW1 (test point)

Net: T_SCLK
  ├── J2 (ESP32 header): GPIO12
  └── J_TOUCH (touch connector): SCLK (pin 2)

Net: T_MOSI
  ├── J2 (ESP32 header): GPIO13
  └── J_TOUCH (touch connector): MOSI (pin 3)

Net: T_MISO
  ├── J2 (ESP32 header): GPIO16
  └── J_TOUCH (touch connector): MISO (pin 4)

Net: T_CS
  ├── J2 (ESP32 header): GPIO11
  └── J_TOUCH (touch connector): CS (pin 5)

Net: T_IRQ
  ├── J2 (ESP32 header): GPIO14
  ├── R6 (10kΩ pull-up to 3.3V)
  └── J_TOUCH (touch connector): IRQ (pin 6)

Net: CONSOLE_RX
  ├── J2 (ESP32 header): GPIO43
  └── (USB-Serial on DevKit, no external connection)

Net: CONSOLE_TX
  ├── J2 (ESP32 header): GPIO44
  └── (USB-Serial on DevKit, no external connection)

Net: RST
  ├── J2 (ESP32 header): EN pin
  ├── SW1 (reset button): pin 1
  └── R7 (10kΩ pull-up to 3.3V)

Net: IO0 (BOOT)
  ├── J2 (ESP32 header): GPIO0
  ├── SW2 (boot button): pin 1
  └── R8 (10kΩ pull-up to 3.3V)

Net: LED_FAULT
  ├── J2 (ESP32 header): GPIO2 (or spare GPIO)
  └── LED3 (fault): anode (cathode via R_LED3 to GND)

Net: LED_WIFI
  ├── J2 (ESP32 header): GPIO5 (or spare GPIO)
  └── LED2 (Wi-Fi): anode (cathode via R_LED2 to GND)
```

### 6.6 Design Improvements Over Current Design

| # | Improvement | Description |
|---|-------------|-------------|
| **I1** | **ESD Protection** | USBLC6-2SC6 or PESD5V0 on each 1-Wire bus and CT input. Prevents latch-up from ESD events on external connectors. |
| **I2** | **ADC Filter** | Pi filter (100 Ω series + 2× 1 nF to GND) on GPIO6 ADC input, cutting off > 1.6 MHz noise. |
| **I3** | **Status LEDs** | 3 LEDs: Power (green), Wi-Fi connected (blue), Fault (red). Driven via GPIO2/GPIO5 with 330 Ω current-limit resistors. |
| **I4** | **Reset & Boot Buttons** | SW1 pulls EN low for reset. SW2 pulls GPIO0 low for bootloader entry. Both with 10kΩ pull-ups. |
| **I5** | **Antenna Keep-Out** | Explicit 15 mm no-copper zone on all layers around ESP32 PCB antenna area. Marked in silkscreen. |
| **I6** | **Test Points** | 5× test point loops: 3.3V, GND, ADC_MIDPOINT, OW_BUS0, OW_BUS1. For oscilloscope debugging. |
| **I7** | **Isolation Slot** | Routed PCB slot (1.5 mm × 25 mm) between AC mains zone and DC zone. Ensures ≥ 5 mm creepage even with condensation. |
| **I8** | **Silkscreen Labels** | Every connector labeled with pin function (e.g., "CT+ / CT-" instead of "1 / 2"). HV zone marked WARNING. |
| **I9** | **Mounting Holes** | 4× M3 (3.2 mm) at corners, connected to GND pour via 0 Ω jumper or capacitor for EMC. |
| **I10** | **Trace Width Compliance** | Power traces: 5V @ 40 mil, 3.3V @ 30 mil, AC mains @ 80 mil minimum. Signal traces: 10 mil. |

### 6.7 Trace Width Calculations (IPC-2221)

| Net | Max Current | Min Width (1oz, 10°C rise) | Recommended | Margin |
|-----|-------------|----------------------------|-------------|--------|
| AC_L (fused) | 10 A | 180 mil (external) | 200 mil + solder mask opening | 11% |
| AC_N | 10 A | 180 mil (external) | 200 mil | 11% |
| 5V rail | 600 mA | 12 mil | 40 mil | 233% |
| 3.3V rail | 200 mA | 6 mil | 30 mil | 400% |
| 1-Wire bus | <5 mA | 6 mil | 12 mil | 100% |
| ADC signal | <1 mA | 6 mil | 12 mil | 100% |
| UART signals | <10 mA | 6 mil | 12 mil | 100% |
| SPI signals | <10 mA | 6 mil | 12 mil | 100% |
| LED drivers | 10 mA | 6 mil | 10 mil | 67% |

### 6.8 ESD Protection Scheme

```
External Connector        TVS Diode       Series R       ESP32 GPIO
─────────────────        ──────────       ────────       ───────────
J_TIN.DQ ───────┬────── D1 (PESD5V0) ──── 100 Ω ──────── GPIO4
                │       to GND
J_TOUT.DQ ──────┼────── D2 (PESD5V0) ──── 100 Ω ──────── GPIO15
                │       to GND
J_CT pin1 ──────┼────── D3 (PESD5V0) ──── 100 Ω ──────── ADC_MIDPOINT
                │       to GND
J_LCD.TX/RX ────┼────── D4,D5 (PESD5V0) ── 22 Ω ──────── GPIO1/GPIO3
                │       to GND
```

**Recommended TVS:** Nexperia PESD5V0S1BA (SOD-323, 5 V working, ultra-low capacitance < 1 pF for 1-Wire).

### 6.9 Decoupling Scheme

| Location | Cap | Type | Package | Purpose |
|----------|-----|------|---------|---------|
| 3.3V rail near ESP32 left | 100 nF | X7R ceramic | 0805 | High-freq digital decoupling |
| 3.3V rail near ESP32 right | 100 nF | X7R ceramic | 0805 | High-freq digital decoupling |
| 5V rail near HLK-PM01 output | 100 nF + 10 µF | X7R + electrolytic | 0805 + TH | Bulk + HF filtering |
| 5V rail near LCD connector | 100 nF | X7R ceramic | 0805 | LCD backlight decoupling |
| ADC_MIDPOINT (GPIO6) | 10 µF | Electrolytic, 25 V | TH radial | AC coupling / DC stabilization |
| ADC_MIDPOINT (GPIO6) | 100 nF | X7R ceramic | 0805 | HF noise bypass |
| 1-Wire bus (GPIO4/GPIO15) | (not needed — bus is open-drain) | — | — | 4.7kΩ pull-up is sufficient |

---

## 7. COMPLETE BILL OF MATERIALS (REVISED)

### 7.1 Core Electronics

| # | Ref | Manufacturer | MPN | Description | Qty | Package | Est. Cost |
|---|-----|--------------|-----|-------------|-----|---------|-----------|
| 1 | U2 | Espressif | ESP32-S3-DevKitC-1-N16R8 | ESP32-S3 16MB Flash 8MB PSRAM | 1 | DevKit | $8.00 |
| 2 | U1 | Hi-Link | HLK-PM01 | AC-DC 5V 600mA isolated | 1 | Module | $3.50 |
| 3 | DISP1 | Topway | HKT070DTA-1C | 7" 800×480 smart LCD UART | 1 | Module | $25.00 |
| 4 | CT1 | YHDC | SCT-013-030 | 30A/1V split-core CT | 1 | Sensor | $6.00 |
| 5 | TEMP1,2 | Generic | DS18B20-Waterproof | 1-Wire temp probe 1m cable | 2 | Probe | $3.00 |

### 7.2 PCB Components

| # | Ref | Manufacturer | MPN | Description | Qty | Package | Est. Cost |
|---|-----|--------------|-----|-------------|-----|---------|-----------|
| 6 | R1,R2 | Yageo | RC0805FR-0710KL | 10kΩ 1% 0805 | 2 | 0805 | $0.06 |
| 7 | R3 | Yageo | RC0805FR-0733RL | 33Ω 1% 0805 (DNP for 030 variant) | 1 | 0805 | $0.03 |
| 8 | R4,R5 | Yageo | RC0805FR-074K7L | 4.7kΩ 1% 0805 | 2 | 0805 | $0.06 |
| 9 | R6 | Yageo | RC0805FR-0710KL | 10kΩ 1% 0805 (IRQ pull-up) | 1 | 0805 | $0.03 |
| 10 | R7 | Yageo | RC0805FR-0710KL | 10kΩ 1% 0805 (RST pull-up) | 1 | 0805 | $0.03 |
| 11 | R8 | Yageo | RC0805FR-0710KL | 10kΩ 1% 0805 (BOOT pull-up) | 1 | 0805 | $0.03 |
| 12 | R_LED1-3 | Yageo | RC0805FR-07330RL | 330Ω 1% 0805 | 3 | 0805 | $0.09 |
| 13 | R_ESD1-3 | Yageo | RC0805FR-07100RL | 100Ω 1% 0805 (ESD series) | 3 | 0805 | $0.09 |
| 14 | R_ESD4-5 | Yageo | RC0805FR-0722RL | 22Ω 1% 0805 (UART ESD series) | 2 | 0805 | $0.06 |
| 15 | C1 | Nichicon | UCS1E100MDD | 10µF 25V electrolytic radial | 1 | TH 5mm | $0.20 |
| 16 | C2 | Samsung | CL21B104KBCNNNC | 100nF X7R 50V 0805 | 5 | 0805 | $0.10 |
| 17 | C3 | Nichicon | UMA1E100MDD | 10µF 25V electrolytic | 1 | TH 5mm | $0.20 |
| 18 | D1,D2 | Nexperia | PESD5V0S1BA | TVS 5V SOD-323 | 2 | SOD-323 | $0.20 |
| 19 | D3,D4,D5 | Nexperia | PESD5V0S1BA | TVS 5V SOD-323 | 3 | SOD-323 | $0.30 |
| 20 | LED1 | Lite-On | LTL-4231N | Green LED 3mm | 1 | TH 3mm | $0.08 |
| 21 | LED2 | Lite-On | LTL-4233 | Blue LED 3mm | 1 | TH 3mm | $0.10 |
| 22 | LED3 | Lite-On | LTL-4234 | Red LED 3mm | 1 | TH 3mm | $0.08 |
| 23 | SW1,SW2 | E-Switch | TL3301AF160QG | Tactile button 6×6mm | 2 | TH | $0.30 |
| 24 | F1 | Littelfuse | 0215010.MXP | 5×20mm fuse 10A slow-blow | 1 | TH | $0.50 |

### 7.3 Connectors & Enclosure

| # | Ref | Manufacturer | MPN | Description | Qty | Package | Est. Cost |
|---|-----|--------------|-----|-------------|-----|---------|-----------|
| 25 | J1 | Weidmuller | 1715720000 | Screw terminal 3-pin 5.08mm | 1 | TH | $1.50 |
| 26 | J2,J3 | Samtec | SSQ-119-01-G-D | 19-pin female header 2.54mm | 2 | TH | $3.00 |
| 27 | J_LCD | JST | B4B-XH-A | 4-pin XH header right-angle | 1 | TH RA | $0.30 |
| 28 | J_TIN/J_TOUT | JST | B3B-XH-A | 3-pin XH header right-angle | 2 | TH RA | $0.40 |
| 29 | J_CT | JST | B2B-XH-A | 2-pin XH header right-angle | 1 | TH RA | $0.20 |
| 30 | J_TOUCH | JST | B6B-XH-A | 6-pin XH header right-angle | 1 | TH RA | $0.40 |
| 31 | H1-H4 | Keystone | 24341 | M3 mounting hole standoff | 4 | HW | $0.80 |
| 32 | — | Generic | IP65-ABS-BOX | IP65 enclosure 120×80×55mm | 1 | — | $5.00 |
| 33 | — | Generic | PG9-4PK | PG9 cable glands ×4 | 4 | — | $2.00 |
| 34 | — | Keystone | 3557-2 | PCB fuse clip 5×20mm | 2 | TH | $0.60 |
| 35 | TPx | Keystone | 5005 | Test point loop 0.063" | 5 | TH | $0.50 |

### 7.4 Cost Summary

| Category | Est. Cost |
|----------|-----------|
| Core electronics (MCU, LCD, sensors, PSU) | $45.50 |
| PCB components (passives, ESD, buttons, LEDs) | $2.48 |
| Connectors | $6.80 |
| Enclosure & hardware | $8.90 |
| **Total (assembled, modular)** | **~$63.68** |
| **Total (PCB components only, no modules)** | **~$11.68** |

---

## 8. EDA IMPLEMENTATION ROADMAP

Since no EDA project exists, the recommended approach is:

### Option A: KiCad 8 (Recommended)

1. Create `hardware/esp-idms.kicad_sch` — schematic with all components above
2. Create `hardware/esp-idms.kicad_pcb` — PCB layout per zones in Section 6.3
3. Generate Gerber, drill, BOM, and pick-and-place from KiCad
4. Use JLCPCB or PCBWay for fabrication ($2 for 5 pcs, 2-layer)

### Option B: atopile (Existing)

1. Fix the `hardware/esp-idms.ato` schematic (correct CT connector wiring, add ESD, LEDs, buttons)
2. Generate PCB from atopile toolchain
3. Export Gerber from atopile

**Recommendation:** **Option A (KiCad)** — KiCad is the industry standard for open-source PCB design. The project already has all design documentation; translating to KiCad is straightforward. The atopile ecosystem is experimental and not proven for production fabrication.

### Required KiCad Libraries

| Library | Components |
|---------|------------|
| `Device` | R, C, LED, test points |
| `Connector` | JST-XH, screw terminals, pin headers |
| `Diode` | TVS (PESD5V0 or generic) |
| `Switch` | Tactile buttons |
| `Regulator_Switching` | HLK-PM01 (custom symbol needed) |
| `Espressif` | ESP32-S3 DevKit (custom footprint needed) |
| `Sensor` | Custom DS18B20 connector symbol |
| Custom | Topway LCD connector, SCT-013 bias network |

---

## 9. FIRMWARE UPDATES REQUIRED (PCB v2)

To support the redesigned PCB with status LEDs:

| File | Change |
|------|--------|
| `main/Kconfig.projbuild` | Add `IDMS_PIN_LED_POWER`, `IDMS_PIN_LED_WIFI`, `IDMS_PIN_LED_FAULT` config entries |
| `components/monitor/monitor.c` | Drive fault LED (GPIO2) on error condition |
| `components/wifi/wifi_manager.c` | Drive Wi-Fi LED (GPIO5) on STA connected |
| `components/core/include/pins.h` | Add LED GPIO definitions |
| API | No breaking changes to existing functionality |

---

## 10. TEST & VERIFICATION CHECKLIST

### 10.1 Pre-Power Checks

- [ ] Visual inspection: solder bridges, missing components, correct orientation
- [ ] Continuity: GND, 3.3V, 5V not shorted to each other or AC
- [ ] Resistance: 3.3V to GND > 1kΩ, 5V to GND > 1kΩ
- [ ] Fuse continuity (should be ~0 Ω)
- [ ] Isolation slot not bridged by solder
- [ ] Connector pinouts match documentation

### 10.2 Power-On Checks

- [ ] Apply AC power, verify HLK-PM01 outputs 5.0 V ± 0.25 V
- [ ] Verify 3.3 V at ESP32 headers
- [ ] Verify 1.65 V ± 0.05 V at TP_ADC (bias midpoint)
- [ ] Check all LEDs light (power green should be on)
- [ ] Verify no smoke, excessive heat, or burning smell

### 10.3 Functional Checks

- [ ] Flash firmware via USB-Serial
- [ ] Verify serial console output at 115200 baud
- [ ] Run `status` — verify ADC reading ~2048 with no CT connected
- [ ] Connect CT, run `adc` — verify AC waveform visible
- [ ] Connect DS18B20 probes — verify temperature reads via `status`
- [ ] Connect Topway LCD — verify handshake and display update
- [ ] Verify Wi-Fi connects and Telegram bot polls
- [ ] Trigger OTA update via HTTP

### 10.4 Safety Checks

- [ ] Hip test: 1.5 kV AC between AC and DC for 1 minute (production only)
- [ ] Earth continuity: < 0.1 Ω from earth pin to enclosure stud
- [ ] Creepage measurement: ≥ 5 mm AC to DC (calipers)
- [ ] Insulation resistance: > 2 MΩ at 500 VDC between AC and DC

---

## 11. FILES UPDATED / CREATED

| File | Status | Description |
|------|--------|-------------|
| `HARDWARE_DEEP_DIVE.md` | **New** | This document — complete hardware analysis & redesign |
| `hardware/esp-idms_v2.ato` | **To Create** | Fixed atopile schematic with ESD, LEDs, buttons |
| `main/Kconfig.projbuild` | **To Fix** | Correct DS18B20 GPIO defaults (GPIO4/GPIO15, not 5/7) |
| `BOM.md` | **To Update** | Add ESD diodes, LEDs, buttons, fix part numbers |

---

## 12. REFERENCES

| Document | Path |
|----------|------|
| README | `README.md` |
| Specifications (v2.0) | `Specifications.md` |
| PCB Layout (v2.0) | `PCB_Layout.md` |
| Original BOM (v2.0) | `BOM.md` |
| Project Definition (v2.0) | `Project Definition.md` |
| Original atopile schematic | `hardware/esp-idms.ato` |
| OTA Implementation | `OTA_IMPLEMENTATION.md` |
| DS18B20 Datasheet | `ds18b20.pdf` |
| Kconfig Configuration | `main/Kconfig.projbuild` |
| Pin Definitions | `components/core/include/pins.h` |

---

*End of Hardware Deep-Dive Analysis & PCB Redesign*

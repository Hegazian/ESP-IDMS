# PCB Layout & Schematic — ESP-IDMS (ESP32-S3 + Topway LCD)

**Document Version:** 2.0  
**Date:** April 2026  
**Target:** ESP32-S3 N16R8 + Topway HKT070DTA-1C

> Current-state note, 2026-05-10:
> This file is still a PCB/layout guidance document, not a fabricable PCB
> package. Use `README.md`, `Specifications.md`, and `PROJECT_HANDOFF.md` for
> the current firmware contract before updating the PCB. The next hardware
> phase should produce real EDA files, Gerbers, assembly drawings, and a
> manufacturing test procedure.

---

## 1. System Architecture

```
┌──────────────────────────────────────────────────────────────────┐
│                        ESP-IDMS PCB                              │
│                                                                  │
│  ┌──────────────┐    ┌───────────────┐    ┌──────────────────┐  │
│  │  HLK-PM01    │    │   ESP32-S3     │    │  Topway LCD      │  │
│  │  AC-DC 5V    │    │   DevKit       │    │  HKT070DTA-1C    │  │
│  │  100-240VAC  │5V──│ 3.3V  GPIO44  │TX──│ UART1    RX      │  │
│  │  → 5V 600mA  │    │       GPIO43   │RX──│ UART1    TX      │  │
│  │              │    │       GPIO1    │TX──│ (RS232-TTL)     │  │
│  │  AC_IN       │    │       GPIO3    │RX──│                  │  │
│  │  L ──┐       │    │       GPIO6    │    │  800×480 LCD     │  │
│  │  N ──┤Fuse──┤    │       │        │    │  Resistive Touch  │  │
│  │  E ──┘       │    │    ┌──┴──┐     │    │  UART 115200     │  │
│  └──────────────┘    │    │SCT   │     │    └──────────────────┘  │
│                      │    │Bias  │     │                          │
│  ┌──────────────┐    │    │Circuit│     │    ┌──────────────────┐  │
│  │  SCT-013     │────│───►│GPIO6 │     │    │  DS18B20 ×2      │  │
│  │  CT Sensor   │    │    └──────┘     │    │  (1-Wire probes) │  │
│  └──────────────┘    │                 │    │                   │  │
│                      │  GPIO4 ─────┬───│──►│ DQ (T_in)         │  │
│                      │             4.7kΩ │  │                   │  │
│                      │  GPIO15 ────┬───│──►│ DQ (T_out)        │  │
│                      │             4.7kΩ │  │                   │  │
│                      │             3V3  │  │ VCC               │  │
│                      │             GND  │  │ GND               │  │
│                      └───────────────┘    └──────────────────┘  │
│                                                                  │
│  ┌──────────────────────────────────────────────────────────────┐│
│  │  XPT2046 Touch (on Topway LCD module)                       ││
│  │  GPIO12=SCLK  GPIO13=MOSI  GPIO16=MISO  GPIO11=CS  GPIO14=IRQ │
│  └──────────────────────────────────────────────────────────────┘│
└──────────────────────────────────────────────────────────────────┘
```

---

## 2. Layer Stack-Up

| Layer | Function |
|-------|----------|
| **Top (Layer 1)** | Component placement, primary signal routing, power distribution |
| **Bottom (Layer 2)** | Solid ground copper pour (GND plane), return current paths |

2-layer FR4, 1.6 mm, 1 oz (35 µm) copper. Sufficient for signal frequencies (1-Wire ~15 kHz, UART 115200 baud).

---

## 3. Component Placement

### 3.1 ESP32-S3 DevKit (pluggable)

- Use 2× 19-pin female pin headers at 2.54 mm pitch matching ESP32-S3 DevKit footprint
- Keep ESP32 antenna area clear: no copper pour, traces, or components within 15 mm
- Position centrally to minimize trace lengths to all peripherals
- Add labeled test points for all GPIOs

### 3.2 Topway LCD Interface

The HKT070DTA-1C smart LCD communicates via UART with a built-in RS232-TTL transceiver. Connection:

| Signal | PCB Net | ESP32-S3 GPIO | Direction |
|--------|---------|---------------|-----------|
| UART TX | LCD_TX | GPIO 1 | Output (ESP→LCD) |
| UART RX | LCD_RX | GPIO 3 | Input (LCD→ESP) |
| VCC | 5V | — | Power (5 V from HLK-PM01) |
| GND | GND | — | Common ground |

> **Important:** The Topway LCD has an onboard RS232-TTL level shifter. Use direct 3.3 V UART connection (no external MAX232 needed). Baud rate is factory-set to 115200 but changed at startup via software command.

### 3.3 SCT-013 Current Transformer

SCT-013 bias and burden circuit (soldered on PCB, close to GPIO6):

```
         3.3V ──── R1 (10kΩ) ──┬──── GPIO6 (ADC input)
                                 │
                                C1 (10µF electrolytic, + to GPIO6, − to GND)
                                 │
         GND ──── R2 (10kΩ) ──┤──── GND
                                 │
                    ┌───────────┘│
                    │            │
              SCT-013 ── R_burden ── GND
              (output wires)
```

**Component values:**

| Component | Value | Notes |
|-----------|-------|-------|
| R1 | 10 kΩ 1% | 3.3 V to GPIO6 voltage divider upper leg |
| R2 | 10 kΩ 1% | GPIO6 to GND voltage divider lower leg |
| C1 | 10 µF 25 V electrolytic | AC coupling + DC stabilization at midpoint |
| R_burden | 33 Ω 1% | **Only for SCT-013-000 (100 A)**; omit for SCT-013-030 (30 A) which has internal burden |

**Expected DC voltage at GPIO6 with no AC current:** ~1.65 V (ADC count ≈ 2048)

**When ADC reads 4095:** GPIO6 is floating (check R1 connection to 3.3 V)  
**When ADC reads ~0:** GPIO6 shorted to GND (check R2 connection)

### 3.4 DS18B20 Temperature Sensors

Each sensor bus needs a 4.7 kΩ pull-up resistor to 3.3 V:

```
    3.3V ──── 4.7kΩ ──┬──── GPIO4 (T_in) or GPIO15 (T_out)
                         │
                    DS18B20 data pin
                         │
                        GND
```

**Per-bus connections (identical for both T_in and T_out):**

| DS18B20 Wire | Connects To |
|--------------|-------------|
| Red (VCC) | 3.3 V |
| Yellow (Data) | GPIO4 (T_in) or GPIO15 (T_out) + 4.7 kΩ pull-up |
| Black/Bare (GND) | GND |

Each bus is independent (separate 1-Wire bus, separate pull-up). Do not put both sensors on one bus.

### 3.5 HLK-PM01 AC-DC Module

- Position at board edge, AC input facing enclosure cable gland
- **5 mm minimum creepage** between AC and DC zones
- Add PCB slot under the HLK-PM01 between AC and DC sides
- Label AC zone: `⚠ HIGH VOLTAGE`
- 10 A fuse in series with AC live conductor at board entry

### 3.6 XPT2046 Touch Controller

The Topway LCD has its own touch interface. The XPT2046 SPI connections are for the legacy ILI9341 TFT design and may be left unpopulated on a Topway-only build. If used:

| Signal | GPIO | Notes |
|--------|------|-------|
| SPI SCLK | GPIO 12 | Touch SPI clock |
| SPI MOSI | GPIO 13 | Touch SPI data out |
| SPI MISO | GPIO 16 | Touch SPI data in |
| CS | GPIO 11 | Touch chip select |
| IRQ | GPIO 14 | Touch pen-down interrupt |

---

## 4. Connector Pinouts

### 4.1 ESP32-S3 DevKit Header (2×19 pin)

```
Left side (J1):                    Right side (J2):
┌─────────────────────┐            ┌─────────────────────┐
│ 3V3          GPIO1   │            │  GND         GPIO43  │
│ EN/RST       GPIO2   │            │  GPIO44      GPIO42  │
│ VP/GPIO0     GPIO3   │            │  GPIO10      GPIO41   │
│ VN/GPIO4     GPIO4   │ ← T_in     │  GPIO5       GPIO40  │
│ GPIO5        GPIO5   │            │  GPIO6       GPIO39   │ ← ADC (SCT-013)
│ GPIO7        GPIO6   │ ← ADC      │  GPIO7       GPIO38  │ ← LCD_BL
│ GPIO15       GPIO7   │ ← T_out    │  GPIO8       GPIO37  │
│ GPIO16       GPIO8   │            │  GPIO9       GPIO36  │
│ ...          ...     │            │  ...         ...     │
└─────────────────────┘            └─────────────────────┘
```

### 4.2 Topway LCD Connector (4-pin JST-XH)

| Pin | Net | ESP32 GPIO | Notes |
|-----|-----|------------|-------|
| 1 | 5V | VCC (from HLK-PM01) | LCD power |
| 2 | GND | GND | Common ground |
| 3 | LCD_TX | GPIO 1 | ESP32 → LCD data |
| 4 | LCD_RX | GPIO 3 | LCD → ESP32 data |

### 4.3 DS18B20 Temperature Connector (3-pin JST-XH × 2)

| Pin | T_in (GPIO4) | T_out (GPIO15) |
|-----|---------------|-----------------|
| 1 | 3.3 V | 3.3 V |
| 2 | GPIO4 + 4.7 kΩ pull-up | GPIO15 + 4.7 kΩ pull-up |
| 3 | GND | GND |

### 4.4 SCT-013 CT Connector (2-pin JST-XH or screw terminal)

| Pin | Net | Notes |
|-----|-----|-------|
| 1 | CT_output (to burden + divider) | Connects to GPIO6 bias network midpoint |
| 2 | CT_return (to burden + divider GND) | Connects to GND |

---

## 5. Trace Routing Guidelines

| Net Type | Recommended Width |
|----------|-------------------|
| 1-Wire / ADC analog | 10–12 mil (0.25–0.30 mm) |
| UART (Topway) | 10–12 mil |
| 3.3 V logic power | 20–30 mil (0.50–0.75 mm) |
| 5 V power distribution | 30–40 mil (0.75–1.00 mm) |
| AC mains traces | ≥ 60 mil (1.50 mm), minimize length |

### Analog Isolation Rules

- **Do not** route the SCT-013 analog trace (GPIO6) parallel to or near high-frequency digital traces
- Maintain **20 mil (0.50 mm) minimum separation** between analog and digital zones
- Use ground pour as shield between analog and digital areas
- Place ADC bias components (R1, R2, C1, R_burden) within 10 mm of GPIO6 pad
- Dedicated analog ground island connected at single star-ground point near ADC input

---

## 6. Decoupling and Filtering

| Component | Location | Purpose |
|-----------|----------|---------|
| 100 nF ceramic | VCC pin of ESP32-S3 DevKit | High-frequency decoupling |
| 100 nF ceramic | VCC pin of Topway LCD connector | Display decoupling |
| 10 µF electrolytic | ADC bias midpoint (GPIO6) | AC stabilization and noise filtering |
| 4.7 kΩ resistor ×2 | GPIO4 and GPIO15 (1-Wire pull-ups) | 1-Wire bus integrity |

---

## 7. Electrical Safety

| Requirement | Details |
|-------------|---------|
| Creepage (AC to DC) | ≥ 5 mm (or physical PCB slot) |
| AC trace isolation | AC zone clearly separated and labelled |
| Fusing | 10 A fuse on AC live conductor at board entry |
| Enclosure | IP65 minimum, with cable glands on all wire entry points |
| Grounding | Enclosure earth stud connected to AC earth conductor |

---

## 8. Recommended PCB Fabrication Parameters

| Parameter | Value |
|-----------|-------|
| Layers | 2 |
| Board Thickness | 1.6 mm |
| Copper Weight | 1 oz (35 µm) |
| Surface Finish | HASL (Lead-Free) or ENIG |
| Solder Mask | Green (standard) or Black |
| Min Trace Width / Spacing | 6 mil / 6 mil |
| Board Material | FR4 |

---

## 9. Related Documents

| Document | Description |
|----------|-------------|
| [README.md](./README.md) | Project overview, pin map, console commands |
| [Specifications.md](./Specifications.md) | Electrical specs, thresholds, GPIO mapping |
| [PROJECT_HANDOFF.md](./PROJECT_HANDOFF.md) | Current handoff for next phase |
| [CLOUD_SETUP.md](./CLOUD_SETUP.md) | Cloud telemetry setup |
| [OTA_IMPLEMENTATION.md](./OTA_IMPLEMENTATION.md) | OTA behavior and validation |
| [BOM.md](./BOM.md) | Bill of Materials |
| [hardware/esp-idms.ato](./hardware/esp-idms.ato) | atopile schematic description |

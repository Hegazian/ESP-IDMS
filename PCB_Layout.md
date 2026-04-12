# PCB Layout Guidelines — ESP32 Industrial Device Monitoring System (ESP-IDMS)

**Document Version:** 1.1  
**Date:** April 2026  
**Status:** Approved

---

## 1. Overview

This document defines the PCB design guidelines, layer stack-up strategy, component placement rules, trace routing standards, and GPIO pin mapping for the ESP-IDMS hardware. The design targets a **2-layer PCB** to minimise fabrication cost while maintaining signal integrity and electrical safety in an industrial environment.

### 1.1 Modular vs on-board assembly

To simplify field repair, upgrades, and bench testing, the PCB separates **pluggable subsystems** from **permanently mounted base circuitry**.

| Assembly style | What belongs here | Rationale |
|---|---|---|
| **Pluggable (headers / cable connectors)** | ESP32 module, TFT display assembly, removable sensor harnesses | Highest swap/re-test rate; keeps firmware development and RMA workflows fast |
| **Soldered on main PCB** | HLK-PM01, fuse/holder, SCT-013 bias + burden network, 1-Wire pull-up, decoupling capacitors, screw terminals for fixed installation wiring, connector **footprints** (sockets/receptacles) | Stable, low-cycle-count parts; safety-critical and analog-sensitive circuits stay on one controlled layout |

Use **polarized, latching, or shrouded connectors** where mis-insertion would damage the ESP32 ADC or 3.3 V rails.

---

## 2. Layer Stack-Up

| Layer | Function |
|---|---|
| **Top Layer (Layer 1)** | Component placement, primary signal routing, power distribution |
| **Bottom Layer (Layer 2)** | Solid ground copper pour (GND plane), return current paths |

A 2-layer approach is sufficient for this design due to the relatively low signal frequencies involved (1-Wire at ~15 kHz, SPI at up to 40 MHz) and the moderate component count.

---

## 3. Component Placement Guidelines

### 3.1 ESP32 module (pluggable)

- Use **two rows of female pin headers** (2 × 15 positions at **2.54 mm** pitch) matching the **ESP32 DevKit V1** footprint, or an equivalent **socket strip** rated for multiple insert cycles.
- For easier unplugging, prefer **machine-pin round-hole sockets** or headers with **removal notch / pull tab** clearance in the enclosure layout; avoid soldering the DevKit directly to the mother PCB.
- Position centrally on the board to minimise routing distances to all peripherals.
- Route all signals that leave the ESP zone to **on-board test points** or **named nets** so a bare module swap does not require guesswork.
- Maintain a clear keep-out zone around the ESP32's on-board PCB antenna — **no copper pour, traces, or components** under or immediately adjacent to the antenna area.

### 3.2 AC-DC Power module (Hi-Link HLK-PM01) — soldered

- Position the HLK-PM01 at the **edge of the board**, oriented so that AC input terminals face an external cable gland.
- Maintain a minimum **5 mm creepage/clearance gap** between all AC-side conductors and any DC/logic traces. A physical **PCB slot or cutout** between the AC and DC zones is strongly recommended to meet IEC 60950 / IEC 62368 creepage requirements.
- Label the AC zone with a silkscreen warning: `⚠ HIGH VOLTAGE — DO NOT TOUCH`.

### 3.3 Field wiring terminals — soldered

- Use **5.08 mm pitch screw terminals** (or pluggable terminal blocks if you want tool-free mains/CT wiring while keeping the block body soldered) for **installation cables** that rarely move:
  - **AC mains input** to HLK-PM01 (2-pin), with fuse at entry
  - Optional: **SCT-013 CT** leads if you prefer screw termination to the factory CT cable instead of a pluggable CT jack (see §3.6)
- Position these at the **board perimeter** for cable glands and strain relief.

### 3.4 TFT display (pluggable harness)

- The 2.8" TFT module stays on the **enclosure lid** (or a sub-panel); the **main PCB only holds the receptacle**.
- Use a **board-mounted shrouded box header** (2.54 mm) **or** a compact **JST-SH / GH-style** latching cable to the lid, chosen to match your display breakout’s tail. The goal is **one keyed connector** per display assembly so the panel can be removed without desoldering.
- Keep the **SPI segment length** as short as practical from ESP socket to display connector; route **SCK/MOSI/CS/DC** as a group with adjacent **GND** pins or a parallel ground return on Layer 2.
- If the display module includes **XPT2046 touch** on a **second** flex or pin row, add a **second small connector** (e.g. 6-pin) rather than sharing a fragile single row.

**Suggested primary display connector (logic + SPI, 2.54 mm example — adjust to your module):**

| Pin | Net | ESP32 GPIO / net |
|---|---|---|
| 1 | GND | Ground |
| 2 | 3V3 | 3.3 V (display IO if module is 3.3 V tolerant) |
| 3 | SCK | GPIO 18 |
| 4 | MOSI | GPIO 23 |
| 5 | CS | GPIO 15 |
| 6 | DC | GPIO 2 |
| 7 | RST | **Tie to ESP32 EN on the motherboard** (per [`Specifications.md`](./Specifications.md)); pin may be omitted from cable if RST is hardwired on lid |
| 8 | LED / BL | Backlight control or always-on via resistor — match module |
| 9–10 | Touch SPI or IRQ | As required by lid PCB (or use separate connector) |

Document the **exact** pinout on the silkscreen next to the connector (`DISP1`).

### 3.5 Temperature and current “sensors” (pluggable probes)

Treat **factory-made probe leads** and **CT secondary leads** as removable harnesses:

- **DS18B20 (×2, shared 1-Wire bus):** Provide **two identical 3-pin pluggable connectors** (e.g. **JST-XH 3P** or **Molex PicoBlade**) wired in parallel: **3V3**, **DQ (GPIO 4)**, **GND**. Each waterproof probe then terminates in a mating plug for **T_in** and **T_out**. The **4.7 kΩ pull-up** remains **soldered on the main PCB** between 3V3 and DQ.
- **SCT-013 secondary:** Prefer a **2-pin pluggable polarized connector** (same family as above) from the CT burden/bias network to the CT cable; keep the **33 Ω burden** (if required) and **bias/divider network soldered** next to GPIO 34. If regulatory or site standards require hardwired CT leads, keep the **screw terminal** option as an alternate PCB footprint (not populated by default).

Label connectors `TEMP1`, `TEMP2`, `CT` on silkscreen.

### 3.6 Analog front-end (SCT-013 bias circuit) — soldered

- Place the DC offset bias network (two 10 kΩ resistors + 10 µF capacitor) as close as possible to the **GPIO 34 ADC pin** of the ESP32 **and** the CT connector.
- Route analog traces away from SPI and high-frequency digital traces to minimise noise coupling.

---

## 4. Trace Routing Guidelines

### 4.1 Ground Plane

- Implement a **solid copper pour for GND on the bottom layer** across the entire DC/logic zone. This provides a low-impedance return path and significantly reduces EMI susceptibility in an industrial environment.
- Define a **dedicated analog ground island** for the SCT-013 signal conditioning circuit. Connect this island to the main digital ground at a **single point** (star ground) near the ADC input to prevent high-frequency Wi-Fi RF noise from coupling into the analog measurement path.

### 4.2 Trace Width Recommendations

| Net Type | Recommended Width |
|---|---|
| Sensor signal lines (1-Wire, ADC) | 10 – 12 mil (0.25 – 0.30 mm) |
| SPI display signal lines | 10 – 12 mil |
| 3.3 V logic power | 20 – 30 mil (0.50 – 0.75 mm) |
| 5 V power distribution | 30 – 40 mil (0.75 – 1.00 mm) |
| AC mains traces (if on PCB) | ≥ 60 mil (1.50 mm) — minimise length |

### 4.3 Analog / Digital Isolation

- **Do not route** the SCT-013 analog signal trace parallel to or near TFT SPI clock or MOSI traces.
- Maintain a minimum **20 mil (0.50 mm) separation** between analog and high-frequency digital traces.
- Use the ground pour as a shield between the analog and digital zones wherever practical.

### 4.4 Decoupling Capacitors

- Place **100 nF ceramic decoupling capacitors** as close as possible to the VCC and 3.3 V pins of the ESP32 and any other active ICs.
- The 10 µF electrolytic capacitor for the ADC bias circuit should be placed directly at the voltage divider midpoint, as close to GPIO 34 as possible.

---

## 5. GPIO Pin Mapping

| Signal | GPIO | Direction | Notes |
|---|---|---|---|
| DS18B20 Data (1-Wire) | GPIO 4 | Bidirectional | 4.7 kΩ pull-up to 3.3 V required |
| SCT-013 Analog Output | GPIO 34 | Input (ADC) | Input-only pin; 12-bit ADC; DC-biased to 1.65 V |
| TFT SCK (SPI Clock) | GPIO 18 | Output | VSPI bus |
| TFT MOSI | GPIO 23 | Output | VSPI bus |
| TFT CS (Chip Select) | GPIO 15 | Output | Active low |
| TFT DC / RS | GPIO 2 | Output | High = Data, Low = Command |
| TFT RST (Reset) | ESP32 EN | Output | Tie to EN pin to free GPIO 4 for 1-Wire |

> **Design Note:** Connecting TFT RST to the ESP32 EN (Enable) pin is the recommended approach. This ensures the display resets together with the ESP32 and frees GPIO 4 for exclusive use by the DS18B20 1-Wire bus, avoiding a GPIO conflict.

---

## 6. ADC Bias Circuit — Schematic Description

The SCT-013 CT sensor output is an AC signal centred at 0 V. Since the ESP32 ADC can only measure 0–3.3 V, a bias circuit is required to shift the waveform midpoint to 1.65 V.

```
3.3V ──┬── 10kΩ ──┬── 10kΩ ── GND
       │           │
       │         [midpoint] ── GPIO 34 (ADC)
       │           │
       │         10µF (to GND)
       │
    [Leave floating or decouple with 100nF to GND]

SCT-013 Tip  ──── GPIO 34 (via series coupling)
SCT-013 Sleeve ── Midpoint of voltage divider
```

The voltage divider creates a stable 1.65 V reference. During normal operation, the AC current signal from the CT rides on top of this 1.65 V bias, allowing the ESP32 ADC to read both positive and negative half-cycles of the AC waveform within its 0–3.3 V input range.

---

## 7. Electrical Safety Requirements

| Requirement | Details |
|---|---|
| Creepage distance (AC to DC) | ≥ 5 mm (or physical PCB slot) |
| AC trace isolation | AC zone must be clearly separated and labelled |
| Fusing | 10 A fuse in series with AC live conductor at board/enclosure entry |
| Enclosure | IP65 minimum, with cable glands on all wire entry points |
| Grounding | Enclosure earth stud connected to AC earth conductor |

---

## 8. Mechanical / enclosure notes for modular builds

- Reserve **straight vertical unplug height** for the ESP32 above the socket (typically **≈ 25–35 mm** depending on USB connector and shield).
- Route display cables with a **service loop** or **panel-mount connector** on the lid so the lid can hinge open without pulling SPI pins.
- Align connector latches away from high-voltage zones; never place a low-voltage latch where fingers must reach past exposed mains.

---

## 9. Recommended PCB Fabrication Parameters

| Parameter | Value |
|---|---|
| Layers | 2 |
| Board Thickness | 1.6 mm |
| Copper Weight | 1 oz (35 µm) |
| Surface Finish | HASL (Lead-Free) or ENIG |
| Solder Mask | Green (standard) or Black |
| Min Trace Width / Spacing | 6 mil / 6 mil |
| Board Material | FR4 |

---

## 10. Related Documents

| Document | Description |
|---|---|
| [`Project Definition.md`](./Project%20Definition.md) | Full project scope and functional description |
| [`Specifications.md`](./Specifications.md) | Technical specifications, thresholds, and GPIO mapping |
| [`BOM.md`](./BOM.md) | Bill of Materials with part numbers and cost estimates |
| [`README.md`](./README.md) | Project overview and quick-start guide |
# Bill of Materials — ESP32 Industrial Device Monitoring System (ESP-IDMS)

**Document Version:** 1.1  
**Date:** April 2026  
**Status:** Approved

> All prices are approximate retail estimates (USD) at time of writing. Bulk pricing will reduce unit cost.

---

## Component List

| # | Item | Part / Specification | Qty | Est. Unit Cost (USD) | Est. Total (USD) | Purpose |
|---|---|---|---|---|---|---|
| 1 | **ESP32 DevKit V1** | Dual-Core Xtensa LX6, 240 MHz, 30-pin, Wi-Fi + Bluetooth | 1 | $5.00 | $5.00 | Core microcontroller, Wi-Fi connectivity |
| 2 | **2.8" TFT SPI Display** | ILI9341 Controller, 320×240 px, Resistive Touch | 1 | $9.00 | $9.00 | Real-time dashboard and configuration UI |
| 3 | **AC Current Transformer** | SCT-013-000, Non-Invasive, 30 A / 100 A variant | 1 | $6.00 | $6.00 | Non-invasive AC machine current sensing |
| 4 | **Temperature Sensor** | DS18B20 Waterproof Stainless Steel Probe, 1-Wire | 2 | $1.50 | $3.00 | Cooling inlet (T_in) and outlet (T_out) measurement |
| 5 | **Isolated AC-DC Module** | Hi-Link HLK-PM01, 100–240 V AC → 5 V DC, 600 mA | 1 | $3.50 | $3.50 | Galvanically isolated mains power supply |
| 6 | **Resistor Kit** | 4.7 kΩ × 1, 10 kΩ × 2, 33 Ω × 1 *(see note)* | 1 set | $1.00 | $1.00 | 1-Wire pull-up, ADC bias divider, burden resistor* |
| 7 | **Electrolytic Capacitor** | 10 µF, 25 V, Through-Hole | 1–2 | $0.25 | $0.50 | ADC bias midpoint filtering / noise decoupling |
| 8 | **Enclosure** | IP65 ABS Plastic Industrial Box | 1 | $4.00 | $4.00 | Environmental protection (dust and water) |
| 9 | **Miscellaneous Hardware** | Cable glands (PG-type), 10 A fuse + holder, mounting hardware | 1 lot | — | ~$2.00 | Cable ingress sealing and overcurrent protection |
| 10 | **ESP32 socket / headers** | 2 × 15 female machine-pin sockets (2.54 mm) or quality female headers | 2 strips | $1.50 | ~$3.00 | Plug-in ESP32 DevKit V1; field replacement without soldering |
| 11 | **Display harness connector pair** | Shrouded 2.54 mm box header + IDC ribbon **or** JST-SH/GH latched pair (match display module) | 1 set | $1.50 | ~$1.50 | Lid-mounted TFT disconnects from main PCB |
| 12 | **Temp probe connectors** | JST-XH 3-pin (or equivalent polarized) receptacle ×2 + mating housing on probe leads | 2 pairs | $0.40 | ~$0.80 | Plug-in **T_in** / **T_out** DS18B20 harnesses; pull-up stays on PCB |
| 13 | **CT secondary connector (optional)** | JST-XH 2-pin (or pluggable screw block) for SCT-013 leads after burden network | 1 pair | $0.40 | ~$0.40 | Tool-free CT swap; omit if site requires fixed screw terminals |

---

## Cost Summary

| Category | Est. Cost (USD) |
|---|---|
| Electronic Components (Items 1–7) | $27.00 |
| Enclosure (Item 8) | $4.00 |
| Miscellaneous Hardware (Item 9) | ~$2.00 |
| Modular connectors (Items 10–13, typical build) | ~$5.70 |
| **Total (per unit, modular field-serviceable build)** | **~$39** |
| **Total (per unit, base Items 1–9 only)** | **~$33** *(ESP socketed, display/sensors wired with screw terminals — see [`PCB_Layout.md`](./PCB_Layout.md))* |

---

## Component Notes

### Burden Resistor (33 Ω)
> **Only required for the SCT-013-000 (100 A) variant.**  
> The 30 A version of the SCT-013 includes an internal burden resistor. The 100 A open-core version (`SCT-013-000`) requires a **33 Ω external burden resistor** connected across the CT output terminals to convert the output current into a measurable voltage. Failure to install the burden resistor on the 100 A variant will damage the sensor.

### Backup Battery *(Recommended — Not Included)*
> A **18650 Li-ion cell** (≈ $3.00) paired with a **TP4056 charge and protection module** (≈ $1.00) is strongly recommended. This allows the ESP32 to issue a final "Power Loss" Telegram alert during a total facility power outage, which is the most critical failure scenario for this system.

### Pull-up Resistor (4.7 kΩ)
> Required on the DS18B20 data line (GPIO 4 to 3.3 V) for correct 1-Wire bus operation. The absence of this resistor will cause intermittent or failed temperature readings.

### Modular assembly (Items 10–13)
> Align connector **gender and pinout** with [`PCB_Layout.md`](./PCB_Layout.md): temperature connectors share one 1-Wire bus (parallel **3V3 / DQ / GND**). Crimp or solder mating leads on DS18B20 cables once; subsequent swaps are plug-in. Display connector choice depends on the exact ILI9341+XPT2046 breakout — use a keyed or latching series to avoid reverse insertion.

---

## Procurement Notes

- All components are widely available from suppliers such as **AliExpress**, **LCSC**, **Mouser**, **DigiKey**, and **Amazon**.
- For production quantities, the ESP32 and HLK-PM01 modules offer the greatest price reduction with volume.
- Ensure the SCT-013 variant ordered matches the expected machine current range (30 A or 100 A) before ordering the burden resistor.

---

## Related Documents

| Document | Description |
|---|---|
| [`Project Definition.md`](./Project%20Definition.md) | Full project scope and functional description |
| [`Specifications.md`](./Specifications.md) | Technical specifications, thresholds, and GPIO mapping |
| [`PCB_Layout.md`](./PCB_Layout.md) | PCB design guidelines and routing rules |
| [`README.md`](./README.md) | Project overview and quick-start guide |
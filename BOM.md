# Bill of Materials — ESP-IDMS v3 (ESP32-S3 + Topway LCD)

**Document Version:** 3.0  
**Date:** April 2026  
**Target:** ESP32-S3 N16R8 + Topway HKT070DTA-1C

> All prices are approximate retail (USD). Bulk pricing reduces unit cost.  
> DNP = Do Not Populate (marked on PCB, not soldered).

---

## Section A: Core Modules (Pre-built)

| # | Ref | Manufacturer | MPN | Description | Qty | Est. Unit | Est. Total |
|---|-----|--------------|-----|-------------|-----|-----------|------------|
| A1 | U2 | Espressif | ESP32-S3-DevKitC-1-N16R8 | ESP32-S3 16MB Flash, 8MB octal PSRAM, Wi-Fi+BLE | 1 | $8.00 | $8.00 |
| A2 | DISP1 | Topway | HKT070DTA-1C | 7" 800×480 smart LCD, UART, RS232-TTL | 1 | $25.00 | $25.00 |
| A3 | CT1 | YHDC | SCT-013-030 | 30 A / 1 V split-core CT, internal burden | 1 | $6.00 | $6.00 |
| A4 | TEMP1 | Generic | DS18B20-WP-1M | DS18B20 waterproof probe, 1 m cable (T_in) | 1 | $1.50 | $1.50 |
| A5 | TEMP2 | Generic | DS18B20-WP-1M | DS18B20 waterproof probe, 1 m cable (T_out) | 1 | $1.50 | $1.50 |
| A6 | U1 | Hi-Link | HLK-PM01 | AC-DC isolated, 100-240 VAC → 5 VDC 600 mA | 1 | $3.50 | $3.50 |
| | | | | **Section A Subtotal** | | | **$45.50** |

---

## Section B: PCB Passives (SMD 0805 unless noted)

| # | Ref | Manufacturer | MPN | Description | Qty | Package | Est. Unit | Est. Total |
|---|-----|--------------|-----|-------------|-----|---------|-----------|------------|
| B1 | R1, R2 | Yageo | RC0805FR-0710KL | 10 kΩ 1% 0.125 W thick film | 2 | 0805 | $0.02 | $0.04 |
| B2 | R3 | Yageo | RC0805FR-0733RL | 33 Ω 1% 0.125 W — **DNP** (for SCT-013-000 only) | 1 | 0805 | $0.02 | $0.02 |
| B3 | R_PU_TIN, R_PU_TOUT | Yageo | RC0805FR-074K7L | 4.7 kΩ 1% 0.125 W (1-Wire pull-ups) | 2 | 0805 | $0.02 | $0.04 |
| B4 | R6 | Yageo | RC0805FR-0710KL | 10 kΩ 1% (Touch IRQ pull-up) | 1 | 0805 | $0.02 | $0.02 |
| B5 | R7 | Yageo | RC0805FR-0710KL | 10 kΩ 1% (RST pull-up) | 1 | 0805 | $0.02 | $0.02 |
| B6 | R8 | Yageo | RC0805FR-0710KL | 10 kΩ 1% (BOOT pull-up) | 1 | 0805 | $0.02 | $0.02 |
| B7 | R_LED1, R_LED2, R_LED3 | Yageo | RC0805FR-07330RL | 330 Ω 1% 0.125 W (LED current limit) | 3 | 0805 | $0.02 | $0.06 |
| B8 | R_ESD_TIN, R_ESD_TOUT | Yageo | RC0805FR-07100RL | 100 Ω 1% (1-Wire ESD series) | 2 | 0805 | $0.02 | $0.04 |
| B9 | R_ESD_UART1, R_ESD_UART2 | Yageo | RC0805FR-0722RL | 22 Ω 1% (UART ESD series) | 2 | 0805 | $0.02 | $0.04 |
| B10 | R_FILT | Yageo | RC0805FR-07100RL | 100 Ω 1% (ADC Pi filter series) | 1 | 0805 | $0.02 | $0.02 |
| B11 | C1 | Nichicon | UMA1E100MDD | 10 µF 25 V electrolytic radial 5 mm (ADC bias) | 1 | TH radial | $0.15 | $0.15 |
| B12 | C2, C3 | Samsung | CL21C010CBNNNNC | 1 nF 50 V NP0 (ADC Pi filter) | 2 | 0805 | $0.02 | $0.04 |
| B13 | C4 | Nichicon | UMA1E100MDD | 10 µF 25 V electrolytic (5V bulk decoupling) | 1 | TH radial | $0.15 | $0.15 |
| B14 | C5, C_DC0, C_DC1, C_DC2 | Samsung | CL21B104KBCNNNC | 100 nF 50 V X7R (decoupling) | 4 | 0805 | $0.02 | $0.08 |
| | | | | **Section B Subtotal** | | | | **$0.74** |

---

## Section C: Semiconductors

| # | Ref | Manufacturer | MPN | Description | Qty | Package | Est. Unit | Est. Total |
|---|-----|--------------|-----|-------------|-----|---------|-----------|------------|
| C1 | D_ESD_TIN | Nexperia | PESD5V0S1BA | TVS 5 V SOD-323 (T_in 1-Wire ESD) | 1 | SOD-323 | $0.10 | $0.10 |
| C2 | D_ESD_TOUT | Nexperia | PESD5V0S1BA | TVS 5 V SOD-323 (T_out 1-Wire ESD) | 1 | SOD-323 | $0.10 | $0.10 |
| C3 | D3 | Nexperia | PESD5V0S1BA | TVS 5 V SOD-323 (SCT-013 ADC ESD) | 1 | SOD-323 | $0.10 | $0.10 |
| C4 | D4, D5 | Nexperia | PESD5V0S1BA | TVS 5 V SOD-323 (UART ESD) | 2 | SOD-323 | $0.10 | $0.20 |
| C5 | LED1 | Lite-On | LTL-4231N | Green LED 3 mm 2.1 Vf | 1 | TH 3 mm | $0.08 | $0.08 |
| C6 | LED2 | Lite-On | LTL-4233 | Blue LED 3 mm 3.2 Vf | 1 | TH 3 mm | $0.10 | $0.10 |
| C7 | LED3 | Lite-On | LTL-4234 | Red LED 3 mm 1.8 Vf | 1 | TH 3 mm | $0.08 | $0.08 |
| | | | | **Section C Subtotal** | | | | **$0.76** |

---

## Section D: Connectors, Switches & Hardware

| # | Ref | Manufacturer | MPN | Description | Qty | Package | Est. Unit | Est. Total |
|---|-----|--------------|-----|-------------|-----|---------|-----------|------------|
| D1 | J1 | Weidmüller | 1715720000 | Screw terminal 3-pin 5.08 mm pitch (AC input) | 1 | TH | $1.50 | $1.50 |
| D2 | J2, J3 | Samtec | SSQ-119-01-G-D | 19-pin female machine-pin header 2.54 mm (ESP32 socket) | 2 | TH | $1.50 | $3.00 |
| D3 | J_LCD | JST | B4B-XH-A(LF)(SN) | 4-pin JST-XH header right-angle (Topway LCD) | 1 | TH RA | $0.25 | $0.25 |
| D4 | J_TIN | JST | B3B-XH-A(LF)(SN) | 3-pin JST-XH header right-angle (DS18B20 T_in) | 1 | TH RA | $0.20 | $0.20 |
| D5 | J_TOUT | JST | B3B-XH-A(LF)(SN) | 3-pin JST-XH header right-angle (DS18B20 T_out) | 1 | TH RA | $0.20 | $0.20 |
| D6 | J_CT | JST | B2B-XH-A(LF)(SN) | 2-pin JST-XH header right-angle (SCT-013 CT) | 1 | TH RA | $0.15 | $0.15 |
| D7 | J_TOUCH | JST | B6B-XH-A(LF)(SN) | 6-pin JST-XH header right-angle (Touch SPI, optional) | 1 | TH RA | $0.35 | $0.35 |
| D8 | XH_HOUSING_4P | JST | XHP-4 | JST-XH 4-pin housing (LCD cable mate) | 1 | — | $0.10 | $0.10 |
| D9 | XH_HOUSING_3P | JST | XHP-3 | JST-XH 3-pin housing (temp probe mate) | 2 | — | $0.08 | $0.16 |
| D10 | XH_HOUSING_2P | JST | XHP-2 | JST-XH 2-pin housing (CT cable adaptor) | 1 | — | $0.06 | $0.06 |
| D11 | XH_CONTACTS | JST | SXH-001T-P0.6 | JST-XH crimp contacts (for all housings) | 20 | — | $0.03 | $0.60 |
| D12 | SW1, SW2 | E-Switch | TL3301AF160QG | Tactile button SPST 6×6 mm 160 g | 2 | TH | $0.15 | $0.30 |
| D13 | FH1 | Keystone | 3557-2 | PCB fuse clip 5×20 mm (2 clips = 1 holder) | 2 | TH | $0.30 | $0.60 |
| D14 | F1 | Littelfuse | 0215010.MXP | 5×20 mm glass fuse 10 A 250 V slow-blow | 1 | TH | $0.50 | $0.50 |
| D15 | TP_3V3,TP_5V,TP_GND,TP_ADC,TP_OW0,TP_OW1 | Keystone | 5005 | Test point loop 0.063" (pack of 10) | 6 | TH | $0.05 | $0.30 |
| | | | | **Section D Subtotal** | | | | **$8.27** |

---

## Section E: Enclosure & Assembly

| # | Ref | Manufacturer | MPN | Description | Qty | Est. Unit | Est. Total |
|---|-----|--------------|-----|-------------|-----|-----------|------------|
| E1 | — | Generic | ABS-120×80×55-IP65 | ABS IP65 enclosure 120×80×55 mm | 1 | $5.00 | $5.00 |
| E2 | — | Generic | PG9-BLACK | PG9 cable gland black Nylon (CT, temp probes) | 4 | $0.50 | $2.00 |
| E3 | — | Generic | PG11-BLACK | PG11 cable gland (AC mains) | 1 | $0.60 | $0.60 |
| E4 | H1-H4 | Keystone | 24341 | M3×10mm standoff hex brass | 4 | $0.15 | $0.60 |
| E5 | — | Generic | M3×6mm-SCREW | M3×6 mm pan head screw (PCB-to-standoff) | 4 | $0.05 | $0.20 |
| E6 | — | Generic | M3×6mm-NUT | M3 hex nut (standoff-to-enclosure) | 4 | $0.03 | $0.12 |
| | | | | **Section E Subtotal** | | | **$8.52** |

---

## Cost Summary

| Category | Sections | Est. Cost |
|----------|----------|-----------|
| Core modules | A | $45.50 |
| PCB passives | B | $0.74 |
| Semiconductors | C | $0.76 |
| Connectors & switches | D | $8.27 |
| Enclosure & hardware | E | $8.52 |
| **Total (fully assembled, modular)** | | **$63.79** |
| **PCB + components only (no modules)** | B+C+D | **$9.77** |
| **Minimum build (soldered wires, no connectors)** | A+B+C+E | **~$55** |

---

## Assembly Notes

### DNP (Do Not Populate)

| Ref | Reason |
|-----|--------|
| R3 (33 Ω burden) | Only needed for SCT-013-000 (100 A/50 mA) variant. SCT-013-030 has internal burden resistor. |
| J_TOUCH (optional) | Touch SPI breakout is a legacy interface for ILI9341 TFT. Not needed on Topway-only builds. XPT2046 is driven by Topway's onboard HMI processor, not ESP32. |

### Build Variants

| Variant | CT Sensor | Burden R3 | Notes |
|---------|-----------|-----------|-------|
| **Standard** (30 A) | SCT-013-030 | **DNP** | Internal 1 V output. Covers most machine loads up to 30 A. |
| **High-Current** (100 A) | SCT-013-000 | **33 Ω** | External burden resistor converts 50 mA to ~1.65 V. For machines drawing >30 A. |

### DS18B20 Wiring (Both Probes)

| Wire Color | Signal | Board Connector | Notes |
|------------|--------|-----------------|-------|
| Red | VCC (3.3 V) | VCC pin | Powered mode (not parasitic) |
| Yellow | DQ (Data) | DQ pin | 4.7 kΩ pull-up to 3.3 V per bus |
| Black/Bare | GND | GND pin | Common ground |

> **CRITICAL:** Each DS18B20 probe MUST go to its own dedicated bus. Do NOT connect both probes to the same connector or same GPIO. The firmware uses two separate 1-Wire buses (GPIO4 for T_in, GPIO15 for T_out).

### Topway LCD Jumper Configuration

Factory baud rate is 115200. Verify the RS232-TTL jumpers on the LCD module:
- JP1 (DE/RE): **CLOSE** (enables RS485 transceiver)
- JP2 (!RE): **OPEN**
- JP7: **CLOSE** (TX enable)
- JP8: **OPEN** (RX enable)

### Safety Assembly Order

1. Solder all low-voltage components first (passives, LEDs, buttons, connectors)
2. Solder fuse holder clips FH1
3. Insert fuse F1 into clips
4. Solder AC input screw terminal J1
5. Solder HLK-PM01 module LAST
6. Verify isolation slot is not bridged
7. Measure continuity: AC-L to DC-GND must be > 10 MΩ
8. Visual inspect AC-DC clearance ≥ 5 mm

### PCB Fabrication Notes

| Parameter | Value |
|-----------|-------|
| Layers | 2 (top/bottom) |
| Material | FR4 TG130 |
| Thickness | 1.6 mm |
| Copper | 1 oz (35 µm) |
| Surface finish | HASL Lead-Free |
| Solder mask | Green, both sides |
| Silkscreen | White, both sides |
| Min trace/space | 8 mil / 8 mil |
| PCB slot | 1.5 mm × 25 mm routed slot (AC/DC isolation) |
| Mounting holes | 4× M3 (3.2 mm drill, NPTH) |
| Antenna keep-out | 15 mm radius, no copper all layers |
| Design rule | IPC-A-600 Class 2 |

---

## Reference Documents

| Document | Path |
|----------|------|
| Hardware Deep-Dive & PCB Redesign | `./HARDWARE_DEEP_DIVE.md` |
| atopile Schematic v3 | `./hardware/esp-idms_v3.ato` |
| Original atopile Schematic v2 | `./hardware/esp-idms.ato` |
| PCB Layout (v2) | `./PCB_Layout.md` |
| Specifications (v2) | `./Specifications.md` |
| Project Definition | `./Project Definition.md` |
| Firmware README | `./README.md` |

---

*End of Bill of Materials v3*

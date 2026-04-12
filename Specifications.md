# Technical Specifications — ESP32 Industrial Device Monitoring System (ESP-IDMS)

**Document Version:** 1.0  
**Date:** April 2026  
**Status:** Approved

---

## 1. Connectivity & Network

| Parameter | Specification |
|---|---|
| **Wi-Fi Standard** | IEEE 802.11 b/g/n (2.4 GHz) |
| **Reconnect Strategy** | Automatic reconnect with exponential back-off; monitoring continues locally during outage |
| **Alert Transport** | HTTPS POST via Telegram Bot API (`https://api.telegram.org`) |
| **TLS Security** | SSL/TLS with certificate fingerprint validation |
| **Cloud Heartbeat Interval** | Every 60 seconds |
| **Max Registered Technicians** | 5 (Telegram Chat IDs stored in NVS) |

---

## 2. Monitoring Logic & Thresholds

### 2.1 Sensor Sampling

| Parameter | Value |
|---|---|
| **Sensor Polling Rate** | Every 500 ms |
| **Current RMS Window** | Computed over a rolling 1-second window (2 × 500 ms samples) |

### 2.2 Machine Power / Current Monitoring

| Parameter | Value | Notes |
|---|---|---|
| **Input Signal** | AC Current (0–30 A or 0–100 A) | Measured via SCT-013 CT sensor |
| **ADC Input** | GPIO 34 (12-bit ADC, 0–3.3 V range) | DC-biased to ≈ 1.65 V midpoint |
| **Bias Circuit** | Dual 10 kΩ voltage divider + 10 µF decoupling capacitor | Centres waveform for ESP32 ADC |
| **Burden Resistor** | 33 Ω (required for SCT-013-000 100 A variant only) | Built-in on 30 A variant |
| **Power-Loss Trigger Threshold** | RMS Current < 0.2 A – 0.5 A (configurable) | |
| **Power-Loss Debounce Time** | 5 seconds continuous below threshold | Prevents false alerts from transient loads |

### 2.3 Cooling Efficiency Monitoring

| Parameter | Value | Notes |
|---|---|---|
| **Sensor Type** | DS18B20 Waterproof Probe | 1-Wire digital, ±0.5 °C accuracy |
| **Sensor Bus** | 1-Wire (GPIO 4) | Both sensors on a single shared bus |
| **Pull-up Resistor** | 4.7 kΩ (to 3.3 V) | Required for 1-Wire bus integrity |
| **Temperature Range** | −55 °C to +125 °C | Per DS18B20 datasheet |
| **Monitored Value** | ΔT = T_out − T_in | |
| **Cooling-Failure Lower Threshold** | ΔT < 5 °C | Indicates insufficient heat transfer (cooling failure) |
| **Cooling-Failure Upper Threshold** | ΔT > 15 °C | Indicates excessive heat load (blockage or pump failure) |
| **Note** | Thresholds are machine-baseline dependent and should be calibrated on-site | |

---

## 3. Display & User Interface

| Parameter | Specification |
|---|---|
| **Display Module** | 2.8" SPI TFT with Resistive Touch |
| **Display Controller** | ILI9341 |
| **Resolution** | 320 × 240 px |
| **Interface** | SPI (4-wire) |
| **Touch Controller** | XPT2046 (or compatible) |
| **UI Refresh Rate** | Every 500 ms (synchronized with sensor polling) |

---

## 4. GPIO Pin Mapping

| Signal | GPIO | Notes |
|---|---|---|
| DS18B20 Data (1-Wire) | GPIO 4 | 4.7 kΩ pull-up to 3.3 V required |
| SCT-013 Analog Signal | GPIO 34 | ADC input only; not 5V tolerant |
| TFT SCK (SPI Clock) | GPIO 18 | Shared SPI bus |
| TFT MOSI | GPIO 23 | Shared SPI bus |
| TFT CS (Chip Select) | GPIO 15 | |
| TFT DC / RS | GPIO 2 | Data/Command select |
| TFT RST (Reset) | ESP32 EN pin | Frees GPIO 4 for temperature sensor use |

> **Note:** Tying TFT RST to the ESP32 EN pin is recommended to free GPIO 4 exclusively for the DS18B20 1-Wire bus.

---

## 5. Power & Safety

| Parameter | Specification |
|---|---|
| **Mains Input** | 100–240 V AC, 50/60 Hz |
| **AC-DC Module** | Hi-Link HLK-PM01 |
| **Output Voltage** | 5 V DC (regulated) |
| **Output Current** | 600 mA maximum |
| **Isolation** | Galvanic isolation between AC mains and DC logic |
| **ESP32 Supply** | 3.3 V (via onboard LDO on ESP32 DevKit) |
| **Backup Power** *(recommended)* | 18650 Li-ion cell + charge/protection module |
| **Enclosure Rating** | IP65 (dust-tight, water-jet resistant) |

---

## 6. NVS Data Model

| Key | Data Type | Description |
|---|---|---|
| `tech_count` | `uint8_t` | Number of registered technician IDs (0–5) |
| `tech_id_0` … `tech_id_4` | `String` | Telegram Chat IDs for each registered technician |

---

## 7. Alert Message Definitions

| Alert Type | Trigger Condition | Example Message |
|---|---|---|
| **Power Loss** | Current < threshold for ≥ 5 s | `⚠️ ALERT: Machine power loss detected. Current: 0.0A` |
| **Power Restored** | Current rises above threshold | `✅ Machine power has been restored.` |
| **Cooling Failure (Low ΔT)** | ΔT < 5 °C | `⚠️ ALERT: Cooling failure. ΔT = 2.3°C (below minimum).` |
| **Cooling Failure (High ΔT)** | ΔT > 15 °C | `⚠️ ALERT: Thermal overload. ΔT = 18.7°C (above maximum).` |
| **Cooling Restored** | ΔT returns within normal range | `✅ Cooling system has returned to normal operation.` |

---

## 8. Related Documents

| Document | Description |
|---|---|
| [`Project Definition.md`](./Project%20Definition.md) | Full project scope, objectives, and functional description |
| [`BOM.md`](./BOM.md) | Bill of Materials with part numbers and cost estimates |
| [`PCB_Layout.md`](./PCB_Layout.md) | PCB design guidelines, layer usage, and GPIO pin mapping |
| [`README.md`](./README.md) | Project overview and quick-start guide |
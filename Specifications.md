# Technical Specifications — ESP-IDMS (ESP32-S3 + Topway LCD)

**Document Version:** 2.0  
**Date:** April 2026  
**Target:** ESP32-S3 N16R8 + Topway HKT070DTA-1C

---

## 1. Connectivity & Network

| Parameter | Specification |
|-----------|---------------|
| Wi-Fi Standard | IEEE 802.11 b/g/n (2.4 GHz) |
| Reconnect Strategy | Automatic with exponential back-off; local monitoring continues during outage |
| Alert Transport | HTTPS POST via Telegram Bot API |
| TLS Security | esp-x509-certificate-bundle (Mozilla root CAs) |
| Heartbeat Interval | Every 120 s (20 min bot poll cycle) |
| Max Technicians | 5 (Telegram Chat IDs in NVS) |

---

## 2. Current Monitoring (SCT-013)

| Parameter | Value | Notes |
|-----------|-------|-------|
| Sensor | SCT-013-030 (30 A/1 V) or SCT-013-000 (100 A/50 mA) | Non-invasive split-core CT |
| ADC GPIO | GPIO 6 (ESP32-S3 ADC_UNIT_0 CH5) | 12-bit, 0–3.3 V range, attenuation 12 dB |
| ADC Bias Circuit | 2× 10 kΩ voltage divider + 10 µF capacitor | Creates 1.65 V midpoint at ADC input |
| Burden Resistor | 33 Ω (SCT-013-000 only) | Built-in on SCT-013-030 |
| Sampling | 512-sample RMS window every 500 ms | EMA filter (α=0.15) for display stability |
| Auto-Zero | 512 samples at boot | Measures DC offset, subtracted from RMS |
| Sensor Disconnect Detect | ADC mean < 15 or > 4080 counts | Marks current_valid = false |
| Calibration Factor | 300 A/V (CONFIG_IDMS_CT_AMPS_PER_VOLT_X100=30000) | Configurable in menuconfig |
| Power-Loss Threshold | < 350 mA for ≥ 5 s | Configurable (CONFIG_IDMS_CURRENT_THRESHOLD_MA) |

### 2.1 SCT-013 Bias Circuit Schematic

```
                    3.3V
                     |
                   [10kΩ]
                     |
    SCT-013  ───────┤──────── GPIO6 (ADC)
    output    │      │
             [33Ω]  │
             burden  │
               │   [10µF]
               │     │
              GND   GND

    (33Ω burden only needed for SCT-013-000 100A variant)
```

The voltage divider (2× 10 kΩ) creates a stable 1.65 V DC bias. The SCT-013 AC output rides on top of this bias, allowing the ADC to sample the full AC waveform within the 0–3.3 V range.

---

## 3. Temperature Monitoring (DS18B20)

| Parameter | Value | Notes |
|-----------|-------|-------|
| Sensor Type | DS18B20 waterproof probe | 1-Wire digital, ±0.5 °C accuracy |
| T_in Bus | GPIO 4 | Inlet temperature, separate bus |
| T_out Bus | GPIO 15 | Outlet temperature, separate bus |
| Pull-up Resistor | 4.7 kΩ to 3.3 V | Required on each 1-Wire bus |
| Temperature Range | −55 °C to +125 °C | Per DS18B20 datasheet |
| Conversion Time | ~750 ms (12-bit, pipelined) | Read + request-next alternated |
| CRC Validation | 8-bit CRC checked on every read | Invalid readings are discarded |

### 3.1 Cooling Thresholds

| Parameter | Value | Config Key |
|-----------|-------|------------|
| ΔT Low | 5 °C | `IDMS_DT_LOW_C` |
| ΔT High | 15 °C | `IDMS_DT_HIGH_C` |
| Fault Debounce | 5 s continuous | Hard-coded |

ΔT = T_out − T_in. Values outside [5 °C, 15 °C] for ≥ 5 s trigger a cooling fault alert.

---

## 4. Display (Topway HKT070DTA-1C)

| Parameter | Specification |
|-----------|---------------|
| Display Module | Topway HKT070DTA-1C smart LCD |
| Resolution | 800 × 480 px |
| Interface | UART (TTL 3.3 V) |
| Baud Rate | 115200 (8N1) |
| TX Pin | GPIO 1 |
| RX Pin | GPIO 3 |
| RTS Pin | Not used (−1) |
| Protocol | Topway DGUS-style: `AA [cmd] [3-byte-addr] [data] CC 33 C3 3C` |
| Handshake | 5 retries at boot; non-fatal if display not connected |

### 4.1 Topway VP Address Map

| VP Address | Type | Content |
|------------|------|---------|
| 0x000000 | String | Current text ("--" if invalid) |
| 0x000200 | String | Wi-Fi IP address |
| 0x000280 | String | Firmware version |
| 0x000300 | String | OTA status |
| 0x000380 | String | ACTIVE / INACTIVE status |
| 0x000500 | String | Diagnostic detail |
| 0x000600 | String | ERROR status |
| 0x000700 | String | WARNING status |
| 0x080000 | N16 | Current × 10 (A × 10) |
| 0x080002 | N16 | T_in × 10 (°C × 10) |
| 0x080004 | N16 | T_out × 10 (°C × 10) |
| 0x080006 | N16 | ΔT × 10 (signed, °C × 10) |
| 0x080010–0x080020 | N16 | Valid flags and status indicators |

---

## 5. GPIO Pin Mapping (ESP32-S3)

| Signal | GPIO | Direction | Notes |
|--------|------|-----------|-------|
| Console TX | GPIO 44 | Output | USB-Serial (fixed) |
| Console RX | GPIO 43 | Input | USB-Serial (fixed) |
| Topway UART TX | GPIO 1 | Output | UART1 to LCD |
| Topway UART RX | GPIO 3 | Input | UART1 from LCD |
| DS18B20 T_in | GPIO 4 | Bidirectional | 1-Wire bus 0, 4.7 kΩ pull-up |
| DS18B20 T_out | GPIO 15 | Bidirectional | 1-Wire bus 1, 4.7 kΩ pull-up |
| SCT-013 ADC | GPIO 6 | Input (ADC) | ADC_UNIT_0 channel 5, bias circuit required |
| Touch SCLK | GPIO 12 | Output | XPT2046 SPI clock |
| Touch MOSI | GPIO 13 | Output | XPT2046 SPI data out |
| Touch MISO | GPIO 16 | Input | XPT2046 SPI data in |
| Touch CS | GPIO 11 | Output | XPT2046 chip select |
| Touch IRQ | GPIO 14 | Input | XPT2046 pen-down interrupt |

> **Note:** All GPIO assignments (except console UART) are configurable via `idf.py menuconfig`.

---

## 6. NVS Data Model

| Key | Type | Description |
|-----|------|-------------|
| `tech_count` | uint8 | Number of registered technician IDs (0–5) |
| `tech_id_0`…`tech_id_4` | string | Telegram Chat IDs |
| `wifi_ssid` | string | Wi-Fi SSID (overrides Kconfig default) |
| `wifi_password` | string | Wi-Fi password (overrides Kconfig default) |
| `telegram_token` | string | Bot token (overrides Kconfig default) |
| `ota_user` | string | OTA HTTP auth username |
| `ota_password` | string | OTA HTTP auth password |

---

## 7. Alert Message Definitions

| Alert Type | Trigger | Example Message |
|------------|---------|-----------------|
| **Power Loss** | Current < 350 mA for ≥ 5 s | ⚠️ ALERT: Machine power loss detected. Current: 0.0A |
| **Power Restored** | Current rises above threshold | ✅ Machine power has been RESTORED. |
| **Cooling Failure (Low ΔT)** | ΔT < 5 °C for ≥ 5 s | ⚠️ ALERT: Cooling failure. ΔT = 2.3°C (below minimum). |
| **Cooling Failure (High ΔT)** | ΔT > 15 °C for ≥ 5 s | ⚠️ ALERT: Thermal overload. ΔT = 18.7°C (above maximum). |
| **Cooling Restored** | ΔT returns to normal | ✅ Cooling system has returned to NORMAL. |

---

## 8. Status Indication on Display

| Condition | 0x000600 | 0x000700 | 0x000380 | 0x000500 |
|-----------|----------|----------|----------|----------|
| All OK, current ≥ 0.5 A | — | — | ACTIVE | OK |
| Standby, current < 0.5 A | — | — | INACTIVE | Standby |
| Current sensor disconnected | — | WARNING | — | Current Sensor |
| Temp sensor invalid | — | WARNING | — | Temp Sensor |
| Power loss | ERROR | — | — | Power Loss |
| Cooling fault | ERROR | — | — | Cooling Fault |
| Both faults | ERROR | — | — | Power + Cooling |
| All sensors offline | ERROR | — | — | Sensors Offline |

---

## 9. Related Documents

| Document | Description |
|----------|-------------|
| [README.md](./README.md) | Project overview, pin map, console commands |
| [PCB_Layout.md](./PCB_Layout.md) | PCB design, schematic, component placement |
| [BOM.md](./BOM.md) | Bill of Materials |
| [Project Definition.md](./Project%20Definition.md) | Project scope and objectives |
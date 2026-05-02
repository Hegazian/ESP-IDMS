# Project Definition — ESP-IDMS

**Document Version:** 2.0  
**Date:** April 2026  
**Status:** Active Development

---

## 1. Project Overview

ESP-IDMS (ESP-IDF Industrial Device Monitoring System) is firmware for ESP32-S3 that monitors industrial machine status through non-invasive current sensing (SCT-013) and cooling efficiency (DS18B20 ΔT), displays real-time data on a Topway HKT070DTA-1C smart LCD, and sends Telegram alerts when fault conditions are detected.

## 2. Objectives

1. **Current monitoring** — Detect machine power loss when AC current falls below a configurable threshold for 5+ seconds
2. **Cooling monitoring** — Detect cooling system failure when ΔT (T_out − T_in) falls outside a configurable range for 5+ seconds
3. **Remote alerts** — Send instant Telegram notifications to registered technicians on fault detection and restoration
4. **Local display** — Show real-time current, temperatures, ΔT, status, and diagnostics on a 7-inch smart LCD
5. **OTA updates** — Allow firmware updates over Wi-Fi via HTTP upload with rollback protection
6. **NVS persistence** — Store Wi-Fi credentials, Telegram token, and technician IDs in non-volatile storage

## 3. Design Philosophy

| Decision | Rationale |
|----------|-----------|
| Wi-Fi over GSM | Lower cost, higher bandwidth, existing infrastructure |
| Telegram Bot API | No custom server required, end-to-end encryption, group notification |
| DS18B20 1-Wire | Digital accuracy (±0.5 °C), single-wire interface, waterproof probes |
| SCT-013 CT | Non-invasive, safe, split-core for easy installation |
| Topway smart LCD | Built-in HMI controller, UART protocol, no SPI overhead on ESP32 |
| NVS secrets | Avoids plaintext credentials in firmware binary |
| Dual 1-Wire buses | Separate buses for T_in and T_out for reliability |

## 4. Functional Modules

| Module | Source Files | Description |
|--------|-------------|-------------|
| Monitor | `monitor/monitor.c` | Main 500 ms polling loop: ADC RMS current, DS18B20 temperature, fault detection |
| DS18B20 | `drivers/ds18b20.c`, `drivers/onewire.c` | 1-Wire bus driver, temperature conversion with CRC validation |
| Topway LCD | `drivers/topway_lcd.c` | UART protocol driver for Topway smart LCD |
| UI | `ui/ui_topway.c` | Display update logic, status/diagnostic routing, VP address management |
| Wi-Fi | `wifi/wifi_manager.c` | STA mode, auto-reconnect, IP acquisition |
| Telegram | `telegram/tg_bot.c`, `tg_http.c`, `tg_send.c`, `tg_parse.c`, `tg_ui.c` | Bot polling, HTTPS client, message sending, JSON parsing |
| Config Store | `core/config_store.c` | NVS secrets: Wi-Fi SSID/password, Telegram token, OTA credentials |
| OTA | `ota/ota.c` | HTTP firmware upload server on port 8080, basic auth, SHA256 verification |
| Console | `cli/serial_console.c` | Serial command interface for configuration and debugging |

## 5. Alert Behavior

| Condition | Trigger | Action |
|-----------|---------|--------|
| Power loss | Current < threshold for ≥ 5 s | Telegram alert to all technicians |
| Power restored | Current rises above threshold | Telegram restoration notice |
| Cooling fault (low ΔT) | ΔT < 5 °C for ≥ 5 s | Telegram alert |
| Cooling fault (high ΔT) | ΔT > 15 °C for ≥ 5 s | Telegram alert |
| Cooling restored | ΔT returns to normal | Telegram restoration notice |
| Wi-Fi outage | Connection lost | Local monitoring continues; alerts queued |
| Wi-Fi restored | Connection re-established | Queued alerts sent |

## 6. Display Pages

The Topway HKT070DTA-1C display shows (Page 0):

- **Current (A)** — N16 value at VP 0x080000; text "--" at VP 0x000000 when invalid
- **T_in (°C)** — N16 at VP 0x080002; valid flag at 0x080012
- **T_out (°C)** — N16 at VP 0x080004; valid flag at 0x080014
- **ΔT (°C)** — N16 at VP 0x080006 (signed); valid flag at 0x080016
- **Wi-Fi IP** — String at VP 0x000200
- **OTA status** — String at VP 0x000300
- **Firmware version** — String at VP 0x000280
- **Status** — ACTIVE/INACTIVE at VP 0x000380, ERROR at 0x000600, WARNING at 0x000700
- **Diagnostic** — String at VP 0x000500

## 7. Out of Scope

- Direct SCADA/Modbus integration (may be added later)
- Local data logging / historical trends (SD card or flash)
- Wi-Fi AP mode (STA only)
- Bluetooth configuration
- LCD touch input handling (display is output-only in current firmware)

---

## Related Documents

| Document | Description |
|----------|-------------|
| [README.md](./README.md) | Project overview, pin map, console commands |
| [Specifications.md](./Specifications.md) | Electrical specs, thresholds, GPIO mapping |
| [PCB_Layout.md](./PCB_Layout.md) | PCB design, schematic, component placement |
| [BOM.md](./BOM.md) | Bill of Materials |
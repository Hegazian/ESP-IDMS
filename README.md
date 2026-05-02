# ESP-IDMS — ESP-IDF Industrial Device Monitoring System

Firmware for the ESP32-S3 Industrial Device Monitoring System. Monitors machine current (SCT-013 CT sensor), cooling efficiency (DS18B20 ΔT), displays live data on a Topway HKT070DTA-1C smart LCD, and sends Telegram alerts on fault conditions. Supports OTA firmware updates.

## Supported Hardware

| Target | Display | Notes |
|--------|---------|-------|
| **ESP32-S3 N16R8** | Topway HKT070DTA-1C (UART 115200) | Production target — 16 MB flash + 8 MB PSRAM |
| **ESP32-S3 N8R8** | Topway HKT070DTA-1C | 8 MB flash variant |
| **ESP32** | ILI9341 SPI TFT (legacy) | Original design, not actively maintained |

## Current Pin Assignments (ESP32-S3 + Topway)

| Signal | GPIO | Direction | Notes |
|--------|------|-----------|-------|
| Console UART TX | GPIO 44 | Output | USB-Serial (do not reassign) |
| Console UART RX | GPIO 43 | Input | USB-Serial (do not reassign) |
| Topway LCD TX | GPIO 1 | Output | UART1 to smart LCD |
| Topway LCD RX | GPIO 3 | Input | UART1 from smart LCD |
| DS18B20 T_in (1-Wire) | GPIO 4 | Bidirectional | 4.7 kΩ pull-up to 3.3 V |
| DS18B20 T_out (1-Wire) | GPIO 15 | Bidirectional | 4.7 kΩ pull-up to 3.3 V |
| SCT-013 ADC | GPIO 6 | Input (ADC) | ADC_UNIT_0 CH5, DC-biased via 2×10 kΩ divider |
| Touch SPI SCLK | GPIO 12 | Output | XPT2046 resistive touch |
| Touch SPI MOSI | GPIO 13 | Output | XPT2046 |
| Touch SPI MISO | GPIO 16 | Input | XPT2046 |
| Touch CS | GPIO 11 | Output | XPT2046 chip select |
| Touch IRQ | GPIO 14 | Input | XPT2046 pen-down interrupt |

All GPIO assignments are configurable via `idf.py menuconfig` → ESP-IDMS Configuration.

## Requirements

- ESP-IDF **v5.5.x** (v5.5.4 used in development)
- ESP32-S3 DevKit with 16 MB flash + 8 MB PSRAM (recommended)

## Build and Flash

```bash
idf.py set-target esp32s3
idf.py menuconfig    # Set Wi-Fi, GPIOs, thresholds
idf.py build
idf.py -p COMx flash monitor
```

## Serial Console Commands

| Command | Description |
|---------|-------------|
| `status` | Show all sensor readings, Wi-Fi, firmware version |
| `adc` | Read 64 raw ADC samples (diagnose current sensor) |
| `topway_str <hex> <text>` | Write string to Topway VP address |
| `topway_test` | Write test values to Topway display |
| `add <chat_id>` | Add Telegram technician chat ID |
| `list` | List registered technician IDs |
| `remove <n>` | Remove technician by index |
| `set_ssid <ssid>` | Set Wi-Fi SSID |
| `set_pass <pass>` | Set Wi-Fi password |
| `set_token <tok>` | Set Telegram bot token |
| `show_token` | Show bot token (first/last chars only) |
| `set_ota_user <u>` | Set OTA HTTP username |
| `set_ota_pass <p>` | Set OTA HTTP password |
| `show_secrets` | Show secret status (values hidden) |
| `reboot` | Reboot device |

## OTA Firmware Updates

OTA update server runs on port 8080 with basic auth. See [OTA_IMPLEMENTATION.md](./OTA_IMPLEMENTATION.md) for details.

## Thresholds (configurable in menuconfig)

| Parameter | Default | Config Key |
|-----------|---------|------------|
| Power loss current threshold | 350 mA | `IDMS_CURRENT_THRESHOLD_MA` |
| CT calibration (A/V × 100) | 30000 | `IDMS_CT_AMPS_PER_VOLT_X100` |
| Auto-zero samples | 512 | `IDMS_SCT_AUTOZERO_SAMPLES` |
| Cooling ΔT low threshold | 5 °C | `IDMS_DT_LOW_C` |
| Cooling ΔT high threshold | 15 °C | `IDMS_DT_HIGH_C` |
| Fault debounce time | 5 s | Hard-coded |

## Display Protocol (Topway HKT070DTA-1C)

Communication via UART1 at 115200 baud using the Topway DGUS-style protocol:

| VP Address | Type | Content |
|------------|------|---------|
| 0x000000 | String | Current display text ("--" if invalid) |
| 0x000200 | String | Wi-Fi IP address |
| 0x000280 | String | Firmware version |
| 0x000300 | String | OTA status ("ready" / "updating" / "error") |
| 0x000380 | String | Device status (ACTIVE / INACTIVE) |
| 0x000500 | String | Diagnostic detail |
| 0x000600 | String | Error status (ERROR) |
| 0x000700 | String | Warning status (WARNING) |
| 0x080000 | N16 | Current × 10 (A×10) |
| 0x080002 | N16 | T_in × 10 (°C×10) |
| 0x080004 | N16 | T_out × 10 (°C×10) |
| 0x080006 | N16 | ΔT × 10 (°C×10, signed) |
| 0x080010 | N16 | Current valid flag |
| 0x080012 | N16 | T_in valid flag |
| 0x080014 | N16 | T_out valid flag |
| 0x080016 | N16 | ΔT valid flag |
| 0x080018 | N16 | Wi-Fi status (0=offline, 1=connected) |
| 0x08001A | N16 | OTA status (0=ready, 1=updating, 2=error) |
| 0x08001C | N16 | Technician count |
| 0x08001E | N16 | Power fault flag |
| 0x080020 | N16 | Cooling fault flag |

Protocol format: `AA [cmd] [3-byte addr] [data] [0x00 for strings] CC 33 C3 3C`

## Alert Logic

| Condition | Status | Diagnostic | Telegram Alert |
|-----------|--------|-----------|----------------|
| Current < threshold for ≥ 5 s | ERROR | Power Loss | Machine power loss |
| Cooling ΔT outside range for ≥ 5 s | ERROR | Cooling Fault / Power + Cooling | Cooling alert |
| Current sensor disconnected (ADC stuck) | WARNING | Current Sensor | — |
| Temperature sensor invalid | WARNING | Temp Sensor | — |
| All sensors disconnected | ERROR | Sensors Offline | — |
| Current ≥ threshold, cooling OK | ACTIVE | OK | — |
| Current < 0.5 A, cooling OK | INACTIVE | Standby | — |
| Power restores after fault | — | — | Machine restored |

## Related Documents

| Document | Description |
|----------|-------------|
| [Specifications.md](./Specifications.md) | Electrical specs, thresholds, GPIO mapping |
| [PCB_Layout.md](./PCB_Layout.md) | PCB design, schematic, component placement |
| [BOM.md](./BOM.md) | Bill of Materials |
| [Project Definition.md](./Project%20Definition.md) | Project scope and objectives |
| [OTA_IMPLEMENTATION.md](./OTA_IMPLEMENTATION.md) | OTA update implementation details |
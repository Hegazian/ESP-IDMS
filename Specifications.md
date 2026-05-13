# Technical Specifications - ESP-IDMS

Document version: 3.0
Last updated: 2026-05-10
Target: ESP32-S3 N16R8 + Topway HKT070DTA-1C

## 1. System Overview

| Area | Specification |
|------|---------------|
| MCU | ESP32-S3, 16 MB flash, 8 MB PSRAM |
| Framework | ESP-IDF v5.5.x |
| Display | Topway HKT070DTA-1C, 800x480, UART smart LCD |
| Current sensor | SCT-013 voltage-output CT by default |
| Temperature sensors | Two DS18B20 waterproof probes |
| Connectivity | Wi-Fi STA, SNTP |
| Alerting | Telegram Bot API over HTTPS |
| OTA | HTTPS server on port 8080 |
| Storage | NVS for config/secrets, SPIFFS for telemetry CSV |
| Cloud | Generic HTTPS POST endpoint with Bearer token |

## 2. GPIO Map

| Signal | GPIO | Direction | Notes |
|--------|------|-----------|-------|
| Console UART TX | 44 | Output | USB serial console |
| Console UART RX | 43 | Input | USB serial console |
| Topway UART TX | 1 | Output | ESP32 to LCD |
| Topway UART RX | 3 | Input | LCD to ESP32 |
| SCT-013 ADC | 6 | Analog input | ADC unit 0, channel 5 |
| DS18B20 T_in | 4 | Bidirectional | 1-Wire bus 0 |
| DS18B20 T_out | 15 | Bidirectional | 1-Wire bus 1 |
| Touch SCLK | 12 | Output | Optional/legacy |
| Touch MOSI | 13 | Output | Optional/legacy |
| Touch MISO | 16 | Input | Optional/legacy |
| Touch CS | 11 | Output | Optional/legacy |
| Touch IRQ | 14 | Input | Optional/legacy |

## 3. Current Sensor

| Parameter | Current value |
|-----------|---------------|
| ADC pin | GPIO6 |
| ADC bias expected | About 1.65 V at TP_ADC |
| Bias circuit | 2 x 10 kOhm divider plus 10 uF capacitor |
| Sampling | 512-sample RMS window |
| Display smoothing | EMA filter |
| Default scale | 30.00 A/V |
| Scale storage | NVS `curr_cal`, fixed-point x100 |
| Auto-zero samples | 512 |
| Auto-zero guard | Skip RMS subtraction when no-load RMS is above 50 mV |
| Power-loss threshold | NVS `power_ma`, default 350 mA |
| Machine-running threshold | NVS `run_ma`, default 1000 mA |

### SCT Variants

| Sensor type | Hardware | Firmware calibration |
|-------------|----------|----------------------|
| SCT-013 30A/1V | Internal burden | `set_current_cal 30.00` |
| SCT-013 100A/1V | Internal burden | `set_current_cal 100.00` |
| SCT-013-000 100A/50mA | External burden required | Calibrate with known load |

The CT wire polarity does not change RMS current magnitude. It only changes
waveform phase, which this firmware does not use.

## 4. Temperature Sensors

| Parameter | Current value |
|-----------|---------------|
| Sensor | DS18B20 waterproof probe |
| T_in GPIO | GPIO4 |
| T_out GPIO | GPIO15 |
| Pull-up | 4.7 kOhm to 3.3 V on each bus |
| Mode | Powered mode, not parasite power |
| CRC | Checked on every scratchpad read |
| Conversion | Pipelined read/request cycle |

Delta T is calculated as:

```text
Delta T = T_out - T_in
```

## 5. Fault Logic

| Fault | Trigger | Restore |
|-------|---------|---------|
| Power loss | Current below `power_ma` for 5 seconds | Current rises above threshold |
| Cooling low | Machine running and Delta T below `dt_alert` for 5 seconds | Delta T returns to allowed range |
| Cooling high | Machine running and Delta T above `dt_high` for 5 seconds | Delta T returns to allowed range |
| Current sensor | ADC missing, ADC read error, or bias outside range | Valid ADC readings return |
| Temperature sensor | DS18B20 missing/CRC/read failures | Valid temperature readings return |

## 6. Runtime Thresholds

| Name | Default | Storage | Console command |
|------|---------|---------|-----------------|
| Power-loss current | 0.350 A | `power_ma` | `set_power_threshold <A>` |
| Machine-running current | 1.000 A | `run_ma` | `set_running_threshold <A>` |
| Display current min | 1 A | `min_curr` | Topway config page |
| Display current max | 20 A | `max_curr` | Topway config page |
| T_in min | -10 C | `min_tin` | Topway config page |
| T_in max | 0 C | `max_tin` | Topway config page |
| T_out min | 0 C | `min_tout` | Topway config page |
| T_out max | 55 C | `max_tout` | Topway config page |
| Delta T low | 5 C | `dt_alert` | Topway config page |
| Delta T high | 15 C | `dt_high` | `set_dt_high <C>` |
| Temp In offset | 0.0 C | `tin_off_x10` | Topway settings calibration |
| Temp Out offset | 0.0 C | `tout_off_x10` | Topway settings calibration |

## 7. Topway VP Map

### Home/status values

| VP | Type | Meaning |
|----|------|---------|
| `0x080000` | N16 | Current, A |
| `0x080002` | N16 | T_in, C |
| `0x080004` | N16 | T_out, C |
| `0x000800` | String | ESP RTC datetime, Cairo local `YYYY-MM-DD HH:MM` |
| `0x080006` | N16 | Device status text color, RGB565 |
| `0x000380` | String | Device status text |
| `0x000500` | String | Diagnostic detail |

### Settings page

| VP | Type | Meaning |
|----|------|---------|
| `0x000400` | String | Wi-Fi status message |
| `0x000900` | String | Wi-Fi SSID input |
| `0x000080` | String | Wi-Fi password input |
| `0x080020` | N16 | Wi-Fi connect button |
| `0x080050` | N16 | Current calibration scale, A/V |
| `0x080052` | N16 | Temp In offset, signed C |
| `0x080054` | N16 | Temp Out offset, signed C |
| `0x080056` | N16 | Current zero button |
| `0x080058` | N16 | Calibration apply button |
| `0x08005A` | N16 | Calibration save button |
| `0x000700` | String | Calibration status message |

### Configuration page

| VP | Type | Meaning |
|----|------|---------|
| `0x080030` | N16 | T_in min |
| `0x080036` | N16 | T_in max |
| `0x080032` | N16 | T_out min |
| `0x080038` | N16 | T_out max |
| `0x080034` | N16 | Current min |
| `0x08003A` | N16 | Current max |
| `0x080024` | N16 | Delta T low threshold |
| `0x08003C` | N16 | Apply button |
| `0x000600` | String | Validation message below Apply |

### Telegram page

| VP | Type | Meaning |
|----|------|---------|
| `0x000280` | String | Telegram bot QR URL |
| `0x000000-BUFF` | String | Technician number input |
| `0x000C80` | String | Bot status message |
| `0x000D00` | String | Authorized technician row 1 |
| `0x000D80` | String | Authorized technician row 2 |
| `0x000E00` | String | Authorized technician row 3 |
| `0x000E80` | String | Authorized technician row 4 |
| `0x000F00` | String | Authorized technician row 5 |
| `0x080060` | N16 | Authorize technician button |
| `0x080026` | N16 | Delete row 1 button |
| `0x080028` | N16 | Delete row 2 button |
| `0x08002A` | N16 | Delete row 3 button |
| `0x08002C` | N16 | Delete row 4 button |
| `0x08002E` | N16 | Delete row 5 button |

Default QR payload: `https://t.me/IDMS_USERBOT`. The bot task refreshes it
from Telegram `getMe` when networking is ready.

The technician input is a local Egyptian mobile number. Firmware accepts
`01024912688`, `1024912688`, `+201024912688`, or `+20 01024912688` and stores
the canonical `+201024912688`. The slot remains pending until the technician
shares their own Telegram contact with the bot, after which the real Telegram
ID is bound to the phone slot.

Technician identity is one-to-one: one phone number maps to one Telegram ID,
and one Telegram ID maps to one phone number. Display rows show phone numbers,
not chat IDs.

## 8. NVS Namespaces And Important Keys

| Namespace | Keys |
|-----------|------|
| `idms` | technicians, thresholds, calibration, Telegram QR |
| `secrets` | Wi-Fi, Telegram, OTA, cloud URL/token |
| `telemetry` | weekly report state |
| `cloud` | uploaded CSV offset |
| `tg_bot` | Telegram update offset |

Important keys:

| Key | Meaning |
|-----|---------|
| `wifi_ssid`, `wifi_pass` | Wi-Fi credentials |
| `tg_token` | Telegram bot token |
| `ota_user`, `ota_pass` | OTA Basic Auth |
| `cloud_url`, `cloud_token` | Cloud upload endpoint and token |
| `tech_count`, `tech_id_0..4` | Telegram technicians |
| `curr_cal` | Current calibration A/V x100 |
| `tin_off_x10`, `tout_off_x10` | Temperature offsets in C x10 |
| `power_ma` | Power-loss threshold in mA |
| `run_ma` | Machine-running threshold in mA |
| `qr_code` | Telegram bot QR URL payload |

Automatic NVS erase is disabled. If NVS cannot mount because of no free pages or
version mismatch, the firmware returns an error instead of wiping provisioned
data.

## 9. OTA

| Parameter | Value |
|-----------|-------|
| Server port | 8080 |
| HTTPS | Enabled by default |
| Auth methods | Basic Auth and short-lived token |
| Upload form | Browser multipart upload |
| Verification | `esp_ota_end`, optional client SHA256 header |
| Rollback | Health validation before marking valid |
| Health gate | Telemetry ready, UI ready, Telegram task created, sensors valid |

## 10. Telemetry

| Parameter | Value |
|-----------|-------|
| Local path | `/spiffs/telemetry.csv` |
| Rotation | 512 KiB to `/spiffs/telemetry.old.csv` |
| Sample period | 60 seconds |
| Cloud upload interval | 300 seconds by default |
| Cloud payload | JSON with CSV rows |
| Weekly report | Monday 09:00 UTC if time is synced |

## 11. Production Security

Use `sdkconfig.defaults.production` as the release baseline.

Required production controls:

- `CONFIG_IDMS_PRODUCTION_BUILD=y`
- `CONFIG_SECURE_BOOT=y`
- `CONFIG_SECURE_FLASH_ENC_ENABLED=y`
- `CONFIG_NVS_ENCRYPTION=y`
- Empty compiled-in secrets
- Runtime/manufacturing provisioning

Secure boot and flash encryption are manufacturing steps. Test on sacrificial
hardware before enabling irreversible eFuse settings on production units.

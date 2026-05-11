# ESP-IDMS

ESP-IDMS is an ESP-IDF firmware project for an ESP32-S3 industrial device
monitor. It measures machine current, inlet/outlet temperature, cooling
performance, local display status, Telegram alerts, OTA updates, and telemetry
history.

Current phase status: **firmware feature-complete for pilot/production
hardening**. The next phase should focus on field validation, production
security provisioning, enclosure/PCB finalization, and cloud dashboard/export.

Last updated: 2026-05-10

## What The Device Does

- Measures AC current with an SCT-013 current transformer on ESP32-S3 ADC.
- Measures T_in and T_out with DS18B20 probes on two separate 1-Wire buses.
- Calculates Delta T as `T_out - T_in`.
- Detects power loss when current stays below the power-loss threshold.
- Detects cooling faults when Delta T is outside the configured allowed range.
- Shows live readings and status on a Topway HKT070DTA-1C 800x480 smart LCD.
- Lets operators edit Wi-Fi, threshold, and device info fields from Topway.
- Sends Telegram alerts, reminders, status, OTA links, and weekly reports.
- Supports OTA firmware upload with HTTPS, auth, SHA256, and rollback health
  validation.
- Stores telemetry locally as CSV and can upload pending rows to a cloud HTTPS
  endpoint.

## Current Production Notes

The active development build is intentionally not a locked production image.
For production, use `sdkconfig.defaults.production` as the security baseline and
complete the ESP-IDF secure provisioning flow.

Production must enable:

- Secure boot
- Flash encryption
- NVS encryption
- Empty compiled-in secrets
- Runtime provisioning for Wi-Fi, Telegram, OTA credentials, and cloud token
- Strong OTA credentials or one-time token flow

Do not ship devices with secrets compiled into `sdkconfig`.

## Hardware Target

| Item | Current target |
|------|----------------|
| MCU | ESP32-S3 N16R8, 16 MB flash, 8 MB PSRAM |
| Display | Topway HKT070DTA-1C smart LCD |
| Current sensor | SCT-013 voltage-output CT, default calibration 30.00 A/V |
| Temperature sensors | Two DS18B20 waterproof probes |
| Network | Wi-Fi STA mode |
| Alert channel | Telegram Bot API over HTTPS |
| OTA | HTTPS server on port 8080 |
| Telemetry | Local SPIFFS CSV plus optional HTTPS cloud upload |

## ESP32-S3 Pin Map

| Signal | GPIO | Notes |
|--------|------|-------|
| Console UART TX | GPIO44 | USB serial console |
| Console UART RX | GPIO43 | USB serial console |
| Topway UART TX | GPIO1 | ESP32 to LCD |
| Topway UART RX | GPIO3 | LCD to ESP32 |
| T_in DS18B20 | GPIO4 | 4.7 kOhm pull-up to 3.3 V |
| T_out DS18B20 | GPIO15 | 4.7 kOhm pull-up to 3.3 V |
| SCT-013 ADC | GPIO6 | ADC unit 0, channel 5, biased near mid-scale |
| Touch SPI SCLK | GPIO12 | Optional/legacy Topway touch breakout |
| Touch SPI MOSI | GPIO13 | Optional/legacy |
| Touch SPI MISO | GPIO16 | Optional/legacy |
| Touch CS | GPIO11 | Optional/legacy |
| Touch IRQ | GPIO14 | Optional/legacy |

## Build

Development environment used:

- ESP-IDF v5.5.4
- Target: `esp32s3`
- Build system: Ninja

Typical commands:

```powershell
idf.py set-target esp32s3
idf.py menuconfig
idf.py build
idf.py -p COM5 flash monitor
```

If `idf.py` is not on PATH, use the ESP-IDF shell or the full ESP-IDF Python
command for your installation.

Last verified build:

```powershell
C:\Espressif\tools\ninja\1.12.1\ninja.exe -C build
```

Result: `build/esp_idms.bin` generated successfully.

## First Boot Provisioning

Use the serial console to set runtime secrets and site-specific data:

```text
set_ssid YOUR_WIFI_NAME
set_pass YOUR_WIFI_PASSWORD
set_token YOUR_TELEGRAM_BOT_TOKEN
set_ota_user admin
set_ota_pass STRONG_PASSWORD
set_cloud_url https://YOUR_ENDPOINT/ingest
set_cloud_token YOUR_RANDOM_CLOUD_TOKEN
reboot
```

Use `show_secrets` to confirm which secrets are configured without printing the
secret values.

Telegram bot login is configured once from Telegram on a blank device:

1. Set Wi-Fi and the bot token, then reboot.
2. Open the bot in a private Telegram chat and send `/start`.
3. If no technicians are saved yet, the bot starts a setup wizard for the shared
   admin name and password and registers that Telegram account as the first
   technician.
4. Share the same bot URL with other technicians. Each technician enters the
   shared admin name and password once; after that, the ESP stores their
   Telegram ID and does not ask again.

After that first Telegram setup, the shared admin name and password cannot be
changed from Telegram. Use serial console commands `set_bot_admin` and
`set_bot_password` to change them.

## Serial Console Commands

| Command | Purpose |
|---------|---------|
| `status` | Show firmware, Wi-Fi, readings, faults, thresholds, and technicians |
| `adc` | Show raw ADC mean/RMS diagnostics for the current sensor |
| `cal_zero` | No-load current zero calibration |
| `cal_all` | Sensor self-test plus safe automatic calibration |
| `cal_current <A>` | Calibrate current scale using a known load |
| `set_current_cal <A/V>` | Set current calibration scale directly |
| `set_power_threshold <A>` | Current below this means power loss |
| `set_running_threshold <A>` | Current above this enables cooling alerts |
| `set_dt_high <C>` | Set high Delta T threshold |
| `set_ssid <ssid>` | Save Wi-Fi SSID to NVS |
| `set_pass <password>` | Save Wi-Fi password to NVS |
| `set_token <token>` | Save Telegram bot token to NVS |
| `set_bot_admin <name>` | Save shared Telegram bot login admin name |
| `set_bot_password <password>` | Save shared Telegram bot login password hash |
| `set_ota_user <user>` | Save OTA username to NVS |
| `set_ota_pass <password>` | Save OTA password to NVS |
| `set_cloud_url <url>` | Save telemetry upload endpoint |
| `set_cloud_token <token>` | Save telemetry Bearer token |
| `add <chat_id> [name]` | Add authorized Telegram technician manually |
| `list` | List Telegram technicians and names |
| `remove <index>` | Remove one technician |
| `clear` | Remove all technicians |
| `show_secrets` | Show secret status with values hidden |
| `topway_test` | Write test values to the Topway display |
| `topway_str <hex_addr> <text>` | Write a string to a Topway VP |
| `reboot` | Reboot the device |
| `help` | Show commands |

## Topway Pages And VP Contract

The Topway project is under:

```text
topway_display/Display_Topway/Display_Topway
```

### Home Page

| Field | VP |
|-------|----|
| Current_Value | `0x080000` |
| Temp_In | `0x080002` |
| Temp_Out | `0x080004` |
| Device Status | `0x000700`, `0x000600`, `0x000380` |
| Diagnostic info | `0x000500` |

### Settings Page

| Field | VP |
|-------|----|
| Wi-Fi state | `0x000400` |
| Wi-Fi SSID | `0x000900` |
| Wi-Fi password | `0x000080` |
| Wi-Fi connect button | `0x080020` |

### Configuration Page

| Field | VP |
|-------|----|
| Temp_In min | `0x080030` |
| Temp_In max | `0x080036` |
| Temp_Out min | `0x080032` |
| Temp_Out max | `0x080038` |
| Current min | `0x080034` |
| Current max | `0x08003A` |
| Delta_T | `0x080024` |
| Apply button | `0x08003C` |

### Info Page

| Field | VP |
|-------|----|
| Device Model | `0x000980` |
| Serial Number | `0x000B00` |
| Manufacture Date | `0x000B80` |
| Support Email | `0x000100` |
| Support Phone | `0x000180` |
| QR Code | `0x000280` |

Default QR payload: `@IDMS_USERBOT`.

## Thresholds And Calibration

| Setting | Default | Runtime storage |
|---------|---------|-----------------|
| Display min current | 1 A | NVS `min_curr` |
| Display max current | 20 A | NVS `max_curr` |
| Power-loss threshold | 350 mA | NVS `power_ma` |
| Machine-running threshold | 1 A | NVS `run_ma` |
| Current scale | 30.00 A/V | NVS `curr_cal` |
| Auto-zero samples | 512 | Kconfig |
| Auto-zero max RMS | 50 mV | Kconfig |
| Delta T low threshold | 5 C | NVS `dt_alert` |
| Delta T high threshold | 15 C | NVS `dt_high` |

Important current sensor notes:

- SCT polarity does not affect RMS current magnitude.
- ADC bias must be near mid-scale; GPIO6 should be around 1.65 V at TP_ADC.
- For SCT-013 100A/1V, set current calibration near `100.00 A/V`.
- For SCT-013-000 current-output type, the burden resistor must be installed.

## Telemetry And Cloud

Local telemetry:

- Stored in SPIFFS at `/spiffs/telemetry.csv`
- Sample period: 60 seconds
- Rotates at 512 KiB to `/spiffs/telemetry.old.csv`

Cloud telemetry:

- Optional generic HTTPS POST endpoint
- Configured with `set_cloud_url` and `set_cloud_token`
- Upload interval default: 300 seconds
- See [CLOUD_SETUP.md](./CLOUD_SETUP.md)

## Telegram

Telegram supports:

- `/start` menu
- `/status`
- `/weekly`
- OTA link generation
- Technician management
- Shared admin-name/password technician enrollment
- Ringing-style critical alerts
- Reminder/cancel flow
- Offline queue flushing after Wi-Fi returns

Provision the shared Telegram admin name and password from the bot itself only
once, on a blank device with no saved technicians. The first user who opens the
bot with `/start` creates the shared admin name/password and is saved as the
first technician. Later technicians open the same bot, send the shared admin
name, then send the shared password. If both are correct, the ESP saves that
Telegram ID in the technician list and future bot use does not ask again.

After the first Telegram setup, changing the shared admin name/password is a
serial-console-only operation:

```text
set_bot_admin NEW_SHARED_ADMIN_NAME
set_bot_password NEW_SHARED_ADMIN_PASSWORD
show_secrets
```

The shared password is stored only as a salted hash, and Telegram chat input is
not hidden by Telegram clients.

## Telegram Login Scenarios

| Scenario | Expected behavior | Operator action |
|----------|-------------------|-----------------|
| Blank device, no technicians, no shared login | First `/start` opens the one-time Telegram setup wizard | Enter shared admin name, password, and password confirmation in a private chat |
| First setup completed | First Telegram user is saved as technician slot 0 | Use `/start` normally; no more password prompts for that Telegram account |
| New technician joins later | Bot asks for shared admin name and password once | Share the bot URL; technician sends `/start`, enters the shared credentials, then is saved |
| Registered technician returns | Bot recognizes saved Telegram ID | Use menu commands directly; no admin name/password prompt |
| Wrong shared credentials | Bot rejects login and allows up to 3 attempts | Send the admin name again, then the correct password |
| Technician list is full | New technician login is rejected | Remove an old technician from Telegram Technician IDs or serial `remove <index>` |
| Need to change shared admin name/password | Telegram has no credential-change command after first setup | Use serial `set_bot_admin` and `set_bot_password`; existing saved technicians stay authorized |
| Technicians exist but shared login is missing | Telegram setup is blocked because it is no longer a blank device | Set the shared login from serial console |
| `clear` removes all technicians | Shared login remains in NVS if already configured | Next user can register with the existing shared credentials; change credentials from serial if needed |
| Full NVS erase / factory blank state | Device has no technicians and no shared login | Telegram one-time setup is available again |

## OTA

OTA server:

- Port: `8080`
- HTTPS enabled by default
- Basic Auth can be provisioned through NVS
- Telegram can generate an OTA link
- Browser upload supports multipart `.bin`
- OTA rollback is validated by health checks, not time alone

See [OTA_IMPLEMENTATION.md](./OTA_IMPLEMENTATION.md).

## Documentation Map

| Document | Purpose |
|----------|---------|
| [Project Definition.md](./Project%20Definition.md) | Scope, modules, and current phase state |
| [Specifications.md](./Specifications.md) | Technical details, pins, VP map, NVS keys |
| [CLOUD_SETUP.md](./CLOUD_SETUP.md) | Beginner cloud setup guide |
| [OTA_IMPLEMENTATION.md](./OTA_IMPLEMENTATION.md) | OTA behavior and production notes |
| [PROJECT_HANDOFF.md](./PROJECT_HANDOFF.md) | New-session handoff summary |
| [HARDWARE_DEEP_DIVE.md](./HARDWARE_DEEP_DIVE.md) | Hardware audit and PCB redesign notes |
| [PCB_Layout.md](./PCB_Layout.md) | PCB layout guidance |
| [BOM.md](./BOM.md) | Bill of materials |

## Recommended Next Phase

1. Run hardware acceptance tests on every unit.
2. Finalize secure provisioning with encrypted NVS.
3. Validate SCT calibration with known loads.
4. Validate long-wire DS18B20 reliability in the enclosure.
5. Add cloud export/dashboard on top of the Worker endpoint.
6. Freeze a production tag after field burn-in.

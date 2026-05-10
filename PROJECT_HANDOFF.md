# ESP-IDMS Project Handoff

Last updated: 2026-05-10

Use this file as the first context document for a new chat/session.

## Current State

ESP-IDMS firmware is feature-complete for the current phase. The codebase now
contains:

- Sensor monitoring and recovery
- Topway multi-page UI integration
- Runtime NVS configuration
- Telegram bot alerts and weekly reports
- OTA HTTPS upload with health-based rollback validation
- Local telemetry CSV
- Optional cloud upload to a generic HTTPS endpoint
- Production security gate and production defaults file

The latest full build completed successfully and generated:

```text
build/esp_idms.bin
```

Build command used:

```powershell
C:\Espressif\tools\ninja\1.12.1\ninja.exe -C build
```

## Most Important Files

| File | Why it matters |
|------|----------------|
| `README.md` | Current project overview and operator commands |
| `Specifications.md` | Pin map, VP map, thresholds, NVS keys |
| `CLOUD_SETUP.md` | Beginner cloud setup guide |
| `OTA_IMPLEMENTATION.md` | OTA behavior and rollback details |
| `main/app_main.c` | System init and OTA health validation |
| `components/monitor/monitor.c` | Sensor reads, calibration, fault logic |
| `components/ui/ui_topway.c` | Topway page integration |
| `components/core/config_store.c` | NVS config and secrets |
| `components/telegram/*` | Telegram bot and alerts |
| `components/telemetry/telemetry.c` | Local CSV and weekly report data |
| `components/cloud_sync/cloud_sync.c` | Cloud upload worker |

## Recent Completed Fixes

- Removed automatic NVS erase on NVS init mismatch/full errors.
- Fixed OTA token flow so browser upload works after opening a Telegram token
  link.
- Made telemetry storage failure return an init error.
- Split power-loss threshold from machine-running threshold.
- Added console commands:
  - `set_power_threshold <A>`
  - `set_running_threshold <A>`
- Hardened Topway Info page persistence with validation.
- Changed default QR payload to `@IDMS_USERBOT`.
- Made Telegram poll task startup checked and added it to OTA health readiness.
- Added `sdkconfig.defaults.production`.
- Added beginner cloud setup documentation.

## Current Runtime Commands To Know

```text
status
adc
cal_zero
cal_all
cal_current <A>
set_current_cal <A/V>
set_power_threshold <A>
set_running_threshold <A>
set_dt_high <C>
set_ssid <ssid>
set_pass <password>
set_token <telegram_token>
set_ota_user <user>
set_ota_pass <password>
set_cloud_url <url>
set_cloud_token <token>
add <telegram_chat_id>
list
show_secrets
reboot
```

## Hardware Notes From Field Testing

- SCT polarity does not matter for RMS current.
- GPIO6 ADC bias must be around 1.65 V.
- A reading near 4095 means floating/high ADC input.
- A reading near 0 means short to GND or missing bias.
- If a real 6 A load reads around 0.6 A, calibration scale is likely off by
  about 10x or auto-zero was done under load.
- For SCT-013 100A/1V, use `set_current_cal 100.00` as the starting point.
- Long sensor runs around 5 m should use twisted/shielded wiring where possible.

## Production Blockers Before Shipping

1. Enable secure boot, flash encryption, and NVS encryption through the
   manufacturing flow.
2. Rotate all credentials used during development.
3. Provision Wi-Fi, Telegram, OTA, cloud token, technicians, and device info in
   NVS, not compiled into firmware.
4. Replace development OTA TLS certificate/key strategy.
5. Run factory acceptance tests for current, T_in, T_out, Topway, Wi-Fi,
   Telegram, OTA, telemetry, and cloud.
6. Validate CT calibration with a known load.
7. Confirm enclosure/PCB electrical safety for mains-powered deployment.

## Recommended Next Phase

### Phase 2A - Pilot Validation

- Build and flash one pilot device.
- Run `status`, `adc`, `cal_zero`, and `cal_current <known_load>`.
- Verify Topway pages and all VP addresses.
- Trigger power-loss and cooling-fault tests.
- Verify Telegram alerts and restore messages.
- Verify OTA update and rollback behavior.

### Phase 2B - Cloud Dashboard

- Set up Cloudflare Worker + KV from `CLOUD_SETUP.md`.
- Add `/export` route for CSV download.
- Add a simple dashboard for current, T_in, T_out, Delta T, faults, and uptime.
- Decide whether Firebase is needed for app/dashboard use.

### Phase 2C - Production Release

- Create release branch/tag.
- Enable production security defaults.
- Document provisioning and eFuse steps.
- Produce manufacturing checklist.
- Freeze Topway HMI project and firmware VP contract.

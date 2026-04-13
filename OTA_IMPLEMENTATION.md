# OTA Implementation Summary

## Files Created
- `partitions_ota_16m.csv` — OTA partition table for 16 MB flash (N16R8)
- `partitions_ota_8m.csv` — OTA partition table for 8 MB flash (N8R8/N8R4)
- `sdkconfig.defaults.esp32s3_n8r8` — Config defaults for 8 MB S3 variants
- `main/ota.c` — Core OTA module
- `main/ota.h` — OTA public API

## Files Modified
- `main/Kconfig.projbuild` — Added OTA configuration menu
- `main/CMakeLists.txt` — Added ota.c, dependencies, project version
- `main/app_main.c` — Integrated OTA init, TWDT, version logging
- `main/telegram.c` — Added Telegram command polling (`/ota_update`)
- `main/telegram.h` — Added `telegram_command_poll_start()` declaration
- `main/ui_lvgl.c` — Added firmware version + OTA status labels
- `sdkconfig.defaults` — Added OTA rollback, TWDT, NVS encryption, SNTP
- `sdkconfig.defaults.esp32s3` — Switched to custom OTA partition table
- `README.md` — Full OTA documentation

## Features Implemented

### OTA Firmware Updates
- **HTTP Server** on port 8080 with HTML upload page
- **Basic authentication** (configurable username/password)
- **Multipart form-data parsing** for browser uploads
- **JSON status API** at `/info`
- **Telegram command** `/ota_update` triggers upload mode notification

### Rollback Protection
- `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` — bootloader tracks OTA boots
- `ota_mark_app_valid()` called at startup — cancels rollback if boot succeeds
- If firmware crashes before validation, bootloader reverts to previous slot

### Task Watchdog Timer
- 30-second timeout on main task
- Panic-on-trigger (forces reboot + rollback)
- Configured via `sdkconfig.defaults`

### Firmware Version Tracking
- Compile-time version string: `v<version>-<chip>-<date>_<time>`
- Displayed on LVGL dashboard
- Exposed via `/info` JSON API

### Multi-Flash-Size Support
| Target | Flash | Partition Table |
|--------|-------|-----------------|
| ESP32-S3 N16R8 | 16 MB | `partitions_ota_16m.csv` (4 MB per slot) |
| ESP32-S3 N8R8/N8R4 | 8 MB | `partitions_ota_8m.csv` (2 MB per slot) |
| ESP32 (legacy) | 4 MB+ | Uses defaults (OTA via menuconfig) |

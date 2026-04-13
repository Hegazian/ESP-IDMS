# ESP-IDMS — ESP-IDF firmware

Firmware for the ESP32 Industrial Device Monitoring System. It implements Wi-Fi STA, DS18B20 1-Wire, **ADC** current sense (GPIO set in `menuconfig`), Telegram HTTPS (certificate bundle), NVS technician storage, optional **LVGL 8 + ILI9341 + XPT2046**, and **OTA firmware updates**.

## Supported hardware

| Target | Notes |
|--------|--------|
| **ESP32** | Defaults match legacy docs: 1-Wire **GPIO 4**, SCT ADC **GPIO 34**. |
| **ESP32-S3 N16R8** | 16 MB flash + 8 MB octal PSRAM: use `sdkconfig.defaults.esp32s3` (see below). Default SCT ADC GPIO is **3** (change in `menuconfig` to match your PCB). GPIO range **0–48** in project Kconfig. |
| **ESP32-S3 N8R8 / N8R4** | 8 MB flash: copy `sdkconfig.defaults.esp32s3_n8r8` → override defaults. See *Flash Size Variants* below. |

## Requirements

- [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/get-started/) **v5.1+** (5.5.x used in development)
- A board matching your `idf.py set-target` (ESP32 or ESP32-S3)

## Configure

From the project directory (this repo root):

```bash
idf.py set-target esp32s3
idf.py menuconfig
```

For **ESP32** instead:

```bash
idf.py set-target esp32
idf.py menuconfig
```

Open **ESP-IDMS Configuration** and set at least:

- Wi-Fi SSID / password
- Telegram bot token (optional for bench test without cloud)
- **GPIO for SCT-013 ADC** (`IDMS_ADC_GPIO`) for your module
- LCD / touch GPIOs if your display wiring differs

## OTA Firmware Updates

The device supports **over-the-air (OTA) firmware updates** with automatic rollback on failure.

### How OTA Works

1. The flash is divided into **factory + ota_0 + ota_1** partitions.
2. New firmware is written to the *inactive* OTA slot.
3. On reboot, the bootloader marks the new partition as *pending verify*.
4. If the firmware boots and calls `ota_mark_app_valid()`, the rollback is cancelled.
5. If the firmware crashes before validating, the bootloader **rolls back** to the previous slot.

### Updating Firmware via HTTP

After the device connects to Wi-Fi, an OTA upload server starts:

```
http://<device-ip>:8080/
```

1. Open the URL in a browser
2. Authenticate with the configured credentials (default: `admin` / `ota_admin`)
3. Upload a `.bin` firmware file built with `idf.py build`
4. The device reboots into the new firmware automatically

### Updating via Telegram

Send `/ota_update` to the bot. The device will broadcast the OTA upload URL and credentials to all registered technicians.

### OTA API

The device exposes a JSON status endpoint:

```
GET http://<device-ip>:8080/info
```

Response:
```json
{
  "version": "v1.0.0-s3-2026-04-13_12:00:00",
  "partition": "ota_0",
  "status": "ready",
  "idf_version": "...",
  "chip": "ESP32-S3",
  "compile_time": "..."
}
```

### Flash Size Variants

| Variant | Config override | Partition table |
|---------|----------------|-----------------|
| **ESP32-S3 N16R8** (default) | `sdkconfig.defaults.esp32s3` | `partitions_ota_16m.csv` |
| **ESP32-S3 N8R8 / N8R4** | `sdkconfig.defaults.esp32s3_n8r8` | `partitions_ota_8m.csv` |

To switch variants, edit or replace the corresponding `sdkconfig.defaults.*` file **before** running `idf.py build`.

## ESP32-S3 N16R8 (16 MB flash, 8 MB octal PSRAM)

After `set-target esp32s3`, ESP-IDF merges **`sdkconfig.defaults`** and **`sdkconfig.defaults.esp32s3`**, which enable:

- `CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y`
- Octal PSRAM @ 80 MHz
- OTA partition table (`partitions_ota_16m.csv`)
- `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y`

Then build and flash (replace `COMx`):

```bash
idf.py build
idf.py -p COMx flash monitor
```

If PSRAM is not detected on a specific module, run `idf.py menuconfig` → **Component config → ESP32S3-Specific → Support for external, SPI-connected RAM** and match your vendor's mode (octal / speed).

## Build and flash (generic)

```bash
idf.py build
idf.py -p COMx flash monitor
```

Managed components (`esp_lcd_ili9341`, `lvgl`) are resolved on first build.

**Security:** `menuconfig` stores the Wi-Fi password and Telegram token in `sdkconfig` (plaintext). Do not commit `sdkconfig` to a public repository.

## Behaviour summary

| Item | Detail |
|------|--------|
| Sampling | 500 ms task; ADC RMS from 256 samples; DS18B20 conversion pipelined |
| Power loss | RMS current below threshold for **5 s** → Telegram broadcast |
| Cooling | ΔT = Tout − Tin; outside **5 °C … 15 °C** for **5 s** → alert |
| Heartbeat | `getMe` every **60 s** when Wi-Fi is up |
| UI | Live metrics + textarea to append a technician `chat_id` into NVS (max 5) |
| OTA | HTTP server on port 8080 + Telegram `/ota_update` command; auto-rollback on crash |
| Watchdog | Task WDT (30 s) — panics and triggers rollback if main task hangs |

Hardware pinout for the original ESP32 design is in [`Specifications.md`](./Specifications.md); **S3 builds must align `menuconfig` GPIOs with your PCB** (S3 has no GPIO 34).

## Disable the display

In `menuconfig` disable **ESP-IDMS Configuration → Enable LVGL + ILI9341 UI** for a headless image (monitoring and Telegram still run).

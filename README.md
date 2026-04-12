# ESP-IDMS — ESP-IDF firmware

Firmware for the ESP32 Industrial Device Monitoring System. It implements Wi-Fi STA, DS18B20 1-Wire, **ADC** current sense (GPIO set in `menuconfig`), Telegram HTTPS (certificate bundle), NVS technician storage, and optional **LVGL 8 + ILI9341 + XPT2046**.

## Supported hardware

| Target | Notes |
|--------|--------|
| **ESP32** | Defaults match legacy docs: 1-Wire **GPIO 4**, SCT ADC **GPIO 34**. |
| **ESP32-S3 N16R8** | 16 MB flash + 8 MB octal PSRAM: use `sdkconfig.defaults.esp32s3` (see below). Default SCT ADC GPIO is **3** (change in `menuconfig` to match your PCB). GPIO range **0–48** in project Kconfig. |

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

## ESP32-S3 N16R8 (16 MB flash, 8 MB octal PSRAM)

After `set-target esp32s3`, ESP-IDF merges **`sdkconfig.defaults`** and **`sdkconfig.defaults.esp32s3`**, which enable:

- `CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y`  
- Octal PSRAM @ 80 MHz  
- `CONFIG_PARTITION_TABLE_SINGLE_APP_LARGE=y`  
- `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y`  

Then build and flash (replace `COMx`):

```bash
idf.py build
idf.py -p COMx flash monitor
```

If PSRAM is not detected on a specific module, run `idf.py menuconfig` → **Component config → ESP32S3-Specific → Support for external, SPI-connected RAM** and match your vendor’s mode (octal / speed).

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

Hardware pinout for the original ESP32 design is in [`Specifications.md`](./Specifications.md); **S3 builds must align `menuconfig` GPIOs with your PCB** (S3 has no GPIO 34).

## Disable the display

In `menuconfig` disable **ESP-IDMS Configuration → Enable LVGL + ILI9341 UI** for a headless image (monitoring and Telegram still run).

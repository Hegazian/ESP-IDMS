# OTA Implementation - ESP-IDMS

Document version: 3.0
Last updated: 2026-05-10

## 1. Current State

OTA is implemented in:

```text
components/ota/ota.c
components/ota/include/ota.h
components/ota/certs/
```

The OTA server is started from `main/app_main.c` after Wi-Fi, monitor,
telemetry, cloud sync, UI, Telegram, and serial console initialization.

## 2. Features

| Feature | Status |
|---------|--------|
| OTA HTTP server | Implemented |
| HTTPS mode | Enabled by default |
| Browser upload page | Implemented |
| Multipart upload parser | Implemented with boundary carryover |
| Basic Auth | Implemented, credentials stored in NVS |
| Telegram one-time OTA token | Implemented |
| Token upload flow | Token preserved from page GET to POST upload |
| SHA256 calculation | Implemented |
| Optional expected SHA256 header | Implemented |
| OTA partition selection | Implemented |
| Rollback support | Implemented |
| Health-based validation | Implemented |

## 3. Endpoints

| Endpoint | Method | Purpose |
|----------|--------|---------|
| `/` | GET | HTML firmware upload page |
| `/` | POST | Firmware upload |
| `/info` | GET | OTA/version/status JSON |
| `/telemetry.csv` | GET | Download local telemetry CSV |

All endpoints require auth.

## 4. Authentication

OTA supports two auth methods:

1. Basic Auth using `ota_user` and `ota_pass` from NVS.
2. Short-lived token generated for Telegram OTA links.

Provision Basic Auth from serial console:

```text
set_ota_user admin
set_ota_pass STRONG_PASSWORD
```

The Telegram token is not compiled into the firmware image. It is generated at
runtime and expires after the configured token lifetime.

## 5. HTTPS

`CONFIG_IDMS_OTA_HTTPS_ENABLE=y` is enabled by default.

The current repo contains a development self-signed certificate/key pair in:

```text
components/ota/certs/ota_server_cert.pem
components/ota/certs/ota_server_key.pem
```

Production recommendation:

- Generate a per-product or per-device certificate/key.
- Do not reuse a public development private key across all shipped devices.
- Use OTA only on trusted LANs or behind a controlled service path.
- Keep strong Basic Auth credentials provisioned in encrypted NVS.

## 6. Upload Flow

1. User opens the OTA page.
2. Auth is checked.
3. User selects a `.bin` file.
4. Browser sends multipart form-data.
5. Firmware writes only firmware bytes to the next OTA partition.
6. Firmware calculates SHA256 during write.
7. `esp_ota_end()` validates the image.
8. Firmware sets the next boot partition.
9. Firmware notifies Telegram and reboots.

The upload form preserves a valid token in its POST action, so a Telegram OTA
link can authorize both opening the page and uploading the image.

## 7. Rollback Validation

Rollback is not cancelled just because the firmware stayed alive for a fixed
time. The image is marked valid only after the app health gate passes.

Health gate currently requires:

- Telemetry storage initialized
- UI task initialized
- Telegram poll task created
- Sensor preflight complete and OK
- Current valid
- T_in valid
- T_out valid
- Delta T valid

If health does not pass before timeout, the firmware marks the app invalid and
reboots for rollback.

## 8. Build Configuration

Relevant Kconfig/defaults:

```text
CONFIG_IDMS_OTA_ENABLE=y
CONFIG_IDMS_OTA_HTTP_PORT=8080
CONFIG_IDMS_OTA_HTTPS_ENABLE=y
CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y
```

Production security baseline is in:

```text
sdkconfig.defaults.production
```

## 9. Partition Tables

| File | Target |
|------|--------|
| `partitions_ota_16m.csv` | ESP32-S3 16 MB flash |
| `partitions_ota_8m.csv` | ESP32-S3 8 MB flash |

The 16 MB table provides large OTA slots for the current firmware and future
feature growth.

## 10. Testing Checklist

Before release:

- [ ] Provision OTA username/password.
- [ ] Confirm OTA page opens over HTTPS.
- [ ] Upload a known-good `.bin`.
- [ ] Confirm reboot into new partition.
- [ ] Confirm health validation marks the image valid.
- [ ] Confirm a deliberately bad sensor state prevents validation.
- [ ] Confirm rollback to previous firmware.
- [ ] Confirm `/info` returns correct version/partition/status.
- [ ] Confirm `/telemetry.csv` requires auth.

## 11. Known Production Work

- Replace development TLS key/cert with a production certificate strategy.
- Add operator-visible OTA status on Topway if needed.
- Add signed firmware verification if the deployment needs stronger protection
  than ESP-IDF secure boot alone.
- Add a documented manufacturing OTA credential provisioning step.

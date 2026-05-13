# Project Definition - ESP-IDMS

Document version: 3.0
Last updated: 2026-05-10
Status: Feature-complete firmware, entering production hardening and field validation

## 1. Project Summary

ESP-IDMS is an ESP32-S3 based industrial monitoring device. It monitors machine
power state through a current transformer, monitors cooling performance through
two temperature probes, shows live values on a Topway smart LCD, sends Telegram
alerts, supports OTA firmware updates, and stores/uploads telemetry for later
analysis.

The project is now ready to start a new phase:

- Pilot deployment
- Cloud dashboard/export work
- Production security provisioning
- PCB/enclosure finalization
- Manufacturing test process

## 2. Primary Goals

1. Detect machine power loss from AC current.
2. Detect cooling problems from `Delta T = T_out - T_in`.
3. Show status locally on a 7-inch Topway LCD.
4. Alert technicians through Telegram.
5. Allow safe OTA updates with rollback.
6. Persist configuration, credentials, calibration, and device info in NVS.
7. Record telemetry locally and upload it to a cloud endpoint.
8. Support production hardening with secure boot, flash encryption, and NVS
   encryption.

## 3. Current Implemented Features

| Area | Current implementation |
|------|------------------------|
| Current sensing | SCT-013 ADC RMS sampling, auto-zero guard, runtime calibration |
| Temperature | DS18B20 T_in and T_out on separate 1-Wire buses |
| Fault detection | Power-loss and cooling-fault state machines with debounce |
| Sensor health | Startup preflight plus runtime recovery for current/temp faults |
| Display | Topway Home, Settings, Configuration, and Info pages |
| Config persistence | NVS storage for thresholds, device info, secrets, technicians |
| Wi-Fi | STA mode, reconnect, SNTP |
| Telegram | Bot menu, status, alerts, reminders, weekly report, OTA link |
| OTA | HTTPS upload, Basic Auth/token, SHA256, rollback health validation |
| Telemetry | SPIFFS CSV, weekly stats, cloud upload batching |
| Cloud | Generic HTTPS POST with Bearer token |
| Console | Runtime provisioning, diagnostics, calibration commands |

## 4. Key Architecture

```text
Sensors -> monitor task -> metrics/state
                    |-> Topway UI task
                    |-> Telegram alerts
                    |-> telemetry CSV
                    |-> cloud sync task

Serial console -> config_store -> NVS
Topway settings -> config_store -> NVS
OTA server -> new firmware -> health validation -> rollback or accept
```

## 5. Source Modules

| Module | Path | Responsibility |
|--------|------|----------------|
| App orchestration | `main/app_main.c` | Init order and OTA health validation |
| Config store | `components/core/config_store.c` | NVS config, secrets, thresholds, device info |
| Monitor | `components/monitor/monitor.c` | Sensor sampling, calibration, faults, metrics |
| Topway driver | `components/drivers/topway_lcd.c` | UART protocol for smart LCD |
| Topway UI | `components/ui/ui_topway.c` | Display updates, Wi-Fi/config/info page handling |
| Wi-Fi | `components/wifi/wifi_manager.c` | STA connection and SNTP |
| Telegram | `components/telegram/*` | Bot polling, HTTP, parsing, sending, menus |
| OTA | `components/ota/ota.c` | Firmware upload server and rollback helpers |
| Telemetry | `components/telemetry/telemetry.c` | CSV history and weekly statistics |
| Cloud sync | `components/cloud_sync/cloud_sync.c` | HTTPS upload of pending CSV rows |
| Console | `components/cli/serial_console.c` | Serial commands for provisioning and tests |

## 6. Operational Behavior

### Power Loss

Power loss is detected when valid current stays below the NVS-backed
`power_ma` threshold for at least 5 seconds. This threshold is separate from the
display's min/max current limits.

### Cooling Fault

Cooling fault is checked only when the machine is considered running. The
machine-running gate uses the NVS-backed `run_ma` threshold. Cooling is faulty
when valid Delta T is below `dt_alert` or above `dt_high` for at least 5
seconds.

### Sensor Faults

Sensor preflight and runtime checks report current ADC/bias faults, T_in faults,
T_out faults, and Delta T invalid state. Faults can recover automatically once
readings become valid again.

### Alerts

Critical alerts are sent through Telegram. Ringing alerts are queued through a
Telegram worker, reminders are protected by a mutex, and offline messages are
kept for later flush when Wi-Fi returns.

## 7. Production Security Model

Development builds keep hardware security features off for convenience.
Production builds must use:

- `CONFIG_IDMS_PRODUCTION_BUILD=y`
- `CONFIG_SECURE_BOOT=y`
- `CONFIG_SECURE_FLASH_ENC_ENABLED=y`
- `CONFIG_NVS_ENCRYPTION=y`

The repository includes `sdkconfig.defaults.production` as the production
baseline. Enabling these settings is part of manufacturing because secure boot
and flash encryption can involve irreversible eFuse operations.

## 8. Cloud Strategy

The firmware uploads telemetry to a generic HTTPS endpoint. The recommended
first cloud implementation is Cloudflare Worker + KV because it accepts the
firmware's Bearer token flow without putting database credentials on the ESP32.

Firebase is a useful next layer for dashboards, but direct Firebase REST writes
from the device should be avoided unless a safe token issuing mechanism is
designed.

See [CLOUD_SETUP.md](./CLOUD_SETUP.md).

## 9. Out Of Scope For This Phase

- Full mobile app
- Web dashboard charts
- Factory provisioning GUI
- Final certified PCB design
- Modbus/SCADA integration
- Cellular/GSM fallback
- Multi-device fleet management UI

These are good candidates for the next phase.

## 10. Exit Criteria For Current Phase

The current firmware phase is considered complete when:

- Full firmware build passes.
- Topway VP contract matches the HMI project.
- Sensor preflight/recovery behavior is stable on real hardware.
- OTA health validation works on a real OTA update.
- Telegram alerts and technician setup work after NVS provisioning.
- Cloud endpoint receives telemetry rows.
- Documentation reflects the current state.

## 11. Next Phase Entry Points

For a new chat/session, start with:

1. [PROJECT_HANDOFF.md](./PROJECT_HANDOFF.md)
2. [README.md](./README.md)
3. [Specifications.md](./Specifications.md)
4. [CLOUD_SETUP.md](./CLOUD_SETUP.md)

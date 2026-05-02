# ESP-IDMS — Atopile Hardware Project

ESP32-S3 Industrial Device Monitoring System.
PCB: 2-layer FR4, 100×80 mm.

## Build

```bash
ato build
```

Result: `layouts/default/default.kicad_pcb`

## Verify

```bash
ato build  # 19/19 stages, 0 errors
```

## Project Structure

```
hardware/
├── ato.yaml              # Build manifest (ato build)
├── esp-idms.ato           # Top-level circuit (ESP_IDMS module)
├── usage.ato              # Build target entry
├── src/                   # Logical module definitions (8 files)
│   ├── esp32s3_devkitc.ato    # MCU with 28 GPIO signals
│   ├── topway_hkt070dta.ato   # Topway LCD UART module
│   ├── hlk_pm01.ato           # AC-DC 5V PSU module
│   ├── sct013_bias.ato        # CT bias + Pi filter + ESD
│   ├── ds18b20_bus.ato        # 1-Wire bus + pull-up + ESD
│   ├── status_led.ato         # 3 LED variants (Green/Blue/Red)
│   ├── button.ato             # Active-low button + 10kΩ pull-up
│   └── connectors.ato         # 6 connector wrappers
├── parts/                 # Atomic components (22 files)
│   ├── esp32s3_devkit/        # ESP32-S3 DevKitC-1 N16R8
│   ├── topway_lcd/            # Topway HKT070DTA-1C
│   ├── hlk_pm01/              # Hi-Link HLK-PM01
│   ├── pesd5v0s1ba/           # Nexperia TVS diode
│   ├── led_3mm/               # Lite-On 3mm TH LEDs
│   ├── tl3301/                # E-Switch tactile button
│   ├── jst_xh/                # JST XH connectors (2P/3P/4P/6P)
│   ├── screw_terminal/        # Weidmüller 3-pin screw term
│   ├── pin_header/            # Samtec 1×19 female header
│   ├── fuse_holder/           # Keystone 5×20mm fuse clip
│   ├── test_point/            # Keystone test point loop
│   └── (auto-generated SMD)   # Resistors & capacitors (solver-picked)
├── layouts/               # Build output (KiCad PCB, BOM, etc.)
└── README.md
```

## Design

See `../HARDWARE_DEEP_DIVE.md` for the full hardware analysis.
See `../BOM.md` for the bill of materials.

## Resolving Warnings (TH footprints)

The 23 warnings about missing footprints are for through-hole
components (connectors, LEDs, buttons). To resolve:

```bash
# Generate footprint files for each component:
cd parts/<component>
ato create part --supplier lcsc <LCSC_CODE>

# Then add back the trait to the .ato file:
#   trait is_atomic_part<manufacturer="...", footprint="...", symbol="...", model="...">
```

# Project Definition — ESP32 Industrial Device Monitoring System (ESP-IDMS)

**Document Version:** 1.0  
**Date:** April 2026  
**Status:** Approved

---

## 1. Executive Summary

The **ESP32 Industrial Device Monitoring System (ESP-IDMS)** is a low-cost, professional-grade IoT monitoring appliance designed for deployment inside industrial facilities. The system continuously monitors a target machine's **operational status** and **cooling circuit efficiency**, and delivers instant push notifications to designated maintenance technicians via **Telegram** whenever a fault condition is detected.

The project is purpose-built to minimize total cost of ownership: there are no recurring SIM or data subscription fees, and the entire connectivity stack runs over the facility's existing Wi-Fi infrastructure.

---

## 2. Project Objectives

| # | Objective |
|---|---|
| 1 | Provide **24/7 unattended monitoring** of machine power state and cooling performance. |
| 2 | Deliver **instant, remote fault alerts** to maintenance technicians worldwide, without requiring an on-site presence. |
| 3 | Maintain a **total BOM cost under $35 USD** per unit to allow economical fleet deployment. |
| 4 | Eliminate recurring costs by replacing GSM/SIM-based connectivity with **Wi-Fi + cloud push notifications**. |
| 5 | Ensure **personnel safety** through galvanic isolation between mains AC and all low-voltage electronics. |

---

## 3. Design Philosophy & Technology Selection

### 3.1 Connectivity: Wi-Fi over GSM

The system was initially conceived with a GSM module for cellular connectivity. After evaluation, Wi-Fi was selected as the final approach for the following reasons:

| Criterion | GSM/SIM | Wi-Fi (Selected) |
|---|---|---|
| Recurring cost | Monthly SIM fees | None |
| Network reliability | Dependent on 2G/EDGE infrastructure (declining) | Facility LAN (always on) |
| Data latency | Higher | Lower |
| Hardware complexity | Higher (AT commands, SIM management) | Lower (native ESP32 stack) |

### 3.2 Alert Delivery: Telegram Bot API

Telegram was selected for alert delivery due to its free tier, robust HTTPS API, multi-device delivery, and instant notification capability. Technician IDs can be managed directly on the device without requiring a laptop or web interface.

---

## 4. Core Functional Modules

### 4.1 Machine State Monitoring

The system uses a **non-invasive AC Current Transformer (SCT-013-000)** that clips around a single live or neutral wire. This approach requires no physical connection to copper conductors and does not interrupt machine operation during installation.

A **custom DC offset bias circuit** (dual 10 kΩ voltage divider + 10 µF decoupling capacitor) centres the AC waveform at 1.65 V (half of 3.3 V), making it readable by the ESP32's ADC on GPIO 34.

**Fault Trigger:** If the measured RMS current drops below the configured threshold (0.2 A – 0.5 A) for more than 5 consecutive seconds, a **"Power Loss"** alert is dispatched to all registered technicians.

### 4.2 Cooling Efficiency Monitoring

Two **waterproof DS18B20** digital temperature sensors are deployed on a shared **1-Wire bus** (GPIO 4):

- **Sensor 1 (T_in):** Measures coolant/air temperature at the machine's cooling inlet.
- **Sensor 2 (T_out):** Measures coolant/air temperature at the machine's cooling outlet.

The system continuously computes the **temperature differential (ΔT = T_out − T_in)** and compares it against configurable thresholds.

**Fault Trigger:** If ΔT falls below 5 °C (insufficient heat transfer) or rises above 15 °C (excessive heat load, indicating coolant blockage or pump failure), a **"Cooling Failure"** alert is dispatched.

### 4.3 Remote Alert & Notification System

- **Transport:** Wi-Fi 2.4 GHz (802.11 b/g/n) with automatic reconnect logic.
- **Protocol:** HTTPS POST to the Telegram Bot API (`api.telegram.org`) with SSL/TLS validation.
- **Heartbeat:** A cloud connectivity check runs every 60 seconds.
- **Storage:** Up to **5 technician Telegram Chat IDs** are stored persistently in ESP32 Non-Volatile Storage (NVS).

### 4.4 On-Device User Interface

A **2.8" SPI TFT touch display** (ILI9341 controller) provides:

- A real-time dashboard showing current machine state, T_in, T_out, ΔT, and network status.
- An interactive menu system for adding or removing technician Telegram Chat IDs without any external tools.

### 4.5 Data Persistence

Technician IDs are written to the ESP32's **Non-Volatile Storage (NVS)** using the `Preferences.h` library. This ensures that all configuration survives power outages and device reboots without requiring re-commissioning.

### 4.6 Power Supply & Safety

The unit is powered by a **Hi-Link HLK-PM01** isolated AC-DC module (100–240 V AC → 5 V DC). This module provides:

- **Galvanic isolation** between the mains AC supply and all low-voltage logic circuitry.
- A wide input voltage range suitable for global deployment.

**Recommended Enhancement:** A small **18650 Li-ion cell** (with a suitable charge/protection module) is recommended as a backup power source. This allows the ESP32 to transmit a final "Power Loss" alert during a total facility power outage before shutting down.

---

## 5. Out of Scope

- Local area network (LAN) dashboard or web server interface.
- Integration with SCADA or PLC systems.
- Data logging / time-series database integration.
- OTA (Over-the-Air) firmware update mechanism *(recommended for future revision)*.

---

## 6. Related Documents

| Document | Description |
|---|---|
| [`Specifications.md`](./Specifications.md) | Technical specifications, sampling rates, and fault thresholds |
| [`BOM.md`](./BOM.md) | Bill of Materials with part numbers and cost estimates |
| [`PCB_Layout.md`](./PCB_Layout.md) | PCB design guidelines, layer usage, and GPIO pin mapping |
| [`README.md`](./README.md) | Project overview and quick-start guide |
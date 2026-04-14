# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Firmware for a humidity control system in a storage room (~7 m²). Hardware: Wemos Lolin ESP32 (clone with integrated SSD1306 OLED), two DHT21 (AM2301) sensors, a 2-channel relay module (active LOW), and a button. Built with PlatformIO + Arduino framework.

## Build & Flash Commands

```bash
# Build
pio run

# Build + upload to device
pio run --target upload

# Open serial monitor (115200 baud)
pio device monitor

# Build + upload + monitor in one step
pio run --target upload && pio device monitor
```

Uses the `min_spiffs.csv` partition table (maximises app space, minimal SPIFFS). There are no automated tests; verification is done via serial output and nRF Connect for BLE.

## Architecture

The firmware uses **FreeRTOS with four concurrent tasks**:

| Task | Core | Prio | Role |
|---|---|---|---|
| `taskSensors` | 1 | 3 | Reads both DHT21 sensors outside the mutex, then writes results under it |
| `taskControl` | 1 | 2 | Evaluates state machine and drives relays |
| `taskDisplay` | 0 | 1 | Snapshots shared data under mutex, renders OLED outside it |
| `taskBLE` | 0 | 2 | Sends GATT notifications every 1 s |

All shared state (`interior`, `exterior`, `currentState`, `manualOverride`, `humActivate`, `humDeactivate`, `humDelta`) is protected by `dataMutex`. `taskBLE` uses a 100 ms timeout on the mutex to avoid blocking critical tasks.

### Module split

- [src/main.cpp](src/main.cpp) — pin definitions, shared globals, sensor logic, control state machine, display rendering, FreeRTOS task implementations, `setup()`/`loop()`
- [src/ble_gatt.h](src/ble_gatt.h) — shared types (`SensorData`, `ControlState`, `ManualOverride`) and BLE public API (`bleInit()`, `taskBLE()`)
- [src/ble_gatt.cpp](src/ble_gatt.cpp) — GATT server implementation; accesses main.cpp globals via `extern`

### Control state machine

Four states: `IDLE` → `EXTRACTOR_ON` or `DEHUMID_ON` → `IDLE`, plus `SAFE_OFF` on ≥5 consecutive sensor failures. Hysteresis band: activate at 70% RH, deactivate at 65% RH. Extractor is preferred when `interior_humidity - exterior_humidity >= 10%`.

### Adaptive sensor intervals

| State | Production | Test (`PRUEBAS 1`) |
|---|---|---|
| IDLE | 10 min | 20 s |
| EXTRACTOR_ON | 2 min | 10 s |
| DEHUMID_ON | 5 min | 15 s |
| SAFE_OFF | 30 s | 5 s |
| Confirmation (post state-change) | 30 s | 5 s |

**Before flashing production firmware, set `#define PRUEBAS 0` in [src/main.cpp](src/main.cpp#L49).**

When `taskControl` changes state it calls `xTaskNotify(hTaskSensors, ...)` to wake `taskSensors` early for a confirmation reading.

## Key Pin Assignments

| Signal | GPIO |
|---|---|
| OLED SDA | 5 |
| OLED SCL | 4 |
| DHT21 interior | 13 |
| DHT21 exterior | 14 |
| Relay extractor (active LOW) | 26 |
| Relay dehumidifier (active LOW) | 25 |
| Button (internal pull-up, GND on press) | 12 |

## BLE GATT Interface

Device name: **"Trastero"**. Service UUID: `1b5ab4f0-0000-1000-8000-000000000000`.

> Note: the comment at the top of [src/ble_gatt.h](src/ble_gatt.h) shows `...00805f9b34fb`, but the actual `#define TRASTERO_SVC_UUID` in [src/ble_gatt.cpp](src/ble_gatt.cpp#L27) is `...000000000000`. Use the `.cpp` value when connecting with nRF Connect.

All multi-byte values are little-endian. Payload formats are documented in [src/ble_gatt.h](src/ble_gatt.h).

| Characteristic | UUID suffix | Properties | Payload |
|---|---|---|---|
| `SENSOR_INT` | `...0001` | READ, NOTIFY | 9 B: float temp + float hum + uint8 valid |
| `SENSOR_EXT` | `...0002` | READ, NOTIFY | 9 B: same |
| `ESTADO` | `...0003` | READ, NOTIFY | 1 B: 0=IDLE 1=EXTRACTOR 2=DEHUMID 3=SAFE |
| `CMD_ACTUADOR` | `...0010` | WRITE | 2 B: [0]=cmd (0=AUTO 1=EXTRACTOR 2=DEHUMID 3=OFF) [1]=reserved |
| `UMBRALES` | `...0011` | READ, WRITE | 12 B: float activar + float desactivar + float delta |
| `HORA` | `...0012` | READ, WRITE | 4 B: uint32 unix timestamp UTC |
| `CMD_DISPLAY` | `...0013` | WRITE | 1 B: 0x01=turn on 30 s, 0x00=turn off immediately |

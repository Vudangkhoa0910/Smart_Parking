# ParkingLite — Production Firmware

> **ParkingLite v1.0** — Smart Parking Detection System  
> Phenikaa University · NCKH Research 2025–2026  
> Hardware-validated · F1 = 0.985 · 100% accuracy on real parking images

---

## Overview

ParkingLite is an edge-AI parking detection system using ESP32-CAM nodes that classify parking slot occupancy in real time and transmit results wirelessly via ESP-NOW — **no router or internet required**.

```
┌─────────────────┐       ESP-NOW       ┌─────────────────┐
│  Sensor Node    │  ─────────────────► │  Gateway Node   │
│  (ESP32-CAM)    │   { node_id, bitmap }│  (ESP32 Dev)    │
│                 │                     │                  │
│  OV2640 Camera  │                     │  Serial → App   │
│  ROI Classifier │                     │  JSON Output    │
│  11 methods     │                     │  Multi-node     │
└─────────────────┘                     └─────────────────┘
```

**Multi-lot support:** Each gateway handles up to 8 sensor nodes (64 slots total). Deploy one gateway per parking lot.

---

## Hardware

| Component | Model | Role |
|-----------|-------|------|
| Sensor Node | ESP32-CAM AI-Thinker | Camera + edge classification |
| Gateway Node | ESP32 Dev Board (any) | Wireless receiver + serial output |

**Validated MAC addresses (lab units):**
- Sensor: `1C:C3:AB:FB:00:AC` — port `/dev/cu.usbserial-110`
- Gateway: `B0:CB:D8:CF:56:14` — port `/dev/cu.usbserial-10`

---

## Repository Structure

```
Production/
├── sensor_node/          # ESP32-CAM firmware
│   ├── sensor_node.ino   # Main firmware (clean, production-ready)
│   ├── config.h          # ← EDIT THIS per device
│   ├── camera_config.h   # OV2640 pin map + camera configs
│   ├── roi_classifier.h  # Classification API
│   └── roi_classifier.cpp# 11-method integer MAD classifier
├── gateway/
│   └── gateway.ino       # Multi-node receiver, JSON output
├── tools/
│   ├── flash.sh          # One-command flash helper
│   └── quick_snap.py     # CLI image capture tool
└── README.md             # This file
```

---

## Quick Start

### 1. Prerequisites

```bash
# Install arduino-cli
brew install arduino-cli            # macOS
# or: https://arduino.github.io/arduino-cli/

# Install ESP32 board package
arduino-cli core update-index
arduino-cli core install esp32:esp32
```

### 2. Configure Sensor Node

Edit `sensor_node/config.h` **before flashing each device**:

```c
#define NODE_ID    0x01   // Unique per camera (0x01, 0x02, 0x03, ...)
#define LOT_ID     0x01   // Parking lot this node reports to
#define N_SLOTS    8      // Number of active parking slots
```

Adjust `SLOT_ROIS[]` to match your camera angle (use the ROI calibration tool in `ROI/app/`).

### 3. Flash Firmware

```bash
cd Production/tools

# Flash sensor node (ESP32-CAM)
./flash.sh sensor /dev/cu.usbserial-110

# Flash gateway (ESP32 Dev)
./flash.sh gateway /dev/cu.usbserial-10
```

For multiple sensor nodes, change `NODE_ID` in `config.h` and re-flash each one.

### 4. Calibrate

After flashing, open a serial monitor at **115200 baud** and calibrate the sensor:

```
# With the parking lot EMPTY:
CAL
```

Calibration is saved to NVS flash — survives power cycles.

### 5. Monitor

```bash
# Sensor node
arduino-cli monitor -p /dev/cu.usbserial-110 --config baudrate=115200

# Gateway
arduino-cli monitor -p /dev/cu.usbserial-10 --config baudrate=115200
```

Or use the parking monitor app:

```bash
cd Tools/parking_monitor
pip install -r requirements.txt
python parking_monitor.py
```

---

## Serial Commands (Sensor Node)

| Command | Description |
|---------|-------------|
| `CAL` | Calibrate with current (empty-lot) frame → NVS |
| `RESET` | Clear calibration |
| `STATUS` | Print slot states and system info |
| `METHOD X` | Switch classification method (0–10) |
| `INTERVAL X` | Set scan interval in ms (1000–60000) |
| `ROI X Y W H I` | Override ROI for slot I |
| `ROI_GET` | Print ROI config as JSON |
| `SLOTS_GET` | Print last results as JSON |
| `SNAP` | Stream grayscale JPEG via serial |
| `SNAP_COLOR` | Stream color SVGA JPEG via serial |
| `PING` | Connectivity check |

---

## Gateway JSON Output

Every received packet emits a JSON line:

```json
{"event":"update","lot":1,"node":1,"bitmap":237,"slots":8,"occupied":6,"rx_count":42,"uptime_ms":12345}
```

`STATUS` command emits full node table:

```json
{"lot":1,"uptime_ms":60000,"nodes":[{"id":1,"bitmap":237,"occupied":6,"online":true,"rx_count":42}]}
```

---

## Classification Algorithm

Integer MAD (Mean Absolute Difference) with per-ROI mean-shift normalization:

```
Per-ROI Mean Shift  — local brightness normalization (normalize_brightness())
Combined Ensemble   — 7 sub-methods weighted vote (classify_combined())
```

| Method | Name | Accuracy | Calibration |
|--------|------|----------|-------------|
| 0 | edge_density | 68.8% | Not needed |
| 2 | ref_frame_mad | 100.0% | Required |
| 10 | combined | 100.0% | Required ← **default** |

Evaluated on 54,000 samples across 6 lighting conditions.

---

## Multi-Node Deployment

For a parking lot with 24 slots (3 cameras × 8 slots):

```
config.h Node 1: NODE_ID=0x01, N_SLOTS=8 → flash to Camera 1
config.h Node 2: NODE_ID=0x02, N_SLOTS=8 → flash to Camera 2
config.h Node 3: NODE_ID=0x03, N_SLOTS=8 → flash to Camera 3
```

One gateway handles all three. Gateway output:

```
[RX] Node=0x01 Bitmap=0xED → 6/8 occupied
[RX] Node=0x02 Bitmap=0x03 → 2/8 occupied
[RX] Node=0x03 Bitmap=0x00 → 0/8 occupied
```

---

## Critical Hardware Notes

| Parameter | Value | Reason |
|-----------|-------|--------|
| XCLK frequency | **10 MHz** only | 20 MHz unstable on AI-Thinker |
| Upload baud | `--upload-property upload.speed=115200` | 921600 causes "chip stopped responding" |
| Serial baud | 115200 | Both nodes must match |
| ESP-NOW callback (core 3.3.x) | `esp_now_recv_info_t*` | Breaking API change in 3.3.0 |

---

## License

Research prototype — Phenikaa University NCKH 2025-2026.

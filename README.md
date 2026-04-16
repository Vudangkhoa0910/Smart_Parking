# ParkingLite — Smart Parking for Small Lots

> **NCKH 2025–2026 · Phenikaa University**  
> Edge-AI parking detection on ESP32-CAM · No Internet · No GPU · ~31,000 VND/slot

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/platform-ESP32--CAM-green.svg)](https://www.espressif.com/en/products/socs/esp32)
[![F1 Score](https://img.shields.io/badge/F1-0.985-brightgreen.svg)](Production/report_assets/charts/fig1_f1_comparison_11methods.png)
[![Accuracy](https://img.shields.io/badge/Real--image_acc-100%25-brightgreen.svg)](#results)

---

## Overview

ParkingLite solves the "last 100 meters" problem in smart city parking:
**how to add real-time occupancy detection to a small lot (≤8 slots) without
cloud infrastructure, expensive hardware, or reliable Internet access.**

The system runs entirely on a pair of ESP32 microcontrollers costing
**~31,000 VND per slot** (~$1.25 USD). A camera node classifies each parking slot
locally and broadcasts a 2-byte status packet over ESP-NOW (no router, no Wi-Fi
association). A gateway node receives and displays results on a PyQt5 dashboard.

---

## Key Results

| Metric | Value |
|---|---|
| F1 score (MAD method, 54K evaluations) | **0.985** |
| Accuracy on real parking lot images | **100%** (Combined Ensemble) |
| Classification latency (ROI + Norm + Ensemble) | **3.4 ms** |
| Active duty per 5 s cycle | **1.2%** (60.6 ms active) |
| SRAM usage | **50.7 / 320 KB** |
| Flash usage | **432 / 3145 KB** |
| Cost per slot | **~31,000 VND** |
| Bandwidth per day (adaptive protocol) | **2.4 KB** (−99.9% vs MQTT) |
| Scans per day (adaptive) | **6,956** (−60% vs fixed 5 s polling) |
| Edge latency | **7 ms** end-to-end |
| Internet required | **No** |
| FPU / GPU required | **No** |

---

## System Architecture

```
┌─────────────────────┐      ESP-NOW (2 B/event)      ┌──────────────────────┐
│  Sensor Node ×N     │ ─────────────────────────────▶ │  Gateway Node        │
│  ESP32-CAM          │      broadcast, no pairing     │  ESP32 Dev Board     │
│  OV2640 camera      │      range ~200 m LOS          │  USB serial → PC     │
│                     │                                 │                      │
│  ① Capture (50 ms)  │                                 │  parking_monitor.py  │
│  ② Classify (3.4ms) │                                 │  PyQt5 dashboard     │
│  ③ TX if changed    │                                 │  JSON output         │
│  ④ Deep Sleep       │                                 └──────────────────────┘
└─────────────────────┘

Three-tier pipeline (camera node):
  Raw Image → [Tier 1] Capture & Grayscale
            → [Tier 2] ROI Extract + 3-Stage Normalization
            → [Tier 3] Combined Ensemble Classify → ESP-NOW TX
```

---

## Main Contributions

### Contribution #1 — Combined Ensemble (7-method weighted voting)

Runs entirely in **100% integer math** (×10 fixed-point) — no float, no CNN, no FPU.

```
Score(x) = Σ_{k=1}^{7} w_k · 1[m_k(x̃) > θ_k]  ≥  50  →  "Occupied"
```

where `x̃` = ROI after 3-stage brightness normalization.

| Sub-method | Weight | Feature domain |
|---|---|---|
| MAD (Mean Absolute Deviation) | 15% | Pixel-level diff vs reference |
| Gaussian-weighted MAD | 15% | Center-focused MAD (robust to alignment) |
| Block MAD (4×4 grid) | 10% | Spatial localization |
| Percentile P75 MAD | 20% | Robust to partial occlusion |
| Max Block MAD | 20% | Max-changed block (detects corner parking) |
| Histogram Intersection | 10% | Global texture distribution |
| Variance Ratio | 10% | Texture energy vs empty reference |

**3-Stage brightness normalization** (key to robustness across weather):
1. Mean-shift compensation (auto-exposure OV2640)
2. Local ROI normalization (shadow removal)
3. Per-pixel clamp to [0, 255]

**Ablation results** (cumulative configurations):

| Config | Classify (ms) | Scans/day | Note |
|---|---|---|---|
| (A) Baseline Float | ~32.1 | 17,280 | Reference |
| (B) + Integer Math | **3.2** | 17,280 | ~10× faster |
| (C) + 3-Stage Norm | 3.4 | 17,280 | FP → 0.4% |
| **(D) + Adaptive Protocol** | **3.4** | **6,956** | **−60% scans** |

---

### Contribution #2 — LiteComm Adaptive Protocol (FSM)

Event-driven transmission controlled by **ensemble confidence**, not a fixed timer.

```
IDLE (30 s) → SENSE (5 s) → BURST (3-frame confirm) → SLEEP (static scene)
                                      ↓ bitmap changed
                                   TX 2-byte payload
```

- **Payload**: `{ node_id (1B), bitmap (1B) }` — broadcast, fire-and-forget
- **vs MQTT 5 s**: 180 B × 17,280 = ~3.1 MB payload/day → **2 B × ~1,200 events = 2.4 KB/day** (−99.9%)
- **Cross-layer**: ensemble confidence gates BURST→SLEEP transition

---

## Comparison with Existing Solutions

| Criterion | Ultrasonic | Cloud AI (YOLOv8) | Edge CNN (Jetson) | IoT+MQTT (RPi4) | **ParkingLite** |
|---|---|---|---|---|---|
| Cost/slot | 500K–1M VND | >500K + cloud/mo | 400K+ | 200K+ | **~31K VND** |
| Internet | Not needed | Required | Optional | Not needed | **Not needed** |
| FPU/GPU | None | GPU | FPU+NPU | FPU | **None** |
| Accuracy | 85–95% | 98%+ | 95%+ | — | **98.5%/100%** |
| Edge latency | <1 ms | 200+ ms | 50+ ms | <10 ms | **7 ms** |
| BW/day | ~10 KB | MB video | MB meta | KB JSON | **2.4 KB** |
| RAM (device) | 0.1 KB | GB | 100+ KB | 512 MB+ | **50.7 KB** |

---

## Repository Structure

```
Production/
  sensor_node/          ← ESP32-CAM firmware (main deliverable)
    sensor_node.ino       ← Main sketch — classification + ESP-NOW TX
    roi_classifier.cpp    ← 11 methods, 100% integer math
    roi_classifier.h      ← API, constants, calibration structs
    camera_config.h       ← OV2640 pin mapping (AI-Thinker)
    config.h              ← NODE_ID, lot config
  gateway/              ← ESP32 Dev Board receiver firmware
    gateway.ino
  tools/                ← Desktop utilities
    flash.sh              ← Arduino-CLI flash helper
    local_detector.py     ← Python port of all 11 methods (offline testing)
    generate_debug_images.py  ← 7-scenario debug overlay generator
    quick_snap.py         ← Capture single frame from sensor node
  report_assets/        ← Research figures (charts, diagrams, real photos)
  README.md             ← Hardware setup & flash guide

Tools/
  parking_monitor/      ← PyQt5 real-time dashboard
    parking_monitor.py    ← Main app — receives gateway serial, displays grid
    README.md

requirements.txt        ← Python dependencies
```

---

## Hardware Requirements

| Component | Model | Unit cost (VND) |
|---|---|---|
| Camera node | ESP32-CAM AI-Thinker (OV2640) | ~22,000 |
| Gateway | ESP32 Dev Board (any) | ~45,000 |
| Power (node) | 5 V USB adapter | ~15,000 |
| Mount | 3D-printed bracket or cable tie | ~5,000 |
| **Total per slot** | | **~31,000** |

> Gateway is shared: one gateway serves all nodes in a lot.  
> Gateway cost amortized across slots → negligible for ≥3 slots.

---

## Quick Start

### 1. Flash the sensor node

```bash
# Requires: arduino-cli + ESP32 core 3.3.x
cd Production/tools

# Flash sensor node (set NODE_ID in config.h first)
./flash.sh sensor /dev/cu.usbserial-XXXX

# Flash gateway
./flash.sh gateway /dev/cu.usbserial-YYYY
```

### 2. Calibrate (one-time, empty parking lot)

```
# Connect to sensor node at 115200 baud
# 1. Capture reference image
SNAP_COLOR

# 2. Run ROI calibration tool on the captured image
#    (draw bounding boxes over each slot, export config)
python Production/tools/local_detector.py --calibrate image.jpg

# 3. Load ROI config to device
ROI_LOAD 8  x0 y0 w0 h0  x1 y1 w1 h1  ...

# 4. Calibrate with empty lot
CAL
```

### 3. Run the monitor dashboard

```bash
pip install -r requirements.txt
python Tools/parking_monitor/parking_monitor.py --port /dev/cu.usbserial-YYYY
```

### 4. Test offline (without hardware)

```bash
# Test all 11 methods on a local image
python Production/tools/local_detector.py --image path/to/parking.jpg \
       --roi-config Production/sensor_node/roi_config_example.json

# Generate 7-scenario debug overlay
python Production/tools/generate_debug_images.py
```

---

## Research Context

**Project:** Giải pháp Quản lý Bãi đỗ xe Thông minh cho Quy mô Nhỏ và Chi phí Thấp  
**Program:** Nghiên Cứu Khoa Học Sinh Viên 2025–2026  
**Institution:** Khoa Hệ thống Thông tin · Trường Công nghệ Thông tin · Đại học Phenikaa

**Team:**
| Name | Role |
|---|---|
| Vũ Đăng Khoa | Team lead, firmware & algorithm |
| Tiêu Công Tuấn | Hardware integration |
| Trịnh Văn Toàn | Data collection & evaluation |
| Phan Vũ Hoài Nam | Dashboard & visualization |

**Supervisor:** TS. Phạm Ngọc Hưng — Khoa Hệ thống Thông tin

---

## Results Summary

**F1 score across 11 methods** (54,000 ROI evaluations, 16-slot configuration):

| Rank | Method | F1 | Accuracy |
|---|---|---|---|
| 1 | Reference Frame MAD | **0.985** | 100% |
| 1 | Gaussian MAD | 0.985 | 100% |
| 1 | Block MAD (4×4) | 0.985 | 100% |
| 1 | Percentile P75 MAD | 0.985 | 100% |
| 1 | Max Block MAD | 0.985 | 100% |
| 1 | Histogram Intersection | 0.985 | 100% |
| 1 | Variance Ratio | 0.985 | 100% |
| 1 | **Combined Ensemble** | **0.985** | **100%** |
| 9 | Edge Density (no calib.) | 0.765 | 68.8% |

All calibration-based methods achieve **100% accuracy on real parking lot images**
across 7 lighting/weather scenarios (sunny, overcast, light rain, heavy rain,
fog, evening, night rain).

See [Production/report_assets/](Production/report_assets/) for full evaluation figures.

---

## License

MIT License — see [LICENSE](LICENSE) for details.

> Contact: `22010357@st.phenikaa-uni.edu.vn`  
> GitHub: [Vudangkhoa0910/Smart_Parking](https://github.com/Vudangkhoa0910/Smart_Parking)

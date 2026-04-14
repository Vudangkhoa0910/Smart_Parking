# Smart Parking — ParkingLite

> Edge-AI parking detection system based on ESP32-CAM + ESP-NOW wireless  
> Phenikaa University · NCKH Research 2025–2026

## System Overview

ParkingLite detects parking slot occupancy using computer vision on low-cost ESP32-CAM hardware. Results are transmitted wirelessly via ESP-NOW (no router required) to a gateway node that outputs JSON for integration with monitoring software.

**Key metrics:** F1 = 0.985 · 100% accuracy on real parking lot images · <2ms classification per frame · Runs on 4MB ESP32 with PSRAM

## Repository Structure

```
Production/           ← Production-ready firmware (START HERE)
  sensor_node/        ← ESP32-CAM firmware
  gateway/            ← ESP32 Dev Board receiver
  tools/              ← Flash scripts and utilities
  README.md           ← Hardware setup guide

ROI/                  ← ROI calibration tool (Python desktop app)
Tools/                ← Parking monitor app (PyQt5)
Simulation/           ← Docker-based simulation environment
build_esp32/          ← Development / experimental firmware
Docs/                 ← Project documentation
Images/               ← Sample images (6 lighting conditions)
Prompt/               ← AI assistant context files
```

## Quick Start

See [Production/README.md](Production/README.md) for full hardware setup instructions.

```bash
# Flash sensor node
cd Production/tools && ./flash.sh sensor /dev/cu.usbserial-110

# Flash gateway
./flash.sh gateway /dev/cu.usbserial-10

# Capture image
python quick_snap.py
```

## Requirements

- arduino-cli + ESP32 core 3.3.x
- Python 3.9+ (for tools and monitor app)
- ESP32-CAM AI-Thinker + ESP32 Dev Board

#!/usr/bin/env bash
# flash.sh — ParkingLite Firmware Flash Tool
# Usage: ./flash.sh [sensor|gateway] [port]
#
# Examples:
#   ./flash.sh sensor /dev/cu.usbserial-110   # Flash ESP32-CAM
#   ./flash.sh gateway /dev/cu.usbserial-10   # Flash ESP32 Dev
#
# Requires: arduino-cli installed and in PATH
#   Install: https://arduino.github.io/arduino-cli/
#   Board package: arduino-cli core install esp32:esp32
#
# ParkingLite v1.0 — Phenikaa University NCKH 2025-2026

set -e

BOARD_CAM="esp32:esp32:esp32cam"
BOARD_GW="esp32:esp32:esp32"
BAUD="115200"         # Stable baud for AI-Thinker USB-Serial adapters

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SENSOR_DIR="$SCRIPT_DIR/../sensor_node"
GATEWAY_DIR="$SCRIPT_DIR/../gateway"

# ─── Parse arguments ────────────────────────────────────────────────
TARGET="${1:-}"
PORT="${2:-}"

usage() {
    echo "Usage: $0 [sensor|gateway] [port]"
    echo ""
    echo "  sensor   Flash ESP32-CAM sensor node firmware"
    echo "  gateway  Flash ESP32 Dev board gateway firmware"
    echo ""
    echo "  port     Serial port (e.g., /dev/cu.usbserial-110)"
    echo "           Omit to list available ports"
    exit 1
}

if [[ -z "$TARGET" ]]; then
    usage
fi

# ─── List ports if not specified ────────────────────────────────────
if [[ -z "$PORT" ]]; then
    echo "Available serial ports:"
    arduino-cli board list 2>/dev/null || ls /dev/cu.usb* /dev/ttyUSB* 2>/dev/null || true
    echo ""
    echo "Re-run with port specified, e.g.:"
    echo "  $0 $TARGET /dev/cu.usbserial-110"
    exit 0
fi

# ─── Flash ──────────────────────────────────────────────────────────
case "$TARGET" in
    sensor)
        echo "╔══════════════════════════════════╗"
        echo "║  Flashing Sensor Node (ESP32-CAM) ║"
        echo "╚══════════════════════════════════╝"
        echo "  Sketch : $SENSOR_DIR"
        echo "  Board  : $BOARD_CAM"
        echo "  Port   : $PORT"
        echo "  Baud   : $BAUD"
        echo ""
        arduino-cli compile \
            --fqbn "$BOARD_CAM" \
            "$SENSOR_DIR"
        arduino-cli upload \
            --fqbn "$BOARD_CAM" \
            --port "$PORT" \
            --upload-property upload.speed=$BAUD \
            "$SENSOR_DIR"
        echo ""
        echo "✓ Sensor node flashed successfully"
        echo "  Monitor: arduino-cli monitor -p $PORT --config baudrate=115200"
        ;;
    gateway)
        echo "╔══════════════════════════════════╗"
        echo "║  Flashing Gateway (ESP32 Dev)     ║"
        echo "╚══════════════════════════════════╝"
        echo "  Sketch : $GATEWAY_DIR"
        echo "  Board  : $BOARD_GW"
        echo "  Port   : $PORT"
        echo ""
        arduino-cli compile \
            --fqbn "$BOARD_GW" \
            "$GATEWAY_DIR"
        arduino-cli upload \
            --fqbn "$BOARD_GW" \
            --port "$PORT" \
            "$GATEWAY_DIR"
        echo ""
        echo "✓ Gateway flashed successfully"
        echo "  Monitor: arduino-cli monitor -p $PORT --config baudrate=115200"
        ;;
    *)
        echo "Unknown target: $TARGET"
        usage
        ;;
esac

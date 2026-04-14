/*
 * config.h — ParkingLite Sensor Node Configuration
 *
 * Copy this file and edit for EACH device before flashing.
 * All hardware-specific parameters are centralized here.
 *
 * ParkingLite v1.0 — Phenikaa University NCKH 2025-2026
 */

#ifndef CONFIG_H
#define CONFIG_H

// ═══════════════════════════════════════════════════════════════════
//  NODE IDENTITY
//  Change these per device. NODE_ID must be unique in the parking lot.
// ═══════════════════════════════════════════════════════════════════

#define NODE_ID     0x01    // Unique node ID: 0x01–0xFF (1 per camera)
#define LOT_ID      0x01    // Parking lot this node belongs to

// ═══════════════════════════════════════════════════════════════════
//  SCAN PARAMETERS
// ═══════════════════════════════════════════════════════════════════

#define SCAN_INTERVAL_MS  5000    // Periodic scan interval (ms). Range: 1000–60000
#define DEFAULT_METHOD    10      // Classification method (0–10). See roi_classifier.h
                                  //   0  = edge_density (no calibration, Acc=68.8%)
                                  //   2  = ref_frame_mad (calibrated, Acc=100%)
                                  //  10  = combined (calibrated, Acc=100%) ← recommended

// ═══════════════════════════════════════════════════════════════════
//  SLOT CONFIGURATION
//  Adjust ROI coordinates to match your camera angle and lot layout.
//  Coordinate system: origin at top-left of 320×240 grayscale frame.
//  Each ROI = rectangular region covering one parking slot.
// ═══════════════════════════════════════════════════════════════════

#define N_SLOTS   8   // Number of active slots (max 8 per node)

// Default: 2×4 grid in 320×240 frame
// Layout:  [0][1][2][3]
//          [4][5][6][7]
// Calibrate with the ROI calibration tool (ROI/app/) for best accuracy.
static roi_rect_t SLOT_ROIS[MAX_SLOTS] = {
    // Row 1 (top): slots 0–3
    { .x =  10, .y =  30, .w = 60, .h = 80 },  // Slot 0
    { .x =  80, .y =  30, .w = 60, .h = 80 },  // Slot 1
    { .x = 150, .y =  30, .w = 60, .h = 80 },  // Slot 2
    { .x = 220, .y =  30, .w = 60, .h = 80 },  // Slot 3
    // Row 2 (bottom): slots 4–7
    { .x =  10, .y = 130, .w = 60, .h = 80 },  // Slot 4
    { .x =  80, .y = 130, .w = 60, .h = 80 },  // Slot 5
    { .x = 150, .y = 130, .w = 60, .h = 80 },  // Slot 6
    { .x = 220, .y = 130, .w = 60, .h = 80 },  // Slot 7
};

// ═══════════════════════════════════════════════════════════════════
//  ESP-NOW CHANNEL
//  Must match the Gateway. Use 1–13. Default 1 (most compatible).
// ═══════════════════════════════════════════════════════════════════

#define ESPNOW_CHANNEL  1   // Wi-Fi channel for ESP-NOW communication

#endif // CONFIG_H

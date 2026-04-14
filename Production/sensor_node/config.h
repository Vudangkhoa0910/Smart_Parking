/*
 * config.h — ParkingLite Sensor Node Configuration
 *
 * Per-device identity: edit NODE_ID before flashing each camera.
 * ROI layout: configure at runtime via Serial (saved to NVS flash).
 *
 * Deployment workflow:
 *   1. Set NODE_ID/LOT_ID → flash once
 *   2. SNAP_COLOR → capture parking lot view
 *   3. ROI_LOAD x y w h n (per slot) → sent from PC calibration tool
 *   4. CAL → calibrate with empty lot → saved to NVS
 *   5. Done. Device runs autonomously. No re-flash needed.
 *
 * ParkingLite v1.1 — Phenikaa University NCKH 2025-2026
 */

#ifndef CONFIG_H
#define CONFIG_H

// ═══════════════════════════════════════════════════════════════════
//  NODE IDENTITY — change per device before flash
// ═══════════════════════════════════════════════════════════════════

#define NODE_ID     0x01    // Unique node ID: 0x01–0xFF
#define LOT_ID      0x01    // Parking lot this node belongs to

// ═══════════════════════════════════════════════════════════════════
//  SCAN PARAMETERS
// ═══════════════════════════════════════════════════════════════════

#define SCAN_INTERVAL_MS  5000    // Periodic scan interval (ms). Range: 1000–60000
#define DEFAULT_METHOD    10      // Classification method (0–10). See roi_classifier.h

// ═══════════════════════════════════════════════════════════════════
//  SLOT CONFIGURATION
//  These are DEFAULTS used only if no ROI config exists in NVS.
//  Runtime ROI config is loaded from NVS (set via ROI_LOAD command).
//  Coordinate system: origin at top-left of 320×240 grayscale frame.
// ═══════════════════════════════════════════════════════════════════

#define N_SLOTS_DEFAULT   8   // Default number of slots (overridden by NVS)

// Fallback ROI layout: 2×4 grid for 8 slots in 320×240 frame
// These are only used on FIRST boot (before ROI_LOAD configures NVS).
static roi_rect_t DEFAULT_SLOT_ROIS[MAX_SLOTS] = {
    { .x =  10, .y =  30, .w = 60, .h = 80 },  // Slot 0
    { .x =  80, .y =  30, .w = 60, .h = 80 },  // Slot 1
    { .x = 150, .y =  30, .w = 60, .h = 80 },  // Slot 2
    { .x = 220, .y =  30, .w = 60, .h = 80 },  // Slot 3
    { .x =  10, .y = 130, .w = 60, .h = 80 },  // Slot 4
    { .x =  80, .y = 130, .w = 60, .h = 80 },  // Slot 5
    { .x = 150, .y = 130, .w = 60, .h = 80 },  // Slot 6
    { .x = 220, .y = 130, .w = 60, .h = 80 },  // Slot 7
};

// ═══════════════════════════════════════════════════════════════════
//  ESP-NOW SETTINGS
// ═══════════════════════════════════════════════════════════════════

#define ESPNOW_CHANNEL      1       // Wi-Fi channel (1–13, must match Gateway)
#define ESPNOW_TX_POWER     20      // TX power in dBm (2–20). Default 20 = max
#define HEARTBEAT_INTERVAL  15000   // Heartbeat broadcast every 15s (even if no change)
#define TX_RETRY_COUNT      2       // Retry on send failure (0 = no retry)

#endif // CONFIG_H

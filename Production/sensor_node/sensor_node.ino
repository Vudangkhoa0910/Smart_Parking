/*
 * sensor_node.ino — ParkingLite Sensor Node v1.1
 *
 * Hardware:  ESP32-CAM AI-Thinker (OV2640 + 4MB PSRAM)
 * Framework: Arduino + ESP32 core 3.3.x
 * Protocol:  ESP-NOW broadcast (no router required)
 *
 * Algorithm: Integer MAD — 11-method ensemble, F1=0.985 on 54K samples
 *
 * Deployment workflow (no re-flash needed per lot):
 *   1. Flash firmware once (with NODE_ID set in config.h)
 *   2. SNAP_COLOR → capture the lot from camera view
 *   3. Use ROI calibration tool to draw slot regions on image
 *   4. ROI_LOAD n x0 y0 w0 h0 x1 y1 w1 h1 ... → saves to NVS
 *   5. CAL → calibrate classifier with empty lot → saved to NVS
 *   6. Device runs autonomously. Survives reboots.
 *
 * ESP-NOW payload (8 bytes, versioned):
 *   { version, lot_id, node_id, n_slots, bitmap, seq, tx_power, reserved }
 *   + periodic HEARTBEAT even when no change (for link quality monitoring)
 *
 * Serial commands (115200 baud):
 *   CAL            — Calibrate classifier with empty lot → NVS
 *   RESET          — Clear calibration
 *   STATUS         — Print system status
 *   METHOD X       — Set classification method (0–10)
 *   INTERVAL X     — Set scan interval ms (1000–60000)
 *   ROI X Y W H I  — Set single ROI slot I
 *   ROI_LOAD N x0 y0 w0 h0 ... — Bulk-load N slots → NVS
 *   ROI_GET        — Print ROI config as JSON
 *   ROI_CLEAR      — Clear ROI config from NVS (reverts to defaults)
 *   SLOTS_GET      — Print last classification results as JSON
 *   SNAP           — Grayscale JPEG stream
 *   SNAP_COLOR     — Color SVGA JPEG stream
 *   SNAP_XGA / SNAP_UXGA — Higher resolution color
 *   FLASH 0/1/2    — LED control
 *   PING           — Connectivity check
 *
 * ParkingLite v1.1 — Phenikaa University NCKH 2025-2026
 */

#include <Arduino.h>
#include "esp_camera.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "img_converters.h"
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

#include "camera_config.h"
#include "roi_classifier.h"
#include "config.h"

static const char *TAG = "SENSOR_NODE";

// ─── NVS Keys ───────────────────────────────────────────────────────
#define NVS_NAMESPACE     "parklite"
#define NVS_KEY_N_SLOTS   "n_slots"
#define NVS_KEY_ROI_DATA  "roi_data"

// ─── ESP-NOW Payload v2 (8 bytes, backwards-detectable) ─────────────
#define PAYLOAD_VERSION 2

typedef struct __attribute__((packed)) {
    uint8_t  version;     // Protocol version (2)
    uint8_t  lot_id;      // Parking lot ID
    uint8_t  node_id;     // Sensor node ID
    uint8_t  n_slots;     // Active slot count (1–8)
    uint8_t  bitmap;      // Occupancy bitmap (bit per slot)
    uint8_t  seq;         // Sequence number (0–255 wrapping)
    int8_t   tx_power;    // TX power in dBm (for receiver RSSI calibration)
    uint8_t  flags;       // Bit 0: is_heartbeat, Bit 1: has_calibration
} espnow_payload_v2_t;

static espnow_payload_v2_t tx_payload;
static esp_now_peer_info_t peer_info;
static const uint8_t BROADCAST_ADDR[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// ─── Runtime State ──────────────────────────────────────────────────
static uint8_t  current_bitmap   = 0;
static uint8_t  last_tx_bitmap   = 0xFF;   // 0xFF forces first transmission
static uint8_t  classify_method  = DEFAULT_METHOD;
static uint32_t scan_interval    = SCAN_INTERVAL_MS;
static uint32_t last_scan_ms     = 0;
static uint32_t last_heartbeat_ms = 0;
static uint32_t frame_count      = 0;
static uint8_t  tx_seq           = 0;      // Wrapping sequence number
static uint32_t tx_success_count = 0;
static uint32_t tx_fail_count    = 0;
static uint8_t  n_slots          = N_SLOTS_DEFAULT;
static classify_result_t slot_results[MAX_SLOTS];
static roi_rect_t slot_rois[MAX_SLOTS];

// ─── Method Name Table ──────────────────────────────────────────────
static const char *METHOD_NAMES[NUM_METHODS] = {
    "edge_density",   // 0 — no calibration, Acc=68.8%
    "bg_relative",    // 1 — Acc=96.1%
    "ref_frame_mad",  // 2 — Acc=100.0% ← simple recommended
    "hybrid",         // 3 — Acc=100.0%
    "gaussian_mad",   // 4 — Acc=100.0%
    "block_mad",      // 5 — Acc=100.0%
    "percentile_mad", // 6 — Acc=100.0%
    "max_block",      // 7 — Acc=100.0%
    "histogram",      // 8 — Acc=100.0%
    "variance_ratio", // 9 — Acc=100.0%
    "combined",       // 10 — Acc=100.0% ← most robust
};

// ─── Forward Declarations ───────────────────────────────────────────
static bool init_camera(void);
static void process_frame(void);
static void espnow_broadcast(uint8_t bitmap, bool is_heartbeat);
static void handle_serial_command(void);
static void print_status(void);
static void led_flash(uint8_t mode);
static bool capture_color_jpeg(framesize_t fsize, int quality, const char *label);
static bool load_roi_from_nvs(void);
static bool save_roi_to_nvs(void);
static void clear_roi_nvs(void);


// ═════════════════════════════════════════════════════════════════════
//  ESP-NOW Callbacks
// ═════════════════════════════════════════════════════════════════════

static void on_data_sent(const esp_now_send_info_t *tx_info, esp_now_send_status_t status) {
    if (status == ESP_NOW_SEND_SUCCESS) {
        Serial.println("         [TX_OK] Broadcast delivered");
    } else {
        Serial.println("         [TX_FAIL] Broadcast not ACKed");
    }
}


// ═════════════════════════════════════════════════════════════════════
//  Setup
// ═════════════════════════════════════════════════════════════════════

void setup() {
    Serial.begin(115200);
    Serial.println();
    Serial.println("╔═══════════════════════════════════╗");
    Serial.println("║   ParkingLite Sensor Node v1.1    ║");
    Serial.println("║   ESP32-CAM + ESP-NOW              ║");
    Serial.println("╚═══════════════════════════════════╝");
    Serial.println();

    // NVS for calibration + ROI persistence
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    // Load ROI from NVS (or use defaults from config.h)
    if (!load_roi_from_nvs()) {
        memcpy(slot_rois, DEFAULT_SLOT_ROIS, sizeof(slot_rois));
        n_slots = N_SLOTS_DEFAULT;
        Serial.printf("[ROI] Using defaults: %d slots (no NVS config found)\n", n_slots);
        Serial.println("      Use ROI_LOAD to set per-lot layout → saved to NVS");
    } else {
        Serial.printf("[ROI] Loaded from NVS: %d slots\n", n_slots);
    }

    // Camera
    if (!init_camera()) {
        Serial.println("[FATAL] Camera init failed — power cycle and retry");
        while (1) delay(1000);
    }
    Serial.println("[OK] Camera: QVGA 320x240 grayscale");

    // Classifier
    bool has_cal = classifier_init();
    if (has_cal) {
        Serial.println("[OK] Calibration loaded from NVS");
        Serial.printf("     Active method: %d (%s)\n", classify_method,
                      METHOD_NAMES[classify_method]);
    } else {
        Serial.println("[WARN] No calibration — using edge_density (method 0)");
        Serial.println("       Run 'CAL' with an empty lot to activate full accuracy");
    }

    // ESP-NOW
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    // Set TX power for range optimization
    esp_wifi_set_max_tx_power(ESPNOW_TX_POWER * 4);  // API uses 0.25 dBm units
    if (esp_now_init() != ESP_OK) {
        Serial.println("[FATAL] ESP-NOW init failed");
        while (1) delay(1000);
    }
    esp_now_register_send_cb(on_data_sent);
    memcpy(peer_info.peer_addr, BROADCAST_ADDR, 6);
    peer_info.channel = ESPNOW_CHANNEL;
    peer_info.encrypt = false;
    if (esp_now_add_peer(&peer_info) != ESP_OK) {
        Serial.println("[ERROR] Failed to add broadcast peer");
    } else {
        Serial.printf("[OK] ESP-NOW: ch=%d txpwr=%ddBm heartbeat=%ds\n",
                      ESPNOW_CHANNEL, ESPNOW_TX_POWER, HEARTBEAT_INTERVAL / 1000);
    }

    // LED ready indicator
    pinMode(FLASH_LED_PIN, OUTPUT);
    led_flash(2);

    Serial.printf("\n[READY] Node=0x%02X Lot=0x%02X Method=%d Interval=%ums Slots=%d\n\n",
                  NODE_ID, LOT_ID, classify_method, scan_interval, n_slots);
}


// ═════════════════════════════════════════════════════════════════════
//  Main Loop
// ═════════════════════════════════════════════════════════════════════

void loop() {
    uint32_t now = millis();

    if (Serial.available()) {
        handle_serial_command();
    }

    if (now - last_scan_ms >= scan_interval) {
        last_scan_ms = now;
        process_frame();
    }

    // Periodic heartbeat (even when bitmap unchanged, for link quality)
    if (now - last_heartbeat_ms >= HEARTBEAT_INTERVAL) {
        last_heartbeat_ms = now;
        espnow_broadcast(current_bitmap, true);
    }

    delay(10);
}


// ═════════════════════════════════════════════════════════════════════
//  Camera Initialization
// ═════════════════════════════════════════════════════════════════════

static bool init_camera(void) {
    camera_config_t config = get_camera_config();
    delay(300);

    esp_err_t err = ESP_FAIL;
    for (int attempt = 1; attempt <= 20; attempt++) {
        err = esp_camera_init(&config);
        if (err == ESP_OK) {
            Serial.printf("[OK] Camera init on attempt %d/20\n", attempt);
            break;
        }
        Serial.printf("[WARN] Camera attempt %d/20 failed: 0x%x\n", attempt, err);
        esp_camera_deinit();
        delay(150);
    }
    if (err != ESP_OK) return false;

    sensor_t *s = esp_camera_sensor_get();
    if (s) {
        s->set_brightness(s, 1);
        s->set_contrast(s, 2);
        s->set_saturation(s, 0);
        s->set_sharpness(s, 2);
        s->set_gainceiling(s, GAINCEILING_4X);
        s->set_whitebal(s, 1);
        s->set_awb_gain(s, 1);
        s->set_gain_ctrl(s, 1);
        s->set_exposure_ctrl(s, 1);
        s->set_aec2(s, 1);
        s->set_ae_level(s, 0);
        s->set_denoise(s, 1);
    }

    // Discard warm-up frames so AEC/AWB converge
    for (int i = 0; i < 5; i++) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (fb) esp_camera_fb_return(fb);
        delay(100);
    }
    return true;
}


// ═════════════════════════════════════════════════════════════════════
//  Frame Classification
// ═════════════════════════════════════════════════════════════════════

static void process_frame(void) {
    uint32_t t0 = millis();

    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        ESP_LOGE(TAG, "Frame capture failed");
        return;
    }
    if (fb->format != PIXFORMAT_GRAYSCALE) {
        ESP_LOGE(TAG, "Unexpected pixel format: %d", fb->format);
        esp_camera_fb_return(fb);
        return;
    }

    frame_count++;

    // Fallback to edge_density when not calibrated
    uint8_t effective_method = classify_method;
    if (effective_method > 0 && !classifier_is_calibrated()) {
        effective_method = 0;
    }

    current_bitmap = classify_all_slots(
        fb->buf, fb->width,
        slot_rois, n_slots,
        effective_method, slot_results
    );
    esp_camera_fb_return(fb);

    uint32_t elapsed_ms = millis() - t0;

    // Count occupied slots
    uint8_t n_occupied = 0;
    for (int i = 0; i < n_slots; i++) {
        if (slot_results[i].prediction) n_occupied++;
    }

    Serial.printf("[%6u] Bitmap=0b", frame_count);
    for (int i = n_slots - 1; i >= 0; i--) Serial.print((current_bitmap >> i) & 1);
    Serial.printf(" (0x%02X) | %ums | M%d | %d/%d occupied\n",
                  current_bitmap, elapsed_ms, effective_method, n_occupied, n_slots);

    // Transmit on change only
    if (current_bitmap != last_tx_bitmap) {
        Serial.printf("         → CHANGE: 0x%02X → 0x%02X\n", last_tx_bitmap, current_bitmap);
        espnow_broadcast(current_bitmap, false);
        last_tx_bitmap = current_bitmap;
    }
}


// ═════════════════════════════════════════════════════════════════════
//  ESP-NOW Transmission
// ═════════════════════════════════════════════════════════════════════

static void espnow_broadcast(uint8_t bitmap, bool is_heartbeat) {
    tx_payload.version  = PAYLOAD_VERSION;
    tx_payload.lot_id   = LOT_ID;
    tx_payload.node_id  = NODE_ID;
    tx_payload.n_slots  = n_slots;
    tx_payload.bitmap   = bitmap;
    tx_payload.seq      = tx_seq++;
    tx_payload.tx_power = ESPNOW_TX_POWER;
    tx_payload.flags    = (is_heartbeat ? 0x01 : 0x00)
                        | (classifier_is_calibrated() ? 0x02 : 0x00);

    uint8_t retries = TX_RETRY_COUNT + 1;
    esp_err_t result = ESP_FAIL;
    for (uint8_t r = 0; r < retries; r++) {
        result = esp_now_send(BROADCAST_ADDR, (uint8_t *)&tx_payload, sizeof(tx_payload));
        if (result == ESP_OK) break;
        delay(5);
    }

    if (result == ESP_OK) {
        tx_success_count++;
        Serial.printf("         [TX] seq=%u %s Node=0x%02X Bitmap=0x%02X (%u/%u ok)\n",
                      tx_payload.seq, is_heartbeat ? "HB" : "EVT",
                      NODE_ID, bitmap, tx_success_count,
                      tx_success_count + tx_fail_count);
    } else {
        tx_fail_count++;
        Serial.printf("         [TX_FAIL] seq=%u err=0x%x (%u/%u ok)\n",
                      tx_payload.seq, result, tx_success_count,
                      tx_success_count + tx_fail_count);
    }
}


// ═════════════════════════════════════════════════════════════════════
//  Color JPEG Capture (debug / research commands)
// ═════════════════════════════════════════════════════════════════════

static bool capture_color_jpeg(framesize_t fsize, int quality, const char *label) {
    Serial.printf("[%s] Switching to color mode...\n", label);
    esp_camera_deinit();
    delay(200);

    camera_config_t hd_cfg = get_color_camera_config(fsize, quality);
    esp_err_t err = ESP_FAIL;
    for (int attempt = 1; attempt <= 10; attempt++) {
        err = esp_camera_init(&hd_cfg);
        if (err == ESP_OK) {
            Serial.printf("[%s] Color init OK on attempt %d\n", label, attempt);
            break;
        }
        esp_camera_deinit();
        delay(150);
    }

    bool success = false;
    if (err == ESP_OK) {
        sensor_t *s = esp_camera_sensor_get();
        if (s) {
            s->set_brightness(s, 0);
            s->set_contrast(s, 1);
            s->set_saturation(s, 1);
            s->set_sharpness(s, 1);
            s->set_whitebal(s, 1);
            s->set_awb_gain(s, 1);
            s->set_wb_mode(s, 0);
            s->set_exposure_ctrl(s, 1);
            s->set_aec2(s, 1);
            s->set_ae_level(s, 0);
            s->set_gain_ctrl(s, 1);
            s->set_gainceiling(s, GAINCEILING_4X);
            s->set_denoise(s, 1);
            s->set_special_effect(s, 0);
            s->set_lenc(s, 1);
            s->set_raw_gma(s, 1);
        }
        delay(80);

        // Discard warm-up frames
        for (int i = 0; i < 3; i++) {
            camera_fb_t *fb = esp_camera_fb_get();
            if (fb) esp_camera_fb_return(fb);
            delay(80);
        }

        camera_fb_t *fb = esp_camera_fb_get();
        if (fb && fb->format == PIXFORMAT_JPEG && fb->len > 0) {
            Serial.printf("[SNAP_START] size=%u width=%d height=%d color=1\n",
                          (unsigned)fb->len, fb->width, fb->height);
            delay(50);

            // Chunked write: 1024 bytes + 3ms gap to avoid USB-FIFO overflow
            const size_t CHUNK = 1024;
            for (size_t i = 0; i < fb->len; i += CHUNK) {
                size_t len = (fb->len - i < CHUNK) ? (fb->len - i) : CHUNK;
                Serial.write(fb->buf + i, len);
                Serial.flush();
                delay(3);
            }
            Serial.println();
            Serial.println("[SNAP_END]");
            Serial.printf("[%s] OK %dx%d %u bytes\n",
                          label, fb->width, fb->height, (unsigned)fb->len);
            esp_camera_fb_return(fb);
            success = true;
        } else {
            Serial.printf("[%s] Capture failed (fmt=%d len=%u)\n",
                          label, fb ? fb->format : -1, fb ? (unsigned)fb->len : 0);
            if (fb) esp_camera_fb_return(fb);
        }
    } else {
        Serial.printf("[%s] Color init FAILED\n", label);
    }

    // Revert to grayscale
    esp_camera_deinit();
    delay(200);
    if (!init_camera()) {
        Serial.println("[FATAL] Failed to revert to grayscale mode");
    }
    return success;
}


// ═════════════════════════════════════════════════════════════════════
//  Serial Command Handler
// ═════════════════════════════════════════════════════════════════════

static void handle_serial_command(void) {
    String line = Serial.readStringUntil('\n');
    line.trim();
    line.toUpperCase();

    if (line == "CAL") {
        Serial.println("\n[CAL] Ensure the parking lot is EMPTY, then wait...");
        led_flash(1);
        camera_fb_t *fb = esp_camera_fb_get();
        if (fb && fb->format == PIXFORMAT_GRAYSCALE) {
            bool ok = classifier_calibrate(fb->buf, fb->width, slot_rois, n_slots);
            Serial.println(ok ? "[CAL] OK — calibration saved to NVS"
                              : "[CAL] FAILED — check NVS flash");
            if (ok) Serial.println("      Recommended: METHOD 10 (combined)");
            esp_camera_fb_return(fb);
        } else {
            Serial.println("[CAL] Frame capture failed");
        }
        led_flash(0);
    }
    else if (line == "RESET") {
        classifier_reset_calibration();
        Serial.println("[RESET] Calibration cleared → falling back to method 0");
    }
    else if (line == "STATUS") {
        print_status();
    }
    else if (line.startsWith("METHOD ")) {
        int m = line.substring(7).toInt();
        if (m >= 0 && m <= 10) {
            classify_method = m;
            Serial.printf("[CONFIG] Method → %d (%s)\n", m, METHOD_NAMES[m]);
            if (m > 0 && !classifier_is_calibrated()) {
                Serial.println("         WARNING: method requires CAL first");
            }
        } else {
            Serial.println("[ERROR] Method must be 0–10");
        }
    }
    else if (line.startsWith("INTERVAL ")) {
        int ms = line.substring(9).toInt();
        if (ms >= 1000 && ms <= 60000) {
            scan_interval = (uint32_t)ms;
            Serial.printf("[CONFIG] Interval → %u ms\n", scan_interval);
        } else {
            Serial.println("[ERROR] Interval must be 1000–60000 ms");
        }
    }
    else if (line.startsWith("ROI ")) {
        int x, y, w, h, idx;
        if (sscanf(line.c_str(), "ROI %d %d %d %d %d", &x, &y, &w, &h, &idx) == 5) {
            if (idx >= 0 && idx < MAX_SLOTS && x >= 0 && y >= 0 && w > 0 && h > 0) {
                slot_rois[idx] = { (uint16_t)x, (uint16_t)y, (uint16_t)w, (uint16_t)h };
                Serial.printf("[ROI] Slot %d → (%d,%d,%d,%d)\n", idx, x, y, w, h);
            } else {
                Serial.println("[ERROR] ROI parameters out of range");
            }
        } else {
            Serial.println("[ERROR] Usage: ROI X Y W H INDEX");
        }
    }
    else if (line == "ROI_GET") {
        Serial.print("[ROI_JSON] {\"node\":"); Serial.print(NODE_ID);
        Serial.printf(",\"n_slots\":%d,\"source\":\"%s\",\"slots\":[",
                      n_slots, load_roi_from_nvs() ? "nvs" : "default");
        for (int i = 0; i < n_slots; i++) {
            Serial.printf("{\"i\":%d,\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d}",
                         i, slot_rois[i].x, slot_rois[i].y, slot_rois[i].w, slot_rois[i].h);
            if (i < n_slots - 1) Serial.print(",");
        }
        Serial.println("]}");
    }
    else if (line == "SLOTS_GET") {
        Serial.printf("[SLOTS_JSON] {\"node\":0x%02X,\"bitmap\":0x%02X,\"method\":%d,\"slots\":[",
                      NODE_ID, current_bitmap, classify_method);
        for (int i = 0; i < n_slots; i++) {
            Serial.printf("{\"i\":%d,\"occ\":%d,\"conf\":%d,\"raw\":%d}",
                         i, slot_results[i].prediction,
                         slot_results[i].confidence, slot_results[i].raw_metric);
            if (i < n_slots - 1) Serial.print(",");
        }
        Serial.println("]}");
    }
    else if (line.startsWith("ROI_LOAD ")) {
        // Bulk ROI load: ROI_LOAD N x0 y0 w0 h0 x1 y1 w1 h1 ...
        const char *p = line.c_str() + 9;
        int new_n = 0;
        int consumed = 0;
        if (sscanf(p, "%d%n", &new_n, &consumed) != 1 || new_n < 1 || new_n > MAX_SLOTS) {
            Serial.println("[ERROR] ROI_LOAD N x0 y0 w0 h0 ... (N=1–8)");
        } else {
            p += consumed;
            roi_rect_t tmp[MAX_SLOTS] = {};
            bool ok = true;
            for (int i = 0; i < new_n; i++) {
                int x, y, w, h;
                int c = 0;
                if (sscanf(p, " %d %d %d %d%n", &x, &y, &w, &h, &c) != 4
                    || x < 0 || y < 0 || w < 1 || h < 1
                    || x + w > 320 || y + h > 240) {
                    Serial.printf("[ERROR] Invalid ROI for slot %d\n", i);
                    ok = false;
                    break;
                }
                tmp[i] = { (uint16_t)x, (uint16_t)y, (uint16_t)w, (uint16_t)h };
                p += c;
            }
            if (ok) {
                n_slots = new_n;
                memcpy(slot_rois, tmp, sizeof(slot_rois));
                if (save_roi_to_nvs()) {
                    Serial.printf("[ROI_LOAD] %d slots saved to NVS ✓\n", n_slots);
                    for (int i = 0; i < n_slots; i++) {
                        Serial.printf("  Slot %d: (%d,%d,%d,%d)\n",
                                      i, slot_rois[i].x, slot_rois[i].y,
                                      slot_rois[i].w, slot_rois[i].h);
                    }
                    Serial.println("  → Run CAL to calibrate with empty lot");
                } else {
                    Serial.println("[ERROR] NVS write failed");
                }
            }
        }
    }
    else if (line == "ROI_CLEAR") {
        clear_roi_nvs();
        memcpy(slot_rois, DEFAULT_SLOT_ROIS, sizeof(slot_rois));
        n_slots = N_SLOTS_DEFAULT;
        Serial.printf("[ROI_CLEAR] Reverted to %d default slots\n", n_slots);
    }
    else if (line == "SNAP" || line == "SNAPSHOT") {
        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) { Serial.println("[SNAP_ERROR] Capture failed"); return; }

        uint8_t *jpg_buf = NULL;
        size_t   jpg_len = 0;
        bool ok = fmt2jpg(fb->buf, fb->len, fb->width, fb->height,
                          PIXFORMAT_GRAYSCALE, 10, &jpg_buf, &jpg_len);
        esp_camera_fb_return(fb);
        if (!ok || !jpg_buf) { Serial.println("[SNAP_ERROR] JPEG encode failed"); return; }

        Serial.printf("[SNAP_START] size=%u width=%d height=%d color=0\n",
                      (unsigned)jpg_len, 320, 240);
        delay(30);
        const size_t CHUNK = 1024;
        for (size_t i = 0; i < jpg_len; i += CHUNK) {
            size_t len = (jpg_len - i < CHUNK) ? (jpg_len - i) : CHUNK;
            Serial.write(jpg_buf + i, len);
            Serial.flush();
            delay(3);
        }
        Serial.println();
        Serial.println("[SNAP_END]");
        free(jpg_buf);
    }
    else if (line == "SNAP_COLOR" || line == "SNAP_HD") {
        capture_color_jpeg(FRAMESIZE_SVGA, 18, "SNAP_COLOR");
    }
    else if (line == "SNAP_XGA") {
        capture_color_jpeg(FRAMESIZE_XGA, 22, "SNAP_XGA");
    }
    else if (line == "SNAP_UXGA") {
        capture_color_jpeg(FRAMESIZE_UXGA, 26, "SNAP_UXGA");
    }
    else if (line.startsWith("FLASH ")) {
        led_flash(line.substring(6).toInt());
    }
    else if (line == "PING") {
        Serial.println("[PONG]");
    }
    else if (line.length() > 0) {
        Serial.println("Commands: CAL | RESET | STATUS | PING");
        Serial.println("          METHOD 0-10 | INTERVAL ms");
        Serial.println("          ROI X Y W H I | ROI_GET | ROI_CLEAR");
        Serial.println("          ROI_LOAD N x0 y0 w0 h0 x1 y1 ...");
        Serial.println("          SLOTS_GET");
        Serial.println("          SNAP | SNAP_COLOR | SNAP_XGA | SNAP_UXGA");
        Serial.println("          FLASH 0/1/2");
    }
}


// ═════════════════════════════════════════════════════════════════════
//  Utilities
// ═════════════════════════════════════════════════════════════════════

static void print_status(void) {
    const char *mname = (classify_method < NUM_METHODS) ? METHOD_NAMES[classify_method] : "?";
    uint8_t n_occ = 0;
    for (int i = 0; i < n_slots; i++) if (slot_results[i].prediction) n_occ++;
    uint32_t total_tx = tx_success_count + tx_fail_count;
    uint8_t loss_pct = total_tx > 0 ? (uint8_t)(tx_fail_count * 100 / total_tx) : 0;

    Serial.println();
    Serial.println("╔══════════════════════════════════════════════╗");
    Serial.println("║    ParkingLite Sensor Node v1.1 — Status     ║");
    Serial.println("╠══════════════════════════════════════════════╣");
    Serial.printf( "║  Node ID:        0x%02X  Lot ID: 0x%02X          ║\n", NODE_ID, LOT_ID);
    Serial.printf( "║  Method:         %2d %-14s            ║\n", classify_method, mname);
    Serial.printf( "║  Calibrated:     %-3s                         ║\n",
                   classifier_is_calibrated() ? "YES" : "NO");
    Serial.printf( "║  Scan interval:  %-5u ms                    ║\n", scan_interval);
    Serial.printf( "║  Frame count:    %-8u                    ║\n", frame_count);
    Serial.printf( "║  Active slots:   %-2d  Occupied: %-2d           ║\n", n_slots, n_occ);
    Serial.printf( "║  Current bitmap: 0x%02X                        ║\n", current_bitmap);
    Serial.println("╠═══ ESP-NOW Link ═════════════════════════════╣");
    Serial.printf( "║  TX power:    %2d dBm  Channel: %d             ║\n",
                   ESPNOW_TX_POWER, ESPNOW_CHANNEL);
    Serial.printf( "║  TX OK/Fail:  %lu / %lu (%d%% loss)%*s║\n",
                   tx_success_count, tx_fail_count, loss_pct,
                   (int)(8 - String(loss_pct).length()), "");
    Serial.printf( "║  Heartbeat:   every %ds                     ║\n",
                   HEARTBEAT_INTERVAL / 1000);
    Serial.printf( "║  Seq counter: %u                              ║\n", tx_seq);
    Serial.println("╠═══ Slot Details ═════════════════════════════╣");
    for (int i = 0; i < n_slots; i++) {
        const char *state = slot_results[i].prediction ? "OCC " : "FREE";
        Serial.printf("║  Slot %d: %s  conf=%3d%%  raw=%5d        ║\n",
                      i, state, slot_results[i].confidence, slot_results[i].raw_metric);
    }
    Serial.println("╠══════════════════════════════════════════════╣");
    Serial.printf( "║  Uptime: %lu s%*s║\n", millis() / 1000,
                   (int)(32 - String(millis() / 1000).length()), "");
    Serial.println("╚══════════════════════════════════════════════╝");
    Serial.println();
}

static void led_flash(uint8_t mode) {
    if (mode == 0) {
        digitalWrite(FLASH_LED_PIN, LOW);
    } else if (mode == 1) {
        digitalWrite(FLASH_LED_PIN, HIGH);
    } else {  // momentary
        digitalWrite(FLASH_LED_PIN, HIGH);
        delay(200);
        digitalWrite(FLASH_LED_PIN, LOW);
    }
}


// ═════════════════════════════════════════════════════════════════════
//  NVS — ROI Persistence
//  Stores ROI layout so each parking lot keeps its config across reboots.
// ═════════════════════════════════════════════════════════════════════

static bool load_roi_from_nvs(void) {
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) return false;

    uint8_t stored_n = 0;
    if (nvs_get_u8(nvs, NVS_KEY_N_SLOTS, &stored_n) != ESP_OK || stored_n < 1 || stored_n > MAX_SLOTS) {
        nvs_close(nvs);
        return false;
    }

    size_t blob_len = sizeof(roi_rect_t) * MAX_SLOTS;
    roi_rect_t buf[MAX_SLOTS] = {};
    if (nvs_get_blob(nvs, NVS_KEY_ROI_DATA, buf, &blob_len) != ESP_OK) {
        nvs_close(nvs);
        return false;
    }

    n_slots = stored_n;
    memcpy(slot_rois, buf, sizeof(slot_rois));
    nvs_close(nvs);
    return true;
}

static bool save_roi_to_nvs(void) {
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) return false;

    bool ok = true;
    ok = ok && (nvs_set_u8(nvs, NVS_KEY_N_SLOTS, n_slots) == ESP_OK);
    ok = ok && (nvs_set_blob(nvs, NVS_KEY_ROI_DATA, slot_rois, sizeof(roi_rect_t) * MAX_SLOTS) == ESP_OK);
    ok = ok && (nvs_commit(nvs) == ESP_OK);
    nvs_close(nvs);
    return ok;
}

static void clear_roi_nvs(void) {
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_erase_key(nvs, NVS_KEY_N_SLOTS);
        nvs_erase_key(nvs, NVS_KEY_ROI_DATA);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}

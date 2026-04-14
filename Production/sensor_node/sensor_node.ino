/*
 * sensor_node.ino — ParkingLite Sensor Node v1.0
 *
 * Hardware:  ESP32-CAM AI-Thinker (OV2640 + 4MB PSRAM)
 * Framework: Arduino + ESP32 core 3.3.x
 * Protocol:  ESP-NOW broadcast (no router required)
 *
 * Algorithm: Integer MAD — 11-method ensemble, F1=0.985 on 54K samples
 * Payload:   { node_id: uint8, bitmap: uint8 } — 2 bytes per scan
 *
 * Serial commands (115200 baud):
 *   CAL            — Calibrate with current (empty-lot) frame → NVS
 *   RESET          — Clear calibration from NVS
 *   STATUS         — Print slot states and system info
 *   METHOD X       — Switch classification method (0–10)
 *   INTERVAL X     — Set scan interval in ms (1000–60000)
 *   ROI X Y W H I  — Override ROI for slot I
 *   ROI_GET        — Print ROI config as JSON
 *   SLOTS_GET      — Print last classification results as JSON
 *   SNAP           — Capture and stream grayscale JPEG via serial
 *   SNAP_COLOR     — Capture color SVGA JPEG via serial
 *   SNAP_XGA       — Capture color XGA JPEG via serial
 *   SNAP_UXGA      — Capture color UXGA JPEG via serial
 *   FLASH 0/1/2    — LED off / on / momentary
 *   PING           — Connectivity check
 *
 * ParkingLite v1.0 — Phenikaa University NCKH 2025-2026
 */

#include <Arduino.h>
#include "esp_camera.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "img_converters.h"
#include <WiFi.h>
#include <esp_now.h>

#include "camera_config.h"
#include "roi_classifier.h"
#include "config.h"

static const char *TAG = "SENSOR_NODE";

// ─── ESP-NOW Payload ────────────────────────────────────────────────
typedef struct __attribute__((packed)) {
    uint8_t node_id;
    uint8_t bitmap;
} espnow_payload_t;

static espnow_payload_t tx_payload;
static esp_now_peer_info_t peer_info;
static const uint8_t BROADCAST_ADDR[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// ─── Runtime State ──────────────────────────────────────────────────
static uint8_t  current_bitmap   = 0;
static uint8_t  last_tx_bitmap   = 0xFF;   // 0xFF forces first transmission
static uint8_t  classify_method  = DEFAULT_METHOD;
static uint32_t scan_interval    = SCAN_INTERVAL_MS;
static uint32_t last_scan_ms     = 0;
static uint32_t frame_count      = 0;
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
static void espnow_broadcast(uint8_t bitmap);
static void handle_serial_command(void);
static void print_status(void);
static void led_flash(uint8_t mode);
static bool capture_color_jpeg(framesize_t fsize, int quality, const char *label);


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
    Serial.println("║   ParkingLite Sensor Node v1.0    ║");
    Serial.println("║   ESP32-CAM + ESP-NOW              ║");
    Serial.println("╚═══════════════════════════════════╝");
    Serial.println();

    // Copy config ROI table to mutable runtime state
    memcpy(slot_rois, SLOT_ROIS, sizeof(slot_rois));

    // NVS for calibration persistence
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
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
        Serial.println("[OK] ESP-NOW: broadcast mode ready");
    }

    // LED ready indicator
    pinMode(FLASH_LED_PIN, OUTPUT);
    led_flash(2);

    Serial.printf("\n[READY] Node=0x%02X Lot=0x%02X Method=%d Interval=%ums Slots=%d\n\n",
                  NODE_ID, LOT_ID, classify_method, scan_interval, N_SLOTS);
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
        slot_rois, N_SLOTS,
        effective_method, slot_results
    );
    esp_camera_fb_return(fb);

    uint32_t elapsed_ms = millis() - t0;

    // Count occupied slots
    uint8_t n_occupied = 0;
    for (int i = 0; i < N_SLOTS; i++) {
        if (slot_results[i].prediction) n_occupied++;
    }

    Serial.printf("[%6u] Bitmap=0b", frame_count);
    for (int i = N_SLOTS - 1; i >= 0; i--) Serial.print((current_bitmap >> i) & 1);
    Serial.printf(" (0x%02X) | %ums | M%d | %d/%d occupied\n",
                  current_bitmap, elapsed_ms, effective_method, n_occupied, N_SLOTS);

    // Transmit on change only
    if (current_bitmap != last_tx_bitmap) {
        Serial.printf("         → CHANGE: 0x%02X → 0x%02X\n", last_tx_bitmap, current_bitmap);
        espnow_broadcast(current_bitmap);
        last_tx_bitmap = current_bitmap;
    }
}


// ═════════════════════════════════════════════════════════════════════
//  ESP-NOW Transmission
// ═════════════════════════════════════════════════════════════════════

static void espnow_broadcast(uint8_t bitmap) {
    tx_payload.node_id = NODE_ID;
    tx_payload.bitmap  = bitmap;

    esp_err_t result = esp_now_send(BROADCAST_ADDR, (uint8_t *)&tx_payload, sizeof(tx_payload));
    if (result == ESP_OK) {
        Serial.printf("         [TX] Node=0x%02X Lot=0x%02X Bitmap=0x%02X\n",
                      NODE_ID, LOT_ID, bitmap);
    } else {
        Serial.printf("         [TX_ERROR] esp_now_send returned 0x%x\n", result);
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
            bool ok = classifier_calibrate(fb->buf, fb->width, slot_rois, N_SLOTS);
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
        Serial.print(",\"slots\":[");
        for (int i = 0; i < N_SLOTS; i++) {
            Serial.printf("{\"i\":%d,\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d}",
                         i, slot_rois[i].x, slot_rois[i].y, slot_rois[i].w, slot_rois[i].h);
            if (i < N_SLOTS - 1) Serial.print(",");
        }
        Serial.println("]}");
    }
    else if (line == "SLOTS_GET") {
        Serial.printf("[SLOTS_JSON] {\"node\":0x%02X,\"bitmap\":0x%02X,\"method\":%d,\"slots\":[",
                      NODE_ID, current_bitmap, classify_method);
        for (int i = 0; i < N_SLOTS; i++) {
            Serial.printf("{\"i\":%d,\"occ\":%d,\"conf\":%d,\"raw\":%d}",
                         i, slot_results[i].prediction,
                         slot_results[i].confidence, slot_results[i].raw_metric);
            if (i < N_SLOTS - 1) Serial.print(",");
        }
        Serial.println("]}");
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
        Serial.println("          ROI X Y W H I | ROI_GET | SLOTS_GET");
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
    for (int i = 0; i < N_SLOTS; i++) if (slot_results[i].prediction) n_occ++;

    Serial.println();
    Serial.println("╔══════════════════════════════════════════════╗");
    Serial.println("║    ParkingLite Sensor Node v1.0 — Status     ║");
    Serial.println("╠══════════════════════════════════════════════╣");
    Serial.printf( "║  Node ID:        0x%02X  Lot ID: 0x%02X          ║\n", NODE_ID, LOT_ID);
    Serial.printf( "║  Method:         %2d %-14s            ║\n", classify_method, mname);
    Serial.printf( "║  Calibrated:     %-3s                         ║\n",
                   classifier_is_calibrated() ? "YES" : "NO");
    Serial.printf( "║  Scan interval:  %-5u ms                    ║\n", scan_interval);
    Serial.printf( "║  Frame count:    %-8u                    ║\n", frame_count);
    Serial.printf( "║  Active slots:   %-2d  Occupied: %-2d           ║\n", N_SLOTS, n_occ);
    Serial.printf( "║  Current bitmap: 0x%02X                        ║\n", current_bitmap);
    Serial.println("╠══════════════════════════════════════════════╣");
    for (int i = 0; i < N_SLOTS; i++) {
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

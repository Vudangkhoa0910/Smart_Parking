/*
 * sensor_cam_main.ino — ESP32-CAM Smart Parking Sensor Node v2.0
 *
 * Hardware: ESP32-CAM AI-Thinker (ESP32 + OV2640 + 4MB PSRAM)
 * Protocol: LiteComm v3.3 gradient mesh over ESP-NOW
 * Classification: 11 methods, default=combined (most robust)
 *
 * Capture modes (debug/research):
 *   SNAP         — Grayscale QVGA 320×240 (fast, current mode)
 *   SNAP_COLOR   — Color SVGA 800×600
 *   SNAP_XGA     — Color XGA 1024×768
 *   SNAP_UXGA    — Color UXGA 1600×1200 (max OV2640)
 *
 * Workflow:
 *   1. Capture grayscale image (320×240)
 *   2. Extract ROIs (parking slot regions, 32×32 each)
 *   3. Classify each ROI → occupied/empty bitmap
 *   4. Transmit bitmap via LiteComm DATA frame (ESP-NOW)
 *
 * Serial commands:
 *   "CAL"        — Calibrate with current (empty) image
 *   "RESET"      — Reset calibration
 *   "STATUS"     — Print current slot states
 *   "METHOD X"   — Set classification method (0-10)
 *   "INTERVAL X" — Set scan interval (ms)
 *   "ROI X Y W H I" — Set ROI for slot I
 *   "FLASH X"    — Flash LED (0=off, 1=on, 2=momentary)
 *
 * Methods (real-image evaluation):
 *   0  edge_density     Acc=68.8%  (no calibration needed)
 *   1  bg_relative      Acc=96.1%
 *   2  ref_frame MAD    Acc=100.0% ← recommended simple
 *   3  hybrid           Acc=100.0%
 *   4  gaussian_mad     Acc=100.0%
 *   5  block_mad        Acc=100.0%
 *   6  percentile_mad   Acc=100.0%
 *   7  max_block        Acc=100.0%
 *   8  histogram        Acc=100.0%
 *   9  variance_ratio   Acc=100.0%
 *  10  combined         Acc=100.0% ← default (most robust)
 */

#include <Arduino.h>
#include "esp_camera.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "img_converters.h"

#include "camera_config.h"
#include "roi_classifier.h"
#include <WiFi.h>
#include <esp_now.h>

static const char *TAG = "SENSOR_CAM";

// ─── Simple ESP-NOW Communication ──────────────────────────────────
typedef struct struct_message {
    uint8_t node_id;
    uint8_t bitmap;
} struct_message;

struct_message tx_data;
esp_now_peer_info_t peerInfo;
uint8_t broadcastAddress[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// Callback when data is sent
void OnDataSent(const esp_now_send_info_t *tx_info, esp_now_send_status_t status) {
    if (status == ESP_NOW_SEND_SUCCESS) {
        Serial.println("         [TX_OK] Broadcast successful");
    } else {
        Serial.println("         [TX_FAIL] Broadcast failed");
    }
}
// ───────────────────────────────────────────────────────────────────

// ─── Configuration ──────────────────────────────────────────────────
#define NODE_ID           0x01    // Unique ID for this node (change per device)
#define SCAN_INTERVAL_MS  5000    // Camera scan every 5 seconds
#define DEFAULT_METHOD    10      // 0=edge, 2=ref_mad, 10=combined (best)

// ─── ROI Configuration ─────────────────────────────────────────────
// Default: 5×2 grid for 10 parking slots in 320×240 frame
// Adjust these based on your camera angle and parking lot layout!
#define N_SLOTS   8   // Max 8 slots per node (LiteComm bitmap limit)

static roi_rect_t slot_rois[MAX_SLOTS] = {
    // Row 1 (top): slots 0-3
    { .x =  10, .y =  30, .w = 60, .h = 80 },  // Slot 0
    { .x =  80, .y =  30, .w = 60, .h = 80 },  // Slot 1
    { .x = 150, .y =  30, .w = 60, .h = 80 },  // Slot 2
    { .x = 220, .y =  30, .w = 60, .h = 80 },  // Slot 3
    // Row 2 (bottom): slots 4-7
    { .x =  10, .y = 130, .w = 60, .h = 80 },  // Slot 4
    { .x =  80, .y = 130, .w = 60, .h = 80 },  // Slot 5
    { .x = 150, .y = 130, .w = 60, .h = 80 },  // Slot 6
    { .x = 220, .y = 130, .w = 60, .h = 80 },  // Slot 7
};

// ─── State ──────────────────────────────────────────────────────────
static uint8_t  current_bitmap = 0;
static uint8_t  last_bitmap = 0xFF;  // Force first send
static uint8_t  classify_method = DEFAULT_METHOD;
static uint32_t scan_interval = SCAN_INTERVAL_MS;
static uint32_t last_scan_time = 0;
static uint32_t frame_count = 0;
static classify_result_t slot_results[MAX_SLOTS];

// ─── Function declarations ──────────────────────────────────────────
static bool init_camera(void);
static void process_frame(void);
static void send_litecomm_data(uint8_t bitmap);
static void handle_serial_command(void);
static void print_status(void);
static void led_flash(uint8_t mode);
static bool capture_color_jpeg(framesize_t fsize, int quality, const char *label);


// ═════════════════════════════════════════════════════════════════════
// Setup
// ═════════════════════════════════════════════════════════════════════

void setup() {
    Serial.begin(115200);
    Serial.println("\n=================================");
    Serial.println("  Smart Parking Sensor Node");
    Serial.println("  ESP32-CAM + LiteComm v3.3");
    Serial.println("=================================\n");
    
    // Initialize NVS (for calibration storage)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    
    // Initialize camera
    if (!init_camera()) {
        Serial.println("[FATAL] Camera init failed!");
        while (1) delay(1000);
    }
    Serial.println("[OK] Camera initialized (QVGA 320x240 grayscale)");
    
    // Initialize classifier (loads calibration from NVS if available)
    bool has_cal = classifier_init();
    if (has_cal) {
        Serial.println("[OK] Calibration data loaded from NVS");
        Serial.printf("     Method %d ready with calibrated thresholds\n", classify_method);
    } else {
        Serial.println("[WARN] No calibration. Using method 0 (edge_density)");
        Serial.println("       Send 'CAL' to calibrate with empty parking lot");
        if (classify_method > 0) {
            Serial.println("       Methods 1-10 require calibration -> falling back to 0");
        }
    }
    
    // Initialize Simple ESP-NOW (replaces LiteComm for now)
    WiFi.mode(WIFI_STA);
    if (esp_now_init() != ESP_OK) {
        Serial.println("[FATAL] Error initializing ESP-NOW");
    } else {
        esp_now_register_send_cb(OnDataSent);
        // Register broadcast peer
        memcpy(peerInfo.peer_addr, broadcastAddress, 6);
        peerInfo.channel = 0;  
        peerInfo.encrypt = false;
        if (esp_now_add_peer(&peerInfo) != ESP_OK) {
            Serial.println("[ERROR] Failed to add ESP-NOW peer");
        }
        Serial.println("[OK] ESP-NOW initialized (Broadcast mode)");
    }

    
    // LED flash to indicate ready
    pinMode(FLASH_LED_PIN, OUTPUT);
    led_flash(2);  // Momentary flash
    
    Serial.printf("\n[READY] Node 0x%02X | Method=%d | Interval=%ums | Slots=%d\n\n",
                  NODE_ID, classify_method, scan_interval, N_SLOTS);
}


// ═════════════════════════════════════════════════════════════════════
// Main Loop
// ═════════════════════════════════════════════════════════════════════

void loop() {
    uint32_t now = millis();
    
    // LiteComm tick (beacon, routing maintenance)
    // litecomm_tick(now);
    
    // Check serial commands
    if (Serial.available()) {
        handle_serial_command();
    }
    
    // Periodic scan
    if (now - last_scan_time >= scan_interval) {
        last_scan_time = now;
        process_frame();
    }
    
    delay(10);
}


// ═════════════════════════════════════════════════════════════════════
// Camera Init
// ═════════════════════════════════════════════════════════════════════

static bool init_camera(void) {
    camera_config_t config = get_camera_config();

    // Retry init up to 20 times - OV2640 SCCB timing-sensitive
    // Let library manage PWDN pin (avoid conflicts)
    delay(500);  // Stabilization after boot
    esp_err_t err = ESP_FAIL;
    for (int attempt = 1; attempt <= 20; attempt++) {
        err = esp_camera_init(&config);
        if (err == ESP_OK) {
            Serial.printf("[OK] Camera init success on attempt %d/20\n", attempt);
            break;
        }
        Serial.printf("[WARN] Camera init attempt %d/20 failed: 0x%x\n", attempt, err);
        esp_camera_deinit();
        delay(150);
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Camera init failed after 20 attempts: 0x%x", err);
        Serial.println("[HINT] Power cycle USB (unplug 5s, reconnect)");
        return false;
    }
    
    // Optimize sensor settings for parking detection
    sensor_t *s = esp_camera_sensor_get();
    if (s) {
        s->set_brightness(s, 1);      // Slightly brighter
        s->set_contrast(s, 2);        // Higher contrast for clearer edges
        s->set_saturation(s, 0);      // N/A for grayscale
        s->set_sharpness(s, 2);       // Sharper image (reduce blur)
        s->set_gainceiling(s, GAINCEILING_4X);  // Lower max gain = less noise
        s->set_whitebal(s, 1);        // Auto white balance
        s->set_awb_gain(s, 1);        // AWB gain on
        s->set_gain_ctrl(s, 1);       // Auto gain
        s->set_exposure_ctrl(s, 1);   // Auto exposure
        s->set_aec2(s, 1);            // AEC DSP
        s->set_ae_level(s, 0);        // AE level
        s->set_denoise(s, 1);         // Enable denoise
    }
    
    // Warm-up: take and discard first few frames
    for (int i = 0; i < 5; i++) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (fb) esp_camera_fb_return(fb);
        delay(100);
    }
    
    return true;
}


// ═════════════════════════════════════════════════════════════════════
// Frame Processing & Classification
// ═════════════════════════════════════════════════════════════════════

static void process_frame(void) {
    uint32_t t0 = millis();
    
    // Capture frame
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        ESP_LOGE(TAG, "Frame capture failed");
        return;
    }
    
    // Verify format
    if (fb->format != PIXFORMAT_GRAYSCALE) {
        ESP_LOGE(TAG, "Unexpected format: %d (expected GRAYSCALE)", fb->format);
        esp_camera_fb_return(fb);
        return;
    }
    
    frame_count++;
    
    // Determine effective method (fallback if not calibrated)
    uint8_t effective_method = classify_method;
    if (effective_method > 0 && !classifier_is_calibrated()) {
        effective_method = 0;  // Fall back to edge_density
    }
    
    // Classify all slots
    current_bitmap = classify_all_slots(
        fb->buf, fb->width,
        slot_rois, N_SLOTS,
        effective_method, slot_results
    );
    
    uint32_t classify_ms = millis() - t0;
    
    // Print results
    Serial.printf("[%6u] Bitmap=0b", frame_count);
    for (int i = N_SLOTS - 1; i >= 0; i--) {
        Serial.print((current_bitmap >> i) & 1);
    }
    Serial.printf(" (0x%02X) | %ums | M%d", current_bitmap, classify_ms, effective_method);
    
    // Slot details
    uint8_t n_occupied = 0;
    for (int i = 0; i < N_SLOTS; i++) {
        if (slot_results[i].prediction) n_occupied++;
    }
    Serial.printf(" | %d/%d occupied\n", n_occupied, N_SLOTS);
    
    // Send via LiteComm if bitmap changed
    if (current_bitmap != last_bitmap) {
        Serial.printf("         → CHANGE detected! Old=0x%02X New=0x%02X\n",
                      last_bitmap, current_bitmap);
        send_litecomm_data(current_bitmap);
        last_bitmap = current_bitmap;
    }
    
    // Return frame buffer
    esp_camera_fb_return(fb);
}

static void send_litecomm_data(uint8_t bitmap) {
    tx_data.node_id = NODE_ID;
    tx_data.bitmap = bitmap;
    
    esp_err_t result = esp_now_send(broadcastAddress, (uint8_t *) &tx_data, sizeof(tx_data));
    
    if (result == ESP_OK) {
        Serial.printf("         [TX] Broadcast Node=0x%02X Bitmap=0x%02X\n", NODE_ID, bitmap);
    } else {
        Serial.println("         [ERROR] ESP-NOW send failed");
    }
}


// ═════════════════════════════════════════════════════════════════════
// HD Color Capture Helper
// ═════════════════════════════════════════════════════════════════════

/**
 * Capture a single high-quality COLOR JPEG at the specified resolution.
 * Deinits grayscale camera → inits color mode → tunes sensor → captures
 * → sends over serial → reverts to grayscale for classification.
 *
 * @param fsize   Target frame size (FRAMESIZE_SVGA, FRAMESIZE_XGA, FRAMESIZE_UXGA)
 * @param quality JPEG quality 0-63 (lower = better)
 * @param label   Label for serial output (e.g., "SNAP_COLOR", "SNAP_XGA")
 * @return true on success
 */
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
        Serial.printf("[%s] Init attempt %d failed: 0x%x\n", label, attempt, err);
        esp_camera_deinit();
        delay(150);
    }

    bool success = false;

    if (err == ESP_OK) {
        // Tune sensor for stable color preview without heavy processing
        sensor_t *s = esp_camera_sensor_get();
        if (s) {
            s->set_brightness(s, 0);       // Natural brightness
            s->set_contrast(s, 1);         // Moderate contrast for better rendering
            s->set_saturation(s, 1);       // Color visible but not oversaturated
            s->set_sharpness(s, 1);        // Moderate sharpness to reduce artifacts
            s->set_whitebal(s, 1);         // Auto white balance ON
            s->set_awb_gain(s, 1);         // AWB gain ON
            s->set_wb_mode(s, 0);          // Auto WB mode
            s->set_exposure_ctrl(s, 1);    // Auto exposure ON
            s->set_aec2(s, 1);             // AEC DSP ON
            s->set_ae_level(s, 0);         // Neutral AE level
            s->set_gain_ctrl(s, 1);        // Auto gain ON
            s->set_gainceiling(s, GAINCEILING_4X);  // Good tradeoff noise/speed
            s->set_denoise(s, 1);          // Denoise ON
            s->set_special_effect(s, 0);   // No effect (normal)
            s->set_lenc(s, 1);             // Lens correction ON
            s->set_raw_gma(s, 1);          // Gamma correction ON
        }
        delay(80);  // Let sensor settings stabilize

        // Warm-up: discard first 3 frames (AEC/AWB need time to converge)
        for (int i = 0; i < 3; i++) {
            camera_fb_t *fb = esp_camera_fb_get();
            if (fb) esp_camera_fb_return(fb);
            delay(80);
        }

        // Capture the actual frame
        camera_fb_t *fb = esp_camera_fb_get();
        if (fb && fb->format == PIXFORMAT_JPEG && fb->len > 0) {
            Serial.printf("[SNAP_START] size=%u width=%d height=%d color=1\n",
                          (unsigned)fb->len, fb->width, fb->height);
            delay(50);
            
            // Gửi dữ liệu theo cục nhỏ (chunks) để tránh ghi đè/tràn buffer Serial làm vỡ ảnh
            const size_t CHUNK_SIZE = 1024;
            for (size_t i = 0; i < fb->len; i += CHUNK_SIZE) {
                size_t to_send = (fb->len - i < CHUNK_SIZE) ? (fb->len - i) : CHUNK_SIZE;
                Serial.write(fb->buf + i, to_send);
                Serial.flush();
                delay(3); // Nghỉ nhẹ giữa các chunk
            }
            
            Serial.println();
            Serial.println("[SNAP_END]");
            Serial.printf("[%s] OK! %dx%d, %u bytes\n",
                          label, fb->width, fb->height, (unsigned)fb->len);
            esp_camera_fb_return(fb);
            success = true;
        } else {
            Serial.printf("[%s] Capture failed (format=%d, len=%u)\n",
                          label, fb ? fb->format : -1, fb ? (unsigned)fb->len : 0);
            if (fb) esp_camera_fb_return(fb);
        }
    } else {
        Serial.printf("[%s] Color mode init FAILED after 10 attempts\n", label);
    }

    // Always revert to grayscale QVGA for classification
    esp_camera_deinit();
    delay(200);
    if (!init_camera()) {
        Serial.println("[FATAL] Failed to revert to classification mode!");
    } else {
        Serial.printf("[%s] Reverted to grayscale QVGA\n", label);
    }

    return success;
}


// ═════════════════════════════════════════════════════════════════════
// Serial Command Handler
// ═════════════════════════════════════════════════════════════════════

static void handle_serial_command(void) {
    String line = Serial.readStringUntil('\n');
    line.trim();
    line.toUpperCase();
    
    if (line == "CAL") {
        Serial.println("\n[CAL] Calibrating with current frame (ensure lot is EMPTY)...");
        led_flash(1);  // LED on during calibration
        
        camera_fb_t *fb = esp_camera_fb_get();
        if (fb && fb->format == PIXFORMAT_GRAYSCALE) {
            bool ok = classifier_calibrate(fb->buf, fb->width, slot_rois, N_SLOTS);
            if (ok) {
                Serial.println("[CAL] Success! Calibration saved to NVS.");
                Serial.printf("[CAL] Recommend method 2 (ref_frame) or 10 (combined).\n");
            } else {
                Serial.println("[CAL] FAILED. Check NVS storage.");
            }
            esp_camera_fb_return(fb);
        } else {
            Serial.println("[CAL] Frame capture failed!");
        }
        
        led_flash(0);  // LED off
    }
    else if (line == "RESET") {
        classifier_reset_calibration();
        Serial.println("[RESET] Calibration cleared. Falling back to method 0.");
    }
    else if (line == "STATUS") {
        print_status();
    }
    else if (line.startsWith("METHOD ")) {
        int m = line.substring(7).toInt();
        if (m >= 0 && m <= 10) {
            classify_method = m;
            Serial.printf("[CONFIG] Method set to %d\n", m);
            if (m > 0 && !classifier_is_calibrated()) {
                Serial.println("         WARNING: Method requires calibration. Send 'CAL' first.");
            }
        } else {
            Serial.println("[ERROR] Method must be 0-10");
        }
    }
    else if (line.startsWith("INTERVAL ")) {
        int ms = line.substring(9).toInt();
        if (ms >= 1000 && ms <= 60000) {
            scan_interval = ms;
            Serial.printf("[CONFIG] Scan interval set to %u ms\n", scan_interval);
        } else {
            Serial.println("[ERROR] Interval must be 1000-60000 ms");
        }
    }
    else if (line.startsWith("ROI ")) {
        // Parse: ROI X Y W H I
        int x, y, w, h, idx;
        if (sscanf(line.c_str(), "ROI %d %d %d %d %d", &x, &y, &w, &h, &idx) == 5) {
            if (idx >= 0 && idx < MAX_SLOTS && x >= 0 && y >= 0 && w > 0 && h > 0) {
                slot_rois[idx].x = x;
                slot_rois[idx].y = y;
                slot_rois[idx].w = w;
                slot_rois[idx].h = h;
                Serial.printf("[ROI] Slot %d: (%d,%d,%d,%d)\n", idx, x, y, w, h);
            } else {
                Serial.println("[ERROR] Invalid ROI parameters");
            }
        } else {
            Serial.println("[ERROR] Usage: ROI X Y W H INDEX");
        }
    }
    else if (line.startsWith("FLASH ")) {
        int mode = line.substring(6).toInt();
        led_flash(mode);
    }
    else if (line == "SNAP" || line == "SNAPSHOT") {
        // Capture current grayscale frame as JPEG (fast, current mode)
        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) {
            Serial.println("[SNAP_ERROR] Frame capture failed");
            return;
        }

        // JPEG quality 0-63 (lower = better). Use 10 for sharp image.
        uint8_t *jpg_buf = NULL;
        size_t jpg_len = 0;
        int w = fb->width, h = fb->height;
        bool ok = fmt2jpg(fb->buf, fb->len, w, h,
                          PIXFORMAT_GRAYSCALE, 10, &jpg_buf, &jpg_len);
        esp_camera_fb_return(fb);

        if (!ok || !jpg_buf) {
            Serial.println("[SNAP_ERROR] JPEG encode failed");
            return;
        }

        Serial.printf("[SNAP_START] size=%u width=%d height=%d\n",
                      (unsigned)jpg_len, w, h);
        delay(30);
        
        const size_t CHUNK_SIZE = 1024;
        for (size_t i = 0; i < jpg_len; i += CHUNK_SIZE) {
            size_t to_send = (jpg_len - i < CHUNK_SIZE) ? (jpg_len - i) : CHUNK_SIZE;
            Serial.write(jpg_buf + i, to_send);
            Serial.flush();
            delay(3);
        }
        
        Serial.println();
        Serial.println("[SNAP_END]");
        free(jpg_buf);
    }
    else if (line == "SNAP_COLOR" || line == "SNAP_HD") {
        // Color SVGA 800×600 — balanced for demo, faster transfer
        capture_color_jpeg(FRAMESIZE_SVGA, 18, "SNAP_COLOR");
    }
    else if (line == "SNAP_XGA") {
        // Color XGA 1024×768 — detailed, use slightly higher compression
        capture_color_jpeg(FRAMESIZE_XGA, 22, "SNAP_XGA");
    }
    else if (line == "SNAP_UXGA") {
        // Color UXGA 1600×1200 — maximum OV2640 resolution, slower
        capture_color_jpeg(FRAMESIZE_UXGA, 26, "SNAP_UXGA");
    }
    else if (line == "ROI_GET") {
        // Send current ROI configuration as JSON
        Serial.print("[ROI_JSON] {\"slots\":[");
        for (int i = 0; i < N_SLOTS; i++) {
            Serial.printf("{\"i\":%d,\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d}",
                         i, slot_rois[i].x, slot_rois[i].y, slot_rois[i].w, slot_rois[i].h);
            if (i < N_SLOTS - 1) Serial.print(",");
        }
        Serial.println("]}");
    }
    else if (line == "SLOTS_GET") {
        // Send detailed slot results as JSON
        Serial.print("[SLOTS_JSON] {\"bitmap\":");
        Serial.printf("%u,\"method\":%d,\"slots\":[", current_bitmap, classify_method);
        for (int i = 0; i < N_SLOTS; i++) {
            Serial.printf("{\"i\":%d,\"pred\":%d,\"conf\":%d,\"raw\":%d}",
                         i, slot_results[i].prediction, slot_results[i].confidence,
                         slot_results[i].raw_metric);
            if (i < N_SLOTS - 1) Serial.print(",");
        }
        Serial.println("]}");
    }
    else if (line == "PING") {
        Serial.println("[PONG]");
    }
    else if (line.length() > 0) {
        Serial.println("Commands: CAL | RESET | STATUS | METHOD 0-10 | INTERVAL ms");
        Serial.println("          ROI x y w h idx | FLASH 0/1/2 | PING");
        Serial.println("          SNAP (gray) | SNAP_COLOR (SVGA) | SNAP_XGA | SNAP_UXGA (max)");
        Serial.println("          ROI_GET | SLOTS_GET");
    }
}

// Method name lookup table
static const char *method_names[] = {
    "edge_density",   // 0
    "bg_relative",    // 1
    "ref_frame_mad",  // 2
    "hybrid",         // 3
    "gaussian_mad",   // 4
    "block_mad",      // 5
    "percentile_mad", // 6
    "max_block",      // 7
    "histogram",      // 8
    "variance_ratio", // 9
    "combined",       // 10
};

static void print_status(void) {
    const char *mname = (classify_method < NUM_METHODS) ?
                        method_names[classify_method] : "unknown";

    Serial.println("\n╔══════════════════════════════════════════════╗");
    Serial.println("║    Smart Parking Sensor v2.0 — Status        ║");
    Serial.println("╠══════════════════════════════════════════════╣");
    Serial.printf( "║  Node ID:        0x%02X                        ║\n", NODE_ID);
    Serial.printf( "║  Frame count:    %-8u                    ║\n", frame_count);
    Serial.printf( "║  Method:         %2d (%s)%*s║\n",
                   classify_method, mname,
                   (int)(16 - strlen(mname)), "");
    Serial.printf( "║  Calibrated:     %-3s                         ║\n",
        classifier_is_calibrated() ? "YES" : "NO");
    Serial.printf( "║  Scan interval:  %-5u ms                    ║\n", scan_interval);
    Serial.printf( "║  Active slots:   %-2d                          ║\n", N_SLOTS);
    Serial.printf( "║  Current bitmap: 0x%02X                        ║\n", current_bitmap);
    Serial.println("╠══════════════════════════════════════════════╣");

    uint8_t n_occ = 0;
    for (int i = 0; i < N_SLOTS; i++) {
        const char *state = slot_results[i].prediction ? "OCC " : "FREE";
        Serial.printf("║  Slot %d: %s  conf=%3d%%  raw=%5d       ║\n",
                      i, state, slot_results[i].confidence, slot_results[i].raw_metric);
        if (slot_results[i].prediction) n_occ++;
    }

    Serial.println("╠══════════════════════════════════════════════╣");
    Serial.printf( "║  Occupancy: %d/%d slots (%.0f%%)                ║\n",
                   n_occ, N_SLOTS, (float)n_occ / N_SLOTS * 100);
    Serial.printf( "║  Free heap:  %u bytes                  ║\n", esp_get_free_heap_size());
    Serial.printf( "║  PSRAM free: %u bytes                 ║\n", ESP.getFreePsram());
    Serial.printf( "║  Cal data:   %u bytes                 ║\n",
                   (unsigned)sizeof(calibration_data_t));
    Serial.println("╚══════════════════════════════════════════════╝\n");
}

static void led_flash(uint8_t mode) {
    switch (mode) {
        case 0: digitalWrite(FLASH_LED_PIN, LOW); break;
        case 1: digitalWrite(FLASH_LED_PIN, HIGH); break;
        case 2: // Momentary
            digitalWrite(FLASH_LED_PIN, HIGH);
            delay(200);
            digitalWrite(FLASH_LED_PIN, LOW);
            break;
    }
}

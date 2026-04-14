/*
 * camera_test.ino — Minimal ESP32-CAM diagnostic
 *
 * Thử init camera OV2640 với nhiều config khác nhau để tìm vấn đề.
 */

#include "esp_camera.h"

// AI-Thinker ESP32-CAM pins
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27
#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22
#define FLASH_LED_PIN      4

camera_config_t make_config(int xclk_hz, pixformat_t fmt, framesize_t fs, bool use_psram) {
    camera_config_t config = {};
    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer   = LEDC_TIMER_0;
    config.pin_d0       = Y2_GPIO_NUM;
    config.pin_d1       = Y3_GPIO_NUM;
    config.pin_d2       = Y4_GPIO_NUM;
    config.pin_d3       = Y5_GPIO_NUM;
    config.pin_d4       = Y6_GPIO_NUM;
    config.pin_d5       = Y7_GPIO_NUM;
    config.pin_d6       = Y8_GPIO_NUM;
    config.pin_d7       = Y9_GPIO_NUM;
    config.pin_xclk     = XCLK_GPIO_NUM;
    config.pin_pclk     = PCLK_GPIO_NUM;
    config.pin_vsync    = VSYNC_GPIO_NUM;
    config.pin_href     = HREF_GPIO_NUM;
    config.pin_sccb_sda = SIOD_GPIO_NUM;
    config.pin_sccb_scl = SIOC_GPIO_NUM;
    config.pin_pwdn     = PWDN_GPIO_NUM;
    config.pin_reset    = RESET_GPIO_NUM;
    config.xclk_freq_hz = xclk_hz;
    config.pixel_format = fmt;
    config.frame_size   = fs;
    config.jpeg_quality = 12;
    config.fb_count     = 1;
    config.fb_location  = use_psram ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM;
    config.grab_mode    = CAMERA_GRAB_LATEST;
    return config;
}

const char* err_to_str(esp_err_t e) {
    switch(e) {
        case ESP_OK: return "OK";
        case ESP_FAIL: return "FAIL";
        case ESP_ERR_NO_MEM: return "NO_MEM";
        case ESP_ERR_INVALID_ARG: return "INVALID_ARG";
        case ESP_ERR_INVALID_STATE: return "INVALID_STATE";
        case ESP_ERR_NOT_FOUND: return "NOT_FOUND";
        default: return "UNKNOWN";
    }
}

void try_init(const char* name, camera_config_t cfg) {
    Serial.printf("\n[TEST] %s\n", name);
    Serial.printf("  XCLK=%d Hz, format=%d, size=%d, PSRAM=%s\n",
                  (int)cfg.xclk_freq_hz, cfg.pixel_format, cfg.frame_size,
                  cfg.fb_location == CAMERA_FB_IN_PSRAM ? "yes" : "no");

    esp_err_t err = esp_camera_init(&cfg);
    Serial.printf("  Result: %s (0x%x)\n", err_to_str(err), err);

    if (err == ESP_OK) {
        Serial.println("  [SUCCESS] Capturing test frame...");
        camera_fb_t *fb = esp_camera_fb_get();
        if (fb) {
            Serial.printf("  Frame: %dx%d, %u bytes, format=%d\n",
                          fb->width, fb->height, fb->len, fb->format);
            // Print first 16 pixel values
            Serial.print("  First 16 pixels: ");
            for (int i = 0; i < 16 && i < (int)fb->len; i++) {
                Serial.printf("%02x ", fb->buf[i]);
            }
            Serial.println();

            // Compute mean brightness
            uint32_t sum = 0;
            for (uint32_t i = 0; i < fb->len; i++) sum += fb->buf[i];
            uint32_t mean = sum / fb->len;
            Serial.printf("  Mean brightness: %u\n", mean);

            esp_camera_fb_return(fb);
        } else {
            Serial.println("  [WARN] Could not capture frame");
        }
        esp_camera_deinit();
    }
}

void setup() {
    Serial.begin(115200);
    delay(1000);

    Serial.println("\n\n========================================");
    Serial.println("  ESP32-CAM Camera Diagnostic");
    Serial.println("========================================");

    // System info
    Serial.printf("ESP32 Chip: %s rev%d, %d cores, %d MHz\n",
                  ESP.getChipModel(), ESP.getChipRevision(),
                  ESP.getChipCores(), ESP.getCpuFreqMHz());
    Serial.printf("Flash: %d MB\n", ESP.getFlashChipSize() / (1024*1024));
    Serial.printf("PSRAM: %s, size=%d bytes, free=%d\n",
                  psramFound() ? "YES" : "NO",
                  ESP.getPsramSize(), ESP.getFreePsram());
    Serial.printf("Heap free: %d bytes\n", ESP.getFreeHeap());

    // Turn off flash LED
    pinMode(FLASH_LED_PIN, OUTPUT);
    digitalWrite(FLASH_LED_PIN, LOW);

    // Test 1: Default config (20MHz XCLK, grayscale, QVGA, PSRAM)
    try_init("1. Default (20MHz, grayscale, QVGA, PSRAM)",
             make_config(20000000, PIXFORMAT_GRAYSCALE, FRAMESIZE_QVGA, true));

    delay(500);

    // Test 2: Lower XCLK
    try_init("2. Lower XCLK (10MHz, grayscale, QVGA, PSRAM)",
             make_config(10000000, PIXFORMAT_GRAYSCALE, FRAMESIZE_QVGA, true));

    delay(500);

    // Test 3: JPEG (simpler)
    try_init("3. JPEG mode (20MHz, JPEG, QVGA, PSRAM)",
             make_config(20000000, PIXFORMAT_JPEG, FRAMESIZE_QVGA, true));

    delay(500);

    // Test 4: Without PSRAM
    try_init("4. DRAM only (20MHz, grayscale, QQVGA 160x120)",
             make_config(20000000, PIXFORMAT_GRAYSCALE, FRAMESIZE_QQVGA, false));

    delay(500);

    // Test 5: Smaller frame
    try_init("5. Smaller (16MHz, grayscale, 96x96)",
             make_config(16000000, PIXFORMAT_GRAYSCALE, FRAMESIZE_96X96, true));

    Serial.println("\n========================================");
    Serial.println("  DIAGNOSTIC DONE");
    Serial.println("========================================");
    Serial.println("If ALL tests fail:");
    Serial.println("  1. Camera ribbon cable not seated properly");
    Serial.println("  2. Insufficient USB power (use powered hub or 5V/1A supply)");
    Serial.println("  3. Camera module damaged");
    Serial.println("If JPEG works but grayscale fails: firmware/library bug");
}

void loop() {
    static uint32_t last = 0;
    if (millis() - last > 5000) {
        last = millis();
        Serial.println("[loop] Heartbeat. Press RESET to rerun tests.");
    }
}

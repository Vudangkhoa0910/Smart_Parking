/*
 * gateway.ino — ParkingLite Gateway Node v1.0
 *
 * Hardware:  ESP32 Dev Board (any variant)
 * Framework: Arduino + ESP32 core 3.3.x
 * Protocol:  ESP-NOW receive (no router required)
 *
 * Receives 2-byte { node_id, bitmap } packets from up to 8 sensor nodes.
 * Maintains per-node state and outputs JSON to Serial for the monitor app.
 *
 * Serial output formats:
 *   [READY]  — system boot complete
 *   [RX]     — new packet received, pretty-printed
 *   [STATUS_JSON] — full lot state (triggered by 'STATUS' command)
 *
 * JSON event line (emitted on every valid packet):
 *   {"event":"update","lot":1,"node":1,"bitmap":237,"slots":8,"occupied":6,"uptime":12345}
 *
 * Serial commands (115200 baud):
 *   STATUS   — print current state of all nodes as JSON
 *   PING     — connectivity check
 *
 * ParkingLite v1.0 — Phenikaa University NCKH 2025-2026
 */

#include <esp_now.h>
#include <WiFi.h>

// ─── Configuration ──────────────────────────────────────────────────
#define LOT_ID          0x01    // Parking lot identifier (match sensor config)
#define MAX_NODES       8       // Maximum sensor nodes per gateway
#define NODE_TIMEOUT_MS 30000   // Mark node offline after 30 s without packet
#define STATUS_LED_PIN  2       // Built-in LED (active HIGH on most Dev boards)

// ─── Payload (must match sensor_node) ───────────────────────────────
typedef struct __attribute__((packed)) {
    uint8_t node_id;
    uint8_t bitmap;
} espnow_payload_t;

// ─── Per-Node State ──────────────────────────────────────────────────
typedef struct {
    uint8_t  node_id;
    uint8_t  bitmap;
    uint8_t  mac[6];
    uint32_t last_rx_ms;
    uint32_t rx_count;
    bool     active;
} node_state_t;

static node_state_t nodes[MAX_NODES];
static uint8_t      node_count = 0;

// ─── Helpers ────────────────────────────────────────────────────────

static int find_node(uint8_t id) {
    for (int i = 0; i < node_count; i++) {
        if (nodes[i].node_id == id) return i;
    }
    return -1;
}

static int register_node(uint8_t id, const uint8_t mac[6]) {
    if (node_count >= MAX_NODES) return -1;
    int idx = node_count++;
    nodes[idx].node_id    = id;
    nodes[idx].rx_count   = 0;
    nodes[idx].active     = true;
    memcpy(nodes[idx].mac, mac, 6);
    Serial.printf("[NEW] Node 0x%02X registered (slot %d)\n", id, idx);
    return idx;
}

static uint8_t popcount(uint8_t b) {
    uint8_t n = 0;
    while (b) { n += b & 1; b >>= 1; }
    return n;
}

static void print_bitmap_bits(uint8_t bitmap, uint8_t n_slots) {
    for (int i = n_slots - 1; i >= 0; i--) Serial.print((bitmap >> i) & 1);
}

static void emit_event_json(int idx) {
    uint8_t occ = popcount(nodes[idx].bitmap);
    Serial.printf("{\"event\":\"update\",\"lot\":%d,\"node\":%d,"
                  "\"bitmap\":%d,\"slots\":8,\"occupied\":%d,"
                  "\"rx_count\":%lu,\"uptime_ms\":%lu}\n",
                  LOT_ID, nodes[idx].node_id,
                  nodes[idx].bitmap, occ,
                  nodes[idx].rx_count, millis());
}

static void led_blink(void) {
    digitalWrite(STATUS_LED_PIN, HIGH);
    delay(30);
    digitalWrite(STATUS_LED_PIN, LOW);
}


// ─── ESP-NOW Callback ────────────────────────────────────────────────

void OnDataRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
    if (len != sizeof(espnow_payload_t)) {
        Serial.printf("[RX_WARN] Unexpected length %d bytes (expected %d)\n",
                      len, (int)sizeof(espnow_payload_t));
        return;
    }

    espnow_payload_t payload;
    memcpy(&payload, data, sizeof(payload));
    const uint8_t *mac = info->src_addr;

    // Find or register node
    int idx = find_node(payload.node_id);
    if (idx < 0) {
        idx = register_node(payload.node_id, mac);
        if (idx < 0) {
            Serial.println("[RX_WARN] Node table full, ignoring packet");
            return;
        }
    }

    // Update state
    uint8_t old_bitmap        = nodes[idx].bitmap;
    nodes[idx].bitmap         = payload.bitmap;
    nodes[idx].last_rx_ms     = millis();
    nodes[idx].rx_count++;
    nodes[idx].active         = true;
    memcpy(nodes[idx].mac, mac, 6);

    // Human-readable log
    uint8_t occ = popcount(payload.bitmap);
    Serial.printf("[RX] From %02X:%02X:%02X:%02X:%02X:%02X | "
                  "Node=0x%02X | Bitmap=0b",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
                  payload.node_id);
    print_bitmap_bits(payload.bitmap, 8);
    Serial.printf(" (0x%02X) | %d/8 occupied", payload.bitmap, occ);
    if (old_bitmap != payload.bitmap) {
        Serial.printf(" | CHANGED from 0x%02X", old_bitmap);
    }
    Serial.println();

    // Machine-readable JSON event
    emit_event_json(idx);

    // LED blink on receive
    led_blink();
}


// ═════════════════════════════════════════════════════════════════════
//  Setup
// ═════════════════════════════════════════════════════════════════════

void setup() {
    Serial.begin(115200);
    Serial.println();
    Serial.println("╔════════════════════════════════════╗");
    Serial.println("║   ParkingLite Gateway Node v1.0    ║");
    Serial.println("║   ESP-NOW Receiver + JSON Output   ║");
    Serial.println("╚════════════════════════════════════╝");
    Serial.println();

    pinMode(STATUS_LED_PIN, OUTPUT);
    digitalWrite(STATUS_LED_PIN, LOW);

    memset(nodes, 0, sizeof(nodes));

    WiFi.mode(WIFI_STA);
    WiFi.disconnect();

    Serial.print("[INFO] Gateway MAC: ");
    Serial.println(WiFi.macAddress());
    Serial.printf("[INFO] Lot ID: 0x%02X | Max nodes: %d\n", LOT_ID, MAX_NODES);

    if (esp_now_init() != ESP_OK) {
        Serial.println("[FATAL] ESP-NOW init failed");
        while (1) {
            digitalWrite(STATUS_LED_PIN, !digitalRead(STATUS_LED_PIN));
            delay(200);
        }
    }
    esp_now_register_recv_cb(OnDataRecv);

    // Boot LED flash
    for (int i = 0; i < 3; i++) { led_blink(); delay(100); }

    Serial.println("[READY] Listening for sensor nodes...");
    Serial.println();
}


// ═════════════════════════════════════════════════════════════════════
//  Main Loop
// ═════════════════════════════════════════════════════════════════════

void loop() {
    // Serial commands
    if (Serial.available()) {
        String line = Serial.readStringUntil('\n');
        line.trim();
        line.toUpperCase();

        if (line == "STATUS") {
            uint32_t now = millis();
            Serial.println();
            Serial.println("╔══════════════════════════════════════════════╗");
            Serial.println("║    ParkingLite Gateway — Node Status          ║");
            Serial.println("╠══════════════════════════════════════════════╣");
            Serial.printf( "║  Lot ID: 0x%02X  Uptime: %lu s%*s║\n",
                           LOT_ID, now / 1000,
                           (int)(19 - String(now / 1000).length()), "");
            Serial.println("╠══════════════════════════════════════════════╣");
            if (node_count == 0) {
                Serial.println("║  No nodes registered yet                     ║");
            }
            for (int i = 0; i < (int)node_count; i++) {
                bool online = (now - nodes[i].last_rx_ms) < NODE_TIMEOUT_MS;
                uint8_t occ = popcount(nodes[i].bitmap);
                Serial.printf("║  Node 0x%02X  %s  %d/8 occ  Bitmap=0x%02X  rx=%lu%*s║\n",
                              nodes[i].node_id, online ? "ONLINE " : "OFFLINE",
                              occ, nodes[i].bitmap, nodes[i].rx_count,
                              (int)(2 - String(nodes[i].rx_count).length()), "");
            }
            Serial.println("╚══════════════════════════════════════════════╝");
            Serial.println();

            // Also emit JSON summary
            Serial.print("[STATUS_JSON] {\"lot\":");
            Serial.print(LOT_ID);
            Serial.print(",\"uptime_ms\":");
            Serial.print(millis());
            Serial.print(",\"nodes\":[");
            for (int i = 0; i < (int)node_count; i++) {
                bool online = (now - nodes[i].last_rx_ms) < NODE_TIMEOUT_MS;
                uint8_t occ = popcount(nodes[i].bitmap);
                Serial.printf("{\"id\":%d,\"bitmap\":%d,\"occupied\":%d,"
                              "\"online\":%s,\"rx_count\":%lu}",
                              nodes[i].node_id, nodes[i].bitmap, occ,
                              online ? "true" : "false", nodes[i].rx_count);
                if (i < (int)node_count - 1) Serial.print(",");
            }
            Serial.println("]}");
        }
        else if (line == "PING") {
            Serial.println("[PONG]");
        }
        else if (line.length() > 0) {
            Serial.println("Commands: STATUS | PING");
        }
    }

    // Timeout detection (mark nodes offline silently — no log spam)
    static uint32_t last_timeout_check = 0;
    if (millis() - last_timeout_check > 5000) {
        last_timeout_check = millis();
        for (int i = 0; i < (int)node_count; i++) {
            bool was_active = nodes[i].active;
            nodes[i].active = (millis() - nodes[i].last_rx_ms) < NODE_TIMEOUT_MS;
            if (was_active && !nodes[i].active) {
                Serial.printf("[WARN] Node 0x%02X timed out (no packet for >%ds)\n",
                              nodes[i].node_id, NODE_TIMEOUT_MS / 1000);
            }
        }
    }

    delay(10);
}

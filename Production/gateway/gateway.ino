/*
 * gateway.ino — ParkingLite Gateway Node v1.1
 *
 * Hardware:  ESP32 Dev Board (any variant)
 * Framework: Arduino + ESP32 core 3.3.x
 * Protocol:  ESP-NOW receive (no router required)
 *
 * Receives 8-byte v2 packets from up to 8 sensor nodes.
 * Tracks per-node: bitmap, RSSI, packet loss, last seen.
 * Outputs JSON events for the monitor app.
 *
 * Link quality metrics (important for research):
 *   - RSSI (dBm): signal strength per packet
 *   - Packet loss: computed from seq number gaps
 *   - Distance estimate: derived from RSSI + TX power
 *   - Online/Offline: timeout-based node health
 *
 * JSON event (on every valid packet):
 *   {"event":"update","lot":1,"node":1,"bitmap":237,"slots":8,
 *    "occupied":6,"rssi":-45,"seq":42,"hb":false,"uptime_ms":12345}
 *
 * Serial commands (115200 baud):
 *   STATUS   — Full node state + link quality as JSON
 *   PING     — Connectivity check
 *   LINK     — Link quality analysis (RSSI histogram, estimated range)
 *
 * ParkingLite v1.1 — Phenikaa University NCKH 2025-2026
 */

#include <esp_now.h>
#include <esp_wifi.h>
#include <WiFi.h>

// ─── Configuration ──────────────────────────────────────────────────
#define LOT_ID          0x01    // Parking lot identifier (match sensor config)
#define MAX_NODES       8       // Maximum sensor nodes per gateway
#define NODE_TIMEOUT_MS 30000   // Mark node offline after 30 s without packet
#define STATUS_LED_PIN  2       // Built-in LED (active HIGH on most Dev boards)
#define PAYLOAD_VERSION 2       // Expected protocol version

// ─── Payload v2 (must match sensor_node) ─────────────────────────────
typedef struct __attribute__((packed)) {
    uint8_t  version;
    uint8_t  lot_id;
    uint8_t  node_id;
    uint8_t  n_slots;
    uint8_t  bitmap;
    uint8_t  seq;
    int8_t   tx_power;
    uint8_t  flags;     // Bit 0: heartbeat, Bit 1: calibrated
} espnow_payload_v2_t;

// Legacy v1 payload (backward compatibility)
typedef struct __attribute__((packed)) {
    uint8_t node_id;
    uint8_t bitmap;
} espnow_payload_v1_t;

// ─── RSSI Statistics ─────────────────────────────────────────────────
#define RSSI_HISTORY_SIZE 16   // Rolling window for RSSI averaging

typedef struct {
    int8_t   samples[RSSI_HISTORY_SIZE];
    uint8_t  index;
    uint8_t  count;
    int8_t   min_rssi;    // worst seen
    int8_t   max_rssi;    // best seen
} rssi_stats_t;

// ─── Per-Node State ──────────────────────────────────────────────────
typedef struct {
    uint8_t      node_id;
    uint8_t      bitmap;
    uint8_t      n_slots;
    uint8_t      mac[6];
    uint32_t     last_rx_ms;
    uint32_t     rx_count;
    uint8_t      last_seq;
    uint32_t     seq_gaps;     // total missed sequence numbers
    int8_t       tx_power;
    bool         calibrated;
    bool         active;
    rssi_stats_t rssi;
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
    memset(&nodes[idx], 0, sizeof(node_state_t));
    nodes[idx].node_id    = id;
    nodes[idx].active     = true;
    nodes[idx].rssi.min_rssi = 0;
    nodes[idx].rssi.max_rssi = -127;
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

static void rssi_add(rssi_stats_t *s, int8_t rssi) {
    s->samples[s->index] = rssi;
    s->index = (s->index + 1) % RSSI_HISTORY_SIZE;
    if (s->count < RSSI_HISTORY_SIZE) s->count++;
    if (rssi < s->min_rssi) s->min_rssi = rssi;
    if (rssi > s->max_rssi) s->max_rssi = rssi;
}

static int8_t rssi_avg(const rssi_stats_t *s) {
    if (s->count == 0) return -127;
    int32_t sum = 0;
    for (uint8_t i = 0; i < s->count; i++) sum += s->samples[i];
    return (int8_t)(sum / s->count);
}

// RSSI → estimated distance (Log-Distance Path Loss model for parking lot)
// PL(d) = PL(d0) + 10*n*log10(d/d0)
// With d0=1m, PL_d0=40.05 dB (FSPL@2.4GHz@1m), n=2.8 (outdoor parking)
// Antenna gain: TX 2dBi + RX 2dBi = 4 dBi total
// PL_measured = tx_power + 4 - rssi
// d = 10^((PL_measured - 40.05) / (10*n))
#define PL_EXPONENT   2.8f    // Path loss exponent for outdoor parking lot
#define PL_REF_1M     40.05f  // FSPL at 1m for 2.4 GHz
#define ANTENNA_GAIN  4.0f    // TX(2dBi) + RX(2dBi)

static float estimate_distance(int8_t tx_power, int8_t rssi) {
    float pl_measured = (float)tx_power + ANTENNA_GAIN - (float)rssi;
    float exponent = (pl_measured - PL_REF_1M) / (10.0f * PL_EXPONENT);
    if (exponent < 0) return 0.1f;   // closer than 1m
    if (exponent > 4) return 9999.0f; // cap at ~10 km
    // Efficient 10^x without math.h: integer part * fractional approx
    float dist = 1.0f;
    int   int_exp = (int)exponent;
    float frac    = exponent - int_exp;
    for (int i = 0; i < int_exp; i++) dist *= 10.0f;
    // 10^frac ≈ 1 + frac*ln(10) + 0.5*(frac*ln(10))^2 (2nd order Taylor)
    float fl = frac * 2.302585f; // frac * ln(10)
    dist *= (1.0f + fl + 0.5f * fl * fl);
    return dist;
}

static void emit_event_json(int idx, int8_t rssi) {
    uint8_t occ = popcount(nodes[idx].bitmap);
    bool is_hb = false;
    // We track heartbeat flag internally, but emit for the event
    Serial.printf("{\"event\":\"update\",\"lot\":%d,\"node\":%d,"
                  "\"bitmap\":%d,\"slots\":%d,\"occupied\":%d,"
                  "\"rssi\":%d,\"seq\":%d,\"rx_count\":%lu,"
                  "\"seq_gaps\":%lu,\"uptime_ms\":%lu}\n",
                  LOT_ID, nodes[idx].node_id,
                  nodes[idx].bitmap, nodes[idx].n_slots, occ,
                  rssi, nodes[idx].last_seq, nodes[idx].rx_count,
                  nodes[idx].seq_gaps, millis());
}

static void led_blink(void) {
    digitalWrite(STATUS_LED_PIN, HIGH);
    delay(30);
    digitalWrite(STATUS_LED_PIN, LOW);
}


// ─── ESP-NOW Callback ────────────────────────────────────────────────

void OnDataRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
    const uint8_t *mac = info->src_addr;

    // Extract RSSI from the rx_ctrl structure
    int8_t rssi = -127;
    if (info->rx_ctrl) {
        rssi = info->rx_ctrl->rssi;
    }

    uint8_t node_id = 0;
    uint8_t bitmap = 0;
    uint8_t n_slots_rx = 8;
    uint8_t seq = 0;
    int8_t  tx_pwr = 20;
    bool    is_heartbeat = false;
    bool    is_calibrated = false;

    // Detect payload version by length
    if (len == sizeof(espnow_payload_v2_t)) {
        espnow_payload_v2_t pkt;
        memcpy(&pkt, data, sizeof(pkt));
        if (pkt.version != PAYLOAD_VERSION) {
            Serial.printf("[RX_WARN] Unknown protocol version %d\n", pkt.version);
            return;
        }
        node_id      = pkt.node_id;
        bitmap       = pkt.bitmap;
        n_slots_rx   = pkt.n_slots;
        seq          = pkt.seq;
        tx_pwr       = pkt.tx_power;
        is_heartbeat = (pkt.flags & 0x01) != 0;
        is_calibrated = (pkt.flags & 0x02) != 0;
    } else if (len == sizeof(espnow_payload_v1_t)) {
        // Backward compat with v1 sensors
        espnow_payload_v1_t pkt;
        memcpy(&pkt, data, sizeof(pkt));
        node_id = pkt.node_id;
        bitmap  = pkt.bitmap;
    } else {
        Serial.printf("[RX_WARN] Unknown packet length %d bytes\n", len);
        return;
    }

    // Find or register node
    int idx = find_node(node_id);
    if (idx < 0) {
        idx = register_node(node_id, mac);
        if (idx < 0) {
            Serial.println("[RX_WARN] Node table full");
            return;
        }
    }

    // Detect sequence gaps (packet loss)
    if (nodes[idx].rx_count > 0 && len == sizeof(espnow_payload_v2_t)) {
        uint8_t expected_seq = (uint8_t)(nodes[idx].last_seq + 1);
        if (seq != expected_seq) {
            uint8_t gap = (uint8_t)(seq - expected_seq);
            nodes[idx].seq_gaps += gap;
        }
    }

    // Update state
    uint8_t old_bitmap        = nodes[idx].bitmap;
    nodes[idx].bitmap         = bitmap;
    nodes[idx].n_slots        = n_slots_rx;
    nodes[idx].last_rx_ms     = millis();
    nodes[idx].rx_count++;
    nodes[idx].last_seq       = seq;
    nodes[idx].tx_power       = tx_pwr;
    nodes[idx].calibrated     = is_calibrated;
    nodes[idx].active         = true;
    memcpy(nodes[idx].mac, mac, 6);
    rssi_add(&nodes[idx].rssi, rssi);

    // Human-readable log
    uint8_t occ = popcount(bitmap);
    Serial.printf("[RX] %02X:%02X:%02X:%02X:%02X:%02X | "
                  "Node=0x%02X | ",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], node_id);
    if (is_heartbeat) Serial.print("HB ");
    Serial.print("Bitmap=0b");
    print_bitmap_bits(bitmap, n_slots_rx);
    Serial.printf(" (0x%02X) | %d/%d occ | RSSI=%ddBm", bitmap, occ, n_slots_rx, rssi);
    if (old_bitmap != bitmap && !is_heartbeat) {
        Serial.printf(" | CHANGED from 0x%02X", old_bitmap);
    }
    Serial.println();

    // Machine-readable JSON event
    emit_event_json(idx, rssi);

    led_blink();
}


// ═════════════════════════════════════════════════════════════════════
//  Setup
// ═════════════════════════════════════════════════════════════════════

void setup() {
    Serial.begin(115200);
    Serial.println();
    Serial.println("╔════════════════════════════════════╗");
    Serial.println("║   ParkingLite Gateway Node v1.1    ║");
    Serial.println("║   ESP-NOW + RSSI + Link Quality    ║");
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
            Serial.println("║    ParkingLite Gateway v1.1 — Status          ║");
            Serial.println("╠══════════════════════════════════════════════╣");
            Serial.printf( "║  Lot ID: 0x%02X  Uptime: %lu s%*s║\n",
                           LOT_ID, now / 1000,
                           (int)(19 - String(now / 1000).length()), "");
            Serial.printf( "║  Nodes registered: %d / %d                    ║\n",
                           node_count, MAX_NODES);
            Serial.println("╠══════════════════════════════════════════════╣");
            if (node_count == 0) {
                Serial.println("║  No nodes registered yet                     ║");
            }
            for (int i = 0; i < (int)node_count; i++) {
                bool online = (now - nodes[i].last_rx_ms) < NODE_TIMEOUT_MS;
                uint8_t occ = popcount(nodes[i].bitmap);
                int8_t avg = rssi_avg(&nodes[i].rssi);
                uint32_t total = nodes[i].rx_count + nodes[i].seq_gaps;
                uint8_t loss = total > 0 ? (uint8_t)(nodes[i].seq_gaps * 100 / total) : 0;
                Serial.printf("║  Node 0x%02X %s %d/%d occ  RSSI=%ddBm  loss=%d%%  ║\n",
                              nodes[i].node_id, online ? "ON " : "OFF",
                              occ, nodes[i].n_slots, avg, loss);
            }
            Serial.println("╚══════════════════════════════════════════════╝");
            Serial.println();

            // JSON summary with link quality
            Serial.print("[STATUS_JSON] {\"lot\":");
            Serial.print(LOT_ID);
            Serial.print(",\"uptime_ms\":");
            Serial.print(millis());
            Serial.print(",\"nodes\":[");
            for (int i = 0; i < (int)node_count; i++) {
                bool online = (now - nodes[i].last_rx_ms) < NODE_TIMEOUT_MS;
                uint8_t occ = popcount(nodes[i].bitmap);
                int8_t avg = rssi_avg(&nodes[i].rssi);
                float dist = estimate_distance(nodes[i].tx_power, avg);
                Serial.printf("{\"id\":%d,\"bitmap\":%d,\"slots\":%d,\"occupied\":%d,"
                              "\"online\":%s,\"calibrated\":%s,"
                              "\"rssi_avg\":%d,\"rssi_min\":%d,\"rssi_max\":%d,"
                              "\"rx_count\":%lu,\"seq_gaps\":%lu,\"est_dist_m\":%.1f}",
                              nodes[i].node_id, nodes[i].bitmap,
                              nodes[i].n_slots, occ,
                              online ? "true" : "false",
                              nodes[i].calibrated ? "true" : "false",
                              avg, nodes[i].rssi.min_rssi, nodes[i].rssi.max_rssi,
                              nodes[i].rx_count, nodes[i].seq_gaps, dist);
                if (i < (int)node_count - 1) Serial.print(",");
            }
            Serial.println("]}");
        }
        else if (line == "LINK") {
            // Detailed link quality analysis for research
            Serial.println("\n[LINK_QUALITY] ESP-NOW Link Analysis");
            Serial.println("═══════════════════════════════════════");
            Serial.printf("ESP-NOW spec: 2.4 GHz, max 250 bytes/pkt, ~1 Mbps\n");
            Serial.printf("Payload size: %d bytes (v2)\n\n", (int)sizeof(espnow_payload_v2_t));
            for (int i = 0; i < (int)node_count; i++) {
                int8_t avg = rssi_avg(&nodes[i].rssi);
                float dist = estimate_distance(nodes[i].tx_power, avg);
                uint32_t total = nodes[i].rx_count + nodes[i].seq_gaps;
                float loss_f = total > 0 ? (float)nodes[i].seq_gaps * 100.0f / total : 0;
                const char *quality = "UNKNOWN";
                if (avg > -50) quality = "EXCELLENT";
                else if (avg > -60) quality = "GOOD";
                else if (avg > -70) quality = "FAIR";
                else if (avg > -80) quality = "WEAK";
                else quality = "POOR";

                Serial.printf("Node 0x%02X:\n", nodes[i].node_id);
                Serial.printf("  MAC:        %02X:%02X:%02X:%02X:%02X:%02X\n",
                              nodes[i].mac[0], nodes[i].mac[1], nodes[i].mac[2],
                              nodes[i].mac[3], nodes[i].mac[4], nodes[i].mac[5]);
                Serial.printf("  TX Power:   %d dBm\n", nodes[i].tx_power);
                Serial.printf("  RSSI avg:   %d dBm (%s)\n", avg, quality);
                Serial.printf("  RSSI range: %d to %d dBm\n",
                              nodes[i].rssi.min_rssi, nodes[i].rssi.max_rssi);
                Serial.printf("  Est. dist:  %.1f m (log-distance n=%.1f)\n", dist, PL_EXPONENT);
                Serial.printf("  Packets:    %lu received, %lu gaps (%.1f%% loss)\n",
                              nodes[i].rx_count, nodes[i].seq_gaps, loss_f);
                Serial.printf("  Calibrated: %s\n\n", nodes[i].calibrated ? "YES" : "NO");
            }
            if (node_count == 0) Serial.println("  No nodes registered yet.\n");

            // JSON for programmatic use
            Serial.print("[LINK_JSON] {\"nodes\":[");
            for (int i = 0; i < (int)node_count; i++) {
                int8_t avg = rssi_avg(&nodes[i].rssi);
                float dist = estimate_distance(nodes[i].tx_power, avg);
                Serial.printf("{\"id\":%d,\"tx_pwr\":%d,\"rssi\":%d,"
                              "\"dist_m\":%.1f,\"rx\":%lu,\"gaps\":%lu}",
                              nodes[i].node_id, nodes[i].tx_power, avg,
                              dist, nodes[i].rx_count, nodes[i].seq_gaps);
                if (i < (int)node_count - 1) Serial.print(",");
            }
            Serial.println("]}");
        }
        else if (line == "PING") {
            Serial.println("[PONG]");
        }
        else if (line.length() > 0) {
            Serial.println("Commands: STATUS | LINK | PING");
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

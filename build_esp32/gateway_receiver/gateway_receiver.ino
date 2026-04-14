/*
 * gateway_receiver.ino — Simple ESP-NOW Receiver for ESP32 Dev Board
 * Receives bitmap parking data from ESP32-CAM sensor nodes.
 */

#include <esp_now.h>
#include <WiFi.h>

// Structure must match the sender's format
typedef struct struct_message {
    uint8_t node_id;
    uint8_t bitmap;
} struct_message;

struct_message incomingReadings;

// Callback function that will be executed when data is received
void OnDataRecv(const esp_now_recv_info_t * esp_now_info, const uint8_t *incomingData, int len) {
    if (len == sizeof(struct_message)) {
        memcpy(&incomingReadings, incomingData, sizeof(incomingReadings));
        
        Serial.print("[RX_GATEWAY] Received from: ");
        for (int i=0; i<6; i++) {
            Serial.printf("%02X", esp_now_info->src_addr[i]);
            if (i<5) Serial.print(":");
        }
        
        Serial.printf(" | Node ID: 0x%02X | Bitmap: 0x%02X (0b", incomingReadings.node_id, incomingReadings.bitmap);
        for(int i=7; i>=0; i--) {
            Serial.print((incomingReadings.bitmap >> i) & 1);
        }
        Serial.println(")");
    } else {
        Serial.printf("[RX_GATEWAY] Received bad data length: %d bytes\n", len);
    }
}

void setup() {
    Serial.begin(115200);
    Serial.println("\n=================================");
    Serial.println("  Smart Parking Gateway Node");
    Serial.println("  ESP-NOW Receiver");
    Serial.println("=================================\n");

    // Set device as a Wi-Fi Station
    WiFi.mode(WIFI_STA);
    
    // Output our MAC Address so the sender could know (optional, we use broadcast)
    Serial.print("Gateway MAC Address: ");
    Serial.println(WiFi.macAddress());

    // Initialize ESP-NOW
    if (esp_now_init() != ESP_OK) {
        Serial.println("[ERROR] Error initializing ESP-NOW");
        return;
    }
    
    // Register for recv CB to get recv packer info
    esp_now_register_recv_cb(OnDataRecv);
    
    Serial.println("[READY] Listening for Sensor Nodes...");
}

void loop() {
    delay(1000);
}

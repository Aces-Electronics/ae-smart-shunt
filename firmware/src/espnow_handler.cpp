#include "espnow_handler.h"
#include "esp_err.h"
#include <cstring>
#include "tpms_handler.h" // Access to tpmsHandler for config updates

// Callback outside class
static ESPNowHandler* g_espNowHandler = nullptr;

static void OnDataRecv(const uint8_t * mac, const uint8_t *incomingData, int len) {

    
    if (len == sizeof(struct_message_tpms_config)) {
        struct_message_tpms_config config;
        memcpy(&config, incomingData, sizeof(config));
        
        Serial.printf("[ESP-NOW RX] Possible TPMS config, messageID=%d\n", config.messageID);
        
        if (config.messageID == 99) {
            Serial.println("[ESP-NOW] Received TPMS Config (ID 99)");
            for(int i=0; i<4; i++) {
                Serial.printf("  Pos %d: %02X:%02X:%02X:%02X:%02X:%02X (Base: %.1f, En: %d)\n", i,
                    config.macs[i][0], config.macs[i][1], config.macs[i][2],
                    config.macs[i][3], config.macs[i][4], config.macs[i][5],
                    config.baselines[i], config.configured[i]);
            }
            tpmsHandler.setConfig(config.macs, config.baselines, config.configured);
        }
    } else if (len == sizeof(struct_message_temp_sensor)) {
         struct_message_temp_sensor sensorData;
         memcpy(&sensorData, incomingData, sizeof(sensorData));
         
         if (sensorData.id == 22 && g_espNowHandler != nullptr) {
             Serial.printf("[ESP-NOW] Rx Temp Sensor: %.2f C, %d%%\n", sensorData.temperature, sensorData.batteryLevel);
             g_espNowHandler->updateTempSensorData(sensorData.temperature, sensorData.batteryLevel);
         }
    }
}

// 🔒 Compile-time check: catch padding/alignment mismatches.
// Update "EXPECTED_AE_SMART_SHUNT_STRUCT_SIZE" if your struct changes.
// Update "EXPECTED_AE_SMART_SHUNT_STRUCT_SIZE" if your struct changes.
#define EXPECTED_AE_SMART_SHUNT_STRUCT_SIZE 183   // Updated: 174 + 9 (Temp Sensor)
static_assert(sizeof(struct_message_ae_smart_shunt_1) == EXPECTED_AE_SMART_SHUNT_STRUCT_SIZE,
              "struct_message_ae_smart_shunt_1 has unexpected size! Possible padding/alignment issue.");

ESPNowHandler::ESPNowHandler(const uint8_t *broadcastAddr)
{
    g_espNowHandler = this;
    memcpy(broadcastAddress, broadcastAddr, 6);
    memset(&peerInfo, 0, sizeof(peerInfo));
    // Optionally zero the local struct
    memset(&localAeSmartShuntStruct, 0, sizeof(localAeSmartShuntStruct));
    memset(targetPeer, 0, 6);
}

void ESPNowHandler::setAeSmartShuntStruct(const struct_message_ae_smart_shunt_1 &shuntStruct)
{
    // shallow copy of struct (same layout). If you need cross-platform compatibility,
    // serialize the fields into a packed buffer instead.
    localAeSmartShuntStruct = shuntStruct;
}

void ESPNowHandler::printMacAddress(const uint8_t* mac) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    Serial.println(buf);
}

void ESPNowHandler::sendMessageAeSmartShunt()
{
    uint8_t *data = (uint8_t *)&localAeSmartShuntStruct;
    size_t len = sizeof(localAeSmartShuntStruct);

    Serial.printf("Struct size: %d bytes\n", len);
    
    if (isSecure) {
        // Send Encrypted Unicast to Target (no broadcast in secure mode)
        Serial.print("Sending Encrypted to: ");
        printMacAddress(targetPeer);
        esp_err_t res = esp_now_send(targetPeer, data, len);
        if (res != ESP_OK) Serial.println("Encrypted Send Failed");
        return; // Done - no broadcast in secure mode
    }
    
    // Else: Not paired, use broadcast for discovery
    Serial.print("Sending Broadcast to: ");
    printMacAddress(broadcastAddress);
    
    // Explicitly set ID to 33 (Discovery Beacon) for unencrypted broadcasts
    localAeSmartShuntStruct.messageID = 33;

    Serial.println();

    esp_err_t result = esp_now_send(broadcastAddress, data, len);
    if (result == ESP_OK)
    {
        Serial.println("Sent AE Smart Shunt message successfully");
    }
    else
    {
        Serial.print("Error sending AeSmartShunt data: ");
        Serial.println(esp_err_to_name(result));
    }
}

bool ESPNowHandler::addEncryptedPeer(const uint8_t* mac, const uint8_t* key)
{
    esp_now_peer_info_t securePeer;
    memset(&securePeer, 0, sizeof(securePeer));
    memcpy(securePeer.peer_addr, mac, 6);

    securePeer.channel = 0;
    securePeer.encrypt = true;
    memcpy(securePeer.lmk, key, 16);

    // Remove if exists
    if (esp_now_is_peer_exist(mac)) {
        esp_now_del_peer(mac);
    }

    if (esp_now_add_peer(&securePeer) != ESP_OK) {
        Serial.println("Failed to add secure peer");
        return false;
    }
    Serial.println("Secure peer added");
    return true;
}

void ESPNowHandler::switchToSecureMode(const uint8_t* gaugeMac)
{
    memcpy(targetPeer, gaugeMac, 6);
    isSecure = true;
    Serial.println("Switched to Secure Mode");
}

bool ESPNowHandler::begin()
{
    WiFi.mode(WIFI_MODE_STA);
    // Disable WiFi scan power save if needed (optional).
    if (esp_now_init() != ESP_OK)
    {
        Serial.println("Error initializing ESP-NOW");
        return false;
    }
    esp_now_register_recv_cb(OnDataRecv);
    return true;
}

void ESPNowHandler::registerSendCallback(esp_now_send_cb_t callback)
{
    esp_now_register_send_cb(callback);
}

bool ESPNowHandler::addPeer()
{
    memset(&peerInfo, 0, sizeof(peerInfo));
    memcpy(peerInfo.peer_addr, broadcastAddress, 6);
    peerInfo.channel = 0; // 0 means current WiFi channel. Set explicitly if needed.
    peerInfo.encrypt = false;

    if (esp_now_add_peer(&peerInfo) != ESP_OK)
    {
        Serial.println("Failed to add peer");
        return false;
    }
    return true;
}

void ESPNowHandler::updateTempSensorData(float temp, uint8_t batt) {
    localAeSmartShuntStruct.tempSensorTemperature = temp;
    localAeSmartShuntStruct.tempSensorBatteryLevel = batt;
    localAeSmartShuntStruct.tempSensorLastUpdate = millis();
}

void ESPNowHandler::getTempSensorData(float &temp, uint8_t &batt, uint32_t &lastUpdate) {
    temp = localAeSmartShuntStruct.tempSensorTemperature;
    batt = localAeSmartShuntStruct.tempSensorBatteryLevel;
    lastUpdate = localAeSmartShuntStruct.tempSensorLastUpdate;
}

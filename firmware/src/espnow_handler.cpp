#include "espnow_handler.h"
#include "esp_err.h"
#include <cstring>
#include "tpms_handler.h" // Access to tpmsHandler for config updates

// Callback outside class
static ESPNowHandler* g_espNowHandler = nullptr;

static void OnDataRecv(const uint8_t * mac, const uint8_t *incomingData, int len) {

    Serial.printf("[ESP-NOW RAW] Rx from %02X:%02X:%02X:%02X:%02X:%02X, Len=%d\n", 
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], len);

    // 0. Check for Gauge RX (Any data from Paired Gauge)
    if (g_espNowHandler && g_espNowHandler->isGaugeMac(mac)) {
         g_espNowHandler->recordGaugeRx();
         // Don't return, continue parsing (it might be a specific command)
    }

    // 1. Check for TPMS Config
    if (len == sizeof(struct_message_tpms_config)) {
        struct_message_tpms_config config;
        memcpy(&config, incomingData, sizeof(config));
        
        Serial.printf("[ESP-NOW RX] Possible TPMS config, messageID=%d\n", config.messageID);
        
        if (config.messageID == 99) {
            // SECURITY: Only accept Config from Paired Gauge (or if we are not paired yet)
            bool accept = true;
            if (g_espNowHandler && g_espNowHandler->isPaired()) { // Helper method to check if p_gauge_mac is set
                 if (!g_espNowHandler->isGaugeMac(mac)) {
                     Serial.println("[ESP-NOW] REJECTING TPMS Config from Unknown MAC (Expected Paired Gauge)");
                     accept = false;
                 }
            }

            if (accept) {
                Serial.println("[ESP-NOW] Received TPMS Config (ID 99)");
                for(int i=0; i<4; i++) {
                    Serial.printf("  Pos %d: %02X:%02X:%02X:%02X:%02X:%02X (Base: %.1f, En: %d)\n", i,
                        config.macs[i][0], config.macs[i][1], config.macs[i][2],
                        config.macs[i][3], config.macs[i][4], config.macs[i][5],
                        config.baselines[i], config.configured[i]);
                }
                tpmsHandler.setConfig(config.macs, config.baselines, config.configured);
            }
        }
        return;
    } 
    
     if (len == sizeof(struct_message_temp_sensor)) {
          struct_message_temp_sensor sensorData;
          memcpy(&sensorData, incomingData, sizeof(sensorData));
          
          if (sensorData.id == 22 && g_espNowHandler != nullptr) {
               // Logged inside updateTempSensorData
               g_espNowHandler->updateTempSensorData(sensorData.temperature, sensorData.batteryLevel, sensorData.updateInterval, sensorData.name);
               
               Serial.println("=== RX Temp Sensor ===");
               Serial.printf("  ID      : %d\n", sensorData.id);
               Serial.printf("  Temp    : %.1f C\n", sensorData.temperature);
               Serial.printf("  Batt V  : %.2f V\n", sensorData.batteryVoltage);
               Serial.printf("  Batt %%  : %d %%\n", sensorData.batteryLevel);
               Serial.printf("  Interval: %u ms\n", sensorData.updateInterval);
               Serial.println("======================");
          }
          return;
     } 
    
    // 3. Check for Add Peer Command
    if (len == sizeof(struct_message_add_peer)) {
         struct_message_add_peer peerMsg;
         memcpy(&peerMsg, incomingData, sizeof(peerMsg));
         
         if (peerMsg.messageID == 200 && g_espNowHandler != nullptr) {
             Serial.println("[ESP-NOW] Received ADD PEER Command");
             g_espNowHandler->handleNewPeer(peerMsg.mac, peerMsg.key);
         }
         return;
    }
}

// 🔒 Compile-time check: catch padding/alignment mismatches.
// Update "EXPECTED_AE_SMART_SHUNT_STRUCT_SIZE" if your struct changes.
// Update "EXPECTED_AE_SMART_SHUNT_STRUCT_SIZE" if your struct changes.
#define EXPECTED_AE_SMART_SHUNT_STRUCT_SIZE 211   // Updated: 187 + 24 (Name)
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
    
    // Explicitly zero temp sensor storage
    rawTempC = 0.0f;
    rawTempBatt = 0;
    rawTempLastUpdate = 0;
    rawTempInterval = 0;
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
    
    if (isSecure && !m_forceBroadcast) {
        // Send Encrypted Unicast to Target (no broadcast in secure mode unless forced)
        Serial.print("Sending Encrypted to: ");
        printMacAddress(targetPeer);
        esp_err_t res = esp_now_send(targetPeer, data, len);
        if (res != ESP_OK) Serial.println("Encrypted Send Failed");
        return; // Done - no broadcast in secure mode
    }
    
    // Else: Not paired OR Forced Broadcast during pairing
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
    WiFi.setSleep(false); // Critical for BLE co-existence
    // Disable WiFi scan power save if needed (optional).
    if (esp_now_init() != ESP_OK)
    {
        Serial.println("Error initializing ESP-NOW");
        return false;
    }
    esp_now_register_recv_cb(OnDataRecv);
    Serial.printf("[DEBUG] sizeof(struct_message_temp_sensor) = %d\n", sizeof(struct_message_temp_sensor));

    // --- FIX: Load Saved Secure Peer from NVS ---
    Preferences prefs;
    prefs.begin("pairing", true); // Read-only
    String macStr = prefs.getString("p_temp_mac", "");
    String keyHex = prefs.getString("p_temp_key", "");
    prefs.end();

    if (macStr.length() == 12 && keyHex.length() == 32) {
        Serial.printf("[ESP-NOW] Loading Saved Peer: MAC=%s, Key=%s\n", macStr.c_str(), keyHex.c_str());
        
        uint8_t mac[6];
        uint8_t key[16];
        
        // Parse MAC (Hex string no colons)
        for(int i=0; i<6; i++) {
             char buf[3] = { macStr[i*2], macStr[i*2+1], '\0' };
             mac[i] = (uint8_t)strtoul(buf, NULL, 16);
        }
        
        // Parse Key
        for(int i=0; i<16; i++) {
             char buf[3] = { keyHex[i*2], keyHex[i*2+1], '\0' };
             key[i] = (uint8_t)strtoul(buf, NULL, 16);
        }
        
        addEncryptedPeer(mac, key);
    } else {
        Serial.println("[ESP-NOW] No saved secure peer found in NVS.");
    }

    return true;
}

#include <Preferences.h>

void ESPNowHandler::handleNewPeer(const uint8_t* mac, const uint8_t* key)
{
    // 1. Add to Runtime
    addEncryptedPeer(mac, key);
    
    // 2. Save to NVS so it persists after reboot
    Preferences prefs;
    prefs.begin("pairing", false);
    
    char keyHex[33];
    char macStr[18];
    
    snprintf(macStr, sizeof(macStr), "%02X%02X%02X%02X%02X%02X", 
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
             
    for(int i=0; i<16; i++) sprintf(&keyHex[i*2], "%02X", key[i]);
    keyHex[32] = '\0';
    
    prefs.putString("p_temp_mac", macStr); // Hex string without colons
    prefs.putString("p_temp_key", keyHex);
    prefs.end();
    
    Serial.println("New Peer Saved to NVS (p_temp_mac)");
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

void ESPNowHandler::updateTempSensorData(float temp, uint8_t batt, uint32_t interval, const char* name) {
    rawTempC = temp;
    rawTempBatt = batt;
    rawTempInterval = interval;
    if (name) {
        strncpy(rawTempName, name, sizeof(rawTempName)-1);
        rawTempName[sizeof(rawTempName)-1] = '\0';
    }
    rawTempLastUpdate = millis();
    Serial.printf("[ESP-NOW] Rx Temp Sensor: %.1f C, Batt %d%%, Interval %u ms, Name: %s\n", temp, batt, interval, rawTempName);
}

void ESPNowHandler::getTempSensorData(float &temp, uint8_t &batt, uint32_t &lastUpdate, uint32_t &interval, char* nameBuf) {
    temp = rawTempC;
    batt = rawTempBatt;
    lastUpdate = rawTempLastUpdate;
    interval = rawTempInterval;
    if (nameBuf) {
        strncpy(nameBuf, rawTempName, 24); // Assuming caller buffer is sufficient (24)
    }
}

void ESPNowHandler::recordGaugeRx() {
    lastGaugeRxTime = millis();
}

bool ESPNowHandler::isPaired() {
    // We are considered "paired" if we are in secure mode (encrypted link to Gauge established)
    // or if we have a saved peer.
    // However, isSecure is set only after switchToSecureMode is called.
    return isSecure;
}


uint32_t ESPNowHandler::getLastGaugeRx() {
    return lastGaugeRxTime;
}

bool ESPNowHandler::isGaugeMac(const uint8_t* mac) {
    if (!isSecure) return false;
    return (memcmp(mac, targetPeer, 6) == 0);
}

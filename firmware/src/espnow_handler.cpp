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
               g_espNowHandler->updateTempSensorData(mac, sensorData.temperature, sensorData.batteryLevel, sensorData.updateInterval, sensorData.name, sensorData.hardwareVersion, sensorData.firmwareVersion);
               
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
#define EXPECTED_AE_SMART_SHUNT_STRUCT_SIZE 298   // Updated for Gauge relay + MAC fields
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
    memset(rawTempMac, 0, sizeof(rawTempMac));
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
    // We only send the core mesh telemetry over ESP-NOW to stay under 250-byte limit
    uint8_t *data = (uint8_t *)&localAeSmartShuntStruct.mesh;
    size_t len = sizeof(struct_message_ae_smart_shunt_mesh);

    Serial.printf("Struct size: %d bytes (Full: %d)\n", len, sizeof(localAeSmartShuntStruct));
    
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
    localAeSmartShuntStruct.mesh.messageID = 33;

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
    localAeSmartShuntStruct.mesh.messageID = 11; // Restore standard ID after broadcast
}

void ESPNowHandler::sendOtaTrigger(const uint8_t* targetMac, const struct_message_ota_trigger& trigger)
{
    Serial.print("[ESP-NOW] Sending OTA Trigger to: ");
    printMacAddress(targetMac);
    
    // Ensure peer exists
    if (!esp_now_is_peer_exist(targetMac)) {
        esp_now_peer_info_t peer;
        memset(&peer, 0, sizeof(peer));
        memcpy(peer.peer_addr, targetMac, 6);
        peer.channel = 0;
        peer.encrypt = false;
        if (esp_now_add_peer(&peer) != ESP_OK) {
            Serial.println("[ESP-NOW] Failed to add Peer for OTA Trigger");
            return;
        }
    }
    
    esp_err_t result = esp_now_send(targetMac, (const uint8_t *)&trigger, sizeof(trigger));
    if (result == ESP_OK) {
        Serial.println("[ESP-NOW] OTA Trigger sent successfully");
    } else {
        Serial.printf("[ESP-NOW] Error sending OTA Trigger: %s\n", esp_err_to_name(result));
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
    if (esp_now_init() != ESP_OK)
    {
        Serial.println("Error initializing ESP-NOW");
        return false;
    }
    
    // 1. Re-register Callbacks
    esp_now_register_recv_cb(OnDataRecv);
    if (m_sendCallback) {
        esp_now_register_send_cb(m_sendCallback);
        Serial.println("[ESP-NOW] Send callback re-registered");
    }

    // 2. Add Broadcast Peer
    addPeer(); 

    // 3. Restore Saved Peers from NVS
    Preferences prefs;
    
    // --- Gauge Peer ---
    prefs.begin("pairing", true);
    String g_macStr = prefs.getString("p_gauge_mac", "");
    String g_keyHex = prefs.getString("p_key", "");
    prefs.end();

    if (g_macStr.length() > 0 && g_keyHex.length() == 32) {
        Serial.printf("[ESP-NOW] Restoring Gauge Peer: MAC=%s\n", g_macStr.c_str());
        uint8_t mac[6];
        uint8_t key[16];
        
        g_macStr.replace(":", "");
        if (g_macStr.length() == 12) {
            for(int i=0; i<6; i++) {
                char buf[3] = { g_macStr[i*2], g_macStr[i*2+1], '\0' };
                mac[i] = (uint8_t)strtoul(buf, NULL, 16);
            }
            for(int i=0; i<16; i++) {
                char buf[3] = { g_keyHex[i*2], g_keyHex[i*2+1], '\0' };
                key[i] = (uint8_t)strtoul(buf, NULL, 16);
            }
            addEncryptedPeer(mac, key);
            switchToSecureMode(mac);
        }
    }

    // --- Temp Sensor Peer ---
    prefs.begin("pairing", true);
    String t_macStr = prefs.getString("p_temp_mac", "");
    String t_keyHex = prefs.getString("p_temp_key", "");
    prefs.end();

    if (t_macStr.length() == 12 && t_keyHex.length() == 32) {
        Serial.printf("[ESP-NOW] Restoring Temp Sensor Peer: MAC=%s\n", t_macStr.c_str());
        uint8_t mac[6];
        uint8_t key[16];
        for(int i=0; i<6; i++) {
            char buf[3] = { t_macStr[i*2], t_macStr[i*2+1], '\0' };
            mac[i] = (uint8_t)strtoul(buf, NULL, 16);
        }
        for(int i=0; i<16; i++) {
            char buf[3] = { t_keyHex[i*2], t_keyHex[i*2+1], '\0' };
            key[i] = (uint8_t)strtoul(buf, NULL, 16);
        }
        addEncryptedPeer(mac, key);
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
    m_sendCallback = callback;
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

void ESPNowHandler::updateTempSensorData(const uint8_t* mac, float temp, uint8_t batt, uint32_t interval, const char* name, uint8_t hwVersion, const char* fwVersion)
{
    rawTempC = temp;
    rawTempBatt = batt;
    rawTempInterval = interval;
    rawTempLastUpdate = millis();
    rawTempHwVersion = hwVersion;
    if (name) strncpy(rawTempName, name, sizeof(rawTempName) - 1);
    if (fwVersion) strncpy(rawTempFwVersion, fwVersion, sizeof(rawTempFwVersion) - 1);
    
    if (mac) {
        snprintf(rawTempMac, sizeof(rawTempMac), "%02X:%02X:%02X:%02X:%02X:%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
}

String ESPNowHandler::getTempSensorMac() {
    return String(rawTempMac);
}

void ESPNowHandler::getTempSensorData(float &temp, uint8_t &batt, uint32_t &lastUpdate, uint32_t &interval, char* nameBuf, uint8_t &hwVersion, char* fwVersionBuf)
{
    temp = rawTempC;
    batt = rawTempBatt;
    lastUpdate = rawTempLastUpdate;
    interval = rawTempInterval;
    hwVersion = rawTempHwVersion;
    if (nameBuf) strncpy(nameBuf, rawTempName, 23);
    if (fwVersionBuf) strncpy(fwVersionBuf, rawTempFwVersion, 11);
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

void ESPNowHandler::loadGaugeDataFromNVS() {
    Preferences prefs;
    prefs.begin("pairing", true); // read-only
    
    String macStr = prefs.getString("p_gauge_mac", "");
    String name = prefs.getString("p_gauge_name", "AE Gauge");
    
    prefs.end();
    
    if (macStr.length() > 0) {
        // Parse MAC string to binary
        macStr.replace(":", "");
        if (macStr.length() == 12) {
            for (int i = 0; i < 6; i++) {
                char buf[3] = { macStr[i*2], macStr[i*2+1], '\0' };
                rawGaugeMac[i] = (uint8_t)strtoul(buf, NULL, 16);
            }
            
            // CRITICAL: Update Last Update Logic
            // If we have just loaded valid gauge data from NVS, and we have live data, ensure timestamps align?
            // Actually, rawGaugeMac being non-zero is enough for MQTT uplink to send "Offline/Old" status
            // instead of "Missing" status.
            
        } else {
             Serial.printf("[ESP-NOW] loadGaugeDataFromNVS: MAC Length Invalid (%d bytes): '%s'\n", macStr.length(), macStr.c_str());
        }
        
        strncpy(rawGaugeName, name.c_str(), sizeof(rawGaugeName) - 1);
        rawGaugeHwVersion = 1; // Default, Gauge doesn't report this yet
        strncpy(rawGaugeFwVersion, "unknown", sizeof(rawGaugeFwVersion) - 1);
        rawGaugeLastUpdate = lastGaugeRxTime;
        
        Serial.printf("[ESP-NOW] Loaded Gauge from NVS: %s (%s)\n", rawGaugeName, macStr.c_str());
    } else {
        Serial.println("[ESP-NOW] loadGaugeDataFromNVS: MAC String is empty or invalid (Len=0)");
    }
}

void ESPNowHandler::getGaugeData(char* nameBuf, uint8_t &hwVersion, char* fwVersionBuf, uint8_t* macBuf, uint32_t &lastUpdate) {
    if (nameBuf) strncpy(nameBuf, rawGaugeName, 31);
    hwVersion = rawGaugeHwVersion;
    if (fwVersionBuf) strncpy(fwVersionBuf, rawGaugeFwVersion, 11);
    if (macBuf) memcpy(macBuf, rawGaugeMac, 6);
    lastUpdate = rawGaugeLastUpdate;
}

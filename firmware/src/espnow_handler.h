#ifndef ESPNOW_HANDLER_H
#define ESPNOW_HANDLER_H

#include <Arduino.h>
#include <esp_now.h>
#include <WiFi.h>
#include "shared_defs.h" // defines struct_message_ae_smart_shunt_1

class ESPNowHandler {
public:
    ESPNowHandler(const uint8_t *broadcastAddr);

    bool begin();
    void registerSendCallback(esp_now_send_cb_t callback);
    bool addPeer();

    // Copy the struct into the handler for later sending
    void setAeSmartShuntStruct(const struct_message_ae_smart_shunt_1 &shuntStruct);

    // Send the currently stored struct using ESP-NOW
    // If secure mode is active, sends encrypted unicast to targetPeer + insecure beacon (optional)
    // Else broadcasts insecurely.
    void sendMessageAeSmartShunt();
    void sendOtaTrigger(const uint8_t* targetMac, const struct_message_ota_trigger& trigger);
    void setForceBroadcast(bool force) { m_forceBroadcast = force; }
    
    bool addEncryptedPeer(const uint8_t* mac, const uint8_t* key);
    void switchToSecureMode(const uint8_t* gaugeMac);
    
    // Process incoming Temp Sensor Data
    void updateTempSensorData(const uint8_t* mac, float temp, uint8_t batt, uint32_t interval, const char* name, uint8_t hwVersion, const char* fwVersion);
    void getTempSensorData(float &temp, uint8_t &batt, uint32_t &lastUpdate, uint32_t &interval, char* nameBuf, uint8_t &hwVersion, char* fwVersionBuf);
    
    // Helper to get the MAC of the last reported temp sensor
    String getTempSensorMac();
    
    // Gauge RX Tracking
    void recordGaugeRx();
    uint32_t getLastGaugeRx();
    bool isGaugeMac(const uint8_t* mac);
    bool isPaired();
    
    // Gauge Data Management
    void loadGaugeDataFromNVS(); // Load paired Gauge info from NVS
    void getGaugeData(char* nameBuf, uint8_t &hwVersion, char* fwVersionBuf, uint8_t* macBuf, uint32_t &lastUpdate);
    
    // Handle new peer request (save to NVS + add to ESP-NOW)
    void handleNewPeer(const uint8_t* mac, const uint8_t* key);

private:
    uint8_t broadcastAddress[6];
    esp_now_peer_info_t peerInfo;
    struct_message_ae_smart_shunt_1 localAeSmartShuntStruct;
    
    // Dedicated storage for incoming Temp Sensor Data (to avoid feedback loop with outgoing struct)
    float rawTempC = 0.0f;
    uint8_t rawTempBatt = 0;
    uint32_t rawTempLastUpdate = 0; // Timestamp (millis) of last RX
    uint32_t rawTempInterval = 0;
    char rawTempName[32] = {0};
    uint8_t rawTempHwVersion = 0;
    char rawTempFwVersion[12] = {0};
    char rawTempMac[18] = {0}; // Add storage for MAC string "AA:BB:CC:DD:EE:FF"
    
    // Gauge Data Storage
    char rawGaugeName[32] = {0};
    uint8_t rawGaugeHwVersion = 0;
    char rawGaugeFwVersion[12] = {0};
    uint8_t rawGaugeMac[6] = {0}; // Binary MAC
    uint32_t rawGaugeLastUpdate = 0;

    bool isSecure = false;
public: // Made public for static callback access (or add friend/getter)
    uint8_t targetPeer[6];
    uint32_t lastGaugeRxTime = 0;
    bool m_forceBroadcast = false;
    esp_now_send_cb_t m_sendCallback = nullptr;

    // OTA Trigger Queueing (for when ESP-NOW is paused during WiFi uplink)
    void queueOtaTrigger(const uint8_t* targetMac, const struct_message_ota_trigger& trigger);
    void processQueuedOtaTrigger();

private:
    void printMacAddress(const uint8_t* mac);

    // Queue storage
    bool pendingOtaTrigger = false;
    struct_message_ota_trigger queuedOtaTrigger;
    uint8_t queuedOtaTarget[6];
};

#endif // ESPNOW_HANDLER_H
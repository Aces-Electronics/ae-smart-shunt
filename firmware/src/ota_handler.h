#ifndef OTA_HANDLER_H
#define OTA_HANDLER_H

#include <Arduino.h>
#include <WiFiClientSecure.h>
#include <functional>
#include "ble_handler.h"
#include "espnow_handler.h"

class OtaHandler {
public:
    OtaHandler(BLEHandler& bleHandler, ESPNowHandler& espNowHandler, WiFiClientSecure& wifi_client);
    void begin();
    void loop();

    void setWifiSsid(const String& ssid);
    void setWifiPass(const String& pass);
    void triggerOta();
    void setPreUpdateCallback(std::function<void()> callback);

private:
    void runBleOtaUpdate();
    bool handleOTA();

    BLEHandler& bleHandler;
    ESPNowHandler& espNowHandler;
    WiFiClientSecure& wifi_client;
    std::function<void()> pre_update_callback;

    String wifi_ssid;
    String wifi_pass;
    bool ota_triggered = false;
};

#endif // OTA_HANDLER_H
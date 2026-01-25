#ifndef OTA_HANDLER_H
#define OTA_HANDLER_H

#include <Arduino.h>
#include <WiFiClientSecure.h>
#include <functional>
#include "ble_handler.h"
#include "espnow_handler.h"
#include <OTA-Hub.hpp>
#include <Preferences.h>

class OtaHandler {
public:
    OtaHandler(BLEHandler& bleHandler, ESPNowHandler& espNowHandler, WiFiClientSecure& wifi_client);
    void begin();
    void loop();

    void setWifiSsid(const String& ssid);
    void setWifiPass(const String& pass);
    String getWifiSsid() { return wifi_ssid; }
    String getWifiPass() { return wifi_pass; }
    void handleOtaControl(uint8_t command);
    void setPreUpdateCallback(std::function<void()> callback);


private:
    void checkForUpdate();
    void startUpdate();

    BLEHandler& bleHandler;
    ESPNowHandler& espNowHandler;
    WiFiClientSecure& wifi_client;
    std::function<void()> pre_update_callback;
    OTA::UpdateObject latest_update_details;

    String wifi_ssid;
    String wifi_pass;

    bool check_for_update_pending = false;
    bool start_update_pending = false;

    enum OtaState {
        OTA_IDLE,
        OTA_WIFI_CONNECTING,
        OTA_CHECKING_FOR_UPDATE,
        OTA_UPDATE_AVAILABLE,
        OTA_IN_PROGRESS,
        OTA_FAILED
    };

    OtaState ota_state = OTA_IDLE;
    unsigned long ota_wifi_start_time = 0;
};

#endif // OTA_HANDLER_H
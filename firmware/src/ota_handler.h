#ifndef OTA_HANDLER_H
#define OTA_HANDLER_H

#include <Arduino.h>
#include "ble_handler.h"
#include "espnow_handler.h"
#include "ina226_adc.h"

class OtaHandler {
public:
    OtaHandler(BLEHandler& bleHandler, ESPNowHandler& espNowHandler, INA226_ADC& ina226_adc, struct_message_ae_smart_shunt_1& shunt_struct);
    void begin();
    void loop();

    void setWifiSsid(const String& ssid);
    void setWifiPass(const String& pass);
    void triggerOta();

private:
    void runBleOtaUpdate();
    bool handleOTA();

    BLEHandler& bleHandler;
    ESPNowHandler& espNowHandler;
    INA226_ADC& ina226_adc;
    struct_message_ae_smart_shunt_1& ae_smart_shunt_struct;

    String wifi_ssid;
    String wifi_pass;
    bool ota_triggered = false;
};

#endif // OTA_HANDLER_H
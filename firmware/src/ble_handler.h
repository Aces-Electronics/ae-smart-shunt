#ifndef BLE_HANDLER_H
#define BLE_HANDLER_H

#include <NimBLEDevice.h>
#include <NimBLEAdvertisedDevice.h>
#include "NimBLEBeacon.h"
#include "shared_defs.h"
#include <aes/esp_aes.h>

#include "passwords.h"

#include <Arduino.h>

struct struct_message_voltage0;

class BLEHandler : public BLEAdvertisedDeviceCallbacks {
    public:
        BLEHandler(struct_message_voltage0* voltageStruct);
        void startScan(int scanTimeSeconds);
        void stopScan();
    
        void onResult(BLEAdvertisedDevice* advertisedDevice) override;
    
    private:
        char convertCharToHex(char ch);
        void prtnib(int n);
    
        char savedDeviceName[32];
        int keyBits = 128;
    
        struct_message_voltage0* voltageStruct;
    };

#endif // BLE_HANDLER_H
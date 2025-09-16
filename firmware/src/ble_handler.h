#ifndef BLE_HANDLER_H
#define BLE_HANDLER_H

#include <NimBLEDevice.h>

// Define a struct to hold all the telemetry data
struct Telemetry {
    float batteryVoltage;
    float batteryCurrent;
    float batteryPower;
    float batterySOC;
    float batteryCapacity;
    float starterBatteryVoltage;
    bool isCalibrated;
};

class BLEHandler {
public:
    BLEHandler();
    void begin();
    void updateTelemetry(const Telemetry& telemetry);

public:
    // Service and Characteristic UUIDs
    // Using UUIDs from a random generator
    static const char* SERVICE_UUID;
    static const char* VOLTAGE_CHAR_UUID;
    static const char* CURRENT_CHAR_UUID;
    static const char* POWER_CHAR_UUID;
    static const char* SOC_CHAR_UUID;
    static const char* CAPACITY_CHAR_UUID;
    static const char* STARTER_VOLTAGE_CHAR_UUID;
    static const char* CALIBRATION_STATUS_CHAR_UUID;
private:
    BLEServer* pServer;
    BLEService* pService;
    BLECharacteristic* pVoltageCharacteristic;
    BLECharacteristic* pCurrentCharacteristic;
    BLECharacteristic* pPowerCharacteristic;
    BLECharacteristic* pSocCharacteristic;
    BLECharacteristic* pCapacityCharacteristic;
    BLECharacteristic* pStarterVoltageCharacteristic;
    BLECharacteristic* pCalibrationStatusCharacteristic;
};

#endif // BLE_HANDLER_H
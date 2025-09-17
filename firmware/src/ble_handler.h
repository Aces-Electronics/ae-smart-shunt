#ifndef BLE_HANDLER_H
#define BLE_HANDLER_H

#include <NimBLEDevice.h>

#include <functional>

// Define a struct to hold all the telemetry data
struct Telemetry {
    float batteryVoltage;
    float batteryCurrent;
    float batteryPower;
    float batterySOC;
    float batteryCapacity;
    float starterBatteryVoltage;
    bool isCalibrated;
    int errorState;
    bool loadState;
    float cutoffVoltage;
    float reconnectVoltage;
};

class BLEHandler {
public:
    BLEHandler();
    void begin();
    void updateTelemetry(const Telemetry& telemetry);
    void setLoadSwitchCallback(std::function<void(bool)> callback);
    void setSOCCallback(std::function<void(float)> callback);
    void setVoltageProtectionCallback(std::function<void(String)> callback);

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
    static const char* ERROR_STATE_CHAR_UUID;
    static const char* LOAD_STATE_CHAR_UUID;
    static const char* LOAD_CONTROL_CHAR_UUID;
    static const char* SET_SOC_CHAR_UUID;
    static const char* SET_VOLTAGE_PROTECTION_CHAR_UUID;
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
    BLECharacteristic* pErrorStateCharacteristic;
    BLECharacteristic* pLoadStateCharacteristic;
    BLECharacteristic* pLoadControlCharacteristic;
    BLECharacteristic* pSetSocCharacteristic;
    BLECharacteristic* pSetVoltageProtectionCharacteristic;

    std::function<void(bool)> loadSwitchCallback;
    std::function<void(float)> socCallback;
    std::function<void(String)> voltageProtectionCallback;
};

#endif // BLE_HANDLER_H
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
    float lastHourWh;
    float lastDayWh;
    float lastWeekWh;
    uint32_t lowVoltageDelayS;
    String deviceNameSuffix;
};

class BLEHandler {
public:
    BLEHandler();
    void begin(const Telemetry& initial_telemetry);
    void updateTelemetry(const Telemetry& telemetry);
    void startAdvertising(const Telemetry& telemetry);
    void setLoadSwitchCallback(std::function<void(bool)> callback);
    void setSOCCallback(std::function<void(float)> callback);
    void setVoltageProtectionCallback(std::function<void(String)> callback);
    void setLowVoltageDelayCallback(std::function<void(uint32_t)> callback);
    void setDeviceNameSuffixCallback(std::function<void(String)> callback);
    void setWifiSsidCallback(std::function<void(String)> callback);
    void setWifiPassCallback(std::function<void(String)> callback);
    void setOtaTriggerCallback(std::function<void(bool)> callback);
    void updateFirmwareVersion(const String& version);
    void updateUpdateUrl(const String& url);
    void updateOtaStatus(const String& status);

public:
    // Service and Characteristic UUIDs
    // Using UUIDs from a random generator
    static const char* SERVICE_UUID;
    static const char* WIFI_SSID_CHAR_UUID;
    static const char* WIFI_PASS_CHAR_UUID;
    static const char* OTA_TRIGGER_CHAR_UUID;
    static const char* FIRMWARE_VERSION_CHAR_UUID;
    static const char* UPDATE_URL_CHAR_UUID;
    static const char* OTA_STATUS_CHAR_UUID;
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
    static const char* LAST_HOUR_WH_CHAR_UUID;
    static const char* LAST_DAY_WH_CHAR_UUID;
    static const char* LAST_WEEK_WH_CHAR_UUID;
    static const char* LOW_VOLTAGE_DELAY_CHAR_UUID;
    static const char* DEVICE_NAME_SUFFIX_CHAR_UUID;
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
    BLECharacteristic* pLastHourWhCharacteristic;
    BLECharacteristic* pLastDayWhCharacteristic;
    BLECharacteristic* pLastWeekWhCharacteristic;
    BLECharacteristic* pLowVoltageDelayCharacteristic;
    BLECharacteristic* pDeviceNameSuffixCharacteristic;
    BLECharacteristic* pWifiSsidCharacteristic;
    BLECharacteristic* pWifiPassCharacteristic;
    BLECharacteristic* pOtaTriggerCharacteristic;
    BLECharacteristic* pFirmwareVersionCharacteristic;
    BLECharacteristic* pUpdateUrlCharacteristic;
    BLECharacteristic* pOtaStatusCharacteristic;

    std::function<void(bool)> loadSwitchCallback;
    std::function<void(float)> socCallback;
    std::function<void(String)> voltageProtectionCallback;
    std::function<void(uint32_t)> lowVoltageDelayCallback;
    std::function<void(String)> deviceNameSuffixCallback;
    std::function<void(String)> wifiSsidCallback;
    std::function<void(String)> wifiPassCallback;
    std::function<void(bool)> otaTriggerCallback;
};

#endif // BLE_HANDLER_H
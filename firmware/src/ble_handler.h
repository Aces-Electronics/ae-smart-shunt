#ifndef BLE_HANDLER_H
#define BLE_HANDLER_H

#include <NimBLEDevice.h>
#include <vector>
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
    float eFuseLimit;
    uint16_t activeShuntRating;
    float ratedCapacity;
    String runFlatTime; // Added for sync
    String diagnostics; // Added for crash/uptime info
    String crashLog;    // Added for full backtrace
    // Temp Sensor
    float tempSensorTemperature;
    uint8_t tempSensorBatteryLevel;
    uint32_t tempSensorLastUpdate;
    // TPMS
    float tpmsPressurePsi[4];
    // Gauge
    uint32_t gaugeLastRx;
    bool gaugeLastTxSuccess;
};


class BLEHandler {
public:
    BLEHandler();
    void begin(const Telemetry& initial_telemetry);
    void updateTelemetry(const Telemetry& telemetry);
    void startAdvertising(const Telemetry& telemetry);
    bool isConnected(); // Check connection status
    void setServerCallbacks(BLEServerCallbacks* callbacks);
    void setLoadSwitchCallback(std::function<void(bool)> callback);
    void setSOCCallback(std::function<void(float)> callback);
    void setVoltageProtectionCallback(std::function<void(String)> callback);
    void setLowVoltageDelayCallback(std::function<void(uint32_t)> callback);
    void setDeviceNameSuffixCallback(std::function<void(String)> callback);
    void setRatedCapacityCallback(std::function<void(float)> callback);
    void setWifiSsidCallback(std::function<void(String)> callback);
    void setWifiPassCallback(std::function<void(String)> callback);
    void setOtaTriggerCallback(std::function<void(bool)> callback);
    void setOtaControlCallback(std::function<void(uint8_t)> callback);
    void updateFirmwareVersion(const String& version);
    void updateOtaStatus(uint8_t status);
    void updateReleaseMetadata(const String& metadata);
    void updateOtaProgress(uint8_t progress);
    void setPairingCallback(std::function<void(String)> callback);
    void setEfuseLimitCallback(std::function<void(float)> callback);

public:
    // Service and Characteristic UUIDs
    // Using UUIDs from a random generator
    static const char* SERVICE_UUID;
    static const char* CRASH_LOG_CHAR_UUID;
    static const char* TEMP_SENSOR_DATA_CHAR_UUID;
    static const char* WIFI_SSID_CHAR_UUID;
    static const char* WIFI_PASS_CHAR_UUID;
    static const char* FIRMWARE_VERSION_CHAR_UUID;
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
    static const char* SET_RATED_CAPACITY_CHAR_UUID;
    static const char* PAIRING_CHAR_UUID;
    static const char* EFUSE_LIMIT_CHAR_UUID;
    static const char* ACTIVE_SHUNT_CHAR_UUID;
    static const char* RUN_FLAT_TIME_CHAR_UUID;
    static const char* DIAGNOSTICS_CHAR_UUID; // New
    static const char* TPMS_DATA_CHAR_UUID;
    static const char* GAUGE_STATUS_CHAR_UUID;

    // --- New OTA Service ---
    static const char* OTA_SERVICE_UUID;
    static const char* OTA_UPDATE_STATUS_CHAR_UUID;
    static const char* OTA_UPDATE_CONTROL_CHAR_UUID;
    static const char* OTA_RELEASE_METADATA_CHAR_UUID;
    static const char* OTA_PROGRESS_CHAR_UUID;

private:
    BLEServer* pServer;
    BLEService* pService;
    BLECharacteristic* pEfuseLimitCharacteristic;
    BLECharacteristic* pActiveShuntCharacteristic;
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
    BLECharacteristic* pSetRatedCapacityCharacteristic;
    BLECharacteristic* pWifiSsidCharacteristic;
    BLECharacteristic* pWifiPassCharacteristic;
    BLECharacteristic* pFirmwareVersionCharacteristic;
    BLECharacteristic* pPairingCharacteristic;
    BLECharacteristic* pRunFlatTimeCharacteristic;
    BLECharacteristic* pDiagnosticsCharacteristic;
    BLECharacteristic* pCrashLogCharacteristic;
    BLECharacteristic* pTempSensorDataCharacteristic;
    BLECharacteristic* pTpmsDataCharacteristic;
    BLECharacteristic* pGaugeStatusCharacteristic;


    // --- New OTA service and characteristics ---
    BLEService* pOtaService;
    BLECharacteristic* pOtaUpdateStatusCharacteristic;
    BLECharacteristic* pOtaUpdateControlCharacteristic;
    BLECharacteristic* pOtaReleaseMetadataCharacteristic;
    BLECharacteristic* pOtaProgressCharacteristic;

    std::function<void(bool)> loadSwitchCallback;
    std::function<void(float)> socCallback;
    std::function<void(String)> voltageProtectionCallback;
    std::function<void(uint32_t)> lowVoltageDelayCallback;
    std::function<void(String)> deviceNameSuffixCallback;
    std::function<void(float)> ratedCapacityCallback;
    std::function<void(String)> wifiSsidCallback;
    std::function<void(String)> wifiPassCallback;
    std::function<void(bool)> otaTriggerCallback;
    std::function<void(uint8_t)> otaControlCallback;
    std::vector<uint8_t> _metadata_buffer;
    std::function<void(String)> pairingCallback;
    std::function<void(float)> efuseLimitCallback;
};

#endif // BLE_HANDLER_H
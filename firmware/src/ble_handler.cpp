#include "ble_handler.h"
#include <NimBLEDevice.h>
#include <WiFi.h>
#include "esp_mac.h"

// UUIDs generated from https://www.uuidgenerator.net/
const char* BLEHandler::SERVICE_UUID = "4fafc201-1fb5-459e-8fcc-c5c9c331914b";
const char* BLEHandler::WIFI_SSID_CHAR_UUID = "5A1B2C3D-4E5F-6A7B-8C9D-0E1F2A3B4C62";
const char* BLEHandler::WIFI_PASS_CHAR_UUID = "6A1B2C3D-4E5F-6A7B-8C9D-0E1F2A3B4C63";
const char* BLEHandler::FIRMWARE_VERSION_CHAR_UUID = "8A1B2C3D-4E5F-6A7B-8C9D-0E1F2A3B4C65";
const char* BLEHandler::VOLTAGE_CHAR_UUID = "beb5483e-36e1-4688-b7f5-ea07361b26a8";
const char* BLEHandler::CURRENT_CHAR_UUID = "a8b31859-676a-486c-94a2-8928b8e3a249";
const char* BLEHandler::POWER_CHAR_UUID = "465048d2-871d-4234-9e48-35d033a875a8";
const char* BLEHandler::SOC_CHAR_UUID = "7c6c3e2e-4171-4228-8e8e-8b6c3a3b341b";
const char* BLEHandler::CAPACITY_CHAR_UUID = "3c3e8e1a-8b8a-4b0e-8e8e-8b6c3a3b341b";
const char* BLEHandler::STARTER_VOLTAGE_CHAR_UUID = "5b2e3f40-8b8a-4b0e-8e8e-8b6c3a3b341b";
const char* BLEHandler::CALIBRATION_STATUS_CHAR_UUID = "9b1e3f40-8b8a-4b0e-8e8e-8b6c3a3b341b";
const char* BLEHandler::ERROR_STATE_CHAR_UUID = "a3b4c5d6-e7f8-9012-3456-789012345678";
const char* BLEHandler::LOAD_STATE_CHAR_UUID = "b4c5d6e7-f890-1234-5678-901234567890";
const char* BLEHandler::LOAD_CONTROL_CHAR_UUID = "c5d6e7f8-9012-3456-7890-123456789012";
const char* BLEHandler::SET_SOC_CHAR_UUID = "d6e7f890-1234-5678-9012-345678901234";
const char* BLEHandler::SET_VOLTAGE_PROTECTION_CHAR_UUID = "e7f89012-3456-7890-1234-567890123456";
const char* BLEHandler::LAST_HOUR_WH_CHAR_UUID = "0A1B2C3D-4E5F-6A7B-8C9D-0E1F2A3B4C5D";
const char* BLEHandler::LAST_DAY_WH_CHAR_UUID = "1A1B2C3D-4E5F-6A7B-8C9D-0E1F2A3B4C5E";
const char* BLEHandler::LAST_WEEK_WH_CHAR_UUID = "2A1B2C3D-4E5F-6A7B-8C9D-0E1F2A3B4C5F";
const char* BLEHandler::LOW_VOLTAGE_DELAY_CHAR_UUID = "3A1B2C3D-4E5F-6A7B-8C9D-0E1F2A3B4C60";
const char* BLEHandler::DEVICE_NAME_SUFFIX_CHAR_UUID = "4A1B2C3D-4E5F-6A7B-8C9D-0E1F2A3B4C61";
const char* BLEHandler::SET_RATED_CAPACITY_CHAR_UUID = "5A1B2C3D-4E5F-6A7B-8C9D-0E1F2A3B4C64";
const char* BLEHandler::PAIRING_CHAR_UUID = "ACDC1234-5678-90AB-CDEF-1234567890CB";
const char* BLEHandler::EFUSE_LIMIT_CHAR_UUID = "BB1B2C3D-4E5F-6A7B-8C9D-0E1F2A3B4C68";
const char* BLEHandler::ACTIVE_SHUNT_CHAR_UUID = "CB1B2C3D-4E5F-6A7B-8C9D-0E1F2A3B4C69";
const char* BLEHandler::RUN_FLAT_TIME_CHAR_UUID = "CC1B2C3D-4E5F-6A7B-8C9D-0E1F2A3B4C6A";
const char* BLEHandler::DIAGNOSTICS_CHAR_UUID     = "ACDC1234-5678-90AB-CDEF-1234567890CC"; // Next available
const char* BLEHandler::CRASH_LOG_CHAR_UUID       = "ACDC1234-5678-90AB-CDEF-1234567890CD"; // Detailed Log
const char* BLEHandler::TEMP_SENSOR_DATA_CHAR_UUID = "ACDC1234-5678-90AB-CDEF-1234567890CE"; // Relayed Temp Sensor
const char* BLEHandler::TPMS_DATA_CHAR_UUID        = "ACDC1234-5678-90AB-CDEF-1234567890CF"; // TPMS Pressures
const char* BLEHandler::TPMS_CONFIG_CHAR_UUID      = "ACDC1234-5678-90AB-CDEF-1234567890D1"; // TPMS Config Backup/Restore
const char* BLEHandler::GAUGE_STATUS_CHAR_UUID     = "ACDC1234-5678-90AB-CDEF-1234567890D0"; // Gauge Status

// --- New OTA Service UUIDs ---
const char* BLEHandler::OTA_SERVICE_UUID = "1a89b148-b4e8-43d7-952b-a0b4b01e43b3";
const char* BLEHandler::OTA_UPDATE_STATUS_CHAR_UUID = "2a89b148-b4e8-43d7-952b-a0b4b01e43b3";
const char* BLEHandler::OTA_UPDATE_CONTROL_CHAR_UUID = "3a89b148-b4e8-43d7-952b-a0b4b01e43b3";
const char* BLEHandler::OTA_RELEASE_METADATA_CHAR_UUID = "4a89b148-b4e8-43d7-952b-a0b4b01e43b3";
const char* BLEHandler::OTA_PROGRESS_CHAR_UUID = "5a89b148-b4e8-43d7-952b-a0b4b01e43b3";


void BLEHandler::setInitialWifiSsid(const String& ssid) {
    Serial.printf("[BLE] Setting Initial SSID: '%s'\n", ssid.c_str());
    if (pWifiSsidCharacteristic) {
        pWifiSsidCharacteristic->setValue(std::string(ssid.c_str()));
    }
}

void BLEHandler::setInitialMqttBroker(const String& broker) {
    if (pMqttBrokerCharacteristic) {
        // NimBLE setValue takes std::string or (uint8_t*, len) for arbitrary data.
        // It also has setValue(const std::string&).
        pMqttBrokerCharacteristic->setValue(std::string(broker.c_str())); 
    }
}

void BLEHandler::setInitialMqttUser(const String& user) {
    if (pMqttUserCharacteristic) {
        pMqttUserCharacteristic->setValue(std::string(user.c_str()));
    }
}
const char* BLEHandler::CLOUD_CONFIG_CHAR_UUID = "6a89b148-b4e8-43d7-952b-a0b4b01e43b3";
const char* BLEHandler::CLOUD_STATUS_CHAR_UUID = "7a89b148-b4e8-43d7-952b-a0b4b01e43b3";
const char* BLEHandler::MQTT_BROKER_CHAR_UUID = "8a89b148-b4e8-43d7-952b-a0b4b01e43b3";
const char* BLEHandler::MQTT_USER_CHAR_UUID = "9a89b148-b4e8-43d7-952b-a0b4b01e43b3";
const char* BLEHandler::MQTT_PASS_CHAR_UUID = "aa89b148-b4e8-43d7-952b-a0b4b01e43b3";


class BoolCharacteristicCallbacks : public BLECharacteristicCallbacks {
    std::function<void(bool)> _callback;
public:
    BoolCharacteristicCallbacks(std::function<void(bool)> callback) : _callback(callback) {}

    void onWrite(BLECharacteristic* pCharacteristic) {
        std::string value = pCharacteristic->getValue();
        if (value.length() > 0 && _callback) {
            _callback(value[0] != 0);
            pCharacteristic->notify();
        }
    }
};

class Uint8CharacteristicCallbacks : public BLECharacteristicCallbacks {
    std::function<void(uint8_t)> _callback;
public:
    Uint8CharacteristicCallbacks(std::function<void(uint8_t)> callback) : _callback(callback) {}

    void onWrite(BLECharacteristic* pCharacteristic) {
        std::string value = pCharacteristic->getValue();
        if (value.length() > 0 && _callback) {
            _callback(value[0]);
            pCharacteristic->notify();
        }
    }
};

class Uint32CharacteristicCallbacks : public BLECharacteristicCallbacks {
    std::function<void(uint32_t)> _callback;
public:
    Uint32CharacteristicCallbacks(std::function<void(uint32_t)> callback) : _callback(callback) {}

    void onWrite(BLECharacteristic* pCharacteristic) {
        std::string value = pCharacteristic->getValue();
        if (value.length() == sizeof(uint32_t) && _callback) {
            uint32_t val;
            memcpy(&val, value.data(), sizeof(val));
            _callback(val);
            pCharacteristic->notify();
        }
    }
};

class ServerCallbacks: public BLEServerCallbacks {
    BLEHandler* pHandler;
public:
    ServerCallbacks(BLEHandler* handler) : pHandler(handler) {}

    void onConnect(BLEServer* pServer, ble_gap_conn_desc* desc) {
      Serial.printf("BLE client connected (ID: %d). Scheduling Params Update (Delayed)...\n", desc->conn_handle);
      if(pHandler) pHandler->scheduleConnParamsUpdate(desc->conn_handle);
    }

    void onDisconnect(BLEServer* pServer) {
        Serial.println("BLE client disconnected");
        if (pHandler) {
             // Cancel any pending update
             pHandler->scheduleConnParamsUpdate(0);
        }
    }

    void onMtuChanged(uint16_t MTU, ble_gap_conn_desc* desc) {
        Serial.printf("MTU changed to: %d\n", MTU);
    }
};

class FloatCharacteristicCallbacks : public BLECharacteristicCallbacks {
    std::function<void(float)> _callback;
public:
    FloatCharacteristicCallbacks(std::function<void(float)> callback) : _callback(callback) {}

    void onWrite(BLECharacteristic* pCharacteristic) {
        std::string value = pCharacteristic->getValue();
        if (value.length() == sizeof(float) && _callback) {
            float float_val;
            memcpy(&float_val, value.data(), sizeof(float));

            Serial.printf("BLE float write received. Bytes: ");
            for(int i=0; i<value.length(); i++) {
                Serial.printf("%02X ", (uint8_t)value[i]);
            }
            Serial.printf(" | Converted to float: %f\n", float_val);

            _callback(float_val);
            
            // Notify immediately so the app sees the new value
            pCharacteristic->notify();
        }
    }
};

class StringCharacteristicCallbacks : public BLECharacteristicCallbacks {
    std::function<void(String)> _callback;
public:
    StringCharacteristicCallbacks(std::function<void(String)> callback) : _callback(callback) {}

    void onWrite(BLECharacteristic* pCharacteristic) {
        std::string value = pCharacteristic->getValue();
        if (value.length() > 0 && _callback) {
            _callback(String(value.c_str()));
            pCharacteristic->notify();
        }
    }
};

class ByteVectorCharacteristicCallbacks : public BLECharacteristicCallbacks {
    std::function<void(std::vector<uint8_t>)> _callback;
public:
    ByteVectorCharacteristicCallbacks(std::function<void(std::vector<uint8_t>)> callback) : _callback(callback) {}

    void onWrite(BLECharacteristic* pCharacteristic) {
        std::string value = pCharacteristic->getValue();
        if (value.length() > 0 && _callback) {
            std::vector<uint8_t> data(value.begin(), value.end());
            _callback(data);
            pCharacteristic->notify();
        }
    }
};

BLEHandler::BLEHandler() : pServer(NULL), pService(NULL) {
    _pendingConnHandle = 0;
    _connTime = 0;
}

void BLEHandler::setServerCallbacks(BLEServerCallbacks* callbacks) {
    if (pServer) {
        pServer->setCallbacks(callbacks);
    }
}

void BLEHandler::setLoadSwitchCallback(std::function<void(bool)> callback) {
    this->loadSwitchCallback = callback;
}

void BLEHandler::setSOCCallback(std::function<void(float)> callback) {
    this->socCallback = callback;
}

void BLEHandler::setVoltageProtectionCallback(std::function<void(String)> callback) {
    this->voltageProtectionCallback = callback;
}

void BLEHandler::setLowVoltageDelayCallback(std::function<void(uint32_t)> callback) {
    this->lowVoltageDelayCallback = callback;
}

void BLEHandler::setDeviceNameSuffixCallback(std::function<void(String)> callback) {
    this->deviceNameSuffixCallback = callback;
}

void BLEHandler::setRatedCapacityCallback(std::function<void(float)> callback) {
    this->ratedCapacityCallback = callback;
}

void BLEHandler::setWifiSsidCallback(std::function<void(String)> callback) {
    this->wifiSsidCallback = callback;
}

void BLEHandler::setWifiPassCallback(std::function<void(String)> callback) {
    this->wifiPassCallback = callback;
}

void BLEHandler::setOtaTriggerCallback(std::function<void(bool)> callback) {
    this->otaTriggerCallback = callback;
}

void BLEHandler::setOtaControlCallback(std::function<void(uint8_t)> callback) {
    this->otaControlCallback = callback;
}

void BLEHandler::setPairingCallback(std::function<void(String)> callback) {
    this->pairingCallback = callback;
}

void BLEHandler::setEfuseLimitCallback(std::function<void(float)> callback) {
    this->efuseLimitCallback = callback;
}

void BLEHandler::setTpmsConfigCallback(std::function<void(std::vector<uint8_t>)> callback) {
    this->tpmsConfigCallback = callback;
}

void BLEHandler::setCloudConfigCallback(std::function<void(bool)> callback) {
    this->cloudConfigCallback = callback;
}

void BLEHandler::setMqttBrokerCallback(std::function<void(String)> callback) {
    this->mqttBrokerCallback = callback;
}

void BLEHandler::setMqttAuthCallback(std::function<void(String, String)> callback) {
    this->mqttAuthCallback = callback;
}

// Helper to generate PIN from MAC
uint32_t generatePinFromMac() {
    uint8_t mac[6];
    // Use esp_read_mac to get base MAC even if WiFi is off
    esp_read_mac(mac, ESP_MAC_WIFI_STA); 
    
    // Log the MAC used for PIN generation
    Serial.printf("[BLE SEC] MAC for PIN: %02X:%02X:%02X:%02X:%02X:%02X\n", 
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    // Use last 3 bytes to generate a unique 6-digit PIN
    uint32_t val = (mac[3] << 16) | (mac[4] << 8) | mac[5];
    uint32_t pin = val % 1000000;
    Serial.printf("[BLE SEC] PIN Code: %06d\n", pin);
    return pin;
}

void BLEHandler::updateFirmwareVersion(const String& version) {
    if (pFirmwareVersionCharacteristic) {
        pFirmwareVersionCharacteristic->setValue(version);
        // pFirmwareVersionCharacteristic->notify();
    }
}

void BLEHandler::updateOtaStatus(uint8_t status) {
    if (pOtaUpdateStatusCharacteristic) {
        pOtaUpdateStatusCharacteristic->setValue(status);
        pOtaUpdateStatusCharacteristic->notify();
    }
}

void BLEHandler::updateReleaseMetadata(const String& metadata) {
    if (pOtaReleaseMetadataCharacteristic) {
        Serial.printf("[%lu] [BLE_HANDLER] Metadata to be set (length %d): %s\n", millis(), metadata.length(), metadata.c_str());

        // Use the persistent buffer to store the metadata
        _metadata_buffer.assign(metadata.c_str(), metadata.c_str() + metadata.length());

        // Log the buffer size before setting
        Serial.printf("[%lu] [BLE_HANDLER] Persistent buffer size: %d\n", millis(), _metadata_buffer.size());

        // Set the characteristic value using the persistent buffer's data
        pOtaReleaseMetadataCharacteristic->setValue(_metadata_buffer.data(), _metadata_buffer.size());
        Serial.printf("[%lu] [BLE_HANDLER] setValue() called with persistent buffer\n", millis());

        // Notify the client that the characteristic has been updated
        pOtaReleaseMetadataCharacteristic->notify();
    }
}

void BLEHandler::updateOtaProgress(uint8_t progress) {
    if (pOtaProgressCharacteristic) {
        pOtaProgressCharacteristic->setValue(progress);
        pOtaProgressCharacteristic->notify();
    }
}

void BLEHandler::updateCloudStatus(uint8_t status, uint32_t lastSuccessTime) {
    if (pCloudStatusCharacteristic) {
        uint8_t buf[5];
        buf[0] = status;
        memcpy(&buf[1], &lastSuccessTime, 4);
        pCloudStatusCharacteristic->setValue(buf, 5);
        pCloudStatusCharacteristic->notify();
    }
}

void BLEHandler::begin(const Telemetry& initial_telemetry) {
    // BLEDevice::init and setMTU moved to main.cpp (centralized)
    
    // Security & Speed Configuration
    // Security & Speed Configuration
    // uint32_t passkey = generatePinFromMac();
    // BLEDevice::setSecurityAuth(true, true, true); 
    // BLEDevice::setSecurityPasskey(passkey);
    // BLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_ONLY);

    pServer = BLEDevice::createServer();
    pServer->setCallbacks(new ServerCallbacks(this));
    pService = pServer->createService(SERVICE_UUID);

    // Create characteristics
    pVoltageCharacteristic = pService->createCharacteristic(
        VOLTAGE_CHAR_UUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
    );
    pCurrentCharacteristic = pService->createCharacteristic(
        CURRENT_CHAR_UUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
    );
    pPowerCharacteristic = pService->createCharacteristic(
        POWER_CHAR_UUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
    );
    pSocCharacteristic = pService->createCharacteristic(
        SOC_CHAR_UUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
    );
    pCapacityCharacteristic = pService->createCharacteristic(
        CAPACITY_CHAR_UUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
    );
    pStarterVoltageCharacteristic = pService->createCharacteristic(
        STARTER_VOLTAGE_CHAR_UUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
    );
    pCalibrationStatusCharacteristic = pService->createCharacteristic(
        CALIBRATION_STATUS_CHAR_UUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
    );

    pErrorStateCharacteristic = pService->createCharacteristic(
        ERROR_STATE_CHAR_UUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
    );

    pLoadStateCharacteristic = pService->createCharacteristic(
        LOAD_STATE_CHAR_UUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
    );

    pLoadControlCharacteristic = pService->createCharacteristic(
        LOAD_CONTROL_CHAR_UUID,
        NIMBLE_PROPERTY::WRITE
    );
    pLoadControlCharacteristic->setCallbacks(new BoolCharacteristicCallbacks(this->loadSwitchCallback));

    pSetSocCharacteristic = pService->createCharacteristic(
        SET_SOC_CHAR_UUID,
        NIMBLE_PROPERTY::WRITE
    );
    pSetSocCharacteristic->setCallbacks(new FloatCharacteristicCallbacks(this->socCallback));

    pSetVoltageProtectionCharacteristic = pService->createCharacteristic(
        SET_VOLTAGE_PROTECTION_CHAR_UUID,
        NIMBLE_PROPERTY::WRITE | 
        NIMBLE_PROPERTY::READ | 
        NIMBLE_PROPERTY::NOTIFY
    );
    pSetVoltageProtectionCharacteristic->setCallbacks(new StringCharacteristicCallbacks(this->voltageProtectionCallback));

    pLastHourWhCharacteristic = pService->createCharacteristic(
        LAST_HOUR_WH_CHAR_UUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
    );
    pLastDayWhCharacteristic = pService->createCharacteristic(
        LAST_DAY_WH_CHAR_UUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
    );
    pLastWeekWhCharacteristic = pService->createCharacteristic(
        LAST_WEEK_WH_CHAR_UUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
    );

    pLowVoltageDelayCharacteristic = pService->createCharacteristic(
        LOW_VOLTAGE_DELAY_CHAR_UUID,
        NIMBLE_PROPERTY::READ | 
        NIMBLE_PROPERTY::WRITE | 
        NIMBLE_PROPERTY::NOTIFY
    );
    pLowVoltageDelayCharacteristic->setCallbacks(new Uint32CharacteristicCallbacks(this->lowVoltageDelayCallback));

    pDeviceNameSuffixCharacteristic = pService->createCharacteristic(
        DEVICE_NAME_SUFFIX_CHAR_UUID,
        NIMBLE_PROPERTY::READ | 
        NIMBLE_PROPERTY::WRITE
    );
    pDeviceNameSuffixCharacteristic->setCallbacks(new StringCharacteristicCallbacks(this->deviceNameSuffixCallback));

    pSetRatedCapacityCharacteristic = pService->createCharacteristic(
        SET_RATED_CAPACITY_CHAR_UUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::NOTIFY
    );
    pSetRatedCapacityCharacteristic->setCallbacks(new FloatCharacteristicCallbacks(this->ratedCapacityCallback));

    pWifiSsidCharacteristic = pService->createCharacteristic(
        WIFI_SSID_CHAR_UUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE
    );
    pWifiSsidCharacteristic->setCallbacks(new StringCharacteristicCallbacks(this->wifiSsidCallback));

    pWifiPassCharacteristic = pService->createCharacteristic(
        WIFI_PASS_CHAR_UUID,
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_ENC
    );
    pWifiPassCharacteristic->setCallbacks(new StringCharacteristicCallbacks(this->wifiPassCallback));


    pFirmwareVersionCharacteristic = pService->createCharacteristic(
        FIRMWARE_VERSION_CHAR_UUID,
        NIMBLE_PROPERTY::READ
    );


    pPairingCharacteristic = pService->createCharacteristic(
        PAIRING_CHAR_UUID,
        NIMBLE_PROPERTY::WRITE | 
        NIMBLE_PROPERTY::READ
    );
    pPairingCharacteristic->setCallbacks(new StringCharacteristicCallbacks(this->pairingCallback));
    
    // E-Fuse Limit Characteristic
    pEfuseLimitCharacteristic = pService->createCharacteristic(
        EFUSE_LIMIT_CHAR_UUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::READ_ENC | 
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_ENC
    );
    pEfuseLimitCharacteristic->setCallbacks(new FloatCharacteristicCallbacks(this->efuseLimitCallback));

    // Active Shunt Rating Characteristic (Read-only)
    pActiveShuntCharacteristic = pService->createCharacteristic(
        ACTIVE_SHUNT_CHAR_UUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
    );

    // Set the value to the Wi-Fi MAC address (for verification by the App)
    String macAddr = WiFi.macAddress();
    Serial.printf("BLE: Pairing Char UUID: %s\n", PAIRING_CHAR_UUID);
    Serial.printf("Setting Pairing Characteristic Value to: %s\n", macAddr.c_str());
    pPairingCharacteristic->setValue(std::string(macAddr.c_str()));

    // Run Flat Time Characteristic (Read-only string)
    pRunFlatTimeCharacteristic = pService->createCharacteristic(
        RUN_FLAT_TIME_CHAR_UUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
    );
    pRunFlatTimeCharacteristic->setValue("--");  // Initial value before first telemetry

    pDiagnosticsCharacteristic = pService->createCharacteristic(
        DIAGNOSTICS_CHAR_UUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
    );
    pDiagnosticsCharacteristic->setValue("Initializing...");

    pCrashLogCharacteristic = pService->createCharacteristic(
        CRASH_LOG_CHAR_UUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
    );
    pCrashLogCharacteristic->setValue(std::string(initial_telemetry.crashLog.c_str()));

    // Temp Sensor Data Relay
    pTempSensorDataCharacteristic = pService->createCharacteristic(
        TEMP_SENSOR_DATA_CHAR_UUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
    );
    // Initial value: 0.0, 0%
    uint8_t initTempData[5] = {0}; // float(0.0) + uint8(0)
    pTempSensorDataCharacteristic->setValue(initTempData, 5);

    // TPMS Data (4 * float = 16 bytes)
    pTpmsDataCharacteristic = pService->createCharacteristic(
        TPMS_DATA_CHAR_UUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
    );
    float initTpms[4] = {0,0,0,0};
    pTpmsDataCharacteristic->setValue((uint8_t*)initTpms, 16);

    // TPMS Config (Backup/Restore - 48 bytes)
    pTpmsConfigCharacteristic = pService->createCharacteristic(
        TPMS_CONFIG_CHAR_UUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE
    );
    pTpmsConfigCharacteristic->setCallbacks(new ByteVectorCharacteristicCallbacks(this->tpmsConfigCallback));
    uint8_t initTpmsConfig[48] = {0};
    pTpmsConfigCharacteristic->setValue(initTpmsConfig, 48);

    // Gauge Status (uint32_t lastRx + bool lastTxSuccess = 5 bytes)
    pGaugeStatusCharacteristic = pService->createCharacteristic(
        GAUGE_STATUS_CHAR_UUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
    );
    uint8_t initGaugeStatus[5] = {0};
    pGaugeStatusCharacteristic->setValue(initGaugeStatus, 5);


    // Cloud Config
    pCloudConfigCharacteristic = pService->createCharacteristic(
        CLOUD_CONFIG_CHAR_UUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE
    );
    pCloudConfigCharacteristic->setCallbacks(new BoolCharacteristicCallbacks([this](bool val){
        if(cloudConfigCallback) cloudConfigCallback(val);
    }));

    // Cloud Status
    pCloudStatusCharacteristic = pService->createCharacteristic(
        CLOUD_STATUS_CHAR_UUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
    );
    uint8_t initialStatus[5] = {0};
    pCloudStatusCharacteristic->setValue(initialStatus, 5);
    
    // MQTT Broker
    pMqttBrokerCharacteristic = pService->createCharacteristic(
        MQTT_BROKER_CHAR_UUID,
         NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE
    );
    pMqttBrokerCharacteristic->setCallbacks(new ByteVectorCharacteristicCallbacks([this](std::vector<uint8_t> data){
        std::string s(data.begin(), data.end());
        String broker = String(s.c_str());
        Serial.printf("[BLE] MQTT Broker Set: %s\n", broker.c_str());
        if(mqttBrokerCallback) mqttBrokerCallback(broker);
    }));

    // MQTT User
    pMqttUserCharacteristic = pService->createCharacteristic(
        MQTT_USER_CHAR_UUID,
         NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE
    );
    pMqttUserCharacteristic->setCallbacks(new ByteVectorCharacteristicCallbacks([this](std::vector<uint8_t> data){
        std::string s(data.begin(), data.end());
        _pendingMqttUser = String(s.c_str());
        // Wait for Password? Or callback now if pass exists?
        // Let's assume we update ONE by ONE. But handler expects both.
        // We will call callback if BOTH are set? Or just whenever one changes, we need the other.
        // Issue: user sets user, then pass. Callback called twice?
        // Better: We only call callback if valid?
        // Actually, we need to read the OTHER one if not in memory.
        // But BLE Handler logic restarts.
        // Let's simplify: pass both to callback?
        // If one is empty, we might overwrite with empty.
        // Let's just store in MEMBER variables and invoke callback.
        if (mqttAuthCallback && _pendingMqttUser.length() > 0 && _pendingMqttPass.length() > 0) {
             mqttAuthCallback(_pendingMqttUser, _pendingMqttPass);
        }
    }));

     // MQTT Pass
    pMqttPassCharacteristic = pService->createCharacteristic(
        MQTT_PASS_CHAR_UUID,
         NIMBLE_PROPERTY::WRITE // Write only
    );
    pMqttPassCharacteristic->setCallbacks(new ByteVectorCharacteristicCallbacks([this](std::vector<uint8_t> data){
        std::string s(data.begin(), data.end());
        _pendingMqttPass = String(s.c_str());
        if (mqttAuthCallback && _pendingMqttUser.length() > 0 && _pendingMqttPass.length() > 0) {
             mqttAuthCallback(_pendingMqttUser, _pendingMqttPass);
        }
    }));

    pService->start();

    // --- Create New OTA Service ---
    pOtaService = pServer->createService(OTA_SERVICE_UUID);

    pOtaUpdateStatusCharacteristic = pOtaService->createCharacteristic(
        OTA_UPDATE_STATUS_CHAR_UUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
    );

    pOtaUpdateControlCharacteristic = pOtaService->createCharacteristic(
        OTA_UPDATE_CONTROL_CHAR_UUID,
        NIMBLE_PROPERTY::WRITE
    );
    pOtaUpdateControlCharacteristic->setCallbacks(new Uint8CharacteristicCallbacks(this->otaControlCallback));

    pOtaReleaseMetadataCharacteristic = pOtaService->createCharacteristic(
        OTA_RELEASE_METADATA_CHAR_UUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY,
        1024 // Max size for release notes JSON
    );

    pOtaProgressCharacteristic = pOtaService->createCharacteristic(
        OTA_PROGRESS_CHAR_UUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
    );

    pOtaService->start();


    startAdvertising(initial_telemetry);
}

void BLEHandler::updateTelemetry(const Telemetry& telemetry) {
    pVoltageCharacteristic->setValue(telemetry.batteryVoltage);
    pVoltageCharacteristic->notify();

    pCurrentCharacteristic->setValue(telemetry.batteryCurrent);
    pCurrentCharacteristic->notify();

    pPowerCharacteristic->setValue(telemetry.batteryPower);
    pPowerCharacteristic->notify();

    pSocCharacteristic->setValue(telemetry.batterySOC);
    pSocCharacteristic->notify();

    pCapacityCharacteristic->setValue(telemetry.batteryCapacity);
    pCapacityCharacteristic->notify();

    pStarterVoltageCharacteristic->setValue(telemetry.starterBatteryVoltage);
    pStarterVoltageCharacteristic->notify();

    pCalibrationStatusCharacteristic->setValue(telemetry.isCalibrated);
    pCalibrationStatusCharacteristic->notify();

    pErrorStateCharacteristic->setValue(telemetry.errorState);
    pErrorStateCharacteristic->notify();

    pLoadStateCharacteristic->setValue(telemetry.loadState);
    pLoadStateCharacteristic->notify();

    char voltage_buf[20];
    snprintf(voltage_buf, sizeof(voltage_buf), "%.2f,%.2f", telemetry.cutoffVoltage, telemetry.reconnectVoltage);
    pSetVoltageProtectionCharacteristic->setValue(voltage_buf);
    pSetVoltageProtectionCharacteristic->notify();

    pLastHourWhCharacteristic->setValue(telemetry.lastHourWh);
    pLastHourWhCharacteristic->notify();
    pLastDayWhCharacteristic->setValue(telemetry.lastDayWh);
    pLastDayWhCharacteristic->notify();
    pLastWeekWhCharacteristic->setValue(telemetry.lastWeekWh);
    pLastWeekWhCharacteristic->notify();

    pLowVoltageDelayCharacteristic->setValue(telemetry.lowVoltageDelayS);
    pLowVoltageDelayCharacteristic->notify();

    pDeviceNameSuffixCharacteristic->setValue(telemetry.deviceNameSuffix);
    pDeviceNameSuffixCharacteristic->notify();

    pEfuseLimitCharacteristic->setValue(telemetry.eFuseLimit);
    pEfuseLimitCharacteristic->notify();

    pActiveShuntCharacteristic->setValue(telemetry.activeShuntRating);
    pActiveShuntCharacteristic->notify();

    pSetRatedCapacityCharacteristic->setValue(telemetry.ratedCapacity);
    pSetRatedCapacityCharacteristic->notify();

    // Update Run Flat Time string
    pRunFlatTimeCharacteristic->setValue(std::string(telemetry.runFlatTime.c_str()));
    pRunFlatTimeCharacteristic->notify();

    // Update Diagnostics
    pDiagnosticsCharacteristic->setValue(std::string(telemetry.diagnostics.c_str()));
    pDiagnosticsCharacteristic->notify();

    // Update Temp Sensor (Float + Uint8 + Uint32 + Uint32 = 13 bytes)
    uint8_t tempBuf[13];
    memcpy(&tempBuf[0], &telemetry.tempSensorTemperature, 4);
    tempBuf[4] = telemetry.tempSensorBatteryLevel;
    memcpy(&tempBuf[5], &telemetry.tempSensorLastUpdate, 4);
    memcpy(&tempBuf[9], &telemetry.tempSensorUpdateInterval, 4);
    pTempSensorDataCharacteristic->setValue(tempBuf, 13);
    pTempSensorDataCharacteristic->notify();

    pTpmsDataCharacteristic->setValue((uint8_t*)telemetry.tpmsPressurePsi, 16);
    pTpmsDataCharacteristic->notify();

    // Update TPMS Config Backup (48 bytes)
    pTpmsConfigCharacteristic->setValue(const_cast<uint8_t*>(telemetry.tpmsConfig), 48);
    // No notify needed for config unless changed, but read is primary

    // Update Gauge Status (uint32_t lastRx + bool lastTxSuccess = 5 bytes)
    uint8_t gaugeBuf[5];
    memcpy(&gaugeBuf[0], &telemetry.gaugeLastRx, 4);
    gaugeBuf[4] = telemetry.gaugeLastTxSuccess ? 1 : 0;
    pGaugeStatusCharacteristic->setValue(gaugeBuf, 5);
    pGaugeStatusCharacteristic->notify();

    // Conditional advertising restart
    bool dataChanged = (fabsf(telemetry.batteryVoltage - lastAdvVoltage) > 0.05f) || 
                       (telemetry.errorState != lastAdvErrorState) || 
                       (telemetry.loadState != lastAdvLoadState);
    
    unsigned long now = millis();
    if (dataChanged || (now - lastAdvUpdateTime > 60000) || lastAdvUpdateTime == 0) {
        lastAdvVoltage = telemetry.batteryVoltage;
        lastAdvErrorState = telemetry.errorState;
        lastAdvLoadState = telemetry.loadState;
        lastAdvUpdateTime = now;
        
        startAdvertising(telemetry);
    }
}

bool BLEHandler::isConnected() {
    return pServer->getConnectedCount() > 0;
}

void BLEHandler::startAdvertising(const Telemetry& telemetry) {
    BLEAdvertising* pAdvertising = BLEDevice::getAdvertising();

    // Stop advertising to update the data
    pAdvertising->stop();

    BLEAdvertisementData oAdvertisementData = BLEAdvertisementData();
    oAdvertisementData.setFlags(0x06); // BR_EDR_NOT_SUPPORTED | GENERAL_DISC_MODE

    std::string manuf_data;
    // Add a company ID (e.g., 0x02E5 for Espressif)
    uint16_t company_id = 0x02E5;
    manuf_data += (char)(company_id & 0xFF);
    manuf_data += (char)((company_id >> 8) & 0xFF);

    // Add telemetry data
    // Pack voltage as a 16-bit integer (e.g., in mV) to save space
    uint16_t voltage_mv = (uint16_t)(telemetry.batteryVoltage * 1000);
    manuf_data.append((char*)&voltage_mv, sizeof(voltage_mv));

    uint8_t error_state = (uint8_t)telemetry.errorState;
    manuf_data.append((char*)&error_state, sizeof(error_state));

    uint8_t load_state = (uint8_t)telemetry.loadState;
    manuf_data.append((char*)&load_state, sizeof(load_state));

    oAdvertisementData.setManufacturerData(manuf_data);
    pAdvertising->setAdvertisementData(oAdvertisementData);

    // Also set the scan response data
    BLEAdvertisementData oScanResponseData = BLEAdvertisementData();
    String deviceName = "AE Smart Shunt";
    if (telemetry.deviceNameSuffix.length() > 0) {
        deviceName += " - " + telemetry.deviceNameSuffix;
    }
    oScanResponseData.setName(deviceName.c_str());
    pAdvertising->setScanResponseData(oScanResponseData);

    pAdvertising->addServiceUUID(SERVICE_UUID);
    pAdvertising->setScanResponse(true);

    // Speed Optimization: Request preferred connection params
    // Min Interval: 6 * 1.25ms = 7.5ms
    // Max Interval: 12 * 1.25ms = 15ms
    // Latency: 0
    // Timeout: 100 * 10ms = 1000ms
    pAdvertising->setMinPreferred(0x06);
    pAdvertising->setMaxPreferred(0x0C); 

    pAdvertising->start();
}

void BLEHandler::scheduleConnParamsUpdate(uint16_t connHandle) {
    if (connHandle == 0) {
        _pendingConnHandle = 0;
        _connTime = 0;
    } else {
        _pendingConnHandle = connHandle;
        _connTime = millis();
    }
}

void BLEHandler::loop() {
    if (_pendingConnHandle != 0 && _connTime != 0) {
        if (millis() - _connTime > 2000) { // 2 Seconds Delay
            Serial.printf("[BLE] Updating Conn Params for Handle %d (Delayed)\n", _pendingConnHandle);
             // Min 24(30ms), Max 40(50ms), Latency 4, Timeout 300(3000ms)
             // Using start on pServer
             if (pServer) {
                 pServer->updateConnParams(_pendingConnHandle, 24, 40, 4, 300);
             }
             _pendingConnHandle = 0; // Done
             _connTime = 0;
        }
    }
}

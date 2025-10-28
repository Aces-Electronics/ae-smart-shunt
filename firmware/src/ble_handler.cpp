#include "ble_handler.h"
#include <NimBLEDevice.h>

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

// --- New OTA Service UUIDs ---
const char* BLEHandler::OTA_SERVICE_UUID = "1a89b148-b4e8-43d7-952b-a0b4b01e43b3";
const char* BLEHandler::OTA_UPDATE_STATUS_CHAR_UUID = "2a89b148-b4e8-43d7-952b-a0b4b01e43b3";
const char* BLEHandler::OTA_UPDATE_CONTROL_CHAR_UUID = "3a89b148-b4e8-43d7-952b-a0b4b01e43b3";
const char* BLEHandler::OTA_RELEASE_METADATA_CHAR_UUID = "4a89b148-b4e8-43d7-952b-a0b4b01e43b3";
const char* BLEHandler::OTA_PROGRESS_CHAR_UUID = "5a89b148-b4e8-43d7-952b-a0b4b01e43b3";


class BoolCharacteristicCallbacks : public BLECharacteristicCallbacks {
    std::function<void(bool)> _callback;
public:
    BoolCharacteristicCallbacks(std::function<void(bool)> callback) : _callback(callback) {}

    void onWrite(BLECharacteristic* pCharacteristic) {
        std::string value = pCharacteristic->getValue();
        if (value.length() > 0 && _callback) {
            _callback(value[0] != 0);
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
        }
    }
};

class ServerCallbacks: public BLEServerCallbacks {
    void onConnect(BLEServer* pServer) {
      Serial.println("BLE client connected");
    }

    void onDisconnect(BLEServer* pServer) {
      Serial.println("BLE client disconnected");
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
        }
    }
};

BLEHandler::BLEHandler() : pServer(NULL), pService(NULL) {}

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

void BLEHandler::updateFirmwareVersion(const String& version) {
    if (pFirmwareVersionCharacteristic) {
        pFirmwareVersionCharacteristic->setValue(version);
        pFirmwareVersionCharacteristic->notify();
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

void BLEHandler::begin(const Telemetry& initial_telemetry) {
    BLEDevice::init("AE Smart Shunt");
    BLEDevice::setMTU(517);
    pServer = BLEDevice::createServer();
    pServer->setCallbacks(new ServerCallbacks());
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
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
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
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::NOTIFY
    );
    pLowVoltageDelayCharacteristic->setCallbacks(new Uint32CharacteristicCallbacks(this->lowVoltageDelayCallback));

    pDeviceNameSuffixCharacteristic = pService->createCharacteristic(
        DEVICE_NAME_SUFFIX_CHAR_UUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::NOTIFY
    );
    pDeviceNameSuffixCharacteristic->setCallbacks(new StringCharacteristicCallbacks(this->deviceNameSuffixCallback));

    pWifiSsidCharacteristic = pService->createCharacteristic(
        WIFI_SSID_CHAR_UUID,
        NIMBLE_PROPERTY::WRITE
    );
    pWifiSsidCharacteristic->setCallbacks(new StringCharacteristicCallbacks(this->wifiSsidCallback));

    pWifiPassCharacteristic = pService->createCharacteristic(
        WIFI_PASS_CHAR_UUID,
        NIMBLE_PROPERTY::WRITE
    );
    pWifiPassCharacteristic->setCallbacks(new StringCharacteristicCallbacks(this->wifiPassCallback));

    pFirmwareVersionCharacteristic = pService->createCharacteristic(
        FIRMWARE_VERSION_CHAR_UUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
    );

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

    // Update advertising data
    startAdvertising(telemetry);
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

    // Restore connection interval hints
    pAdvertising->setMinPreferred(0x06);
    pAdvertising->setMinPreferred(0x12);

    pAdvertising->start();
}

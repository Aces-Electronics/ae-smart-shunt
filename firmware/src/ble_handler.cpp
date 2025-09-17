#include "ble_handler.h"
#include <NimBLEDevice.h>

// UUIDs generated from https://www.uuidgenerator.net/
const char* BLEHandler::SERVICE_UUID = "4fafc201-1fb5-459e-8fcc-c5c9c331914b";
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

class LoadControlCallbacks : public BLECharacteristicCallbacks {
    std::function<void(bool)> _callback;
public:
    LoadControlCallbacks(std::function<void(bool)> callback) : _callback(callback) {}

    void onWrite(BLECharacteristic* pCharacteristic) {
        std::string value = pCharacteristic->getValue();
        if (value.length() > 0 && _callback) {
            _callback(value[0] != 0);
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

void BLEHandler::setLoadSwitchCallback(std::function<void(bool)> callback) {
    this->loadSwitchCallback = callback;
}

void BLEHandler::setSOCCallback(std::function<void(float)> callback) {
    this->socCallback = callback;
}

void BLEHandler::setVoltageProtectionCallback(std::function<void(String)> callback) {
    this->voltageProtectionCallback = callback;
}

void BLEHandler::begin() {
    BLEDevice::init("AE Smart Shunt");
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
    pLoadControlCharacteristic->setCallbacks(new LoadControlCallbacks(this->loadSwitchCallback));

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

    pService->start();

    BLEAdvertising* pAdvertising = BLEDevice::getAdvertising();
    pAdvertising->addServiceUUID(SERVICE_UUID);
    pAdvertising->setScanResponse(true);
    pAdvertising->setMinPreferred(0x06);
    pAdvertising->setMinPreferred(0x12);
    BLEDevice::startAdvertising();
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
}

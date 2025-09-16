#include "ble_handler.h"
#include <NimBLEDevice.h>

// UUIDs generated from https://www.uuidgenerator.net/
const char* BLEHandler::SERVICE_UUID = "4fafc201-1fb5-459e-8fcc-c5c9c331914b";
const char* BLEHandler::VOLTAGE_CHAR_UUID = "beb5483e-36e1-4688-b7f5-ea07361b26a8";
const char* BLEHandler::CURRENT_CHAR_UUID = "a8b31859-676a-486c-94a2-8928b8e3a249";
const char* BLEHandler::POWER_CHAR_UUID = "465048d2-871d-4234-9e48-35d033a875a8";
const char* BLEHandler::SOC_CHAR_UUID = "7c6c3e2e-4171-4228-8e8e-8b6c3a3b341b";
const char* BLEHandler::CAPACITY_CHAR_UUID = "3c3e8e1a-8b8a-4b0e-8e8e-8b6c3a3b341b";
const char* BLEHandler::STARTER_VOLTAGE_CHAR_UUID = "5b2e3f4g-8b8a-4b0e-8e8e-8b6c3a3b341b";
const char* BLEHandler::CALIBRATION_STATUS_CHAR_UUID = "9b1e3f4g-8b8a-4b0e-8e8e-8b6c3a3b341b";

BLEHandler::BLEHandler() : pServer(NULL), pService(NULL) {}

void BLEHandler::begin() {
    BLEDevice::init("AE Smart Shunt");
    pServer = BLEDevice::createServer();
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
}

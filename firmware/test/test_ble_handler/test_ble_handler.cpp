#include <unity.h>

#include "ble_handler.h"

// HACK: Include the source file directly to get around linker issues
#include "../../src/ble_handler.cpp"
#include "../lib/mocks/NimBLEDevice.cpp"

void setUp(void) {
    BLEDevice::reset();
}

void tearDown(void) {
    // clean stuff up here
}

void test_ble_handler_initialization(void) {
    BLEHandler handler;
    handler.begin();

    TEST_ASSERT_EQUAL_STRING("AE Smart Shunt", BLEDevice::getDeviceName().c_str());

    BLEServer* pServer = BLEDevice::getServer();
    TEST_ASSERT_NOT_NULL(pServer);

    auto& services = pServer->getServices();
    TEST_ASSERT_EQUAL(1, services.size());

    BLEService* pService = services[BLEHandler::SERVICE_UUID];
    TEST_ASSERT_NOT_NULL(pService);
    TEST_ASSERT_EQUAL_STRING(BLEHandler::SERVICE_UUID, pService->getUUID());

    auto& characteristics = pService->getCharacteristics();
    TEST_ASSERT_EQUAL(7, characteristics.size());

    TEST_ASSERT_NOT_NULL(characteristics[BLEHandler::VOLTAGE_CHAR_UUID]);
    TEST_ASSERT_NOT_NULL(characteristics[BLEHandler::CURRENT_CHAR_UUID]);
    TEST_ASSERT_NOT_NULL(characteristics[BLEHandler::POWER_CHAR_UUID]);
    TEST_ASSERT_NOT_NULL(characteristics[BLEHandler::SOC_CHAR_UUID]);
    TEST_ASSERT_NOT_NULL(characteristics[BLEHandler::CAPACITY_CHAR_UUID]);
    TEST_ASSERT_NOT_NULL(characteristics[BLEHandler::STARTER_VOLTAGE_CHAR_UUID]);
    TEST_ASSERT_NOT_NULL(characteristics[BLEHandler::CALIBRATION_STATUS_CHAR_UUID]);

    TEST_ASSERT_TRUE(BLEDevice::isAdvertising());
}

void test_ble_handler_update_telemetry(void) {
    BLEHandler handler;
    handler.begin();

    Telemetry telemetry_data = {
        .batteryVoltage = 12.5f,
        .batteryCurrent = 1.2f,
        .batteryPower = 15.0f,
        .batterySOC = 0.8f,
        .batteryCapacity = 80.0f,
        .starterBatteryVoltage = 13.8f,
        .isCalibrated = true
    };

    handler.updateTelemetry(telemetry_data);

    BLEServer* pServer = BLEDevice::getServer();
    BLEService* pService = pServer->getServices()[BLEHandler::SERVICE_UUID];
    auto& characteristics = pService->getCharacteristics();

    BLECharacteristic* pVoltageChar = characteristics[BLEHandler::VOLTAGE_CHAR_UUID];
    TEST_ASSERT_EQUAL_FLOAT(telemetry_data.batteryVoltage, pVoltageChar->getFloatValue());
    TEST_ASSERT_EQUAL(1, pVoltageChar->getNotifyCount());

    BLECharacteristic* pCurrentChar = characteristics[BLEHandler::CURRENT_CHAR_UUID];
    TEST_ASSERT_EQUAL_FLOAT(telemetry_data.batteryCurrent, pCurrentChar->getFloatValue());
    TEST_ASSERT_EQUAL(1, pCurrentChar->getNotifyCount());

    BLECharacteristic* pPowerChar = characteristics[BLEHandler::POWER_CHAR_UUID];
    TEST_ASSERT_EQUAL_FLOAT(telemetry_data.batteryPower, pPowerChar->getFloatValue());
    TEST_ASSERT_EQUAL(1, pPowerChar->getNotifyCount());

    BLECharacteristic* pSocChar = characteristics[BLEHandler::SOC_CHAR_UUID];
    TEST_ASSERT_EQUAL_FLOAT(telemetry_data.batterySOC, pSocChar->getFloatValue());
    TEST_ASSERT_EQUAL(1, pSocChar->getNotifyCount());

    BLECharacteristic* pCapacityChar = characteristics[BLEHandler::CAPACITY_CHAR_UUID];
    TEST_ASSERT_EQUAL_FLOAT(telemetry_data.batteryCapacity, pCapacityChar->getFloatValue());
    TEST_ASSERT_EQUAL(1, pCapacityChar->getNotifyCount());

    BLECharacteristic* pStarterVoltageChar = characteristics[BLEHandler::STARTER_VOLTAGE_CHAR_UUID];
    TEST_ASSERT_EQUAL_FLOAT(telemetry_data.starterBatteryVoltage, pStarterVoltageChar->getFloatValue());
    TEST_ASSERT_EQUAL(1, pStarterVoltageChar->getNotifyCount());

    BLECharacteristic* pCalibrationStatusChar = characteristics[BLEHandler::CALIBRATION_STATUS_CHAR_UUID];
    TEST_ASSERT_EQUAL(telemetry_data.isCalibrated, pCalibrationStatusChar->getBoolValue());
    TEST_ASSERT_EQUAL(1, pCalibrationStatusChar->getNotifyCount());
}


int main(int argc, char **argv) {
    UNITY_BEGIN();
    RUN_TEST(test_ble_handler_initialization);
    RUN_TEST(test_ble_handler_update_telemetry);
    UNITY_END();
    return 0;
}

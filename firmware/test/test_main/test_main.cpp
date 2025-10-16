#include <unity.h>

// Include the headers of the classes to be tested
#include "ina226_adc.h"
#include "espnow_handler.h"
#include "shared_defs.h"

// HACK: Include the source file directly to get around linker issues
#include "../../src/ina226_adc.cpp"
#include "../../src/espnow_handler.cpp"
#include "../lib/mocks/Arduino.h"
#include "../lib/mocks/Arduino.cpp"
#include "../lib/mocks/Wire.cpp"
#include "../lib/mocks/Preferences.cpp"
#include "../lib/mocks/INA226_WE.cpp"
#include "../lib/mocks/esp_now.cpp"
#include "../lib/mocks/WiFi.cpp"
#include "../lib/mocks/esp_err.cpp"

void setUp(void) {
    // Reset mocks before each test
    INA226_WE::mockShuntVoltage_mV = 0.0;
    INA226_WE::mockBusVoltage_V = 0.0;
    INA226_WE::mockCurrent_mA = 0.0;
    INA226_WE::mockBusPower = 0.0;
    INA226_WE::overflow = false;
    set_mock_millis(0);
    Preferences::clear_static();
    mock_digital_write_clear();
    mock_esp_deep_sleep_clear();
}

void tearDown(void) {
    // clean stuff up here
}


void test_current_calibration(void) {
    //
    // Test that the INA226_ADC class correctly applies calibration to the raw current.
    //
    INA226_ADC adc(0x40, 0.001, 100.0);

    // Set calibration parameters
    float gain = 1.1;
    float offset_mA = -5.5;
    adc.setCalibration(gain, offset_mA);

    // Set the raw current value that the mock sensor will return
    INA226_WE::mockCurrent_mA = 1000.0;

    // Read the sensor
    adc.readSensors();

    // Check that the calibrated current is correct
    float expectedCurrent_mA = (1000.0 * gain) + offset_mA;
    TEST_ASSERT_EQUAL_FLOAT(-expectedCurrent_mA, adc.getCurrent_mA());
}

void test_battery_capacity(void) {
    //
    // Test the battery capacity calculation
    //
    float initialCapacity = 100.0;
    INA226_ADC adc(0x40, 0.001, initialCapacity);

    // Initialize lastUpdateTime
    set_mock_millis(1000);
    adc.updateBatteryCapacity(0.0);

    // --- Test discharging ---
    set_mock_millis(1000 + 3600 * 1000); // Advance time by 1 hour
    adc.updateBatteryCapacity(-10.0);    // 10A discharge over the last hour

    // After 1 hour at 10A, capacity should decrease by 10Ah
    float expectedCapacity = initialCapacity - 10.0;
    TEST_ASSERT_EQUAL_FLOAT(expectedCapacity, adc.getBatteryCapacity());

    // --- Test charging ---
    set_mock_millis(1000 + 2 * 3600 * 1000); // Advance time by another 1 hour
    adc.updateBatteryCapacity(5.0);        // 5A charge over the last hour

    // After 1 hour at -5A, capacity should increase by 5Ah
    expectedCapacity += 5.0;
    TEST_ASSERT_EQUAL_FLOAT(expectedCapacity, adc.getBatteryCapacity());

    // --- Test capacity limits ---
    // Test not exceeding max capacity
    set_mock_millis(1000 + 3 * 3600 * 1000); // Advance time by another 1 hour
    adc.updateBatteryCapacity(100.0);      // charge with 100A for 1h
    expectedCapacity += 100.0;
    if (expectedCapacity > initialCapacity) {
        expectedCapacity = initialCapacity;
    }
    TEST_ASSERT_EQUAL_FLOAT(expectedCapacity, adc.getBatteryCapacity());

    // Test not going below zero
    set_mock_millis(1000 + 4 * 3600 * 1000); // Advance time by another 1 hour
    adc.updateBatteryCapacity(-200.0);       // discharge with 200A for 1h
    expectedCapacity -= 200.0;
    if (expectedCapacity < 0) {
        expectedCapacity = 0;
    }
    TEST_ASSERT_EQUAL_FLOAT(expectedCapacity, adc.getBatteryCapacity());
}

void test_run_flat_time_formatted(void) {
    bool warning;

    // Test discharging (100Ah capacity, 10A load -> 10 hours)
    {
        INA226_ADC adc(0x40, 0.001, 100.0);
        String result = adc.calculateRunFlatTimeFormatted(-10.0, 12.0, warning);
        TEST_ASSERT_EQUAL_STRING("10 hours until flat", result.c_str());
        TEST_ASSERT(warning);
    }

    // Test charging (100Ah max, 50Ah current, 10A charge -> 5 hours)
    {
        INA226_ADC adc(0x40, 0.001, 100.0);
        // Set current capacity to 50Ah by discharging for 1h at 50A
        set_mock_millis(1);
        adc.updateBatteryCapacity(0); // init
        set_mock_millis(1 + 3600 * 1000);
        adc.updateBatteryCapacity(50.0);
        TEST_ASSERT_EQUAL_FLOAT(50.0, adc.getBatteryCapacity());

        String result = adc.calculateRunFlatTimeFormatted(10.0, 12.0, warning);
        TEST_ASSERT_EQUAL_STRING("5 hours until full", result.c_str());
        // The warning should not be triggered when charging.
        TEST_ASSERT(!warning);
    }

    // Test fully charged (99.6Ah is >= 99.5% of 100Ah)
    {
        INA226_ADC adc(0x40, 0.001, 100.0);
        // Set current capacity to 99.6Ah
        set_mock_millis(1);
        adc.updateBatteryCapacity(0); // init
        set_mock_millis(1 + 3600 * 1000);
        adc.updateBatteryCapacity(0.4);

        String result = adc.calculateRunFlatTimeFormatted(1.0, 12.0, warning);
        TEST_ASSERT_EQUAL_STRING("Fully Charged!", result.c_str());
    }

    // Test > 7 days (200Ah capacity, 1A load -> 200 hours)
    {
        INA226_ADC adc(0x40, 0.001, 200.0);
        String result = adc.calculateRunFlatTimeFormatted(-1.0, 200.0, warning);
        TEST_ASSERT_EQUAL_STRING("> 7 days", result.c_str());
    }

    // Test zero current
    {
        INA226_ADC adc(0x40, 0.001, 100.0);
        String result = adc.calculateRunFlatTimeFormatted(0.0, 12.0, warning);
        TEST_ASSERT_EQUAL_STRING("Fully Charged!", result.c_str());
    }

    // Test days and hours formatting
    {
        INA226_ADC adc(0x40, 0.001, 100.0);
        String result = adc.calculateRunFlatTimeFormatted(-2.0, 100.0, warning); // 50 hours
        TEST_ASSERT_EQUAL_STRING("2 days 2 hours until flat", result.c_str());
    }
}

void test_averaged_run_flat_time(void) {
    INA226_ADC adc(0x40, 0.001, 100.0);
    bool warning;

    // 1. Initial state - no samples yet
    set_mock_millis(0);
    String result = adc.getAveragedRunFlatTime(-10.0, 12.0, warning);
    TEST_ASSERT_EQUAL_STRING("Gathering data...", result.c_str());

    // 2. First sample is taken after 10 seconds
    set_mock_millis(10000); // 10s
    result = adc.getAveragedRunFlatTime(-10.0, 12.0, warning);
    TEST_ASSERT_EQUAL_STRING("Gathering data...", result.c_str()); // Still not enough samples

    // 3. Second sample is taken after 20 seconds
    set_mock_millis(20000); // 20s
    result = adc.getAveragedRunFlatTime(-10.0, 12.0, warning);
    TEST_ASSERT_EQUAL_STRING("Gathering data...", result.c_str()); // Still not enough samples

    // 4. Third sample is taken, now we have enough for an average
    set_mock_millis(30000); // 30s
    result = adc.getAveragedRunFlatTime(-10.0, 12.0, warning);
    TEST_ASSERT_EQUAL_STRING("10 hours until flat", result.c_str());
}

void test_calibration_persistence(void) {
    uint16_t shuntRatedA = 100;
    float gain = 1.23;
    float offset = -4.56;

    // 1. Save calibration
    {
        INA226_ADC adc(0x40, 0.001, 100.0);
        adc.saveCalibration(shuntRatedA, gain, offset);
    }

    // 2. Load calibration into a new instance
    {
        INA226_ADC adc2(0x40, 0.001, 100.0);
        bool loaded = adc2.loadCalibration(shuntRatedA);
        TEST_ASSERT_TRUE(loaded);

        float loadedGain, loadedOffset;
        adc2.getCalibration(loadedGain, loadedOffset);
        TEST_ASSERT_EQUAL_FLOAT(gain, loadedGain);
        TEST_ASSERT_EQUAL_FLOAT(offset, loadedOffset);
    }
}

void test_espnow_handler(void) {
    uint8_t broadcastAddress[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    ESPNowHandler handler(broadcastAddress);

    struct_message_ae_smart_shunt_1 shunt_message;
    shunt_message.messageID = 123;
    shunt_message.batteryVoltage = 12.5;
    shunt_message.batteryCurrent = 1.2;

    handler.setAeSmartShuntStruct(shunt_message);
    handler.sendMessageAeSmartShunt();

    const std::vector<uint8_t>& sent_data = mock_esp_now_get_sent_data();
    TEST_ASSERT_EQUAL(sizeof(shunt_message), sent_data.size());

    struct_message_ae_smart_shunt_1 sent_message;
    memcpy(&sent_message, sent_data.data(), sent_data.size());

    TEST_ASSERT_EQUAL(shunt_message.messageID, sent_message.messageID);
    TEST_ASSERT_EQUAL_FLOAT(shunt_message.batteryVoltage, sent_message.batteryVoltage);
    TEST_ASSERT_EQUAL_FLOAT(shunt_message.batteryCurrent, sent_message.batteryCurrent);
}

void test_main_loop_logic(void) {
    // Mock sensor readings
    INA226_WE::mockBusVoltage_V = 12.8;
    INA226_WE::mockCurrent_mA = 2500.0; // 2.5A
    INA226_WE::mockBusPower = 32000.0; // 32W

    float ratedCapacity = 100.0;
    INA226_ADC adc(0x40, 0.001, ratedCapacity);
    struct_message_ae_smart_shunt_1 shunt_message;

    // Replicate logic from main.cpp loop()
    adc.readSensors();
    shunt_message.batteryVoltage = adc.getBusVoltage_V();
    shunt_message.batteryCurrent = adc.getCurrent_mA() / 1000.0f;
    shunt_message.batteryPower = adc.getPower_mW() / 1000.0f;

    set_mock_millis(1000);
    adc.updateBatteryCapacity(adc.getCurrent_mA() / 1000.0f);
    set_mock_millis(1000 + 3600 * 1000);
    adc.updateBatteryCapacity(adc.getCurrent_mA() / 1000.0f);

    float remainingAh = adc.getBatteryCapacity();
    shunt_message.batteryCapacity = remainingAh;
    if (ratedCapacity > 0.0f) {
        shunt_message.batterySOC = remainingAh / ratedCapacity;
    } else {
        shunt_message.batterySOC = 0.0f;
    }

    // Assertions
    TEST_ASSERT_EQUAL_FLOAT(12.8, shunt_message.batteryVoltage);
    TEST_ASSERT_EQUAL_FLOAT(-2.5, shunt_message.batteryCurrent);
    TEST_ASSERT_EQUAL_FLOAT(-32.0, shunt_message.batteryPower);
    TEST_ASSERT_EQUAL_FLOAT(ratedCapacity - 2.5, shunt_message.batteryCapacity);
    TEST_ASSERT_EQUAL_FLOAT((ratedCapacity - 2.5) / ratedCapacity, shunt_message.batterySOC);
}

void test_protection_settings_persistence(void) {
    float lv_cutoff = 8.5f;
    float hysteresis = 0.8f;
    float oc_thresh = 60.0f;

    // 1. Save settings
    {
        INA226_ADC adc(0x40, 0.001, 100.0);
        adc.setProtectionSettings(lv_cutoff, hysteresis, oc_thresh);
    }

    // 2. Load settings into a new instance
    {
        INA226_ADC adc2(0x40, 0.001, 100.0);
        adc2.loadProtectionSettings();
        TEST_ASSERT_EQUAL_FLOAT(lv_cutoff, adc2.getLowVoltageCutoff());
        TEST_ASSERT_EQUAL_FLOAT(hysteresis, adc2.getHysteresis());
        TEST_ASSERT_EQUAL_FLOAT(oc_thresh, adc2.getOvercurrentThreshold());
    }
}

void test_low_voltage_disconnect(void) {
    INA226_ADC adc(0x40, 0.001, 100.0);
    adc.setProtectionSettings(9.0f, 0.5f, 50.0f);
    adc.setLoadConnected(true);

    INA226_WE::mockBusVoltage_V = 8.9f; // Below cutoff
    adc.readSensors();
    adc.checkAndHandleProtection();

    TEST_ASSERT_FALSE(adc.isLoadConnected());
    TEST_ASSERT_EQUAL(LOW, mock_digital_write_get_last_value(LOAD_SWITCH_PIN));
    TEST_ASSERT_TRUE(mock_esp_deep_sleep_called());
}

void test_overcurrent_disconnect(void) {
    INA226_ADC adc(0x40, 0.001, 100.0);
    adc.setProtectionSettings(9.0f, 0.5f, 50.0f);
    adc.setLoadConnected(true);

    INA226_WE::mockBusVoltage_V = 12.0; // Set a realistic voltage
    INA226_WE::mockCurrent_mA = 51000.0f; // 51A, above threshold
    adc.readSensors();
    adc.checkAndHandleProtection();

    TEST_ASSERT_FALSE(adc.isLoadConnected());
    TEST_ASSERT_EQUAL(LOW, mock_digital_write_get_last_value(LOAD_SWITCH_PIN));
    TEST_ASSERT_FALSE(mock_esp_deep_sleep_called()); // No sleep on overcurrent
}

void test_voltage_reconnect(void) {
    INA226_ADC adc(0x40, 0.001, 100.0);
    adc.setProtectionSettings(9.0f, 0.5f, 50.0f);
    adc.setLoadConnected(false, LOW_VOLTAGE);

    INA226_WE::mockBusVoltage_V = 9.6f; // Above cutoff + hysteresis
    adc.readSensors();
    adc.checkAndHandleProtection();

    TEST_ASSERT_TRUE(adc.isLoadConnected());
    TEST_ASSERT_EQUAL(HIGH, mock_digital_write_get_last_value(LOAD_SWITCH_PIN));
}

void test_alert_disconnect(void) {
    INA226_ADC adc(0x40, 0.001, 100.0);
    adc.setLoadConnected(true);

    // Simulate ISR
    adc.handleAlert();
    TEST_ASSERT_TRUE(adc.isAlertTriggered());

    // Simulate main loop processing
    adc.processAlert();

    TEST_ASSERT_FALSE(adc.isLoadConnected());
    TEST_ASSERT_EQUAL(LOW, mock_digital_write_get_last_value(LOAD_SWITCH_PIN));
    TEST_ASSERT_FALSE(adc.isAlertTriggered());
}

void test_usb_power_no_disconnect(void) {
    INA226_ADC adc(0x40, 0.001, 100.0);
    adc.setProtectionSettings(9.0f, 0.5f, 50.0f);
    adc.setLoadConnected(true);

    INA226_WE::mockBusVoltage_V = 5.0f; // USB power
    adc.readSensors();
    adc.checkAndHandleProtection();

    TEST_ASSERT_TRUE(adc.isLoadConnected());
    TEST_ASSERT_EQUAL(HIGH, mock_digital_write_get_last_value(LOAD_SWITCH_PIN));
    TEST_ASSERT_FALSE(mock_esp_deep_sleep_called());
}

void test_alert_ignored_when_disconnected(void) {
    INA226_ADC adc(0x40, 0.001, 100.0);
    adc.setLoadConnected(false);

    // Simulate ISR
    adc.handleAlert();
    TEST_ASSERT_TRUE(adc.isAlertTriggered());

    // Simulate main loop processing
    adc.processAlert();

    // Assert that the load is still disconnected and the flag is cleared
    TEST_ASSERT_FALSE(adc.isLoadConnected());
    TEST_ASSERT_FALSE(adc.isAlertTriggered());
}

int main(int argc, char **argv) {
    UNITY_BEGIN();
    RUN_TEST(test_current_calibration);
    RUN_TEST(test_battery_capacity);
    RUN_TEST(test_run_flat_time_formatted);
    RUN_TEST(test_averaged_run_flat_time);
    RUN_TEST(test_calibration_persistence);
    RUN_TEST(test_espnow_handler);
    RUN_TEST(test_main_loop_logic);
    RUN_TEST(test_protection_settings_persistence);
    RUN_TEST(test_low_voltage_disconnect);
    RUN_TEST(test_overcurrent_disconnect);
    RUN_TEST(test_voltage_reconnect);
    RUN_TEST(test_alert_disconnect);
    RUN_TEST(test_usb_power_no_disconnect);
    RUN_TEST(test_alert_ignored_when_disconnected);
    UNITY_END();
    return 0;
}

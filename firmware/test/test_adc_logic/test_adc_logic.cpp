#include "../../src/ina226_adc.cpp"
#include "../lib/mocks/Arduino.cpp"
#include "../lib/mocks/Arduino.h"
#include "../lib/mocks/INA226_WE.cpp"
#include "../lib/mocks/Preferences.cpp"
#include "../lib/mocks/Wire.cpp"
#include "../lib/mocks/driver/gpio.cpp"
#include "ina226_adc.h"
#include <unity.h>

void setUp(void) { Preferences::clear_static(); }

void tearDown(void) {}

void test_set_soc_percent(void) {
  float maxCapacity = 100.0f;
  INA226_ADC adc(0x40, 0.001, maxCapacity);

  adc.setSOC_percent(50.0f);
  TEST_ASSERT_EQUAL_FLOAT(50.0f, adc.getBatteryCapacity());

  adc.setSOC_percent(0.0f);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, adc.getBatteryCapacity());

  adc.setSOC_percent(100.0f);
  TEST_ASSERT_EQUAL_FLOAT(100.0f, adc.getBatteryCapacity());

  // Test clamping
  adc.setSOC_percent(110.0f);
  TEST_ASSERT_EQUAL_FLOAT(100.0f, adc.getBatteryCapacity());

  adc.setSOC_percent(-10.0f);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, adc.getBatteryCapacity());
}

void test_set_voltage_protection(void) {
  INA226_ADC adc(0x40, 0.001, 100.0f);

  float cutoff = 9.5f;
  float reconnect = 10.5f;
  float expected_hysteresis = 1.0f;

  adc.setVoltageProtection(cutoff, reconnect);

  TEST_ASSERT_EQUAL_FLOAT(cutoff, adc.getLowVoltageCutoff());
  TEST_ASSERT_EQUAL_FLOAT(expected_hysteresis, adc.getHysteresis());

  // Verify that the values were saved to preferences
  Preferences prefs;
  prefs.begin(NVS_PROTECTION_NAMESPACE, true);
  float saved_cutoff = prefs.getFloat(NVS_KEY_LOW_VOLTAGE_CUTOFF, 0.0f);
  float saved_hysteresis = prefs.getFloat(NVS_KEY_HYSTERESIS, 0.0f);
  prefs.end();

  TEST_ASSERT_EQUAL_FLOAT(cutoff, saved_cutoff);
  TEST_ASSERT_EQUAL_FLOAT(expected_hysteresis, saved_hysteresis);
}

void test_set_voltage_protection_invalid(void) {
  INA226_ADC adc(0x40, 0.001, 100.0f);

  float initial_cutoff = adc.getLowVoltageCutoff();
  float initial_hysteresis = adc.getHysteresis();

  // Test with cutoff >= reconnect
  adc.setVoltageProtection(10.0f, 9.0f);

  // Values should not have changed
  TEST_ASSERT_EQUAL_FLOAT(initial_cutoff, adc.getLowVoltageCutoff());
  TEST_ASSERT_EQUAL_FLOAT(initial_hysteresis, adc.getHysteresis());
}

void test_load_protection_settings_validation(void) {
  // Test case 1: NVS is empty, should load defaults
  {
    INA226_ADC adc(0x40, 0.001, 100.0f);
    adc.loadProtectionSettings();
    TEST_ASSERT_EQUAL_FLOAT(11.6f, adc.getLowVoltageCutoff());
    TEST_ASSERT_EQUAL_FLOAT(0.2f, adc.getHysteresis());
  }

  // Test case 2: NVS has valid values
  {
    Preferences prefs;
    prefs.begin(NVS_PROTECTION_NAMESPACE, false);
    prefs.putFloat(NVS_KEY_LOW_VOLTAGE_CUTOFF, 10.0f);
    prefs.putFloat(NVS_KEY_HYSTERESIS, 1.0f);
    prefs.end();

    INA226_ADC adc(0x40, 0.001, 100.0f);
    adc.loadProtectionSettings();
    TEST_ASSERT_EQUAL_FLOAT(10.0f, adc.getLowVoltageCutoff());
    TEST_ASSERT_EQUAL_FLOAT(1.0f, adc.getHysteresis());
  }

  // Test case 3: NVS has invalid cutoff (too low)
  {
    Preferences::clear_static();
    Preferences prefs;
    prefs.begin(NVS_PROTECTION_NAMESPACE, false);
    prefs.putFloat(NVS_KEY_LOW_VOLTAGE_CUTOFF, 5.0f); // Invalid
    prefs.putFloat(NVS_KEY_HYSTERESIS, 1.0f);
    prefs.end();

    INA226_ADC adc(0x40, 0.001, 100.0f);
    adc.loadProtectionSettings();
    TEST_ASSERT_EQUAL_FLOAT(
        11.6f, adc.getLowVoltageCutoff()); // Should revert to default
    TEST_ASSERT_EQUAL_FLOAT(
        1.0f, adc.getHysteresis()); // Hysteresis should still be loaded
  }

  // Test case 4: NVS has invalid cutoff (too high)
  {
    Preferences::clear_static();
    Preferences prefs;
    prefs.begin(NVS_PROTECTION_NAMESPACE, false);
    prefs.putFloat(NVS_KEY_LOW_VOLTAGE_CUTOFF, 15.0f); // Invalid
    prefs.putFloat(NVS_KEY_HYSTERESIS, 1.0f);
    prefs.end();

    INA226_ADC adc(0x40, 0.001, 100.0f);
    adc.loadProtectionSettings();
    TEST_ASSERT_EQUAL_FLOAT(
        11.6f, adc.getLowVoltageCutoff()); // Should revert to default
    TEST_ASSERT_EQUAL_FLOAT(1.0f, adc.getHysteresis());
  }

  // Test case 5: NVS has invalid hysteresis (too low)
  {
    Preferences::clear_static();
    Preferences prefs;
    prefs.begin(NVS_PROTECTION_NAMESPACE, false);
    prefs.putFloat(NVS_KEY_LOW_VOLTAGE_CUTOFF, 10.0f);
    prefs.putFloat(NVS_KEY_HYSTERESIS, 0.05f); // Invalid
    prefs.end();

    INA226_ADC adc(0x40, 0.001, 100.0f);
    adc.loadProtectionSettings();
    TEST_ASSERT_EQUAL_FLOAT(10.0f, adc.getLowVoltageCutoff());
    TEST_ASSERT_EQUAL_FLOAT(0.2f,
                            adc.getHysteresis()); // Should revert to default
  }

  // Test case 6: NVS has invalid hysteresis (too high)
  {
    Preferences::clear_static();
    Preferences prefs;
    prefs.begin(NVS_PROTECTION_NAMESPACE, false);
    prefs.putFloat(NVS_KEY_LOW_VOLTAGE_CUTOFF, 10.0f);
    prefs.putFloat(NVS_KEY_HYSTERESIS, 4.0f); // Invalid
    prefs.end();

    INA226_ADC adc(0x40, 0.001, 100.0f);
    adc.loadProtectionSettings();
    TEST_ASSERT_EQUAL_FLOAT(10.0f, adc.getLowVoltageCutoff());
    TEST_ASSERT_EQUAL_FLOAT(0.2f,
                            adc.getHysteresis()); // Should revert to default
  }
}

void test_energy_usage_tracking(void) {
  INA226_ADC adc(0x40, 0.001, 100.0f);
  unsigned long initial_millis = 1000000;
  set_mock_millis(initial_millis);

  // Initial call to set timestamps
  adc.updateEnergyUsage(0.0f);

  // Simulate 10W (10000mW) usage for 1 second (1000ms)
  set_mock_millis(initial_millis + 1000);
  adc.updateEnergyUsage(10000.0f);

  // Energy should be 10 Ws. Wh = Ws / 3600
  float expected_wh = 10.0f / 3600.0f;

  TEST_ASSERT_EQUAL_FLOAT(expected_wh, adc.getLastHourEnergy_Wh());
  TEST_ASSERT_EQUAL_FLOAT(expected_wh, adc.getLastDayEnergy_Wh());
  TEST_ASSERT_EQUAL_FLOAT(expected_wh, adc.getLastWeekEnergy_Wh());

  // Simulate another 5W usage for 2 seconds
  set_mock_millis(initial_millis + 3000); // 1000 + 2000
  adc.updateEnergyUsage(5000.0f);         // 5W

  // Total energy is now 10Ws (from before) + 5W * 2s = 20Ws
  expected_wh = 20.0f / 3600.0f;
  TEST_ASSERT_EQUAL_FLOAT(expected_wh, adc.getLastHourEnergy_Wh());
  TEST_ASSERT_EQUAL_FLOAT(expected_wh, adc.getLastDayEnergy_Wh());
  TEST_ASSERT_EQUAL_FLOAT(expected_wh, adc.getLastWeekEnergy_Wh());
}

void test_energy_usage_rollover(void) {
  INA226_ADC adc(0x40, 0.001, 100.0f);
  unsigned long initial_millis = 1000000;
  set_mock_millis(initial_millis);
  adc.updateEnergyUsage(0.0f); // Initialize

  float power_w = 10.0f;
  float power_mw = power_w * 1000.0f;
  unsigned long hour_ms = 3600000;
  unsigned long time_step_ms = 1000;
  unsigned long current_millis = initial_millis;

  // Simulate usage up to the point of rollover
  for (unsigned long t = 0; t < hour_ms; t += time_step_ms) {
    current_millis += time_step_ms;
    set_mock_millis(current_millis);
    adc.updateEnergyUsage(power_mw);
  }

  // At this point, the rollover has just occurred. `currentHourEnergy_Ws` was
  // calculated for the full hour, then copied to `lastHourEnergy_Wh`, and then
  // reset to 0. So, the current hour's usage should be 0.
  TEST_ASSERT_EQUAL_FLOAT(0.0f, adc.getLastHourEnergy_Wh());

  // Simulate one more step of usage
  current_millis += time_step_ms;
  set_mock_millis(current_millis);
  adc.updateEnergyUsage(power_mw);

  // Now, the current hour's usage should be for one time step.
  float expected_wh_after_rollover =
      (power_w * (time_step_ms / 1000.0f)) / 3600.0f;
  TEST_ASSERT_FLOAT_WITHIN(0.0001, expected_wh_after_rollover,
                           adc.getLastHourEnergy_Wh());
}

void test_run_flat_averaging(void) {
  // Setup
  INA226_ADC adc(0x40, 0.001, 100.0f);
  set_mock_millis(1000000);
  bool warning = false;

  // Set SOC to 50% so we have room to charge/discharge
  adc.setSOC_percent(50.0f);

  // 1. Discharge State (-1A)
  // Send 10 samples of -1A. Interval > 10s.
  for (int i = 0; i < 10; i++) {
    set_mock_millis(1000000 + (unsigned long)(i * 10005));
    adc.getAveragedRunFlatTime(-1.0f, 10.0f, warning);
  }

  // Check prediction.
  // Capacity 50Ah. Current -1A. Time 50h.
  // 50h = 2 days 2 hours.
  String res = adc.getAveragedRunFlatTime(-1.0f, 10.0f, warning);
  // res should be "2 days 2 hours until flat"

  // Check if it matches (std::string find)
  bool found = (res.find("2 days 2 hours") != std::string::npos);
  std::string msg = "Expected 2 days 2 hours, got: " + res;
  TEST_ASSERT_TRUE_MESSAGE(found, msg.c_str());

  // 2. Discharge -> Charge State Change
  // Switch to +5A.
  // Need to advance time for sample to be accepted.
  set_mock_millis(1000000 + (10 * 10005) + 10005);

  // This call should trigger RESET and record +5A.
  res = adc.getAveragedRunFlatTime(5.0f, 10.0f, warning);

  // If reset worked, avg is +5A.
  // Remaining to full: 50Ah. 5A. 10 hours.
  // "10 hours until full".
  found = (res.find("10 hours") != std::string::npos);
  msg = "Expected 10 hours, got: " + res;
  TEST_ASSERT_TRUE_MESSAGE(found, msg.c_str());

  // 3. Charging Window (Short) vs Long Discharge Buffer
  // Let's fill the buffer with +5A for 50 samples.
  // Start from i=0 (relative to now)
  unsigned long baseTime = 1000000 + (12 * 10005);
  for (int i = 0; i < 50; i++) {
    set_mock_millis(baseTime + (unsigned long)(i * 10005));
    adc.getAveragedRunFlatTime(5.0f, 10.0f, warning);
  }

  // Now switch to +10A. Still Charging. NO RESET.
  // But averaging window should be max 10.
  // Feed 10 samples of +10A.
  baseTime += (50 * 10005);
  for (int i = 0; i < 10; i++) {
    set_mock_millis(baseTime + (unsigned long)(i * 10005));
    adc.getAveragedRunFlatTime(10.0f, 10.0f, warning);
  }

  // Avg should be 10A (from last 10 samples), NOT mixed with 5A.
  // Time to full: 50Ah / 10A = 5 hours.

  res = adc.getAveragedRunFlatTime(10.0f, 10.0f, warning);
  found = (res.find("5 hours") != std::string::npos);
  msg = "Expected 5 hours, got: " + res;
  TEST_ASSERT_TRUE_MESSAGE(found, msg.c_str());
}

int main(int argc, char **argv) {
  UNITY_BEGIN();
  RUN_TEST(test_set_soc_percent);
  RUN_TEST(test_set_voltage_protection);
  RUN_TEST(test_set_voltage_protection_invalid);
  RUN_TEST(test_load_protection_settings_validation);
  RUN_TEST(test_energy_usage_tracking);
  RUN_TEST(test_energy_usage_rollover);
  RUN_TEST(test_run_flat_averaging);
  UNITY_END();
  return 0;
}

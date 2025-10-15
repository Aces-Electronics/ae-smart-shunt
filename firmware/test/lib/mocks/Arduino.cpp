#include "Arduino.h"
#include "esp_sleep.h"

static unsigned long mock_millis_value = 0;
// g_low_power_sleep_flag is defined in ina226_adc.cpp, which is included by the test
//uint32_t g_low_power_sleep_flag = 0;

unsigned long millis() {
    return mock_millis_value;
}

void set_mock_millis(unsigned long value) {
    mock_millis_value = value;
}

void delay(unsigned long ms) {
    mock_millis_value += ms;
}

MockSerial Serial;

#include <map>
static std::map<uint8_t, uint8_t> mock_pin_modes;
static std::map<uint8_t, uint8_t> mock_digital_write_values;
static bool mock_deep_sleep_called = false;

void pinMode(uint8_t pin, uint8_t mode) {
    mock_pin_modes[pin] = mode;
}

void digitalWrite(uint8_t pin, uint8_t val) {
    mock_digital_write_values[pin] = val;
}

int digitalRead(uint8_t pin) {
    return mock_digital_write_values.count(pin) ? mock_digital_write_values[pin] : 0;
}

int analogRead(uint8_t pin) {
    return 0; // Not mocked yet
}

void mock_digital_write_clear() {
    mock_digital_write_values.clear();
}

int mock_digital_write_get_last_value(uint8_t pin) {
    return mock_digital_write_values.count(pin) ? mock_digital_write_values[pin] : -1;
}

void esp_sleep_enable_timer_wakeup(uint64_t time_in_us) {
    //
}

void esp_deep_sleep_start() {
    mock_deep_sleep_called = true;
}

void mock_esp_deep_sleep_clear() {
    mock_deep_sleep_called = false;
}

bool mock_esp_deep_sleep_called() {
    return mock_deep_sleep_called;
}

esp_reset_reason_t esp_reset_reason(void) {
    // Return a default reason for tests
    return ESP_RST_POWERON;
}

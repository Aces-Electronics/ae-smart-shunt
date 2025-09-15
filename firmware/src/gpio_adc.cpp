#include "gpio_adc.h"
#include <Preferences.h>

// NVS namespace and keys for storing calibration data
#define GPIO_ADC_NVS_NAMESPACE "gpio_adc_cal"
#define GPIO_ADC_KEY_GAIN "gain"
#define GPIO_ADC_KEY_OFFSET "offset"

// Voltage divider resistor values (for uncalibrated readings)
#define R1 180000.0f // 180k Ohm
#define R2 33200.0f  // 33.2k Ohm

GPIO_ADC::GPIO_ADC(int pin) : _pin(pin), _gain(1.0), _offset(0.0), _isCalibrated(false) {}

void GPIO_ADC::begin() {
    pinMode(_pin, INPUT);
    // Attempt to load existing calibration data
    loadCalibration();
}

float GPIO_ADC::readVoltage() {
    int raw_adc = analogRead(_pin);

    if (_isCalibrated) {
        // Apply calibration: V_true = gain * V_measured + offset
        // First, calculate the measured voltage before calibration
        float measured_v = (raw_adc / 4095.0f) * 3.3f * ((R1 + R2) / R2);
        return (_gain * measured_v) + _offset;
    } else {
        // Return uncalibrated voltage based on ideal resistor values and ADC range
        // V_in = V_adc * (R1 + R2) / R2
        // ESP32-C3 ADC is 12-bit (0-4095) and reference voltage is ~3.3V
        return (raw_adc / 4095.0f) * 3.3f * ((R1 + R2) / R2);
    }
}

void GPIO_ADC::calibrate(float true_v1, int raw_adc1, float true_v2, int raw_adc2) {
    // Calculate the measured voltages at the calibration points
    float measured_v1 = (raw_adc1 / 4095.0f) * 3.3f * ((R1 + R2) / R2);
    float measured_v2 = (raw_adc2 / 4095.0f) * 3.3f * ((R1 + R2) / R2);

    // Perform a two-point calibration to find gain and offset
    // true_v = gain * measured_v + offset
    // Solves a system of two linear equations:
    // true_v1 = gain * measured_v1 + offset
    // true_v2 = gain * measured_v2 + offset

    // Avoid division by zero
    if (abs(measured_v2 - measured_v1) < 1e-6) {
        _gain = 1.0;
        _offset = 0.0;
    } else {
        _gain = (true_v2 - true_v1) / (measured_v2 - measured_v1);
        _offset = true_v1 - _gain * measured_v1;
    }

    _isCalibrated = true;
    saveCalibration();
}

bool GPIO_ADC::isCalibrated() const {
    return _isCalibrated;
}

void GPIO_ADC::loadCalibration() {
    Preferences prefs;
    if (prefs.begin(GPIO_ADC_NVS_NAMESPACE, true)) { // read-only
        // Use a sentinel value to check if the key exists
        const float sentinel = 1e30f;
        float gain = prefs.getFloat(GPIO_ADC_KEY_GAIN, sentinel);
        float offset = prefs.getFloat(GPIO_ADC_KEY_OFFSET, sentinel);
        prefs.end();

        if (gain != sentinel && offset != sentinel) {
            _gain = gain;
            _offset = offset;
            _isCalibrated = true;
            Serial.println("Loaded GPIO ADC calibration.");
        } else {
            Serial.println("No GPIO ADC calibration found in NVS.");
        }
    }
}

void GPIO_ADC::saveCalibration() {
    Preferences prefs;
    if (prefs.begin(GPIO_ADC_NVS_NAMESPACE, false)) { // read-write
        prefs.putFloat(GPIO_ADC_KEY_GAIN, _gain);
        prefs.putFloat(GPIO_ADC_KEY_OFFSET, _offset);
        prefs.end();
        Serial.println("Saved GPIO ADC calibration.");
    }
}

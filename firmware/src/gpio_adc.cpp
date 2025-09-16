#include "gpio_adc.h"
#include <algorithm> // For std::sort

// NVS namespace and keys for storing calibration data
#define GPIO_ADC_NVS_NAMESPACE "gpio_adc_cal_v2" // New namespace for new format
#define GPIO_ADC_KEY_COUNT "count"

GPIO_ADC::GPIO_ADC(int pin) : _pin(pin) {}

void GPIO_ADC::begin() {
    pinMode(_pin, INPUT);
    loadCalibration();
}

float GPIO_ADC::readVoltage() {
    if (!isCalibrated()) {
        // For safety, return a value that indicates an uncalibrated state.
        // Returning 0.0 might be misleading.
        return -1.0f;
    }

    int raw_adc = analogRead(_pin);

    // Handle edge cases: value is below the first point or above the last point
    if (raw_adc <= _calibrationTable.front().raw_adc) {
        return _calibrationTable.front().voltage;
    }
    if (raw_adc >= _calibrationTable.back().raw_adc) {
        return _calibrationTable.back().voltage;
    }

    // Find the segment for interpolation
    for (size_t i = 1; i < _calibrationTable.size(); ++i) {
        if (raw_adc < _calibrationTable[i].raw_adc) {
            const auto& p0 = _calibrationTable[i-1];
            const auto& p1 = _calibrationTable[i];

            // Linear interpolation: y = y0 + (x - x0) * (y1 - y0) / (x1 - x0)
            // Avoid division by zero, though sorted unique points should prevent this.
            if (p1.raw_adc == p0.raw_adc) {
                return p0.voltage;
            }
            float t = (float)(raw_adc - p0.raw_adc) / (float)(p1.raw_adc - p0.raw_adc);
            return p0.voltage + t * (p1.voltage - p0.voltage);
        }
    }

    return -1.0f; // Should not be reached if table is sorted and has > 1 point
}

void GPIO_ADC::calibrate(const std::vector<VoltageCalPoint>& points) {
    _calibrationTable = points;
    // Ensure the table is sorted by raw_adc value for correct interpolation
    std::sort(_calibrationTable.begin(), _calibrationTable.end(),
              [](const VoltageCalPoint& a, const VoltageCalPoint& b) {
                  return a.raw_adc < b.raw_adc;
              });
    saveCalibration();
}

const std::vector<VoltageCalPoint>& GPIO_ADC::getCalibrationTable() const {
    return _calibrationTable;
}

bool GPIO_ADC::isCalibrated() const {
    // A valid calibration needs at least two points to interpolate between.
    return _calibrationTable.size() >= 2;
}

void GPIO_ADC::loadCalibration() {
    Preferences prefs;
    if (!prefs.begin(GPIO_ADC_NVS_NAMESPACE, true)) { // read-only
        Serial.println("Could not open GPIO ADC preferences.");
        return;
    }

    uint32_t count = prefs.getUInt(GPIO_ADC_KEY_COUNT, 0);
    if (count == 0) {
        Serial.println("No GPIO ADC calibration found in NVS.");
        prefs.end();
        return;
    }

    _calibrationTable.clear();
    _calibrationTable.reserve(count);

    for (uint32_t i = 0; i < count; ++i) {
        char key_raw[16], key_volt[16];
        snprintf(key_raw, sizeof(key_raw), "raw_%u", i);
        snprintf(key_volt, sizeof(key_volt), "volt_%u", i);

        int raw_adc = prefs.getInt(key_raw, -1);
        float voltage = prefs.getFloat(key_volt, -1.0f);

        if (raw_adc != -1 && voltage != -1.0f) {
            _calibrationTable.push_back({raw_adc, voltage});
        }
    }
    prefs.end();
    Serial.printf("Loaded %u GPIO ADC calibration points.\\n", _calibrationTable.size());
}

void GPIO_ADC::saveCalibration() {
    Preferences prefs;
    if (!prefs.begin(GPIO_ADC_NVS_NAMESPACE, false)) { // read-write
        Serial.println("Could not open GPIO ADC preferences for writing.");
        return;
    }

    // Clear old calibration data before saving new data
    prefs.clear();

    prefs.putUInt(GPIO_ADC_KEY_COUNT, _calibrationTable.size());

    for (size_t i = 0; i < _calibrationTable.size(); ++i) {
        char key_raw[16], key_volt[16];
        snprintf(key_raw, sizeof(key_raw), "raw_%u", i);
        snprintf(key_volt, sizeof(key_volt), "volt_%u", i);

        prefs.putInt(key_raw, _calibrationTable[i].raw_adc);
        prefs.putFloat(key_volt, _calibrationTable[i].voltage);
    }
    prefs.end();
    Serial.printf("Saved %u GPIO ADC calibration points.\\n", _calibrationTable.size());
}

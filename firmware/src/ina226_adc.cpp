#include "ina226_adc.h"
#include <cfloat>
#include <algorithm>

INA226_ADC::INA226_ADC(uint8_t address, float shuntResistorOhms, float batteryCapacityAh)
    : ina226(address),
      defaultOhms(shuntResistorOhms), // Store the default value
      calibratedOhms(shuntResistorOhms), // Initialize with default
      batteryCapacity(batteryCapacityAh),
      maxBatteryCapacity(batteryCapacityAh),
      lastUpdateTime(0),
      shuntVoltage_mV(-1),
      loadVoltage_V(-1),
      busVoltage_V(-1),
      current_mA(-1),
      power_mW(-1),
      calibrationGain(1.0f),
      calibrationOffset_mA(0.0f),
      lowVoltageCutoff(9.0f), // Default for 3S LiFePO4
      hysteresis(0.6f),       // Default hysteresis
      overcurrentThreshold(50.0f), // Default 50A
      loadConnected(true),
      alertTriggered(false),
      m_isConfigured(false),
      m_activeShuntA(50), // Default to 50A
      m_disconnectReason(NONE),
      m_hardwareAlertsDisabled(false),
      sampleIndex(0),
      sampleCount(0),
      lastSampleTime(0),
      sampleIntervalSeconds(10)
{
    for (int i = 0; i < maxSamples; ++i) runFlatSamples[i] = -1.0f;
}

void INA226_ADC::begin(int sdaPin, int sclPin) {
    Wire.begin(sdaPin, sclPin);

    pinMode(LOAD_SWITCH_PIN, OUTPUT);
    setLoadConnected(true, NONE);

    pinMode(INA_ALERT_PIN, INPUT_PULLUP);

    // Load active shunt rating
    Preferences prefs;
    prefs.begin(NVS_CAL_NAMESPACE, true);
    m_activeShuntA = prefs.getUShort(NVS_KEY_ACTIVE_SHUNT, 50); // Default 50A
    prefs.end();
    Serial.printf("Using active shunt rating: %dA\n", m_activeShuntA);

    ina226.init();
    ina226.waitUntilConversionCompleted();

    ina226.setAverage(AVERAGE_16);
    ina226.setConversionTime(CONV_TIME_8244);
    
    // Load the calibrated shunt resistance from NVS, if it exists
    this->m_isConfigured = loadShuntResistance();
    if (!this->m_isConfigured) {
        calibratedOhms = defaultOhms; // Use the default if not found
        Serial.printf("No calibrated shunt resistance found. Using default: %.9f Ohms.\n", calibratedOhms);
    }
    
    // Set the resistor range with the calibrated or default value
    ina226.setResistorRange(calibratedOhms, (float)m_activeShuntA);
    Serial.printf("Set INA226 range for %.2fA\n", (float)m_activeShuntA);

    // Load the calibration table for the active shunt
    if (loadCalibrationTable(m_activeShuntA)) {
        Serial.printf("Loaded calibration table for %dA shunt.\n", m_activeShuntA);
    } else {
        Serial.printf("No calibration table found for %dA shunt.\n", m_activeShuntA);
    }

    loadProtectionSettings();
    configureAlert(overcurrentThreshold);
}

void INA226_ADC::readSensors() {
    ina226.readAndClearFlags();
    shuntVoltage_mV = ina226.getShuntVoltage_mV();
    busVoltage_V = ina226.getBusVoltage_V();
    current_mA = ina226.getCurrent_mA(); // raw mA
    // Calculate power manually, as the chip's internal calculation seems to be off.
    // Use the calibrated current for this calculation.
    power_mW = getBusVoltage_V() * getCurrent_mA();
    loadVoltage_V = busVoltage_V + (shuntVoltage_mV / 1000.0f);
}

float INA226_ADC::getShuntVoltage_mV() const { return shuntVoltage_mV; }
float INA226_ADC::getBusVoltage_V() const { return busVoltage_V; }
float INA226_ADC::getRawCurrent_mA() const { return current_mA; }

float INA226_ADC::getCurrent_mA() const {
    if (!calibrationTable.empty()) {
        return getCalibratedCurrent_mA(current_mA);
    }
    // fallback: linear
    return (current_mA * calibrationGain) + calibrationOffset_mA;
}

float INA226_ADC::getCalibratedCurrent_mA(float raw_mA) const {
    if (calibrationTable.empty()) return raw_mA;

    // Below/above range -> clamp to edge true values
    if (raw_mA <= calibrationTable.front().raw_mA) return calibrationTable.front().true_mA;
    if (raw_mA >= calibrationTable.back().raw_mA)  return calibrationTable.back().true_mA;

    // Find interval [i-1, i] such that raw_mA < points[i].raw_mA
    for (size_t i = 1; i < calibrationTable.size(); ++i) {
        if (raw_mA < calibrationTable[i].raw_mA) {
            const float x0 = calibrationTable[i-1].raw_mA;
            const float y0 = calibrationTable[i-1].true_mA;
            const float x1 = calibrationTable[i].raw_mA;
            const float y1 = calibrationTable[i].true_mA;
            if (fabsf(x1 - x0) < 1e-9f) return y0; // degenerate
            return y0 + (raw_mA - x0) * (y1 - y0) / (x1 - x0);
        }
    }
    return raw_mA; // should not hit
}

float INA226_ADC::getPower_mW() const { return power_mW; }
float INA226_ADC::getLoadVoltage_V() const { return loadVoltage_V; }
float INA226_ADC::getBatteryCapacity() const { return batteryCapacity; }
void INA226_ADC::setBatteryCapacity(float capacity) { batteryCapacity = capacity; }

void INA226_ADC::setCalibration(float gain, float offset_mA) {
    calibrationGain = gain;
    calibrationOffset_mA = offset_mA;
}

void INA226_ADC::getCalibration(float &gainOut, float &offsetOut) const {
    gainOut = calibrationGain;
    offsetOut = calibrationOffset_mA;
}

// Try to load a persisted calibration for a given shunt rating and apply it.
// Returns true if a persisted calibration existed and was applied; false otherwise.
bool INA226_ADC::loadCalibration(uint16_t shuntRatedA) {
    Preferences prefs;
    prefs.begin("ina_cal", true);
    char keyGain[16];
    char keyOff[16];
    snprintf(keyGain, sizeof(keyGain), "g_%u", (unsigned)shuntRatedA);
    snprintf(keyOff, sizeof(keyOff), "o_%u", (unsigned)shuntRatedA);

    const float sentinel = 1e30f;
    float g = prefs.getFloat(keyGain, sentinel);
    float o = prefs.getFloat(keyOff, sentinel);
    prefs.end();

    if (g == sentinel && o == sentinel) {
        // No stored calibration for this shunt rating
        return false;
    }

    // If one was present, apply values (if one missing, replace with sensible defaults)
    if (g == sentinel) g = 1.0f;
    if (o == sentinel) o = 0.0f;

    calibrationGain = g;
    calibrationOffset_mA = o;
    return true;
}

// Query whether a stored calibration exists for the given shunt rating.
// Does not modify the active calibration in memory.
bool INA226_ADC::getStoredCalibrationForShunt(uint16_t shuntRatedA, float &gainOut, float &offsetOut) const {
    Preferences prefs;
    prefs.begin("ina_cal", true);
    char keyGain[16];
    char keyOff[16];
    snprintf(keyGain, sizeof(keyGain), "g_%u", (unsigned)shuntRatedA);
    snprintf(keyOff, sizeof(keyOff), "o_%u", (unsigned)shuntRatedA);

    const float sentinel = 1e30f;
    float g = prefs.getFloat(keyGain, sentinel);
    float o = prefs.getFloat(keyOff, sentinel);
    prefs.end();

    if (g == sentinel && o == sentinel) {
        return false;
    }

    // If one missing use defaults for that entry
    if (g == sentinel) g = 1.0f;
    if (o == sentinel) o = 0.0f;

    gainOut = g;
    offsetOut = o;
    return true;
}

bool INA226_ADC::saveCalibration(uint16_t shuntRatedA, float gain, float offset_mA) {
    Preferences prefs;
    prefs.begin("ina_cal", false);
    char keyGain[16];
    char keyOff[16];
    snprintf(keyGain, sizeof(keyGain), "g_%u", (unsigned)shuntRatedA);
    snprintf(keyOff, sizeof(keyOff), "o_%u", (unsigned)shuntRatedA);
    prefs.putFloat(keyGain, gain);
    prefs.putFloat(keyOff, offset_mA);
    prefs.end();

    calibrationGain = gain;
    calibrationOffset_mA = offset_mA;
    return true;
}

// New method to save calibrated shunt resistance to NVS
bool INA226_ADC::saveShuntResistance(float resistance) {
    Preferences prefs;
    prefs.begin("ina_cal", false);
    prefs.putFloat("cal_ohms", resistance);
    prefs.end();
    calibratedOhms = resistance;
    return true;
}

// New method to load calibrated shunt resistance from NVS
bool INA226_ADC::loadShuntResistance() {
    Preferences prefs;
    // Start preferences in read-only mode
    if (!prefs.begin("ina_cal", true)) {
        // Namespace does not exist, so it's not configured.
        // We can end here, no need to print an error as this is expected on first boot.
        prefs.end();
        return false;
    }

    // Check if the key exists
    if (!prefs.isKey("cal_ohms")) {
        prefs.end();
        return false;
    }

    float resistance = prefs.getFloat("cal_ohms", -1.0f);
    prefs.end();

    if (resistance > 0.0f) {
        calibratedOhms = resistance;
        Serial.printf("Loaded calibrated shunt resistance: %.9f Ohms.\n", calibratedOhms);
        return true;
    }

    return false;
}

// ---------------- Table-based calibration ----------------

static void sortAndDedup(std::vector<CalPoint> &pts) {
    std::sort(pts.begin(), pts.end(), [](const CalPoint &a, const CalPoint &b){
        return a.raw_mA < b.raw_mA;
    });
    // Collapse any duplicate raw_mA by averaging their true_mA
    std::vector<CalPoint> out;
    for (const auto &p : pts) {
        if (out.empty() || fabsf(p.raw_mA - out.back().raw_mA) > 1e-6f) {
            out.push_back(p);
        } else {
            // average
            out.back().true_mA = 0.5f * (out.back().true_mA + p.true_mA);
        }
    }
    pts.swap(out);
}

bool INA226_ADC::saveCalibrationTable(uint16_t shuntRatedA, const std::vector<CalPoint> &points) {
    std::vector<CalPoint> pts = points;
    if (pts.empty()) return false;
    sortAndDedup(pts);

    Preferences prefs;
    prefs.begin("ina_cal", false);

    // Store number of points
    char keyCount[16];
    snprintf(keyCount, sizeof(keyCount), "n_%u", (unsigned)shuntRatedA);
    prefs.putUInt(keyCount, (uint32_t)pts.size());

    // Store each point
    for (size_t i = 0; i < pts.size(); i++) {
        char keyRaw[20], keyTrue[20];
        snprintf(keyRaw,  sizeof(keyRaw),  "r_%u_%u", (unsigned)shuntRatedA, (unsigned)i);
        snprintf(keyTrue, sizeof(keyTrue), "t_%u_%u", (unsigned)shuntRatedA, (unsigned)i);
        prefs.putFloat(keyRaw,  pts[i].raw_mA);
        prefs.putFloat(keyTrue, pts[i].true_mA);
    }

    prefs.end();
    calibrationTable = std::move(pts);
    return true;
}

bool INA226_ADC::loadCalibrationTable(uint16_t shuntRatedA) {
    Preferences prefs;
    prefs.begin("ina_cal", true);

    char keyCount[16];
    snprintf(keyCount, sizeof(keyCount), "n_%u", (unsigned)shuntRatedA);
    uint32_t N = prefs.getUInt(keyCount, 0);

    if (N == 0) {
        prefs.end();
        calibrationTable.clear();
        return false;
    }

    std::vector<CalPoint> pts;
    pts.reserve(N);
    for (uint32_t i = 0; i < N; i++) {
        char keyRaw[20], keyTrue[20];
        snprintf(keyRaw,  sizeof(keyRaw),  "r_%u_%u", (unsigned)shuntRatedA, (unsigned)i);
        snprintf(keyTrue, sizeof(keyTrue), "t_%u_%u", (unsigned)shuntRatedA, (unsigned)i);
        float raw = prefs.getFloat(keyRaw,  NAN);
        float tru = prefs.getFloat(keyTrue, NAN);
        if (isnan(raw) || isnan(tru)) continue;
        pts.push_back({raw, tru});
    }
    prefs.end();

    if (pts.empty()) {
        calibrationTable.clear();
        return false;
    }
    sortAndDedup(pts);
    calibrationTable = std::move(pts);
    return true;
}

bool INA226_ADC::hasCalibrationTable() const {
    return !calibrationTable.empty();
}

const std::vector<CalPoint>& INA226_ADC::getCalibrationTable() const {
    return calibrationTable;
}

bool INA226_ADC::hasStoredCalibrationTable(uint16_t shuntRatedA, size_t &countOut) const {
    Preferences prefs;
    prefs.begin("ina_cal", true);
    char keyCount[16];
    snprintf(keyCount, sizeof(keyCount), "n_%u", (unsigned)shuntRatedA);
    uint32_t N = prefs.getUInt(keyCount, 0);
    prefs.end();
    countOut = (size_t)N;
    return (N > 0);
}

bool INA226_ADC::clearCalibrationTable(uint16_t shuntRatedA) {
    Preferences prefs;
    prefs.begin("ina_cal", false);

    char keyCount[16];
    snprintf(keyCount, sizeof(keyCount), "n_%u", (unsigned)shuntRatedA);
    uint32_t N = prefs.getUInt(keyCount, 0);

    // Remove count first
    prefs.remove(keyCount);

    // Remove individual points if they existed
    for (uint32_t i = 0; i < N; i++) {
        char keyRaw[20], keyTrue[20];
        snprintf(keyRaw,  sizeof(keyRaw),  "r_%u_%u", (unsigned)shuntRatedA, (unsigned)i);
        snprintf(keyTrue, sizeof(keyTrue), "t_%u_%u", (unsigned)shuntRatedA, (unsigned)i);
        prefs.remove(keyRaw);
        prefs.remove(keyTrue);
    }

    prefs.end();
    calibrationTable.clear();
    return true;
}

// ---------------- Battery/run-flat logic (unchanged) ----------------

void INA226_ADC::updateBatteryCapacity(float currentA) {
    unsigned long currentTime = millis();

    if (lastUpdateTime == 0) {
        lastUpdateTime = currentTime;
        return;
    }

    float deltaTimeSec = (currentTime - lastUpdateTime) / 1000.0f;
    float deltaAh = (currentA * deltaTimeSec) / 3600.0f;
    batteryCapacity -= deltaAh;
    if (batteryCapacity < 0.0f) batteryCapacity = 0.0f;
    if (batteryCapacity > maxBatteryCapacity) batteryCapacity = maxBatteryCapacity;
    lastUpdateTime = currentTime;
}

bool INA226_ADC::isOverflow() const { return ina226.overflow; }

String INA226_ADC::calculateRunFlatTimeFormatted(float currentA, float warningThresholdHours, bool &warningTriggered) {
    warningTriggered = false;

    const float maxRunFlatHours = 24.0f * 7.0f;
    float runHours = -1.0f;
    bool charging = false;

    // Define a small tolerance for "fully charged" state, e.g., 99.5%
    const float fullyChargedThreshold = maxBatteryCapacity * 0.995f;

    if (currentA > 0.001f) {
        runHours = batteryCapacity / currentA;
        charging = false;
    } else if (currentA < -0.20f) {
        if (batteryCapacity >= fullyChargedThreshold) {
             return String("Fully Charged!");
        }
        float remainingToFullAh = maxBatteryCapacity - batteryCapacity;
        runHours = remainingToFullAh / (-currentA);
        charging = true;
    }

    if (runHours <= 0.0f) {
      return String("Fully Charged!");
    }

    if (runHours > maxRunFlatHours) {
        return String("> 7 days");
    }

    if (!charging && runHours <= warningThresholdHours) {
        warningTriggered = true;
    }

    uint32_t totalMinutes = (uint32_t)(runHours * 60.0f);
    totalMinutes %= (7 * 24 * 60);
    uint32_t days = totalMinutes / (24 * 60);
    totalMinutes %= (24 * 60);
    uint32_t hours = totalMinutes / 60;

    String result;
    if (days > 0) {
        result += String(days) + (days == 1 ? " day " : " days ");         
    }
    if (hours > 0) {
        result += String(hours) + (hours == 1 ? " hour " : " hours ");
    }

    // Add "until flat" or "until full" based on charging state
    if (!charging) {
        result += "until flat";
    } else {
        result += "until full";
    }

    return result;
}

String INA226_ADC::getAveragedRunFlatTime(float currentA, float warningThresholdHours, bool &warningTriggered) {
    warningTriggered = false;
    const int minSamplesForAverage = 3;
    unsigned long now = millis();

    if (now - lastSampleTime < (unsigned long)sampleIntervalSeconds * 1000UL) {
        if (sampleCount == 0) {
            return String("Gathering data...");
        } else if (sampleCount < minSamplesForAverage) {
            int lastSampleIndex = (sampleIndex + maxSamples - 1) % maxSamples;
            float lastSample = runFlatSamples[lastSampleIndex];
            if (lastSample <= 0.0f) return String("Gathering data...");
            warningTriggered = (lastSample <= warningThresholdHours);
            return calculateRunFlatTimeFormatted((lastSample > 0.0f) ? (batteryCapacity / lastSample) : 0.0f, warningThresholdHours, warningTriggered);
        } else {
            float sum = 0.0f;
            int validSamples = 0;
            for (int i = 0; i < sampleCount; i++) {
                if (runFlatSamples[i] > 0.0f) {
                    sum += runFlatSamples[i];
                    validSamples++;
                }
            }
            float avgRunFlatHours = (validSamples > 0) ? (sum / validSamples) : -1.0f;
            warningTriggered = (avgRunFlatHours >= 0.0f) && (avgRunFlatHours <= warningThresholdHours);
            float approxCurrentA = (avgRunFlatHours > 0.0f) ? (batteryCapacity / avgRunFlatHours) : 0.0f;
            return calculateRunFlatTimeFormatted(approxCurrentA, warningThresholdHours, warningTriggered);
        }
    }

    lastSampleTime = now;

    float currentRunFlatHours = (currentA > 0.001f) ? (batteryCapacity / currentA) : -1.0f;

    if (currentRunFlatHours >= 0.0f) {
        runFlatSamples[sampleIndex] = currentRunFlatHours;
        sampleIndex = (sampleIndex + 1) % maxSamples;
        if (sampleCount < maxSamples) sampleCount++;
    }

    float sum = 0.0f;
    int validSamples = 0;
    for (int i = 0; i < sampleCount; i++) {
        if (runFlatSamples[i] > 0.0f) {
            sum += runFlatSamples[i];
            validSamples++;
        }
    }
    float avgRunFlatHours = (validSamples > 0) ? (sum / validSamples) : -1.0f;

    warningTriggered = (avgRunFlatHours >= 0.0f) && (avgRunFlatHours <= warningThresholdHours);
    float approxCurrentA = (avgRunFlatHours > 0.0f) ? (batteryCapacity / avgRunFlatHours) : 0.0f;
    return calculateRunFlatTimeFormatted(approxCurrentA, warningThresholdHours, warningTriggered);
}

// ---------------- Protection Features ----------------

void INA226_ADC::loadProtectionSettings() {
    Preferences prefs;
    prefs.begin(NVS_PROTECTION_NAMESPACE, true); // read-only
    lowVoltageCutoff = prefs.getFloat(NVS_KEY_LOW_VOLTAGE_CUTOFF, 9.0f);
    hysteresis = prefs.getFloat(NVS_KEY_HYSTERESIS, 0.6f);
    overcurrentThreshold = prefs.getFloat(NVS_KEY_OVERCURRENT, 50.0f);
    prefs.end();
    Serial.println("Loaded protection settings:");
    Serial.printf("  LV Cutoff: %.2fV\n", lowVoltageCutoff);
    Serial.printf("  Hysteresis: %.2fV\n", hysteresis);
    Serial.printf("  OC Threshold: %.2fA\n", overcurrentThreshold);
}

void INA226_ADC::saveProtectionSettings() {
    Preferences prefs;
    prefs.begin(NVS_PROTECTION_NAMESPACE, false); // read-write
    prefs.putFloat(NVS_KEY_LOW_VOLTAGE_CUTOFF, lowVoltageCutoff);
    prefs.putFloat(NVS_KEY_HYSTERESIS, hysteresis);
    prefs.putFloat(NVS_KEY_OVERCURRENT, overcurrentThreshold);
    prefs.end();
    Serial.println("Saved protection settings.");
}

void INA226_ADC::setProtectionSettings(float lv_cutoff, float hyst, float oc_thresh) {
    lowVoltageCutoff = lv_cutoff;
    hysteresis = hyst;
    overcurrentThreshold = oc_thresh;
    saveProtectionSettings();
    configureAlert(overcurrentThreshold); // Re-configure alert with new threshold
}

float INA226_ADC::getLowVoltageCutoff() const {
    return lowVoltageCutoff;
}

float INA226_ADC::getHysteresis() const {
    return hysteresis;
}

float INA226_ADC::getOvercurrentThreshold() const {
    return overcurrentThreshold;
}

void INA226_ADC::checkAndHandleProtection() {
    float voltage = getBusVoltage_V();
    float current = getCurrent_mA() / 1000.0f;

    // If the voltage is very low, it's likely that we are powered via USB
    // for configuration and don't have a battery connected. In this case,
    // we should not trigger low-voltage protection.
    if (voltage < 5.25f) {
        return;
    }

    if (isLoadConnected()) {
        if (voltage < lowVoltageCutoff) {
            Serial.printf("Low voltage detected (%.2fV < %.2fV). Disconnecting load.\n", voltage, lowVoltageCutoff);
            setLoadConnected(false, LOW_VOLTAGE);
            enterSleepMode();
        } else if (current > overcurrentThreshold) {
            Serial.printf("Overcurrent detected (%.2fA > %.2fA). Disconnecting load.\n", current, overcurrentThreshold);
            setLoadConnected(false, OVERCURRENT);
        }
    } else {
        // If load is disconnected, only auto-reconnect if it was for low voltage
        if (m_disconnectReason == LOW_VOLTAGE && voltage > (lowVoltageCutoff + hysteresis)) {
            Serial.printf("Voltage recovered (%.2fV > %.2fV). Reconnecting load.\n", voltage, lowVoltageCutoff + hysteresis);
            setLoadConnected(true, NONE);
        }
    }
}

void INA226_ADC::setLoadConnected(bool connected, DisconnectReason reason) {
    Serial.printf("DEBUG: setLoadConnected called. Target state: %s, Reason: %d\n", connected ? "ON" : "OFF", reason);
    digitalWrite(LOAD_SWITCH_PIN, connected ? HIGH : LOW);
    loadConnected = connected;
    if (connected) {
        m_disconnectReason = NONE;
    } else {
        m_disconnectReason = reason;
    }
    Serial.printf("DEBUG: setLoadConnected finished. Internal state: loadConnected=%s, m_disconnectReason=%d\n", loadConnected ? "true" : "false", m_disconnectReason);
}

bool INA226_ADC::isLoadConnected() const {
    return loadConnected;
}

void INA226_ADC::configureAlert(float amps) {
    if (m_hardwareAlertsDisabled) {
        // Disable alerts by clearing the Mask/Enable Register
        ina226.writeRegister(INA226_WE::INA226_MASK_EN_REG, 0x0000);
        Serial.println("INA226 hardware alert DISABLED.");
    } else {
        // Configure INA226 to trigger alert on overcurrent (shunt voltage over limit)
        float shuntVoltageLimit_V = amps * calibratedOhms;

        ina226.setAlertType(SHUNT_OVER, shuntVoltageLimit_V);
        ina226.enableAlertLatch();
        Serial.printf("Configured INA226 alert for overcurrent threshold of %.2fA (Shunt Voltage > %.4fV)\n",
                      amps, shuntVoltageLimit_V);
    }
}

void INA226_ADC::handleAlert() {
    alertTriggered = true;
}

void INA226_ADC::processAlert() {
    if (alertTriggered) {
        if (m_hardwareAlertsDisabled) {
            // If alerts are disabled, just clear the flag and do nothing else.
            alertTriggered = false;
            ina226.readAndClearFlags();
            return;
        }
        if (isLoadConnected()) { // Only process if the load is currently connected
            Serial.println("Short circuit or overcurrent alert triggered! Disconnecting load.");
            setLoadConnected(false, OVERCURRENT);
        }
        ina226.readAndClearFlags(); // Always clear the alert on the chip
        alertTriggered = false; // Reset the software flag
    }
}

bool INA226_ADC::isAlertTriggered() const {
    return alertTriggered;
}

void INA226_ADC::clearAlerts() {
    ina226.readAndClearFlags();
}

void INA226_ADC::enterSleepMode() {
    Serial.println("Entering deep sleep to conserve power.");
    esp_sleep_enable_timer_wakeup(10 * 1000000); // Wake up every 10 seconds
    esp_deep_sleep_start();
}

bool INA226_ADC::isConfigured() const {
    return m_isConfigured;
}

void INA226_ADC::setTempOvercurrentAlert(float amps) {
    configureAlert(amps);
}

void INA226_ADC::restoreOvercurrentAlert() {
    configureAlert(overcurrentThreshold);
}

void INA226_ADC::toggleHardwareAlerts() {
    m_hardwareAlertsDisabled = !m_hardwareAlertsDisabled;
    // Re-apply the alert configuration to either enable or disable it on the chip
    configureAlert(overcurrentThreshold);
}

bool INA226_ADC::areHardwareAlertsDisabled() const {
    return m_hardwareAlertsDisabled;
}
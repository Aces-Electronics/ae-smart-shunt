#include "ina226_adc.h"
#include "esp_sleep.h"
#include <cfloat>
#include <algorithm>
#include <map>
#include "driver/gpio.h"

// This flag is stored in RTC memory to persist across deep sleep cycles.
RTC_DATA_ATTR uint32_t g_low_power_sleep_flag = 0;
#define LOW_POWER_SLEEP_MAGIC 0x12345678 // A magic number to indicate low power sleep

namespace {
    // Factory-calibrated table for the 50A shunt, based on user-provided data
    const std::vector<CalPoint> factory_cal_50A = {
        {688.171f, 0.000f},
        {2116.966f, 1000.000f},
        {3544.236f, 2000.000f},
        {7829.666f, 5000.000f},
        {15001.298f, 10000.000f},
        {29259.684f, 20000.000f},
        {43518.070f, 30000.002f},
        {57776.453f, 40000.000f},
        {72034.844f, 50000.000f}
    };
} // end anonymous namespace

// Initialize the static map of factory default shunt resistances.
const std::map<uint16_t, float> INA226_ADC::factory_shunt_resistances = {
    {50, 0.000789840f},
    {100, 0.000789840f},
    {150, 0.000789840f},
    {200, 0.000789840f},
    {250, 0.000789840f},
    {300, 0.000789840f},
    {350, 0.000789840f},
    {400, 0.000789840f},
    {450, 0.000789840f},
    {500, 0.000789840f}
};

const std::map<float, float> INA226_ADC::soc_voltage_map = {
    {14.60, 100.0},
    {14.45, 99.0},
    {13.87, 95.0},
    {13.30, 90.0},
    {13.25, 80.0},
    {13.20, 70.0},
    {13.17, 60.0},
    {13.13, 50.0},
    {13.10, 40.0},
    {13.00, 30.0},
    {12.90, 20.0},
    {12.80, 17.0},
    {12.50, 14.0},
    {12.00, 9.0},
    {10.00, 0.0}
};

INA226_ADC::INA226_ADC(uint8_t address, float shuntResistorOhms, float batteryCapacityAh)
    : ina226(address),
      defaultOhms(0.000789840f), // Store the default value
      calibratedOhms(0.000789840f), // Initialize with default
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
      lowVoltageCutoff(11.6f),
      hysteresis(0.2f),
      overcurrentThreshold(50.0f), // Default 50A
      lowVoltageDelayMs(10000), // Default to 10 seconds
      lowVoltageStartTime(0),
      deviceNameSuffix(""),
      loadConnected(true),
      alertTriggered(false),
      m_isConfigured(false),
      m_activeShuntA(50), // Default to 50A
      m_disconnectReason(NONE),
      m_hardwareAlertsDisabled(false),
      m_invertCurrent(false),
      sampleIndex(0),
      sampleCount(0),
      lastSampleTime(0),
      sampleIntervalSeconds(10),
      lastEnergyUpdateTime(0),
      currentHourEnergy_Ws(0.0f),
      currentDayEnergy_Ws(0.0f),
      currentWeekEnergy_Ws(0.0f),
      lastHourEnergy_Wh(0.0f),
      lastDayEnergy_Wh(0.0f),
      lastWeekEnergy_Wh(0.0f),
      currentHourStartMillis(0),
      currentDayStartMillis(0),
      currentWeekStartMillis(0)
{
    for (int i = 0; i < maxSamples; ++i) currentSamples[i] = 0.0f;
}

void INA226_ADC::begin(int sdaPin, int sclPin) {
    esp_reset_reason_t reason = esp_reset_reason();
    bool from_low_power_sleep = (reason == ESP_RST_DEEPSLEEP && g_low_power_sleep_flag == LOW_POWER_SLEEP_MAGIC);

    if (from_low_power_sleep) {
        g_low_power_sleep_flag = 0; // Clear the flag
        Serial.println("Woke from low-power deep sleep. Keeping load OFF.");
    }

    Wire.begin(sdaPin, sclPin);

    pinMode(LOAD_SWITCH_PIN, OUTPUT);
    if(from_low_power_sleep) {
        setLoadConnected(false, LOW_VOLTAGE);
    } else {
        setLoadConnected(true, NONE);
    }

    pinMode(INA_ALERT_PIN, INPUT_PULLUP);

    // Load active shunt rating
    Preferences prefs;
    prefs.begin(NVS_CAL_NAMESPACE, true);
    m_activeShuntA = prefs.getUShort(NVS_KEY_ACTIVE_SHUNT, 100); // Default 100A
    prefs.end();
    Serial.printf("Using active shunt rating: %dA\n", m_activeShuntA);

    ina226.init();
    ina226.waitUntilConversionCompleted();

    ina226.setAverage(AVERAGE_16);
    ina226.setConversionTime(CONV_TIME_8244);
    
    // Try to load the custom calibrated shunt resistance.
    // If it fails, use the factory default for the active shunt.
    this->m_isConfigured = loadShuntResistance();
    if (!this->m_isConfigured) {
        auto it = factory_shunt_resistances.find(m_activeShuntA);
        if (it != factory_shunt_resistances.end()) {
            calibratedOhms = it->second;
            Serial.printf("No custom calibrated shunt resistance found. Using factory default for %dA shunt: %.9f Ohms.\n", m_activeShuntA, calibratedOhms);
        } else {
            calibratedOhms = defaultOhms; // Fallback to the single firmware default
            Serial.printf("No custom calibrated shunt resistance AND no factory default for %dA shunt found. Using firmware default: %.9f Ohms.\n", m_activeShuntA, calibratedOhms);
        }
    }
    
    // Set the resistor range with the calibrated or default value
    ina226.setResistorRange(calibratedOhms, (float)m_activeShuntA);
    Serial.printf("Set INA226 range for %.2fA\n", (float)m_activeShuntA);

    // Load the calibration table for the active shunt
    if (loadCalibrationTable(m_activeShuntA)) {
        Serial.printf("Loaded custom calibration table for %dA shunt.\n", m_activeShuntA);
    } else {
        Serial.printf("No custom calibration table found for %dA shunt. Attempting to load factory default table...\n", m_activeShuntA);
        if (loadFactoryCalibrationTable(m_activeShuntA)) {
            Serial.printf("Successfully loaded factory default calibration table for %dA shunt.\n", m_activeShuntA);
        } else {
            Serial.printf("No factory default calibration table available for %dA shunt.\n", m_activeShuntA);
        }
    }

    loadProtectionSettings();
    loadInvertCurrent();
    configureAlert(overcurrentThreshold);
    setInitialSOC();
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
    float result_mA;
    if (!calibrationTable.empty()) {
        result_mA = getCalibratedCurrent_mA(current_mA);
    }
    else {
        // fallback: linear
        result_mA = (current_mA * calibrationGain) + calibrationOffset_mA;
    }

    if (m_invertCurrent) {
        return -result_mA;
    }
    return result_mA;
}

void INA226_ADC::setInitialSOC() {
    readSensors();
    float voltage = getBusVoltage_V();
    float current = getCurrent_mA() / 1000.0f; // Convert to Amps

    // Adjust voltage based on load/charge
    if (current > 0.1) { // Discharging (under load)
        voltage += 0.4;
    } else if (current < -0.1) { // Charging
        voltage -= 0.4;
    }

    float soc_percent = 50.0; // Default SOC

    // Handle special cases
    if (voltage <= 11.6) {
        soc_percent = 10.0;
    } else if (voltage >= 14.0) {
        soc_percent = 100.0;
    } else {
        // Lookup SOC from the map
        auto it = soc_voltage_map.lower_bound(voltage);
        if (it == soc_voltage_map.end()) {
            soc_percent = 100.0;
        } else if (it == soc_voltage_map.begin()) {
            soc_percent = 0.0;
        } else {
            // Linear interpolation
            float v_high = it->first;
            float soc_high = it->second;
            it--;
            float v_low = it->first;
            float soc_low = it->second;

            if (v_high > v_low) {
                soc_percent = soc_low + ((voltage - v_low) * (soc_high - soc_low)) / (v_high - v_low);
            } else {
                soc_percent = soc_low;
            }
        }
    }

    // Set the battery capacity based on the calculated SOC
    batteryCapacity = maxBatteryCapacity * (soc_percent / 100.0f);
    lastUpdateTime = millis();
    Serial.printf("Initial SOC set to %.2f%% based on adjusted voltage of %.2fV. Initial capacity: %.2fAh\n", soc_percent, voltage, batteryCapacity);
}

float INA226_ADC::getCalibratedCurrent_mA(float raw_mA) const {
    if (calibrationTable.empty()) {
        return raw_mA;
    }

    // Handle negative currents by assuming symmetric calibration around zero.
    const bool is_negative = raw_mA < 0.0f;
    const float abs_raw_mA = fabsf(raw_mA);

    float calibrated_abs_mA;

    // If the absolute value is below or at the first calibration point,
    // use the first calibration point's true value (which should be 0).
    if (abs_raw_mA <= calibrationTable.front().raw_mA) {
        calibrated_abs_mA = calibrationTable.front().true_mA;
    }
    // If the absolute value is above or at the last calibration point,
    // clamp to the last point's true value.
    else if (abs_raw_mA >= calibrationTable.back().raw_mA) {
        calibrated_abs_mA = calibrationTable.back().true_mA;
    }
    // Otherwise, find the interval and linearly interpolate.
    else {
        calibrated_abs_mA = abs_raw_mA; // Fallback
        for (size_t i = 1; i < calibrationTable.size(); ++i) {
            if (abs_raw_mA < calibrationTable[i].raw_mA) {
                const float x0 = calibrationTable[i-1].raw_mA;
                const float y0 = calibrationTable[i-1].true_mA;
                const float x1 = calibrationTable[i].raw_mA;
                const float y1 = calibrationTable[i].true_mA;

                if (fabsf(x1 - x0) < 1e-9f) {
                    calibrated_abs_mA = y0; // Should not happen with sorted, deduped points
                } else {
                    calibrated_abs_mA = y0 + (abs_raw_mA - x0) * (y1 - y0) / (x1 - x0);
                }
                break; // Interval found
            }
        }
    }

    return is_negative ? -calibrated_abs_mA : calibrated_abs_mA;
}

float INA226_ADC::getPower_mW() const { return power_mW; }
float INA226_ADC::getLoadVoltage_V() const { return loadVoltage_V; }
float INA226_ADC::getBatteryCapacity() const { return batteryCapacity; }
void INA226_ADC::setBatteryCapacity(float capacity) { batteryCapacity = capacity; }

void INA226_ADC::setSOC_percent(float percent) {
    if (percent < 0.0f) {
        percent = 0.0f;
    } else if (percent > 100.0f) {
        percent = 100.0f;
    }
    batteryCapacity = maxBatteryCapacity * (percent / 100.0f);
    Serial.printf("SOC set to %.2f%%. New capacity: %.2fAh\n", percent, batteryCapacity);
}

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

    // Immediately apply the new resistance to the INA226 configuration
    ina226.setResistorRange(calibratedOhms, (float)m_activeShuntA);
    Serial.printf("Live INA226 configuration updated for new shunt resistance and %dA range.\n", m_activeShuntA);

    // Mark the device as configured now
    m_isConfigured = true;

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

bool INA226_ADC::loadFactoryDefaultResistance(uint16_t shuntRatedA) {
    auto it = factory_shunt_resistances.find(shuntRatedA);
    if (it != factory_shunt_resistances.end()) {
        float factoryOhms = it->second;

        // Save the factory default resistance to NVS so it persists.
        // This is better than just removing the key, which would cause `isConfigured` to be false on next boot.
        saveShuntResistance(factoryOhms);
        Serial.printf("Saved factory default resistance for %dA shunt to NVS: %.9f Ohms\n", shuntRatedA, factoryOhms);

        // Also clear any associated table calibration to avoid mismatches.
        clearCalibrationTable(shuntRatedA);
        Serial.printf("Cleared any existing calibration table for %dA shunt.\n", shuntRatedA);

        // The saveShuntResistance function already updates the live configuration and sets m_isConfigured.
        return true;
    } else {
        Serial.printf("No factory default found for %dA shunt.\n", shuntRatedA);
        return false;
    }
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

bool INA226_ADC::loadFactoryCalibrationTable(uint16_t shuntRatedA) {
    const std::vector<CalPoint>* factory_table = nullptr;

    switch (shuntRatedA) {
        case 50:
            factory_table = &factory_cal_50A;
            break;
        // Future pre-calibrated tables can be added here
        default:
            Serial.printf("No factory calibration table available for %dA shunt.\\n", shuntRatedA);
            return false;
    }

    if (factory_table) {
        // saveCalibrationTable persists to NVS and loads into RAM
        if (saveCalibrationTable(shuntRatedA, *factory_table)) {
            Serial.printf("Successfully loaded and saved factory calibration for %dA shunt.\\n", shuntRatedA);
            return true;
        }
    }

    Serial.printf("Failed to load factory calibration for %dA shunt.\\n", shuntRatedA);
    return false;
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
    batteryCapacity += deltaAh;
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

    // With inverted current, positive is charging, negative is discharging.
    if (currentA > 0.20f) { // Charging
        if (batteryCapacity >= fullyChargedThreshold) {
             return String("Fully Charged!");
        }
        float remainingToFullAh = maxBatteryCapacity - batteryCapacity;
        runHours = remainingToFullAh / currentA;
        charging = true;
    } else if (currentA < -0.001f) { // Discharging
        runHours = batteryCapacity / (-currentA);
        charging = false;
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
    uint32_t days = totalMinutes / (24 * 60);
    uint32_t hours = (totalMinutes / 60) % 24;
    uint32_t minutes = totalMinutes % 60;

    String result;

    if (days > 0) {
        result += String(days) + (days == 1 ? " day" : " days");
        if (hours > 0) {
            result += " " + String(hours) + (hours == 1 ? " hour" : " hours");
        }
    } else if (hours > 0) {
        result += String(hours) + (hours == 1 ? " hour" : " hours");
        if (minutes > 0) {
            result += " " + String(minutes) + (minutes == 1 ? " min" : " mins");
        }
    } else if (minutes > 0) {
        result += String(minutes) + (minutes == 1 ? " min" : " mins");
    } else {
        if (runHours > 0) {
            result = "< 1 min";
        } else {
            return charging ? "Fully Charged!" : "Instantly flat";
        }
    }

    // Add "until flat" or "until full" based on charging state
    if (!charging) {
        result += " until flat";
    } else {
        result += " until full";
    }

    return result;
}

String INA226_ADC::getAveragedRunFlatTime(float currentA, float warningThresholdHours, bool &warningTriggered) {
    warningTriggered = false;
    const int minSamplesForAverage = 3;
    unsigned long now = millis();

    // Store the new current sample every `sampleIntervalSeconds`
    if (now - lastSampleTime >= (unsigned long)sampleIntervalSeconds * 1000UL) {
        lastSampleTime = now;
        currentSamples[sampleIndex] = currentA;
        sampleIndex = (sampleIndex + 1) % maxSamples;
        if (sampleCount < maxSamples) {
            sampleCount++;
        }
    }

    // If we don't have enough samples yet, return a status message
    if (sampleCount < minSamplesForAverage) {
        return String("Gathering data...");
    }

    // Calculate the average current from the collected samples
    float sum = 0.0f;
    for (int i = 0; i < sampleCount; i++) {
        sum += currentSamples[i];
    }
    float avgCurrentA = sum / (float)sampleCount;

    // Now, use the averaged current to format the time string
    return calculateRunFlatTimeFormatted(avgCurrentA, warningThresholdHours, warningTriggered);
}

// ---------------- Protection Features ----------------

void INA226_ADC::loadProtectionSettings() {
    Preferences prefs;
    prefs.begin(NVS_PROTECTION_NAMESPACE, true); // read-only

    float loaded_cutoff = prefs.getFloat(NVS_KEY_LOW_VOLTAGE_CUTOFF, 11.6f);
    Serial.printf("NVS loaded cutoff: %.2fV\n", loaded_cutoff);
    if (loaded_cutoff < 6.0f || loaded_cutoff > 14.0f) {
        lowVoltageCutoff = 11.6f;
        Serial.println("Loaded cutoff is invalid, using default.");
    } else {
        lowVoltageCutoff = loaded_cutoff;
    }

    float loaded_hysteresis = prefs.getFloat(NVS_KEY_HYSTERESIS, 0.2f);
    Serial.printf("NVS loaded hysteresis: %.2fV\n", loaded_hysteresis);
    if (loaded_hysteresis < 0.1f || loaded_hysteresis > 3.0f) {
        hysteresis = 0.2f;
        Serial.println("Loaded hysteresis is invalid, using default.");
    } else {
        hysteresis = loaded_hysteresis;
    }

    overcurrentThreshold = prefs.getFloat(NVS_KEY_OVERCURRENT, 50.0f);
    lowVoltageDelayMs = prefs.getUInt(NVS_KEY_LOW_VOLTAGE_DELAY, 10000); // Default 10s
    deviceNameSuffix = prefs.getString(NVS_KEY_DEVICE_NAME_SUFFIX, "");
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
    prefs.putUInt(NVS_KEY_LOW_VOLTAGE_DELAY, lowVoltageDelayMs);
    prefs.putString(NVS_KEY_DEVICE_NAME_SUFFIX, deviceNameSuffix);
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

void INA226_ADC::setVoltageProtection(float cutoff, float reconnect_voltage) {
    if (cutoff >= reconnect_voltage) {
        Serial.println("Error: Cutoff voltage must be less than reconnect voltage.");
        return;
    }
    float new_hysteresis = reconnect_voltage - cutoff;
    setProtectionSettings(cutoff, new_hysteresis, this->overcurrentThreshold);
    Serial.printf("Voltage protection updated. Cutoff: %.2fV, Reconnect: %.2fV (Hysteresis: %.2fV)\n", cutoff, reconnect_voltage, new_hysteresis);
}

uint16_t INA226_ADC::getActiveShunt() const {
    return m_activeShuntA;
}

void INA226_ADC::setActiveShunt(uint16_t shuntRatedA) {
    m_activeShuntA = shuntRatedA;

    // Save the selected shunt as the active one
    Preferences prefs;
    prefs.begin(NVS_CAL_NAMESPACE, false);
    prefs.putUShort(NVS_KEY_ACTIVE_SHUNT, m_activeShuntA);
    prefs.end();
    Serial.printf("Set %dA as active shunt.\n", m_activeShuntA);

    // Reload configuration for the new shunt
    // Try to load the custom calibrated shunt resistance.
    // If it fails, use the factory default for the active shunt.
    this->m_isConfigured = loadShuntResistance();
    if (!this->m_isConfigured) {
        auto it = factory_shunt_resistances.find(m_activeShuntA);
        if (it != factory_shunt_resistances.end()) {
            calibratedOhms = it->second;
            Serial.printf("No custom calibrated shunt resistance found. Using factory default for %dA shunt: %.9f Ohms.\n", m_activeShuntA, calibratedOhms);
        } else {
            calibratedOhms = defaultOhms; // Fallback to the single firmware default
            Serial.printf("No custom calibrated shunt resistance AND no factory default for %dA shunt found. Using firmware default: %.9f Ohms.\n", m_activeShuntA, calibratedOhms);
        }
    }

    // Set the resistor range with the calibrated or default value
    ina226.setResistorRange(calibratedOhms, (float)m_activeShuntA);
    Serial.printf("Set INA226 range for %.2fA\n", (float)m_activeShuntA);

    // Load the calibration table for the new active shunt
    if (loadCalibrationTable(m_activeShuntA)) {
        Serial.printf("Loaded calibration table for %dA shunt.\n", m_activeShuntA);
    } else {
        Serial.printf("No calibration table found for %dA shunt.\n", m_activeShuntA);
    }
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

float INA226_ADC::getHardwareAlertThreshold_A() const {
    // Read the raw value from the INA226 Alert Limit Register
    uint16_t alertLimitRaw = ina226.readRegister(INA226_WE::INA226_ALERT_LIMIT_REG);

    // The alert is on Shunt Voltage, LSB is 2.5µV.
    // V_shunt = raw_value * 2.5µV
    float shuntVoltageLimit_V = alertLimitRaw * 2.5e-6f;

    // Convert shunt voltage limit to current limit using Ohm's law: I = V/R
    if (calibratedOhms > 0.0f) {
        return shuntVoltageLimit_V / calibratedOhms;
    } else {
        return 0.0f; // Avoid division by zero
    }
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
        // Low voltage protection
        if (voltage < lowVoltageCutoff) {
            if (lowVoltageStartTime == 0) {
                // Low voltage detected for the first time, start the timer
                lowVoltageStartTime = millis();
                Serial.printf("Low voltage detected (%.2fV < %.2fV). Starting %lus timer.\n", voltage, lowVoltageCutoff, lowVoltageDelayMs / 1000);
            } else {
                // Timer is already running, check if it has expired
                if (millis() - lowVoltageStartTime >= lowVoltageDelayMs) {
                    Serial.printf("Low voltage persistent for %lus. Disconnecting load.\n", lowVoltageDelayMs / 1000);
                    setLoadConnected(false, LOW_VOLTAGE);
                    enterSleepMode();
                }
            }
        } else {
            // Voltage has recovered, reset the timer
            if (lowVoltageStartTime > 0) {
                Serial.println("Voltage recovered. Cancelling disconnect timer.");
                lowVoltageStartTime = 0;
            }
        }

        // Overcurrent protection (immediate) - use absolute value of current
        if (fabs(current) > overcurrentThreshold) {
            Serial.printf("Overcurrent detected (%.2fA > %.2fA). Disconnecting load.\n", fabs(current), overcurrentThreshold);
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
    g_low_power_sleep_flag = LOW_POWER_SLEEP_MAGIC;
    gpio_hold_en(GPIO_NUM_5);
    esp_sleep_enable_timer_wakeup(30 * 1000000); // Wake up every 30 seconds
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

// ---------------- Invert Current ----------------

void INA226_ADC::loadInvertCurrent() {
    Preferences prefs;
    prefs.begin("ina_cal", true);
    m_invertCurrent = prefs.getBool("invert_curr", true);
    prefs.end();
    Serial.printf("Current inversion is %s\n", m_invertCurrent ? "ENABLED" : "DISABLED");
}

void INA226_ADC::saveInvertCurrent() {
    Preferences prefs;
    prefs.begin("ina_cal", false);
    prefs.putBool("invert_curr", m_invertCurrent);
    prefs.end();
}

void INA226_ADC::toggleInvertCurrent() {
    m_invertCurrent = !m_invertCurrent;
    saveInvertCurrent();
    Serial.printf("Current inversion set to: %s\n", m_invertCurrent ? "ENABLED" : "DISABLED");
}

bool INA226_ADC::isInvertCurrentEnabled() const {
    return m_invertCurrent;
}

void INA226_ADC::dumpRegisters() const {
    Serial.println(F("\n--- INA226 Register Dump ---"));

    uint16_t confReg = ina226.readRegister(INA226_WE::INA226_CONF_REG);
    Serial.print(F("Config (0x00)        : 0x"));
    Serial.println(confReg, HEX);

    uint16_t calReg = ina226.readRegister(INA226_WE::INA226_CAL_REG);
    Serial.print(F("Calibration (0x05)   : 0x"));
    Serial.println(calReg, HEX);

    uint16_t maskEnReg = ina226.readRegister(INA226_WE::INA226_MASK_EN_REG);
    Serial.print(F("Mask/Enable (0x06)   : 0x"));
    Serial.println(maskEnReg, HEX);

    uint16_t alertLimitReg = ina226.readRegister(INA226_WE::INA226_ALERT_LIMIT_REG);
    Serial.print(F("Alert Limit (0x07)   : 0x"));
    Serial.println(alertLimitReg, HEX);

    Serial.println(F("----------------------------"));
}

// ---------------- Energy Usage Tracking ----------------

void INA226_ADC::updateEnergyUsage(float power_mW) {
    unsigned long now = millis();
    if (lastEnergyUpdateTime == 0) {
        lastEnergyUpdateTime = now;
        currentHourStartMillis = now;
        currentDayStartMillis = now;
        currentWeekStartMillis = now;
        return;
    }

    float power_W = power_mW / 1000.0f;
    float time_delta_s = (now - lastEnergyUpdateTime) / 1000.0f;
    float energy_delta_Ws = power_W * time_delta_s;

    currentHourEnergy_Ws += energy_delta_Ws;
    currentDayEnergy_Ws += energy_delta_Ws;
    currentWeekEnergy_Ws += energy_delta_Ws;

    lastEnergyUpdateTime = now;

    // Rollover logic
    const unsigned long hour_ms = 3600000;
    const unsigned long day_ms = 24 * hour_ms;
    const unsigned long week_ms = 7 * day_ms;

    if (now - currentHourStartMillis >= hour_ms) {
        lastHourEnergy_Wh = currentHourEnergy_Ws / 3600.0f;
        currentHourEnergy_Ws = 0.0f;
        currentHourStartMillis = now;
    }
    if (now - currentDayStartMillis >= day_ms) {
        lastDayEnergy_Wh = currentDayEnergy_Ws / 3600.0f;
        currentDayEnergy_Ws = 0.0f;
        currentDayStartMillis = now;
    }
    if (now - currentWeekStartMillis >= week_ms) {
        lastWeekEnergy_Wh = currentWeekEnergy_Ws / 3600.0f;
        currentWeekEnergy_Ws = 0.0f;
        currentWeekStartMillis = now;
    }
}

float INA226_ADC::getLastHourEnergy_Wh() const {
    // Return the energy usage for the current (incomplete) hour.
    // This provides a continuously updating value, rather than waiting for the hour to be over.
    return currentHourEnergy_Ws / 3600.0f;
}

float INA226_ADC::getLastDayEnergy_Wh() const {
    // Return the energy usage for the current (incomplete) day.
    return currentDayEnergy_Ws / 3600.0f;
}

float INA226_ADC::getLastWeekEnergy_Wh() const {
    // Return the energy usage for the current (incomplete) week.
    return currentWeekEnergy_Ws / 3600.0f;
}

void INA226_ADC::setLowVoltageDelay(uint32_t delay_s) {
    lowVoltageDelayMs = delay_s * 1000;
    saveProtectionSettings();
}

uint32_t INA226_ADC::getLowVoltageDelay() const {
    return lowVoltageDelayMs / 1000;
}

void INA226_ADC::setDeviceNameSuffix(String suffix) {
    if (suffix.length() > 15) {
        deviceNameSuffix = suffix.substring(0, 15);
    } else {
        deviceNameSuffix = suffix;
    }
    saveProtectionSettings();
}

String INA226_ADC::getDeviceNameSuffix() const {
    return deviceNameSuffix;
}
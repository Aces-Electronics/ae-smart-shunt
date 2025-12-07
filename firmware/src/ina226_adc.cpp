#include "ina226_adc.h"
#include "driver/gpio.h"
#include "esp_sleep.h"
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <map>

// This flag is stored in RTC memory to persist across deep sleep cycles.
RTC_DATA_ATTR uint32_t g_low_power_sleep_flag = 0;
#define LOW_POWER_SLEEP_MAGIC                                                  \
  0x12345678 // A magic number to indicate low power sleep

namespace {
// Factory-calibrated table for the 100A shunt, based on user-provided data
const std::vector<CalPoint> factory_cal_100A = {
    {27.161f, 50.000f},       {2021.088f, 2050.000f},
    {4019.657f, 4050.000f},   {10002.939f, 10050.000f},
    {19962.066f, 20050.000f}, {99592.500f, 100050.000f},
};
// Factory-calibrated table for the 100A shunt, based on user-provided data
const std::vector<CalPoint> factory_cal_200A = {
    {28.015135f, 50.000000f},         {4031.613037f, 4050.000244f},
    {8020.571289f, 8050.000000f},     {20000.863281f, 20050.000000f},
    {199728.696339f, 200050.000000f}, // extrapolated to 200 A
};
} // end anonymous namespace

// Initialize the static map of factory default shunt resistances.
const std::map<uint16_t, float> INA226_ADC::factory_shunt_resistances = {
    {100, 0.003286742f}, {150, 0.003450000f}, {200, 0.003794600f},
    {250, 0.000300000f}, {300, 0.000250000f}, {350, 0.000214286f},
    {400, 0.000187500f}, {450, 0.000166667f}, {500, 0.000150000f}};

const std::map<float, float> INA226_ADC::soc_voltage_map = {
    {14.60, 100.0}, {14.45, 99.0}, {13.87, 95.0}, {13.30, 90.0}, {13.25, 80.0},
    {13.20, 70.0},  {13.17, 60.0}, {13.13, 50.0}, {13.10, 40.0}, {13.00, 30.0},
    {12.90, 20.0},  {12.80, 17.0}, {12.50, 14.0}, {12.00, 9.0},  {10.00, 0.0}};

INA226_ADC::INA226_ADC(uint8_t address, float shuntResistorOhms,
                       float batteryCapacityAh)
    : ina226(address),
      defaultOhms(shuntResistorOhms > 0.0f ? shuntResistorOhms : 0.003286742f),
      calibratedOhms(shuntResistorOhms > 0.0f ? shuntResistorOhms
                                              : 0.003286742f),
      batteryCapacity(batteryCapacityAh), maxBatteryCapacity(batteryCapacityAh),
      lastUpdateTime(0), shuntVoltage_mV(-1), loadVoltage_V(-1),
      busVoltage_V(-1), current_mA(-1), power_mW(-1), calibrationGain(1.0f),
      calibrationOffset_mA(0.0f), lowVoltageCutoff(11.6f), hysteresis(0.2f),
      overcurrentThreshold(50.0f), // Default 50A
      lowVoltageDelayMs(10000),    // Default to 10 seconds
      lowVoltageStartTime(0), deviceNameSuffix(""), loadConnected(true),
      alertTriggered(false), m_isConfigured(false),
      m_activeShuntA(100), // Default to 100A
      m_disconnectReason(NONE), m_hardwareAlertsDisabled(false), sampleIndex(0),
      sampleCount(0), lastSampleTime(0), sampleIntervalSeconds(10),
      lastEnergyUpdateTime(0), currentHourEnergy_Ws(0.0f),
      currentDayEnergy_Ws(0.0f), currentWeekEnergy_Ws(0.0f),
      lastHourEnergy_Wh(0.0f), lastDayEnergy_Wh(0.0f), lastWeekEnergy_Wh(0.0f),
      currentHourStartMillis(0), currentDayStartMillis(0),
      currentWeekStartMillis(0), averagingState(STATE_UNKNOWN) {
  for (int i = 0; i < maxSamples; ++i)
    currentSamples[i] = 0.0f;
}

void INA226_ADC::begin(int sdaPin, int sclPin) {
  esp_reset_reason_t reason = esp_reset_reason();
  bool from_low_power_sleep = (reason == ESP_RST_DEEPSLEEP &&
                               g_low_power_sleep_flag == LOW_POWER_SLEEP_MAGIC);

  if (from_low_power_sleep) {
    g_low_power_sleep_flag = 0; // Clear the flag
    Serial.println("Woke from low-power deep sleep. Keeping load OFF.");
  }

  Wire.begin(sdaPin, sclPin);

  pinMode(LOAD_SWITCH_PIN, OUTPUT);
  if (from_low_power_sleep) {
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
      Serial.printf("No custom calibrated shunt resistance found. Using "
                    "factory default for %dA shunt: %.9f Ohms.\n",
                    m_activeShuntA, calibratedOhms);
    } else {
      calibratedOhms = defaultOhms; // Fallback to the single firmware default
      Serial.printf(
          "No custom calibrated shunt resistance AND no factory default for "
          "%dA shunt found. Using firmware default: %.9f Ohms.\n",
          m_activeShuntA, calibratedOhms);
    }
  }

  // Apply the resistor and calibration settings to the INA226
  applyShuntConfiguration();

  Serial.printf("INA226 boot cfg: activeShunt=%u A, Rsh=%.9f Ohm\n",
                m_activeShuntA, calibratedOhms);
  // Load the calibration table for the active shunt
  if (loadCalibrationTable(m_activeShuntA)) {
    Serial.printf("Loaded custom calibration table for %dA shunt.\n",
                  m_activeShuntA);
  } else {
    Serial.printf("No custom calibration table found for %dA shunt. Attempting "
                  "to load factory default table...\n",
                  m_activeShuntA);
    if (loadFactoryCalibrationTable(m_activeShuntA)) {
      Serial.printf("Successfully loaded factory default calibration table for "
                    "%dA shunt.\n",
                    m_activeShuntA);
    } else {
      Serial.printf(
          "No factory default calibration table available for %dA shunt.\n",
          m_activeShuntA);
    }
  }

  loadProtectionSettings();
  configureAlert(overcurrentThreshold);
  setInitialSOC();
}

void INA226_ADC::readSensors() {
  ina226.readAndClearFlags();
  shuntVoltage_mV = ina226.getShuntVoltage_mV();
  busVoltage_V = ina226.getBusVoltage_V();
  current_mA = ina226.getCurrent_mA(); // raw mA
  // Calculate power manually, as the chip's internal calculation seems to be
  // off. Use the calibrated current for this calculation.
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
  } else {
    // fallback: linear
    result_mA = (current_mA * calibrationGain) + calibrationOffset_mA;
  }

  return -result_mA;
}

void INA226_ADC::setInitialSOC() {
  readSensors();
  float voltage = getBusVoltage_V();
  float current = getCurrent_mA() / 1000.0f; // Convert to Amps

  // Adjust voltage based on load/charge
  if (current < -0.1f) { // Discharging (under load)
    voltage += 0.4f;
  } else if (current > 0.1f) { // Charging
    voltage -= 0.4f;
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
        soc_percent = soc_low + ((voltage - v_low) * (soc_high - soc_low)) /
                                    (v_high - v_low);
      } else {
        soc_percent = soc_low;
      }
    }
  }

  // Set the battery capacity based on the calculated SOC
  batteryCapacity = maxBatteryCapacity * (soc_percent / 100.0f);
  lastUpdateTime = millis();
  Serial.printf("Initial SOC set to %.2f%% based on adjusted voltage of %.2fV. "
                "Initial capacity: %.2fAh\n",
                soc_percent, voltage, batteryCapacity);
}

float INA226_ADC::getCalibratedCurrent_mA(float raw_mA) const {

  if (calibrationTable.size() < 2)
    return raw_mA;
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
        const float x0 = calibrationTable[i - 1].raw_mA;
        const float y0 = calibrationTable[i - 1].true_mA;
        const float x1 = calibrationTable[i].raw_mA;
        const float y1 = calibrationTable[i].true_mA;

        if (fabsf(x1 - x0) < 1e-9f) {
          calibrated_abs_mA =
              y0; // Should not happen with sorted, deduped points
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
void INA226_ADC::setBatteryCapacity(float capacity) {
  batteryCapacity = capacity;
}

void INA226_ADC::setSOC_percent(float percent) {
  if (percent < 0.0f) {
    percent = 0.0f;
  } else if (percent > 100.0f) {
    percent = 100.0f;
  }
  batteryCapacity = maxBatteryCapacity * (percent / 100.0f);
  Serial.printf("SOC set to %.2f%%. New capacity: %.2fAh\n", percent,
                batteryCapacity);
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
// Returns true if a persisted calibration existed and was applied; false
// otherwise.
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

  // If one was present, apply values (if one missing, replace with sensible
  // defaults)
  if (g == sentinel)
    g = 1.0f;
  if (o == sentinel)
    o = 0.0f;

  calibrationGain = g;
  calibrationOffset_mA = o;
  return true;
}

// Query whether a stored calibration exists for the given shunt rating.
// Does not modify the active calibration in memory.
bool INA226_ADC::getStoredCalibrationForShunt(uint16_t shuntRatedA,
                                              float &gainOut,
                                              float &offsetOut) const {
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
  if (g == sentinel)
    g = 1.0f;
  if (o == sentinel)
    o = 0.0f;

  gainOut = g;
  offsetOut = o;
  return true;
}

bool INA226_ADC::saveCalibration(uint16_t shuntRatedA, float gain,
                                 float offset_mA) {
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
  applyShuntConfiguration();

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
    // We can end here, no need to print an error as this is expected on first
    // boot.
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
    Serial.printf("Loaded calibrated shunt resistance: %.9f Ohms.\n",
                  calibratedOhms);
    return true;
  }

  return false;
}

bool INA226_ADC::loadFactoryDefaultResistance(uint16_t shuntRatedA) {
  auto it = factory_shunt_resistances.find(shuntRatedA);
  if (it != factory_shunt_resistances.end()) {
    float factoryOhms = it->second;

    // Save the factory default resistance to NVS so it persists.
    // This is better than just removing the key, which would cause
    // `isConfigured` to be false on next boot.
    saveShuntResistance(factoryOhms);
    Serial.printf(
        "Saved factory default resistance for %dA shunt to NVS: %.9f Ohms\n",
        shuntRatedA, factoryOhms);

    // Also clear any associated table calibration to avoid mismatches.
    clearCalibrationTable(shuntRatedA);
    Serial.printf("Cleared any existing calibration table for %dA shunt.\n",
                  shuntRatedA);

    // The saveShuntResistance function already updates the live configuration
    // and sets m_isConfigured.
    return true;
  } else {
    Serial.printf("No factory default found for %dA shunt.\n", shuntRatedA);
    return false;
  }
}

bool INA226_ADC::getFactoryDefaultResistance(uint16_t shuntRatedA,
                                             float &outOhms) const {
  auto it = factory_shunt_resistances.find(shuntRatedA);
  if (it == factory_shunt_resistances.end()) {
    return false;
  }

  outOhms = it->second;
  return true;
}

void INA226_ADC::applyShuntConfiguration() {
  // Sanity-check the stored/calculated shunt value; reject obviously-bad values
  // Typical shunts here are ~0.2–5.0 mΩ
  float shunt = calibratedOhms;

  if (!(shunt > 0.0002f && shunt < 0.005f)) {
    Serial.printf(
        "WARN: Rejected invalid shunt resistance %.9f Ohm; falling back.\n",
        shunt);
    shunt = defaultOhms;
  }
  if (!(shunt > 0.0002f && shunt < 0.005f)) {
    Serial.printf("WARN: Rejected invalid shunt resistance %.9f Ω; falling "
                  "back to defaults.\n",
                  shunt);
    shunt = defaultOhms;
  }

  // Derive current_LSB sensibly. If the active shunt rating is known, use that;
  // otherwise derive max current from INA226 shunt range (±81.92 mV) with a
  // margin.
  float maxCurrentA = static_cast<float>(m_activeShuntA);
  if (!(maxCurrentA > 0.0f)) {
    maxCurrentA = (0.08192f * 0.95f) / shunt; // 5% headroom
  }
  float currentLsbA = maxCurrentA / 32768.0f;

  if (!(currentLsbA > 0.0f) || currentLsbA > 0.01f) {
    Serial.printf("WARN: current_LSB %.6f A out of expected range; fixing\n",
                  currentLsbA);
    currentLsbA = 100.0f / 32768.0f;
  }
  if (!(currentLsbA > 0.0f))
    currentLsbA = 0.0001f;

  ina226.setCalibration(shunt, currentLsbA);
  Serial.printf("Configured INA226: Rsh=%.9f Ohm, I_LSB=%.6f A (max~=%.2f A)\n",
                shunt, currentLsbA, maxCurrentA);
  Serial.printf("Configured INA226: Rsh=%.9f Ω, I_LSB=%.6f A (max≈%.2f A).\n",
                shunt, currentLsbA, maxCurrentA);
}

// ---------------- Table-based calibration ----------------

static void sortAndDedup(std::vector<CalPoint> &pts) {
  std::sort(pts.begin(), pts.end(), [](const CalPoint &a, const CalPoint &b) {
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

bool INA226_ADC::saveCalibrationTable(uint16_t shuntRatedA,
                                      const std::vector<CalPoint> &points) {
  std::vector<CalPoint> pts = points;
  if (pts.empty())
    return false;
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
    snprintf(keyRaw, sizeof(keyRaw), "r_%u_%u", (unsigned)shuntRatedA,
             (unsigned)i);
    snprintf(keyTrue, sizeof(keyTrue), "t_%u_%u", (unsigned)shuntRatedA,
             (unsigned)i);
    prefs.putFloat(keyRaw, pts[i].raw_mA);
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
    snprintf(keyRaw, sizeof(keyRaw), "r_%u_%u", (unsigned)shuntRatedA,
             (unsigned)i);
    snprintf(keyTrue, sizeof(keyTrue), "t_%u_%u", (unsigned)shuntRatedA,
             (unsigned)i);
    float raw = prefs.getFloat(keyRaw, NAN);
    float tru = prefs.getFloat(keyTrue, NAN);
    if (isnan(raw) || isnan(tru))
      continue;
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

const std::vector<CalPoint> &INA226_ADC::getCalibrationTable() const {
  return calibrationTable;
}

bool INA226_ADC::hasStoredCalibrationTable(uint16_t shuntRatedA,
                                           size_t &countOut) const {
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
    snprintf(keyRaw, sizeof(keyRaw), "r_%u_%u", (unsigned)shuntRatedA,
             (unsigned)i);
    snprintf(keyTrue, sizeof(keyTrue), "t_%u_%u", (unsigned)shuntRatedA,
             (unsigned)i);
    prefs.remove(keyRaw);
    prefs.remove(keyTrue);
  }

  prefs.end();
  calibrationTable.clear();
  return true;
}

bool INA226_ADC::loadFactoryCalibrationTable(uint16_t shuntRatedA) {
  const std::vector<CalPoint> *factory_table = nullptr;

  switch (shuntRatedA) {
  case 100:
    factory_table = &factory_cal_100A;
    break;
  // Future pre-calibrated tables can be added here
  default:
    Serial.printf("No factory calibration table available for %dA shunt.\\n",
                  shuntRatedA);
    return false;
  }

  if (factory_table) {
    // saveCalibrationTable persists to NVS and loads into RAM
    if (saveCalibrationTable(shuntRatedA, *factory_table)) {
      Serial.printf(
          "Successfully loaded and saved factory calibration for %dA shunt.\\n",
          shuntRatedA);
      return true;
    }
  }

  Serial.printf("Failed to load factory calibration for %dA shunt.\\n",
                shuntRatedA);
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

  // Sync SOC with voltage extrema to correct drift
  checkSoCSync();

  if (batteryCapacity < 0.0f)
    batteryCapacity = 0.0f;
  if (batteryCapacity > maxBatteryCapacity)
    batteryCapacity = maxBatteryCapacity;
  lastUpdateTime = currentTime;
}

void INA226_ADC::checkSoCSync() {
  // Current variables are expected to be up to date (readSensors called
  // recently)
  float currentA = current_mA / 1000.0f;

  // Tail current threshold: e.g. 4% of capacity.
  // If charging at high voltage with low current, we are full.
  float tailCurrentA = maxBatteryCapacity * 0.04f;

  // Sync to 100%
  // If voltage is high (Absorption/Float) and current has dropped off.
  // We use 14.2V as a general "Charging" voltage threshold for 12V LiFePO4/Lead
  // AND current is positive but low.
  if (busVoltage_V > 14.2f && currentA > 0.0f && currentA < tailCurrentA) {
    // Debounce could be added here, but for now we sync immediately
    // Only upwards sync if we are close to full? No, drift can be large.
    if (batteryCapacity < maxBatteryCapacity) {
      // Only log seldomly to avoid spam?
      // Serial.println("SOC Synced to 100%");
      batteryCapacity = maxBatteryCapacity;
    }
  }

  // Hard Sync on Over-Voltage (14.6V is usually absolute max for 12V LFP)
  // If we hit this, we are definitely full.
  if (busVoltage_V >= 14.0f) {
    batteryCapacity = maxBatteryCapacity;
  }

  // Sync to 0%
  // If voltage drops below absolute functional minimum.
  if (busVoltage_V < 10.5f) {
    batteryCapacity = 0.0f;
  }
}

bool INA226_ADC::isOverflow() const { return ina226.overflow; }

String INA226_ADC::calculateRunFlatTimeFormatted(float currentA,
                                                 float warningThresholdHours,
                                                 bool &warningTriggered) {
  warningTriggered = false;

  const float maxRunFlatHours = 24.0f * 7.0f;
  float runHours = -1.0f;
  bool charging = false;

  // Define a small tolerance for "fully charged" state, e.g., 99.5%
  const float fullyChargedThreshold = maxBatteryCapacity * 0.995f;

  // Positive current indicates charging, negative indicates discharging.
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

String INA226_ADC::getAveragedRunFlatTime(float currentA,
                                          float warningThresholdHours,
                                          bool &warningTriggered) {
  warningTriggered = false;
  const int minSamplesForAverage = 3;
  unsigned long now = millis();

  // --- State Detection & Buffer Reset ---
  // We maintain the averaging buffer, but if we switch modes (Charge <->
  // Discharge), we reset the buffer to ensure immediate responsiveness and
  // avoid mixing data.

  CurrentState newState = averagingState;
  // Hysteresis for state switching
  if (currentA > 0.1f) {
    newState = STATE_CHARGING;
  } else if (currentA <= 0.05f) {
    // We treat Idle (approx 0A) as part of the "Discharge/RunFlat" state
    // so that fridge cycling (Load -> Idle -> Load) is averaged together.
    newState = STATE_DISCHARGING;
  } else if (newState == STATE_UNKNOWN) {
    // Initial state determination if in deadband
    newState = (currentA > 0) ? STATE_CHARGING : STATE_DISCHARGING;
  }

  // Reset buffer on true state change
  if (newState != averagingState) {
    if (averagingState != STATE_UNKNOWN) {
      // Reset buffer
      sampleIndex = 0;
      sampleCount = 0;
      // Clearing memory is optional but cleaner for debugging
      for (int i = 0; i < maxSamples; ++i)
        currentSamples[i] = 0.0f;
    }
    averagingState = newState;
  }

  // --- Sample Acquisition ---
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
    // Fallback to instant calculation if needed, or just wait
    return calculateRunFlatTimeFormatted(currentA, warningThresholdHours,
                                         warningTriggered);
  }

  // --- Adaptive Averaging ---
  // User Requirement: "small window to start with" (handled by sampleCount
  // growing) "keep the time until full/charged average window the same as it is
  // now" (Short) "average the time until flat over a longer period" (Long)

  int countToAverage = sampleCount;

  if (averagingState == STATE_CHARGING) {
    // Cap the averaging window for Charging to keep it responsive (e.g., 10
    // samples ~ 100s) This matches the original behavior.
    const int chargingWindow = 10;
    if (countToAverage > chargingWindow)
      countToAverage = chargingWindow;
  }
  // For DISCHARGING, we use the full available `sampleCount` (up to
  // maxSamples/1 hour)

  float sum = 0.0f;
  // Sum the last `countToAverage` samples from the ring buffer.
  // The latest sample is at `(sampleIndex - 1 + maxSamples) % maxSamples`.
  int idx = (sampleIndex - 1 + maxSamples) % maxSamples;

  for (int i = 0; i < countToAverage; i++) {
    sum += currentSamples[idx];
    idx = (idx - 1 + maxSamples) % maxSamples;
  }

  float avgCurrentA = sum / (float)countToAverage;

  // Now, use the averaged current to format the time string
  return calculateRunFlatTimeFormatted(avgCurrentA, warningThresholdHours,
                                       warningTriggered);
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
  lowVoltageDelayMs =
      prefs.getUInt(NVS_KEY_LOW_VOLTAGE_DELAY, 10000); // Default 10s
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

void INA226_ADC::setProtectionSettings(float lv_cutoff, float hyst,
                                       float oc_thresh) {
  lowVoltageCutoff = lv_cutoff;
  hysteresis = hyst;
  overcurrentThreshold = oc_thresh;
  saveProtectionSettings();
  configureAlert(overcurrentThreshold); // Re-configure alert with new threshold
}

void INA226_ADC::setVoltageProtection(float cutoff, float reconnect_voltage) {
  if (cutoff >= reconnect_voltage) {
    Serial.println(
        "Error: Cutoff voltage must be less than reconnect voltage.");
    return;
  }
  float new_hysteresis = reconnect_voltage - cutoff;
  setProtectionSettings(cutoff, new_hysteresis, this->overcurrentThreshold);
  Serial.printf("Voltage protection updated. Cutoff: %.2fV, Reconnect: %.2fV "
                "(Hysteresis: %.2fV)\n",
                cutoff, reconnect_voltage, new_hysteresis);
}

uint16_t INA226_ADC::getActiveShunt() const { return m_activeShuntA; }

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
      Serial.printf("No custom calibrated shunt resistance found. Using "
                    "factory default for %dA shunt: %.9f Ohms.\n",
                    m_activeShuntA, calibratedOhms);
    } else {
      calibratedOhms = defaultOhms; // Fallback to the single firmware default
      Serial.printf(
          "No custom calibrated shunt resistance AND no factory default for "
          "%dA shunt found. Using firmware default: %.9f Ohms.\n",
          m_activeShuntA, calibratedOhms);
    }
  }

  // Apply the resistor range and calibration settings to the INA226
  applyShuntConfiguration();

  // Load the calibration table for the new active shunt
  if (loadCalibrationTable(m_activeShuntA)) {
    Serial.printf("Loaded calibration table for %dA shunt.\n", m_activeShuntA);
  } else {
    Serial.printf("No calibration table found for %dA shunt.\n",
                  m_activeShuntA);
  }
}

float INA226_ADC::getLowVoltageCutoff() const { return lowVoltageCutoff; }

float INA226_ADC::getHysteresis() const { return hysteresis; }

float INA226_ADC::getOvercurrentThreshold() const {
  return overcurrentThreshold;
}

float INA226_ADC::getHardwareAlertThreshold_A() const {
  // Read the raw value from the INA226 Alert Limit Register
  uint16_t alertLimitRaw =
      ina226.readRegister(INA226_WE::INA226_ALERT_LIMIT_REG);

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

float INA226_ADC::getCalibratedShuntResistance() const {
  return calibratedOhms;
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
        Serial.printf(
            "Low voltage detected (%.2fV < %.2fV). Starting %lus timer.\n",
            voltage, lowVoltageCutoff, lowVoltageDelayMs / 1000);
      } else {
        // Timer is already running, check if it has expired
        if (millis() - lowVoltageStartTime >= lowVoltageDelayMs) {
          Serial.printf(
              "Low voltage persistent for %lus. Disconnecting load.\n",
              lowVoltageDelayMs / 1000);
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
      Serial.printf(
          "Overcurrent detected (%.2fA > %.2fA). Disconnecting load.\n",
          fabs(current), overcurrentThreshold);
      setLoadConnected(false, OVERCURRENT);
    }
  } else {
    // If load is disconnected, only auto-reconnect if it was for low voltage
    if (m_disconnectReason == LOW_VOLTAGE &&
        voltage > (lowVoltageCutoff + hysteresis)) {
      Serial.printf("Voltage recovered (%.2fV > %.2fV). Reconnecting load.\n",
                    voltage, lowVoltageCutoff + hysteresis);
      setLoadConnected(true, NONE);
    }
  }
}

void INA226_ADC::setLoadConnected(bool connected, DisconnectReason reason) {
  Serial.printf(
      "DEBUG: setLoadConnected called. Target state: %s, Reason: %d\n",
      connected ? "ON" : "OFF", reason);
  digitalWrite(LOAD_SWITCH_PIN, connected ? HIGH : LOW);
  loadConnected = connected;
  if (connected) {
    m_disconnectReason = NONE;
  } else {
    m_disconnectReason = reason;
  }
  Serial.printf("DEBUG: setLoadConnected finished. Internal state: "
                "loadConnected=%s, m_disconnectReason=%d\n",
                loadConnected ? "true" : "false", m_disconnectReason);
}

bool INA226_ADC::isLoadConnected() const { return loadConnected; }

void INA226_ADC::configureAlert(float amps) {
  if (m_hardwareAlertsDisabled) {
    // Disable alerts by clearing the Mask/Enable Register
    ina226.writeRegister(INA226_WE::INA226_MASK_EN_REG, 0x0000);
    Serial.println("INA226 hardware alert DISABLED.");
    return;
  }

  // Program alert as SHUNT_OVER in VOLTS to avoid dependence on CAL or current
  // LSB. V_limit = I_limit * R_shunt
  const float limitAmps = fabsf(amps);
  const float r_shunt = (calibratedOhms > 0.0f) ? calibratedOhms : defaultOhms;
  const float v_limit = limitAmps * r_shunt; // volts

  ina226.setAlertType(SHUNT_OVER, v_limit);
  ina226.enableAlertLatch();
  Serial.printf("Configured INA226 alert: %.2f A (%.3f mV @ %.6f Ω).\n",
                limitAmps, v_limit * 1000.0f, r_shunt);
}

void INA226_ADC::handleAlert() { alertTriggered = true; }

void INA226_ADC::processAlert() {
  if (alertTriggered) {
    if (m_hardwareAlertsDisabled) {
      // If alerts are disabled, just clear the flag and do nothing else.
      alertTriggered = false;
      ina226.readAndClearFlags();
      return;
    }
    if (isLoadConnected()) { // Only process if the load is currently connected
      Serial.println(
          "Short circuit or overcurrent alert triggered! Disconnecting load.");
      setLoadConnected(false, OVERCURRENT);
    }
    ina226.readAndClearFlags(); // Always clear the alert on the chip
    alertTriggered = false;     // Reset the software flag
  }
}

bool INA226_ADC::isAlertTriggered() const { return alertTriggered; }

void INA226_ADC::clearAlerts() { ina226.readAndClearFlags(); }

void INA226_ADC::enterSleepMode() {
  Serial.println("Entering deep sleep to conserve power.");
  g_low_power_sleep_flag = LOW_POWER_SLEEP_MAGIC;
  gpio_hold_en(GPIO_NUM_5);
  esp_sleep_enable_timer_wakeup(30 * 1000000); // Wake up every 30 seconds
  esp_deep_sleep_start();
}

bool INA226_ADC::isConfigured() const { return m_isConfigured; }

void INA226_ADC::setTempOvercurrentAlert(float amps) { configureAlert(amps); }

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

  uint16_t alertLimitReg =
      ina226.readRegister(INA226_WE::INA226_ALERT_LIMIT_REG);
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
  // This provides a continuously updating value, rather than waiting for the
  // hour to be over.
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

String INA226_ADC::getDeviceNameSuffix() const { return deviceNameSuffix; }
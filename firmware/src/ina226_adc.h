// ina_226_adc.h:
#ifndef INA226_ADC_H
#define INA226_ADC_H

#include <INA226_WE.h>
#include <Wire.h>
#include <Arduino.h>
#include <map>
#include <Preferences.h>
#include <vector>
#include "shared_defs.h"

enum DisconnectReason { NONE, LOW_VOLTAGE, OVERCURRENT, MANUAL };

struct CalPoint {
    float raw_mA;   // raw measured current from INA226 (mA)
    float true_mA;  // ground-truth current (mA)
};

class INA226_ADC {
public:
    static constexpr float MCU_IDLE_CURRENT_A = 0.05f;

    INA226_ADC(uint8_t address, float shuntResistorOhms, float batteryCapacityAh);
    void begin(int sdaPin, int sclPin);
    void readSensors();
    float getShuntVoltage_mV() const;
    float getBusVoltage_V() const;
    float getCurrent_mA() const;      // calibrated current (mA) using table when present, else linear
    float getRawCurrent_mA() const;   // raw measured current (mA) from INA226
    float getPower_mW() const;
    float getLoadVoltage_V() const;
    float getBatteryCapacity() const;
    void setBatteryCapacity(float capacity);
    void updateBatteryCapacity(float currentA); // current in A (positive = charge)
    bool isOverflow() const;
    bool clearCalibrationTable(uint16_t shuntRatedA);
    String getAveragedRunFlatTime(float currentA, float warningThresholdHours, bool &warningTriggered);
    String calculateRunFlatTimeFormatted(float currentA, float warningThresholdHours, bool &warningTriggered);

    void setSOC_percent(float percent);
    void setVoltageProtection(float cutoff, float reconnect_voltage);

    // New shunt resistance calibration methods
    bool saveShuntResistance(float resistance);
    bool loadShuntResistance();
    bool loadFactoryDefaultResistance(uint16_t shuntRatedA);
    bool getFactoryDefaultResistance(uint16_t shuntRatedA, float &outOhms) const;

    // Protection features
    void loadProtectionSettings();
    void saveProtectionSettings();
    void setProtectionSettings(float lv_cutoff, float hyst, float oc_thresh);
    void setActiveShunt(uint16_t shuntRatedA);
    uint16_t getActiveShunt() const;
    float getLowVoltageCutoff() const;
    float getHysteresis() const;
    float getOvercurrentThreshold() const;
    void setLowVoltageDelay(uint32_t delay_s);
    uint32_t getLowVoltageDelay() const;
    void setDeviceNameSuffix(String suffix);
    String getDeviceNameSuffix() const;
    void checkAndHandleProtection();
    void setLoadConnected(bool connected, DisconnectReason reason = MANUAL);
    bool isLoadConnected() const;
    void configureAlert(float amps);
    void setTempOvercurrentAlert(float amps);
    void restoreOvercurrentAlert();
    void handleAlert();
    void processAlert();
    bool isAlertTriggered() const;
    void clearAlerts();
    void enterSleepMode();
    bool isConfigured() const;
    void toggleHardwareAlerts();
    bool areHardwareAlertsDisabled() const;
    float getHardwareAlertThreshold_A() const;
    void dumpRegisters() const;

    float getCalibratedShuntResistance() const;

    // ---------- Linear calibration (legacy / fallback) ----------
    bool loadCalibration(uint16_t shuntRatedA);                          // apply stored linear (gain/offset)
    bool saveCalibration(uint16_t shuntRatedA, float gain, float offset_mA);
    void setCalibration(float gain, float offset_mA);
    void getCalibration(float &gainOut, float &offsetOut) const;
    bool getStoredCalibrationForShunt(uint16_t shuntRatedA, float &gainOut, float &offsetOut) const;

    // ---------- Table calibration (preferred) ----------
    // Save/load a piecewise calibration table for the given shunt
    bool saveCalibrationTable(uint16_t shuntRatedA, const std::vector<CalPoint> &points);
    bool loadCalibrationTable(uint16_t shuntRatedA);                     // loads into RAM; returns true if found
    const std::vector<CalPoint>& getCalibrationTable() const;
    bool hasCalibrationTable() const;                                    // RAM presence
    bool hasStoredCalibrationTable(uint16_t shuntRatedA, size_t &countOut) const;
    bool loadFactoryCalibrationTable(uint16_t shuntRatedA);

    // Energy usage tracking
    void updateEnergyUsage(float power_mW);
    float getLastHourEnergy_Wh() const;
    float getLastDayEnergy_Wh() const;
    float getLastWeekEnergy_Wh() const;

private:
    INA226_WE ina226;
    float defaultOhms;      // Original default shunt resistance
    float calibratedOhms;   // Calibrated shunt resistance
    float batteryCapacity;
    float maxBatteryCapacity;
    unsigned long lastUpdateTime;
    float shuntVoltage_mV, loadVoltage_V, busVoltage_V, current_mA, power_mW;
    float calibrationGain, calibrationOffset_mA;

    // Protection settings
    float lowVoltageCutoff;
    float hysteresis;
    float overcurrentThreshold;
    uint32_t lowVoltageDelayMs;
    unsigned long lowVoltageStartTime;
    String deviceNameSuffix;
    bool loadConnected;
    volatile bool alertTriggered;
    bool m_isConfigured;
    uint16_t m_activeShuntA;
    DisconnectReason m_disconnectReason;
    bool m_hardwareAlertsDisabled;

    // Table-based calibration
    std::vector<CalPoint> calibrationTable;
    float getCalibratedCurrent_mA(float raw_mA) const;

    void setInitialSOC();

    // Factory default resistances
    static const std::map<uint16_t, float> factory_shunt_resistances;
    static const std::map<float, float> soc_voltage_map;

    // run-flat time averaging
    const static int maxSamples = 10;
    float currentSamples[maxSamples];
    int sampleIndex;
    int sampleCount;
    unsigned long lastSampleTime;
    int sampleIntervalSeconds;

    void applyShuntConfiguration();

    // Energy usage tracking
    unsigned long lastEnergyUpdateTime;
    float currentHourEnergy_Ws;
    float currentDayEnergy_Ws;
    float currentWeekEnergy_Ws;
    float lastHourEnergy_Wh;
    float lastDayEnergy_Wh;
    float lastWeekEnergy_Wh;
    unsigned long currentHourStartMillis;
    unsigned long currentDayStartMillis;
    unsigned long currentWeekStartMillis;
};
#endif

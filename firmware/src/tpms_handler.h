#ifndef TPMS_HANDLER_H
#define TPMS_HANDLER_H

#include <Arduino.h>
#include <Preferences.h>
#include "shared_defs.h"

// Positions (FR, RR, RL, FL)
enum TPMSPosition { 
    TPMS_FR = 0, 
    TPMS_RR = 1, 
    TPMS_RL = 2, 
    TPMS_FL = 3, 
    TPMS_COUNT = 4 
};

static const char* TPMS_POSITION_SHORT[TPMS_COUNT] = {
    "FR", "RR", "RL", "FL"
};

// Data structure for a single TPMS sensor
struct TPMSSensor {
    uint8_t mac[6];          // BLE MAC address
    bool configured;         // Is this sensor slot active?
    float batteryVoltage;    // Battery voltage in V
    int temperature;         // Temperature in °C
    float pressurePsi;       // Pressure in PSI (Gauge)
    float baselinePsi;       // Baseline Pressure (Saved)
    unsigned long lastUpdate; // millis() relative to generic start
    
    TPMSSensor() : configured(false), batteryVoltage(0), 
                   temperature(0), pressurePsi(0), baselinePsi(0), lastUpdate(0) {
        memset(mac, 0, 6);
    }
};

class TPMSHandler {
public:
    TPMSHandler();
    
    void begin();
    void update(); // Main loop task
    
    // Configuration (Called when Config Packet received)
    // macs: 4x6 array
    // baselines: 4 floats
    // configured: 4 bools
    void setConfig(const uint8_t macs[4][6], const float baselines[4], const bool configured[4]);
    
    // Data Access (For Telemetry)
    const TPMSSensor* getSensor(int position) const;
    
    // Called by BLE scan callback
    void onSensorDiscovered(const uint8_t* mac, float voltage, int temp, float pressure);

    // Callback for scan complete (Trigger WiFi TX)
    typedef void (*ScanCompleteCallback)(void);
    void setScanCompleteCallback(ScanCompleteCallback cb) { scanCompleteCB = cb; }
    void stopScan(); // Force stop scanning

private:
    TPMSSensor sensors[TPMS_COUNT];
    ScanCompleteCallback scanCompleteCB = nullptr;
    
    // Scanning state
    bool scanActive;
    unsigned long lastScanTime;
    unsigned long scanStartTime;
    
    void loadFromNVS();
    void saveToNVS();
    
    void startScan();
    // No stopScan needed (async handles it)

    // Configuration
    static constexpr int SCAN_DURATION_S = 5;               // Scan duration in seconds (50% Duty Cycle)
    static constexpr unsigned long SCAN_INTERVAL_MS = 10000; // Interval (10s)
};

extern TPMSHandler tpmsHandler;

#endif // TPMS_HANDLER_H

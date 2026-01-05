#include "tpms_handler.h"
#include <NimBLEDevice.h>
#include <NimBLEUtils.h>
#include <NimBLEScan.h>
#include <NimBLEAdvertisedDevice.h>

TPMSHandler tpmsHandler;
static TPMSHandler* g_tpmsHandler = nullptr;

// Scan complete callback (Async)
static void scanCompleteCB(NimBLEScanResults results) {
    // Nothing to do, results handled in onResult
}

class TPMSAdvertisedDeviceCallbacks : public NimBLEAdvertisedDeviceCallbacks {
    void onResult(NimBLEAdvertisedDevice* advertisedDevice) override {
        if (!g_tpmsHandler) return;

        // 1. Check Name "BR"
        bool isTPMS = false;
        if (advertisedDevice->haveName() && advertisedDevice->getName() == "BR") isTPMS = true;
        
        // 2. Check Service UUID 0x27A5
        if (advertisedDevice->haveServiceUUID() && advertisedDevice->isAdvertisingService(NimBLEUUID((uint16_t)0x27A5))) isTPMS = true;

        if (!isTPMS) return;

        if (!advertisedDevice->haveManufacturerData()) return;
        std::string mfrData = advertisedDevice->getManufacturerData();
        const uint8_t* data = (const uint8_t*)mfrData.c_str();

        if (mfrData.length() < 5) return;

        // Parse: SS BB TT PPPP (Absolute 1/10 PSI)
        float voltage = (float)data[1] / 10.0f;
        int temperature = (int)data[2];
        uint16_t pressureRaw = ((uint16_t)data[3] << 8) | data[4];
        
        // Convert Raw (1/10 PSI Abs) to PSI Gauge
        float pressureAbsPsi = (float)pressureRaw / 10.0f;
        float pressurePsi = pressureAbsPsi - 14.7f; 
        if (pressurePsi < 0) pressurePsi = 0.0f;

        // Get MAC
        uint8_t mac[6];
        memcpy(mac, advertisedDevice->getAddress().getNative(), 6);

        g_tpmsHandler->onSensorDiscovered(mac, voltage, temperature, pressurePsi);
    }
};

static NimBLEScan* pBLEScan = nullptr;

TPMSHandler::TPMSHandler() : scanActive(false), lastScanTime(0), scanStartTime(0) {
    g_tpmsHandler = this;
}

void TPMSHandler::begin() {
    Serial.println("[TPMS] Initializing Shunt Scanner...");
    if (!NimBLEDevice::getInitialized()) {
        NimBLEDevice::init("AE-Shunt");
    }
    
    pBLEScan = NimBLEDevice::getScan();
    pBLEScan->setAdvertisedDeviceCallbacks(new TPMSAdvertisedDeviceCallbacks());
    pBLEScan->setActiveScan(true);
    pBLEScan->setInterval(100);
    pBLEScan->setWindow(99);
    
    loadFromNVS();
}

void TPMSHandler::update() {
    // Manage Scanning Cycle
    if (scanActive) {
        // Check if duration passed
        if (millis() - scanStartTime > (SCAN_DURATION_S * 1000 + 100)) {
             pBLEScan->clearResults();
             scanActive = false;
             Serial.println("[TPMS] Scan Cycle Complete");
             if (scanCompleteCB) scanCompleteCB();
        }
    } else {
        // Start next scan if interval passed
        if (millis() - lastScanTime > SCAN_INTERVAL_MS) {
            startScan();
        }
    }
}

void TPMSHandler::startScan() {
    if (scanActive) return;
    scanActive = true;
    lastScanTime = millis();
    scanStartTime = millis();
    pBLEScan->start(SCAN_DURATION_S, nullptr, false);
    Serial.println("[TPMS] Scan Started");
}

void TPMSHandler::stopScan() {
    if (scanActive && pBLEScan) {
        Serial.println("[TPMS] Forcing Scan Stop");
        pBLEScan->stop();
        pBLEScan->clearResults();
        scanActive = false;
    }
}

void TPMSHandler::onSensorDiscovered(const uint8_t* mac, float voltage, int temp, float pressure) {
    // Check if MAC matches any configured sensor
    for (int i = 0; i < TPMS_COUNT; i++) {
        if (sensors[i].configured && memcmp(sensors[i].mac, mac, 6) == 0) {
            sensors[i].pressurePsi = pressure;
            sensors[i].temperature = temp;
            sensors[i].batteryVoltage = voltage;
            sensors[i].lastUpdate = millis();
            Serial.printf("[TPMS] Update %s: %.1f PSI\n", TPMS_POSITION_SHORT[i], pressure);
            return;
        }
    }
}

void TPMSHandler::setConfig(const uint8_t macs[4][6], const float baselines[4], const bool configured[4]) {
    Serial.println("[TPMS] Received New Configuration");
    for (int i = 0; i < TPMS_COUNT; i++) {
        memcpy(sensors[i].mac, macs[i], 6);
        sensors[i].baselinePsi = baselines[i];
        sensors[i].configured = configured[i];
    }
    saveToNVS();
}

const TPMSSensor* TPMSHandler::getSensor(int position) const {
    if (position < 0 || position >= TPMS_COUNT) return nullptr;
    return &sensors[position];
}

void TPMSHandler::loadFromNVS() {
    Preferences prefs;
    prefs.begin("tpms", true);
    const char* keys[TPMS_COUNT] = {"tpms_fr", "tpms_rr", "tpms_rl", "tpms_fl"};
    const char* baseKeys[TPMS_COUNT] = {"base_fr", "base_rr", "base_rl", "base_fl"};
    
    for (int i = 0; i < TPMS_COUNT; i++) {
        String macStr = prefs.getString(keys[i], "");
        if (macStr.length() > 0) {
             // Hex string to bytes
             if (macStr.length() == 17) {
                 int m[6];
                 sscanf(macStr.c_str(), "%x:%x:%x:%x:%x:%x", &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]);
                 for(int k=0; k<6; k++) sensors[i].mac[k] = (uint8_t)m[k];
                 sensors[i].configured = true;
             }
        } else {
            sensors[i].configured = false;
        }
        sensors[i].baselinePsi = prefs.getFloat(baseKeys[i], 0.0f);
    }
    prefs.end();
}

void TPMSHandler::saveToNVS() {
    Preferences prefs;
    prefs.begin("tpms", false);
    const char* keys[TPMS_COUNT] = {"tpms_fr", "tpms_rr", "tpms_rl", "tpms_fl"};
    const char* baseKeys[TPMS_COUNT] = {"base_fr", "base_rr", "base_rl", "base_fl"};
    
    for (int i = 0; i < TPMS_COUNT; i++) {
        if (sensors[i].configured) {
            char buf[18];
            snprintf(buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x", 
                sensors[i].mac[0], sensors[i].mac[1], sensors[i].mac[2], 
                sensors[i].mac[3], sensors[i].mac[4], sensors[i].mac[5]);
            prefs.putString(keys[i], buf);
            prefs.putFloat(baseKeys[i], sensors[i].baselinePsi);
        } else {
            prefs.remove(keys[i]);
            prefs.remove(baseKeys[i]);
        }
    }
    prefs.end();
    Serial.println("[TPMS] Configuration Saved to NVS");
}

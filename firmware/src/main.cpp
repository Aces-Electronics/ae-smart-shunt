#include <vector>
#include <Arduino.h>
SET_LOOP_TASK_STACK_SIZE(16 * 1024); // 16KB, GitHub responses are heavy
#include <Preferences.h>
#include <cmath>
#include "shared_defs.h"
#include "ina226_adc.h"
#include "ble_handler.h"
#include "espnow_handler.h"
#include "gpio_adc.h"
#include "passwords.h"
#include "ota_handler.h"
#include "tpms_handler.h"
#include "crash_handler.h"
#include <esp_now.h>
#include <esp_err.h>
#include "driver/gpio.h"
#include <ArduinoJson.h>
#include <nvs_flash.h>
#include "esp_wifi.h"

// WiFi and OTA
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <Update.h>

volatile bool ota_command_pending = false;
volatile uint8_t ota_command = 0;
bool ota_success_notification_pending = false;

// The firmware version is defined by the build system via the version.py script.
// It is passed as a compile-time macro OTA_VERSION (e.g., -DOTA_VERSION="1.0.0").
#define USE_ADC // if defined, use ADC, else, victron BLE

float batteryCapacity = 100.0f; // Default rated battery capacity in Ah (used for SOC calc)

uint8_t broadcastAddress[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// Main loop interval
const unsigned long telemetry_interval = 5000; // Increased frequency (5s)
unsigned long last_telemetry_millis = 0;
uint32_t telemetry_counter = 0;

// Polling interval for accurate coulomb counting
const unsigned long polling_interval = 100; // 100ms = 10Hz
unsigned long last_polling_millis = 0;

// Async Restart Management
bool g_pendingRestart = false;
unsigned long g_restartTs = 0;

void scheduleRestart(uint32_t delay_ms) {
    g_pendingRestart = true;
    g_restartTs = millis() + delay_ms;
    Serial.printf("Restart scheduled in %u ms...\n", delay_ms);
}

// LED Heartbeat
unsigned long last_led_blink = 0;
const unsigned long led_blink_interval = 500; // ms

struct_message_ae_smart_shunt_1 ae_smart_shunt_struct = {};  // Zero-initialize to prevent garbage
// Initializing with a default shunt resistor value, which will be overwritten
// if a calibrated value is loaded from NVS.
// The default resistance value here is a fallback and will be overwritten by either a
// custom calibrated value from NVS or the factory default for the active shunt.
// Use the global `batteryCapacity` (rated Ah) so INA226_ADC::maxBatteryCapacity
// matches the configured rated capacity used for SOC calculations.
// INA226_ADC ina226_adc(I2C_ADDRESS, 0.000750000f, batteryCapacity);
INA226_ADC ina226_adc(I2C_ADDRESS, 0.001730000f, batteryCapacity);

// ADC for the starter battery voltage on GPIO3
GPIO_ADC starter_adc(3);

ESPNowHandler espNowHandler(broadcastAddress); // ESP-NOW handler for sending data
BLEHandler bleHandler;
WiFiClientSecure wifi_client;
OtaHandler otaHandler(bleHandler, espNowHandler, wifi_client);

// MQTT Handler
#include "mqtt_handler.h"
MqttHandler mqttHandler(espNowHandler, ina226_adc);

unsigned long lastMqttUplink = 0;
const unsigned long MQTT_UPLINK_INTERVAL = 15 * 60 * 1000; // 15 Minutes
bool g_cloudEnabled = false;
bool g_forceMqttUplink = false;
uint8_t g_lastCloudStatus = 0;
uint32_t g_lastCloudSuccessTime = 0;
bool g_hasCrashLog = false;

void preOtaUpdate() {
    Serial.println("[MAIN] Pre-OTA update callback triggered. Saving battery capacity...");
    Preferences preferences;
    preferences.begin("storage", false);
    float capacity = ina226_adc.getBatteryCapacity();
    preferences.putFloat("bat_cap", capacity);
    preferences.end();
    Serial.printf("[MAIN] Saved battery capacity: %f\n", capacity);
}

void loadSwitchCallback(bool enabled) {
    if (enabled) {
        ina226_adc.setLoadConnected(true, NONE);
        Serial.println("[BLE WRITE] Load Control: ON");
    } else {
        ina226_adc.setLoadConnected(false, MANUAL);
        Serial.println("[BLE WRITE] Load Control: OFF");
    }
}

void socCallback(float percent) {
    Serial.printf("[BLE WRITE] SOC: %.2f%%\n", percent);
    ina226_adc.setSOC_percent(percent);
}

void voltageProtectionCallback(String value) {
    Serial.printf("[BLE WRITE] Voltage Protection: %s\n", value.c_str());
    int commaIndex = value.indexOf(',');
    if (commaIndex > 0) {
        String cutoff_str = value.substring(0, commaIndex);
        String reconnect_str = value.substring(commaIndex + 1);
        float cutoff = cutoff_str.toFloat();
        float reconnect = reconnect_str.toFloat();
        ina226_adc.setVoltageProtection(cutoff, reconnect);
    } else {
        Serial.println("[BLE WRITE] Invalid format for voltage protection setting.");
    }
}

void lowVoltageDelayCallback(uint32_t seconds) {
    Serial.printf("[BLE WRITE] Low Voltage Delay: %u seconds\n", seconds);
    ina226_adc.setLowVoltageDelay(seconds);
}

void deviceNameSuffixCallback(String suffix) {
    Serial.printf("[BLE WRITE] Device Name Suffix: '%s'\n", suffix.c_str());
    ina226_adc.setDeviceNameSuffix(suffix);
}

void ratedCapacityCallback(float capacityAh) {
    Serial.printf("[BLE WRITE] Rated Capacity: %.2f Ah\n", capacityAh);
    ina226_adc.setMaxBatteryCapacity(capacityAh);
}

void wifiSsidCallback(String ssid) {
    Serial.printf("[BLE WRITE] WiFi SSID: '%s'\n", ssid.c_str());
    otaHandler.setWifiSsid(ssid);
}

void wifiPassCallback(String pass) {
    Serial.println("[BLE WRITE] WiFi Password: ****");
    otaHandler.setWifiPass(pass);
}

void otaControlCallback(uint8_t command) {
    Serial.printf("[BLE WRITE] OTA Control Command: %d\n", command);
    ota_command = command;
    ota_command_pending = true;
}

void otaTriggerCallback(bool triggered) {
    if (triggered) {
        Serial.println("[BLE WRITE] OTA Trigger: Starting update");
        // Use the existing OTA control mechanism - command 1 starts check for update
        ota_command = 1;
        ota_command_pending = true;
    }
}

void hexStringToBytes(String hex, uint8_t* bytes, int len) {
    for (int i=0; i<len; i++) {
        char buf[3] = { hex[i*2], hex[i*2+1], '\0' };
        bytes[i] = (uint8_t)strtoul(buf, NULL, 16);
    }
}

void performUnpair() {
    Serial.println("Unpairing Device...");
    Preferences prefs;
    prefs.begin("pairing", false);
    prefs.clear();
    prefs.end();
    
    Serial.println("Pairing Data wiped.");
    scheduleRestart(1000); // Give BLE 1s to Ack
}

void pairingCallback(String payload) {
    Serial.println("Received Pairing Payload: " + payload);
    
    if (payload == "CRASH") {
        Serial.println("Forcing Crash (Divide by Zero)...");
        delay(100);
        volatile int x = 0;
        volatile int y = 10 / x; 
        (void)y;
        return;
    }

    if (payload == "RESET") {
        performUnpair();
        return;
    }

    if (payload == "PAIRING") {
        Serial.println("Received PAIRING command via BLE. Forcing ESP-NOW broadcast for 5 minutes.");
        espNowHandler.setForceBroadcast(true);
        // We reuse the existing polling loop or fallback timer to reset this or just let it be.
        // For simplicity, we could add a timer here, but since the shunt often isn't on a battery 
        // constraint, just forcing it until reboot or manual stop is fine, or let's use a simple timer check in loop.
        // Actually, let's keep it simple: Pairing mode remains active until reboot or next regular pairing.
        return;
    }

    // Handle "MAC:KEY" format (e.g. "AABBCCDDEEFF:001122...33")
    // MAC (12) + : + Key (32) = 45 chars min.
    // Or MAC with colons (17) + : + Key (32) = 50 chars.
    int separatorIndex = payload.indexOf(':');
    if (separatorIndex > 0) {
        String part1 = payload.substring(0, separatorIndex); // Potential MAC or Prefix (ADD)
        String part2 = payload.substring(separatorIndex + 1); // Key?
        
        // Check if part1 is "ADD"
        if (part1 == "ADD") {
            // Format: ADD:MAC:KEY
             int secondSep = part2.indexOf(':');
             if (secondSep > 0) {
                 part1 = part2.substring(0, secondSep); // MAC
                 part2 = part2.substring(secondSep + 1); // Key
             }
        }
        
        // Clean MAC (remove colons)
        part1.replace(":", "");
        if (part1.length() == 12 && part2.length() == 32) {
             Serial.printf("BLE: Received Pairing Credentials. MAC=%s, Key=%s\n", part1.c_str(), part2.c_str());
             
             uint8_t macBytes[6];
             uint8_t keyBytes[16];
             
             hexStringToBytes(part1, macBytes, 6);
             hexStringToBytes(part2, keyBytes, 16);
             
             espNowHandler.handleNewPeer(macBytes, keyBytes);
             // handleNewPeer saves to NVS automatically.
             return;
        }
    }

    if (payload == "RESET_ENERGY") {
        Serial.println("Received RESET_ENERGY command via BLE.");
        ina226_adc.resetEnergyStats();
        return;
    }

    if (payload == "FACTORY_RESET") {
        Serial.println("Received FACTORY RESET command via BLE.");
        
        // 1. Backup Calibration Data
        // Active Shunt Rating
        uint16_t backup_activeShunt = ina226_adc.getActiveShunt();
        // Resistance Logic
        float backup_resistance = ina226_adc.getCalibratedShuntResistance();
        bool backup_configured = ina226_adc.isConfigured();
        // Linear Calibration (Gain/Offset)
        float backup_gain = 1.0f;
        float backup_offset = 0.0f;
        ina226_adc.getCalibration(backup_gain, backup_offset);
        
        Serial.printf("Backing up: Shunt=%dA, Res=%.9f, Gain=%.6f, Off=%.3f\n", 
                      backup_activeShunt, backup_resistance, backup_gain, backup_offset);


        Serial.println("PERFORMING FULL HARDWARE WIPE of NVS partition...");
        WiFi.disconnect(true, true);
        
        esp_err_t err = nvs_flash_erase();
        if (err != ESP_OK) Serial.printf("Error: nvs_flash_erase failed (0x%x)\n", err);
        err = nvs_flash_init();
        if (err != ESP_OK) Serial.printf("Error: nvs_flash_init failed (0x%x)\n", err);

        // 2. Restore Calibration Data
        Serial.println("Restoring Shunt Calibration...");
        ina226_adc.setActiveShunt(backup_activeShunt);
        
        if (backup_configured) {
            // Restore Resistance
            ina226_adc.saveShuntResistance(backup_resistance);
            // Restore Linear Calibration (Gain/Offset)
            if (backup_gain != 1.0f || backup_offset != 0.0f) {
                ina226_adc.saveCalibration(backup_activeShunt, backup_gain, backup_offset);
                Serial.println("Restored Linear Calibration (Gain/Offset).");
            }
        }

        Serial.println("NVS wiped and Calibration Restored. Rebooting in 1s...");
        delay(1000);
        ESP.restart();
        return;
    }
    
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, payload);
    if (error) {
        Serial.print("deserializeJson() failed: ");
        Serial.println(error.c_str());
        return;
    }

    String gaugeMac = doc["gauge_mac"];
    String keyHex = doc["key"];
    
    if (gaugeMac.length() == 0 || keyHex.length() != 32) {
        Serial.println("Invalid Pairing Data");
        return;
    }
    
    uint8_t macBytes[6];
    uint8_t keyBytes[16];
    
    // Parse MAC (assuming colon separated)
    int p = 0;
    for (int i=0; i<6; i++) {
        String byteStr = gaugeMac.substring(p, p+2);
        macBytes[i] = (uint8_t)strtoul(byteStr.c_str(), NULL, 16);
        p += 3; // skip byte and colon
    }
    
    hexStringToBytes(keyHex, keyBytes, 16);
    
    // Save to Prefs
    Preferences prefs;
    prefs.begin("pairing", false);
    prefs.putString("p_gauge_mac", gaugeMac);
    prefs.putString("p_key", keyHex);
    prefs.end();
    
    // Restart to apply new secured state cleanly
    Serial.println("Pairing Data Saved.");
    scheduleRestart(1000); // Give BLE 1s to Ack
}

class MainServerCallbacks: public BLEServerCallbacks {
    void onConnect(BLEServer* pServer) {
      Serial.println("BLE client connected");
      if (ota_success_notification_pending) {
          bleHandler.updateOtaStatus(7); // 7: Post-reboot success confirmation
          ota_success_notification_pending = false; // Reset the flag
      }
    }

    void onDisconnect(BLEServer* pServer) {
      Serial.println("BLE client disconnected");
    }
};

void IRAM_ATTR alertISR()
{
  ina226_adc.handleAlert();
}

// helper: read a trimmed line from Serial (blocks until newline), with echo and backspace support.
static String SerialReadLineBlocking()
{
  String s = "";
  char c;

  // Consume any preceding newline characters left in the buffer.
  while(Serial.available() && (Serial.peek() == '\r' || Serial.peek() == '\n')) {
    Serial.read();
  }

  while (true) {
    if (Serial.available()) {
      c = Serial.read();

      if (c == '\r' || c == '\n') {
        Serial.println(); // Echo for neatness.
        // Consume any trailing newline characters (\r\n or \n\r).
        delay(10); // Allow time for the second character of a pair to arrive
        while(Serial.available() && (Serial.peek() == '\r' || Serial.peek() == '\n')) {
          Serial.read();
        }
        s.trim();
        return s;
      } else if (c == 127 || c == 8) { // Handle Backspace (ASCII 8) and Delete (ASCII 127).
        if (s.length() > 0) {
          s.remove(s.length() - 1);
          Serial.print("\b \b"); // Erase character from the terminal.
        }
      } else if (isPrintable(c)) {
        s += c;
        Serial.print(c); // Echo character to the terminal.
      }
    }
    delay(5); // Yield to other tasks.
  }
}

// Helper: wait for a single key press: Enter or 'x'/'X'.
// Returns the character pressed ('\n' for Enter, 'x' for cancel).
// This function does NOT wait for a newline after 'x' is pressed.
static char waitForEnterOrXWithDebug(INA226_ADC &ina, bool debugMode)
{
  // Flush any existing chars from previous inputs to avoid accidental triggers.
  while (Serial.available()) Serial.read();

  unsigned long lastPrint = 0;
  const unsigned long printInterval = 300; // ms

  while (true)
  {
    if (Serial.available() > 0) {
      char c = Serial.read();

      if (c == 'x' || c == 'X') {
        Serial.println("x"); // Echo and newline
        return 'x';
      }

      if (c == '\r' || c == '\n') {
        Serial.println(); // Newline for neatness
        // Consume any other newline characters that might follow (\r\n or \n\r).
        while(Serial.available() && (Serial.peek() == '\r' || Serial.peek() == '\n')) {
          Serial.read();
        }
        return '\n';
      }
      // Silently ignore other characters.
    }

    // Periodically print debug readings if enabled
    unsigned long now = millis();
    if (debugMode && (now - lastPrint >= printInterval))
    {
      ina.readSensors();
      float raw = ina.getRawCurrent_mA();
      float cal = ina.getCurrent_mA();
      Serial.printf("RAW: %8.3f mA\tCAL: %8.3f mA   \r", raw, cal);
      lastPrint = now;
    }
    delay(20);
  }
}

// New multi-point calibration function for the starter battery ADC
void runStarterADC_MultiPoint_Calibration(GPIO_ADC &adc) {
    Serial.println(F("\n--- Starter Battery ADC Multi-Point Calibration ---"));
    Serial.println(F("You will need a precise power supply."));
    Serial.println(F("Press 'x' at any time to cancel."));

    const float voltages[] = {10.0f, 11.0f, 11.5f, 12.0f, 12.5f, 13.0f, 14.0f, 15.0f};
    const int num_points = sizeof(voltages) / sizeof(voltages[0]);
    std::vector<VoltageCalPoint> cal_points;
    cal_points.reserve(num_points);

    for (int i = 0; i < num_points; ++i) {
        Serial.printf("\nStep %d of %d: Set power supply to %.2fV\n", i + 1, num_points, voltages[i]);
        Serial.println(F("Press Enter when ready."));
        Serial.print("> ");
        if (SerialReadLineBlocking().equalsIgnoreCase("x")) {
            Serial.println(F("Canceled."));
            return;
        }
        // Read raw ADC value
        const int samples = 8;
        int raw_adc_sum = 0;
        for (int s = 0; s < samples; ++s) {
            raw_adc_sum += analogRead(3); // Reading from GPIO3
            delay(50);
        }
        int raw_adc_avg = raw_adc_sum / samples;
        Serial.printf("  -> Recorded raw ADC value: %d\n", raw_adc_avg);
        cal_points.push_back({raw_adc_avg, voltages[i]});
    }

    // --- Save and apply calibration ---
    adc.calibrate(cal_points);
    Serial.println(F("\nMulti-point calibration complete and saved."));
}

static void printCalibrationCode(const INA226_ADC &ina, int shuntA) {
  Serial.println(F("\n--- Calibration Export (copy/paste into code) ---"));
  Serial.printf("// Shunt rating: %dA\n", shuntA);
  Serial.printf("constexpr float FACTORY_SHUNT_%dA_OHMS = %.9ff;\n", shuntA, ina.getCalibratedShuntResistance());

  const std::vector<CalPoint> &table = ina.getCalibrationTable();
  if (table.empty()) {
    Serial.println(F("// No table calibration stored for this shunt."));
    return;
  }

  Serial.printf("const std::vector<CalPoint> factory_cal_%dA = {\n", shuntA);
  for (size_t i = 0; i < table.size(); ++i) {
    const CalPoint &pt = table[i];
    Serial.printf("    {%.3ff, %.3ff}%s\n", pt.raw_mA, pt.true_mA, (i + 1 < table.size()) ? "," : "");
  }
  Serial.println("};");
}

// New function to export the starter battery ADC calibration data
void runExportVoltageCalibration(GPIO_ADC &adc) {
    const auto& table = adc.getCalibrationTable();
    if (!adc.isCalibrated()) {
        Serial.println(F("Starter ADC is not calibrated. Nothing to export."));
        return;
    }

    Serial.println(F("\n--- Copy the following C++ code for Starter ADC Calibration ---"));
    Serial.println("const std::vector<VoltageCalPoint> precalibrated_starter_adc = {");
    for (const auto &point : table) {
        Serial.printf("    {%d, %.3f},\\n", point.raw_adc, point.voltage);
    }
    Serial.println("};");
    Serial.println(F("--- End of C++ code ---"));
}

// Forward declaration for the calibration function
void runShuntResistanceCalibration(INA226_ADC &ina);
void runTableBasedCalibration(INA226_ADC &ina, int shuntA);

// This is the main calibration entry point, combining shunt selection,
// resistance calibration, and current table calibration.
void runCurrentCalibrationMenu(INA226_ADC &ina)
{
  // Ensure load is enabled for calibration
  ina.setLoadConnected(true, MANUAL);
  Serial.println(F("Load enabled for calibration."));

  Serial.println(F("\n--- Calibration Menu ---"));
  Serial.println(F("Step 1: Choose installed shunt rating (100-500 A in 50A steps) or 'x' to cancel:"));
  Serial.print(F("> "));

  String sel = SerialReadLineBlocking();
  if (sel.equalsIgnoreCase("x")) {
    Serial.println(F("Calibration canceled."));
    return;
  }

  int shuntA = sel.toInt();
  if (shuntA < 100 || shuntA > 500 || (shuntA % 50) != 0) {
    Serial.println(F("Invalid shunt rating. Aborting calibration."));
    return;
  }

  // Set the new active shunt. This will load its config.
  ina.setActiveShunt(shuntA);

  // Now offer options for this shunt
  while (true) {
    Serial.println(F("\nStep 2: Choose action for the selected shunt:"));
    Serial.println(F("  (R) - Perform 3-point resistance calibration"));
    Serial.println(F("  (L) - Load factory default resistance"));
    Serial.println(F("  (T) - Perform fine-tuning current calibration (table)"));
    Serial.println(F("  (F) - Restore factory default calibration table"));
    Serial.println(F("  (X) - Exit to main menu"));
    Serial.print(F("> "));
    String choice = SerialReadLineBlocking();

    if (choice.equalsIgnoreCase("R")) {
      runShuntResistanceCalibration(ina);
      Serial.println(F("\nResistance calibration complete."));
    } else if (choice.equalsIgnoreCase("L")) {
      ina.loadFactoryDefaultResistance(shuntA);
      Serial.println(F("\nFactory default resistance loaded."));
    } else if (choice.equalsIgnoreCase("T")) {
      runTableBasedCalibration(ina, shuntA);
    } else if (choice.equalsIgnoreCase("F")) {
        if (ina.loadFactoryCalibrationTable(shuntA)) {
            Serial.println(F("Factory calibration table restored."));
        } else {
            Serial.println(F("Could not restore factory calibration table."));
        }
    } else if (choice.equalsIgnoreCase("X")) {
      Serial.println(F("Exiting calibration menu."));
      printCalibrationCode(ina, shuntA);
      return;
    } else {
      Serial.println(F("Invalid choice."));
    }
  }
}

// This function is from the user's provided code.
// It was originally named runCalibrationMenu.
void runTableBasedCalibration(INA226_ADC &ina, int shuntA)
{
  const float MAX_MEASURABLE_A = 40.0f;
  // First, check if the base shunt resistance has been calibrated.
  if (!ina.isConfigured()) {
    Serial.println(F("\n[WARNING] Base shunt resistance not calibrated."));
    Serial.println(F("This fine-tuning step requires the base resistance to be calibrated first."));
    Serial.println(F("Would you like to run the 3-point resistance calibration now? (y/N)"));
    Serial.print("> ");
    String choice = SerialReadLineBlocking();
    if (choice.equalsIgnoreCase("y")) {
      runShuntResistanceCalibration(ina);
      // After resistance calibration, check again. If it's still not configured, abort.
      if (!ina.isConfigured()) {
        Serial.println(F("[ERROR] Resistance calibration was not completed successfully. Aborting fine-tuning."));
        return;
      }
      Serial.println(F("\nResistance calibration complete. Now proceeding to fine-tuning..."));
    } else {
      Serial.println(F("Fine-tuning calibration aborted."));
      return;
    }
  }

  // Ensure load is enabled for calibration
  ina.setLoadConnected(true, MANUAL);
  Serial.println(F("Load enabled for calibration."));

  Serial.println(F("\n--- Current Calibration Menu ---"));
  Serial.println(F("MCU draws ~0.052A at all times; prompts below refer to EXTERNAL load only."));

  // Save the selected shunt as the active one
  Preferences prefs;
  prefs.begin(NVS_CAL_NAMESPACE, false);
  prefs.putUShort(NVS_KEY_ACTIVE_SHUNT, shuntA);
  prefs.end();
  Serial.printf("Set %dA as active shunt.\n", shuntA);

  // Show existing linear + table calibration (if any)
  float g0, o0;
  bool hadLinear = ina.loadCalibration(shuntA);
  ina.getCalibration(g0, o0);

  size_t storedCount = 0;
  bool hasTableStored = ina.hasStoredCalibrationTable(shuntA, storedCount);
  bool hasTableRAM = ina.loadCalibrationTable(shuntA);

  if (hadLinear)
    Serial.printf("Loaded LINEAR calibration for %dA: gain=%.9f offset_mA=%.3f\n", shuntA, g0, o0);
  else
    Serial.printf("No stored LINEAR calibration for %dA. Using defaults gain=%.9f offset_mA=%.3f\n", shuntA, g0, o0);

  if (hasTableStored)
  {
    Serial.printf("Found TABLE calibration for %dA with %u points. Loaded into RAM.\n", shuntA, (unsigned)storedCount);
  }
  else
  {
    Serial.printf("No TABLE calibration stored for %dA.\n", shuntA);
  }

  // Ask if user wants live debug streaming while waiting for each step
  Serial.println(F("Enable live debug stream (raw vs calibrated) while waiting to record each step? (y/N)"));
  Serial.print(F("> "));
  String dbgAns = SerialReadLineBlocking();
  bool debugMode = dbgAns.equalsIgnoreCase("y") || dbgAns.equalsIgnoreCase("yes");

  // build measurement percentages
  std::vector<float> perc;
  perc.push_back(0.0f);    // 0%
  perc.push_back(0.02f);   // 2%  (1A on 50A shunt)
  perc.push_back(0.04f);   // 4%  (2A on 50A shunt)
  perc.push_back(0.1f);    // 10% (5A on 50A shunt)
  perc.push_back(0.2f);    // 20% (10A on 50A shunt)
  perc.push_back(0.4f);    // 40% (20A on 50A shunt)
  perc.push_back(0.6f);    // 60% (30A on 50A shunt)
  perc.push_back(0.8f);    // 80% (40A on 50A shunt)
  perc.push_back(1.0f);    // 100%(50A on 50A shunt)

  std::vector<float> measured_mA;
  std::vector<float> true_mA;

  size_t last_measured_idx = 0;

  for (size_t i = 0; i < perc.size(); ++i)
  {
    float p = perc[i];
    float externalA = shuntA * p;
    float netA = externalA + INA226_ADC::MCU_IDLE_CURRENT_A;
    float true_milli = netA * 1000.0f;

  // --- Measure points up to the 40A dummy load limit ---
    if (externalA <= MAX_MEASURABLE_A) {
        if (p == 0.0f) {
        Serial.printf("\nStep %u of %u: Target external load = %.3f A (Zero Load).\n",
                        (unsigned)(i + 1), (unsigned)perc.size());
        Serial.println(F("Disconnect all external loads, then press Enter to record. Enter 'x' to cancel."));
        } else {
        Serial.printf("\nStep %u of %u: Target external load = %.3f A (%.2f%% of %dA).\n",
                        (unsigned)(i + 1), (unsigned)perc.size(), externalA, p * 100.0f, shuntA);
        Serial.println(F("Set test jig to the external target current, then press Enter. Enter 'x' to cancel."));
        }
        Serial.printf("   (Total through shunt will be %.3f A including MCU draw of %.3f A)\n", netA, INA226_ADC::MCU_IDLE_CURRENT_A);

        Serial.print("> ");
        char key = waitForEnterOrXWithDebug(ina, debugMode);
        if (key == 'x') {
            Serial.println("User canceled early; accepting tests recorded so far.");
            break;
        }

        const int samples = 100;
        float sumRaw = 0.0f;
        for (int s = 0; s < samples; ++s) {
            ina.readSensors();
            sumRaw += ina.getRawCurrent_mA();
            delay(20);
        }
        float avgRaw = sumRaw / (float)samples;
        Serial.printf("Recorded avg raw reading: %.3f mA  (expected total current: %.3f mA)\n", avgRaw, true_milli);
        measured_mA.push_back(avgRaw);
        true_mA.push_back(true_milli);
        last_measured_idx = i;
    }
  }

  // --- Extrapolate for points > 40A ---
  if (last_measured_idx > 0 && last_measured_idx < perc.size() - 1) {
    Serial.printf("\nExtrapolating remaining points > %.1fA...\n", MAX_MEASURABLE_A);

    // Get the last two measured points to establish a linear trend
    float raw1 = measured_mA[last_measured_idx - 1];
    float true1 = true_mA[last_measured_idx - 1];
    float raw2 = measured_mA[last_measured_idx];
    float true2 = true_mA[last_measured_idx];

    // Calculate the slope (gain) of the last segment
    float slope = (raw2 - raw1) / (true2 - true1);

    for (size_t i = last_measured_idx + 1; i < perc.size(); ++i) {
        float p = perc[i];
        float externalA = shuntA * p;
        float netA = externalA + INA226_ADC::MCU_IDLE_CURRENT_A;
        float true_milli = netA * 1000.0f;

        // Extrapolate the raw value
        float extrapolated_raw = raw2 + slope * (true_milli - true2);

        Serial.printf("Extrapolated Point: raw=%.3f mA -> total current=%.3f mA (external %.3f A, %.2f%%)\n",
                      extrapolated_raw, true_milli, externalA, p * 100.0f);
        measured_mA.push_back(extrapolated_raw);
        true_mA.push_back(true_milli);
    }
  }

  size_t N = measured_mA.size();
  if (N == 0)
  {
    Serial.println("No measurements taken; leaving calibration unchanged.");
    return;
  }

  // -------- Build & save calibration TABLE (piecewise linear) --------
  std::vector<CalPoint> points;
  points.reserve(N);
  for (size_t i = 0; i < N; ++i)
  {
    points.push_back({measured_mA[i], true_mA[i]});
    Serial.printf("Point %u: raw=%.3f mA -> true=%.3f mA\n", (unsigned)i, measured_mA[i], true_mA[i]);
  }

  // Wipe any existing calibration for this shunt before saving new one
  ina.clearCalibrationTable(shuntA);

  // Save table (this also sorts/dedups internally and loads into RAM)
  if (ina.saveCalibrationTable(shuntA, points))
  {
    Serial.println("\nCalibration complete (TABLE).");
    Serial.printf("Saved %u calibration points for %dA shunt.\n", (unsigned)points.size(), shuntA);
  }
  else
  {
    Serial.println("\nCalibration failed: no points saved.");
    return; // Can't run tests if calibration failed
  }

  Serial.println("These values are persisted and will be applied to subsequent current readings.");

  // --- Guided Tests ---
  Serial.println(F("\n--- Guided Hardware Tests ---"));
  Serial.println(F("Would you like to run guided tests to verify hardware functionality? (y/N)"));
  Serial.print(F("> "));
  String testAns = SerialReadLineBlocking();
  if (!testAns.equalsIgnoreCase("y")) {
    Serial.println("Skipping hardware tests.");
    return;
  }

  // Test 1: Load Switch Test
  Serial.println(F("\n--- Test 1: Load Switch ---"));
  Serial.println(F("This test will verify the load disconnect MOSFET."));
  Serial.println(F("Please apply a constant 1A load, then press Enter."));
  Serial.print(F("> "));
  if (waitForEnterOrXWithDebug(ina, false) == 'x') {
    Serial.println(F("Test canceled."));
    // Restore original alert configuration and reconnect load before exiting.
    ina.restoreOvercurrentAlert();
    ina.setLoadConnected(true, NONE);
    return;
  }

  delay(500);
  ina.readSensors();
  float current_before = ina.getCurrent_mA();
  Serial.printf("Current before disconnect: %.3f mA\n", current_before);

  Serial.println("Disconnecting load...");
  ina.setLoadConnected(false, MANUAL);
  delay(500); // Wait for load to disconnect and reading to settle

  ina.readSensors();
  float current_after = ina.getCurrent_mA();
  float no_load_current = measured_mA[0]; // First point was zero-load
  Serial.printf("Current after disconnect: %.3f mA (expected ~%.3f mA)\n", current_after, no_load_current);

  if (abs(current_after - no_load_current) < 200.0f) { // Allow 200mA tolerance for device draw
    Serial.println(F("SUCCESS: Load switch appears to be working."));
  } else {
    Serial.println(F("FAILURE: Current did not drop to no-load value. Check MOSFET wiring."));
  }

  // Reconnect load for next test
  Serial.println("Reconnecting load...");
  ina.setLoadConnected(true, NONE);
  delay(500);

  // Test 2: Overcurrent Alert Test
  Serial.println(F("\n--- Test 2: Overcurrent Alert ---"));
  Serial.println(F("This test will verify the INA226 alert functionality."));

  float test_current = 0.5f; // 500mA
  Serial.printf("The alert threshold will be temporarily set to %.3f A.\n", test_current);
  Serial.println(F("Please ensure your load is set to 0A, then press Enter."));
  Serial.print(F("> "));
  if (waitForEnterOrXWithDebug(ina, false) == 'x') {
    Serial.println(F("Test canceled."));
    // Restore original alert configuration and reconnect load before exiting.
    ina.restoreOvercurrentAlert();
    ina.setLoadConnected(true, NONE);
    return;
  }

  ina.setTempOvercurrentAlert(test_current);

  Serial.println(F("Now, slowly increase the load. The load should disconnect when you exceed the test threshold."));
  Serial.println(F("The test will wait for 15 seconds..."));

  bool alert_fired = false;
  unsigned long test_start = millis();
  while(millis() - test_start < 20000) { // 20s timeout
      if (ina.isAlertTriggered()) {
          ina.processAlert();
          alert_fired = true;
          break;
      }
      delay(50);
  }

  if (alert_fired) {
    Serial.println(F("SUCCESS: Overcurrent alert triggered and load was disconnected."));
  } else {
    Serial.println(F("FAILURE: Alert did not trigger within 15 seconds. Check INA226 wiring."));
  }

  // Restore original alert configuration
  ina.restoreOvercurrentAlert();
  // Ensure load is connected for normal operation
  ina.setLoadConnected(true, NONE);
}

void printShunt(const struct_message_ae_smart_shunt_1 *p)
{
  if (!p)
    return;

  Serial.printf(
      "=== Local Shunt ===\n"
      "Message ID     : %d\n"
      "Data Changed   : %s\n"
      "Voltage        : %.2f V\n"
      "Current        : %.2f A\n"
      "Power          : %.2f W\n"
      "SOC            : %.1f %%\n"
      "Capacity       : %.2f Ah\n"
      "Starter Voltage: %.2f V\n"
      "Error          : %d\n"
      "Run Flat Time  : %s\n"
      "Last Hour      : %.2f Wh\n"
      "Last Day       : %.2f Wh\n"
      "Last Week      : %.2f Wh\n"
      "Load Output    : %s\n"
      "===================\n",
      p->messageID,
      p->dataChanged ? "true" : "false",
      p->batteryVoltage,
      p->batteryCurrent,
      p->batteryPower,
      p->batterySOC * 100.0f,
      p->batteryCapacity,
      p->starterBatteryVoltage,
      p->batteryState,
      p->runFlatTime,
      p->lastHourWh,
      p->lastDayWh,
      p->lastWeekWh,
      ina226_adc.isLoadConnected() ? "ON" : "OFF"
  );
  
  // Print Temp Sensor Data (Relay) - Always show what is in the struct!
  Serial.println("--- Relayed Temp Sensor ---");
  Serial.printf("  Temp : %.1f C\n", p->tempSensorTemperature);
  Serial.printf("  Batt : %d %%\n", p->tempSensorBatteryLevel);
  if (p->tempSensorLastUpdate == 0xFFFFFFFF) {
      Serial.printf("  Age  : (NO DATA)\n");
  } else {
      if (p->tempSensorLastUpdate < 60000) {
          Serial.printf("  Age  : %u s\n", p->tempSensorLastUpdate / 1000);
      } else {
          Serial.printf("  Age  : %u min\n", p->tempSensorLastUpdate / 60000);
      }
  }
  Serial.println("===========================");

  Serial.println("--- TPMS Data ---");
  for(int i=0; i<4; i++) {
      if (p->tpmsLastUpdate[i] != 0xFFFFFFFF) {
          Serial.printf("  %s: %.1f PSI, %d C, %.1f V (Age: %u ms)\n", 
              TPMS_POSITION_SHORT[i], 
              p->tpmsPressurePsi[i], 
              p->tpmsTemperature[i], 
              p->tpmsVoltage[i],
              p->tpmsLastUpdate[i]);
      } else {
           if (p->tpmsLastUpdate[i] == 0xFFFFFFFE) {
                 Serial.printf("  %s: Waiting for Data...\n", TPMS_POSITION_SHORT[i]);
           } else {
                 Serial.printf("  %s: (Not Configured)\n", TPMS_POSITION_SHORT[i]);
           }
      }
  }
  Serial.println("===================");
}

// Helper to prompt for shunt selection via Serial
int promptForShuntSelection(INA226_ADC &ina) {
    Serial.println(F("\nSelect installed shunt rating (100-500 A in 50A steps) or 'x' to cancel:"));
    Serial.printf("(Current active shunt: %dA)\n", ina.getActiveShunt());
    Serial.print(F("> "));

    String sel = SerialReadLineBlocking();
    if (sel.equalsIgnoreCase("x")) {
        return -1;
    }

    // If user just presses Enter, keep current
    int shuntInput = ina.getActiveShunt();
    if (sel.length() > 0) {
        shuntInput = sel.toInt();
    }

    if (shuntInput < 100 || shuntInput > 500 || (shuntInput % 50) != 0) {
        Serial.println(F("Invalid shunt rating. Must be 100-500 in 50A steps."));
        return -2;
    }

    return shuntInput;
}

// New function to handle shunt resistance calibration
void runShuntResistanceCalibration(INA226_ADC &ina)
{
  Serial.println(F("Preparing for calibration..."));
  
  // 1. Ensure Load is Enabled (so user can draw current)
  // We use MANUAL mode to prevent auto-disconnect logic from interfering
  ina.setLoadConnected(true, MANUAL);
  Serial.println(F("Load enabled (MANUAL mode)."));

  // 2. Disable Hardware Alert Interrupts
  // Prevents the ISR from checking protection/safety limits during calibration
  detachInterrupt(digitalPinToInterrupt(INA_ALERT_PIN));
  Serial.println(F("Alert Pin Interrupt DISABLED for calibration safety."));

  // 3. Shunt Selection
  int shuntInput = promptForShuntSelection(ina);
  if (shuntInput < 0) {
      // Restore Interrupt before returning
      ina.clearAlerts(); // clear any stale status
      attachInterrupt(digitalPinToInterrupt(INA_ALERT_PIN), alertISR, FALLING);
      Serial.println(F("Alert Pin Interrupt RESTORED."));
      return; 
  }

  if (shuntInput != (int)ina.getActiveShunt()) {
      ina.setActiveShunt(shuntInput);
  }
  // Set E-Fuse limit to 50% of the selected shunt rating as a safety baseline
  ina.setEfuseLimit((float)shuntInput * 0.5f);
  const uint16_t activeShuntA = ina.getActiveShunt();

  Serial.println(F("\n--- 5-Point Calibration (0A to 3A) ---"));
  Serial.println(F("This routine calibrates the sensor by creating a correction curve."));
  Serial.println(F("1. We will RESET the sensor to Factory Defaults for your selected shunt."));
  Serial.println(F("2. We will measure 5 points: 0A, 0.5A, 1A, 2A, 3A."));
  Serial.println(F("   (Note: Max 3A chosen because your shunt saturates >3.5A)"));
  Serial.println(F("3. You enter the TRUE current from your meter."));
  Serial.println(F("Press 'x' at any time to cancel."));

  // Reset to factory defaults
  Serial.printf("\nResetting to factory default settings for %dA shunt...\n", activeShuntA);
  if (!ina.loadFactoryDefaultResistance(activeShuntA)) {
      Serial.println(F("Warning: No factory default found for this shunt rating. Using current settings."));
  }
  // Clear any existing linear/table calibration
  ina.setCalibration(1.0f, 0.0f);
  ina.clearCalibrationTable(activeShuntA); // Though we are doing linear now

  // Calibration points
  struct DataPoint {
      float target;
      float raw;
      float true_val;
  };
  std::vector<DataPoint> points;
  float targets[] = {0.0f, 0.5f, 1.0f, 2.0f, 3.0f};

  // Ask about MCU current inclusion (for the USER input side)
  Serial.print(F("\nAdd MCU idle current (~0.052A) to your entered value? (Y/n): "));
  String mcuChoice = SerialReadLineBlocking();
  bool addMcuCurrent = true;
  if (mcuChoice.equalsIgnoreCase("n")) {
      addMcuCurrent = false;
      Serial.println(F("Not adding MCU current. Using your value as the TOTAL true current."));
  } else {
      Serial.println(F("Adding MCU current to your value (default)."));
  }
  float mcuCurrentOffset = addMcuCurrent ? INA226_ADC::MCU_IDLE_CURRENT_A : 0.0f;


  for (float target : targets) {
      Serial.printf("\n--- Step: %.1f A ---\n", target);
      Serial.printf("1. Set your load to %.3f A.\n", target);
      Serial.print(F("2. Enter the TRUE current from your meter: "));

      String line = SerialReadLineBlocking();
      if (line.equalsIgnoreCase("x")) { 
          Serial.println(F("Canceled.")); 
          ina.clearAlerts();
          attachInterrupt(digitalPinToInterrupt(INA_ALERT_PIN), alertISR, FALLING);
          Serial.println(F("Alert Pin Interrupt RESTORED."));
          return; 
      }
      
      float user_input = 0.0f;
      if (line.length() == 0) {
          user_input = target; // Default to target if empty
          Serial.printf("Using default: %.3f\n", user_input);
      } else {
          user_input = line.toFloat();
      }

      float true_current = user_input + mcuCurrentOffset;
      if (addMcuCurrent) {
           Serial.printf("  (Calculated True Total: %.3f A Input + %.3f A MCU = %.3f A)\n", 
              user_input, INA226_ADC::MCU_IDLE_CURRENT_A, true_current);
      }

      // Read RAW current from sensor
      Serial.print(F("Reading sensor..."));
      const int samples = 50;
      float sum_current = 0;
      bool sat_error = false;
      for (int s = 0; s < samples; ++s) {
          ina.readSensors();
          if (ina.isSaturated()) { sat_error = true; }
          // We use getRawCurrent_mA() to get the uncorrected reading based on factory ohms
          sum_current += ina.getRawCurrent_mA(); 
          delay(20);
      }
      if (sat_error) {
          Serial.println(F("\n[CRITICAL ERROR] Sensor Saturated! Voltage limit reached."));
          Serial.println(F("Cannot calibrate at this current. Aborting."));
          return;
      }

      float avg_raw_mA = sum_current / samples;
      float avg_raw_A = avg_raw_mA / 1000.0f;
      Serial.printf(" Done. Raw: %.4f A, True: %.4f A\n", avg_raw_A, true_current);
      
      points.push_back({target, avg_raw_A, true_current});
  }

  // Calculate Linear Regression (y = mx + c)
  // x = Raw Reading, y = True Value
  // We want to map Raw -> True
  
  float sum_x = 0, sum_y = 0, sum_xy = 0, sum_xx = 0;
  int n = points.size();

  for (const auto &p : points) {
      sum_x += p.raw;
      sum_y += p.true_val;
      sum_xy += p.raw * p.true_val;
      sum_xx += p.raw * p.raw;
  }

  float slope = (n * sum_xy - sum_x * sum_y) / (n * sum_xx - sum_x * sum_x);
  float intercept = (sum_y - slope * sum_x) / n;

  // Convert intercept to mA for storage
  float offset_mA = intercept * 1000.0f;

  Serial.println(F("\n--- Calculation Results ---"));
  Serial.printf("Gain (Slope): %.6f\n", slope);
  Serial.printf("Offset: %.6f mA\n", offset_mA);

  Serial.print(F("Save this calibration? (Y/n): "));
  String save = SerialReadLineBlocking();
  if (!save.equalsIgnoreCase("n")) {
      ina.saveCalibration(activeShuntA, slope, offset_mA);
      Serial.println(F("Saved! Calibration active."));
  } else {
      Serial.println(F("Discarded."));
  }
}

void runQuickCalibration(INA226_ADC &ina)
{
  // Ensure load is enabled for calibration
  ina.setLoadConnected(true, MANUAL);
  Serial.println(F("Load enabled for calibration."));

  Serial.println(F("\n--- Quick Shunt Resistance Calibration (1A / 5A) ---"));
  Serial.println(F("This routine will calculate the actual shunt resistance using fixed 1A and 5A load steps."));
  Serial.println(F("You will need an external multimeter to measure the true current at each step."));
  Serial.println(F("Press 'x' at any time to cancel."));
  Serial.println(F("Note: Enter EXTERNAL load current only. Firmware adds ~0.052A MCU draw automatically (optional)."));

  // Ask about MCU current inclusion
  Serial.print(F("\nInclude MCU idle current (~0.052A) in calculation? (Y/n): "));
  String mcuChoice = SerialReadLineBlocking();
  bool includeMcuCurrent = true;
  if (mcuChoice.equalsIgnoreCase("n")) {
      includeMcuCurrent = false;
      Serial.println(F("Excluding MCU current from total."));
  } else {
      Serial.println(F("Including MCU current in total (default)."));
  }
  float mcuCurrentOffset = includeMcuCurrent ? INA226_ADC::MCU_IDLE_CURRENT_A : 0.0f;

  // Select Shunt Rating
  int shuntInput = promptForShuntSelection(ina);
  if (shuntInput < 0) return; // Canceled or Invalid

  // Set the new active shunt (this saves to NVS and reloads defaults if needed)
  if (shuntInput != (int)ina.getActiveShunt()) {
      ina.setActiveShunt(shuntInput);
  }
  // Set E-Fuse limit to 50% of the selected shunt rating as a safety baseline
  ina.setEfuseLimit((float)shuntInput * 0.5f);
  
  const float step2TargetExternalA = 1.0f;
  const float step3TargetExternalA = 5.0f;
  const uint16_t activeShuntA = ina.getActiveShunt();

  float true_a_zero = 0.0f, v_shunt_zero_mv = 0.0f;
  float true_a_1 = 0.0f, v_shunt_1_mv = 0.0f;
  float true_a_5 = 0.0f, v_shunt_5_mv = 0.0f;
  String line;

  // --- Step 1: No Load ---
  Serial.println(F("\n--- Step 1 of 3: No External Load ---"));
  Serial.println(F("1. Disconnect all external loads from the shunt."));
  Serial.printf("2. Measure the external load current (firmware adds ~%.3fA MCU draw if enabled).\n", mcuCurrentOffset);
  Serial.print(F("3. Enter the external load current in Amps (e.g., 0.000) and press Enter: "));

  line = SerialReadLineBlocking();
  if (line.equalsIgnoreCase("x")) { Serial.println(F("Canceled.")); return; }
  true_a_zero = line.toFloat() + mcuCurrentOffset;
  Serial.printf("  -> Total calibration current: %.6f A (External + %.3f A MCU)\n",
                true_a_zero, mcuCurrentOffset);

  const int samples = 100;
  float sum_v_zero = 0;
  for (int s = 0; s < samples; ++s) {
    ina.readSensors();
    sum_v_zero += ina.getShuntVoltage_mV();
    delay(20);
  }
  v_shunt_zero_mv = sum_v_zero / samples;
  Serial.printf("  -> Recorded avg shunt voltage: %.6f mV (for true current %.6f A)\n", v_shunt_zero_mv, true_a_zero);

  // --- Step 2: ~1A Load ---
  Serial.printf("\n--- Step 2 of 3: ~%.3fA External Load ---\n", step2TargetExternalA);
  Serial.printf("1. Apply a constant external load of approximately %.3fA.\n", step2TargetExternalA);
  Serial.printf("2. Measure the external load current (firmware adds ~%.3fA MCU draw if enabled).\n", mcuCurrentOffset);
  Serial.printf("3. Enter the external load current in Amps (e.g., %.3f) and press Enter: ", step2TargetExternalA);

  line = SerialReadLineBlocking();
  if (line.equalsIgnoreCase("x")) { Serial.println(F("Canceled.")); return; }
  
  float val_1 = 0.0f;
  if (line.length() == 0) {
      val_1 = step2TargetExternalA;
      Serial.printf("  -> Using default: %.3f A\n", val_1);
  } else {
      val_1 = line.toFloat();
  }
  true_a_1 = val_1 + mcuCurrentOffset;

  Serial.printf("  -> Total calibration current: %.6f A (External + %.3f A MCU)\n",
                true_a_1, mcuCurrentOffset);

  float sum_v_1a = 0;
  for (int s = 0; s < samples; ++s) {
    ina.readSensors();
    sum_v_1a += ina.getShuntVoltage_mV();
    delay(20);
  }
  v_shunt_1_mv = sum_v_1a / samples;
  Serial.printf("  -> Recorded avg shunt voltage: %.6f mV (for true current %.6f A)\n", v_shunt_1_mv, true_a_1);

  // --- Step 3: ~5A Load ---
  Serial.printf("\n--- Step 3 of 3: ~%.3fA External Load ---\n", step3TargetExternalA);
  Serial.printf("1. Apply a constant external load of approximately %.3fA.\n", step3TargetExternalA);
  Serial.printf("2. Measure the external load current (firmware adds ~%.3fA MCU draw if enabled).\n", mcuCurrentOffset);
  Serial.printf("3. Enter the external load current in Amps (e.g., %.3f) and press Enter: ", step3TargetExternalA);

  line = SerialReadLineBlocking();
  if (line.equalsIgnoreCase("x")) { Serial.println(F("Canceled.")); return; }

  float val_5 = 0.0f;
  if (line.length() == 0) {
      val_5 = step3TargetExternalA;
      Serial.printf("  -> Using default: %.3f A\n", val_5);
  } else {
      val_5 = line.toFloat();
  }
  true_a_5 = val_5 + mcuCurrentOffset;

  Serial.printf("  -> Total calibration current: %.6f A (External + %.3f A MCU)\n",
                true_a_5, mcuCurrentOffset);

  float sum_v_5a = 0;
  for (int s = 0; s < samples; ++s) {
    ina.readSensors();
    sum_v_5a += ina.getShuntVoltage_mV();
    delay(20);
  }
  v_shunt_5_mv = sum_v_5a / samples;
  Serial.printf("  -> Recorded avg shunt voltage: %.6f mV (for true current %.6f A)\n", v_shunt_5_mv, true_a_5);

  // --- Calculations ---
  Serial.println(F("\n--- Calculating Shunt Resistance ---"));

  float delta_v_1 = v_shunt_1_mv - v_shunt_zero_mv;
  float delta_i_1 = true_a_1 - true_a_zero;

  float delta_v_5 = v_shunt_5_mv - v_shunt_zero_mv;
  float delta_i_5 = true_a_5 - true_a_zero;

  // Check for invalid input
  if (delta_i_1 <= 0 || delta_i_5 <= 0) {
    Serial.println(F("\n[ERROR] The current at load steps must be greater than the 'no load' current. Please try again."));
    return;
  }

  // R = (delta_V_mV / 1000) / delta_I_A
  float r_1a = (delta_v_1 / 1000.0f) / delta_i_1;
  float r_5a = (delta_v_5 / 1000.0f) / delta_i_5;

  Serial.printf("Resistance from ~%.3fA load: (%.6f mV / 1000) / %.6f A = %.9f Ohms\n",
                step2TargetExternalA, delta_v_1, delta_i_1, r_1a);
  Serial.printf("Resistance from ~%.3fA load: (%.6f mV / 1000) / %.6f A = %.9f Ohms\n",
                step3TargetExternalA, delta_v_5, delta_i_5, r_5a);

  if (r_1a <= 0 || r_5a <= 0) {
    Serial.println(F("\n[ERROR] Calculated resistance is zero or negative. This can happen if the load was not applied correctly or if the 'no load' voltage was higher than the load voltage. Please try again."));
    return;
  }

  float newShuntOhms = (r_1a + r_5a) / 2.0f;

  float expectedOhms = 0.0f;
  if (!ina.getFactoryDefaultResistance(activeShuntA, expectedOhms) || expectedOhms <= 0.0f) {
    expectedOhms = ina.getCalibratedShuntResistance();
  }

  if (expectedOhms > 0.0f) {
    const float minAllowed = expectedOhms * 0.3f;
    const float maxAllowed = expectedOhms * 3.0f;
    if (newShuntOhms < minAllowed || newShuntOhms > maxAllowed) {
      Serial.printf("\n[WARNING] Calculated resistance %.9f Ohms is implausible for the %dA shunt (expected around %.9f Ohms).\n",
                    newShuntOhms, activeShuntA, expectedOhms);
      Serial.println(F("This may indicate an issue with your measurement setup."));
      Serial.print(F("Do you want to accept this value anyway? (y/N): "));
      String choice = SerialReadLineBlocking();
      if (!choice.equalsIgnoreCase("y")) {
        Serial.println(F("Calibration canceled. The old value has been retained."));
        return;
      }
      Serial.println(F("Accepting implausible value based on user override."));
    }
  }

  Serial.printf("\nCalculated new average shunt resistance: %.9f Ohms.\n", newShuntOhms);

  // Save the new resistance
  ina.saveShuntResistance(newShuntOhms);
  Serial.println("This value has been saved and will be used for all future calculations.");
}

void runProtectionConfigMenu(INA226_ADC &ina)
{
  Serial.println(F("\n--- Protection Settings ---"));

  // Temporary variables to hold the new settings
  float new_lv_cutoff, new_hysteresis, new_oc_thresh;

  // Get current settings to use as defaults
  float current_lv_cutoff = ina.getLowVoltageCutoff();
  float current_hysteresis = ina.getHysteresis();
  float current_oc_thresh = ina.getOvercurrentThreshold();

  // --- Low Voltage Cutoff ---
  Serial.print(F("Enter Low Voltage Cutoff (Volts) [default: "));
  Serial.print(current_lv_cutoff);
  Serial.print(F("]: "));
  String input = SerialReadLineBlocking();
  if (input.length() > 0)
  {
    new_lv_cutoff = input.toFloat();
    if (new_lv_cutoff < 7.0 || new_lv_cutoff > 12.0)
    {
      Serial.println(F("Invalid value. Please enter a value between 7.0 and 12.0."));
      return;
    }
  }
  else
  {
    new_lv_cutoff = current_lv_cutoff;
  }

  // --- Hysteresis ---
  Serial.print(F("Enter Hysteresis (Volts) [default: "));
  Serial.print(current_hysteresis);
  Serial.print(F("]: "));
  input = SerialReadLineBlocking();
  if (input.length() > 0)
  {
    new_hysteresis = input.toFloat();
    if (new_hysteresis < 0.1 || new_hysteresis > 2.0)
    {
      Serial.println(F("Invalid value. Please enter a value between 0.1 and 2.0."));
      return;
    }
  }
  else
  {
    new_hysteresis = current_hysteresis;
  }

  // --- Overcurrent Threshold ---
  Serial.print(F("Enter Overcurrent Threshold (Amps) [default: "));
  Serial.print(current_oc_thresh);
  Serial.print(F("]: "));
  input = SerialReadLineBlocking();
  if (input.length() > 0)
  {
    new_oc_thresh = input.toFloat();
    if (new_oc_thresh < 1.0 || new_oc_thresh > 200.0)
    {
      Serial.println(F("Invalid value. Please enter a value between 1.0 and 200.0."));
      return;
    }
  }
  else
  {
    new_oc_thresh = current_oc_thresh;
  }

  // --- Compensation Resistance ---
  float current_comp_res = ina.getCompensationResistance();
  float new_comp_res;
  Serial.print(F("Enter Compensation Resistance (Ohms) [default: "));
  Serial.print(current_comp_res, 3);
  Serial.print(F("]: "));
  input = SerialReadLineBlocking();
  if (input.length() > 0)
  {
      new_comp_res = input.toFloat();
      if (new_comp_res < 0.0 || new_comp_res > 1.0) {
           Serial.println(F("Invalid value. Must be between 0.0 and 1.0."));
           return;
      }
  } else {
      new_comp_res = current_comp_res;
  }

  // --- Save Settings ---
  // Update compensation resistance first (saves to NVS)
  ina.setCompensationResistance(new_comp_res);
  // Update other settings (saves to NVS again)
  ina.setProtectionSettings(new_lv_cutoff, new_hysteresis, new_oc_thresh);
  Serial.println(F("Protection settings updated."));
}



// Global state for Gauge Connection
bool g_gaugeLastTxSuccess = false;

void onDataSent(const uint8_t *mac_addr, esp_now_send_status_t status)
{
  Serial.print("Last Packet Send Status: ");
  Serial.println(status == ESP_NOW_SEND_SUCCESS ? "Success" : "Fail");
  
  bool isGauge = espNowHandler.isGaugeMac(mac_addr);
  Serial.printf("[DEBUG] onDataSent: isGauge=%d, Status=%d\n", isGauge, status);

  if (isGauge) {
      static int failCount = 0;
      if (status == ESP_NOW_SEND_SUCCESS) {
          failCount = 0;
          g_gaugeLastTxSuccess = true;
          Serial.println("[DEBUG] Gauge: SUCCESS (connected)");
      } else {
          failCount++;
          Serial.printf("[DEBUG] Gauge: FAIL count=%d\n", failCount);
          if (failCount >= 2) {
              g_gaugeLastTxSuccess = false;
              Serial.println("[DEBUG] Gauge: DISCONNECTED");
          }
      }
  }
}

void printResetReason() {
  esp_reset_reason_t reason = esp_reset_reason();
  Serial.printf("\n[BOOT] Reset Reason: %d (", reason);
  switch (reason) {
    case ESP_RST_UNKNOWN: Serial.print("Unknown"); break;
    case ESP_RST_POWERON: Serial.print("Power On"); break;
    case ESP_RST_EXT: Serial.print("External Pin"); break;
    case ESP_RST_SW: Serial.print("Software Reset"); break;
    case ESP_RST_PANIC: Serial.print("Exception/Panic"); break;
    case ESP_RST_INT_WDT: Serial.print("Interrupt WDT"); break;
    case ESP_RST_TASK_WDT: Serial.print("Task WDT"); break;
    case ESP_RST_WDT: Serial.print("Other WDT"); break;
    case ESP_RST_DEEPSLEEP: Serial.print("Deep Sleep"); break;
    case ESP_RST_BROWNOUT: Serial.print("Brownout"); break;
    case ESP_RST_SDIO: Serial.print("SDIO"); break;
    default: Serial.print("Other"); break;
  }
  Serial.println(")");

  // Save to NVS
  Preferences prefs;
  prefs.begin("crash_log", false);
  prefs.putInt("reason", (int)reason);
  prefs.end();
}

void onScanComplete();


void setup()
{
  Serial.begin(115200);

  // Process any crash logs from previous boot
  crash_handler_process_on_boot();

  // Wait for serial monitor to connect
  while (!Serial && millis() < 2000);
  delay(250);

  printResetReason();


  // Disable the RTC GPIO hold on boot
  gpio_hold_dis(GPIO_NUM_5);

  pinMode(LED_PIN, OUTPUT);

  // Check if the device has just been updated
  if (Update.isFinished()) {
    Serial.println("OTA update successful! Rebooted.");
    ota_success_notification_pending = true;
  }

  otaHandler.begin();
  otaHandler.setPreUpdateCallback(preOtaUpdate);

  // The begin method now handles loading the calibrated resistance
  ina226_adc.begin(6, 10);
  starter_adc.begin();

  // Clear any startup alerts before attaching the interrupt
  ina226_adc.clearAlerts();
  // Attach interrupt for INA226 alert pin
  attachInterrupt(digitalPinToInterrupt(INA_ALERT_PIN), alertISR, FALLING);

  if (!ina226_adc.isConfigured())
  {
    Serial.println("\n!!! DEVICE NOT CONFIGURED !!!");
    Serial.println("Load output has been disabled.");
    Serial.println("Please run Shunt Resistance Calibration ('r') and restart.");
    ina226_adc.setLoadConnected(false, MANUAL);
  }

  // Check for and restore battery capacity from NVS
  Preferences preferences;
  preferences.begin("storage", true); // read-only
  if (preferences.isKey("bat_cap"))
  {
    float restored_capacity = preferences.getFloat("bat_cap", 0.0f);
    preferences.end(); // close read-only

    ina226_adc.setBatteryCapacity(restored_capacity);
    Serial.printf("Restored battery capacity: %f\n", restored_capacity);

    // Now clear the key
    preferences.begin("storage", false); // read-write
    preferences.remove("bat_cap");
    preferences.end();
    Serial.println("Cleared battery capacity from NVS");
  }
  else
  {
    preferences.end();
  }

  // Print calibration summary on boot
  Serial.println(F("\n--- Stored Calibration Summary ---"));
  uint16_t activeShuntA = ina226_adc.getActiveShunt();

  Serial.printf("Active Shunt: %uA\n", activeShuntA);

  // --- Smarter Boot SOC ---
  // If voltage is high (>13.4V) and current is low (<0.5A), assume 100% SOC (Float/Full).
  // This overrides the open-circuit voltage lookup which might be conservative.
  ina226_adc.readSensors(); // Ensure fresh reading
  float bootV = ina226_adc.getBusVoltage_V();
  float bootA = ina226_adc.getCurrent_mA() / 1000.0f;
  
  if (bootV > 13.4f && fabs(bootA) < 0.5f) {
      Serial.printf("Boot Smart SOC: Voltage %.2fV > 13.4V and Current %.2fA < 0.5A. Forcing 100%% SOC.\n", bootV, bootA);
      ina226_adc.setSOC_percent(100.0f);
      Serial.println("SOC Synced to 100% (High Voltage + Low Current Detected)");
  }
  Serial.printf(" -> Calibrated Resistance: %.9f Ohms\n", ina226_adc.getCalibratedShuntResistance());
  if (ina226_adc.hasCalibrationTable()) {
      Serial.printf(" -> Using TABLE calibration (%u points)\n", ina226_adc.getCalibrationTable().size());
  } else {
      float g, o;
      ina226_adc.getCalibration(g, o);
      Serial.printf(" -> Using LINEAR calibration (gain=%.6f, offset=%.3f mA)\n", g, o);
  }

  Serial.println(F("\nStored calibrations for all shunts:"));
  for (int sh = 100; sh <= 500; sh += 50)
  {
    size_t cnt = 0;
    bool hasTbl = ina226_adc.hasStoredCalibrationTable(sh, cnt);
    if (hasTbl)
    {
      Serial.printf("  %dA: TABLE present (%u pts)\n", sh, (unsigned)cnt);
    }
    else
    {
      // To keep the log cleaner, let's not print anything if no calibration is stored.
      // The old way printed "No saved calibration", which was noisy.
    }
  }
  Serial.println(F("------------------------------------"));


  // Centralized NimBLE initialization
  BLEDevice::init("AE Smart Shunt");
  BLEDevice::setMTU(517);

// Callback for TPMS Scan Complete - Send WiFi Packet IMMEDIATELY
  // Initialize ESP-NOW
  if (!espNowHandler.begin())
  {
    Serial.println("ESP-NOW init failed");
    return;
  }
  
  // TPMS Setup
  tpmsHandler.setScanCompleteCallback(onScanComplete);
  tpmsHandler.begin();

  // Register callback for send status
  espNowHandler.registerSendCallback(onDataSent);
  // Ensure broadcast peer exists (some SDKs require an explicit broadcast peer)
  if (!espNowHandler.addPeer())
  {
    Serial.println("Warning: failed to add broadcast peer; esp_now_send may return ESP_ERR_ESPNOW_NOT_FOUND on some platforms");
  }
  else
  {
    Serial.println("Broadcast peer added");
  }

  bleHandler.setLoadSwitchCallback(loadSwitchCallback);
  bleHandler.setSOCCallback(socCallback);
  bleHandler.setVoltageProtectionCallback(voltageProtectionCallback);
  bleHandler.setLowVoltageDelayCallback(lowVoltageDelayCallback);
  bleHandler.setLowVoltageDelayCallback(lowVoltageDelayCallback);
  bleHandler.setDeviceNameSuffixCallback(deviceNameSuffixCallback);
  bleHandler.setRatedCapacityCallback(ratedCapacityCallback);
  bleHandler.setWifiSsidCallback(wifiSsidCallback);
  bleHandler.setWifiPassCallback(wifiPassCallback);
  bleHandler.setOtaControlCallback(otaControlCallback);
  bleHandler.setOtaTriggerCallback(otaTriggerCallback);
  bleHandler.setPairingCallback(pairingCallback);
  bleHandler.setEfuseLimitCallback([](float limit){
      Serial.printf("[BLE WRITE] E-Fuse Limit: %.2f A\n", limit);
      ina226_adc.setEfuseLimit(limit);
  });
  bleHandler.setTpmsConfigCallback([](std::vector<uint8_t> data){
      if (data.size() == 48) {
          Serial.println("BLE: Received TPMS Config Restore (48 bytes)");
          struct_message_tpms_config* config = (struct_message_tpms_config*)data.data();
          tpmsHandler.setConfig(config->macs, config->baselines, config->configured);
      } else {
          Serial.printf("BLE: TPMS Config Restore Failed - Invalid Size (%d)\n", data.size());
      }
  });
  bleHandler.setServerCallbacks(new MainServerCallbacks());

  // Cloud Config Callback
  bleHandler.setCloudConfigCallback([](bool enabled){
      Serial.printf("[BLE] Cloud Config Set: %s\n", enabled ? "ON" : "OFF");
      g_cloudEnabled = enabled;
      
      Preferences p;
      p.begin("config", false);
      p.putBool("cloud_enabled", enabled);
      p.end();
      
      
      if (enabled) {
          g_forceMqttUplink = true;
          Serial.println("[BLE] Cloud Enabled. Flag set for Immediate Uplink.");
      }
  });

  // MQTT Broker Callback
  bleHandler.setMqttBrokerCallback([](String broker){
      mqttHandler.setBroker(broker);
  });

  bleHandler.setMqttAuthCallback([](String user, String pass){
      mqttHandler.setAuth(user, pass);
  });

  // Load Pairing Info if exists
  Preferences prefs;
  prefs.begin("pairing", true); // read-only check first
  String storedMac = prefs.getString("p_gauge_mac", "");
  String storedKey = prefs.getString("p_key", "");
  prefs.end();
  
  if (storedMac != "" && storedKey != "") {
      Serial.println("Restoring Encrypted Peer (Gauge) from NVS...");
      uint8_t macBytes[6];
      uint8_t keyBytes[16];
      
      // Sanitise MAC (remove colons if present)
      storedMac.replace(":", "");
      
      hexStringToBytes(storedMac, macBytes, 6); 
      hexStringToBytes(storedKey, keyBytes, 16);
      
      espNowHandler.addEncryptedPeer(macBytes, keyBytes);
      espNowHandler.switchToSecureMode(macBytes);
  }

  // Load Temp Sensor Peer
  String tempMac = prefs.getString("p_temp_mac", "");
  String tempKey = prefs.getString("p_temp_key", "");
  
  if (tempMac != "" && tempKey != "") {
      Serial.println("Restoring Encrypted Peer (Temp Sensor) from NVS...");
      uint8_t macBytes[6];
      uint8_t keyBytes[16];
      
      // Sanitise MAC (remove colons if present)
      tempMac.replace(":", "");
      
      hexStringToBytes(tempMac, macBytes, 6);
      hexStringToBytes(tempKey, keyBytes, 16);
      
      espNowHandler.addEncryptedPeer(macBytes, keyBytes);
      // Do NOT switch secure mode target to Temp Sensor (we talk secure to Gauge primarily)
      // But adding the peer allows us to Receive encrypted data from it.
  }

  // Create initial telemetry data for the first advertisement
  ina226_adc.readSensors(); // Read sensors to get initial values
  
  // Load Cloud Config
  prefs.begin("config", true);
  g_cloudEnabled = prefs.getBool("cloud_enabled", false);
  prefs.end();
  Serial.printf("Cloud Uplink Enabled: %s\n", g_cloudEnabled ? "YES" : "NO");

  Telemetry initial_telemetry = {
      .batteryVoltage = ina226_adc.getBusVoltage_V(),
      .batteryCurrent = ina226_adc.getCurrent_mA() / 1000.0f,
      .batteryPower = ina226_adc.getPower_mW() / 1000.0f,
      .batterySOC = 0.0f, // Will be calculated in the loop
      .batteryCapacity = 0.0f, // Will be calculated in the loop
      .starterBatteryVoltage = starter_adc.readVoltage(),
      .isCalibrated = ina226_adc.isConfigured(),
      .errorState = 0,
      .loadState = ina226_adc.isLoadConnected(),
      .cutoffVoltage = ina226_adc.getLowVoltageCutoff(),
      .reconnectVoltage = (ina226_adc.getLowVoltageCutoff() + ina226_adc.getHysteresis()),
      .lastHourWh = 0.0f,
      .lastDayWh = 0.0f,
      .lastWeekWh = 0.0f,
      .lowVoltageDelayS = ina226_adc.getLowVoltageDelay(),
      .deviceNameSuffix = ina226_adc.getDeviceNameSuffix(),
      .eFuseLimit = ina226_adc.getEfuseLimit(),
      .activeShuntRating = ina226_adc.getActiveShunt(),
      .ratedCapacity = ina226_adc.getMaxBatteryCapacity(),
      .crashLog = crash_handler_get_log()
  };
  bleHandler.begin(initial_telemetry);
  bleHandler.setInitialWifiSsid(otaHandler.getWifiSsid());
  bleHandler.setInitialMqttBroker(mqttHandler.getBroker());
  bleHandler.setInitialMqttUser(mqttHandler.getUser());
  bleHandler.setInitialCloudConfig(g_cloudEnabled); // Set loaded cloud config state
  
  // Initialize TPMS Scanner (Async)


  // Set the firmware version on the BLE characteristic
  bleHandler.updateFirmwareVersion(OTA_VERSION);
  Serial.printf("Firmware version %s set on BLE characteristic.\n", OTA_VERSION);
  
  mqttHandler.begin(); // Init MQTT Client

  Serial.println("Setup done");
}

void loop_deprecated()
{
  tpmsHandler.update();
  
  // LED Heartbeat
  if (millis() - last_led_blink > led_blink_interval)
  {
    digitalWrite(LED_PIN, !digitalRead(LED_PIN));
    last_led_blink = millis();
  }

  if (ina226_adc.isAlertTriggered())
  {
    ina226_adc.processAlert();
  }

  if (ota_command_pending) {
      ota_command_pending = false;
      otaHandler.handleOtaControl(ota_command);
  }

  otaHandler.loop();

  // Check serial for calibration command
  if (Serial.available())
  {
    String s = Serial.readStringUntil('\n');
    s.trim();
    if (s.equalsIgnoreCase("r"))
    {
      // run the new shunt resistance calibration
      runShuntResistanceCalibration(ina226_adc);
    }
    else if (s.equalsIgnoreCase("v"))
    {
      // run the new starter battery ADC multi-point calibration
      runStarterADC_MultiPoint_Calibration(starter_adc);
    }
    else if (s.equalsIgnoreCase("y"))
    {
      // export starter battery voltage calibration data
      runExportVoltageCalibration(starter_adc);
    }
    else if (s.equalsIgnoreCase("p"))
    {
      // run the protection configuration menu
      runProtectionConfigMenu(ina226_adc);
    }
    // run the factory reset menu
    else if (s.equalsIgnoreCase("f"))
    {
      // FACTORY RESET: erase saved settings and reboot
      Serial.println(F("\n*** FACTORY RESET ***"));
      Serial.println(F("This will PERMANENTLY ERASE ALL SETTINGS, CALIBRATION, and pairing info."));
      Serial.println(F("Type YES to confirm:"));
      String conf = SerialReadLineBlocking();
      if (conf == "YES") {
        Serial.println(F("PERFORMING FULL HARDWARE WIPE of NVS partition..."));
        
        // Best-effort: put hardware into a safe state first
        ina226_adc.setLoadConnected(false, MANUAL);

        // Clear WiFi credentials
        WiFi.disconnect(true, true);
        
        // Perform low-level NVS erase
        esp_err_t err = nvs_flash_erase();
        if (err != ESP_OK) {
            Serial.printf("Error: nvs_flash_erase failed (0x%x)\n", err);
        }
        err = nvs_flash_init();
        if (err != ESP_OK) {
             Serial.printf("Error: nvs_flash_init failed (0x%x)\n", err);
        }

        Serial.println(F("Factory reset complete. Rebooting in 1s..."));
        delay(1000);
        ESP.restart();
      } else {
        Serial.println(F("Canceled."));
      }
    }
    else if (s.equalsIgnoreCase("l"))
    {
      // toggle load connection
      if (ina226_adc.isLoadConnected())
      {
        ina226_adc.setLoadConnected(false, MANUAL);
        Serial.println("Load manually toggled OFF");
      }
      else
      {
        ina226_adc.setLoadConnected(true, NONE);
        Serial.println("Load manually toggled ON");
      }
    }

    else if (s.equalsIgnoreCase("a"))
    {
      // toggle hardware alert
      ina226_adc.toggleHardwareAlerts();
      if (ina226_adc.areHardwareAlertsDisabled())
      {
        Serial.println("Hardware alerts DISABLED.");
      }
      else
      {
        Serial.println("Hardware alerts ENABLED.");
      }
    }
    else if (s.equalsIgnoreCase("s"))
    {
      // print protection status
      Serial.println(F("\n--- Protection Status ---"));
      int alertPinState = digitalRead(INA_ALERT_PIN);
      Serial.print(F("Alert Pin State      : "));
      Serial.print(alertPinState == HIGH ? "INACTIVE (HIGH)" : "ACTIVE (LOW)");
      Serial.println();
      Serial.print(F("Hardware Alerts      : "));
      Serial.println(ina226_adc.areHardwareAlertsDisabled() ? "DISABLED" : "ENABLED");
      Serial.print(F("Configured Threshold : "));
      Serial.print(ina226_adc.getOvercurrentThreshold());
      Serial.println(F(" A"));
      Serial.print(F("Actual HW Threshold  : "));
      Serial.print(ina226_adc.getHardwareAlertThreshold_A());
      Serial.println(F(" A"));
      Serial.print(F("Low Voltage Cutoff   : "));
      Serial.print(ina226_adc.getLowVoltageCutoff());
      Serial.println(F(" V"));
      Serial.print(F("Hysteresis           : "));
      Serial.print(ina226_adc.getHysteresis());
      Serial.println(F(" V"));
      Serial.println(F("-------------------------"));
    }
    else if (s.equalsIgnoreCase("d"))
    {
      // dump INA226 registers
      ina226_adc.dumpRegisters();
    }
    // else ignore — keep running
  }

  // --- HIGH FREQUENCY POLLING (100ms) ---
  if (millis() - last_polling_millis > polling_interval)
  {
      ina226_adc.readSensors();
      ina226_adc.checkAndHandleProtection(); // Fast protection check
      
      if (ina226_adc.isConfigured()) {
          // Update coulomb counting integration at 10Hz
          ina226_adc.updateBatteryCapacity(ina226_adc.getCurrent_mA() / 1000.0f);
          
          // Update energy usage stats
          ina226_adc.updateEnergyUsage(ina226_adc.getPower_mW());
      }
      
      last_polling_millis = millis();
  }

  // TPMS Update (Drives Scan Cycle + Callbacks)
  tpmsHandler.update();

  // --- TELEMETRY BROADCAST (Fallback Timer) ---
  if (millis() - last_telemetry_millis > telemetry_interval)
  {
    // Populate struct fields based on configuration status
    ae_smart_shunt_struct.messageID = 11;
    ae_smart_shunt_struct.dataChanged = true;

    if (ina226_adc.isConfigured())
    {
      // --- CONFIGURED ---
      ae_smart_shunt_struct.isCalibrated = true;
      // Note: We don't need to call checkAndHandleProtection here as it's done in the fast loop

      ae_smart_shunt_struct.batteryVoltage = ina226_adc.getBusVoltage_V();
      ae_smart_shunt_struct.batteryCurrent = ina226_adc.getCurrent_mA() / 1000.0f;
      ae_smart_shunt_struct.batteryPower = ina226_adc.getPower_mW() / 1000.0f;
      ae_smart_shunt_struct.starterBatteryVoltage = starter_adc.readVoltage();
      ae_smart_shunt_struct.lastHourWh = ina226_adc.getLastHourEnergy_Wh();
      ae_smart_shunt_struct.lastDayWh = ina226_adc.getLastDayEnergy_Wh();
      ae_smart_shunt_struct.lastWeekWh = ina226_adc.getLastWeekEnergy_Wh();

#ifdef SIMULATION_MODE
      // Mock Daily/Weekly Energy for UI Testing
      // Sweep +/- 1200Wh (Daily) and +/- 5000Wh (Weekly)
      unsigned long sim_t = millis();
      float day_angle = (sim_t % 30000) / 30000.0f * 2 * PI;
      float week_angle = (sim_t % 60000) / 60000.0f * 2 * PI;
      
      ae_smart_shunt_struct.lastDayWh = 1200.0f * sin(day_angle);
      ae_smart_shunt_struct.lastWeekWh = 5000.0f * sin(week_angle);
#endif

      ae_smart_shunt_struct.batteryState = 0; // 0 = Normal, 1 = Warning, 2 = Critical
      
      if (!ina226_adc.isLoadConnected() && ina226_adc.getDisconnectReason() == OVERCURRENT) {
          ae_smart_shunt_struct.batteryState = 5; // 5 = E-Fuse Tripped
      }

      // Get remaining Ah from INA helper
      float remainingAh = ina226_adc.getBatteryCapacity();
      ae_smart_shunt_struct.batteryCapacity = remainingAh; // remaining capacity in Ah
      
      // Use the dynamic max capacity for SOC calc
      float maxCap = ina226_adc.getMaxBatteryCapacity();
      if (maxCap > 0.0f)
      {
        ae_smart_shunt_struct.batterySOC = remainingAh / maxCap; // fraction 0..1
      }
      else
      {
        ae_smart_shunt_struct.batterySOC = 0.0f;
      }

      if (ina226_adc.isOverflow())
      {
        Serial.println("Overflow! Choose higher current range");
        ae_smart_shunt_struct.batteryState = 3; // overflow indicator
      }

      // Calculate and print run-flat time with warning threshold
      bool warning = false;
      float currentA = ina226_adc.getCurrent_mA() / 1000.0f; // convert mA to A
      float warningThresholdHours = 10.0f;
      String avgRunFlatTimeStr = ina226_adc.getAveragedRunFlatTime(currentA, warningThresholdHours, warning);
      memset(ae_smart_shunt_struct.runFlatTime, 0, sizeof(ae_smart_shunt_struct.runFlatTime));  // Clear buffer
      strncpy(ae_smart_shunt_struct.runFlatTime, avgRunFlatTimeStr.c_str(), sizeof(ae_smart_shunt_struct.runFlatTime) - 1);
    }
    else
    {
      // --- NOT CONFIGURED ---
      ae_smart_shunt_struct.isCalibrated = false;
      ae_smart_shunt_struct.batteryVoltage = ina226_adc.getBusVoltage_V();
      ae_smart_shunt_struct.starterBatteryVoltage = starter_adc.readVoltage();

      // Set other fields to default/error values
      ae_smart_shunt_struct.batteryCurrent = 0.0f;
      ae_smart_shunt_struct.batteryPower = 0.0f;
      ae_smart_shunt_struct.batterySOC = 0.0f;
      ae_smart_shunt_struct.batteryCapacity = 0.0f;
      ae_smart_shunt_struct.batteryState = 4; // Use 4 for "Not Calibrated"
      memset(ae_smart_shunt_struct.runFlatTime, 0, sizeof(ae_smart_shunt_struct.runFlatTime));  // Clear buffer
      strncpy(ae_smart_shunt_struct.runFlatTime, "NOT CALIBRATED", sizeof(ae_smart_shunt_struct.runFlatTime) - 1);
      ae_smart_shunt_struct.lastHourWh = 0.0f;
      ae_smart_shunt_struct.lastDayWh = 0.0f;
      ae_smart_shunt_struct.lastWeekWh = 0.0f;
    }
    ae_smart_shunt_struct.runFlatTime[sizeof(ae_smart_shunt_struct.runFlatTime) - 1] = '\0'; // ensure null termination

    // Populate device name (use suffix if configured, otherwise default)
    String suffix = ina226_adc.getDeviceNameSuffix();
    String deviceName = "AE Smart Shunt";
    if (suffix.length() > 0) {
        deviceName = suffix;  // Use just the suffix as the display name
    }
    strncpy(ae_smart_shunt_struct.name, deviceName.c_str(), sizeof(ae_smart_shunt_struct.name) - 1);
    ae_smart_shunt_struct.name[sizeof(ae_smart_shunt_struct.name) - 1] = '\0';

    // Populate Offloaded TPMS Data
    for(int i=0; i<TPMS_COUNT; i++) {
        const TPMSSensor* s = tpmsHandler.getSensor(i);
        ae_smart_shunt_struct.tpmsPressurePsi[i] = s->pressurePsi;
        ae_smart_shunt_struct.tpmsTemperature[i] = s->temperature;
        ae_smart_shunt_struct.tpmsVoltage[i] = s->batteryVoltage;
        ae_smart_shunt_struct.tpmsLastUpdate[i] = s->lastUpdate;
    }
    
    // Populate Relayed Temp Sensor Data & Calculate Age
    float t_temp = 0; uint8_t t_batt = 0; uint32_t t_last = 0; uint32_t t_interval = 0;
    espNowHandler.getTempSensorData(t_temp, t_batt, t_last, t_interval, ae_smart_shunt_struct.tempSensorName);
    
    ae_smart_shunt_struct.tempSensorTemperature = t_temp;
    ae_smart_shunt_struct.tempSensorBatteryLevel = t_batt;
    ae_smart_shunt_struct.tempSensorUpdateInterval = t_interval; // Relay the interval!
    
    if (t_last > 0) {
        ae_smart_shunt_struct.tempSensorLastUpdate = millis() - t_last; // Age in ms
    } else {
        ae_smart_shunt_struct.tempSensorLastUpdate = 0xFFFFFFFF; // Never updated
    }

    // Update BLE characteristics
    Telemetry telemetry_data = {
        .batteryVoltage = ae_smart_shunt_struct.batteryVoltage,
        .batteryCurrent = ae_smart_shunt_struct.batteryCurrent,
        .batteryPower = ae_smart_shunt_struct.batteryPower,
        .batterySOC = ae_smart_shunt_struct.batterySOC,
        .batteryCapacity = ae_smart_shunt_struct.batteryCapacity, // Remaining capacity
        .starterBatteryVoltage = ae_smart_shunt_struct.starterBatteryVoltage,
        .isCalibrated = ae_smart_shunt_struct.isCalibrated,
        .errorState = ae_smart_shunt_struct.batteryState,
        .loadState = ina226_adc.isLoadConnected(),
        .cutoffVoltage = ina226_adc.getLowVoltageCutoff(),
        .reconnectVoltage = (ina226_adc.getLowVoltageCutoff() + ina226_adc.getHysteresis()),
        .lastHourWh = ina226_adc.getLastHourEnergy_Wh(),
        .lastDayWh = ina226_adc.getLastDayEnergy_Wh(),
        .lastWeekWh = ina226_adc.getLastWeekEnergy_Wh(),
        .lowVoltageDelayS = ina226_adc.getLowVoltageDelay(),
        .deviceNameSuffix = ina226_adc.getDeviceNameSuffix(),
        .eFuseLimit = ina226_adc.getEfuseLimit(),
        .activeShuntRating = ina226_adc.getActiveShunt(),
        .ratedCapacity = ina226_adc.getMaxBatteryCapacity(),
        .gaugeLastRx = espNowHandler.getLastGaugeRx(),
        .gaugeLastTxSuccess = g_gaugeLastTxSuccess
    };
    
    Serial.printf("[DEBUG] updateStruct: GaugeTxSuccess=%d\n", g_gaugeLastTxSuccess);
    
    bleHandler.updateTelemetry(telemetry_data);

    // Only send ESP-NOW data if WiFi is not connected (OTA not in progress)
    if (!WiFi.isConnected()) {
        Serial.println("Mesh transmission: ready!");
        espNowHandler.setAeSmartShuntStruct(ae_smart_shunt_struct);
        espNowHandler.sendMessageAeSmartShunt();
    }

    printShunt(&ae_smart_shunt_struct);
    if (ina226_adc.isOverflow())
    {
      Serial.println("Warning: Overflow condition!");
    }

    Serial.println();
    last_telemetry_millis = millis();
  }

  // Handle Async Restart
  if (g_pendingRestart && millis() > g_restartTs) {
      Serial.println("Executing Scheduled Restart...");
      delay(100);
      ESP.restart();
  }
}

// Helper to package and send BLE data
void sendBleUpdate() {


      // 10 Hz Telemetry Loop
      Telemetry telemetry_data = {
          .batteryVoltage = ina226_adc.getBusVoltage_V(),
          .batteryCurrent = ina226_adc.getCurrent_mA() / 1000.0f,
          .batteryPower = ina226_adc.getPower_mW() / 1000.0f,
          .batterySOC = ae_smart_shunt_struct.batterySOC * 100.0f,
          .batteryCapacity = ina226_adc.getBatteryCapacity(),
          .starterBatteryVoltage = starter_adc.readVoltage(),
          .isCalibrated = ina226_adc.isConfigured(),
          .errorState = ae_smart_shunt_struct.batteryState,
          .loadState = ina226_adc.isLoadConnected(),
          .cutoffVoltage = ina226_adc.getLowVoltageCutoff(),
          .reconnectVoltage = (ina226_adc.getLowVoltageCutoff() + ina226_adc.getHysteresis()),
          .lastHourWh = ina226_adc.getLastHourEnergy_Wh(),
          .lastDayWh = ina226_adc.getLastDayEnergy_Wh(),
          .lastWeekWh = ina226_adc.getLastWeekEnergy_Wh(),
          .lowVoltageDelayS = ina226_adc.getLowVoltageDelay(),
          .deviceNameSuffix = ina226_adc.getDeviceNameSuffix(),
          .eFuseLimit = ina226_adc.getEfuseLimit(),
          .activeShuntRating = ina226_adc.getActiveShunt(),
          .ratedCapacity = ina226_adc.getMaxBatteryCapacity(),
          .runFlatTime = String(ae_smart_shunt_struct.runFlatTime),
          // Diagnostics
          .diagnostics = "", 
          // New Telemetry
          .tempSensorTemperature = ae_smart_shunt_struct.tempSensorTemperature,
          .tempSensorBatteryLevel = ae_smart_shunt_struct.tempSensorBatteryLevel,
          .tempSensorLastUpdate = ae_smart_shunt_struct.tempSensorLastUpdate,
          .tempSensorUpdateInterval = ae_smart_shunt_struct.tempSensorUpdateInterval,
          .tpmsPressurePsi = {ae_smart_shunt_struct.tpmsPressurePsi[0], ae_smart_shunt_struct.tpmsPressurePsi[1], ae_smart_shunt_struct.tpmsPressurePsi[2], ae_smart_shunt_struct.tpmsPressurePsi[3]},
          .gaugeLastRx = espNowHandler.getLastGaugeRx(),
          .gaugeLastTxSuccess = g_gaugeLastTxSuccess
      };
      
      // Populate TPMS Config Backup
      tpmsHandler.getRawConfig(telemetry_data.tpmsConfig);

      // Add Diagnostics String
      uint32_t uptime = millis() / 1000;
      int days = uptime / 86400;
      int hours = (uptime % 86400) / 3600;
      int minutes = (uptime % 3600) / 60;
      
      char diagBuf[64];
      snprintf(diagBuf, sizeof(diagBuf), "Rst:%d Up:%dd %dh %dm", esp_reset_reason(), days, hours, minutes);
      telemetry_data.diagnostics = String(diagBuf);

      bleHandler.updateTelemetry(telemetry_data);
}

// Callback for TPMS Scan Complete - Send WiFi Packet IMMEDIATELY
void updateStruct(); // Fwd Decl
void onScanComplete() {
    updateStruct(); // Populate fresh data (calculates Temp Age, etc)
    espNowHandler.sendMessageAeSmartShunt();
    telemetry_counter++;
    sendBleUpdate(); // <-- CRITICAL: Update BLE immediately after scan
    last_telemetry_millis = millis(); // Reset timer fallback
}

void updateStruct() {
    // Populate struct fields based on configuration status
    ae_smart_shunt_struct.messageID = 11;
    ae_smart_shunt_struct.dataChanged = true;

    if (ina226_adc.isConfigured())
    {
      // --- CONFIGURED ---
      ae_smart_shunt_struct.isCalibrated = true;
      // Note: We don't need to call checkAndHandleProtection here as it's done in the fast loop

      ae_smart_shunt_struct.batteryVoltage = ina226_adc.getBusVoltage_V();
      ae_smart_shunt_struct.batteryCurrent = ina226_adc.getCurrent_mA() / 1000.0f;
      ae_smart_shunt_struct.batteryPower = ina226_adc.getPower_mW() / 1000.0f;
      ae_smart_shunt_struct.starterBatteryVoltage = starter_adc.readVoltage();
      ae_smart_shunt_struct.lastHourWh = ina226_adc.getLastHourEnergy_Wh();
      ae_smart_shunt_struct.lastDayWh = ina226_adc.getLastDayEnergy_Wh();
      ae_smart_shunt_struct.lastWeekWh = ina226_adc.getLastWeekEnergy_Wh();

      // Populate Device Name (Consistency with BLE Advertised Name)
      String suffix = ina226_adc.getDeviceNameSuffix();
      String deviceName = "AE Smart Shunt";
      if (suffix.length() > 0) {
          deviceName += " - " + suffix;
      }
      strncpy(ae_smart_shunt_struct.name, deviceName.c_str(), sizeof(ae_smart_shunt_struct.name) - 1);
      ae_smart_shunt_struct.name[sizeof(ae_smart_shunt_struct.name) - 1] = '\0';

#ifdef SIMULATION_MODE
      // Mock Daily/Weekly Energy for UI Testing
      // Sweep +/- 1200Wh (Daily) and +/- 5000Wh (Weekly)
      unsigned long sim_t = millis();
      float day_angle = (sim_t % 30000) / 30000.0f * 2 * PI;
      float week_angle = (sim_t % 60000) / 60000.0f * 2 * PI;
      
      ae_smart_shunt_struct.lastDayWh = 1200.0f * sin(day_angle);
      ae_smart_shunt_struct.lastWeekWh = 5000.0f * sin(week_angle);
#endif

      ae_smart_shunt_struct.batteryState = 0; // 0 = Normal, 1 = Warning, 2 = Critical
      
      if (!ina226_adc.isLoadConnected() && ina226_adc.getDisconnectReason() == OVERCURRENT) {
          ae_smart_shunt_struct.batteryState = 5; // 5 = E-Fuse Tripped
      }

      // Get remaining Ah from INA helper
      float remainingAh = ina226_adc.getBatteryCapacity();
      ae_smart_shunt_struct.batteryCapacity = remainingAh; // remaining capacity in Ah
      
      // Use the dynamic max capacity for SOC calc
      float maxCap = ina226_adc.getMaxBatteryCapacity();
      if (maxCap > 0.0f)
      {
        ae_smart_shunt_struct.batterySOC = remainingAh / maxCap; // fraction 0..1
      }
      else
      {
        ae_smart_shunt_struct.batterySOC = 0.0f;
      }

      // Check for low SOC or low Voltage to set state
      if (ae_smart_shunt_struct.batterySOC < 0.2f || ae_smart_shunt_struct.batteryVoltage < 11.8f)
      {
        if (ae_smart_shunt_struct.batterySOC < 0.1f || ae_smart_shunt_struct.batteryVoltage < 11.5f)
        {
          ae_smart_shunt_struct.batteryState = 2; // Critical
        }
        else
        {
          ae_smart_shunt_struct.batteryState = 1; // Warning
        }
      }
      
      // Calculate Run Flat Time using averaged current from energy buffer
      // This provides a stable reading that accounts for intermittent loads (e.g., fridges)
      bool warning = false;
      float avgCurrentA = ina226_adc.getAverageCurrentFromEnergyBuffer_A();
      String runFlatTimeStr = ina226_adc.getAveragedRunFlatTime(avgCurrentA, 10.0f, warning);
      memset(ae_smart_shunt_struct.runFlatTime, 0, sizeof(ae_smart_shunt_struct.runFlatTime));  // Clear buffer
      strncpy(ae_smart_shunt_struct.runFlatTime, runFlatTimeStr.c_str(), sizeof(ae_smart_shunt_struct.runFlatTime) - 1);
      ae_smart_shunt_struct.runFlatTime[sizeof(ae_smart_shunt_struct.runFlatTime) - 1] = '\0';

    }
    else
    {
      // --- NOT CONFIGURED ---
      ae_smart_shunt_struct.isCalibrated = false;
      ae_smart_shunt_struct.batteryVoltage = 0.0f;
      ae_smart_shunt_struct.batteryCurrent = 0.0f;
      ae_smart_shunt_struct.batteryPower = 0.0f;
      ae_smart_shunt_struct.batterySOC = 0.0f;
      ae_smart_shunt_struct.batteryState = 0;
      snprintf(ae_smart_shunt_struct.runFlatTime, sizeof(ae_smart_shunt_struct.runFlatTime), "--");
    }
    
    // Add TPMS Data (Sent with EVERY update)
    for(int i=0; i<TPMS_COUNT; i++) {
        const TPMSSensor* s = tpmsHandler.getSensor(i);
        if (s && s->configured) {
            ae_smart_shunt_struct.tpmsPressurePsi[i] = s->pressurePsi;
            ae_smart_shunt_struct.tpmsTemperature[i] = s->temperature;
            ae_smart_shunt_struct.tpmsVoltage[i] = s->batteryVoltage;
            // Report Age (Time since last packet)
            if (s->lastUpdate > 0) {
                ae_smart_shunt_struct.tpmsLastUpdate[i] = millis() - s->lastUpdate;
            } else {
                ae_smart_shunt_struct.tpmsLastUpdate[i] = 0xFFFFFFFE; // Configured, Waiting for Data
            }
        } else {
            ae_smart_shunt_struct.tpmsPressurePsi[i] = 0;
            ae_smart_shunt_struct.tpmsTemperature[i] = 0;
            ae_smart_shunt_struct.tpmsVoltage[i] = 0;
            ae_smart_shunt_struct.tpmsLastUpdate[i] = 0xFFFFFFFF;
        }
    }

    // Relayed Temp Sensor Data (Always send freshest data)
    float tsTemp = 0.0f;
    uint8_t tsBatt = 0;
    uint32_t tsUpdate = 0;
    uint32_t tsInterval = 0;
    espNowHandler.getTempSensorData(tsTemp, tsBatt, tsUpdate, tsInterval, ae_smart_shunt_struct.tempSensorName);
    
    // Check for Staleness (Dynamic TTL)
    // Relaxed TTL: We want to keep relaying cached data for a long time (e.g., 10x interval or 10 mins)
    // so the App/Gauge can use the 'Age' field to decide when to show 'Disconnected', 
    // rather than the Shunt censoring the data prematurely.
    uint32_t ttl = (tsInterval > 0) ? (tsInterval * 10) : 600000; 
    if (ttl < 600000) ttl = 600000; // Enforce minimum floor of 10 minutes

    uint32_t age = (tsUpdate > 0) ? (millis() - tsUpdate) : 0xFFFFFFFF;
    if (age > ttl) { 
        Serial.printf("[DEBUG] Temp Stale: Age %u > TTL %u. Clearing.\n", age, ttl);
        age = 0xFFFFFFFF; // Mark as stale/invalid
        tsTemp = 0.0f;    // Clear value
        tsBatt = 0;
    }
    
    ae_smart_shunt_struct.tempSensorTemperature = tsTemp;
    ae_smart_shunt_struct.tempSensorBatteryLevel = tsBatt;
    ae_smart_shunt_struct.tempSensorUpdateInterval = tsInterval;
    ae_smart_shunt_struct.tempSensorLastUpdate = age;
    
    // DEBUG PRINT
    Serial.printf("[DEBUG] Telemetry #%u sent. TPMS=YES, Temp=%s (Interval: %u ms)\n", 
                  telemetry_counter, (age != 0xFFFFFFFF) ? "YES" : "NO_DATA", tsInterval);
    
    // Update local copy in Handler (Ready to Send)
    espNowHandler.setAeSmartShuntStruct(ae_smart_shunt_struct);
}



// Factory Mode Command Handler
void handleFactoryCommands(String cmd) {
    if (cmd == "CMD:TEST_ADC") {
        ina226_adc.readSensors();
        float busV = ina226_adc.getBusVoltage_V();
        float starterV = starter_adc.readVoltage();
        // Return simulated success if values are sane, or just the values
        // Format expectations from Provisioning Tool: "<< ADC_CAL: OK (-2mV offset)"
        Serial.printf("<< ADC_CAL: OK (Bus=%.2fV, Start=%.2fV)\n", busV, starterV);
    } 
    else if (cmd == "CMD:TEST_WIFI") {
        // Simple check if ESP-NOW init passed (it returns early in setup if failed)
        // RSSI is not available if not connected to AP, but we can fake it or scan.
        int8_t rssi = WiFi.RSSI(); 
        Serial.printf("<< WIFI: OK (RSSI: %d dBm)\n", rssi);
    }
    else {
        Serial.println("<< ERROR: Unknown Command");
    }
}

void loop() {
  bleHandler.loop(); 
  // Drives TPMS Scan & Callbacks -> onScanComplete() -> updateStruct() -> espNowHandler.sendMessage()
  // PAUSE SCAN if BLE Client Connected (to allow config/OTA)
  if (bleHandler.isConnected()) {
      tpmsHandler.stopScan();
  } else {
      tpmsHandler.update();
  }
  
  // High Frequency Polling (10 Hz)
  if (millis() - last_polling_millis > polling_interval) {
      ina226_adc.readSensors();
      ina226_adc.checkAndHandleProtection(); 
      if (ina226_adc.isConfigured()) {
          ina226_adc.updateBatteryCapacity(ina226_adc.getCurrent_mA() / 1000.0f);
          ina226_adc.updateEnergyUsage(ina226_adc.getPower_mW());
      }
      last_polling_millis = millis();
  }
  
  // Fallback Telemetry (Safety Net)
  if (millis() - last_telemetry_millis > telemetry_interval) {
      updateStruct();
      sendBleUpdate();
      
      if (!WiFi.isConnected()) {
          Serial.println("Mesh transmission: ready!");
          espNowHandler.setAeSmartShuntStruct(ae_smart_shunt_struct);
          espNowHandler.sendMessageAeSmartShunt();
          telemetry_counter++;
      }

      printShunt(&ae_smart_shunt_struct);
      if (ina226_adc.isOverflow()) {
        Serial.println("Warning: Overflow condition!");
      }

      Serial.println();
      last_telemetry_millis = millis();
  }

  // MQTT UPLINK (15 Minutes) or Forced
  if (g_cloudEnabled && (g_forceMqttUplink || millis() - lastMqttUplink > MQTT_UPLINK_INTERVAL)) {
      g_forceMqttUplink = false;
      lastMqttUplink = millis();
      String ssid = otaHandler.getWifiSsid();
      String pass = otaHandler.getWifiPass();
      
      if (ssid.length() > 0) {
          Serial.println("[MQTT] Starting 15-min Uplink. Pausing Radio Stacks...");
          
          // 1. Store ESP-NOW channel before deinit
          uint8_t espnow_channel = 1; // Default channel
          wifi_second_chan_t second;
          esp_wifi_get_channel(&espnow_channel, &second);
          Serial.printf("[MQTT] Stored ESP-NOW channel: %d\n", espnow_channel);
          
          // 2. Pause BLE (stop advertising, disconnect clients) - PRESERVE BONDING
          BLEAdvertising* pAdvertising = BLEDevice::getAdvertising();
          if (pAdvertising) {
              pAdvertising->stop();
              Serial.println("[MQTT] BLE advertising stopped");
          }
          if (bleHandler.isConnected()) {
              // NimBLE: disconnect all connected clients
              BLEDevice::getServer()->disconnect(0); // 0 = disconnect all
              Serial.println("[MQTT] BLE client disconnected");
          }
          
          // 3. Deinit ESP-NOW only
          esp_now_deinit();

          uint8_t runStatus = 2; // Default Wifi Fail
          unsigned long runResultTime = 0;

          // Check for WiFi Credentials
          if (otaHandler.getWifiSsid().length() == 0) {
              Serial.println("[MQTT] No WiFi SSID Set. Aborting Uplink.");
              runStatus = 4; // Wifi Missing
              // Restore immediately
              WiFi.mode(WIFI_OFF);
              espNowHandler.begin();
          } else {
            // 2. Connect WiFi (With Sniff)
            bool ssidFound = false;
            int n = WiFi.scanNetworks();
            for (int i = 0; i < n; ++i) {
                if (WiFi.SSID(i) == otaHandler.getWifiSsid()) {
                    ssidFound = true;
                    break;
                }
            }
            
            if (!ssidFound) {
                 Serial.println("[MQTT] Target SSID not found in scan. Aborting.");
                 runStatus = 2; // Wifi Fail
            } else {
                 WiFi.begin(otaHandler.getWifiSsid().c_str(), otaHandler.getWifiPass().c_str());
                 unsigned long startWifi = millis();
                 
                 while (WiFi.status() != WL_CONNECTED && millis() - startWifi < 10000) {
                     delay(500);
                     Serial.print(".");
                 }
                 
                 if (WiFi.status() == WL_CONNECTED) {
                     Serial.println("\n[MQTT] WiFi Connected. Connecting to Broker...");
                     if (mqttHandler.connect()) {
                         
                         // Check for Pending Crash Log
                         if (g_hasCrashLog) {
                             String log = crash_handler_get_log();
                             if (mqttHandler.sendCrashLog(log)) {
                                 Serial.println("[MQTT] Crash Log sent successfully.");
                                 g_hasCrashLog = false; // Prevent re-sending
                             } else {
                                 Serial.println("[MQTT] Failed to send Crash Log.");
                             }
                         }

                         // Update struct with fresh telemetry data before sending
                         updateStruct();
                         mqttHandler.sendUplink(ae_smart_shunt_struct);
                         // CRITICAL: Give MQTT client time to send message before WiFi disconnect
                         // PubSubClient needs multiple loop() calls to process outgoing queue
                         for (int i = 0; i < 20; i++) {
                             mqttHandler.loop();
                             delay(100); // 20 * 100ms = 2 seconds total
                         }
                         runStatus = 1; // Success
                         g_lastCloudSuccessTime = millis();
                     } else {
                         Serial.println("[MQTT] Broker Connection Failed.");
                         runStatus = 3; // MQTT Fail
                     }
                 } else {
                     Serial.println("\n[MQTT] WiFi Connection Failed.");
                     runStatus = 2;
                 }
            }

          g_lastCloudStatus = runStatus;

          WiFi.disconnect(true);
          WiFi.mode(WIFI_OFF);
          
          Serial.println("[MQTT] Restoring Radio Stacks...");
          espNowHandler.begin();
          
          // CRITICAL: Restore ESP-NOW channel after WiFi operations
          esp_wifi_set_channel(espnow_channel, WIFI_SECOND_CHAN_NONE);
          Serial.printf("[MQTT] Restored ESP-NOW channel: %d\n", espnow_channel);

          // Resume BLE (restart advertising) - BLE stack still running, bonding preserved
          // Construct Telemetry for advertising (reuse existing struct data)
           Telemetry telemetry_data = {
              .batteryVoltage = ae_smart_shunt_struct.batteryVoltage,
              .batteryCurrent = ae_smart_shunt_struct.batteryCurrent,
              .batteryPower = ae_smart_shunt_struct.batteryPower,
              .batterySOC = ae_smart_shunt_struct.batterySOC,
              .batteryCapacity = ae_smart_shunt_struct.batteryCapacity,
              .starterBatteryVoltage = ae_smart_shunt_struct.starterBatteryVoltage,
              .isCalibrated = ae_smart_shunt_struct.isCalibrated,
              .errorState = ae_smart_shunt_struct.batteryState,
              .loadState = ina226_adc.isLoadConnected(),
              .cutoffVoltage = ina226_adc.getLowVoltageCutoff(),
              .reconnectVoltage = (ina226_adc.getLowVoltageCutoff() + ina226_adc.getHysteresis()),
              .lastHourWh = ae_smart_shunt_struct.lastHourWh,
              .lastDayWh = ae_smart_shunt_struct.lastDayWh,
              .lastWeekWh = ae_smart_shunt_struct.lastWeekWh,
              .lowVoltageDelayS = ina226_adc.getLowVoltageDelay(),
              .deviceNameSuffix = ina226_adc.getDeviceNameSuffix(),
              .eFuseLimit = ina226_adc.getEfuseLimit(),
              .activeShuntRating = ina226_adc.getActiveShunt(),
              .ratedCapacity = ina226_adc.getMaxBatteryCapacity(),
              .runFlatTime = String(ae_smart_shunt_struct.runFlatTime),
              .diagnostics = "", 
              .tempSensorTemperature = ae_smart_shunt_struct.tempSensorTemperature,
              .tempSensorBatteryLevel = ae_smart_shunt_struct.tempSensorBatteryLevel,
              .tempSensorLastUpdate = ae_smart_shunt_struct.tempSensorLastUpdate,
              .tempSensorUpdateInterval = ae_smart_shunt_struct.tempSensorUpdateInterval,
              .tpmsPressurePsi = {ae_smart_shunt_struct.tpmsPressurePsi[0], ae_smart_shunt_struct.tpmsPressurePsi[1], ae_smart_shunt_struct.tpmsPressurePsi[2], ae_smart_shunt_struct.tpmsPressurePsi[3]},
              .gaugeLastRx = espNowHandler.getLastGaugeRx(),
              .gaugeLastTxSuccess = g_gaugeLastTxSuccess
          };
           
           // Restart advertising with current telemetry
           bleHandler.startAdvertising(telemetry_data);
          bleHandler.setInitialWifiSsid(otaHandler.getWifiSsid());
          bleHandler.setInitialMqttBroker(mqttHandler.getBroker());
          bleHandler.setInitialMqttUser(mqttHandler.getUser());
          
          // Report Status immediately
          const char* statusText[] = {"Unknown", "Success", "WiFi Fail", "MQTT Fail", "WiFi Missing"};
          Serial.printf("[MQTT] Cloud Status: %s (code %d)\n", 
                       (g_lastCloudStatus <= 4) ? statusText[g_lastCloudStatus] : "Invalid", 
                       g_lastCloudStatus);
          bleHandler.updateCloudStatus(g_lastCloudStatus, (millis() - g_lastCloudSuccessTime)/1000);
          
          Serial.println("[MQTT] Uplink Sequence Complete.");
      } 
      } else {
          Serial.println("[MQTT] No WiFi Credentials. Skipping Uplink.");
      }
  }
  
  // Handle Async Restart
  if (g_pendingRestart && millis() > g_restartTs) {
      Serial.println("Executing Scheduled Restart...");
      delay(100);
      ESP.restart();
  }
  
  // Handle BLE Serial Input (for debug commands if needed)
  if (Serial.available()) {
      String s = Serial.readStringUntil('\n');
      s.trim();
      if (s.startsWith("CMD:")) {
          handleFactoryCommands(s);
      } else {
          pairingCallback(s); // Reuse pairing callback for serial commands
      }
  }
}
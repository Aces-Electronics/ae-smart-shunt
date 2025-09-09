#include <vector>
#include <Arduino.h>
#include <Preferences.h>
#include "shared_defs.h"
#include "ina226_adc.h"
#include "ble_handler.h"
#include "espnow_handler.h"
#include "passwords.h"
#include <esp_now.h>
#include <esp_err.h>

// WiFi and OTA
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ota-github-cacerts.h>
#include <ota-github-defaults.h>
#define OTAGH_OWNER_NAME "Aces-Electronics"
#define OTAGH_REPO_NAME "ae-smart-shunt"
#include <OTA-Hub.hpp>

#define USE_ADC // if defined, use ADC, else, victron BLE
// #define USE_WIFI // if defined, conect to WIFI, else, don't

float batteryCapacity = 100.0f; // Default rated battery capacity in Ah (used for SOC calc)

uint8_t broadcastAddress[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// OTA update check interval (24 hours)
const unsigned long ota_check_interval = 24 * 60 * 60 * 1000;
unsigned long last_ota_check = 0;

// Main loop interval
const unsigned long loop_interval = 10000;
unsigned long last_loop_millis = 0;

struct_message_ae_smart_shunt_1 ae_smart_shunt_struct;
// Initializing with a default shunt resistor value, which will be overwritten
// if a calibrated value is loaded from NVS.
INA226_ADC ina226_adc(I2C_ADDRESS, 0.000944464f, 100.00f);
ESPNowHandler espNowHandler(broadcastAddress); // ESP-NOW handler for sending data
WiFiClientSecure wifi_client;

void IRAM_ATTR alertISR() {
  ina226_adc.handleAlert();
}

bool handleOTA()
{
  // 1. Check for updates, by checking the latest release on GitHub
  OTA::UpdateObject details = OTA::isUpdateAvailable();

  if (OTA::NO_UPDATE == details.condition)
  {
    Serial.println("No new update available. Continuing...");
    return false;
  }
  else
  // 2. Perform the update (if there is one)
  {
    Serial.println("Update available, saving battery capacity...");
    Preferences preferences;
    preferences.begin("storage", false);
    float capacity = ina226_adc.getBatteryCapacity();
    preferences.putFloat("bat_cap", capacity);
    preferences.end();
    Serial.printf("Saved battery capacity: %f\n", capacity);

    if (OTA::performUpdate(&details) == OTA::SUCCESS)
    {
      // .. success! It'll restart by default, or you can do other things here...
      return true;
    }
  }
  return false;
}

void daily_ota_check()
{
  if (millis() - last_ota_check > ota_check_interval)
  {
    // Notify the user that we are checking for updates
    strncpy(ae_smart_shunt_struct.runFlatTime, "Checking for updates...", sizeof(ae_smart_shunt_struct.runFlatTime));
    espNowHandler.setAeSmartShuntStruct(ae_smart_shunt_struct);
    espNowHandler.sendMessageAeSmartShunt();
    delay(100); // Give a moment for the message to be sent

    // Stop ESP-NOW to allow WiFi to connect
    esp_now_deinit();

    // WiFi connection
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    Serial.print("Connecting to WiFi for OTA check");
    while (WiFi.status() != WL_CONNECTED)
    {
      Serial.print(".");
      delay(500);
    }
    Serial.println("\nConnected to WiFi");

    bool updated = handleOTA();
    last_ota_check = millis();

    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    Serial.println("WiFi disconnected");

    if (!updated)
    {
      // Re-initialize ESP-NOW
      if (!espNowHandler.begin())
      {
        Serial.println("ESP-NOW init failed after OTA check");
      }
    }
  }
}

// helper: read a trimmed line from Serial (blocks until newline)
static String SerialReadLineBlocking()
{
  String s;
  while (true)
  {
    while (Serial.available() == 0)
      delay(5);
    s = Serial.readStringUntil('\n');
    s.trim();
    if (s.length() > 0)
      return s;
    // allow empty Enter to be treated as empty string
    return s;
  }
}

// Helper: wait for enter or 'x' while optionally streaming debug raw vs calibrated values.
// Returns the user-entered line (possibly empty string if they just pressed Enter), or "x" if canceled.
static String waitForEnterOrXWithDebug(INA226_ADC &ina, bool debugMode)
{
  // Flush any existing chars
  while (Serial.available())
    Serial.read();

  unsigned long lastPrint = 0;
  const unsigned long printInterval = 300; // ms

  while (true)
  {
    if (Serial.available())
    {
      String line = Serial.readStringUntil('\n');
      line.trim();
      if (line.length() == 0)
      {
        // Enter pressed (empty line) - record step
        return String("");
      }
      else
      {
        // Could be 'x' or other input
        if (line.equalsIgnoreCase("x"))
          return String("x");
        // treat any non-empty as confirmation as well
        return line;
      }
    }

    // Periodically print debug readings if enabled
    unsigned long now = millis();
    if (debugMode && (now - lastPrint >= printInterval))
    {
      ina.readSensors();
      float raw = ina.getRawCurrent_mA();
      float cal = ina.getCurrent_mA();
      Serial.printf("RAW: %.3f mA\tCAL: %.3f mA\n", raw, cal);
      lastPrint = now;
    }
    delay(20);
  }
}

// Renamed from runCalibrationMenu to be more specific
void runCurrentCalibrationMenu(INA226_ADC &ina)
{
  Serial.println(F("\n--- Current Calibration Menu ---"));
  Serial.println(F("Choose installed shunt rating (50-500 A in 50A steps) or 'x' to cancel:"));
  Serial.print(F("> "));

  String sel = SerialReadLineBlocking();
  if (sel.equalsIgnoreCase("x"))
  {
    Serial.println(F("Calibration canceled."));
    return;
  }

  int shuntA = sel.toInt();
  if (shuntA < 50 || shuntA > 500 || (shuntA % 50) != 0)
  {
    Serial.println(F("Invalid shunt rating. Aborting calibration."));
    return;
  }

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
  perc.push_back(0.0005f); // 0.05%
  perc.push_back(0.001f);  // 0.1%
  perc.push_back(0.01f);   // 1%
  perc.push_back(0.02f);   // 2%
  perc.push_back(0.05f);   // 5%
  perc.push_back(0.10f);   // 10%
  perc.push_back(0.25f);   // 25%
  perc.push_back(0.50f);   // 50%

  std::vector<float> measured_mA;
  std::vector<float> true_mA;

  for (size_t i = 0; i < perc.size(); ++i)
  {
    float p = perc[i];
    float trueA = shuntA * p;
    float true_milli = trueA * 1000.0f;
    Serial.printf("\nStep %u of %u: Target = %.3f A (%.2f%% of %dA).\nSet test jig to the target current, then press Enter to record. Enter 'x' to cancel and accept measured so far.\n",
                  (unsigned)(i + 1), (unsigned)perc.size(), trueA, p * 100.0f, shuntA);

    Serial.print("> ");

    // Wait for Enter or 'x', printing debug stream if enabled.
    String line = waitForEnterOrXWithDebug(ina, debugMode);
    if (line.equalsIgnoreCase("x"))
    {
      Serial.println("User canceled early; accepting tests recorded so far.");
      break;
    }

    // Take a short average of raw measurements
    const int samples = 8;
    float sumRaw = 0.0f;
    for (int s = 0; s < samples; ++s)
    {
      ina.readSensors();
      float raw = ina.getRawCurrent_mA();
      sumRaw += raw;
      delay(120);
    }
    float avgRaw = sumRaw / (float)samples;

    Serial.printf("Recorded avg raw reading: %.3f mA  (expected true: %.3f mA)\n", avgRaw, true_milli);

    measured_mA.push_back(avgRaw);
    true_mA.push_back(true_milli);
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
  }

  Serial.println("These values are persisted and will be applied to subsequent current readings.");
}

void printShunt(const struct_message_ae_smart_shunt_1 *p) {
  if (!p) return;

  Serial.printf(
    "=== Local Shunt ===\n"
    "Message ID     : %d\n"
    "Data Changed   : %s\n"
    "Voltage        : %.2f V\n"
    "Current        : %.2f A\n"
    "Power          : %.2f W\n"
    "SOC            : %.1f %%\n"
    "Capacity       : %.2f Ah\n"
    "State          : %d\n"
    "Run Flat Time  : %s\n"
    "===================\n",
    p->messageID,
    p->dataChanged ? "true" : "false",
    p->batteryVoltage,
    p->batteryCurrent,
    p->batteryPower,
    p->batterySOC * 100.0f,
    p->batteryCapacity,
    p->batteryState,
    p->runFlatTime
  );
}

// New function to handle shunt resistance calibration
void runShuntResistanceCalibration(INA226_ADC &ina)
{
  Serial.println(F("\n--- Shunt Resistance Calibration ---"));
  Serial.println(F("Ensure a constant current load is applied."));
  Serial.println(F("This routine will calculate the actual shunt resistance based on measured shunt voltage at various current levels."));

  // Define the sweep of constant current loads in Amps
  std::vector<float> current_loads = {0.1f, 0.5f, 1.0f, 2.0f, 5.0f, 10.0f}; // A
  std::vector<float> measured_voltages;                                     // Store measured voltages in mV

  Serial.println(F("\nFollow the prompts to apply each constant current load."));
  Serial.println(F("Press 'Enter' after applying the load to take a measurement."));
  Serial.println(F("Press 'x' at any time to cancel."));

  for (size_t i = 0; i < current_loads.size(); ++i)
  {
    float current_A = current_loads[i];
    Serial.printf("\nStep %u of %u: Apply a constant current load of %.2f A.\n",
                  (unsigned)(i + 1), (unsigned)current_loads.size(), current_A);

    Serial.print("> ");
    String line = waitForEnterOrXWithDebug(ina, false); // Use waitForEnterOrXWithDebug for user input
    if (line.equalsIgnoreCase("x"))
    {
      Serial.println(F("Shunt resistance calibration canceled."));
      return;
    }

    // Take a short average of voltage measurements
    const int samples = 8;
    float sumShuntVoltage_mV = 0.0f;
    for (int s = 0; s < samples; ++s)
    {
      ina.readSensors();
      sumShuntVoltage_mV += ina.getShuntVoltage_mV();
      delay(120);
    }
    float avgShuntVoltage_mV = sumShuntVoltage_mV / (float)samples;

    Serial.printf("Recorded avg shunt voltage: %.3f mV (at %.2f A)\n", avgShuntVoltage_mV, current_A);
    measured_voltages.push_back(avgShuntVoltage_mV);
  }

  // Calculate the shunt resistance using Ohm's Law (R = V/I) for each data point
  // Then, average the results for a more stable value.
  float sumOhms = 0.0f;
  size_t valid_measurements = 0;
  for (size_t i = 0; i < current_loads.size(); ++i)
  {
    if (current_loads[i] > 0)
    { // Avoid division by zero
      // Convert mV to V for resistance calculation
      float voltage_V = measured_voltages[i] / 1000.0f;
      float resistance_Ohms = voltage_V / current_loads[i];
      sumOhms += resistance_Ohms;
      valid_measurements++;
      Serial.printf("Calculation %u: %.3f V / %.2f A = %.9f Ohms\n",
                    (unsigned)(i + 1), voltage_V, current_loads[i], resistance_Ohms);
    }
  }

  if (valid_measurements > 0)
  {
    float newShuntOhms = sumOhms / valid_measurements;

    // Save the new resistance
    ina.saveShuntResistance(newShuntOhms);
    Serial.printf("\nCalculated new average shunt resistance: %.9f Ohms.\n", newShuntOhms);
    Serial.println("This value has been saved and will be used for all future calculations.");
  }
  else
  {
    Serial.println("\nNo valid measurements were taken. Shunt resistance calibration failed.");
  }

  // Prompt to run current calibration
  Serial.println(F("\nShunt resistance calibration is complete. Would you like to run the current calibration now? (y/N)"));
  Serial.print(F("> "));
  String response = SerialReadLineBlocking();
  if (response.equalsIgnoreCase("y"))
  {
    runCurrentCalibrationMenu(ina);
  }
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
  if (input.length() > 0) {
    new_lv_cutoff = input.toFloat();
    if (new_lv_cutoff < 7.0 || new_lv_cutoff > 12.0) {
      Serial.println(F("Invalid value. Please enter a value between 7.0 and 12.0."));
      return;
    }
  } else {
    new_lv_cutoff = current_lv_cutoff;
  }

  // --- Hysteresis ---
  Serial.print(F("Enter Hysteresis (Volts) [default: "));
  Serial.print(current_hysteresis);
  Serial.print(F("]: "));
  input = SerialReadLineBlocking();
  if (input.length() > 0) {
    new_hysteresis = input.toFloat();
    if (new_hysteresis < 0.1 || new_hysteresis > 2.0) {
      Serial.println(F("Invalid value. Please enter a value between 0.1 and 2.0."));
      return;
    }
  } else {
    new_hysteresis = current_hysteresis;
  }

  // --- Overcurrent Threshold ---
  Serial.print(F("Enter Overcurrent Threshold (Amps) [default: "));
  Serial.print(current_oc_thresh);
  Serial.print(F("]: "));
  input = SerialReadLineBlocking();
  if (input.length() > 0) {
    new_oc_thresh = input.toFloat();
    if (new_oc_thresh < 1.0 || new_oc_thresh > 200.0) {
      Serial.println(F("Invalid value. Please enter a value between 1.0 and 200.0."));
      return;
    }
  } else {
    new_oc_thresh = current_oc_thresh;
  }

  // --- Save Settings ---
  ina.setProtectionSettings(new_lv_cutoff, new_hysteresis, new_oc_thresh);
  Serial.println(F("Protection settings updated."));
}


void onDataSent(const uint8_t *mac_addr, esp_now_send_status_t status)
{
  Serial.print("Last Packet Send Status: ");
  Serial.println(status == ESP_NOW_SEND_SUCCESS ? "Success" : "Fail");
}

void setup()
{
  Serial.begin(115200);
  delay(100); // let Serial start

#ifdef USE_WIFI
  // WiFi connection
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED)
  {
    Serial.print(".");
    delay(500);
  }
  Serial.println("\nConnected to WiFi");

  // Initialise OTA
  wifi_client.setCACert(OTAGH_CA_CERT); // Set the api.github.com SSL cert on the WiFi Client
  OTA::init(wifi_client);
  handleOTA();
#endif

  // The begin method now handles loading the calibrated resistance
  ina226_adc.begin(6, 10);

  // Attach interrupt for INA226 alert pin
  attachInterrupt(digitalPinToInterrupt(INA_ALERT_PIN), alertISR, FALLING);

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
  Serial.println("Calibration summary:");
  for (int sh = 50; sh <= 500; sh += 50)
  {
    float g, o;
    size_t cnt = 0;
    bool hasTbl = ina226_adc.hasStoredCalibrationTable(sh, cnt);
    bool hasLin = ina226_adc.getStoredCalibrationForShunt(sh, g, o);
    if (hasTbl)
    {
      Serial.printf("  %dA: TABLE present (%u pts)", sh, (unsigned)cnt);
      if (hasLin)
        Serial.printf(", linear fallback gain=%.6f offset_mA=%.3f", g, o);
      Serial.println();
    }
    else if (hasLin)
    {
      Serial.printf("  %dA: LINEAR gain=%.6f offset_mA=%.3f\n", sh, g, o);
    }
    else
    {
      Serial.printf("  %dA: No saved calibration (using defaults)\n", sh);
    }
  }
  // Also print currently applied linear calibration (table is runtime-based)
  float curG, curO;
  ina226_adc.getCalibration(curG, curO);
  Serial.printf("Active linear fallback: gain=%.9f offset_mA=%.3f\n", curG, curO);

  // Initialize ESP-NOW
  if (!espNowHandler.begin())
  {
    Serial.println("ESP-NOW init failed");
    return;
  }

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

#ifndef USE_ADC
  // Code to use victron BLE
  bleHandler.startScan(scanTime);
#endif

  Serial.println("Setup done");
}

void loop()
{
  if (ina226_adc.isAlertTriggered()) {
    ina226_adc.processAlert();
  }

  daily_ota_check();

  // Check serial for calibration command
  if (Serial.available())
  {
    String s = Serial.readStringUntil('\n');
    s.trim();
    if (s.equalsIgnoreCase("c"))
    {
      // run the current calibration menu
      runCurrentCalibrationMenu(ina226_adc);
    }
    else if (s.equalsIgnoreCase("r"))
    {
      // run the new shunt resistance calibration
      runShuntResistanceCalibration(ina226_adc);
    }
    else if (s.equalsIgnoreCase("p"))
    {
      // run the protection configuration menu
      runProtectionConfigMenu(ina226_adc);
    }
    // else ignore — keep running
  }

  if (millis() - last_loop_millis > loop_interval)
  {
#ifdef USE_ADC
    ina226_adc.checkAndHandleProtection();

    ina226_adc.readSensors();

    // Populate struct fields
    ae_smart_shunt_struct.messageID = 11;
    ae_smart_shunt_struct.dataChanged = true;

    ae_smart_shunt_struct.batteryVoltage = ina226_adc.getBusVoltage_V();
    ae_smart_shunt_struct.batteryCurrent = ina226_adc.getCurrent_mA() / 1000.0f;
    ae_smart_shunt_struct.batteryPower = ina226_adc.getPower_mW() / 1000.0f;

    ae_smart_shunt_struct.batteryState = 0; // 0 = Normal, 1 = Warning, 2 = Critical

    // Update remaining capacity in the INA226 helper (expects current in A)
    ina226_adc.updateBatteryCapacity(ina226_adc.getCurrent_mA() / 1000.0f);

    // Get remaining Ah from INA helper
    float remainingAh = ina226_adc.getBatteryCapacity();
    ae_smart_shunt_struct.batteryCapacity = remainingAh; // remaining capacity in Ah
    // batteryCapacity global holds the rated capacity in Ah for SOC calculation
    if (batteryCapacity > 0.0f)
    {
      ae_smart_shunt_struct.batterySOC = remainingAh / batteryCapacity; // fraction 0..1
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

    strncpy(ae_smart_shunt_struct.runFlatTime, avgRunFlatTimeStr.c_str(), sizeof(ae_smart_shunt_struct.runFlatTime));
    ae_smart_shunt_struct.runFlatTime[sizeof(ae_smart_shunt_struct.runFlatTime) - 1] = '\0'; // ensure null termination

#else
    // Code to use victron BLE
    bleHandler.startScan(scanTime);

    // Provide safe defaults so struct is still valid
    ae_smart_shunt_struct.messageID = 0;
    ae_smart_shunt_struct.dataChanged = false;
    ae_smart_shunt_struct.batteryVoltage = 0.0f;
    ae_smart_shunt_struct.batteryCurrent = 0.0f;
    ae_smart_shunt_struct.batteryPower = 0.0f;
    ae_smart_shunt_struct.batteryCapacity = 0.0f;
    ae_smart_shunt_struct.batterySOC = 0.0f;
    ae_smart_shunt_struct.batteryState = 0;
    strncpy(ae_smart_shunt_struct.runFlatTime, "N/A", sizeof(ae_smart_shunt_struct.runFlatTime));
#endif

    // Send the data via ESP-NOW
    espNowHandler.setAeSmartShuntStruct(ae_smart_shunt_struct);
    espNowHandler.sendMessageAeSmartShunt();

#ifdef USE_ADC
    printShunt(&ae_smart_shunt_struct);
    if (ina226_adc.isOverflow())
    {
      Serial.println("Warning: Overflow condition!");
    }
#endif

    Serial.println();
    last_loop_millis = millis();
  }
}
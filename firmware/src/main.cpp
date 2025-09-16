#include <vector>
#include <Arduino.h>
#include <Preferences.h>
#include "shared_defs.h"
#include "ina226_adc.h"
#include "ble_handler.h"
#include "espnow_handler.h"
#include "gpio_adc.h"
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

// LED Heartbeat
unsigned long last_led_blink = 0;
const unsigned long led_blink_interval = 500; // ms

struct_message_ae_smart_shunt_1 ae_smart_shunt_struct;
// Initializing with a default shunt resistor value, which will be overwritten
// if a calibrated value is loaded from NVS.
// The default resistance value here is a fallback and will be overwritten by either a
// custom calibrated value from NVS or the factory default for the active shunt.
INA226_ADC ina226_adc(I2C_ADDRESS, 0.000789840f, 100.00f);

// ADC for the starter battery voltage on GPIO3
GPIO_ADC starter_adc(3);

ESPNowHandler espNowHandler(broadcastAddress); // ESP-NOW handler for sending data
WiFiClientSecure wifi_client;

void IRAM_ATTR alertISR()
{
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
      Serial.printf("RAW: %8.3f mA\tCAL: %8.3f mA   \r", raw, cal);
      lastPrint = now;
    }
    delay(20);
  }
}

void runStarterADC_Calibration(GPIO_ADC &adc) {
  Serial.println(F("\n--- Starter Battery ADC Calibration ---"));
  Serial.println(F("You will need a precise power supply."));
  Serial.println(F("Press 'x' at any time to cancel."));

  // --- Step 1: 10V ---
  Serial.println(F("\nStep 1 of 2: Set power supply to 10.0V"));
  Serial.println(F("Press Enter when ready."));
  Serial.print("> ");
  if (SerialReadLineBlocking().equalsIgnoreCase("x")) {
    Serial.println(F("Canceled."));
    return;
  }
  // Read raw ADC value
  int raw_adc1 = analogRead(3); // Reading from GPIO3
  Serial.printf("  -> Recorded raw ADC value: %d\n", raw_adc1);

  // --- Step 2: 14V ---
  Serial.println(F("\nStep 2 of 2: Set power supply to 14.0V"));
  Serial.println(F("Press Enter when ready."));
  Serial.print("> ");
  if (SerialReadLineBlocking().equalsIgnoreCase("x")) {
    Serial.println(F("Canceled."));
    return;
  }
  // Read raw ADC value
  int raw_adc2 = analogRead(3); // Reading from GPIO3
  Serial.printf("  -> Recorded raw ADC value: %d\n", raw_adc2);

  // --- Calculate and Save ---
  adc.calibrate(10.0f, raw_adc1, 14.0f, raw_adc2);
  Serial.println(F("\nCalibration complete and saved."));
}

// Forward declaration for the calibration function
void runShuntResistanceCalibration(INA226_ADC &ina);

// This is the main calibration entry point, combining shunt selection,
// resistance calibration, and current table calibration.
void runCurrentCalibrationMenu(INA226_ADC &ina)
{
  // Ensure load is enabled for calibration
  ina.setLoadConnected(true, MANUAL);
  Serial.println(F("Load enabled for calibration."));

  Serial.println(F("\n--- Calibration Menu ---"));
  Serial.println(F("Step 1: Choose installed shunt rating (50-500 A in 50A steps) or 'x' to cancel:"));
  Serial.print(F("> "));

  String sel = SerialReadLineBlocking();
  if (sel.equalsIgnoreCase("x")) {
    Serial.println(F("Calibration canceled."));
    return;
  }

  int shuntA = sel.toInt();
  if (shuntA < 50 || shuntA > 500 || (shuntA % 50) != 0) {
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
      // This is where the old table-based calibration logic would go.
      // For now, let's just add a placeholder.
      Serial.println(F("Table-based calibration is not yet implemented in this menu."));
    } else if (choice.equalsIgnoreCase("X")) {
      Serial.println(F("Exiting calibration menu."));
      return;
    } else {
      Serial.println(F("Invalid choice."));
    }
  }
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
      p->runFlatTime);
}

// New function to handle shunt resistance calibration
void runShuntResistanceCalibration(INA226_ADC &ina)
{
  // Ensure load is enabled for calibration
  ina.setLoadConnected(true, MANUAL);
  Serial.println(F("Load enabled for calibration."));

  Serial.println(F("\n--- Shunt Resistance Calibration ---"));
  Serial.println(F("This routine will calculate the actual shunt resistance based on a few measurements."));
  Serial.println(F("You will need an external multimeter to measure the true current at each step."));
  Serial.println(F("Press 'x' at any time to cancel."));

  float true_a_zero = 0.0f, v_shunt_zero_mv = 0.0f;
  float true_a_1 = 0.0f, v_shunt_1_mv = 0.0f;
  float true_a_5 = 0.0f, v_shunt_5_mv = 0.0f;
  String line;

  // --- Step 1: No Load ---
  Serial.println(F("\n--- Step 1 of 3: No Load ---"));
  Serial.println(F("1. Disconnect all external loads from the shunt."));
  Serial.println(F("2. Measure the current with your multimeter."));
  Serial.print(F("3. Enter the measured current in Amps (e.g., 0.025) and press Enter: "));

  line = SerialReadLineBlocking();
  if (line.equalsIgnoreCase("x")) { Serial.println(F("Canceled.")); return; }
  true_a_zero = line.toFloat();

  const int samples = 8;
  float sum_v_zero = 0;
  for (int s = 0; s < samples; ++s) {
    ina.readSensors();
    sum_v_zero += ina.getShuntVoltage_mV();
    delay(120);
  }
  v_shunt_zero_mv = sum_v_zero / samples;
  Serial.printf("  -> Recorded avg shunt voltage: %.6f mV (for true current %.6f A)\n", v_shunt_zero_mv, true_a_zero);

  // --- Step 2: ~1A Load ---
  Serial.println(F("\n--- Step 2 of 3: ~1A Load ---"));
  Serial.println(F("1. Apply a constant load of approximately 1A."));
  Serial.println(F("2. Measure the total current with your multimeter."));
  Serial.print(F("3. Enter the measured current in Amps (e.g., 1.025) and press Enter: "));

  line = SerialReadLineBlocking();
  if (line.equalsIgnoreCase("x")) { Serial.println(F("Canceled.")); return; }
  true_a_1 = line.toFloat();

  float sum_v_1a = 0;
  for (int s = 0; s < samples; ++s) {
    ina.readSensors();
    sum_v_1a += ina.getShuntVoltage_mV();
    delay(120);
  }
  v_shunt_1_mv = sum_v_1a / samples;
  Serial.printf("  -> Recorded avg shunt voltage: %.6f mV (for true current %.6f A)\n", v_shunt_1_mv, true_a_1);

  // --- Step 3: ~5A Load ---
  Serial.println(F("\n--- Step 3 of 3: ~5A Load ---"));
  Serial.println(F("1. Apply a constant load of approximately 5A."));
  Serial.println(F("2. Measure the total current with your multimeter."));
  Serial.print(F("3. Enter the measured current in Amps (e.g., 5.025) and press Enter: "));

  line = SerialReadLineBlocking();
  if (line.equalsIgnoreCase("x")) { Serial.println(F("Canceled.")); return; }
  true_a_5 = line.toFloat();

  float sum_v_5a = 0;
  for (int s = 0; s < samples; ++s) {
    ina.readSensors();
    sum_v_5a += ina.getShuntVoltage_mV();
    delay(120);
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

  Serial.printf("Resistance from ~1A load: (%.6f mV / 1000) / %.6f A = %.9f Ohms\n", delta_v_1, delta_i_1, r_1a);
  Serial.printf("Resistance from ~5A load: (%.6f mV / 1000) / %.6f A = %.9f Ohms\n", delta_v_5, delta_i_5, r_5a);

  if (r_1a <= 0 || r_5a <= 0) {
    Serial.println(F("\n[ERROR] Calculated resistance is zero or negative. This can happen if the load was not applied correctly or if the 'no load' voltage was higher than the load voltage. Please try again."));
    return;
  }

  float newShuntOhms = (r_1a + r_5a) / 2.0f;

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

  // --- Save Settings ---
  ina.setProtectionSettings(new_lv_cutoff, new_hysteresis, new_oc_thresh);
  Serial.println(F("Protection settings updated."));
}

void runExportCalibrationMenu(INA226_ADC &ina)
{
  Serial.println(F("\n--- Export Calibration Data ---"));
  Serial.println(F("Choose shunt rating to export (50-500 A):"));
  Serial.print(F("> "));

  String sel = SerialReadLineBlocking();
  if (sel.equalsIgnoreCase("x"))
  {
    Serial.println(F("Export canceled."));
    return;
  }

  int shuntA = sel.toInt();
  if (shuntA < 50 || shuntA > 500 || (shuntA % 50) != 0)
  {
    Serial.println(F("Invalid shunt rating. Aborting export."));
    return;
  }

  if (!ina.loadCalibrationTable(shuntA))
  {
    Serial.printf("No calibration table found for %dA shunt. Cannot export.\n", shuntA);
    return;
  }

  const std::vector<CalPoint> &table = ina.getCalibrationTable();
  if (table.empty())
  {
    Serial.printf("Calibration table for %dA shunt is empty. Nothing to export.\n", shuntA);
    return;
  }

  Serial.println(F("\n--- Copy the following C++ code ---"));
  Serial.printf("std::vector<CalPoint> preCalibratedPoints_%d = {\n", shuntA);
  for (const auto &point : table)
  {
    Serial.printf("    {%.6f, %.6f},\n", point.raw_mA, point.true_mA);
  }
  Serial.println("};");
  Serial.println(F("--- End of C++ code ---"));
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

  pinMode(LED_PIN, OUTPUT);

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
    else if (s.equalsIgnoreCase("v"))
    {
      // run the starter battery ADC calibration
      runStarterADC_Calibration(starter_adc);
    }
    else if (s.equalsIgnoreCase("p"))
    {
      // run the protection configuration menu
      runProtectionConfigMenu(ina226_adc);
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
    else if (s.equalsIgnoreCase("e"))
    {
      // export calibration data
      runExportCalibrationMenu(ina226_adc);
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
    else if (s.equalsIgnoreCase("i"))
    {
      // toggle current inversion
      ina226_adc.toggleInvertCurrent();
    }
    // else ignore — keep running
  }

  if (millis() - last_loop_millis > loop_interval)
  {
#ifdef USE_ADC
    if (ina226_adc.isConfigured())
    {
      ina226_adc.checkAndHandleProtection();
    }

    ina226_adc.readSensors();

    // Populate struct fields
    ae_smart_shunt_struct.messageID = 11;
    ae_smart_shunt_struct.dataChanged = true;

    ae_smart_shunt_struct.batteryVoltage = ina226_adc.getBusVoltage_V();
    ae_smart_shunt_struct.batteryCurrent = ina226_adc.getCurrent_mA() / 1000.0f;
    ae_smart_shunt_struct.batteryPower = ina226_adc.getPower_mW() / 1000.0f;
    ae_smart_shunt_struct.starterBatteryVoltage = starter_adc.readVoltage();

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

    // Send the data via ESP-NOW if configured
    if (ina226_adc.isConfigured())
    {
      Serial.println("Mesh transmission: ready!");
      espNowHandler.setAeSmartShuntStruct(ae_smart_shunt_struct);
      espNowHandler.sendMessageAeSmartShunt();
    }
    else
    {
      Serial.println("Mesh transmission: ADC not configured, skipping!");
    }

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
#include "ota_handler.h"
#include <WiFi.h>
#include <Preferences.h>
#include <esp_now.h>
#include <ota-github-defaults.h>
#include <ota-github-cacerts.h>
#include <OTA-Hub.hpp>

OtaHandler::OtaHandler(BLEHandler& bleHandler, ESPNowHandler& espNowHandler, INA226_ADC& ina226_adc, struct_message_ae_smart_shunt_1& shunt_struct, WiFiClientSecure& wifi_client)
    : bleHandler(bleHandler), espNowHandler(espNowHandler), ina226_adc(ina226_adc), ae_smart_shunt_struct(shunt_struct), wifi_client(wifi_client) {}

void OtaHandler::begin() {
    wifi_client.setCACert(OTAGH_CA_CERT);
    OTA::init(wifi_client);
    Serial.println("[OTA_HANDLER] OTA Handler initialized.");

    // Construct the update URL and set it on the BLE characteristic
    String update_url = String(STR(OTAGH_OWNER_NAME)) + "/" + String(STR(OTAGH_REPO_NAME));
    bleHandler.updateUpdateUrl(update_url);
    Serial.printf("[OTA_HANDLER] Update URL %s set on BLE characteristic.\n", update_url.c_str());
}

void OtaHandler::loop() {
    if (ota_triggered) {
        Serial.println("[OTA_HANDLER] OTA trigger detected. Initiating update process.");
        ota_triggered = false; // Reset trigger
        runBleOtaUpdate();
    }
}

void OtaHandler::setWifiSsid(const String& ssid) {
    Serial.println("[OTA_HANDLER] wifiSsidCallback received.");
    wifi_ssid = ssid;
    Serial.printf("[OTA_HANDLER] WiFi SSID set to: %s\n", wifi_ssid.c_str());
}

void OtaHandler::setWifiPass(const String& pass) {
    Serial.println("[OTA_HANDLER] wifiPassCallback received.");
    wifi_pass = pass;
    Serial.println("[OTA_HANDLER] WiFi password has been set.");
}

void OtaHandler::triggerOta() {
    Serial.println("[OTA_HANDLER] otaTriggerCallback received with value: true");
    Serial.println("[OTA_HANDLER] OTA trigger has been set to true.");
    ota_triggered = true;
}

void OtaHandler::runBleOtaUpdate() {
    Serial.println("[OTA] runBleOtaUpdate function started.");
    if (wifi_ssid.length() == 0)
    {
        Serial.println("[OTA_ERROR] WiFi SSID is empty. Aborting update.");
        return;
    }

    Serial.printf("[OTA] Attempting update with SSID: %s\n", wifi_ssid.c_str());

    strncpy(ae_smart_shunt_struct.runFlatTime, "Checking for updates...", sizeof(ae_smart_shunt_struct.runFlatTime));
    espNowHandler.setAeSmartShuntStruct(ae_smart_shunt_struct);
    espNowHandler.sendMessageAeSmartShunt();
    delay(100);

    esp_now_deinit();
    Serial.println("[OTA] ESP-NOW de-initialized to enable WiFi.");

    WiFi.begin(wifi_ssid.c_str(), wifi_pass.c_str());
    Serial.print("[OTA] Connecting to WiFi");

    int connect_tries = 0;
    while (WiFi.status() != WL_CONNECTED)
    {
        Serial.print(".");
        delay(500);
        connect_tries++;
        if (connect_tries > 20)
        {
            Serial.println("\n[OTA_ERROR] Failed to connect to WiFi after 10 seconds.");
            WiFi.disconnect(true);
            WiFi.mode(WIFI_OFF);
            Serial.println("[OTA] Re-initializing ESP-NOW after failed WiFi connection.");
            if (!espNowHandler.begin())
            {
                Serial.println("ESP-NOW init failed after OTA attempt.");
            }
            return;
        }
    }
    Serial.println("\n[OTA] Connected to WiFi successfully.");

    Serial.println("[OTA] Calling handleOTA() to check for updates.");
    bool updated = handleOTA();
    Serial.printf("[OTA] handleOTA() returned. Update status: %s\n", updated ? "SUCCESS (restarting)" : "NO_UPDATE_OR_FAILURE");

    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    Serial.println("[OTA] WiFi disconnected.");

    if (!updated)
    {
        Serial.println("[OTA] No update performed. Re-initializing ESP-NOW.");
        if (!espNowHandler.begin())
        {
            Serial.println("ESP-NOW init failed after OTA check");
        }
    }
}

bool OtaHandler::handleOTA() {
  bleHandler.updateOtaStatus("CHECKING");
  OTA::UpdateObject details = OTA::isUpdateAvailable();

  if (OTA::NO_UPDATE == details.condition)
  {
    Serial.println("No new update available. Continuing...");
    bleHandler.updateOtaStatus("NO_UPDATE");
    return false;
  }
  else
  {
    Serial.println("Update available, saving battery capacity...");
    bleHandler.updateOtaStatus("DOWNLOADING");
    Preferences preferences;
    preferences.begin("storage", false);
    float capacity = ina226_adc.getBatteryCapacity();
    preferences.putFloat("bat_cap", capacity);
    preferences.end();
    Serial.printf("Saved battery capacity: %f\n", capacity);

    if (OTA::performUpdate(&details) == OTA::SUCCESS)
    {
      bleHandler.updateOtaStatus("SUCCESS");
      delay(100);
      return true;
    }
  }

  bleHandler.updateOtaStatus("FAILURE");
  return false;
}
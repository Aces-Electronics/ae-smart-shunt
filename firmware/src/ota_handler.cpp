#include "ota_handler.h"
#include <WiFi.h>
#include <esp_now.h>
#include <ota-github-defaults.h>
#include <ota-github-cacerts.h>
#include <OTA-Hub.hpp>
#include <ArduinoJson.h>

OtaHandler::OtaHandler(BLEHandler& bleHandler, ESPNowHandler& espNowHandler, WiFiClientSecure& wifi_client)
    : bleHandler(bleHandler), espNowHandler(espNowHandler), wifi_client(wifi_client) {}

void OtaHandler::begin() {
    wifi_client.setCACert(OTAGH_CA_CERT);
    OTA::init(wifi_client);
    Serial.println("[OTA_HANDLER] OTA Handler initialized.");
}

void OtaHandler::setPreUpdateCallback(std::function<void()> callback) {
    pre_update_callback = callback;
}

void OtaHandler::loop() {
    // The loop is no longer needed as OTA is now triggered by BLE commands.
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

void OtaHandler::handleOtaControl(uint8_t command) {
    Serial.printf("[OTA_HANDLER] Received OTA control command: %d\n", command);
    switch (command) {
        case 1: // Check for update
            checkForUpdate();
            break;
        case 2: // Start the update process
            startUpdate();
            break;
        default:
            Serial.printf("[OTA_HANDLER] Unknown OTA command: %d\n", command);
            break;
    }
}

void OtaHandler::checkForUpdate() {
    Serial.println("[OTA] Check for update sequence started.");
    bleHandler.updateOtaStatus(1); // 1: Checking for update

    if (wifi_ssid.length() == 0) {
        Serial.println("[OTA_ERROR] WiFi SSID is empty. Aborting.");
        bleHandler.updateOtaStatus(5); // 5: Update failed
        return;
    }

    esp_now_deinit();
    WiFi.begin(wifi_ssid.c_str(), wifi_pass.c_str());

    int connect_tries = 0;
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        connect_tries++;
        if (connect_tries > 20) {
            Serial.println("\n[OTA_ERROR] Failed to connect to WiFi.");
            WiFi.disconnect(true);
            WiFi.mode(WIFI_OFF);
            espNowHandler.begin();
            bleHandler.updateOtaStatus(5); // 5: Update failed
            return;
        }
    }

    Serial.println("\n[OTA] Connected to WiFi. Checking for updates...");
    latest_update_details = OTA::isUpdateAvailable();

    if (OTA::NO_UPDATE == latest_update_details.condition) {
        Serial.println("No new update available.");
        bleHandler.updateOtaStatus(3); // 3: No update available
    } else {
        Serial.printf("Update available: %s\n", latest_update_details.tag_name.c_str());

        // Create JSON for release metadata
        JsonDocument doc;
        doc["version"] = latest_update_details.tag_name;
        doc["notes"] = latest_update_details.release_notes;
        String metadata;
        serializeJson(doc, metadata);

        bleHandler.updateReleaseMetadata(metadata);
        bleHandler.updateOtaStatus(2); // 2: Update available
    }

    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    espNowHandler.begin();
}

void OtaHandler::startUpdate() {
    Serial.println("[OTA] Start update sequence initiated.");

    if (latest_update_details.condition != OTA::NEW_DIFFERENT && latest_update_details.condition != OTA::NEW_SAME) {
        Serial.println("[OTA_ERROR] No update details available or no new update. Run 'check for update' first.");
        bleHandler.updateOtaStatus(5); // 5: Update failed
        return;
    }

    bleHandler.updateOtaStatus(4); // 4: Update in progress

    esp_now_deinit();
    WiFi.begin(wifi_ssid.c_str(), wifi_pass.c_str());

    int connect_tries = 0;
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        connect_tries++;
        if (connect_tries > 20) {
            Serial.println("\n[OTA_ERROR] Failed to connect to WiFi for update.");
            WiFi.disconnect(true);
            WiFi.mode(WIFI_OFF);
            espNowHandler.begin();
            bleHandler.updateOtaStatus(5); // 5: Update failed
            return;
        }
    }

    if (pre_update_callback) {
        Serial.println("[OTA_HANDLER] Executing pre-update callback.");
        pre_update_callback();
    }

    auto progress_callback = [&](size_t downloaded, size_t total) {
        if (total > 0) {
            uint8_t percentage = (uint8_t)((downloaded * 100) / total);
            bleHandler.updateOtaProgress(percentage);
        }
    };

    if (OTA::performUpdate(&latest_update_details, true, true, progress_callback) == OTA::SUCCESS) {
        bleHandler.updateOtaStatus(6); // 6: Update successful, rebooting
        delay(1000); // Allow time for BLE notification to send
        ESP.restart();
    } else {
        Serial.println("[OTA_ERROR] OTA::performUpdate failed.");
        bleHandler.updateOtaStatus(5); // 5: Update failed
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
        espNowHandler.begin();
    }
}

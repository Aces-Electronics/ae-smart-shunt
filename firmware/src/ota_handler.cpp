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
    
    // Load WiFi Config
    Preferences p;
    p.begin("ota", true); 
    wifi_ssid = p.getString("w_ssid", "");
    wifi_pass = p.getString("w_pass", "");
    p.end();

    Serial.printf("[OTA_HANDLER] Initialized. Loaded WiFi SSID: '%s'\n", wifi_ssid.c_str());
}

void OtaHandler::setPreUpdateCallback(std::function<void()> callback) {
    pre_update_callback = callback;
}

void OtaHandler::loop() {
    if (check_for_update_pending) {
        check_for_update_pending = false; // Reset flag
        checkForUpdate();
    }
    if (start_update_pending) {
        start_update_pending = false; // Reset flag
        startUpdate();
    }

    // Handle WiFi timeout
    if (ota_state == OTA_UPDATE_AVAILABLE && (millis() - ota_wifi_start_time > 120000)) { // 2 minute timeout
        Serial.println("[OTA_HANDLER] Timed out waiting for start command. Disconnecting WiFi.");
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
        espNowHandler.begin();
        ota_state = OTA_IDLE;
        bleHandler.updateOtaStatus(0); // Set status back to Idle
    }
}

void OtaHandler::setWifiSsid(const String& ssid) {
    Serial.println("[OTA_HANDLER] wifiSsidCallback received.");
    String trimmed_ssid = ssid;
    trimmed_ssid.trim();
    wifi_ssid = trimmed_ssid;
    Serial.printf("[OTA_HANDLER] WiFi SSID set to: '%s'\n", wifi_ssid.c_str());

    Preferences p;
    p.begin("ota", false);
    p.putString("w_ssid", wifi_ssid);
    p.end();

    // Reset OTA state to allow for a new check
    if (ota_state != OTA_IDLE) {
        Serial.println("[OTA_HANDLER] Resetting OTA state due to new SSID.");
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
        espNowHandler.begin();
        ota_state = OTA_IDLE;
        bleHandler.updateOtaStatus(0); // Set status back to Idle
    }
}

void OtaHandler::setWifiPass(const String& pass) {
    Serial.println("[OTA_HANDLER] wifiPassCallback received.");
    wifi_pass = pass;
    Serial.println("[OTA_HANDLER] WiFi password has been set.");

    Preferences p;
    p.begin("ota", false);
    p.putString("w_pass", wifi_pass);
    p.end();

    // Reset OTA state to allow for a new check
    if (ota_state != OTA_IDLE) {
        Serial.println("[OTA_HANDLER] Resetting OTA state due to new password.");
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
        espNowHandler.begin();
        ota_state = OTA_IDLE;
        bleHandler.updateOtaStatus(0); // Set status back to Idle
    }
}

void OtaHandler::handleOtaControl(uint8_t command) {
    Serial.printf("[OTA_HANDLER] Received OTA control command: %d\n", command);
    switch (command) {
        case 1: // Check for update
            check_for_update_pending = true;
            break;
        case 2: // Start the update process
            start_update_pending = true;
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
        delay(500); // Allow time for BLE notification to send
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
            delay(500); // Allow time for BLE notification to send
            return;
        }
    }

    Serial.println("\n[OTA] Connected to WiFi. Checking for updates...");

    checkForUpdateAlreadyConnected();
}

void OtaHandler::checkForUpdateAlreadyConnected() {
    Serial.println("[OTA_HANDLER] Syncing time via NTP for SSL...");
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    
    // Wait for time to be set
    time_t now = time(nullptr);
    int retry = 0;
    while (now < 8 * 3600 * 2 && retry < 20) { // Wait for something better than 1970
        delay(500);
        Serial.print(".");
        now = time(nullptr);
        retry++;
    }
    Serial.println("");

    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) {
        Serial.println("[OTA_ERROR] Failed to obtain time. SSL check may fail.");
    } else {
        Serial.println("[OTA_HANDLER] Time synced.");
    }

    Serial.println("[OTA_HANDLER] Starting version check...");
    latest_update_details = OTA::isUpdateAvailable();

    Serial.printf("[OTA_HANDLER] Update check result: %d (Current: %s)\n", latest_update_details.condition, OTA_VERSION);

    if (OTA::NO_UPDATE == latest_update_details.condition) {
        Serial.println("[OTA_HANDLER] No new update available.");
        bleHandler.updateOtaStatus(3); // 3: No update available
        
        // If we connected WiFi JUST for this check, we'd disconnect here.
        // But if we're called via AlreadyConnected, we let the caller handle WiFi lifecycle.
        if (ota_state != OTA_IDLE) {
            WiFi.disconnect(true);
            WiFi.mode(WIFI_OFF);
            espNowHandler.begin();
            ota_state = OTA_IDLE;
        }
    } else {
        Serial.printf("[OTA_HANDLER] Update available: %s\n", latest_update_details.tag_name.c_str());

        // Create a minimal JSON payload with only the version number
        JsonDocument doc;
        doc["version"] = latest_update_details.tag_name;
        String metadata;
        size_t written = serializeJson(doc, metadata);
        if (written == 0) {
            Serial.println("[OTA_HANDLER] ERROR: serializeJson() failed.");
        } else {
            Serial.printf("[OTA_HANDLER] Generated metadata: %s\n", metadata.c_str());
        }

        bleHandler.updateReleaseMetadata(metadata);
        delay(100);
        bleHandler.updateOtaStatus(2); // 2: Update available
        ota_state = OTA_UPDATE_AVAILABLE;
        ota_wifi_start_time = millis(); // Start the timeout timer

        Serial.println("[OTA_HANDLER] Auto-starting update download...");
        startUpdate();
    }
}

void OtaHandler::startUpdate() {
    Serial.println("[OTA] Start update sequence initiated.");
    Serial.printf("[OTA_HANDLER] Condition at start of update: %d\n", latest_update_details.condition);

    if (latest_update_details.condition != OTA::NEW_DIFFERENT && latest_update_details.condition != OTA::NEW_SAME && latest_update_details.condition != OTA::OLD_DIFFERENT) {
        Serial.println("[OTA_ERROR] No suitable update available. Run 'check for update' first.");
        bleHandler.updateOtaStatus(5); // 5: Update failed
        delay(500); // Allow time for BLE notification to send
        return;
    }

    ota_state = OTA_IN_PROGRESS;
    bleHandler.updateOtaStatus(4); // 4: Update in progress

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
        ota_state = OTA_FAILED;
    }
}

void OtaHandler::startUpdateDirect(const String& url, const String& version, const String& md5) {
    Serial.printf("[OTA_HANDLER] Direct update requested: %s (v%s)\n", url.c_str(), version.c_str());
    
    // Populate the UpdateObject with details provided via MQTT
    latest_update_details.condition = OTA::NEW_DIFFERENT;
    latest_update_details.tag_name = version;
    
    // Parse URL into server and endpoint if it's a full URL
    if (url.startsWith("http")) {
        int protoEnd = url.indexOf("://");
        int pathStart = url.indexOf("/", protoEnd + 3);
        if (pathStart > 0) {
            String host = url.substring(protoEnd + 3, pathStart);
            String path = url.substring(pathStart);
            
            latest_update_details.redirect_server = host;
            latest_update_details.firmware_asset_endpoint = path;
            Serial.printf("[OTA_HANDLER] Parsed URL: Host=%s, Path=%s\n", host.c_str(), path.c_str());
        } else {
            latest_update_details.firmware_asset_endpoint = url;
        }
    } else {
        latest_update_details.firmware_asset_endpoint = url;
    }
    
    // Start update
    startUpdate();
}

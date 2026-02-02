#ifndef MQTT_HANDLER_H
#define MQTT_HANDLER_H

#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include "espnow_handler.h"
#include "ina226_adc.h"

// Hardcoded for Proof of Concept as requested. In prod, use NVS/Manager.
// Hardcoded Default
#define DEFAULT_MQTT_BROKER "mqtt.aceselectronics.com.au" 
#define DEFAULT_MQTT_USER "aesmartshunt"
#define DEFAULT_MQTT_PASS "AERemoteAccess2024!"
#define MQTT_PORT 1883
#include <Preferences.h>

class MqttHandler {
public:
    MqttHandler(ESPNowHandler& espNow, INA226_ADC& ina) : _espNow(espNow), _ina(ina), client(espClient) {}

    void begin() {
        // Load Broker from NVS
        Preferences p;
        p.begin("config", true);
        _broker = p.getString("mqtt_broker", DEFAULT_MQTT_BROKER);
        _user = p.getString("mqtt_user", DEFAULT_MQTT_USER);
        _pass = p.getString("mqtt_pass", DEFAULT_MQTT_PASS);
        p.end();
        Serial.printf("[MQTT] Loaded Broker: %s\n", _broker.c_str());

        client.setServer(_broker.c_str(), MQTT_PORT);
        client.setBufferSize(1024); // Increase from default 256 bytes to handle large JSON payloads
        client.setCallback([this](char* topic, uint8_t* payload, unsigned int length) {
            this->callback(topic, payload, length);
        });
    }

    void loop() {
        // We only loop if connected. Main loop handles connection logic.
        if (client.connected()) {
            client.loop();
        }
    }

    bool connect() {
        if (client.connected()) return true;

        String clientId = "AEShunt-" + WiFi.macAddress();
        // connect(clientId, user, pass, willTopic, willQos, willRetain, willMessage, cleanSession)
        // Set cleanSession = false to request persistent session (queuing of QoS 1+ messages)
        if (client.connect(clientId.c_str(), _user.c_str(), _pass.c_str(), 0, 0, 0, 0, false)) {
            Serial.println("MQTT Connected");
            String subTopic = "ae/downlink/" + WiFi.macAddress() + "/#";
            // Subscribe with QoS 1 to ensure delivery of queued messages
            client.subscribe(subTopic.c_str(), 1);
            return true;
        }
        return false;
    }

    bool sendUplink(const struct_message_ae_smart_shunt_1& shuntStruct) {
        if (!client.connected()) {
            Serial.println("[MQTT] ERROR: Not connected, cannot send uplink");
            return false;
        }

        JsonDocument doc; // ArduinoJson v7
        doc["gateway_mac"] = WiFi.macAddress();
        doc["timestamp"] = millis();
        doc["fw_version"] = String(OTA_VERSION); // Global FW version (for the gateway itself)

        JsonArray sensors = doc["sensors"].to<JsonArray>();

        // 1. Shunt Data - Full struct serialization
        JsonObject shunt = sensors.add<JsonObject>();
        shunt["mac"] = WiFi.macAddress();
        shunt["type"] = "shunt";
        
        // Core telemetry
        shunt["volts"] = shuntStruct.mesh.batteryVoltage;
        shunt["amps"] = shuntStruct.mesh.batteryCurrent;
        shunt["amps_avg"] = shuntStruct.mesh.batteryCurrentAvg; // NEW: Averaged Current
        shunt["power"] = shuntStruct.mesh.batteryPower;
        shunt["soc"] = shuntStruct.mesh.batterySOC * 100.0f; // Convert 0-1 to 0-100
        shunt["capacity_ah"] = shuntStruct.mesh.batteryCapacity;
        shunt["state"] = shuntStruct.mesh.batteryState;
        shunt["run_flat_time"] = String(shuntStruct.mesh.runFlatTime);
        shunt["rssi"] = WiFi.RSSI();
        
        // Starter battery
        shunt["starter_volts"] = shuntStruct.mesh.starterBatteryVoltage;
        
        // Calibration status
        shunt["calibrated"] = shuntStruct.mesh.isCalibrated;
        
        // Energy stats
        shunt["last_hour_wh"] = shuntStruct.mesh.lastHourWh;
        shunt["last_day_wh"] = shuntStruct.mesh.lastDayWh;
        shunt["last_week_wh"] = shuntStruct.mesh.lastWeekWh;
        
        // Device name
        if (strlen(shuntStruct.mesh.name) > 0) {
            shunt["name"] = String(shuntStruct.mesh.name);
        }
        
        // Hardware version
        shunt["hw_version"] = shuntStruct.mesh.hardwareVersion;
        shunt["fw_version"] = String(OTA_VERSION);
        
        // TPMS Data (if any configured)
        JsonArray tpms = shunt["tpms"].to<JsonArray>();
        for (int i = 0; i < 4; i++) {
            if (shuntStruct.mesh.tpmsLastUpdate[i] != 0xFFFFFFFF && shuntStruct.mesh.tpmsLastUpdate[i] != 0xFFFFFFFE) { // Valid data
                JsonObject tire = tpms.add<JsonObject>();
                tire["index"] = i;
                tire["pressure_psi"] = shuntStruct.mesh.tpmsPressurePsi[i];
                tire["temp_c"] = shuntStruct.mesh.tpmsTemperature[i];
                tire["battery_v"] = shuntStruct.mesh.tpmsVoltage[i];
                tire["age_ms"] = millis() - shuntStruct.mesh.tpmsLastUpdate[i];
            }
        }

        // 2. Temp Sensor (if available) - Sent as separate device for Device Tree visibility
        if (shuntStruct.mesh.tempSensorLastUpdate != 0xFFFFFFFF) {
            JsonObject tempSensor = sensors.add<JsonObject>();
            tempSensor["type"] = "temp";
            
            // Use MAC if available, else derive from Shunt MAC to ensure parent-child relationship
            String tempMac = _espNow.getTempSensorMac();
            if (tempMac.length() > 0) {
                 tempSensor["mac"] = tempMac;
            } else {
                 // Create a pseudo-MAC by toggling the last bit of the Shunt MAC or appending
                 // Note: Ideally we want a valid 12-char hex or 17-char colon-sep string.
                 // Let's modify the Shunt MAC slightly.
                 String base = WiFi.macAddress();
                 // Simple hack: Replace last char with 'F' or '0' if different? 
                 // Better: "AA:BB:CC:DD:EE:FF" -> "AA:BB:CC:DD:EE:FE" (if FF)
                 // Or just append unique suffix if DB allows? Let's just use the Shunt MAC + "-T".
                 // Postgres MACADDR type is strict, but VARCHAR is not. 
                 // Assuming VARCHAR based on "New Gateway" logic.
                 tempSensor["mac"] = base + "-TEMP";
            }

            tempSensor["name"] = (strlen(shuntStruct.mesh.tempSensorName) > 0) ? String(shuntStruct.mesh.tempSensorName) : "Temp Sensor";
            tempSensor["temp"] = shuntStruct.mesh.tempSensorTemperature;
            tempSensor["battery"] = shuntStruct.mesh.tempSensorBatteryLevel;
            tempSensor["age_ms"] = millis() - shuntStruct.mesh.tempSensorLastUpdate;
            tempSensor["interval_ms"] = shuntStruct.mesh.tempSensorUpdateInterval;
            tempSensor["hw_version"] = shuntStruct.tempSensorHardwareVersion;
            tempSensor["fw_version"] = shuntStruct.tempSensorFirmwareVersion;
        }

        // 3. Gauge (if available) - Sent as separate device for visibility
        if (shuntStruct.gaugeLastUpdate != 0xFFFFFFFF) {
            // Check if MAC is non-zero
            bool macValid = false;
            for (int i=0; i<6; i++) { if (shuntStruct.gaugeMac[i] != 0) { macValid = true; break; } }
            
            if (macValid) {
                JsonObject gauge = sensors.add<JsonObject>();
                gauge["type"] = "gauge";
                
                String gMac = "";
                for (int i=0; i<6; i++) {
                    if (i>0) gMac += ":";
                    char buf[3];
                    sprintf(buf, "%02X", shuntStruct.gaugeMac[i]);
                    gMac += buf;
                }
                gauge["mac"] = gMac;
                gauge["name"] = (strlen(shuntStruct.gaugeName) > 0) ? String(shuntStruct.gaugeName) : "AE Gauge";
                gauge["hw_version"] = shuntStruct.gaugeHardwareVersion;
                gauge["fw_version"] = shuntStruct.gaugeFirmwareVersion;
                gauge["age_ms"] = millis() - shuntStruct.gaugeLastUpdate;
            } else {
                 Serial.println("[MQTT] Gauge Update Valid but MAC is ZERO.");
            }
        } else {
            Serial.printf("[MQTT] Gauge skipped: Update Age %u (0xFFFFFFFF = invalid)\n", shuntStruct.gaugeLastUpdate);
        }

        String output;
        serializeJson(doc, output);
        
        String topic = "ae/uplink/" + WiFi.macAddress();
        
        Serial.printf("[MQTT] Payload size: %d bytes\n", output.length());
        
        // CRITICAL FIX: Check if publish succeeded
        bool published = client.publish(topic.c_str(), output.c_str());
        
        if (published) {
            Serial.println("MQTT Uplink Sent: " + output);
            // CRITICAL FIX: Ensure message is flushed to network before WiFi disconnect
            client.loop();
            delay(100); // Give TCP stack time to send
            return true;
        } else {
            Serial.println("[MQTT] ERROR: Publish failed!");
            Serial.println("[MQTT] Attempted payload: " + output);
            return false;
        }
    }

    bool sendCrashLog(String log) {
        if (!client.connected()) return false;
        
        String topic = "ae/crash/" + WiFi.macAddress();
        Serial.println("[MQTT] Sending Crash Log to " + topic);
        
        // Publish logic
        // Payload is just the raw text log, or JSON wrapped?
        // User asked for "special payload". 
        // Backend expects raw payload in 'log' column, but maybe we wrap it?
        // Worker logic: `VALUES ($1, $2)` where $2 is payload. 
        // If payload is text, it saves text.
        // Let's send raw text for simplicity as crash logs are unstructured.
        
        return client.publish(topic.c_str(), log.c_str());
    }

    void setBroker(String broker) {
        _broker = broker;
        Preferences p;
        p.begin("config", false);
        p.putString("mqtt_broker", broker);
        p.end();
        Serial.printf("[MQTT] Broker updated to: %s\n", broker.c_str());
    }

    void setAuth(String user, String pass) {
        _user = user;
        _pass = pass;
        Preferences p;
        p.begin("config", false);
        p.putString("mqtt_user", user);
        p.putString("mqtt_pass", pass);
        p.end();
        Serial.println("[MQTT] Auth updated.");
    }

    String getBroker() { return _broker; }
    String getUser() { return _user; }

    void setOtaHandler(OtaHandler* ota) {
        _ota = ota;
    }

    void setUpdateCallback(std::function<void()> callback) {
        _updateCallback = callback;
    }

private:
    void callback(char* topic, uint8_t* payload, unsigned int length) {
        Serial.printf("Message arrived [%s]\n", topic);
        String top = String(topic);
        
        // Parse Payload
        JsonDocument doc;
        DeserializationError error = deserializeJson(doc, payload, length);

        if (error) {
            Serial.print(F("deserializeJson() failed: "));
            Serial.println(error.c_str());
            return;
        }

        // 1. Handle legacy "ae/device/<MAC>/command"
        if (top.endsWith("/command")) {
            if (doc["cmd"].is<const char*>()) {
                String cmd = doc["cmd"];
                Serial.println("MQTT Legacy Command: " + cmd);

                if (cmd == "check_fw" || cmd == "update") {
                    Serial.println("Triggering Firmware Check...");
                    if (_updateCallback) {
                        _updateCallback();
                    }
                }
            }
        }
        // 2. Handle sub-device "ae/downlink/<MAC>/subdevice/<CHILD_MAC>/OTA"
        else if (top.indexOf("/subdevice/") > 0 && top.endsWith("/OTA")) {
            Serial.println("MQTT: Received Indirect OTA Command for sub-device");
            
            // Extract Child MAC
            int subIdx = top.indexOf("/subdevice/") + 11;
            int nextSlash = top.indexOf("/", subIdx);
            String childMacStr = top.substring(subIdx, nextSlash);
            
            uint8_t childMac[6];
            // Format: "AA:BB:CC:DD:EE:FF" or "AABBCCDDEEFF"
            if (childMacStr.length() == 17) {
                for(int i=0; i<6; i++) {
                    char buf[3] = { childMacStr[i*3], childMacStr[i*3+1], '\0' };
                    childMac[i] = (uint8_t)strtoul(buf, NULL, 16);
                }
            } else if (childMacStr.length() == 12) {
                for(int i=0; i<6; i++) {
                    char buf[3] = { childMacStr[i*2], childMacStr[i*2+1], '\0' };
                    childMac[i] = (uint8_t)strtoul(buf, NULL, 16);
                }
            } else {
                Serial.println("[MQTT] ERROR: Invalid Child MAC in topic: " + childMacStr);
                return;
            }
            
            // Load WiFi Credentials from NVS
            Preferences p;
            p.begin("ota", true);
            String ssid = p.getString("w_ssid", "");
            String pass = p.getString("w_pass", "");
            p.end();
            
            if (ssid.length() == 0) {
                 Serial.println("[MQTT] ERROR: No WiFi credentials saved to relay to child");
                 return;
            }
            
            // Prepare Trigger Struct
            struct_message_ota_trigger trigger;
            memset(&trigger, 0, sizeof(trigger));
            trigger.messageID = 110;
            
            strncpy(trigger.ssid, ssid.c_str(), sizeof(trigger.ssid)-1);
            strncpy(trigger.pass, pass.c_str(), sizeof(trigger.pass)-1);
            
            String url = doc["url"];
            String version = doc["version"];
            String md5 = doc["md5"];
            // Check for force flag
            bool force = doc["force"] | false;
            
            strncpy(trigger.url, url.c_str(), sizeof(trigger.url)-1);
            strncpy(trigger.version, version.c_str(), sizeof(trigger.version)-1);
            strncpy(trigger.md5, md5.c_str(), sizeof(trigger.md5)-1);
            trigger.force = force;
            
            // Dispatch via ESP-NOW (Queued until Radio Stack Restored)
            _espNow.queueOtaTrigger(childMac, trigger);
        }
        // 3. Handle new "ae/downlink/<MAC>/OTA" (Local Device)
        else if (top.endsWith("/OTA")) {
            Serial.println("MQTT: Received Push OTA Command");
            if (_ota) {
                String url = doc["url"];
                String version = doc["version"];
                String md5 = doc["md5"];
                
                if (url.length() > 0 && version.length() > 0) {
                     bool force = doc["force"] | false;
                     _ota->startUpdateDirect(url, version, md5, force);
                } else {
                     Serial.println("[MQTT] ERROR: Invalid OTA payload (missing url/version)");
                }
            } else {
                Serial.println("[MQTT] ERROR: OtaHandler not linked");
            }
        }
    }

    ESPNowHandler& _espNow;
    INA226_ADC& _ina;
    OtaHandler* _ota = nullptr;
    WiFiClient espClient;
    PubSubClient client;
    String _broker;
    String _user;
    String _pass;
    std::function<void()> _updateCallback = nullptr;
};

#endif

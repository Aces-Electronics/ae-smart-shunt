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
        if (client.connect(clientId.c_str(), _user.c_str(), _pass.c_str())) {
            Serial.println("MQTT Connected");
            String subTopic = "ae/device/" + WiFi.macAddress() + "/command";
            client.subscribe(subTopic.c_str());
            return true;
        }
        return false;
    }

    void sendUplink(const struct_message_ae_smart_shunt_1& shuntStruct) {
        if (!client.connected()) {
            Serial.println("[MQTT] ERROR: Not connected, cannot send uplink");
            return;
        }

        JsonDocument doc; // ArduinoJson v7
        doc["gateway_mac"] = WiFi.macAddress();
        doc["timestamp"] = millis();

        JsonArray sensors = doc["sensors"].to<JsonArray>();

        // 1. Shunt Data - Full struct serialization
        JsonObject shunt = sensors.add<JsonObject>();
        shunt["mac"] = WiFi.macAddress();
        shunt["type"] = "shunt";
        
        // Core telemetry
        shunt["volts"] = shuntStruct.batteryVoltage;
        shunt["amps"] = shuntStruct.batteryCurrent;
        shunt["power"] = shuntStruct.batteryPower;
        shunt["soc"] = shuntStruct.batterySOC * 100.0f; // Convert 0-1 to 0-100
        shunt["capacity_ah"] = shuntStruct.batteryCapacity;
        shunt["state"] = shuntStruct.batteryState;
        shunt["run_flat_time"] = String(shuntStruct.runFlatTime);
        
        // Starter battery
        shunt["starter_volts"] = shuntStruct.starterBatteryVoltage;
        
        // Calibration status
        shunt["calibrated"] = shuntStruct.isCalibrated;
        
        // Energy stats
        shunt["last_hour_wh"] = shuntStruct.lastHourWh;
        shunt["last_day_wh"] = shuntStruct.lastDayWh;
        shunt["last_week_wh"] = shuntStruct.lastWeekWh;
        
        // Device name
        if (strlen(shuntStruct.name) > 0) {
            shunt["name"] = String(shuntStruct.name);
        }
        
        // TPMS Data (if any configured)
        JsonArray tpms = shunt["tpms"].to<JsonArray>();
        for (int i = 0; i < 4; i++) {
            if (shuntStruct.tpmsLastUpdate[i] != 0xFFFFFFFF) { // Valid data
                JsonObject tire = tpms.add<JsonObject>();
                tire["index"] = i;
                tire["pressure_psi"] = shuntStruct.tpmsPressurePsi[i];
                tire["temp_c"] = shuntStruct.tpmsTemperature[i];
                tire["battery_v"] = shuntStruct.tpmsVoltage[i];
                tire["age_ms"] = shuntStruct.tpmsLastUpdate[i];
            }
        }

        // 2. Temp Sensor (if available)
        if (shuntStruct.tempSensorLastUpdate != 0xFFFFFFFF) {
            JsonObject tempSensor = sensors.add<JsonObject>();
            tempSensor["type"] = "temp";
            tempSensor["temp"] = shuntStruct.tempSensorTemperature;
            tempSensor["battery"] = shuntStruct.tempSensorBatteryLevel;
            tempSensor["age_ms"] = shuntStruct.tempSensorLastUpdate;
            tempSensor["interval_ms"] = shuntStruct.tempSensorUpdateInterval;
            if (strlen(shuntStruct.tempSensorName) > 0) {
                tempSensor["name"] = String(shuntStruct.tempSensorName);
            }
        }

        String output;
        serializeJson(doc, output);
        
        String topic = "ae/uplink/" + WiFi.macAddress();
        
        // CRITICAL FIX: Check if publish succeeded
        bool published = client.publish(topic.c_str(), output.c_str());
        
        if (published) {
            Serial.println("MQTT Uplink Sent: " + output);
            // CRITICAL FIX: Ensure message is flushed to network before WiFi disconnect
            client.loop();
            delay(100); // Give TCP stack time to send
        } else {
            Serial.println("[MQTT] ERROR: Publish failed!");
            Serial.println("[MQTT] Attempted payload: " + output);
        }
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

private:
    void callback(char* topic, uint8_t* payload, unsigned int length) {
        Serial.printf("Message arrived [%s]\n", topic);
        // Handle commands (Toggle Relay etc)
    }

    ESPNowHandler& _espNow;
    INA226_ADC& _ina;
    WiFiClient espClient;
    PubSubClient client;
    String _broker;
    String _user;
    String _pass;
};

#endif

#ifndef MQTT_HANDLER_H
#define MQTT_HANDLER_H

#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include "espnow_handler.h"
#include "ina226_adc.h"

// Hardcoded for Proof of Concept as requested. In prod, use NVS/Manager.
// Hardcoded Default
#define DEFAULT_MQTT_BROKER "155.138.198.158" 
#define DEFAULT_MQTT_USER "aesmartshunt"
#define DEFAULT_MQTT_PASS "AERemoteAccess2024!"
#define MQTT_PORT 1883
#include <Preferences.h>
#define MQTT_PORT 1883

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

    void sendUplink() {
        if (!client.connected()) return;

        JsonDocument doc; // ArduinoJson v7
        doc["gateway_mac"] = WiFi.macAddress();
        doc["timestamp"] = millis(); // Placeholder

        JsonArray sensors = doc["sensors"].to<JsonArray>();

        // 1. Shunt Data
        JsonObject shunt = sensors.add<JsonObject>();
        shunt["mac"] = WiFi.macAddress();
        shunt["type"] = "shunt";
        shunt["volts"] = _ina.getBusVoltage_V();
        shunt["amps"] = _ina.getCurrent_mA() / 1000.0f;
        shunt["soc"] = (_ina.getMaxBatteryCapacity() > 0) ? (_ina.getBatteryCapacity() / _ina.getMaxBatteryCapacity()) * 100.0f : 0.0f;


        // 2. Temp Sensor (Last Known)
        float temp; uint8_t batt; uint32_t lastUp; uint32_t interval; char name[24];
        _espNow.getTempSensorData(temp, batt, lastUp, interval, name);
        
        if (lastUp > 0) { // Only if we have received data
             JsonObject t = sensors.add<JsonObject>();
             // We don't have MAC for temp sensor stored in getTempSensorData?? 
             // espnow_handler.h only stores values, not MAC.
             // User said "The shunt has all the data already".
             // Assuming we just report it as "Linked Temp".
             t["type"] = "temp";
             t["temp"] = temp;
             t["battery"] = batt;
             if (strlen(name) > 0) t["name"] = name;
             // t["mac"] = ??? -> We need to store MAC in ESPNowHandler
        }

        String output;
        serializeJson(doc, output);
        
        String topic = "ae/uplink/" + WiFi.macAddress();
        client.publish(topic.c_str(), output.c_str());
        Serial.println("MQTT Uplink Sent: " + output);
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

#include <Arduino.h>
#include <Wire.h>
#include <INA226_WE.h> 

INA226_WE ina(0x40);

void setup() {
    Serial.begin(115200);
    while(!Serial && millis() < 3000); // 3s wait for serial
    delay(1000);

    Serial.println("\n--- INA226 Linearity Mapper (Raw Mode) ---");
    Serial.println("Streaming RAW Shunt Voltage (mV) directly from register.");
    Serial.println("No software Gain or Offset is applied.");
    Serial.println("Format: Raw_mV , Bus_V");
    Serial.println("-------------------------------------------------------");
    
    // Enable Load Switch (GPIO 5)
    pinMode(5, OUTPUT);
    digitalWrite(5, HIGH);
    Serial.println("Load Switch Enabled (GPIO 5 HIGH)");

    Wire.begin(6, 10); // SDA=6, SCL=10 (from main.cpp)
    if(!ina.init()){
        Serial.println("Failed to init INA226!");
        while(1) delay(100);
    }
    
    // Set 16x averaging for stable readings, similar to main app
    ina.setAverage(AVERAGE_16);
    ina.setConversionTime(CONV_TIME_1100); // 1.1ms
    
    // Ensure no software scaling
    ina.setCorrectionFactor(1.0); 
    
    Serial.println("Sensor Initialized. Starting stream...");
}

void loop() {
    // Read raw hardware register values converted to mV
    float mv = ina.getShuntVoltage_mV(); 
    float bus = ina.getBusVoltage_V();
    
    // Print in CSV-friendly format
    Serial.printf("RAW_mV: %.4f , Bus: %.2f\n", mv, bus);
    
    delay(500); 
}

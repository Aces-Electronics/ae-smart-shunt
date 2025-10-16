#ifndef INA226_WE_H
#define INA226_WE_H

#include <Arduino.h>

// Define enums that are used in ina226_adc.cpp
enum ina226_averages{
    AVERAGE_1,
    AVERAGE_4,
    AVERAGE_16,
    AVERAGE_64,
    AVERAGE_128,
    AVERAGE_256,
    AVERAGE_512,
    AVERAGE_1024
};

enum ina226_conversion_times{
    CONV_TIME_140,
    CONV_TIME_204,
    CONV_TIME_332,
    CONV_TIME_588,
    CONV_TIME_1100,
    CONV_TIME_2116,
    CONV_TIME_4156,
    CONV_TIME_8244
};

enum ina226_alert_type{
    SHUNT_OVER,
    SHUNT_UNDER,
    CURRENT_OVER,
    CURRENT_UNDER,
    BUS_OVER,
    BUS_UNDER,
    POWER_OVER
};

class INA226_WE {
public:
    // Register addresses
    static const uint8_t INA226_CONF_REG = 0x00;
    static const uint8_t INA226_CAL_REG = 0x05;
    static const uint8_t INA226_MASK_EN_REG = 0x06;
    static const uint8_t INA226_ALERT_LIMIT_REG = 0x07;

    INA226_WE(uint8_t addr);
    void init();
    void waitUntilConversionCompleted();
    void setAverage(ina226_averages averages);
    void setConversionTime(ina226_conversion_times convTime);
    void setResistorRange(float resistor, float current);
    void setCalibration(float resistor, float current_lsb_A);
    void readAndClearFlags();
    void setAlertType(ina226_alert_type type, float limit);
    void enableAlertLatch();
    uint16_t readRegister(uint8_t reg) const;
    void writeRegister(uint8_t reg, uint16_t val);

    // Mock data members - public to allow easy manipulation in tests
    static float mockShuntVoltage_mV;
    static float mockBusVoltage_V;
    static float mockCurrent_mA;
    static float mockBusPower;
    static bool overflow;

    // Mock methods to return the mock data
    float getShuntVoltage_mV() { return mockShuntVoltage_mV; }
    float getBusVoltage_V() { return mockBusVoltage_V; }
    float getCurrent_mA() { return mockCurrent_mA; }
    float getBusPower() { return mockBusPower; }
};

#endif // INA226_WE_H

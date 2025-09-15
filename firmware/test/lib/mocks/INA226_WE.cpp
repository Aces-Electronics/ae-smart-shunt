#include "INA226_WE.h"
#include <map>

// Initialize static mock data members
float INA226_WE::mockShuntVoltage_mV = 0.0;
float INA226_WE::mockBusVoltage_V = 0.0;
float INA226_WE::mockCurrent_mA = 0.0;
float INA226_WE::mockBusPower = 0.0;
bool INA226_WE::overflow = false;

static std::map<uint8_t, uint16_t> mock_registers;

INA226_WE::INA226_WE(uint8_t addr) {
    // Mock constructor
}

void INA226_WE::init() {}
void INA226_WE::waitUntilConversionCompleted() {}
void INA226_WE::setAverage(ina226_averages averages) {}
void INA226_WE::setConversionTime(ina226_conversion_times convTime) {}
void INA226_WE::setResistorRange(float resistor, float current) {}
void INA226_WE::readAndClearFlags() {}

void INA226_WE::setAlertType(ina226_alert_type type, float limit) {
    //
}

void INA226_WE::enableAlertLatch() {
    //
}

uint16_t INA226_WE::readRegister(uint8_t reg) const {
    if (mock_registers.count(reg)) {
        return mock_registers.at(reg);
    }
    return 0;
}

void INA226_WE::writeRegister(uint8_t reg, uint16_t val) {
    mock_registers[reg] = val;
}

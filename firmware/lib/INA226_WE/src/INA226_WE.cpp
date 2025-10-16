/*****************************************************************
* This is a library for the INA226 Current and Power Sensor Module
*
* You'll find an example which should enable you to use the library. 
*
* You are free to use it, change it or build on it. In case you like 
* it, it would be cool if you give it a star.
* 
* If you find bugs, please inform me!
* 
* Written by Wolfgang (Wolle) Ewald
* https://wolles-elektronikkiste.de/en/ina226-current-and-power-sensor (English)
* https://wolles-elektronikkiste.de/ina226 (German)
*
******************************************************************/

#include "INA226_WE.h"
#include <cmath>

namespace {
    inline int16_t _ina226_clamp16(int32_t x){
        if (x >  32767) return  32767;
        if (x < -32768) return -32768;
        return static_cast<int16_t>(x);
    }
}

bool INA226_WE::init(){
    _wire->beginTransmission(i2cAddress);
    if(_wire->endTransmission()){
        return 0;
    }
    reset_INA226();
    calVal = 2048; // default
    writeRegister(INA226_CAL_REG, calVal);
    setAverage(AVERAGE_1);
    setConversionTime(CONV_TIME_1100);
#ifndef INA226_WE_COMPATIBILITY_MODE_
    setMeasureMode(CONTINUOUS);
#else
    setMeasureMode(INA226_CONTINUOUS);
#endif 
    currentDivider_mA = 40.0;
    pwrMultiplier_mW = 0.625;
    convAlert = false;
    limitAlert = false;
    corrFactor = 1.0;
    i2cErrorCode = 0;
    return 1;
}

void INA226_WE::reset_INA226(){
    writeRegister(INA226_CONF_REG, INA226_RST); 
}

void INA226_WE::setCorrectionFactor(float corr){
    corrFactor = corr;
    uint16_t calValCorrected = static_cast<uint16_t>(calVal * corrFactor);
    writeRegister(INA226_CAL_REG, calValCorrected);
}

void INA226_WE::setAverage(INA226_AVERAGES averages){
    deviceAverages = averages;
    uint16_t currentConfReg = readRegister(INA226_CONF_REG);
    currentConfReg &= ~(0x0E00);  
    currentConfReg |= deviceAverages;
    writeRegister(INA226_CONF_REG, currentConfReg);
}

void INA226_WE::setConversionTime(INA226_CONV_TIME shuntConvTime, INA226_CONV_TIME busConvTime){
    uint16_t currentConfReg = readRegister(INA226_CONF_REG);
    currentConfReg &= ~(0x01C0);  
    currentConfReg &= ~(0x0038);
    uint16_t convMask = (static_cast<uint16_t>(shuntConvTime))<<3;
    currentConfReg |= convMask;
    convMask = busConvTime<<6;
    currentConfReg |= convMask;
    writeRegister(INA226_CONF_REG, currentConfReg);
}

void INA226_WE::setConversionTime(INA226_CONV_TIME convTime){
    setConversionTime(convTime, convTime);
}

void INA226_WE::setMeasureMode(INA226_MEASURE_MODE mode){
    deviceMeasureMode = mode;
    uint16_t currentConfReg = readRegister(INA226_CONF_REG);
    currentConfReg &= ~(0x0007);
    currentConfReg |= deviceMeasureMode;
    writeRegister(INA226_CONF_REG, currentConfReg);
}

void INA226_WE::setCurrentRange(INA226_CURRENT_RANGE range){ // deprecated, left for downward compatibility
    deviceCurrentRange = range;      
}

//set resistor and current range independant. resistor value in ohm, current range in A
void INA226_WE::setResistorRange(float resistor, float current_range){
    float current_LSB=current_range/32768.0;

    calVal = 0.00512/(current_LSB*resistor);
    currentDivider_mA = 0.001/current_LSB;
    pwrMultiplier_mW = 1000.0*25.0*current_LSB;

    writeRegister(INA226_CAL_REG, calVal);          
}

float INA226_WE::getShuntVoltage_V(){
    int16_t val;
    val = static_cast<int16_t>(readRegister(INA226_SHUNT_REG));
    return (val * 0.0000025 * corrFactor);  
}

float INA226_WE::getShuntVoltage_mV(){
    int16_t val;
    val = static_cast<int16_t>(readRegister(INA226_SHUNT_REG));
    return (val * 0.0025 * corrFactor); 
}

float INA226_WE::getBusVoltage_V(){
    uint16_t val;
    val = readRegister(INA226_BUS_REG);
    return (val * 0.00125);
}

float INA226_WE::getCurrent_mA(){
    int16_t val;
    val = static_cast<int16_t>(readRegister(INA226_CURRENT_REG));
    return (val / currentDivider_mA);
}

float INA226_WE::getCurrent_A() {
    return (getCurrent_mA()/1000);
}

float INA226_WE::getBusPower(){
    uint16_t val;
    val = readRegister(INA226_PWR_REG);
    return (val * pwrMultiplier_mW);
}

void INA226_WE::startSingleMeasurement(){
    uint16_t val = readRegister(INA226_MASK_EN_REG); // clears CNVR (Conversion Ready) Flag
    val = readRegister(INA226_CONF_REG);
    writeRegister(INA226_CONF_REG, val);        // Starts conversion
    uint16_t convReady = 0x0000;
    unsigned long convStart = millis();
    while(!convReady && ((millis()-convStart) < 2000)){
        convReady = ((readRegister(INA226_MASK_EN_REG)) & 0x0008); // checks if sampling is completed
    }
}

// Don't wait for conversion to complete
void INA226_WE::startSingleMeasurementNoWait(){
    uint16_t val = readRegister(INA226_MASK_EN_REG); // clears CNVR (Conversion Ready) Flag
    val = readRegister(INA226_CONF_REG);
    writeRegister(INA226_CONF_REG, val);        // Starts conversion
}

void INA226_WE::powerDown(){
    confRegCopy = readRegister(INA226_CONF_REG);
#ifndef INA226_WE_COMPATIBILITY_MODE_
    setMeasureMode(POWER_DOWN);
#else
    setMeasureMode(INA226_POWER_DOWN);
#endif     
}

void INA226_WE::powerUp(){
    writeRegister(INA226_CONF_REG, confRegCopy);
    delayMicroseconds(40);  
}

// Returns 1 if conversion is still ongoing
bool INA226_WE::isBusy(){
    return (!(readRegister(INA226_MASK_EN_REG) &0x0008));
}
    
void INA226_WE::waitUntilConversionCompleted(){
    readRegister(INA226_MASK_EN_REG); // clears CNVR (Conversion Ready) Flag
    uint16_t convReady = 0x0000;
    while(!convReady){
        convReady = ((readRegister(INA226_MASK_EN_REG)) & 0x0008); // checks if sampling is completed
    }
}

void INA226_WE::setAlertPinActiveHigh(){
    uint16_t val = readRegister(INA226_MASK_EN_REG);
    val |= 0x0002;
    writeRegister(INA226_MASK_EN_REG, val);
}

void INA226_WE::enableAlertLatch(){
    uint16_t val = readRegister(INA226_MASK_EN_REG);
    val |= 0x0001;
    writeRegister(INA226_MASK_EN_REG, val);
}

void INA226_WE::enableConvReadyAlert(){
    uint16_t val = readRegister(INA226_MASK_EN_REG);
    val |= 0x0400;
    writeRegister(INA226_MASK_EN_REG, val);
}
    
void INA226_WE::setAlertType(INA226_ALERT_TYPE type, float limit){
    deviceAlertType = type;
    int32_t alertCounts = 0;

    auto toCurrentCounts = [&](float amps) -> int32_t {
        if (currentDivider_mA <= 0.0f || calVal == 0) {
            return 0;
        }
        const float currentLsbA = 0.001f / currentDivider_mA;
        const float shuntCounts = amps / currentLsbA;
        const float calScale = 2048.0f / static_cast<float>(calVal);
        return static_cast<int32_t>(std::lround(shuntCounts * calScale));
    };

    switch (type) {
        case SHUNT_OVER:
        case SHUNT_UNDER:
            // limit is in Volts; LSB = 2.5uV -> counts = V / 2.5e-6
            alertCounts = static_cast<int32_t>(std::lround(limit * 400000.0f));
            break;

        case BUS_OVER:
        case BUS_UNDER:
            // limit is in Volts; LSB = 1.25mV -> counts = V / 1.25e-3
            alertCounts = static_cast<int32_t>(std::lround(limit * 800.0f));
            break;

        case CURRENT_OVER:
            deviceAlertType = SHUNT_OVER;
            alertCounts = toCurrentCounts(std::fabs(limit));
            break;

        case CURRENT_UNDER:
            deviceAlertType = SHUNT_UNDER;
            alertCounts = -toCurrentCounts(std::fabs(limit));
            break;

        case POWER_OVER:
            if (pwrMultiplier_mW != 0.0f) {
                alertCounts = static_cast<int32_t>(std::lround(limit / pwrMultiplier_mW));
            } else {
                alertCounts = 0;
            }
            break;
    }

    const int16_t reg = _ina226_clamp16(alertCounts);
    writeRegister(INA226_ALERT_LIMIT_REG, static_cast<uint16_t>(reg));

    uint16_t mask = readRegister(INA226_MASK_EN_REG);
    mask &= 0x07FF;
    mask |= static_cast<uint16_t>(deviceAlertType);
    writeRegister(INA226_MASK_EN_REG, mask);
}


void INA226_WE::readAndClearFlags(){
    uint16_t value = readRegister(INA226_MASK_EN_REG);
    overflow = (value>>2) & 0x0001;
    convAlert = (value>>3) & 0x0001;
    limitAlert = (value>>4) & 0x0001;
}

uint8_t INA226_WE::getI2cErrorCode(){
    return i2cErrorCode;
}
    

/************************************************ 
    private functions
*************************************************/

void INA226_WE::writeRegister(uint8_t reg, uint16_t val){
  _wire->beginTransmission(i2cAddress);
  uint8_t lVal = val & 255;
  uint8_t hVal = val >> 8;
  _wire->write(reg);
  _wire->write(hVal);
  _wire->write(lVal);
  _wire->endTransmission();
}
  
uint16_t INA226_WE::readRegister(uint8_t reg) const {
  uint8_t MSByte = 0, LSByte = 0;
  uint16_t regValue = 0;
  _wire->beginTransmission(i2cAddress);
  _wire->write(reg);
  i2cErrorCode = _wire->endTransmission(false);
  _wire->requestFrom(static_cast<uint8_t>(i2cAddress),static_cast<uint8_t>(2));
  if(_wire->available()){
    MSByte = _wire->read();
    LSByte = _wire->read();
  }
  regValue = (MSByte<<8) + LSByte;
  return regValue;
}

// === Patch: calibration API ===
void INA226_WE::setCalibration(float shunt_ohms, float current_lsb_A) {
    if (shunt_ohms <= 0.0f || current_lsb_A <= 0.0f) {
        return;
    }

    // Per datasheet: CAL = 0.00512 / (current_LSB(A) * Rshunt(Ohms))
    const float rawCal = 0.00512f / (current_lsb_A * shunt_ohms);
    float cal = rawCal;
    if (cal > 65535.0f) cal = 65535.0f;
    if (cal < 1.0f)     cal = 1.0f;
    calVal = static_cast<uint16_t>(std::lround(cal));
    writeRegister(INA226_CAL_REG, calVal);

    // Keep the three in sync
    currentDivider_mA = 0.001f / current_lsb_A;              // factor to convert raw current counts to mA
    pwrMultiplier_mW  = 25.0f * current_lsb_A * 1000.0f;     // mW per power-count (Power LSB = 25 * current_LSB (W))
    Serial.printf("INA226 CAL=%u, currentDivider_mA=%.6f, pwrMultiplier_mW=%.6f\n",
        calVal, currentDivider_mA, pwrMultiplier_mW);
}


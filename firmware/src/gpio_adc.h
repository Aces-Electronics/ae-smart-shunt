#ifndef GPIO_ADC_H
#define GPIO_ADC_H

#include <Arduino.h>
#include <Preferences.h>
#include <vector>

struct VoltageCalPoint {
    int raw_adc;
    float voltage;
};

class GPIO_ADC {
public:
    GPIO_ADC(int pin);
    void begin();
    float readVoltage();
    void calibrate(const std::vector<VoltageCalPoint>& points);
    const std::vector<VoltageCalPoint>& getCalibrationTable() const;
    bool isCalibrated() const;

private:
    int _pin;
    std::vector<VoltageCalPoint> _calibrationTable;

    void loadCalibration();
    void saveCalibration();
};

#endif // GPIO_ADC_H

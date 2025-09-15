#ifndef GPIO_ADC_H
#define GPIO_ADC_H

#include <Arduino.h>

class GPIO_ADC {
public:
    GPIO_ADC(int pin);
    void begin();
    float readVoltage();
    void calibrate(float true_v1, int raw_adc1, float true_v2, int raw_adc2);
    bool isCalibrated() const;

private:
    int _pin;
    float _gain;
    float _offset;
    bool _isCalibrated;

    void loadCalibration();
    void saveCalibration();
};

#endif // GPIO_ADC_H

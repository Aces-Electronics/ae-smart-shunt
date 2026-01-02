#ifndef SHARED_DEFS_H
#define SHARED_DEFS_H

#include <stdint.h>

// Pin definitions
#define LOAD_SWITCH_PIN 5
#define INA_ALERT_PIN 7
#define LED_PIN 4

// NVS keys
#define NVS_CAL_NAMESPACE "ina_cal"
#define NVS_KEY_ACTIVE_SHUNT "active_shunt"
#define NVS_PROTECTION_NAMESPACE "protection"
#define NVS_KEY_LOW_VOLTAGE_CUTOFF "lv_cutoff"
#define NVS_KEY_HYSTERESIS "hysteresis"
#define NVS_KEY_OVERCURRENT "oc_thresh"
#define NVS_KEY_LOW_VOLTAGE_DELAY "lv_delay"
#define NVS_KEY_DEVICE_NAME_SUFFIX "name_suffix"
#define NVS_KEY_COMPENSATION_RESISTANCE "comp_res"
#define NVS_KEY_EFUSE_LIMIT "efuse_limit"

#define I2C_ADDRESS 0x40
const int scanTime = 5;

extern uint8_t broadcastAddress[6];

typedef struct {
  uint16_t vendorID;
  uint8_t beaconType;
  uint8_t unknownData1[3];
  uint8_t victronRecordType;
  uint16_t nonceDataCounter;
  uint8_t encryptKeyMatch;
  uint8_t victronEncryptedData[21];
  uint8_t nullPad;
} __attribute__((packed)) victronManufacturerData;

typedef struct {
   uint8_t deviceState;
   uint8_t outputState;
   uint8_t errorCode;
   uint16_t alarmReason;
   uint16_t warningReason;
   uint16_t inputVoltage;
   uint16_t outputVoltage;
   uint32_t offReason;
   uint8_t  unused[32];
} __attribute__((packed)) victronPanelData;

typedef struct struct_message_voltage0 {
  int messageID;
  bool dataChanged;
  float frontMainBatt1V;
  float frontAuxBatt1V;
  float rearMainBatt1V;
  float rearAuxBatt1V;
  float frontMainBatt1I;
  float frontAuxBatt1I;
  float rearMainBatt1I;
  float rearAuxBatt1I; 
} struct_message_voltage0;

typedef struct struct_message_ae_smart_shunt_1 {
  int messageID;
  bool dataChanged;
  float batteryVoltage;
  float batteryCurrent;
  float batteryPower;
  float batterySOC;
  float batteryCapacity;
  int batteryState;
  char runFlatTime[40];
  float starterBatteryVoltage;
  bool isCalibrated;
  float lastHourWh;
  float lastDayWh;
  float lastWeekWh;
} __attribute__((packed)) struct_message_ae_smart_shunt_1;

#endif // SHARED_DEFS_H

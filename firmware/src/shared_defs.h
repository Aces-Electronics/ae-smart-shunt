#ifndef SHARED_DEFS_H
#define SHARED_DEFS_H

#include <stdint.h>

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

#endif // SHARED_DEFS_H
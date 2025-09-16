#ifndef MOCK_NIMBLE_H
#define MOCK_NIMBLE_H

#include <string>
#include <vector>
#include <map>

// Forward declarations
class BLEServer;
class BLEService;
class BLECharacteristic;
class BLEAdvertising;

class NimBLEUUID {
public:
    NimBLEUUID(const char* uuid) : m_uuid(uuid) {}
    const char* toString() const { return m_uuid.c_str(); }
private:
    std::string m_uuid;
};

namespace NIMBLE_PROPERTY {
    const uint32_t READ = 0x0002;
    const uint32_t NOTIFY = 0x0010;
}

class BLECharacteristic {
public:
    BLECharacteristic(const char* uuid, uint32_t properties) : m_uuid(uuid), m_properties(properties) {}
    void setValue(float value);
    void setValue(bool value);
    void notify();

    // Mock-specific methods
    static void reset();
    const char* getUUID() const { return m_uuid; }
    float getFloatValue() const { return m_floatValue; }
    bool getBoolValue() const { return m_boolValue; }
    int getNotifyCount() const { return m_notifyCount; }


private:
    const char* m_uuid;
    uint32_t m_properties;
    float m_floatValue;
    bool m_boolValue;
    int m_notifyCount = 0;
};

class BLEService {
public:
    BLEService(const char* uuid) : m_uuid(uuid) {}
    BLECharacteristic* createCharacteristic(const char* uuid, uint32_t properties);
    void start();

    // Mock-specific methods
    static void reset();
    const char* getUUID() const { return m_uuid; }
    std::map<std::string, BLECharacteristic*>& getCharacteristics() { return m_characteristics; }

private:
    const char* m_uuid;
    std::map<std::string, BLECharacteristic*> m_characteristics;
};

class BLEServer {
public:
    BLEService* createService(const char* uuid);

    // Mock-specific methods
    static void reset();
    std::map<std::string, BLEService*>& getServices() { return m_services; }

private:
    std::map<std::string, BLEService*> m_services;
};

class BLEAdvertising {
public:
    void addServiceUUID(const char* uuid);
    void setScanResponse(bool response);
    void setMinPreferred(uint16_t value);

    // Mock-specific methods
    static void reset();
    std::vector<std::string>& getServiceUUIDs() { return m_serviceUuids; }
    bool getScanResponse() const { return m_scanResponse; }

private:
    std::vector<std::string> m_serviceUuids;
    bool m_scanResponse = false;
};

class BLEDevice {
public:
    static void init(const std::string& deviceName);
    static BLEServer* createServer();
    static BLEAdvertising* getAdvertising();
    static void startAdvertising();

    // Mock-specific methods
    static void reset();
    static std::string getDeviceName() { return m_deviceName; }
    static bool isAdvertising() { return m_isAdvertising; }
    static BLEServer* getServer() { return m_pServer; }
    static BLEAdvertising* getAdvertisingMock() { return m_pAdvertising; }


private:
    static std::string m_deviceName;
    static bool m_isAdvertising;
    static BLEServer* m_pServer;
    static BLEAdvertising* m_pAdvertising;
};

// Mock implementations would go in a .cpp file
// For testing, we might include the .cpp file directly in the test runner.

#endif // MOCK_NIMBLE_H

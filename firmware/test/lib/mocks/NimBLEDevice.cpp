#include "NimBLEDevice.h"

// Initialize static members
std::string BLEDevice::m_deviceName;
bool BLEDevice::m_isAdvertising = false;
BLEServer* BLEDevice::m_pServer = nullptr;
BLEAdvertising* BLEDevice::m_pAdvertising = nullptr;

// BLECharacteristic
void BLECharacteristic::setValue(float value) { m_floatValue = value; }
void BLECharacteristic::setValue(bool value) { m_boolValue = value; }
void BLECharacteristic::notify() { m_notifyCount++; }
void BLECharacteristic::reset() {
    // This is tricky with static mocks. A better approach would be instance-based mocks.
    // For this simple case, we'll rely on global reset functions.
}

// BLEService
BLECharacteristic* BLEService::createCharacteristic(const char* uuid, uint32_t properties) {
    auto* pChar = new BLECharacteristic(uuid, properties);
    m_characteristics[uuid] = pChar;
    return pChar;
}
void BLEService::start() {}
void BLEService::reset() {
    // see above
}

// BLEServer
BLEService* BLEServer::createService(const char* uuid) {
    auto* pService = new BLEService(uuid);
    m_services[uuid] = pService;
    return pService;
}
void BLEServer::reset() {
    // see above
}

// BLEAdvertising
void BLEAdvertising::addServiceUUID(const char* uuid) { m_serviceUuids.push_back(uuid); }
void BLEAdvertising::setScanResponse(bool response) { m_scanResponse = response; }
void BLEAdvertising::setMinPreferred(uint16_t value) {}
void BLEAdvertising::reset() {
    // see above
}

// BLEDevice
void BLEDevice::init(const std::string& deviceName) {
    reset(); // Reset all mocks on init
    m_deviceName = deviceName;
}

BLEServer* BLEDevice::createServer() {
    if (!m_pServer) {
        m_pServer = new BLEServer();
    }
    return m_pServer;
}

BLEAdvertising* BLEDevice::getAdvertising() {
    if (!m_pAdvertising) {
        m_pAdvertising = new BLEAdvertising();
    }
    return m_pAdvertising;
}

void BLEDevice::startAdvertising() {
    m_isAdvertising = true;
}

void BLEDevice::reset() {
    m_deviceName = "";
    m_isAdvertising = false;
    if (m_pServer) {
        for (auto const& [key, val] : m_pServer->getServices()) {
            for (auto const& [key2, val2] : val->getCharacteristics()) {
                delete val2;
            }
            delete val;
        }
        m_pServer->getServices().clear();
        delete m_pServer;
        m_pServer = nullptr;
    }
    if (m_pAdvertising) {
        m_pAdvertising->getServiceUUIDs().clear();
        delete m_pAdvertising;
        m_pAdvertising = nullptr;
    }
}

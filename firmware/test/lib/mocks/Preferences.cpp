#include "Preferences.h"

std::map<std::string, Preferences::pref_variant> Preferences::preferences;

bool Preferences::begin(const char* name, bool readOnly) {
    // Mock always succeeds
    return true;
}

void Preferences::end() {
    // Nothing to do
}

void Preferences::clear() {
    preferences.clear();
}

bool Preferences::isKey(const char* key) {
    return preferences.count(key);
}

void Preferences::remove(const char* key) {
    preferences.erase(key);
}

void Preferences::putFloat(const char* key, float value) {
    preferences[key] = value;
}

float Preferences::getFloat(const char* key, float defaultValue) {
    if (preferences.count(key)) {
        return std::get<float>(preferences[key]);
    }
    return defaultValue;
}

void Preferences::putUShort(const char* key, uint16_t value) {
    preferences[key] = value;
}

uint16_t Preferences::getUShort(const char* key, uint16_t defaultValue) {
    if (preferences.count(key)) {
        return std::get<uint16_t>(preferences[key]);
    }
    return defaultValue;
}

void Preferences::putUInt(const char* key, uint32_t value) {
    preferences[key] = value;
}

uint32_t Preferences::getUInt(const char* key, uint32_t defaultValue) {
    if (preferences.count(key)) {
        return std::get<uint32_t>(preferences[key]);
    }
    return defaultValue;
}

void Preferences::putBool(const char* key, bool value) {
    preferences[key] = value;
}

bool Preferences::getBool(const char* key, bool defaultValue) {
    if (preferences.count(key)) {
        return std::get<bool>(preferences[key]);
    }
    return defaultValue;
}

void Preferences::putString(const char* key, String value) {
    preferences[key] = value;
}

String Preferences::getString(const char* key, String defaultValue) {
    if (preferences.count(key)) {
        return std::get<String>(preferences[key]);
    }
    return defaultValue;
}

void Preferences::clear_static() {
    preferences.clear();
}

#ifndef PREFERENCES_H
#define PREFERENCES_H

#include <string>
#include <map>
#include <variant>

class Preferences {
public:
    bool begin(const char* name, bool readOnly = false);
    void end();
    void clear();
    bool isKey(const char* key);
    void remove(const char* key);

    void putFloat(const char* key, float value);
    float getFloat(const char* key, float defaultValue);

    void putUShort(const char* key, uint16_t value);
    uint16_t getUShort(const char* key, uint16_t defaultValue);

    void putUInt(const char* key, uint32_t value);
    uint32_t getUInt(const char* key, uint32_t defaultValue);

    void putBool(const char* key, bool value);
    bool getBool(const char* key, bool defaultValue);

    static void clear_static();

private:
    using pref_variant = std::variant<float, uint16_t, uint32_t, bool>;
    static std::map<std::string, pref_variant> preferences;
};

#endif // PREFERENCES_H

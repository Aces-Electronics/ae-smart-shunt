#ifndef ARDUINO_H
#define ARDUINO_H

#include <stdint.h>
#include <string>
#include <iostream>
#include <vector>
#include <algorithm>
#include <cstdarg>

// Define missing macros for native builds
#define RTC_DATA_ATTR
#define IRAM_ATTR

// Arduino constants
#define HIGH 0x1
#define LOW  0x0
#define INPUT 0x0
#define OUTPUT 0x1
#define INPUT_PULLUP 0x2
#define HEX 16

// Arduino macros
#define F(string_literal) (string_literal)

// Mock millis() function
unsigned long millis();
void set_mock_millis(unsigned long value);

void delay(unsigned long ms);

class MockSerial {
public:
    void begin(int speed) {}
    void print(const char* msg) { std::cout << msg; }
    void print(const std::string& msg) { std::cout << msg; }
    void print(int val) { std::cout << val; }
    void print(size_t val) { std::cout << val; }
    void print(float val) { std::cout << val; }
    void println(const char* msg) { std::cout << msg << std::endl; }
    void println(const std::string& msg) { std::cout << msg << std::endl; }
    void println(int val) { std::cout << val << std::endl; }
    void println(float val) { std::cout << val << std::endl; }
    void println() { std::cout << std::endl; }
    void println(uint16_t val, int base) { std::cout << val << std::endl; }

    // Variadic printf mock
    void printf(const char* format, ...) {
        char buffer[256];
        va_list args;
        va_start(args, format);
        vsnprintf(buffer, sizeof(buffer), format, args);
        va_end(args);
        std::cout << buffer;
    }
};

extern MockSerial Serial;

// Mock GPIO functions
void pinMode(uint8_t pin, uint8_t mode);
void digitalWrite(uint8_t pin, uint8_t val);
int digitalRead(uint8_t pin);
int analogRead(uint8_t pin);

// Mock GPIO state inspection functions (for tests)
void mock_digital_write_clear();
int mock_digital_write_get_last_value(uint8_t pin);

// Mock sleep functions
void esp_sleep_enable_timer_wakeup(uint64_t time_in_us);
void esp_deep_sleep_start();
void mock_esp_deep_sleep_clear();
bool mock_esp_deep_sleep_called();

// Basic String mock
class String : public std::string {
public:
    String(const char* c_str) : std::string(c_str) {}
    String(const std::string& s) : std::string(s) {}
    String() : std::string() {}
    String(unsigned int val) : std::string(std::to_string(val)) {}
    String(int val) : std::string(std::to_string(val)) {}
    String(long val) : std::string(std::to_string(val)) {}
    String(unsigned long val) : std::string(std::to_string(val)) {}
    String(float val) : std::string(std::to_string(val)) {}
    String(double val) : std::string(std::to_string(val)) {}

    void trim() {
        // basic trim mock
        size_t first = this->find_first_not_of(' ');
        if (std::string::npos == first) {
            this->clear();
            return;
        }
        size_t last = this->find_last_not_of(' ');
        *this = this->substr(first, (last - first + 1));
    }

    bool equalsIgnoreCase(const char* other) const {
        std::string s1(*this);
        std::string s2(other);
        std::transform(s1.begin(), s1.end(), s1.begin(), ::tolower);
        std::transform(s2.begin(), s2.end(), s2.begin(), ::tolower);
        return s1 == s2;
    }

    // Add missing substring method
    String substring(unsigned int beginIndex, unsigned int endIndex) const {
        return this->substr(beginIndex, endIndex - beginIndex);
    }

    // Add missing toFloat method
    float toFloat() const {
        return std::stof(*this);
    }
};

#endif // ARDUINO_H

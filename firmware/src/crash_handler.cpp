#include "crash_handler.h"
#include <esp_debug_helpers.h>
#include <esp_debug_helpers.h>
#include <esp_attr.h>
#include <cstdio>
#include <Preferences.h>
#include <rom/rtc.h>

// RTC Memory - detailed crash info survives reset
// 512 bytes should be enough for a reasonable backtrace
#define CRASH_BUFFER_SIZE 512
#define CRASH_MAGIC 0xDEADBEEF

typedef struct {
    uint32_t magic;
    uint32_t timestamp;
    char buffer[CRASH_BUFFER_SIZE];
} rtc_crash_data_t;

RTC_NOINIT_ATTR rtc_crash_data_t rtc_crash_info;

// Forward decl
void save_crash_info_to_rtc(const char* msg);

// Global Preferences for persistent storage
Preferences crashPrefs;

void crash_handler_init() {
    // Optional: Hook panic handler here if strictly needed, 
    // but often we just rely on the fact that we can capture data 
    // if we control the exception or just rely on 'reset reason' + basic info.
    // Ideally we want to override the panic handler. 
    // Check if we can overwrite `esp_panic_handler`? 
    // PlatformIO/Arduino ESP32 is tricky with overriding internal panic.
    // For now, we mainly use this to PROCESS existing data on boot.
}

// Called on boot to check RTC and move to NVS
void crash_handler_process_on_boot() {
    // 1. Check RTC Magic
    if (rtc_crash_info.magic == CRASH_MAGIC) {
        Serial.println("[CRASH HANDLER] Found crash log in RTC memory!");
        
        crashPrefs.begin("crash", false);
        String existing = crashPrefs.getString("log", "");
        
        // Append new log (with timestamp/count limit?)
        // For simplicity, we just overwrite or keep last 2.
        String newLog = String(rtc_crash_info.buffer);
        
        // Let's just store the LATEST crash for now to save space
        crashPrefs.putString("log", newLog);
        
        Serial.println("[CRASH HANDLER] Saved to NVS:");
        Serial.println(newLog);
        
        crashPrefs.end();
        
        // Clear Magic so we don't re-process
        rtc_crash_info.magic = 0;
    } else {
        // Serial.println("[CRASH HANDLER] No fresh crash log in RTC.");
    }
}

String crash_handler_get_log() {
    crashPrefs.begin("crash", true); // ReadOnly
    String log = crashPrefs.getString("log", "No Crash Log Available");
    crashPrefs.end();
    return log;
}

// Can be called manually or by a custom panic handler
void save_crash_info_to_rtc(const char* msg) {
    rtc_crash_info.magic = CRASH_MAGIC;
    rtc_crash_info.timestamp = millis();
    strncpy(rtc_crash_info.buffer, msg, CRASH_BUFFER_SIZE - 1);
    rtc_crash_info.buffer[CRASH_BUFFER_SIZE - 1] = '\0';
}

extern "C" void __real_esp_panic_handler(void *info);

extern "C" void __wrap_esp_panic_handler(void *info) {
    // Capture basic info. 
    // RISC-V Specific Registers for ESP32-C3
    uint32_t mepc, mcause, mtval, ra;
    asm volatile("csrr %0, mepc" : "=r"(mepc));
    asm volatile("csrr %0, mcause" : "=r"(mcause));
    asm volatile("csrr %0, mtval" : "=r"(mtval));
    asm volatile("mv %0, ra" : "=r"(ra));

    char buf[128]; 
    // Format: Panic at <ms> | MEPC:<hex> CAUSE:<hex> VAL:<hex> RA:<hex>
    snprintf(buf, sizeof(buf), 
        "Panic:%lums\nPC:0x%08lX\nCause:0x%lX\nVal:0x%lX\nRA:0x%lX", 
        millis(), mepc, mcause, mtval, ra
    );
    
    save_crash_info_to_rtc(buf);
    
    // Pass to real handler for printing to Serial and resetting
    __real_esp_panic_handler(info);
}

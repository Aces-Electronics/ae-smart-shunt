#include "crash_handler.h"
#include <esp_debug_helpers.h>
#include <esp_attr.h>
#include <cstdio>
#include <Preferences.h>
#include <rom/rtc.h>

// RTC Memory - detailed crash info survives reset
// Increased to 2048 to accommodate full backtrace
#define CRASH_BUFFER_SIZE 2048
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
    // Optional: Hook panic handler here if strictly needed
}

// Called on boot to check RTC and move to NVS
bool crash_handler_process_on_boot() {
    // 1. Check RTC Magic
    if (rtc_crash_info.magic == CRASH_MAGIC) {
        Serial.println("[CRASH HANDLER] Found crash log in RTC memory!");
        
        crashPrefs.begin("crash", false);
        
        // Save the new log
        String newLog = String(rtc_crash_info.buffer);
        crashPrefs.putString("log", newLog);
        
        Serial.println("[CRASH HANDLER] Saved to NVS:");
        Serial.println(newLog);
        
        crashPrefs.end();
        
        // Clear Magic so we don't re-process
        rtc_crash_info.magic = 0;
        return true; 
    }
    return false;
}

String crash_handler_get_log() {
    crashPrefs.begin("crash", true); // ReadOnly
    String log = crashPrefs.getString("log", "No Crash Log Available");
    crashPrefs.end();
    return log;
}

// Helper to append to RTC buffer safely
void append_to_rtc_buffer(const char* format, ...) {
    if (rtc_crash_info.magic != CRASH_MAGIC) {
        // Initialize if not likely set, though unsafe in panic
        return; 
    }
    
    char buf[128];
    va_list args;
    va_start(args, format);
    vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);

    size_t current_len = strlen(rtc_crash_info.buffer);
    if (current_len + strlen(buf) < CRASH_BUFFER_SIZE - 1) {
        strcat(rtc_crash_info.buffer, buf);
    }
}

extern "C" void __real_esp_panic_handler(void *info);

extern "C" void __wrap_esp_panic_handler(void *info) {
    // Only capture the FIRST panic to avoid loops
    if (rtc_crash_info.magic != CRASH_MAGIC) {
        rtc_crash_info.magic = CRASH_MAGIC;
        rtc_crash_info.timestamp = millis();
        rtc_crash_info.buffer[0] = '\0'; // Initialize empty string

        // Capture Registers
        // RISC-V Specific Registers for ESP32-C3
        uint32_t mepc, mcause, mtval, ra, sp, fp;
        asm volatile("csrr %0, mepc" : "=r"(mepc));
        asm volatile("csrr %0, mcause" : "=r"(mcause));
        asm volatile("csrr %0, mtval" : "=r"(mtval));
        asm volatile("mv %0, ra" : "=r"(ra));
        asm volatile("mv %0, sp" : "=r"(sp));
        asm volatile("mv %0, s0" : "=r"(fp)); // Frame Pointer (s0/fp)

        append_to_rtc_buffer("Panic:%lums\nPC:0x%08lX\nCause:0x%lX\nVal:0x%lX\nRA:0x%lX\nSP:0x%08lX\n\nBacktrace:", 
            millis(), mepc, mcause, mtval, ra, sp
        );

        // Manual Stack Walk for RISC-V (Assumes Frame Pointers enabled)
        // 0(fp) = prev_fp
        // 4(fp) = return_address (ra)
        uint32_t *current_fp = (uint32_t*)fp;
        
        // Include the current PC (mepc) as the first frame usually
        append_to_rtc_buffer("0x%08lX ", mepc);
        
        // Then walk callers
        for (int i = 0; i < 32; i++) {
            // Basic sanity check: pointer must be aligned and in RAM/Cache
            if ((uint32_t)current_fp < 0x3FC00000 || (uint32_t)current_fp & 3) {
                 break;
            }
            
            uint32_t next_fp = current_fp[0]; 
            uint32_t ret_addr = current_fp[1]; 
            
            if (ret_addr == 0) break; // End of stack
            
            append_to_rtc_buffer("0x%08lX ", ret_addr);
            
            if (next_fp == 0 || next_fp <= (uint32_t)current_fp) {
                // Stack should grow up (addresses increase) on return? 
                // Wait, stack grows down. previous frame is at HIGHER address.
                // So next_fp should be > current_fp
                break;
            }
            current_fp = (uint32_t*)next_fp;
        }
        append_to_rtc_buffer("\n");
    }
    
    // Pass to real handler for printing to Serial and resetting
    __real_esp_panic_handler(info);
}

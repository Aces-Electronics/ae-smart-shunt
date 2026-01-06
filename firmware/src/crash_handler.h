#pragma once

#include <Arduino.h>

void crash_handler_init();
void crash_handler_process_on_boot();
String crash_handler_get_log();

// Hook to be called potentially from panic context or just setup
void enable_crash_logging();

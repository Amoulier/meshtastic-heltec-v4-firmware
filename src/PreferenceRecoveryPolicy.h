#pragma once

// Compile the board-independent Heltec preference checks on the native audit
// target as well. This is not an ESP32, NVS, FreeRTOS or low-voltage emulator.
#if defined(HELTEC_V4_NATIVE_STORAGE_AUDIT) && (!defined(PIO_UNIT_TESTING) || !defined(ARCH_PORTDUINO))
#error "HELTEC_V4_NATIVE_STORAGE_AUDIT is restricted to native unit tests"
#endif

#if defined(HELTEC_V4_OLED) || defined(HELTEC_V4_NATIVE_STORAGE_AUDIT)
#define HAS_STRICT_PREFERENCE_RECOVERY 1
#else
#define HAS_STRICT_PREFERENCE_RECOVERY 0
#endif

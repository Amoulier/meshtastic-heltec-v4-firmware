#include "Arduino.h"
#include "esp_sleep.h"
#include "variant.h"

#if defined(HELTEC_V4_OLED)

RTC_DATA_ATTR static bool batteryCriticalLatched = false;

void prepareLowBatterySleep() { batteryCriticalLatched = true; }

static uint16_t readBatteryMillivolts()
{
    pinMode(ADC_CTRL, OUTPUT);
    digitalWrite(ADC_CTRL, ADC_CTRL_ENABLED);
    delay(10);

    analogReadResolution(12);
    analogSetPinAttenuation(BATTERY_PIN, ADC_2_5db);

    uint32_t millivolts = 0;
    constexpr uint8_t samples = 15;
    for (uint8_t i = 0; i < samples; i++) {
        millivolts += analogReadMilliVolts(BATTERY_PIN);
    }

    digitalWrite(ADC_CTRL, !ADC_CTRL_ENABLED);
    return static_cast<uint16_t>((millivolts / samples) * (ADC_MULTIPLIER));
}

static void enterBatteryRecoverySleep()
{
    pinMode(RESET_OLED, OUTPUT);
    digitalWrite(RESET_OLED, LOW);
    pinMode(VEXT_ENABLE, OUTPUT);
    digitalWrite(VEXT_ENABLE, HIGH);
    pinMode(PIN_GPS_EN, OUTPUT);
    digitalWrite(PIN_GPS_EN, !GPS_EN_ACTIVE);
    pinMode(LORA_KCT8103L_PA_CSD, OUTPUT);
    digitalWrite(LORA_KCT8103L_PA_CSD, LOW);
    pinMode(LORA_PA_POWER, OUTPUT);
    digitalWrite(LORA_PA_POWER, LOW);

    esp_sleep_enable_timer_wakeup(static_cast<uint64_t>(BATTERY_CRITICAL_SLEEP_MSEC) * 1000ULL);
    esp_deep_sleep_start();
}

void earlyInitVariant()
{
    const uint16_t batteryMillivolts = readBatteryMillivolts();
    const uint16_t requiredMillivolts =
        batteryCriticalLatched ? BATTERY_CRITICAL_RECOVERY_MILLIVOLTS : BATTERY_CRITICAL_MILLIVOLTS;

    if (batteryMillivolts >= BATTERY_BOOT_GUARD_MIN_MILLIVOLTS && batteryMillivolts <= requiredMillivolts) {
        batteryCriticalLatched = true;
        enterBatteryRecoverySleep();
    }

    batteryCriticalLatched = false;
}

#endif

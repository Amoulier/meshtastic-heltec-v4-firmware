#include "Arduino.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_sleep.h"
#include "power/DeepSleepPolicy.h"
#include "variant.h"

#if defined(HELTEC_V4_OLED) && defined(HELTEC_V4_SOLAR_ROUTER_PROFILE) && HELTEC_V4_SOLAR_ROUTER_PROFILE

RTC_DATA_ATTR static bool batteryCriticalLatched = false;

static_assert(BATTERY_BOOT_GUARD_MIN_MILLIVOLTS < BATTERY_CRITICAL_MILLIVOLTS);
static_assert(BATTERY_CRITICAL_MILLIVOLTS < BATTERY_CRITICAL_RECOVERY_MILLIVOLTS);
static_assert(BATTERY_CRITICAL_SLEEP_MSEC > 0);

void prepareLowBatterySleep() { batteryCriticalLatched = true; }

static void releaseEarlyPinHold(int pin)
{
    if (pin < 0) {
        return;
    }

    const gpio_num_t gpio = static_cast<gpio_num_t>(pin);
#if SOC_RTCIO_HOLD_SUPPORTED
    if (rtc_gpio_is_valid_gpio(gpio)) {
        rtc_gpio_hold_dis(gpio);
        return;
    }
#endif
    if (GPIO_IS_VALID_OUTPUT_GPIO(gpio)) {
        gpio_hold_dis(gpio);
    }
}

static void holdEarlyPinLevel(int pin)
{
    if (pin < 0) {
        return;
    }

    const gpio_num_t gpio = static_cast<gpio_num_t>(pin);
#if SOC_RTCIO_HOLD_SUPPORTED
    if (rtc_gpio_is_valid_gpio(gpio)) {
        rtc_gpio_hold_en(gpio);
        return;
    }
#endif
    if (GPIO_IS_VALID_OUTPUT_GPIO(gpio)) {
        gpio_hold_en(gpio);
    }
}

static void configureAndHoldEarlyPin(int pin, uint8_t level)
{
    if (pin < 0) {
        return;
    }

    // Preload the requested level before releasing a hold inherited from the preceding sleep,
    // then assert it again before installing the new hold.
    pinMode(pin, OUTPUT);
    digitalWrite(pin, level);
    releaseEarlyPinHold(pin);
    pinMode(pin, OUTPUT);
    digitalWrite(pin, level);
    holdEarlyPinLevel(pin);
}

static void releaseBatteryRecoveryHolds()
{
    // A reset during recovery reports an undefined wake cause, so initDeepSleep() will not run its
    // normal release loop. Clear every inherited hold here before resuming a full boot.
    for (int pin = 0; pin <= GPIO_NUM_MAX; pin++) {
        releaseEarlyPinHold(pin);
    }
    gpio_deep_sleep_hold_dis();
}

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

static void forceRadioResetForRecovery()
{
    // earlyInitVariant() can run while an SX1262 retained from normal Router sleep is still receiving.
    // Hold NRESET low only when that state is unknown: the board's external pull-up otherwise costs about 330 uA.
    const gpio_num_t radioReset = static_cast<gpio_num_t>(LORA_RESET);
    gpio_pullup_dis(radioReset);
    gpio_pulldown_dis(radioReset);
#if SOC_RTCIO_HOLD_SUPPORTED
    if (rtc_gpio_is_valid_gpio(radioReset)) {
        rtc_gpio_pullup_dis(radioReset);
        rtc_gpio_pulldown_dis(radioReset);
    }
#endif

    configureAndHoldEarlyPin(LORA_RESET, LOW);
}

static void prepareBatteryRecoveryHardware(bool forceRadioReset)
{
    configureAndHoldEarlyPin(ADC_CTRL, !ADC_CTRL_ENABLED);
    configureAndHoldEarlyPin(RESET_OLED, LOW);
    configureAndHoldEarlyPin(VEXT_ENABLE, HIGH);
    configureAndHoldEarlyPin(PIN_GPS_EN, !GPS_EN_ACTIVE);
    configureAndHoldEarlyPin(LED_POWER, LOW);
    configureAndHoldEarlyPin(LORA_KCT8103L_PA_CSD, LOW);
    configureAndHoldEarlyPin(LORA_PA_POWER, LOW);

    if (forceRadioReset) {
        forceRadioResetForRecovery();
    }

    gpio_deep_sleep_hold_en();
}

__attribute__((noinline)) void variant_shutdown()
{
    if (batteryCriticalLatched) {
        prepareBatteryRecoveryHardware(false);
    }
}

static void enterBatteryRecoverySleep(bool forceRadioReset)
{
    prepareBatteryRecoveryHardware(forceRadioReset);

    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    esp_sleep_enable_timer_wakeup(static_cast<uint64_t>(BATTERY_CRITICAL_SLEEP_MSEC) * 1000ULL);
    esp_deep_sleep_start();
}

void earlyInitVariant()
{
    const bool recoveryWasLatched = batteryCriticalLatched;
    releaseEarlyPinHold(ADC_CTRL);
    releaseEarlyPinHold(BATTERY_PIN);

    const uint16_t batteryMillivolts = readBatteryMillivolts();
    if (shouldUseCriticalBatteryRecovery(batteryMillivolts, recoveryWasLatched,
                                         BATTERY_BOOT_GUARD_MIN_MILLIVOLTS, BATTERY_CRITICAL_MILLIVOLTS,
                                         BATTERY_CRITICAL_RECOVERY_MILLIVOLTS)) {
        batteryCriticalLatched = true;
        const bool radioStateIsKnownSafe = isBatteryRecoveryRadioStateKnownSafe(
            recoveryWasLatched, esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER);
        enterBatteryRecoverySleep(!radioStateIsKnownSafe);
    }

    if (recoveryWasLatched) {
        releaseBatteryRecoveryHolds();
        digitalWrite(LED_POWER, HIGH);
    }

    batteryCriticalLatched = false;
}

#endif

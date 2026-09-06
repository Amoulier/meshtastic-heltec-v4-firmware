#include "variant.h"
#include "Arduino.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_sleep.h"
#include "power/BatteryCriticalPolicy.h"
#include "power/DeepSleepPolicy.h"
#include "power/HeltecV4BatteryAdc.h"
#include "power/HeltecV4BatteryCalibration.h"
#if defined(ARDUINO_USB_CDC_ON_BOOT) && ARDUINO_USB_CDC_ON_BOOT
#include <HWCDC.h>
#endif

#if defined(HELTEC_V4_OLED) && defined(HELTEC_V4_SOLAR_ROUTER_PROFILE) && HELTEC_V4_SOLAR_ROUTER_PROFILE

RTC_DATA_ATTR static bool batteryCriticalLatched = false;

static_assert(BATTERY_BOOT_GUARD_MIN_MILLIVOLTS < BATTERY_CRITICAL_MILLIVOLTS);
static_assert(BATTERY_CRITICAL_MILLIVOLTS < BATTERY_CRITICAL_RECOVERY_MILLIVOLTS);
static_assert(BATTERY_CRITICAL_SLEEP_MSEC > 0);
static_assert(BATTERY_CRITICAL_MILLIVOLTS == HELTEC_V4_SOLAR_CRITICAL_BATTERY_POLICY.cutoffMillivolts);
static_assert(BATTERY_CRITICAL_RECOVERY_MILLIVOLTS == HELTEC_V4_SOLAR_CRITICAL_BATTERY_POLICY.recoveryMillivolts);
static_assert(BATTERY_CRITICAL_READINGS == HELTEC_V4_SOLAR_CRITICAL_BATTERY_POLICY.consecutiveReadings);

void prepareLowBatterySleep()
{
    batteryCriticalLatched = true;
}

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

static bool hasActiveUsbDataHost()
{
#if defined(ARDUINO_USB_CDC_ON_BOOT) && ARDUINO_USB_CDC_ON_BOOT
    // USB Serial/JTAG declares a host from recent SOF traffic. At this early
    // boot point the first watchdog sample can race enumeration, so accept any
    // positive sample across a short bounded window before entering another
    // 60-second recovery sleep. Charge-only supplies still produce no SOF and
    // remain on the conservative path.
    constexpr uint8_t samples = 6;
    for (uint8_t i = 0; i < samples; i++) {
        if (HWCDC::isPlugged()) {
            return true;
        }
        if (i + 1 < samples) {
            delay(10);
        }
    }
    return false;
#else
    return false;
#endif
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

__attribute__((noinline)) void variant_shutdown(bool radioSleepSucceeded)
{
    if (batteryCriticalLatched) {
        prepareBatteryRecoveryHardware(shouldForceRadioResetForCriticalSleep(batteryCriticalLatched, radioSleepSucceeded));
    }
}

static void enterBatteryRecoverySleep(bool forceRadioReset)
{
    prepareBatteryRecoveryHardware(forceRadioReset);

    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    esp_sleep_enable_timer_wakeup(static_cast<uint64_t>(BATTERY_CRITICAL_SLEEP_MSEC) * 1000ULL);
    esp_deep_sleep_start();
}

#endif

#if defined(HELTEC_V4_OLED)
void earlyInitVariant()
{
    const auto calibration = readHeltecV4BatteryCalibration(static_cast<float>(ADC_MULTIPLIER));
    const bool calibrationAvailable = calibration.status == HeltecV4BatteryCalibrationStatus::VALID ||
                                      calibration.status == HeltecV4BatteryCalibrationStatus::MISSING;
    if (calibrationAvailable)
        setActiveHeltecV4AdcMultiplier(calibration.multiplier);
    else
        invalidateHeltecV4AdcCalibration();
#if defined(HELTEC_V4_SOLAR_ROUTER_PROFILE) && HELTEC_V4_SOLAR_ROUTER_PROFILE
    const bool recoveryWasLatched = batteryCriticalLatched;
    releaseEarlyPinHold(ADC_CTRL);
    releaseEarlyPinHold(BATTERY_PIN);

    uint16_t batteryMillivolts = 0;
    const bool validReading = readHeltecV4BatteryMillivolts(calibration.multiplier, batteryMillivolts);
    const bool usbDataHost = hasActiveUsbDataHost();
    if ((!usbDataHost && (!validReading || !calibrationAvailable)) ||
        shouldUseCriticalBatteryRecovery(batteryMillivolts, recoveryWasLatched, BATTERY_BOOT_GUARD_MIN_MILLIVOLTS,
                                         BATTERY_CRITICAL_MILLIVOLTS, BATTERY_CRITICAL_RECOVERY_MILLIVOLTS, usbDataHost)) {
        batteryCriticalLatched = true;
        const bool radioStateIsKnownSafe =
            isBatteryRecoveryRadioStateKnownSafe(recoveryWasLatched, esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER);
        enterBatteryRecoverySleep(!radioStateIsKnownSafe);
    }

    if (recoveryWasLatched) {
        releaseBatteryRecoveryHolds();
        digitalWrite(LED_POWER, HIGH);
    }

    batteryCriticalLatched = false;
#endif
}

#endif

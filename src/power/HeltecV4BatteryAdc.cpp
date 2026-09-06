#include "configuration.h"

#if defined(HELTEC_V4_OLED) || defined(HELTEC_V4_NATIVE_STORAGE_AUDIT)

#include "HeltecV4BatteryAdc.h"
#include <atomic>
#include <cmath>
#include <limits>

namespace
{
#ifdef ADC_MULTIPLIER
constexpr float nominalMultiplier = static_cast<float>(ADC_MULTIPLIER);
#else
constexpr float nominalMultiplier = static_cast<float>(4.9 * 1.045);
#endif
static_assert(nominalMultiplier > 0 && nominalMultiplier <= std::numeric_limits<float>::max());
std::atomic<float> activeMultiplier{nominalMultiplier};
} // namespace

bool resolveHeltecV4AdcMultiplier(float overrideValue, float &resolved)
{
    resolved = 0;
    if (!std::isfinite(overrideValue) || overrideValue < 0)
        return false;
    resolved = overrideValue == 0 ? nominalMultiplier : overrideValue;
    return true;
}

float getActiveHeltecV4AdcMultiplier()
{
    return activeMultiplier.load(std::memory_order_acquire);
}

void setActiveHeltecV4AdcMultiplier(float resolved)
{
    if (std::isfinite(resolved) && resolved > 0)
        activeMultiplier.store(resolved, std::memory_order_release);
}

void invalidateHeltecV4AdcCalibration()
{
    activeMultiplier.store(0, std::memory_order_release);
}

#if defined(HELTEC_V4_OLED) && defined(ARDUINO_ARCH_ESP32)

#include "Arduino.h"
#include "concurrency/Lock.h"
#include "concurrency/LockGuard.h"
#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_cali_scheme.h>
#include <esp_adc/adc_oneshot.h>

namespace
{
constexpr unsigned sampleCount = 15;
constexpr adc_unit_t batteryUnit = ADC_UNIT_1;
constexpr adc_channel_t batteryChannel = ADC_CHANNEL_0;
constexpr adc_atten_t batteryAttenuation = ADC_ATTEN_DB_2_5;
constexpr adc_bitwidth_t batteryBitwidth = ADC_BITWIDTH_12;
static_assert(BATTERY_PIN == 1 && ADC_CHANNEL == ADC_CHANNEL_0);
static_assert(ADC_ATTENUATION == batteryAttenuation);

concurrency::Lock adcLock;
adc_oneshot_unit_handle_t adcHandle = nullptr;
adc_cali_handle_t calibrationHandle = nullptr;
bool adcReady = false;

class BatteryDividerScope
{
  public:
    BatteryDividerScope()
    {
        pinMode(ADC_CTRL, OUTPUT);
        digitalWrite(ADC_CTRL, !ADC_CTRL_ENABLED);
    }
    ~BatteryDividerScope() { digitalWrite(ADC_CTRL, !ADC_CTRL_ENABLED); }
    void enable()
    {
        digitalWrite(ADC_CTRL, ADC_CTRL_ENABLED);
        delay(10);
    }
};

bool releaseAdcResources()
{
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    if (calibrationHandle && adc_cali_delete_scheme_curve_fitting(calibrationHandle) == ESP_OK)
        calibrationHandle = nullptr;
#endif
    if (adcHandle && adc_oneshot_del_unit(adcHandle) == ESP_OK)
        adcHandle = nullptr;
    return !adcHandle && !calibrationHandle;
}

bool initializeAdc()
{
    if (adcReady)
        return true;
    if (!releaseAdcResources())
        return false;

    adc_oneshot_unit_init_cfg_t unitConfig = {};
    unitConfig.unit_id = batteryUnit;
    adc_oneshot_chan_cfg_t channelConfig = {};
    channelConfig.atten = batteryAttenuation;
    channelConfig.bitwidth = batteryBitwidth;
    if (adc_oneshot_new_unit(&unitConfig, &adcHandle) != ESP_OK ||
        adc_oneshot_config_channel(adcHandle, batteryChannel, &channelConfig) != ESP_OK) {
        releaseAdcResources();
        return false;
    }

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t calibrationConfig = {};
    calibrationConfig.unit_id = batteryUnit;
    calibrationConfig.chan = batteryChannel;
    calibrationConfig.atten = batteryAttenuation;
    calibrationConfig.bitwidth = batteryBitwidth;
    if (adc_cali_create_scheme_curve_fitting(&calibrationConfig, &calibrationHandle) == ESP_OK) {
        adcReady = true;
        return true;
    }
#endif
    releaseAdcResources();
    return false;
}
} // namespace

bool readHeltecV4BatteryMillivolts(float resolvedMultiplier, uint16_t &millivolts)
{
    millivolts = 0;
    concurrency::LockGuard guard(&adcLock);
    BatteryDividerScope divider;
    if (!std::isfinite(resolvedMultiplier) || resolvedMultiplier <= 0 || !initializeAdc())
        return false;

    divider.enable();
    uint32_t sum = 0;
    for (unsigned i = 0; i < sampleCount; ++i) {
        int raw = 0;
        int calibrated = 0;
        if (adc_oneshot_read(adcHandle, batteryChannel, &raw) != ESP_OK || raw < 0 || raw > 4095 ||
            adc_cali_raw_to_voltage(calibrationHandle, raw, &calibrated) != ESP_OK || calibrated < 0 ||
            calibrated > std::numeric_limits<uint16_t>::max())
            return false;
        sum += static_cast<uint32_t>(calibrated);
    }

    const double scaled = (static_cast<double>(sum) / sampleCount) * resolvedMultiplier;
    if (!std::isfinite(scaled) || scaled > std::numeric_limits<uint16_t>::max())
        return false;
    millivolts = static_cast<uint16_t>(scaled);
    return true;
}

#endif // HELTEC_V4_OLED && ARDUINO_ARCH_ESP32
#endif // HELTEC_V4_OLED || HELTEC_V4_NATIVE_STORAGE_AUDIT

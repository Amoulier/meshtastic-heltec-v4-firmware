#pragma once

#include <cstdint>

struct CriticalBatteryPolicy {
    uint16_t cutoffMillivolts;
    uint16_t recoveryMillivolts;
    uint8_t consecutiveReadings;
};

constexpr CriticalBatteryPolicy HELTEC_V4_SOLAR_CRITICAL_BATTERY_POLICY = {3500, 3650, 3};

constexpr float initialAnalogBatteryFilteredVoltage(float configuredFloorMillivolts, float sampledMillivolts,
                                                     bool trustFirstSampleBelowFloor)
{
    return trustFirstSampleBelowFloor || sampledMillivolts > configuredFloorMillivolts ? sampledMillivolts
                                                                                       : configuredFloorMillivolts;
}

constexpr bool shouldUseRawBatteryVoltageForCriticalCutoff(bool solarRouterProfile)
{
    return solarRouterProfile;
}

constexpr bool shouldUseRawBatteryVoltageForCriticalCutoff()
{
#if defined(HELTEC_V4_SOLAR_ROUTER_PROFILE) && HELTEC_V4_SOLAR_ROUTER_PROFILE
    return shouldUseRawBatteryVoltageForCriticalCutoff(true);
#else
    return shouldUseRawBatteryVoltageForCriticalCutoff(false);
#endif
}

constexpr int32_t criticalBatteryCutoffVoltage(int32_t filteredVoltageMillivolts, int32_t latestRawAveragedVoltageMillivolts,
                                               bool useRawVoltage)
{
    return useRawVoltage ? latestRawAveragedVoltageMillivolts : filteredVoltageMillivolts;
}

constexpr bool isCriticalBatteryVoltage(int32_t voltageMillivolts, uint16_t cutoffMillivolts)
{
    return voltageMillivolts > 0 && voltageMillivolts <= cutoffMillivolts;
}

constexpr bool shouldConfirmSolarBatteryPresence(int32_t rawBatteryVoltageMillivolts,
                                                 uint16_t criticalMillivolts)
{
    return rawBatteryVoltageMillivolts > criticalMillivolts;
}

constexpr bool shouldEvaluateCriticalBatteryReading(bool batteryPresenceConfirmed,
                                                    bool externalPowerPresent,
                                                    int32_t rawBatteryVoltageMillivolts,
                                                    uint16_t minimumPlausibleMillivolts,
                                                    bool useRawVoltage)
{
    // On the Heltec V4, an open battery input reads about 2.2 V. The Solar
    // Router profile therefore trusts raw ADC at or above the early-boot
    // guard. Below it, only prior healthy-voltage evidence can distinguish an
    // abrupt battery collapse from an open input.
    return !externalPowerPresent &&
           (useRawVoltage ? batteryPresenceConfirmed || rawBatteryVoltageMillivolts >= minimumPlausibleMillivolts
                          : batteryPresenceConfirmed);
}

constexpr uint8_t nextCriticalBatteryReadingCount(uint8_t currentCount, int32_t voltageMillivolts,
                                                  uint16_t cutoffMillivolts, uint8_t requiredReadings)
{
    if (!isCriticalBatteryVoltage(voltageMillivolts, cutoffMillivolts)) {
        return 0;
    }

    return currentCount < requiredReadings ? currentCount + 1 : currentCount;
}

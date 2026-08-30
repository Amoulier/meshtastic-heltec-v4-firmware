#pragma once

#include <cstdint>

enum class DeepSleepWakePolicy : uint8_t { ROLE_DEFAULT, TIMER_ONLY };

constexpr DeepSleepWakePolicy criticalBatteryDeepSleepWakePolicy(bool solarRouterProfile)
{
    return solarRouterProfile ? DeepSleepWakePolicy::TIMER_ONLY : DeepSleepWakePolicy::ROLE_DEFAULT;
}

constexpr DeepSleepWakePolicy criticalBatteryDeepSleepWakePolicy()
{
#if defined(HELTEC_V4_SOLAR_ROUTER_PROFILE) && HELTEC_V4_SOLAR_ROUTER_PROFILE
    return criticalBatteryDeepSleepWakePolicy(true);
#else
    return criticalBatteryDeepSleepWakePolicy(false);
#endif
}

constexpr bool shouldKeepLoraAwakeInDeepSleep(bool hasTimedWake, bool isRouterRole,
                                              DeepSleepWakePolicy policy)
{
    return policy == DeepSleepWakePolicy::ROLE_DEFAULT && hasTimedWake && isRouterRole;
}

constexpr bool shouldEnableExternalWakeInDeepSleep(DeepSleepWakePolicy policy)
{
    return policy == DeepSleepWakePolicy::ROLE_DEFAULT;
}

constexpr bool shouldKeepRtcPeripheralsPoweredInDeepSleep(DeepSleepWakePolicy policy)
{
    return policy == DeepSleepWakePolicy::ROLE_DEFAULT;
}

constexpr bool shouldAssertOnDeepSleepPreflightTimeout(DeepSleepWakePolicy policy)
{
    return policy == DeepSleepWakePolicy::ROLE_DEFAULT;
}

constexpr bool shouldUseCriticalBatteryRecovery(uint16_t batteryMillivolts, bool recoveryLatched,
                                                uint16_t bootGuardMinimumMillivolts,
                                                uint16_t criticalMillivolts, uint16_t recoveryMillivolts)
{
    if (recoveryLatched) {
        return batteryMillivolts < recoveryMillivolts;
    }

    if (batteryMillivolts < bootGuardMinimumMillivolts) {
        return false;
    }

    return batteryMillivolts <= criticalMillivolts;
}

constexpr bool isBatteryRecoveryRadioStateKnownSafe(bool recoveryLatched, bool timerWake)
{
    return recoveryLatched && timerWake;
}

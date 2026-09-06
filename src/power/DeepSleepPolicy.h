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
                                                uint16_t bootGuardMinimumMillivolts, uint16_t criticalMillivolts,
                                                uint16_t recoveryMillivolts, bool externalPowerPresent = false)
{
    if (externalPowerPresent) {
        return false;
    }

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

constexpr bool shouldForceRadioResetForCriticalSleep(bool batteryCriticalLatched, bool radioSleepSucceeded)
{
    return batteryCriticalLatched && !radioSleepSucceeded;
}

// VEXT is a shared OLED/accessory rail on Heltec V4. A persistent OLED disable
// always holds RESET low, but the shared rail may only be removed after a
// completed I2C scan found no accessory.
constexpr bool shouldPowerDownHeltecV4VextForDisplayDisable(bool displayDisabled, bool accessoryDetected)
{
    return displayDisabled && !accessoryDetected;
}

// Cut VEXT on a headless wake
// only when the durable display-off choice is verified and the completed scan,
// run while OLED reset is held low, found no I2C accessory. Any uncertainty is
// fail-safe for accessories and leaves the rail powered.
constexpr bool shouldPowerDownHeltecV4VextOnHeadlessWake(bool headlessTimerWake, bool preferenceReadSucceeded,
                                                         bool displayDisabled, bool accessoryDetected)
{
    return headlessTimerWake && preferenceReadSucceeded &&
           shouldPowerDownHeltecV4VextForDisplayDisable(displayDisabled, accessoryDetected);
}

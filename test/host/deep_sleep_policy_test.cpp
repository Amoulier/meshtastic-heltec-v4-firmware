#include "power/DeepSleepPolicy.h"

static_assert(criticalBatteryDeepSleepWakePolicy(false) == DeepSleepWakePolicy::ROLE_DEFAULT);
static_assert(criticalBatteryDeepSleepWakePolicy(true) == DeepSleepWakePolicy::TIMER_ONLY);
#if defined(HELTEC_V4_SOLAR_ROUTER_PROFILE) && HELTEC_V4_SOLAR_ROUTER_PROFILE
static_assert(criticalBatteryDeepSleepWakePolicy() == DeepSleepWakePolicy::TIMER_ONLY);
#else
static_assert(criticalBatteryDeepSleepWakePolicy() == DeepSleepWakePolicy::ROLE_DEFAULT);
#endif

static_assert(shouldKeepLoraAwakeInDeepSleep(true, true, DeepSleepWakePolicy::ROLE_DEFAULT));
static_assert(!shouldKeepLoraAwakeInDeepSleep(true, true, DeepSleepWakePolicy::TIMER_ONLY));
static_assert(!shouldKeepLoraAwakeInDeepSleep(false, true, DeepSleepWakePolicy::TIMER_ONLY));
static_assert(!shouldKeepLoraAwakeInDeepSleep(false, true, DeepSleepWakePolicy::ROLE_DEFAULT));
static_assert(!shouldKeepLoraAwakeInDeepSleep(true, false, DeepSleepWakePolicy::ROLE_DEFAULT));
static_assert(!shouldKeepLoraAwakeInDeepSleep(true, false, DeepSleepWakePolicy::TIMER_ONLY));

static_assert(shouldEnableExternalWakeInDeepSleep(DeepSleepWakePolicy::ROLE_DEFAULT));
static_assert(!shouldEnableExternalWakeInDeepSleep(DeepSleepWakePolicy::TIMER_ONLY));
static_assert(shouldKeepRtcPeripheralsPoweredInDeepSleep(DeepSleepWakePolicy::ROLE_DEFAULT));
static_assert(!shouldKeepRtcPeripheralsPoweredInDeepSleep(DeepSleepWakePolicy::TIMER_ONLY));
static_assert(shouldAssertOnDeepSleepPreflightTimeout(DeepSleepWakePolicy::ROLE_DEFAULT));
static_assert(!shouldAssertOnDeepSleepPreflightTimeout(DeepSleepWakePolicy::TIMER_ONLY));

static_assert(!shouldUseCriticalBatteryRecovery(0, false, 2500, 3500, 3650));
static_assert(!shouldUseCriticalBatteryRecovery(2499, false, 2500, 3500, 3650));
static_assert(shouldUseCriticalBatteryRecovery(0, true, 2500, 3500, 3650));
static_assert(shouldUseCriticalBatteryRecovery(2499, true, 2500, 3500, 3650));
static_assert(shouldUseCriticalBatteryRecovery(2500, false, 2500, 3500, 3650));
static_assert(shouldUseCriticalBatteryRecovery(3500, false, 2500, 3500, 3650));
static_assert(!shouldUseCriticalBatteryRecovery(3501, false, 2500, 3500, 3650));
static_assert(shouldUseCriticalBatteryRecovery(3649, true, 2500, 3500, 3650));
static_assert(!shouldUseCriticalBatteryRecovery(3650, true, 2500, 3500, 3650));

static_assert(isBatteryRecoveryRadioStateKnownSafe(true, true));
static_assert(!isBatteryRecoveryRadioStateKnownSafe(true, false));
static_assert(!isBatteryRecoveryRadioStateKnownSafe(false, true));
static_assert(!isBatteryRecoveryRadioStateKnownSafe(false, false));

int main()
{
    return 0;
}

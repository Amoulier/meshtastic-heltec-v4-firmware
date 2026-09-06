#include "FilesystemMountPolicy.h"
#include "mesh/RadioRecoveryPolicy.h"
#include "power/BatteryCriticalPolicy.h"
#include "power/DeepSleepPolicy.h"
#include "power/PowerFSMPolicy.h"

static_assert(!shouldAutoFormatFilesystemOnMountFailure(true));
static_assert(shouldAutoFormatFilesystemOnMountFailure(false));
static_assert(!shouldUseFilesystemPersistence(true, false));
static_assert(shouldUseFilesystemPersistence(true, true));
static_assert(shouldUseFilesystemPersistence(false, false));
static_assert(shouldUsePersistentConfiguration(true, false));
static_assert(!shouldUsePersistentConfiguration(false, false));
static_assert(!shouldUsePersistentConfiguration(true, true));
static_assert(!shouldUsePersistentConfiguration(false, true));
static_assert(!persistedIdentityRequiresNodeDatabase(0, 0, 0, false));
static_assert(persistedIdentityRequiresNodeDatabase(1, 0, 0, false));
static_assert(persistedIdentityRequiresNodeDatabase(0, 1, 0, false));
static_assert(persistedIdentityRequiresNodeDatabase(0, 0, 1, false));
static_assert(persistedIdentityRequiresNodeDatabase(0, 0, 0, true));
static_assert(shouldAllowPhonePacketWhileRecovering(true, false, false));
static_assert(shouldAllowPhonePacketWhileRecovering(false, true, true));
static_assert(!shouldAllowPhonePacketWhileRecovering(false, true, false));
static_assert(!shouldAllowPhonePacketWhileRecovering(false, false, true));
static_assert(!shouldAllowPhonePacketWhileRecovering(false, false, false));
static_assert(shouldAllowOtaRequestWhileRecovering(true, true));
static_assert(!shouldAllowOtaRequestWhileRecovering(true, false));
static_assert(!shouldAllowOtaRequestWhileRecovering(false, true));
static_assert(!shouldAllowOtaRequestWhileRecovering(false, false));
static_assert(!shouldAutoEraseLegacyPreferences(true));
static_assert(shouldAutoEraseLegacyPreferences(false));
static_assert(HELTEC_V4_DESTRUCTIVE_STORAGE_MIN_MILLIVOLTS == 3650);
static_assert(!shouldAllowHeltecDestructiveStorageMutation(false, false, false, false, 0));
static_assert(shouldAllowHeltecDestructiveStorageMutation(false, false, true, false, 0));
static_assert(shouldAllowHeltecDestructiveStorageMutation(true, false, true, false, 0));
static_assert(shouldAllowHeltecDestructiveStorageMutation(true, true, true, false, 0));
static_assert(!shouldAllowHeltecDestructiveStorageMutation(true, true, false, false, 4300));
static_assert(!shouldAllowHeltecDestructiveStorageMutation(true, true, false, true, 3649));
static_assert(shouldAllowHeltecDestructiveStorageMutation(true, true, false, true, 3650));
static_assert(HELTEC_V4_PREFERENCE_STORAGE_MIN_MILLIVOLTS == 3300);
static_assert(heltecPreferenceStorageMinimumMillivolts(3100) == 3300);
static_assert(heltecPreferenceStorageMinimumMillivolts(3500) == 3501);
static_assert(heltecPreferenceStorageMinimumMillivolts(UINT16_MAX) == UINT16_MAX);
static_assert(!shouldAllowHeltecPreferenceStorageMutation(false, false, false, false, 0));
static_assert(shouldAllowHeltecPreferenceStorageMutation(false, false, true, false, 0));
static_assert(shouldAllowHeltecPreferenceStorageMutation(true, false, true, false, 0));
static_assert(shouldAllowHeltecPreferenceStorageMutation(true, true, true, false, 0));
static_assert(!shouldAllowHeltecPreferenceStorageMutation(true, true, false, false, 4300));
static_assert(!shouldAllowHeltecPreferenceStorageMutation(true, true, false, true, 3299));
static_assert(shouldAllowHeltecPreferenceStorageMutation(true, true, false, true, 3300));
static_assert(!shouldAllowHeltecPreferenceStorageMutation(true, true, false, true, 3500, 3501));
static_assert(shouldAllowHeltecPreferenceStorageMutation(true, true, false, true, 3501, 3501));
static_assert(shouldQueueHeltecPreferenceWriteRetry(false, false, false));
static_assert(!shouldQueueHeltecPreferenceWriteRetry(true, false, false));
static_assert(!shouldQueueHeltecPreferenceWriteRetry(false, true, false));
static_assert(!shouldQueueHeltecPreferenceWriteRetry(false, false, true));
static_assert(shouldAttemptHeltecDeferredPreferenceRetry(true, false, false, false));
static_assert(!shouldAttemptHeltecDeferredPreferenceRetry(false, false, false, false));
static_assert(!shouldAttemptHeltecDeferredPreferenceRetry(true, true, false, false));
static_assert(!shouldAttemptHeltecDeferredPreferenceRetry(true, false, true, false));
static_assert(!shouldAttemptHeltecDeferredPreferenceRetry(true, false, false, true));
static_assert(!shouldRemovePreferencesBeforeFactoryDefaults(true, false));
static_assert(shouldRemovePreferencesBeforeFactoryDefaults(true, true));
static_assert(shouldRemovePreferencesBeforeFactoryDefaults(false, false));
static_assert(shouldRemovePreferencesBeforeFactoryDefaults(false, true));

#if defined(HELTEC_V4_OLED)
static_assert(!shouldAutoFormatFilesystemOnMountFailure());
static_assert(!shouldUseFilesystemPersistence(false));
static_assert(shouldUseFilesystemPersistence(true));
#else
static_assert(shouldAutoFormatFilesystemOnMountFailure());
static_assert(shouldUseFilesystemPersistence(false));
#endif

constexpr CriticalBatteryPolicy solarBatteryPolicy = HELTEC_V4_SOLAR_CRITICAL_BATTERY_POLICY;
constexpr uint16_t solarBatteryBootGuardMillivolts = 2500;
static_assert(initialAnalogBatteryFilteredVoltage(3500.0f, 2204.0f, true) == 2204.0f);
static_assert(initialAnalogBatteryFilteredVoltage(3500.0f, 2204.0f, false) == 3500.0f);
static_assert(initialAnalogBatteryFilteredVoltage(3500.0f, 3800.0f, true) == 3800.0f);
static_assert(solarBatteryPolicy.cutoffMillivolts == 3500);
static_assert(solarBatteryPolicy.recoveryMillivolts == 3650);
static_assert(solarBatteryPolicy.consecutiveReadings == 3);

static_assert(!shouldUseRawBatteryVoltageForCriticalCutoff(false));
static_assert(shouldUseRawBatteryVoltageForCriticalCutoff(true));
#if defined(HELTEC_V4_SOLAR_ROUTER_PROFILE) && HELTEC_V4_SOLAR_ROUTER_PROFILE
static_assert(shouldUseRawBatteryVoltageForCriticalCutoff());
static_assert(criticalBatteryCutoffVoltage(3800, 3450, shouldUseRawBatteryVoltageForCriticalCutoff()) == 3450);
static_assert(isCriticalBatteryVoltage(criticalBatteryCutoffVoltage(3800, 3450, shouldUseRawBatteryVoltageForCriticalCutoff()),
                                       solarBatteryPolicy.cutoffMillivolts));
#else
static_assert(!shouldUseRawBatteryVoltageForCriticalCutoff());
static_assert(criticalBatteryCutoffVoltage(3800, 3450, shouldUseRawBatteryVoltageForCriticalCutoff()) == 3800);
static_assert(!isCriticalBatteryVoltage(criticalBatteryCutoffVoltage(3800, 3450, shouldUseRawBatteryVoltageForCriticalCutoff()),
                                        solarBatteryPolicy.cutoffMillivolts));
#endif

static_assert(criticalBatteryCutoffVoltage(3400, 3700, false) == 3400);
static_assert(criticalBatteryCutoffVoltage(3400, 3700, true) == 3700);
static_assert(!isCriticalBatteryVoltage(criticalBatteryCutoffVoltage(3400, 3700, true), 3500));
static_assert(isCriticalBatteryVoltage(criticalBatteryCutoffVoltage(3700, 3500, true), 3500));
static_assert(!isCriticalBatteryVoltage(criticalBatteryCutoffVoltage(3700, 3501, true), 3500));

// Standard continues to trust its existing battery-presence gate and ignores
// raw ADC validity. Solar trusts raw ADC down to the boot guard even when the
// derived presence flag has already fallen false below 3.0 V; prior healthy
// evidence also covers an abrupt collapse below that guard.
static_assert(!shouldConfirmSolarBatteryPresence(2204, solarBatteryPolicy.cutoffMillivolts));
static_assert(!shouldConfirmSolarBatteryPresence(3500, solarBatteryPolicy.cutoffMillivolts));
static_assert(shouldConfirmSolarBatteryPresence(3501, solarBatteryPolicy.cutoffMillivolts));
static_assert(shouldEvaluateCriticalBatteryReading(true, false, 2204, solarBatteryBootGuardMillivolts, false));
static_assert(!shouldEvaluateCriticalBatteryReading(false, false, 2900, solarBatteryBootGuardMillivolts, false));
static_assert(shouldEvaluateCriticalBatteryReading(false, false, 2500, solarBatteryBootGuardMillivolts, true));
static_assert(shouldEvaluateCriticalBatteryReading(false, false, 2800, solarBatteryBootGuardMillivolts, true));
static_assert(shouldEvaluateCriticalBatteryReading(false, false, 2999, solarBatteryBootGuardMillivolts, true));
static_assert(shouldEvaluateCriticalBatteryReading(true, false, 2204, solarBatteryBootGuardMillivolts, true));
static_assert(shouldEvaluateCriticalBatteryReading(true, false, 2400, solarBatteryBootGuardMillivolts, true));
static_assert(!shouldEvaluateCriticalBatteryReading(false, false, 2204, solarBatteryBootGuardMillivolts, true));
static_assert(!shouldEvaluateCriticalBatteryReading(false, false, 2400, solarBatteryBootGuardMillivolts, true));
static_assert(!shouldEvaluateCriticalBatteryReading(false, false, 2499, solarBatteryBootGuardMillivolts, true));
static_assert(!shouldEvaluateCriticalBatteryReading(true, true, 2400, solarBatteryBootGuardMillivolts, true));

static_assert(nextCriticalBatteryReadingCount(0, 3500, solarBatteryPolicy.cutoffMillivolts,
                                              solarBatteryPolicy.consecutiveReadings) == 1);
static_assert(nextCriticalBatteryReadingCount(1, 3500, solarBatteryPolicy.cutoffMillivolts,
                                              solarBatteryPolicy.consecutiveReadings) == 2);
static_assert(nextCriticalBatteryReadingCount(2, 3500, solarBatteryPolicy.cutoffMillivolts,
                                              solarBatteryPolicy.consecutiveReadings) == 3);
static_assert(nextCriticalBatteryReadingCount(3, 3500, solarBatteryPolicy.cutoffMillivolts,
                                              solarBatteryPolicy.consecutiveReadings) == 3);
static_assert(nextCriticalBatteryReadingCount(2, 3501, solarBatteryPolicy.cutoffMillivolts,
                                              solarBatteryPolicy.consecutiveReadings) == 0);

constexpr uint8_t nextSolarCriticalReading(uint8_t currentCount, int32_t rawBatteryMillivolts,
                                           bool batteryPresenceConfirmed = false, bool externalPowerPresent = false)
{
    return shouldEvaluateCriticalBatteryReading(batteryPresenceConfirmed, externalPowerPresent,
                                                rawBatteryMillivolts, solarBatteryBootGuardMillivolts, true)
               ? nextCriticalBatteryReadingCount(currentCount, rawBatteryMillivolts,
                                                 solarBatteryPolicy.cutoffMillivolts,
                                                 solarBatteryPolicy.consecutiveReadings)
               : 0;
}

static_assert(nextSolarCriticalReading(0, 0) == 0);
static_assert(nextSolarCriticalReading(2, 2204) == 0);
static_assert(nextSolarCriticalReading(2, 2499) == 0);
static_assert(nextSolarCriticalReading(0, 2500) == 1);
static_assert(nextSolarCriticalReading(0, 2800) == 1);
static_assert(nextSolarCriticalReading(0, 3500) == 1);
static_assert(nextSolarCriticalReading(2, 3501) == 0);
static_assert(nextSolarCriticalReading(2, 2800, false, true) == 0);
static_assert(nextSolarCriticalReading(2, 2400, true, false) == 3);
static_assert(nextSolarCriticalReading(2, 2400, true, true) == 0);
static_assert(nextSolarCriticalReading(nextSolarCriticalReading(nextSolarCriticalReading(0, 2800), 2800), 2800) == 3);
static_assert(nextSolarCriticalReading(nextSolarCriticalReading(nextSolarCriticalReading(0, 3400), 2204), 3400) == 1);
static_assert(nextSolarCriticalReading(nextSolarCriticalReading(nextSolarCriticalReading(0, 3400), 3400, false, true),
                                      3400) == 1);

static_assert(!shouldKeepBluetoothConnectableDuringIdle(false, false));
static_assert(!shouldKeepBluetoothConnectableDuringIdle(false, true));
static_assert(!shouldKeepBluetoothConnectableDuringIdle(true, false));
static_assert(shouldKeepBluetoothConnectableDuringIdle(true, true));
static_assert(!shouldRestartAfterBluetoothDisable(false, false));
static_assert(!shouldRestartAfterBluetoothDisable(false, true));
static_assert(shouldRestartAfterBluetoothDisable(true, false));
static_assert(!shouldRestartAfterBluetoothDisable(true, true));
#if defined(HELTEC_V4_OLED)
static_assert(!shouldKeepBluetoothConnectableDuringIdle(false));
static_assert(shouldKeepBluetoothConnectableDuringIdle(true));
static_assert(shouldRestartAfterBluetoothDisable(false));
static_assert(!shouldRestartAfterBluetoothDisable(true));
#else
static_assert(!shouldKeepBluetoothConnectableDuringIdle(false));
static_assert(!shouldKeepBluetoothConnectableDuringIdle(true));
static_assert(!shouldRestartAfterBluetoothDisable(false));
static_assert(!shouldRestartAfterBluetoothDisable(true));
#endif

static_assert(shouldUseNoBluetoothStateAfterLightSleep(true, true, false));
static_assert(!shouldUseNoBluetoothStateAfterLightSleep(true, true, true));
static_assert(!shouldUseNoBluetoothStateAfterLightSleep(true, false, false));
static_assert(!shouldUseNoBluetoothStateAfterLightSleep(false, true, false));

static_assert(shouldEnterLightSleepFromIdle(true, true, false, false, false, false));
static_assert(shouldEnterLightSleepFromIdle(true, false, true, false, false, false));
static_assert(!shouldEnterLightSleepFromIdle(true, true, false, false, false, true));
static_assert(!shouldEnterLightSleepFromIdle(true, false, true, false, false, true));
static_assert(!shouldEnterLightSleepFromIdle(true, false, false, false, false, false));
static_assert(!shouldEnterLightSleepFromIdle(true, true, false, true, false, false));
static_assert(!shouldEnterLightSleepFromIdle(true, true, false, false, true, false));
static_assert(!shouldEnterLightSleepFromIdle(false, true, false, false, false, false));

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

static_assert(!shouldUseCriticalBatteryRecovery(0, false, 2500, solarBatteryPolicy.cutoffMillivolts,
                                                solarBatteryPolicy.recoveryMillivolts));
static_assert(!shouldUseCriticalBatteryRecovery(2499, false, 2500, solarBatteryPolicy.cutoffMillivolts,
                                                solarBatteryPolicy.recoveryMillivolts));
static_assert(!shouldUseCriticalBatteryRecovery(2204, false, 2500, solarBatteryPolicy.cutoffMillivolts,
                                                solarBatteryPolicy.recoveryMillivolts));
static_assert(shouldUseCriticalBatteryRecovery(0, true, 2500, solarBatteryPolicy.cutoffMillivolts,
                                               solarBatteryPolicy.recoveryMillivolts));
static_assert(shouldUseCriticalBatteryRecovery(2499, true, 2500, solarBatteryPolicy.cutoffMillivolts,
                                               solarBatteryPolicy.recoveryMillivolts));
static_assert(shouldUseCriticalBatteryRecovery(2204, true, 2500, solarBatteryPolicy.cutoffMillivolts,
                                               solarBatteryPolicy.recoveryMillivolts));
static_assert(shouldUseCriticalBatteryRecovery(2500, false, 2500, solarBatteryPolicy.cutoffMillivolts,
                                               solarBatteryPolicy.recoveryMillivolts));
static_assert(shouldUseCriticalBatteryRecovery(3500, false, 2500, solarBatteryPolicy.cutoffMillivolts,
                                               solarBatteryPolicy.recoveryMillivolts));
static_assert(!shouldUseCriticalBatteryRecovery(3501, false, 2500, solarBatteryPolicy.cutoffMillivolts,
                                                solarBatteryPolicy.recoveryMillivolts));
static_assert(shouldUseCriticalBatteryRecovery(3649, true, 2500, solarBatteryPolicy.cutoffMillivolts,
                                               solarBatteryPolicy.recoveryMillivolts));
static_assert(!shouldUseCriticalBatteryRecovery(3650, true, 2500, solarBatteryPolicy.cutoffMillivolts,
                                                solarBatteryPolicy.recoveryMillivolts));
static_assert(!shouldUseCriticalBatteryRecovery(2204, true, 2500, solarBatteryPolicy.cutoffMillivolts,
                                                solarBatteryPolicy.recoveryMillivolts, true));
static_assert(!shouldUseCriticalBatteryRecovery(3400, true, 2500, solarBatteryPolicy.cutoffMillivolts,
                                                solarBatteryPolicy.recoveryMillivolts, true));

static_assert(isBatteryRecoveryRadioStateKnownSafe(true, true));
static_assert(!isBatteryRecoveryRadioStateKnownSafe(true, false));
static_assert(!isBatteryRecoveryRadioStateKnownSafe(false, true));
static_assert(!isBatteryRecoveryRadioStateKnownSafe(false, false));

static_assert(!shouldForceRadioResetForCriticalSleep(false, false));
static_assert(!shouldForceRadioResetForCriticalSleep(false, true));
static_assert(shouldForceRadioResetForCriticalSleep(true, false));
static_assert(!shouldForceRadioResetForCriticalSleep(true, true));

static_assert(!shouldPowerDownHeltecV4VextForDisplayDisable(false, false));
static_assert(!shouldPowerDownHeltecV4VextForDisplayDisable(true, true));
static_assert(shouldPowerDownHeltecV4VextForDisplayDisable(true, false));

static_assert(!shouldPowerDownHeltecV4VextOnHeadlessWake(false, true, true, false));
static_assert(!shouldPowerDownHeltecV4VextOnHeadlessWake(true, false, true, false));
static_assert(!shouldPowerDownHeltecV4VextOnHeadlessWake(true, true, false, false));
static_assert(!shouldPowerDownHeltecV4VextOnHeadlessWake(true, true, true, true));
static_assert(shouldPowerDownHeltecV4VextOnHeadlessWake(true, true, true, false));

static_assert(channelScanAction(true, false, false) == ChannelScanAction::DEFER);
static_assert(channelScanAction(false, true, false) == ChannelScanAction::TRANSMIT);
static_assert(channelScanAction(false, false, false) == ChannelScanAction::RECOVER_AND_RETRY);
static_assert(channelScanAction(true, false, true) == ChannelScanAction::DEFER);
static_assert(channelScanAction(false, true, true) == ChannelScanAction::TRANSMIT);
static_assert(channelScanAction(false, false, true) == ChannelScanAction::DEFER);

int main()
{
    FilesystemMountState mountState;
    constexpr int persistedPreference = 42;
    int loadedPreference = 0;
    bool generatedIdentity = false;
    bool wroteDefaults = false;

    mountState.recordMountResult(false);
    if (shouldUseFilesystemPersistence(true, mountState.mounted())) {
        loadedPreference = persistedPreference;
        generatedIdentity = true;
        wroteDefaults = true;
    }
    if (loadedPreference != 0 || generatedIdentity || wroteDefaults)
        return 1;

    // A failed mount is deliberately not sticky across attempts. A later stable-power boot can
    // mount the untouched partition and take the normal persisted-preferences path.
    mountState.recordMountResult(true);
    if (shouldUseFilesystemPersistence(true, mountState.mounted()))
        loadedPreference = persistedPreference;
    if (loadedPreference != persistedPreference)
        return 2;

    return 0;
}

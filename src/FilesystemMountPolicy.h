#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

// ESP32 traditionally formats LittleFS when mounting fails. That recovery is destructive, so the
// Heltec V4 keeps the existing partition intact and waits for either a clean boot or an explicit reset.
constexpr bool shouldAutoFormatFilesystemOnMountFailure(bool isHeltecV4Oled)
{
    return !isHeltecV4Oled;
}

constexpr bool shouldAutoFormatFilesystemOnMountFailure()
{
#if defined(HELTEC_V4_OLED)
    return shouldAutoFormatFilesystemOnMountFailure(true);
#else
    return shouldAutoFormatFilesystemOnMountFailure(false);
#endif
}

// Other targets retain their existing best-effort behaviour. On the Heltec V4, filesystem-backed
// identity and preferences are usable only after this boot has mounted LittleFS successfully.
constexpr bool shouldUseFilesystemPersistence(bool isHeltecV4Oled, bool mountSucceeded)
{
    return !isHeltecV4Oled || mountSucceeded;
}

constexpr bool shouldUseFilesystemPersistence(bool mountSucceeded)
{
#if defined(HELTEC_V4_OLED)
    return shouldUseFilesystemPersistence(true, mountSucceeded);
#else
    return shouldUseFilesystemPersistence(false, mountSucceeded);
#endif
}

constexpr bool shouldUsePersistentConfiguration(bool filesystemPersistenceUsable, bool configRecoveryRequired)
{
    return filesystemPersistenceUsable && !configRecoveryRequired;
}

constexpr bool persistedIdentityRequiresNodeDatabase(size_t privateKeySize, size_t configPublicKeySize, size_t ownerPublicKeySize,
                                                     bool isLicensed)
{
    // A keyless, unlicensed device deliberately has no nodes.proto yet:
    // saveNodeDatabaseToDisk() skips that cache until an identity exists. Once
    // any persisted identity material (or a licensed identity) exists, a
    // missing node database is a torn core generation and must remain
    // fail-closed for explicit recovery.
    return isLicensed || privateKeySize != 0 || configPublicKeySize != 0 || ownerPublicKeySize != 0;
}

constexpr bool shouldAllowPhonePacketWhileRecovering(bool persistentConfigUsable, bool decodedAdminPacket,
                                                     bool addressedToLocalNode)
{
    return persistentConfigUsable || (decodedAdminPacket && addressedToLocalNode);
}

constexpr bool shouldAllowOtaRequestWhileRecovering(bool otaRequest, bool bluetoothOtaMode)
{
    return otaRequest && bluetoothOtaMode;
}

// The legacy migration used to erase /prefs during early boot, before a
// reliable power reading existed. Heltec V4 instead rewrites every
// identity/config-bearing core file first and removes the obsolete marker only
// after the new state (including the node cache, when applicable) is verified.
constexpr bool shouldAutoEraseLegacyPreferences(bool isHeltecV4Oled)
{
    return !isHeltecV4Oled;
}

constexpr bool shouldAutoEraseLegacyPreferences()
{
#if defined(HELTEC_V4_OLED)
    return shouldAutoEraseLegacyPreferences(true);
#else
    return shouldAutoEraseLegacyPreferences(false);
#endif
}

// A full factory reset first removes the only persisted identity/configuration
// generation and then writes several replacement files. Require either a
// measured, initialized battery state or an enumerated USB data host before
// permitting that sequence on the Heltec V4. The Solar Router recovery
// threshold is intentionally reused as a conservative minimum for both
// profiles.
constexpr uint16_t HELTEC_V4_DESTRUCTIVE_STORAGE_MIN_MILLIVOLTS = 3650;
// Ordinary settings commits preserve the previous file generation and carry a
// durable EDIT marker. They therefore need brownout margin, not the much more
// conservative threshold reserved for reset/restore operations that remove a
// generation. This keeps low-battery role automation usable on Standard while
// preventing Solar Router writes inside its inclusive <=3.50 V shutdown band.
constexpr uint16_t HELTEC_V4_PREFERENCE_STORAGE_MIN_MILLIVOLTS = 3300;

constexpr bool shouldAllowHeltecDestructiveStorageMutation(bool powerStatusAvailable, bool powerStatusInitialized,
                                                           bool usbDataHostConnected, bool batteryPresent,
                                                           int32_t batteryMillivolts)
{
    return usbDataHostConnected ||
           (powerStatusAvailable && powerStatusInitialized && batteryPresent &&
            batteryMillivolts >= HELTEC_V4_DESTRUCTIVE_STORAGE_MIN_MILLIVOLTS);
}

constexpr uint16_t heltecPreferenceStorageMinimumMillivolts(uint16_t profileCriticalMillivolts)
{
    if (profileCriticalMillivolts < HELTEC_V4_PREFERENCE_STORAGE_MIN_MILLIVOLTS) {
        return HELTEC_V4_PREFERENCE_STORAGE_MIN_MILLIVOLTS;
    }

    // Critical cutoffs are inclusive, so the first safe integer millivolt is
    // one above the cutoff. Saturate defensively for an invalid uint16_t-max
    // profile rather than wrapping to zero.
    return profileCriticalMillivolts == UINT16_MAX ? UINT16_MAX : profileCriticalMillivolts + 1U;
}

constexpr bool shouldAllowHeltecPreferenceStorageMutation(bool powerStatusAvailable, bool powerStatusInitialized,
                                                          bool usbDataHostConnected, bool batteryPresent,
                                                          int32_t batteryMillivolts,
                                                          uint16_t minimumBatteryMillivolts =
                                                              HELTEC_V4_PREFERENCE_STORAGE_MIN_MILLIVOLTS)
{
    return usbDataHostConnected ||
           (powerStatusAvailable && powerStatusInitialized && batteryPresent &&
            batteryMillivolts >= minimumBatteryMillivolts);
}

// Only ordinary, transaction-free core saves are safe to replay later. A
// failed edit/reset remains under its durable recovery marker instead of being
// silently replayed outside the transaction that authorized it.
constexpr bool shouldQueueHeltecPreferenceWriteRetry(bool powerIsSafe, bool preferenceEditActive,
                                                     bool destructiveMutationActive)
{
    return !powerIsSafe && !preferenceEditActive && !destructiveMutationActive;
}

constexpr bool shouldAttemptHeltecDeferredPreferenceRetry(bool deferredSegmentsPresent, bool criticalSleepPending,
                                                          bool rebootPending, bool shutdownPending)
{
    return deferredSegmentsPresent && !criticalSleepPending && !rebootPending && !shutdownPending;
}

// Stock factory reset removes /prefs before writing defaults. On the Heltec V4,
// keep the old core files in place during a config-only reset so SafeFile can
// replace them atomically. A full device reset is explicitly destructive and
// may remove the directory first.
constexpr bool shouldRemovePreferencesBeforeFactoryDefaults(bool isHeltecV4Oled, bool fullDeviceReset)
{
    return !isHeltecV4Oled || fullDeviceReset;
}

class FilesystemMountState
{
  public:
    void recordMountResult(bool mounted) { isMounted.store(mounted, std::memory_order_release); }
    bool mounted() const { return isMounted.load(std::memory_order_acquire); }

  private:
    std::atomic<bool> isMounted{false};
};

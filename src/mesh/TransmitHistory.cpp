#include "TransmitHistory.h"
#include "FSCommon.h"
#include "NodeDB.h"
#include "Power.h"
#include "SafeFile.h"
#include "SPILock.h"
#include "gps/RTC.h"
#include <Throttle.h>
#include <utility>

#ifdef FSCom

TransmitHistory *transmitHistory = nullptr;

TransmitHistory *TransmitHistory::getInstance()
{
    if (!transmitHistory) {
        transmitHistory = new TransmitHistory();
    }
    return transmitHistory;
}

TransmitHistory::StoredTimestamp TransmitHistory::makeStoredTimestamp(uint32_t seconds, uint8_t flags)
{
    StoredTimestamp stored;
    stored.seconds = seconds;
    stored.flags = flags;
    return stored;
}

TransmitHistory::StoredTimestamp TransmitHistory::decodeLegacyTimestamp(uint32_t seconds)
{
    const bool isProbablyBootRelative = seconds > 0 && seconds <= LEGACY_BOOT_RELATIVE_MAX_SEC;
    return makeStoredTimestamp(seconds, isProbablyBootRelative ? ENTRY_FLAG_BOOT_RELATIVE : ENTRY_FLAG_NONE);
}

void TransmitHistory::loadFromDisk()
{
    concurrency::LockGuard persistenceGuard(&persistenceLock);
    std::map<uint16_t, StoredTimestamp> loadedHistory;
    {
        concurrency::LockGuard spiGuard(spiLock);
        auto file = FSCom.open(FILENAME, FILE_O_READ);
        if (file) {
            FileHeader header{};
            if (file.read((uint8_t *)&header, sizeof(header)) == sizeof(header) && header.magic == MAGIC &&
                (header.version == 1 || header.version == VERSION) && header.count <= MAX_ENTRIES) {
                for (uint8_t i = 0; i < header.count; i++) {
                    if (header.version == 1) {
                        LegacyEntry entry{};
                        if (file.read((uint8_t *)&entry, sizeof(entry)) == sizeof(entry) && entry.epochSeconds > 0) {
                            loadedHistory[entry.key] = decodeLegacyTimestamp(entry.epochSeconds);
                        }
                    } else {
                        Entry entry{};
                        if (file.read((uint8_t *)&entry, sizeof(entry)) == sizeof(entry) && entry.epochSeconds > 0) {
                            loadedHistory[entry.key] = makeStoredTimestamp(entry.epochSeconds, entry.flags);
                            // Do NOT seed lastMillis here.
                            //
                            // getLastSentToMeshMillis() reconstructs a millis()-relative value
                            // from the stored epoch, and Throttle::isWithinTimespanMs() uses
                            // the same unsigned subtraction pattern. Once getTime() has a valid
                            // wall-clock epoch comparable to stored values, recent reboots still
                            // throttle correctly while long power-off periods no longer look like
                            // "just sent" and incorrectly suppress the first send.
                            //
                            // Before RTC/NTP/GPS time is valid, persisted absolute epochs do not
                            // contribute, but boot-relative entries still suppress near-term reboot
                            // chatter via a narrow recovery window.
                            //
                            // If we seeded lastMillis to millis() here, every loaded entry would
                            // appear to have been sent at boot time, regardless of the true age
                            // of the last transmission. That was the regression behind #9901.
                        }
                    }
                }
                LOG_INFO("TransmitHistory: loaded %u entries from disk", header.count);
            } else {
                LOG_WARN("TransmitHistory: invalid file header, starting fresh");
            }
            file.close();
        } else {
            LOG_INFO("TransmitHistory: no history file found, starting fresh");
        }
    }

    concurrency::LockGuard stateGuard(&stateLock);
    history = std::move(loadedHistory);
    lastMillis.clear();
    dirty = false;
    ++mutationGeneration;
    lastDiskSave = 0;
}

void TransmitHistory::setLastSentToMesh(uint16_t key)
{
    const uint32_t nowMillis = millis();
    const uint32_t now = getTime();
    const uint8_t flags = (getRTCQuality() == RTCQualityNone) ? ENTRY_FLAG_BOOT_RELATIVE : ENTRY_FLAG_NONE;
    bool shouldSave = false;
    {
        concurrency::LockGuard stateGuard(&stateLock);
        lastMillis[key] = nowMillis;
        if (now >= 2) {
            history[key] = makeStoredTimestamp(now, flags);
            dirty = true;
            ++mutationGeneration;
            shouldSave = lastDiskSave == 0 || !Throttle::isWithinTimespanMs(lastDiskSave, SAVE_INTERVAL_MS);
        }
    }
    if (now >= 2) {
        // Don't flush to disk on every transmit - flash has limited write endurance.
        // The in-memory lastMillis map handles throttle during normal operation.
        // Disk is flushed: before deep sleep (sleep.cpp) and periodically here,
        // throttled to at most once per 5 minutes. Always save the first time
        // after boot so a crash-reboot loop can't avoid persisting.
        if (shouldSave)
            (void)saveToDisk();
    }
}

#ifdef PIO_UNIT_TESTING
void TransmitHistory::setLastSentAtEpoch(uint16_t key, uint32_t epochSeconds)
{
    concurrency::LockGuard stateGuard(&stateLock);
    if (epochSeconds > 0) {
        history[key] = makeStoredTimestamp(epochSeconds, ENTRY_FLAG_NONE);
        dirty = true;
    } else {
        history.erase(key);
        lastMillis.erase(key);
    }
    ++mutationGeneration;
}

void TransmitHistory::setLastSentAtBootRelative(uint16_t key, uint32_t secondsSinceBoot)
{
    concurrency::LockGuard stateGuard(&stateLock);
    if (secondsSinceBoot > 0) {
        history[key] = makeStoredTimestamp(secondsSinceBoot, ENTRY_FLAG_BOOT_RELATIVE);
        dirty = true;
    } else {
        history.erase(key);
        lastMillis.erase(key);
    }
    ++mutationGeneration;
}
#endif

uint32_t TransmitHistory::getLastSentToMeshEpoch(uint16_t key) const
{
    concurrency::LockGuard stateGuard(&stateLock);
    auto it = history.find(key);
    if (it != history.end()) {
        return it->second.seconds;
    }
    return 0;
}

uint32_t TransmitHistory::getLastSentAbsoluteMillis(uint32_t storedEpoch) const
{
    uint32_t now = getTime();
    if (now < 2) {
        return 0;
    }

    if (storedEpoch > now) {
        return 0;
    }

    uint32_t secondsAgo = now - storedEpoch;
    uint32_t msAgo = secondsAgo * 1000;

    if (secondsAgo > 86400 || msAgo / 1000 != secondsAgo) {
        return 0;
    }

    return millis() - msAgo;
}

uint32_t TransmitHistory::getLastSentBootRelativeMillis(uint32_t storedSeconds) const
{
    if (getRTCQuality() != RTCQualityNone) {
        return 0;
    }

    uint32_t now = getTime();

    if (storedSeconds <= now) {
        uint32_t secondsAgo = now - storedSeconds;
        if (secondsAgo > BOOT_RELATIVE_RECOVERY_WINDOW_SEC) {
            return 0;
        }
        return millis() - (secondsAgo * 1000);
    }

    uint32_t secondsAhead = storedSeconds - now;
    if (secondsAhead > BOOT_RELATIVE_RECOVERY_WINDOW_SEC) {
        return 0;
    }

    return millis();
}

uint32_t TransmitHistory::getLastSentToMeshMillis(uint16_t key) const
{
    StoredTimestamp stored;
    {
        concurrency::LockGuard stateGuard(&stateLock);
        // Prefer runtime millis value (accurate within this boot)
        auto mit = lastMillis.find(key);
        if (mit != lastMillis.end()) {
            return mit->second;
        }

        // Fall back to epoch conversion (loaded from disk after reboot)
        auto it = history.find(key);
        if (it == history.end() || it->second.seconds == 0) {
            return 0; // No stored time - module has never sent
        }
        stored = it->second;
    }

    // Convert to a millis()-relative timestamp: millis() - msAgo.
    //
    // The result may wrap if msAgo is larger than the current uptime, and that is
    // intentional. Throttle::isWithinTimespanMs() also uses unsigned subtraction,
    // so the reconstructed age is preserved across wraparound:
    // - recent reboot, 5 min ago   -> (millis() - lastMs) == 300000, still throttled
    // - long reboot, 30 min ago    -> (millis() - lastMs) == 1800000, allowed
    if ((stored.flags & ENTRY_FLAG_BOOT_RELATIVE) != 0) {
        return getLastSentBootRelativeMillis(stored.seconds);
    }

    return getLastSentAbsoluteMillis(stored.seconds);
}

bool TransmitHistory::saveToDisk()
{
#if defined(HELTEC_V4_OLED)
    if (nodeDB && nodeDB->isDestructiveStorageMutationActive())
        return false;
#endif
    concurrency::LockGuard persistenceGuard(&persistenceLock);
#if defined(HELTEC_V4_OLED)
    if (nodeDB && nodeDB->isDestructiveStorageMutationActive())
        return false;
#endif
    std::map<uint16_t, StoredTimestamp> snapshot;
    uint32_t savedGeneration = 0;
    {
        concurrency::LockGuard stateGuard(&stateLock);
        if (!dirty)
            return true;
        snapshot = history;
        savedGeneration = mutationGeneration;
    }

#if defined(HELTEC_V4_OLED)
    if (!heltecPreferenceStoragePowerIsSafe()) {
        LOG_WARN("TransmitHistory: deferring persistence while fresh power is unsafe");
        return false;
    }
    // Preserve the last verified generation until the replacement has been
    // written, read back, and atomically published. In particular, do not
    // erase ACK/throttle history merely because supply voltage sags midway
    // through an automatic save.
    {
        concurrency::LockGuard guard(spiLock);
        FSCom.mkdir("/prefs");
    }
    SafeFile file(FILENAME, true);
    bool writeSucceeded = true;
    uint8_t written = 0;
    {
        concurrency::LockGuard guard(spiLock);
        FileHeader header{};
        header.magic = MAGIC;
        header.version = VERSION;
        header.count = (uint8_t)min((size_t)MAX_ENTRIES, snapshot.size());
        writeSucceeded = file.write((uint8_t *)&header, sizeof(header)) == sizeof(header);
        for (const auto &[key, stored] : snapshot) {
            if (written >= MAX_ENTRIES)
                break;
            Entry entry{};
            entry.key = key;
            entry.epochSeconds = stored.seconds;
            entry.flags = stored.flags;
            writeSucceeded &= file.write((uint8_t *)&entry, sizeof(entry)) == sizeof(entry);
            written++;
        }
    }

    const bool closeSucceeded = file.close();
    if (writeSucceeded && closeSucceeded) {
        LOG_DEBUG("TransmitHistory: saved %u entries to disk", written);
    } else {
        LOG_WARN("TransmitHistory: atomic write/readback failed; keeping history dirty");
        return false;
    }
#else
    spiLock->lock();

    FSCom.mkdir("/prefs");

    // Remove old file first
    if (FSCom.exists(FILENAME)) {
        FSCom.remove(FILENAME);
    }

    auto file = FSCom.open(FILENAME, FILE_O_WRITE);
    if (file) {
        FileHeader header{};
        header.magic = MAGIC;
        header.version = VERSION;
        header.count = (uint8_t)min((size_t)MAX_ENTRIES, snapshot.size());

        file.write((uint8_t *)&header, sizeof(header));

        uint8_t written = 0;
        for (const auto &[key, stored] : snapshot) {
            if (written >= MAX_ENTRIES)
                break;
            Entry entry{};
            entry.key = key;
            entry.epochSeconds = stored.seconds;
            entry.flags = stored.flags;
            file.write((uint8_t *)&entry, sizeof(entry));
            written++;
        }
        file.flush();
        file.close();
        LOG_DEBUG("TransmitHistory: saved %u entries to disk", written);
        spiLock->unlock();
    } else {
        LOG_WARN("TransmitHistory: failed to open file for writing");
        spiLock->unlock();
        return false;
    }
#endif

    {
        concurrency::LockGuard stateGuard(&stateLock);
        if (mutationGeneration == savedGeneration)
            dirty = false;
        lastDiskSave = millis();
    }
    return true;
}

bool TransmitHistory::clear(bool requireDestructivePower)
{
#if defined(HELTEC_V4_OLED)
    const auto storagePowerIsSafe = [requireDestructivePower]() {
        return requireDestructivePower ? heltecDestructiveStoragePowerIsSafe()
                                       : heltecPreferenceStoragePowerIsSafe();
    };
    if (nodeDB && nodeDB->isDestructiveStorageMutationActive() &&
        !nodeDB->isDestructiveStorageMutationOwnerCurrentTask())
        return false;
    if (!storagePowerIsSafe()) {
        LOG_WARN("TransmitHistory: refusing clear while fresh power is unsafe");
        return false;
    }
#else
    (void)requireDestructivePower;
#endif

    concurrency::LockGuard persistenceGuard(&persistenceLock);
#if defined(HELTEC_V4_OLED)
    if (nodeDB && nodeDB->isDestructiveStorageMutationActive() &&
        !nodeDB->isDestructiveStorageMutationOwnerCurrentTask())
        return false;
    if (!storagePowerIsSafe()) {
        LOG_WARN("TransmitHistory: refusing clear after wait because fresh power is unsafe");
        return false;
    }
#endif
    concurrency::LockGuard stateGuard(&stateLock);
    bool removed = true;
    {
        concurrency::LockGuard guard(spiLock);
        if (FSCom.exists(FILENAME))
            removed = FSCom.remove(FILENAME) && !FSCom.exists(FILENAME);
        String temporaryPath = FILENAME;
        temporaryPath += ".tmp";
        if (FSCom.exists(temporaryPath.c_str()))
            removed = FSCom.remove(temporaryPath.c_str()) && !FSCom.exists(temporaryPath.c_str()) && removed;
    }
    if (!removed) {
        LOG_WARN("TransmitHistory: failed to clear on-disk history");
        return false;
    }

    history.clear();
    lastMillis.clear();
    dirty = false;
    lastDiskSave = 0; // so the next legit broadcast persists immediately
    LOG_INFO("TransmitHistory: cleared in-memory state + on-disk file");
    return true;
}

void TransmitHistory::drainPersistenceWrites()
{
    concurrency::LockGuard persistenceGuard(&persistenceLock);
}

#else
// No filesystem available - provide stub with in-memory tracking
TransmitHistory *transmitHistory = nullptr;

TransmitHistory *TransmitHistory::getInstance()
{
    if (!transmitHistory) {
        transmitHistory = new TransmitHistory();
    }
    return transmitHistory;
}

void TransmitHistory::loadFromDisk() {}

void TransmitHistory::setLastSentToMesh(uint16_t key)
{
    concurrency::LockGuard stateGuard(&stateLock);
    lastMillis[key] = millis();
}

uint32_t TransmitHistory::getLastSentToMeshEpoch(uint16_t key) const
{
    return 0;
}

uint32_t TransmitHistory::getLastSentToMeshMillis(uint16_t key) const
{
    concurrency::LockGuard stateGuard(&stateLock);
    auto mit = lastMillis.find(key);
    return (mit != lastMillis.end()) ? mit->second : 0;
}

bool TransmitHistory::saveToDisk()
{
    return true;
}

bool TransmitHistory::clear(bool requireDestructivePower)
{
    (void)requireDestructivePower;
    concurrency::LockGuard persistenceGuard(&persistenceLock);
    concurrency::LockGuard stateGuard(&stateLock);
    history.clear();
    lastMillis.clear();
    return true;
}

void TransmitHistory::drainPersistenceWrites()
{
    concurrency::LockGuard persistenceGuard(&persistenceLock);
}

#endif

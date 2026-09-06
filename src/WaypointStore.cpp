#include "configuration.h"

#if !MESHTASTIC_EXCLUDE_WAYPOINT

#include "FSCommon.h"
#include "NodeDB.h"
#include "Power.h"
#include "SPILock.h"
#include "SafeFile.h"
#include "Throttle.h"
#include "UptimeClock.h"
#include "WaypointStore.h"
#include "concurrency/Lock.h"
#include "concurrency/LockGuard.h"
#include "gps/RTC.h"
#include "meshUtils.h"
#include <cstring>
#include <pb_decode.h>
#include <pb_encode.h>
#include <vector>

namespace
{

constexpr uint8_t WAYPOINT_STORE_VERSION = 3;
constexpr const char *WAYPOINT_STORE_FILENAME = "/Waypoints_default.wpts";
// Serialize autosave with factory-reset erasure; this drains a save that
// crossed NodeDB's destructive-operation gate immediately before reset.
concurrency::Lock g_waypointStorePersistenceLock;
#if defined(HELTEC_V4_OLED)
constexpr bool WAYPOINT_STORE_FULL_ATOMIC = true;
#else
constexpr bool WAYPOINT_STORE_FULL_ATOMIC = false;
#endif

#ifndef WAYPOINT_AUTOSAVE_INTERVAL_SEC
#define WAYPOINT_AUTOSAVE_INTERVAL_SEC (2 * 60 * 60)
#endif

struct __attribute__((packed)) StoredWaypointRecord {
    uint32_t creatorNodeNum;
    uint32_t receivedTime;
    uint8_t notificationPreferences;
    uint16_t payloadLength;
    uint8_t payload[meshtastic_Waypoint_size];
};

bool decodeWaypointPayload(const uint8_t *payload, size_t payloadLength, meshtastic_Waypoint &wp)
{
    memset(&wp, 0, sizeof(wp));
    return pb_decode_from_bytes(payload, payloadLength, &meshtastic_Waypoint_msg, &wp);
}

size_t encodeWaypointPayload(const meshtastic_Waypoint &wp, uint8_t *payload, size_t payloadCapacity)
{
    return pb_encode_to_bytes(payload, payloadCapacity, &meshtastic_Waypoint_msg, &wp);
}

uint32_t autosaveIntervalMs()
{
    uint32_t sec = (uint32_t)WAYPOINT_AUTOSAVE_INTERVAL_SEC;
    if (sec < 60)
        sec = 60;
    return sec * 1000UL;
}

} // namespace

WaypointStore waypointStore;

void WaypointStore::notifyChanged()
{
    notifyObservers(this);
}

void WaypointStore::markUnsavedLocked()
{
    ++mutationGeneration;
#if ENABLE_WAYPOINT_PERSISTENCE
    hasUnsavedChanges = true;
    if (lastAutoSaveMs == 0)
        lastAutoSaveMs = Time::getMillis();
#endif
}

bool WaypointStore::isExpired(const meshtastic_Waypoint &wp, uint32_t now)
{
    // getTime() counts from boot until the RTC is set, which reads every real expiry as future.
    if (now == 0)
        now = getValidTime(RTCQuality::RTCQualityDevice);

    return !waypointIsActive(wp.expire, now);
}

bool WaypointStore::isExpired(const StoredWaypoint &entry, uint32_t now)
{
    return isExpired(entry.waypoint, now);
}

uint8_t WaypointStore::notificationPreferencesFromWaypoint(const meshtastic_Waypoint &wp)
{
    uint8_t preferences = 0;
    if (wp.notify_on_enter)
        preferences |= WAYPOINT_NOTIFY_ENTER;
    if (wp.notify_on_exit)
        preferences |= WAYPOINT_NOTIFY_EXIT;
    if (wp.notify_favorites_only)
        preferences |= WAYPOINT_NOTIFY_FAVORITES_ONLY;
    return preferences;
}

uint8_t WaypointStore::mergeNotificationPreferences(bool locallyAuthored, bool hasExisting, uint8_t existingPreferences,
                                                    const meshtastic_Waypoint &incoming)
{
    if (locallyAuthored)
        return notificationPreferencesFromWaypoint(incoming);
    return hasExisting ? existingPreferences : 0;
}

void WaypointStore::clearWireNotificationPreferences(meshtastic_Waypoint &wp)
{
    wp.notify_on_enter = false;
    wp.notify_on_exit = false;
    wp.notify_favorites_only = false;
}

std::deque<StoredWaypoint> WaypointStore::getWaypoints() const
{
    concurrency::LockGuard stateGuard(&stateLock);
    return waypoints;
}

bool WaypointStore::findWaypoint(uint32_t id, StoredWaypoint &result) const
{
    concurrency::LockGuard stateGuard(&stateLock);
    for (const StoredWaypoint &entry : waypoints) {
        if (entry.waypoint.id == id) {
            result = entry;
            return true;
        }
    }
    return false;
}

bool WaypointStore::removeWaypointByIdLocked(uint32_t id)
{
    for (auto it = waypoints.begin(); it != waypoints.end(); ++it) {
        if (it->waypoint.id == id) {
            waypoints.erase(it);
            return true;
        }
    }

    return false;
}

bool WaypointStore::removeWaypoint(uint32_t id)
{
    bool removed = false;
    {
        concurrency::LockGuard stateGuard(&stateLock);
        removed = removeWaypointByIdLocked(id);
        if (removed)
            markUnsavedLocked();
    }
    if (removed)
        notifyChanged();

    return removed;
}

bool WaypointStore::setNotificationPreference(uint32_t id, WaypointNotificationPreference preference, bool enabled)
{
    bool found = false;
    bool changed = false;
    {
        concurrency::LockGuard stateGuard(&stateLock);
        for (StoredWaypoint &entry : waypoints) {
            if (entry.waypoint.id != id)
                continue;

            found = true;
            const uint8_t previous = entry.notificationPreferences;
            if (enabled)
                entry.notificationPreferences |= preference;
            else
                entry.notificationPreferences &= ~preference;
            changed = entry.notificationPreferences != previous;
            if (changed)
                markUnsavedLocked();
            break;
        }
    }
    if (changed)
        notifyChanged();
    return found;
}

void WaypointStore::addStoredWaypointLocked(const StoredWaypoint &entry)
{
    removeWaypointByIdLocked(entry.waypoint.id);

    waypoints.push_front(entry);
    while (waypoints.size() > WAYPOINT_HISTORY_LIMIT)
        waypoints.pop_back();
}

bool WaypointStore::addFromPacket(const meshtastic_MeshPacket &packet, bool locallyAuthored, StoredWaypoint *stored)
{
    StoredWaypoint entry;
    if (!decodeWaypointPayload(packet.decoded.payload.bytes, packet.decoded.payload.size, entry.waypoint))
        return false;

    entry.receivedTime = packet.rx_time ? packet.rx_time : getTime();
    entry.creatorNodeNum = getFrom(&packet);

    // rx_time holds uptime, not an epoch, when has_rx_time is false; pass 0 so isExpired() resolves
    // the clock itself rather than comparing an expiry against seconds since boot.
    const bool expired = isExpired(entry, packet.has_rx_time ? packet.rx_time : 0);
    bool changed = false;
    {
        concurrency::LockGuard stateGuard(&stateLock);
        const StoredWaypoint *existing = nullptr;
        for (const StoredWaypoint &candidate : waypoints) {
            if (candidate.waypoint.id == entry.waypoint.id) {
                existing = &candidate;
                break;
            }
        }
        entry.notificationPreferences = mergeNotificationPreferences(
            locallyAuthored, existing != nullptr, existing ? existing->notificationPreferences : 0, entry.waypoint);
        clearWireNotificationPreferences(entry.waypoint);

        if (stored)
            *stored = entry;

        if (expired) {
            // Only the node a locked waypoint belongs to may delete it locally.
            if (existing && existing->waypoint.locked_to != 0 && existing->waypoint.locked_to != entry.creatorNodeNum)
                return true;
            changed = removeWaypointByIdLocked(entry.waypoint.id);
        } else {
            addStoredWaypointLocked(entry);
            changed = true;
        }

        if (changed)
            markUnsavedLocked();
    }

    if (changed)
        notifyChanged();

    return true;
}

bool WaypointStore::purgeExpired(uint32_t now)
{
    // No local clock normalization: isExpired() owns that policy, including the delete convention.
    bool changed = false;
    {
        concurrency::LockGuard stateGuard(&stateLock);
        for (auto it = waypoints.begin(); it != waypoints.end();) {
            if (!isExpired(*it, now)) {
                ++it;
                continue;
            }

            it = waypoints.erase(it);
            changed = true;
        }
        if (changed)
            markUnsavedLocked();
    }

    if (changed)
        notifyChanged();

    return changed;
}

bool WaypointStore::saveToFlash()
{
#if defined(HELTEC_V4_OLED)
    if (nodeDB && nodeDB->isDestructiveStorageMutationActive())
        return false;
    if (!heltecPreferenceStoragePowerIsSafe()) {
        LOG_WARN("WaypointStore: deferring persistence while fresh power is unsafe");
        return false;
    }
#endif
    purgeExpired();
#if defined(HELTEC_V4_OLED)
    if (nodeDB && nodeDB->isDestructiveStorageMutationActive())
        return false;
#endif
    concurrency::LockGuard persistenceGuard(&g_waypointStorePersistenceLock);
#if defined(HELTEC_V4_OLED)
    if (nodeDB && nodeDB->isDestructiveStorageMutationActive())
        return false;
    if (!heltecPreferenceStoragePowerIsSafe()) {
        LOG_WARN("WaypointStore: deferring persistence after wait because fresh power is unsafe");
        return false;
    }
#endif

    std::deque<StoredWaypoint> snapshot;
    uint32_t savedGeneration = 0;
    {
        concurrency::LockGuard stateGuard(&stateLock);
#if ENABLE_WAYPOINT_PERSISTENCE
        if (!hasUnsavedChanges)
            return true;
#endif
        snapshot = waypoints;
        savedGeneration = mutationGeneration;
    }

#if ENABLE_WAYPOINT_PERSISTENCE && defined(FSCom)
    size_t countFull = snapshot.size();
    if (countFull > WAYPOINT_HISTORY_LIMIT)
        countFull = WAYPOINT_HISTORY_LIMIT;
    if (countFull > UINT8_MAX)
        countFull = UINT8_MAX;
    const uint8_t count = static_cast<uint8_t>(countFull);

    // Encode every record before opening SafeFile. A protobuf encode failure
    // must leave the previously verified generation untouched.
    std::vector<StoredWaypointRecord> records;
    records.reserve(count);
    for (uint8_t i = 0; i < count; ++i) {
        StoredWaypointRecord rec = {};
        rec.creatorNodeNum = snapshot[i].creatorNodeNum;
        rec.receivedTime = snapshot[i].receivedTime;
        rec.notificationPreferences = snapshot[i].notificationPreferences;
        const size_t payloadLength = encodeWaypointPayload(snapshot[i].waypoint, rec.payload, sizeof(rec.payload));
        if (payloadLength == 0 || payloadLength > sizeof(rec.payload) || payloadLength > UINT16_MAX) {
            LOG_ERROR("WaypointStore: refusing malformed encoded record %u", i);
            return false;
        }
        rec.payloadLength = static_cast<uint16_t>(payloadLength);
        records.push_back(rec);
    }

    spiLock->lock();
    FSCom.mkdir("/");
    spiLock->unlock();

    SafeFile f(WAYPOINT_STORE_FILENAME, WAYPOINT_STORE_FULL_ATOMIC);

    spiLock->lock();
    const uint8_t version = WAYPOINT_STORE_VERSION;

    bool stored = f.write(&version, 1) == 1;
    stored = (f.write(&count, 1) == 1) && stored;

    for (uint8_t i = 0; i < count; ++i) {
        stored = (f.write(reinterpret_cast<const uint8_t *>(&records[i]), sizeof(records[i])) == sizeof(records[i])) && stored;
    }
    spiLock->unlock();
    stored = f.close() && stored;
#else
    const bool stored = true;
#endif

    {
        concurrency::LockGuard stateGuard(&stateLock);
        if (stored && mutationGeneration == savedGeneration)
            hasUnsavedChanges = false;
        if (stored)
            lastAutoSaveMs = Time::getMillis();
    }
    return stored;
}

void WaypointStore::loadFromFlash()
{
    concurrency::LockGuard persistenceGuard(&g_waypointStorePersistenceLock);
    concurrency::LockGuard stateGuard(&stateLock);
    std::deque<StoredWaypoint>().swap(waypoints);

#if ENABLE_WAYPOINT_PERSISTENCE && defined(FSCom)
    {
        concurrency::LockGuard guard(spiLock);

        if (FSCom.exists(WAYPOINT_STORE_FILENAME)) {
            auto f = FSCom.open(WAYPOINT_STORE_FILENAME, FILE_O_READ);
            if (f) {
                uint8_t version = 0;
                uint8_t count = 0;
                const bool headerRead = f.readBytes(reinterpret_cast<char *>(&version), 1) == 1 &&
                                        f.readBytes(reinterpret_cast<char *>(&count), 1) == 1;

                if (!headerRead || version != WAYPOINT_STORE_VERSION) {
                    LOG_WARN("WaypointStore version mismatch (%u)", version);
                    f.close();
                } else {
                    if (count > WAYPOINT_HISTORY_LIMIT)
                        count = WAYPOINT_HISTORY_LIMIT;

                    for (uint8_t i = 0; i < count; ++i) {
                        StoredWaypoint entry;
                        StoredWaypointRecord rec = {};
                        if (f.readBytes(reinterpret_cast<char *>(&rec), sizeof(rec)) != sizeof(rec))
                            break;
                        if (rec.payloadLength == 0 || rec.payloadLength > sizeof(rec.payload)) {
                            LOG_WARN("WaypointStore skipping corrupt record %u", i);
                            continue;
                        }
                        constexpr uint8_t validNotificationPreferences =
                            WAYPOINT_NOTIFY_ENTER | WAYPOINT_NOTIFY_EXIT | WAYPOINT_NOTIFY_FAVORITES_ONLY;
                        if ((rec.notificationPreferences & ~validNotificationPreferences) != 0) {
                            LOG_WARN("WaypointStore skipping invalid notification flags in record %u", i);
                            continue;
                        }
                        if (!decodeWaypointPayload(rec.payload, rec.payloadLength, entry.waypoint))
                            continue;
                        entry.receivedTime = rec.receivedTime;
                        entry.creatorNodeNum = rec.creatorNodeNum;
                        entry.notificationPreferences = rec.notificationPreferences;

                        if (isExpired(entry.waypoint))
                            continue;
                        waypoints.push_back(entry);
                    }
                    f.close();
                }
            }
        }
    }
#endif

    ++mutationGeneration;
    hasUnsavedChanges = false;
    lastAutoSaveMs = Time::getMillis();
}

bool WaypointStore::clearAllWaypoints(bool requireDestructivePower)
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
        LOG_WARN("WaypointStore: refusing clear while fresh power is unsafe");
        return false;
    }
#else
    (void)requireDestructivePower;
#endif
    bool hadWaypoints = false;
    bool stored = true;
    {
        concurrency::LockGuard persistenceGuard(&g_waypointStorePersistenceLock);
#if defined(HELTEC_V4_OLED)
        // A clear queued immediately before factory reset must not slip past the
        // reset's one-shot drain. The destructive owner is the sole exception.
        if (nodeDB && nodeDB->isDestructiveStorageMutationActive() &&
            !nodeDB->isDestructiveStorageMutationOwnerCurrentTask())
            return false;
        if (!storagePowerIsSafe()) {
            LOG_WARN("WaypointStore: refusing clear after wait because fresh power is unsafe");
            return false;
        }
#endif
        uint32_t clearedGeneration = 0;
        {
            concurrency::LockGuard stateGuard(&stateLock);
            hadWaypoints = !waypoints.empty();
            std::deque<StoredWaypoint>().swap(waypoints);
            markUnsavedLocked();
            clearedGeneration = mutationGeneration;
        }

#if ENABLE_WAYPOINT_PERSISTENCE && defined(FSCom)
        SafeFile f(WAYPOINT_STORE_FILENAME, WAYPOINT_STORE_FULL_ATOMIC, requireDestructivePower);
        {
            concurrency::LockGuard guard(spiLock);
            const uint8_t version = WAYPOINT_STORE_VERSION;
            const uint8_t count = 0;
            stored = f.write(&version, 1) == 1;
            stored = (f.write(&count, 1) == 1) && stored;
        }
        stored = f.close() && stored;
#endif

        {
            concurrency::LockGuard stateGuard(&stateLock);
            if (stored && mutationGeneration == clearedGeneration)
                hasUnsavedChanges = false;
            if (stored)
                lastAutoSaveMs = Time::getMillis();
        }
    }

    if (hadWaypoints)
        notifyChanged();
    return stored;
}

void WaypointStore::drainPersistenceWrites()
{
    concurrency::LockGuard persistenceGuard(&g_waypointStorePersistenceLock);
}

#if ENABLE_WAYPOINT_PERSISTENCE
void WaypointStore::autosaveTick()
{
    const uint32_t now = Time::getMillis();
    bool shouldSave = false;
    {
        concurrency::LockGuard stateGuard(&stateLock);
        if (lastAutoSaveMs == 0) {
            lastAutoSaveMs = now;
            return;
        }
        if (Throttle::isWithinTimespanMs(lastAutoSaveMs, autosaveIntervalMs()))
            return;

        lastAutoSaveMs = now;
        shouldSave = hasUnsavedChanges;
    }

    if (shouldSave) {
        LOG_INFO("Autosaving WaypointStore to flash");
        saveToFlash();
    }
}

void waypointStoreAutosaveTick()
{
    waypointStore.autosaveTick();
}
#endif

#endif

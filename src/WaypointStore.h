#pragma once

#include "configuration.h"

#if !MESHTASTIC_EXCLUDE_WAYPOINT

#ifndef ENABLE_WAYPOINT_PERSISTENCE
#define ENABLE_WAYPOINT_PERSISTENCE 1
#endif

#ifndef WAYPOINT_HISTORY_LIMIT
#define WAYPOINT_HISTORY_LIMIT 10
#endif

#include "Observer.h"
#include "concurrency/Lock.h"
#include "mesh/MeshTypes.h"
#include "mesh/generated/meshtastic/mesh.pb.h"
#include <cstdint>
#include <deque>

enum WaypointNotificationPreference : uint8_t {
    WAYPOINT_NOTIFY_ENTER = 1 << 0,
    WAYPOINT_NOTIFY_EXIT = 1 << 1,
    WAYPOINT_NOTIFY_FAVORITES_ONLY = 1 << 2,
};

struct StoredWaypoint {
    meshtastic_Waypoint waypoint = meshtastic_Waypoint_init_zero;
    uint32_t receivedTime = 0;
    NodeNum creatorNodeNum = 0;
    uint8_t notificationPreferences = 0;

    bool notificationEnabled(WaypointNotificationPreference preference) const
    {
        return (notificationPreferences & preference) != 0;
    }
};

class WaypointStore : public Observable<const WaypointStore *>
{
  public:
    bool addFromPacket(const meshtastic_MeshPacket &packet, bool locallyAuthored, StoredWaypoint *stored = nullptr);
    bool purgeExpired(uint32_t now = 0);
    bool removeWaypoint(uint32_t id);
    bool setNotificationPreference(uint32_t id, WaypointNotificationPreference preference, bool enabled);

    std::deque<StoredWaypoint> getWaypoints() const;
    bool findWaypoint(uint32_t id, StoredWaypoint &result) const;

    bool saveToFlash();
    void loadFromFlash();
    bool clearAllWaypoints(bool requireDestructivePower = false);
    void drainPersistenceWrites();

    static bool isExpired(const meshtastic_Waypoint &wp, uint32_t now = 0);
    static bool isExpired(const StoredWaypoint &entry, uint32_t now = 0);
    static uint8_t notificationPreferencesFromWaypoint(const meshtastic_Waypoint &wp);
    static uint8_t mergeNotificationPreferences(bool locallyAuthored, bool hasExisting, uint8_t existingPreferences,
                                                const meshtastic_Waypoint &incoming);
    static void clearWireNotificationPreferences(meshtastic_Waypoint &wp);

#if ENABLE_WAYPOINT_PERSISTENCE
    void autosaveTick();
#endif

  private:
    void addStoredWaypointLocked(const StoredWaypoint &entry);
    bool removeWaypointByIdLocked(uint32_t id);
    void markUnsavedLocked();
    void notifyChanged();

    mutable concurrency::Lock stateLock;
    std::deque<StoredWaypoint> waypoints; // Protected by stateLock
    bool hasUnsavedChanges = false;       // Protected by stateLock
    uint32_t lastAutoSaveMs = 0;          // Protected by stateLock
    uint32_t mutationGeneration = 0;      // Prevents a save from clearing a newer dirty state
};

#if ENABLE_WAYPOINT_PERSISTENCE
void waypointStoreAutosaveTick();
#endif

extern WaypointStore waypointStore;

#endif

#pragma once

#if HAS_SCREEN || defined(MESHTASTIC_INCLUDE_NICHE_GRAPHICS)

// Disable debug logging entirely on release builds of HELTEC_MESH_SOLAR for space constraints
#if defined(HELTEC_MESH_SOLAR)
#define LOG_DEBUG(...)
#endif

// Enable or disable message persistence (flash storage)
// Define -DENABLE_MESSAGE_PERSISTENCE=0 in build_flags to disable it entirely
#ifndef ENABLE_MESSAGE_PERSISTENCE
#define ENABLE_MESSAGE_PERSISTENCE 1
#endif

#include "concurrency/Lock.h"
#include "mesh/generated/meshtastic/mesh.pb.h"
#include <cstdint>
#include <deque>
#include <string>

// How many messages are stored (RAM + flash).
// Define -DMESSAGE_HISTORY_LIMIT=N in build_flags to control memory usage.
#ifndef MESSAGE_HISTORY_LIMIT
#if (defined(ARCH_ESP32) &&                                                                                                      \
     !(defined(CONFIG_IDF_TARGET_ESP32C3) || defined(CONFIG_IDF_TARGET_ESP32S3) || defined(CONFIG_IDF_TARGET_ESP32S2))) ||       \
    defined(NRF52840_XXAA)
// Baseline ESP32 (non-PSRAM variants) and nRF52840 (~115 KB heap arena shared with SoftDevice +
// FreeRTOS stacks; 2.8.0 field reports hit 99% use) have limited heap; reduce message history on
// resource-constrained builds. Override with -DMESSAGE_HISTORY_LIMIT=N if needed.
#define MESSAGE_HISTORY_LIMIT 10
#else
#define MESSAGE_HISTORY_LIMIT 20
#endif
#endif

// Internal alias used everywhere in code - do NOT redefine elsewhere.
#define MAX_MESSAGES_SAVED MESSAGE_HISTORY_LIMIT

// Maximum text payload size per message in bytes, including its terminator.
#define MAX_MESSAGE_SIZE 220

// Explicit message classification
enum class MessageType : uint8_t {
    BROADCAST = 0, // broadcast message
    DM_TO_US = 1   // direct message addressed to this node
};

// Delivery status for messages we sent
enum class AckStatus : uint8_t {
    NONE = 0,    // just sent, waiting (no symbol shown)
    ACKED = 1,   // got a valid ACK from destination
    NACKED = 2,  // explicitly failed
    TIMEOUT = 3, // no ACK after retry window
    RELAYED = 4  // got an ACK from relay, not destination
};

struct StoredMessage {
    uint32_t timestamp;   // When message was created (secs since boot or RTC)
    uint32_t sender;      // NodeNum of sender
    uint8_t channelIndex; // Channel index used
    uint32_t dest;        // Destination node (broadcast or direct)
    uint32_t packetId;    // RAM-only packet ID used to match this boot's ACK
    MessageType type;     // Derived from dest (explicit classification)
    bool isBootRelative;  // true = Time::getUptimeSecs() fallback; false = epoch/RTC absolute
    AckStatus ackStatus;  // Delivery status (only meaningful for our own sent messages)

    uint16_t textLength;
    char text[MAX_MESSAGE_SIZE];

    bool xeddsaSigned; // true if packet carried a verified XEdDSA signature

    // Default constructor initializes all fields safely
    StoredMessage()
        : timestamp(0), sender(0), channelIndex(0), dest(0xffffffff), packetId(0), type(MessageType::BROADCAST),
          isBootRelative(false), ackStatus(AckStatus::NONE), textLength(0), text{}, xeddsaSigned(false)
    {
    }
};

class MessageStore
{
  public:
    explicit MessageStore(const std::string &label);

    // Live RAM methods (always current, used by UI and runtime)
    void addLiveMessage(StoredMessage &&msg);
    void addLiveMessage(const StoredMessage &msg); // convenience overload
    std::deque<StoredMessage> getLiveMessages() const;
    // Add from a packet and optionally return an independent copy. False means filtered/invalid.
    bool tryAddFromPacket(const meshtastic_MeshPacket &mp, StoredMessage *stored = nullptr);
    bool updateOwnMessageAck(uint32_t localNode, uint32_t packetId, AckStatus status);

    // Persistence methods (used only on boot/shutdown)
    bool saveToFlash();   // Save messages to flash; false if verification failed
    void loadFromFlash(); // Load messages from flash

    // Clear all messages (RAM + persisted queue)
    bool clearAllMessages(bool requireDestructivePower = false);
    /// Wait for a save that entered immediately before NodeDB raised its
    /// destructive-storage fence. New saves are rejected while that fence is active.
    void drainPersistenceWrites();

    // Delete helpers
    void deleteOldestMessage(); // remove oldest from RAM (and flash on save)
    void deleteOldestMessageInChannel(uint8_t channel);
    void deleteOldestMessageWithPeer(uint32_t peer);
    void deleteAllMessagesInChannel(uint8_t channel);
    void deleteAllMessagesWithPeer(uint32_t peer);
    void deleteAllMessagesFromNode(uint32_t nodeNum);
    // Unified snapshot accessor for UI code.
    std::deque<StoredMessage> getMessages() const;
    bool hasVisibleMessages() const;

    // Helper filters for future use
    std::deque<StoredMessage> getChannelMessages(uint8_t channel) const; // Only broadcast messages on a channel
    std::deque<StoredMessage> getDirectMessages() const;                 // Only direct messages
    bool shouldStorePacket(const meshtastic_MeshPacket &mp) const;
    bool isMessageVisible(const StoredMessage &msg) const;

    // Upgrade boot-relative timestamps once RTC is valid.
    void upgradeBootRelativeTimestamps();

    // Retrieve the C-string text for a stored message
    static const char *getText(const StoredMessage &msg);

    // Copy bounded text into a message so copies retain independent content.
    static void setText(StoredMessage &msg, const char *src, size_t len);

#if ENABLE_MESSAGE_PERSISTENCE
    void autosaveTick();
#endif

  private:
    void markUnsavedLocked();
    bool pruneHiddenMessagesLocked();
    mutable concurrency::Lock stateLock;
    std::deque<StoredMessage> liveMessages; // Protected by stateLock
    std::string filename;                   // Flash filename for persistence
    bool hasUnsavedChanges = false;         // Protected by stateLock
    uint32_t lastAutoSaveMs = 0;            // Protected by stateLock
    uint32_t mutationGeneration = 0;        // Protects saves from clearing a newer dirty state
};

#if ENABLE_MESSAGE_PERSISTENCE
// Called periodically from main loop to trigger time based autosave
void messageStoreAutosaveTick();
#endif

// Global instance (defined in MessageStore.cpp)
extern MessageStore messageStore;

#endif

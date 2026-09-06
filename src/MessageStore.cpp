#include "configuration.h"
#if HAS_SCREEN || defined(MESHTASTIC_INCLUDE_NICHE_GRAPHICS)
#include "FSCommon.h"
#include "MessageStore.h"
#include "NodeDB.h"
#include "Power.h"
#include "SPILock.h"
#include "SafeFile.h"
#include "Throttle.h"
#include "UptimeClock.h"
#include "concurrency/Lock.h"
#include "concurrency/LockGuard.h"
#include "gps/RTC.h"
#include <cstring> // memcpy

// Default autosave interval 2 hours, override per device later with -DMESSAGE_AUTOSAVE_INTERVAL_SEC=300 (etc)
#ifndef MESSAGE_AUTOSAVE_INTERVAL_SEC
#define MESSAGE_AUTOSAVE_INTERVAL_SEC (2 * 60 * 60)
#endif

// Serialize autosave with factory-reset erasure. The NodeDB destructive fence
// stops new saves; this lock drains one that crossed the gate just before it.
static concurrency::Lock g_messageStorePersistenceLock;
#if defined(HELTEC_V4_OLED)
static constexpr bool MESSAGE_STORE_FULL_ATOMIC = true;
#else
static constexpr bool MESSAGE_STORE_FULL_ATOMIC = false;
#endif

static inline bool isIgnoredNodeNum(uint32_t nodeNum)
{
    if (nodeNum == 0 || nodeNum == NODENUM_BROADCAST)
        return false;

    const meshtastic_NodeInfoLite *node = nodeDB->getMeshNode(nodeNum);
    return nodeInfoLiteIsIgnored(node);
}

// Helper: assign a timestamp (RTC if available, else boot-relative)
static inline void assignTimestamp(StoredMessage &sm)
{
    uint32_t nowSecs = getValidTime(RTCQuality::RTCQualityDevice, true);
    if (nowSecs) {
        sm.timestamp = nowSecs;
        sm.isBootRelative = false;
    } else {
        // Uptime seconds, not millis()/1000: a stamp taken before the 32-bit wrap otherwise reads as
        // newer than "now" afterwards, and upgradeBootRelativeTimestamps() then declines to heal it.
        sm.timestamp = Time::getUptimeSecs();
        sm.isBootRelative = true;
    }
}

// Generic push with cap (used by live + persisted queues)
template <typename T> static inline void pushWithLimit(std::deque<T> &queue, const T &msg)
{
    if (queue.size() >= MAX_MESSAGES_SAVED)
        queue.pop_front();
    queue.push_back(msg);
}

template <typename T> static inline void pushWithLimit(std::deque<T> &queue, T &&msg)
{
    if (queue.size() >= MAX_MESSAGES_SAVED)
        queue.pop_front();
    queue.emplace_back(std::move(msg));
}

MessageStore::MessageStore(const std::string &label)
{
    filename = "/Messages_" + label + ".msgs";
}

// Live message handling (RAM only)
void MessageStore::addLiveMessage(StoredMessage &&msg)
{
    concurrency::LockGuard stateGuard(&stateLock);
    pushWithLimit(liveMessages, std::move(msg));
    markUnsavedLocked();
}
void MessageStore::addLiveMessage(const StoredMessage &msg)
{
    concurrency::LockGuard stateGuard(&stateLock);
    pushWithLimit(liveMessages, msg);
    markUnsavedLocked();
}

std::deque<StoredMessage> MessageStore::getLiveMessages() const
{
    concurrency::LockGuard stateGuard(&stateLock);
    return liveMessages;
}

std::deque<StoredMessage> MessageStore::getMessages() const
{
    return getLiveMessages();
}

void MessageStore::markUnsavedLocked()
{
    ++mutationGeneration;
#if ENABLE_MESSAGE_PERSISTENCE
    hasUnsavedChanges = true;
    if (lastAutoSaveMs == 0)
        lastAutoSaveMs = Time::getMillis();
#endif
}

bool MessageStore::updateOwnMessageAck(uint32_t localNode, uint32_t packetId, AckStatus status)
{
    if (packetId == 0)
        return false;
    concurrency::LockGuard stateGuard(&stateLock);
    for (auto it = liveMessages.rbegin(); it != liveMessages.rend(); ++it) {
        if (it->sender != localNode || it->packetId != packetId)
            continue;
        if (it->ackStatus != status) {
            it->ackStatus = status;
            markUnsavedLocked();
        }
        return true;
    }
    return false;
}

#if ENABLE_MESSAGE_PERSISTENCE

static inline uint32_t autosaveIntervalMs()
{
    uint32_t sec = (uint32_t)MESSAGE_AUTOSAVE_INTERVAL_SEC;
    if (sec < 60)
        sec = 60;
    return sec * 1000UL;
}

// Called periodically from the main loop.
void MessageStore::autosaveTick()
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

        // Advance the attempt clock even when persistence fails. Dirty state
        // stays set, so a later normal interval retries without a tight loop.
        lastAutoSaveMs = now;
        shouldSave = hasUnsavedChanges;
    }

    if (shouldSave) {
        LOG_INFO("Autosaving MessageStore to flash");
        saveToFlash();
    } else {
        LOG_INFO("Autosave skipped, no changes to save");
    }
}
#endif

bool MessageStore::shouldStorePacket(const meshtastic_MeshPacket &packet) const
{
    const uint32_t localNode = nodeDB->getNodeNum();
    const bool isDM = packet.to != 0 && packet.to != NODENUM_BROADCAST;
    if (isDM) {
        const bool outgoing = packet.from == 0 || packet.from == localNode;
        const uint32_t peer = outgoing ? packet.to : packet.from;
        return !isIgnoredNodeNum(peer);
    }

    if (packet.from != 0 && packet.from != localNode)
        return !isIgnoredNodeNum(packet.from);

    return true;
}

bool MessageStore::isMessageVisible(const StoredMessage &msg) const
{
    const uint32_t localNode = nodeDB->getNodeNum();
    if (msg.type == MessageType::DM_TO_US) {
        const uint32_t peer = (msg.sender == localNode) ? msg.dest : msg.sender;
        return !isIgnoredNodeNum(peer);
    }

    if (msg.sender != 0 && msg.sender != localNode)
        return !isIgnoredNodeNum(msg.sender);

    return true;
}

// Add from incoming/outgoing packet
bool MessageStore::tryAddFromPacket(const meshtastic_MeshPacket &packet, StoredMessage *stored)
{
    if (!shouldStorePacket(packet)) {
        LOG_DEBUG("Drop store 0x%08x", packet.from);
        return false;
    }

    StoredMessage sm;
    assignTimestamp(sm);
    sm.channelIndex = packet.channel;

    const char *payload = reinterpret_cast<const char *>(packet.decoded.payload.bytes);
    // payload.bytes is not NUL-terminated, so bound by the received size too: a shorter message
    // stored after a longer one would otherwise pick up the previous occupant's trailing bytes.
    size_t avail = packet.decoded.payload.size;
    if (avail > MAX_MESSAGE_SIZE - 1)
        avail = MAX_MESSAGE_SIZE - 1;
    size_t len = strnlen(payload, avail);
    setText(sm, payload, len);

    // Determine sender
    uint32_t localNode = nodeDB->getNodeNum();
    sm.sender = (packet.from == 0) ? localNode : packet.from;

    sm.dest = packet.to;
    sm.packetId = packet.id;

    bool isDM = (sm.dest != 0 && sm.dest != NODENUM_BROADCAST);

    sm.type = isDM ? MessageType::DM_TO_US : MessageType::BROADCAST;
    sm.ackStatus = (packet.from == 0) ? AckStatus::NONE : AckStatus::ACKED;

#if !(MESHTASTIC_EXCLUDE_PKI_KEYGEN || MESHTASTIC_EXCLUDE_PKI)
    sm.xeddsaSigned = packet.xeddsa_signed;
#endif

    addLiveMessage(sm);
    if (stored)
        *stored = sm;
    return true;
}

#if ENABLE_MESSAGE_PERSISTENCE

// Compact, fixed-size on-flash representation.
struct __attribute__((packed)) StoredMessageRecord {
    uint32_t timestamp;
    uint32_t sender;
    uint8_t channelIndex;
    uint32_t dest;
    uint8_t isBootRelative;
    uint8_t ackStatus;           // static_cast<uint8_t>(AckStatus)
    uint8_t type;                // static_cast<uint8_t>(MessageType)
    uint8_t xeddsaSigned;        // 1 if packet carried a verified XEdDSA signature
    uint16_t textLength;         // message length
    char text[MAX_MESSAGE_SIZE]; // store actual text here
};

// Serialize one StoredMessage to flash
static inline bool writeMessageRecord(SafeFile &f, const StoredMessage &m)
{
    StoredMessageRecord rec = {};
    rec.timestamp = m.timestamp;
    rec.sender = m.sender;
    rec.channelIndex = m.channelIndex;
    rec.dest = m.dest;
    rec.isBootRelative = m.isBootRelative;
    rec.ackStatus = static_cast<uint8_t>(m.ackStatus);
    rec.type = static_cast<uint8_t>(m.type);
    rec.xeddsaSigned = m.xeddsaSigned ? 1 : 0;
    rec.textLength = static_cast<uint16_t>(strnlen(m.text, MAX_MESSAGE_SIZE - 1));
    memcpy(rec.text, m.text, rec.textLength);

    return f.write(reinterpret_cast<const uint8_t *>(&rec), sizeof(rec)) == sizeof(rec);
}

// Deserialize one StoredMessage from flash; returns false on a short or invalid record
static inline bool readMessageRecord(File &f, StoredMessage &m)
{
    StoredMessageRecord rec = {};
    if (f.readBytes(reinterpret_cast<char *>(&rec), sizeof(rec)) != sizeof(rec))
        return false;
    const size_t decodedTextLength = strnlen(rec.text, MAX_MESSAGE_SIZE);
    if (rec.isBootRelative > 1 || rec.xeddsaSigned > 1 || rec.ackStatus > static_cast<uint8_t>(AckStatus::RELAYED) ||
        rec.type > static_cast<uint8_t>(MessageType::DM_TO_US) || rec.textLength >= MAX_MESSAGE_SIZE ||
        decodedTextLength != rec.textLength) {
        LOG_WARN("MessageStore: invalid persisted record");
        return false;
    }

    m.timestamp = rec.timestamp;
    m.sender = rec.sender;
    m.channelIndex = rec.channelIndex;
    m.dest = rec.dest;
    m.isBootRelative = rec.isBootRelative;
    m.ackStatus = static_cast<AckStatus>(rec.ackStatus);
    m.type = static_cast<MessageType>(rec.type);
    m.xeddsaSigned = rec.xeddsaSigned != 0;
    MessageStore::setText(m, rec.text, decodedTextLength);

    return true;
}

bool MessageStore::saveToFlash()
{
#if defined(HELTEC_V4_OLED)
    if (nodeDB && nodeDB->isDestructiveStorageMutationActive())
        return false;
    if (!heltecPreferenceStoragePowerIsSafe()) {
        LOG_WARN("MessageStore: deferring persistence while fresh power is unsafe");
        return false;
    }
#endif
    concurrency::LockGuard persistenceGuard(&g_messageStorePersistenceLock);
#if defined(HELTEC_V4_OLED)
    if (nodeDB && nodeDB->isDestructiveStorageMutationActive())
        return false;
    if (!heltecPreferenceStoragePowerIsSafe()) {
        LOG_WARN("MessageStore: deferring persistence after wait because fresh power is unsafe");
        return false;
    }
#endif
    std::deque<StoredMessage> snapshot;
    uint32_t savedGeneration = 0;
    {
        concurrency::LockGuard stateGuard(&stateLock);
        snapshot = liveMessages;
        savedGeneration = mutationGeneration;
    }

    bool stored = true;
#ifdef FSCom
    // Ensure root exists
    spiLock->lock();
    FSCom.mkdir("/");
    spiLock->unlock();

    SafeFile f(filename.c_str(), MESSAGE_STORE_FULL_ATOMIC);

    spiLock->lock();
    uint8_t count = static_cast<uint8_t>(snapshot.size());
    if (count > MAX_MESSAGES_SAVED)
        count = MAX_MESSAGES_SAVED;
    stored = f.write(&count, 1) == 1;

    for (uint8_t i = 0; i < count; ++i) {
        stored = writeMessageRecord(f, snapshot[i]) && stored;
    }
    spiLock->unlock();

    stored = f.close() && stored;
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

void MessageStore::loadFromFlash()
{
    bool pruned = false;
    {
        concurrency::LockGuard persistenceGuard(&g_messageStorePersistenceLock);
        concurrency::LockGuard stateGuard(&stateLock);
        std::deque<StoredMessage>().swap(liveMessages);

#ifdef FSCom
        {
            concurrency::LockGuard guard(spiLock);

            if (FSCom.exists(filename.c_str())) {
                auto f = FSCom.open(filename.c_str(), FILE_O_READ);
                if (f) {
                    uint8_t count = 0;
                    if (f.readBytes(reinterpret_cast<char *>(&count), 1) == 1) {
                        if (count > MAX_MESSAGES_SAVED)
                            count = MAX_MESSAGES_SAVED;

                        for (uint8_t i = 0; i < count; ++i) {
                            StoredMessage m;
                            if (!readMessageRecord(f, m))
                                break;
                            liveMessages.push_back(m);
                        }
                    }
                    f.close();
                }
            }
        }
#endif
        ++mutationGeneration;
        pruned = pruneHiddenMessagesLocked();
        if (pruned)
            ++mutationGeneration;
        hasUnsavedChanges = pruned;
        lastAutoSaveMs = Time::getMillis();
    }

    if (pruned)
        saveToFlash();
}

#else
// If persistence is disabled, these functions become no-ops
bool MessageStore::saveToFlash() { return true; }
void MessageStore::loadFromFlash() {}
#endif

// Clear all messages (RAM + persisted queue)
bool MessageStore::clearAllMessages(bool requireDestructivePower)
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
        LOG_WARN("MessageStore: refusing clear while fresh power is unsafe");
        return false;
    }
#else
    (void)requireDestructivePower;
#endif
    concurrency::LockGuard persistenceGuard(&g_messageStorePersistenceLock);
#if defined(HELTEC_V4_OLED)
    // Close the race in which a UI clear passed the first gate and waited on
    // this lock while factory reset raised its fence and drained an earlier
    // writer. Only the reset owner may proceed while that fence is active.
    if (nodeDB && nodeDB->isDestructiveStorageMutationActive() &&
        !nodeDB->isDestructiveStorageMutationOwnerCurrentTask())
        return false;
    if (!storagePowerIsSafe()) {
        LOG_WARN("MessageStore: refusing clear after wait because fresh power is unsafe");
        return false;
    }
#endif
    uint32_t clearedGeneration = 0;
    {
        concurrency::LockGuard stateGuard(&stateLock);
        std::deque<StoredMessage>().swap(liveMessages);
        markUnsavedLocked();
        clearedGeneration = mutationGeneration;
    }

    bool stored = true;
#ifdef FSCom
    SafeFile f(filename.c_str(), MESSAGE_STORE_FULL_ATOMIC, requireDestructivePower);
    uint8_t count = 0;

    // SafeFile already does its own spiLock in its constructor and close().
    // Avoid nesting spiLocks, as this will hang until watchdog reset!
    {
        concurrency::LockGuard guard(spiLock);
        stored = f.write(&count, 1) == 1; // write "0 messages"
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
    return stored;
}

void MessageStore::drainPersistenceWrites()
{
    concurrency::LockGuard persistenceGuard(&g_messageStorePersistenceLock);
}

// Internal helpers for targeted erasure.
template <typename Predicate> static bool eraseFirstMatch(std::deque<StoredMessage> &deque, Predicate pred)
{
    for (auto it = deque.begin(); it != deque.end(); ++it) {
        if (pred(*it)) {
            deque.erase(it);
            return true;
        }
    }
    return false;
}

template <typename Predicate> static void eraseAllMatches(std::deque<StoredMessage> &deque, Predicate pred)
{
    for (auto it = deque.begin(); it != deque.end();) {
        if (pred(*it)) {
            it = deque.erase(it);
        } else {
            ++it;
        }
    }
}

bool MessageStore::pruneHiddenMessagesLocked()
{
    const size_t before = liveMessages.size();
    eraseAllMatches(liveMessages, [&](const StoredMessage &m) { return !isMessageVisible(m); });
    return liveMessages.size() != before;
}

// Delete oldest message (RAM + persisted queue)
void MessageStore::deleteOldestMessage()
{
    bool changed = false;
    {
        concurrency::LockGuard stateGuard(&stateLock);
        if (!liveMessages.empty()) {
            liveMessages.pop_front();
            markUnsavedLocked();
            changed = true;
        }
    }
    if (changed)
        saveToFlash();
}

// Delete oldest message in a specific channel
void MessageStore::deleteOldestMessageInChannel(uint8_t channel)
{
    auto pred = [channel](const StoredMessage &m) { return m.type == MessageType::BROADCAST && m.channelIndex == channel; };
    bool changed = false;
    {
        concurrency::LockGuard stateGuard(&stateLock);
        changed = eraseFirstMatch(liveMessages, pred);
        if (changed)
            markUnsavedLocked();
    }
    if (changed)
        saveToFlash();
}

void MessageStore::deleteAllMessagesInChannel(uint8_t channel)
{
    auto pred = [channel](const StoredMessage &m) { return m.type == MessageType::BROADCAST && m.channelIndex == channel; };
    bool changed = false;
    {
        concurrency::LockGuard stateGuard(&stateLock);
        const size_t before = liveMessages.size();
        eraseAllMatches(liveMessages, pred);
        changed = liveMessages.size() != before;
        if (changed)
            markUnsavedLocked();
    }
    if (changed)
        saveToFlash();
}

void MessageStore::deleteAllMessagesWithPeer(uint32_t peer)
{
    uint32_t local = nodeDB->getNodeNum();
    auto pred = [&](const StoredMessage &m) {
        if (m.type != MessageType::DM_TO_US)
            return false;
        uint32_t other = (m.sender == local) ? m.dest : m.sender;
        return other == peer;
    };
    bool changed = false;
    {
        concurrency::LockGuard stateGuard(&stateLock);
        const size_t before = liveMessages.size();
        eraseAllMatches(liveMessages, pred);
        changed = liveMessages.size() != before;
        if (changed)
            markUnsavedLocked();
    }
    if (changed)
        saveToFlash();
}

void MessageStore::deleteAllMessagesFromNode(uint32_t nodeNum)
{
    const uint32_t local = nodeDB->getNodeNum();
    auto pred = [&](const StoredMessage &m) {
        if (m.sender == nodeNum)
            return true;
        if (m.type != MessageType::DM_TO_US)
            return false;
        return m.sender == local ? m.dest == nodeNum : m.sender == nodeNum;
    };
    bool changed = false;
    {
        concurrency::LockGuard stateGuard(&stateLock);
        const size_t before = liveMessages.size();
        eraseAllMatches(liveMessages, pred);
        changed = liveMessages.size() != before;
        if (changed)
            markUnsavedLocked();
    }
    if (changed)
        saveToFlash();
}

// Delete oldest message in a direct chat with a node
void MessageStore::deleteOldestMessageWithPeer(uint32_t peer)
{
    const uint32_t local = nodeDB->getNodeNum();
    auto pred = [peer, local](const StoredMessage &m) {
        if (m.type != MessageType::DM_TO_US)
            return false;
        uint32_t other = (m.sender == local) ? m.dest : m.sender;
        return other == peer;
    };
    bool changed = false;
    {
        concurrency::LockGuard stateGuard(&stateLock);
        changed = eraseFirstMatch(liveMessages, pred);
        if (changed)
            markUnsavedLocked();
    }
    if (changed)
        saveToFlash();
}

std::deque<StoredMessage> MessageStore::getChannelMessages(uint8_t channel) const
{
    std::deque<StoredMessage> result;
    for (const auto &m : getLiveMessages()) {
        if (isMessageVisible(m) && m.type == MessageType::BROADCAST && m.channelIndex == channel) {
            result.push_back(m);
        }
    }
    return result;
}

std::deque<StoredMessage> MessageStore::getDirectMessages() const
{
    std::deque<StoredMessage> result;
    for (const auto &m : getLiveMessages()) {
        if (isMessageVisible(m) && m.type == MessageType::DM_TO_US) {
            result.push_back(m);
        }
    }
    return result;
}

bool MessageStore::hasVisibleMessages() const
{
    for (const auto &m : getLiveMessages()) {
        if (isMessageVisible(m))
            return true;
    }
    return false;
}

// Upgrade boot-relative timestamps once RTC is valid
// Only same-boot boot-relative messages are healed.
// Persisted boot-relative messages from old boots stay ??? forever.
void MessageStore::upgradeBootRelativeTimestamps()
{
    uint32_t nowSecs = getValidTime(RTCQuality::RTCQualityDevice, true);
    if (nowSecs == 0)
        return; // Still no valid RTC

    uint32_t bootNow = Time::getUptimeSecs();

    const uint32_t bootOffset = nowSecs - bootNow;
    bool changed = false;
    concurrency::LockGuard stateGuard(&stateLock);
    for (auto &m : liveMessages) {
        if (m.isBootRelative && m.timestamp <= bootNow) {
            m.timestamp += bootOffset;
            m.isBootRelative = false;
            changed = true;
        }
    }
    if (changed)
        markUnsavedLocked();
}

const char *MessageStore::getText(const StoredMessage &msg)
{
    return msg.text;
}

void MessageStore::setText(StoredMessage &msg, const char *src, size_t len)
{
    if (!src)
        len = 0;
    if (len >= MAX_MESSAGE_SIZE)
        len = MAX_MESSAGE_SIZE - 1;
    if (len != 0)
        memcpy(msg.text, src, len);
    msg.text[len] = '\0';
    msg.textLength = static_cast<uint16_t>(len);
}

#if ENABLE_MESSAGE_PERSISTENCE
void messageStoreAutosaveTick()
{
    messageStore.autosaveTick();
}
#endif

// Global definition
MessageStore messageStore("default");
#endif

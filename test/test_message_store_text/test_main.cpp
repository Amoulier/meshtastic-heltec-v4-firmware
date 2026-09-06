#include "configuration.h" // HAS_SCREEN must be known before MessageStore.h
#include "MessageStore.h"
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>
#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

static StoredMessage makeMessage(unsigned index, size_t length)
{
    StoredMessage message{};
    std::string text(length, static_cast<char>('a' + index % 26));
    if (length >= 7) {
        char prefix[8];
        snprintf(prefix, sizeof(prefix), "%07u", index);
        memcpy(text.data(), prefix, 7);
    }
    MessageStore::setText(message, text.data(), text.size());
    return message;
}

void test_max_length_messages_keep_independent_text()
{
    MessageStore store("max-length-test");

    for (unsigned i = 0; i < MAX_MESSAGES_SAVED; ++i)
        store.addLiveMessage(makeMessage(i, MAX_MESSAGE_SIZE - 1));

    unsigned i = 0;
    for (const StoredMessage &message : store.getLiveMessages()) {
        char expectedPrefix[8];
        snprintf(expectedPrefix, sizeof(expectedPrefix), "%07u", i++);
        TEST_ASSERT_EQUAL_STRING_LEN(expectedPrefix, MessageStore::getText(message), 7);
        TEST_ASSERT_EQUAL_UINT16(MAX_MESSAGE_SIZE - 1, message.textLength);
    }
    TEST_ASSERT_EQUAL_UINT(MAX_MESSAGES_SAVED, i);
}

void test_variable_length_eviction_does_not_change_survivors()
{
    MessageStore store("variable-length-test");
    constexpr unsigned extraMessages = 7;

    for (unsigned i = 0; i < MAX_MESSAGES_SAVED + extraMessages; ++i) {
        const size_t length = (i % 4 == 0) ? 8 : MAX_MESSAGE_SIZE - 1 - (i % 11);
        store.addLiveMessage(makeMessage(i, length));
    }

    unsigned expected = extraMessages;
    for (const StoredMessage &message : store.getLiveMessages()) {
        char expectedPrefix[8];
        snprintf(expectedPrefix, sizeof(expectedPrefix), "%07u", expected++);
        TEST_ASSERT_EQUAL_STRING_LEN(expectedPrefix, MessageStore::getText(message), 7);
    }
    TEST_ASSERT_EQUAL_UINT(MAX_MESSAGES_SAVED + extraMessages, expected);
}

void test_snapshots_and_ack_updates_do_not_escape_store_state()
{
    MessageStore store("snapshot-test");
    StoredMessage first = makeMessage(1, 20);
    first.sender = 0x12345678;
    store.addLiveMessage(first);

    auto snapshot = store.getMessages();
    snapshot.front().ackStatus = AckStatus::NACKED;
    store.addLiveMessage(makeMessage(2, 20));

    TEST_ASSERT_EQUAL_UINT(1, snapshot.size());
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(AckStatus::NONE),
                            static_cast<uint8_t>(store.getMessages().front().ackStatus));
    TEST_ASSERT_FALSE(store.updateOwnMessageAck(0x99999999, 41, AckStatus::ACKED));

    StoredMessage outgoing = makeMessage(3, 20);
    outgoing.sender = 0x12345678;
    outgoing.packetId = 41;
    store.addLiveMessage(outgoing);
    StoredMessage incoming = makeMessage(4, 20);
    incoming.sender = 0x87654321;
    incoming.packetId = 99;
    store.addLiveMessage(incoming);
    StoredMessage newerOutgoing = makeMessage(5, 20);
    newerOutgoing.sender = 0x12345678;
    newerOutgoing.packetId = 42;
    store.addLiveMessage(newerOutgoing);

    TEST_ASSERT_FALSE(store.updateOwnMessageAck(0x12345678, 0, AckStatus::ACKED));
    TEST_ASSERT_FALSE(store.updateOwnMessageAck(0x12345678, 43, AckStatus::ACKED));
    TEST_ASSERT_TRUE(store.updateOwnMessageAck(0x12345678, 41, AckStatus::ACKED));
    const auto updated = store.getMessages();
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(AckStatus::ACKED),
                            static_cast<uint8_t>(updated[updated.size() - 3].ackStatus));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(AckStatus::NONE),
                            static_cast<uint8_t>(updated.back().ackStatus));
}

void setup()
{
    UNITY_BEGIN();
    RUN_TEST(test_max_length_messages_keep_independent_text);
    RUN_TEST(test_variable_length_eviction_does_not_change_survivors);
    RUN_TEST(test_snapshots_and_ack_updates_do_not_escape_store_state);
    exit(UNITY_END());
}

void loop() {}

#pragma once

#include <cstddef>
#include <cstdint>

// The caller serializes reserve/finish, never the potentially reentrant delivery.
class MqttAckHistory
{
  public:
    static constexpr size_t capacity = 16;

    bool reserve(uint32_t from, uint32_t id)
    {
        if (id == 0)
            return false;
        for (const auto &entry : entries)
            if (entry.state != State::EMPTY && entry.from == from && entry.id == id)
                return false;
        for (size_t offset = 0; offset < capacity; ++offset) {
            const size_t slot = (next + offset) % capacity;
            if (entries[slot].state == State::PENDING)
                continue;
            entries[slot] = {from, id, State::PENDING};
            next = (slot + 1) % capacity;
            return true;
        }
        return false;
    }

    void finish(uint32_t from, uint32_t id, bool admitted)
    {
        for (auto &entry : entries) {
            if (entry.state == State::PENDING && entry.from == from && entry.id == id) {
                if (admitted)
                    entry.state = State::COMMITTED;
                else
                    entry = {};
                return;
            }
        }
    }

  private:
    enum class State : uint8_t { EMPTY, PENDING, COMMITTED };
    struct Entry {
        uint32_t from = 0;
        uint32_t id = 0;
        State state = State::EMPTY;
    };
    Entry entries[capacity] = {};
    size_t next = 0;
};

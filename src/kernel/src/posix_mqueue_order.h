#pragma once

namespace brook {

// Message storage is owned by the queue. The order table moves small slot
// indexes, so insertion never shifts payload bytes.
struct MqueueOrder {
    static constexpr unsigned MaxMessages = 10;
    struct Message { unsigned length, priority, slot; };

    unsigned count = 0;
    Message messages[MaxMessages]{};

    // Called with capacity available and the queue lock held.
    unsigned Push(unsigned length, unsigned priority) {
        unsigned occupied = 0;
        for (unsigned i = 0; i < count; ++i) occupied |= 1u << messages[i].slot;
        const unsigned slot = __builtin_ctz(~occupied);
        unsigned index = count;
        while (index && messages[index - 1].priority < priority) {
            messages[index] = messages[index - 1];
            --index;
        }
        messages[index] = {length, priority, slot};
        ++count;
        return slot;
    }

    // Called with at least one message queued.
    Message Pop() {
        const auto message = messages[0];
        --count;
        for (unsigned i = 0; i < count; ++i) messages[i] = messages[i + 1];
        return message;
    }

    unsigned Bytes() const {
        unsigned bytes = 0;
        for (unsigned i = 0; i < count; ++i) bytes += messages[i].length;
        return bytes;
    }
};

} // namespace brook

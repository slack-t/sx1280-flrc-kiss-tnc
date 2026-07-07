#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../framing/FramingV3.h"

namespace arq {

static constexpr uint8_t DATAGRAM_POOL_CAPACITY = 8;
static constexpr uint8_t DUPLICATE_WINDOW_CAPACITY = 64;

enum class PoolResult : uint8_t {
    OK = 0,
    EXHAUSTED,
    INVALID_HANDLE,
    DOUBLE_RELEASE,
};

struct DatagramHandle {
    uint8_t index = 0xFFu;

    bool isValid() const {
        return index < DATAGRAM_POOL_CAPACITY;
    }
};

struct DatagramLease {
    DatagramHandle handle;
    uint8_t* data = nullptr;
    uint16_t capacity = 0;
};

struct DatagramPool {
    uint8_t storage[DATAGRAM_POOL_CAPACITY][framing_v3::V3_MAX_DATAGRAM];
    uint8_t free_bitmap = 0xFFu;
    uint8_t used_count = 0;
    uint32_t allocation_failures = 0;
    uint32_t invalid_releases = 0;

    void reset() {
        free_bitmap = 0xFFu;
        used_count = 0;
        allocation_failures = 0;
        invalid_releases = 0;
        memset(storage, 0, sizeof(storage));
    }
};

inline bool poolIsAllocated(const DatagramPool& pool, DatagramHandle handle) {
    if (!handle.isValid()) {
        return false;
    }
    const uint8_t bit = static_cast<uint8_t>(1u << handle.index);
    return (pool.free_bitmap & bit) == 0;
}

inline uint8_t poolFreeCount(const DatagramPool& pool) {
    uint8_t count = 0;
    for (uint8_t i = 0; i < DATAGRAM_POOL_CAPACITY; ++i) {
        if ((pool.free_bitmap & static_cast<uint8_t>(1u << i)) != 0) {
            count++;
        }
    }
    return count;
}

inline PoolResult acquireDatagram(DatagramPool& pool, DatagramLease& lease) {
    for (uint8_t i = 0; i < DATAGRAM_POOL_CAPACITY; ++i) {
        const uint8_t bit = static_cast<uint8_t>(1u << i);
        if ((pool.free_bitmap & bit) != 0) {
            pool.free_bitmap = static_cast<uint8_t>(pool.free_bitmap & ~bit);
            pool.used_count++;
            lease.handle.index = i;
            lease.data = pool.storage[i];
            lease.capacity = framing_v3::V3_MAX_DATAGRAM;
            return PoolResult::OK;
        }
    }

    pool.allocation_failures++;
    lease.handle.index = 0xFFu;
    lease.data = nullptr;
    lease.capacity = 0;
    return PoolResult::EXHAUSTED;
}

inline PoolResult releaseDatagram(DatagramPool& pool, DatagramHandle handle) {
    if (!handle.isValid()) {
        pool.invalid_releases++;
        return PoolResult::INVALID_HANDLE;
    }

    const uint8_t bit = static_cast<uint8_t>(1u << handle.index);
    if ((pool.free_bitmap & bit) != 0) {
        pool.invalid_releases++;
        return PoolResult::DOUBLE_RELEASE;
    }

    pool.free_bitmap = static_cast<uint8_t>(pool.free_bitmap | bit);
    if (pool.used_count > 0) {
        pool.used_count--;
    }
    return PoolResult::OK;
}

struct DuplicateAckState {
    uint16_t fragment_bitmap = 0;
    uint8_t receiver_credits = 0;
    framing_v3::FailureStatus failure = framing_v3::FailureStatus::NONE;
};

struct DuplicateWindowEntry {
    uint16_t datagram_id = 0;
    DuplicateAckState ack;
    bool valid = false;
};

struct DuplicateWindow {
    DuplicateWindowEntry entries[DUPLICATE_WINDOW_CAPACITY];
    uint16_t newest_id = 0;
    bool has_newest = false;
    uint8_t count = 0;
};

inline void resetDuplicateWindow(DuplicateWindow& window) {
    memset(&window, 0, sizeof(window));
}

inline int16_t serialDistance(uint16_t newer, uint16_t older) {
    return static_cast<int16_t>(newer - older);
}

inline bool serialAfter(uint16_t lhs, uint16_t rhs) {
    const int16_t distance = serialDistance(lhs, rhs);
    return distance > 0;
}

inline bool serialWithinWindow(uint16_t newest, uint16_t candidate) {
    const int16_t distance = serialDistance(newest, candidate);
    return distance >= 0 && distance < DUPLICATE_WINDOW_CAPACITY;
}

inline bool duplicateWindowFind(const DuplicateWindow& window,
                                uint16_t datagram_id,
                                DuplicateAckState* ack_out = nullptr) {
    for (uint8_t i = 0; i < DUPLICATE_WINDOW_CAPACITY; ++i) {
        const DuplicateWindowEntry& entry = window.entries[i];
        if (entry.valid && entry.datagram_id == datagram_id) {
            if (ack_out != nullptr) {
                *ack_out = entry.ack;
            }
            return true;
        }
    }
    return false;
}

inline void duplicateWindowPrune(DuplicateWindow& window) {
    if (!window.has_newest) {
        return;
    }

    uint8_t count = 0;
    for (uint8_t i = 0; i < DUPLICATE_WINDOW_CAPACITY; ++i) {
        DuplicateWindowEntry& entry = window.entries[i];
        if (entry.valid && !serialWithinWindow(window.newest_id, entry.datagram_id)) {
            entry.valid = false;
        }
        if (entry.valid) {
            count++;
        }
    }
    window.count = count;
}

inline void duplicateWindowStore(DuplicateWindow& window,
                                 uint16_t datagram_id,
                                 const DuplicateAckState& ack) {
    if (!window.has_newest || serialAfter(datagram_id, window.newest_id)) {
        window.newest_id = datagram_id;
        window.has_newest = true;
    }

    for (uint8_t i = 0; i < DUPLICATE_WINDOW_CAPACITY; ++i) {
        DuplicateWindowEntry& entry = window.entries[i];
        if (entry.valid && entry.datagram_id == datagram_id) {
            entry.ack = ack;
            duplicateWindowPrune(window);
            return;
        }
    }

    duplicateWindowPrune(window);

    uint8_t slot = DUPLICATE_WINDOW_CAPACITY;
    for (uint8_t i = 0; i < DUPLICATE_WINDOW_CAPACITY; ++i) {
        if (!window.entries[i].valid) {
            slot = i;
            break;
        }
    }

    if (slot == DUPLICATE_WINDOW_CAPACITY) {
        uint16_t oldest_distance = 0;
        slot = 0;
        for (uint8_t i = 0; i < DUPLICATE_WINDOW_CAPACITY; ++i) {
            const uint16_t age = static_cast<uint16_t>(
                serialDistance(window.newest_id, window.entries[i].datagram_id));
            if (age >= oldest_distance) {
                oldest_distance = age;
                slot = i;
            }
        }
    }

    window.entries[slot].valid = true;
    window.entries[slot].datagram_id = datagram_id;
    window.entries[slot].ack = ack;
    duplicateWindowPrune(window);
}

} // namespace arq

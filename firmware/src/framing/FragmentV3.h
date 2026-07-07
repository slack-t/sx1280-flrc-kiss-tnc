#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "FramingV3.h"

namespace framing_v3 {

enum class FragmentResult : uint8_t {
    OK = 0,
    BAD_ARGUMENT,
    BAD_LENGTH,
    TOO_MANY_FRAGMENTS,
    OUTPUT_TOO_SMALL,
    METADATA_MISMATCH,
    DUPLICATE,
};

struct FragmentDescriptor {
    DataHeader header;
    const uint8_t* payload = nullptr;
};

struct ReassemblyV3 {
    uint16_t datagram_id = 0;
    uint16_t datagram_length = 0;
    uint8_t fragment_count = 0;
    uint16_t received_bitmap = 0;
    uint8_t fragment_lengths[V3_MAX_FRAGS];
    uint8_t* output = nullptr;
    uint16_t output_capacity = 0;
    uint16_t bytes_received = 0;
    bool active = false;

    uint32_t duplicate_fragments = 0;
    uint32_t metadata_mismatches = 0;
    uint32_t rejected_fragments = 0;
};

inline uint16_t fragmentExpectedBitmap(uint8_t fragment_count) {
    if (fragment_count == 0 || fragment_count > V3_MAX_FRAGS) {
        return 0;
    }
    return static_cast<uint16_t>((1u << fragment_count) - 1u);
}

inline FragmentResult describeDatagram(const uint8_t* datagram,
                                       uint16_t datagram_length,
                                       uint16_t datagram_id,
                                       uint8_t flags,
                                       uint8_t queue_depth_hint,
                                       FragmentDescriptor* fragments,
                                       uint8_t fragment_capacity,
                                       uint8_t& fragment_count) {
    fragment_count = 0;
    if (datagram_length == 0 || datagram_length > V3_MAX_DATAGRAM) {
        return FragmentResult::BAD_LENGTH;
    }
    if (datagram == nullptr || fragments == nullptr) {
        return FragmentResult::BAD_ARGUMENT;
    }
    if (flags > V3_MAX_NIBBLE || queue_depth_hint > V3_MAX_NIBBLE) {
        return FragmentResult::BAD_ARGUMENT;
    }

    const uint8_t needed = expectedFragmentCount(datagram_length);
    if (needed == 0 || needed > V3_MAX_FRAGS) {
        return FragmentResult::TOO_MANY_FRAGMENTS;
    }
    if (fragment_capacity < needed) {
        return FragmentResult::OUTPUT_TOO_SMALL;
    }

    uint16_t offset = 0;
    for (uint8_t i = 0; i < needed; ++i) {
        DataHeader header;
        header.flags = flags;
        header.queue_depth_hint = queue_depth_hint;
        header.datagram_id = datagram_id;
        header.fragment_index = i;
        header.fragment_count = needed;
        header.datagram_length = datagram_length;
        header.payload_len = expectedFragmentPayloadLen(datagram_length, i, needed);
        if (!validateDataHeader(header)) {
            fragment_count = 0;
            return FragmentResult::BAD_LENGTH;
        }
        fragments[i].header = header;
        fragments[i].payload = datagram + offset;
        offset = static_cast<uint16_t>(offset + header.payload_len);
    }

    fragment_count = needed;
    return FragmentResult::OK;
}

inline void resetReassembly(ReassemblyV3& state,
                            uint8_t* output,
                            uint16_t output_capacity) {
    state.datagram_id = 0;
    state.datagram_length = 0;
    state.fragment_count = 0;
    state.received_bitmap = 0;
    memset(state.fragment_lengths, 0, sizeof(state.fragment_lengths));
    state.output = output;
    state.output_capacity = output_capacity;
    state.bytes_received = 0;
    state.active = false;
}

inline void resetReassemblyCounters(ReassemblyV3& state) {
    state.duplicate_fragments = 0;
    state.metadata_mismatches = 0;
    state.rejected_fragments = 0;
}

inline bool reassemblyIsComplete(const ReassemblyV3& state) {
    const uint16_t expected = fragmentExpectedBitmap(state.fragment_count);
    return state.active &&
           expected != 0 &&
           (state.received_bitmap & expected) == expected &&
           state.bytes_received == state.datagram_length;
}

inline uint16_t reassembledLength(const ReassemblyV3& state) {
    return reassemblyIsComplete(state) ? state.datagram_length : 0;
}

inline bool reassemblyMetadataMatches(const ReassemblyV3& state,
                                      const DataHeader& header) {
    return state.datagram_id == header.datagram_id &&
           state.datagram_length == header.datagram_length &&
           state.fragment_count == header.fragment_count;
}

inline FragmentResult acceptFragment(ReassemblyV3& state,
                                     const DataHeader& header,
                                     const uint8_t* payload) {
    if (!validateDataHeader(header)) {
        state.rejected_fragments++;
        return FragmentResult::BAD_LENGTH;
    }
    if (header.payload_len > 0 && payload == nullptr) {
        state.rejected_fragments++;
        return FragmentResult::BAD_ARGUMENT;
    }
    if (state.output == nullptr || state.output_capacity < header.datagram_length) {
        state.rejected_fragments++;
        return FragmentResult::OUTPUT_TOO_SMALL;
    }

    if (!state.active) {
        state.datagram_id = header.datagram_id;
        state.datagram_length = header.datagram_length;
        state.fragment_count = header.fragment_count;
        state.received_bitmap = 0;
        memset(state.fragment_lengths, 0, sizeof(state.fragment_lengths));
        state.bytes_received = 0;
        state.active = true;
    } else if (!reassemblyMetadataMatches(state, header)) {
        state.metadata_mismatches++;
        state.rejected_fragments++;
        return FragmentResult::METADATA_MISMATCH;
    }

    const uint16_t bit = static_cast<uint16_t>(1u << header.fragment_index);
    if ((state.received_bitmap & bit) != 0) {
        if (state.fragment_lengths[header.fragment_index] != header.payload_len) {
            state.metadata_mismatches++;
            state.rejected_fragments++;
            return FragmentResult::METADATA_MISMATCH;
        }
        state.duplicate_fragments++;
        return FragmentResult::DUPLICATE;
    }

    const uint16_t offset =
        static_cast<uint16_t>(header.fragment_index) * V3_FRAGMENT_PAYLOAD_MAX;
    if (offset > header.datagram_length ||
        static_cast<uint16_t>(offset + header.payload_len) > header.datagram_length ||
        static_cast<uint16_t>(offset + header.payload_len) > state.output_capacity) {
        state.rejected_fragments++;
        return FragmentResult::BAD_LENGTH;
    }

    if (header.payload_len > 0) {
        memcpy(state.output + offset, payload, header.payload_len);
    }
    state.fragment_lengths[header.fragment_index] = header.payload_len;
    state.received_bitmap = static_cast<uint16_t>(state.received_bitmap | bit);
    state.bytes_received = static_cast<uint16_t>(state.bytes_received + header.payload_len);
    return FragmentResult::OK;
}

} // namespace framing_v3

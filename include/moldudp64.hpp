#pragma once

#include <cstdint>
#include <cstddef>
#include <string_view>
#include "endian_utils.hpp"
#include "generated_itch_parser.hpp" // For dispatch_message

namespace itch50 {
namespace moldudp64 {

// ============================================================================
// MoldUDP64 Protocol Header
// ============================================================================
#pragma pack(push, 1)
struct MoldUDP64Header {
    char     session[10];
    uint64_t sequence_number;
    uint16_t message_count;
    
    // Zero-copy accessors (converts from big-endian)
    std::string_view session_name() const noexcept {
        return {session, 10};
    }
    uint64_t seq_num() const noexcept {
        return parser_utils::detail::read_be64(reinterpret_cast<const char*>(&sequence_number));
    }
    uint16_t count() const noexcept {
        return parser_utils::detail::read_be16(reinterpret_cast<const char*>(&message_count));
    }
};
#pragma pack(pop)

static_assert(sizeof(MoldUDP64Header) == 20, "MoldUDP64Header must be exactly 20 bytes");

// ============================================================================
// Zero-Copy Packet Parser
// ============================================================================
// Parses a single MoldUDP64 packet and passes the encapsulated ITCH messages 
// to the handler.
//
// Returns the number of bytes processed, or 0 on error.
template<typename Handler>
inline std::size_t parse_packet(Handler& handler, const char* buffer, std::size_t length) {
    if (length < sizeof(MoldUDP64Header)) return 0;
    
    auto* header = reinterpret_cast<const MoldUDP64Header*>(buffer);
    uint16_t count = header->count();
    
    std::size_t offset = sizeof(MoldUDP64Header);
    
    for (uint16_t i = 0; i < count; ++i) {
        if (offset + 2 > length) break;
        
        // ITCH messages inside MoldUDP64 are prefixed with a 2-byte length
        uint16_t msg_len = parser_utils::detail::read_be16(buffer + offset);
        if (offset + 2 + msg_len > length) break;
        
        char type = buffer[offset + 2];
        
        // Dispatch to the strongly-typed handler
        dispatch_message(handler, type, buffer + offset + 2);
        
        offset += 2 + msg_len;
    }
    
    return offset;
}

} // namespace moldudp64
} // namespace itch50

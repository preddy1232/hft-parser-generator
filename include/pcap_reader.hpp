// =============================================================================
// pcap_reader.hpp — Minimal PCAP file reader for ITCH/MoldUDP64 capture files
// =============================================================================
// Zero-copy, memory-mapped PCAP reader that:
//   - Reads the PCAP global header (magic, version, snaplen, link type)
//   - Supports both little-endian (0xa1b2c3d4) and big-endian (0xd4c3b2a1)
//   - Iterates over packets, stripping Ethernet (14B) + IP (variable) + UDP (8B)
//   - Extracts MoldUDP64 session messages and dispatches through the handler
//   - Uses the same CanHandle / DispatchTable pattern as the ITCH parser
//
// Usage:
//   itch50::PcapReader reader;
//   if (!reader.open("capture.pcap")) { /* error */ }
//   MyHandler handler;
//   auto stats = reader.process_file(handler);
// =============================================================================
#pragma once

#include "endian_utils.hpp"
#include "generated_itch_parser.hpp"
#include "itch_file_reader.hpp"   // For MappedFile, FileReaderStats

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <chrono>

namespace itch50 {
namespace detail {

// ─────────────────────────────────────────────────────────────────────────────
// PCAP file format structures (on-disk layout)
// ─────────────────────────────────────────────────────────────────────────────

// PCAP magic numbers
inline constexpr uint32_t PCAP_MAGIC_LE      = 0xa1b2c3d4u;  // Native little-endian
inline constexpr uint32_t PCAP_MAGIC_BE      = 0xd4c3b2a1u;  // Byte-swapped big-endian
inline constexpr uint32_t PCAP_MAGIC_NS_LE   = 0xa1b23c4du;  // Nanosecond-resolution LE
inline constexpr uint32_t PCAP_MAGIC_NS_BE   = 0x4d3cb2a1u;  // Nanosecond-resolution BE

// Link-layer header types we support
inline constexpr uint32_t LINKTYPE_ETHERNET  = 1;
inline constexpr uint32_t LINKTYPE_RAW       = 101;   // Raw IP (no link-layer header)
inline constexpr uint32_t LINKTYPE_LINUX_SLL = 113;   // Linux cooked capture

// Ethernet constants
inline constexpr std::size_t ETHERNET_HEADER_SIZE = 14;
inline constexpr uint16_t ETHERTYPE_IPV4 = 0x0800;
inline constexpr uint16_t ETHERTYPE_VLAN = 0x8100;

// IP protocol numbers
inline constexpr uint8_t IP_PROTO_UDP = 17;

// UDP header size
inline constexpr std::size_t UDP_HEADER_SIZE = 8;

// MoldUDP64 header size: Session (10B) + SequenceNumber (8B) + MessageCount (2B)
inline constexpr std::size_t MOLDUDP64_HEADER_SIZE = 20;

// ─────────────────────────────────────────────────────────────────────────────
// PCAP global header (24 bytes on disk)
// ─────────────────────────────────────────────────────────────────────────────
struct PcapGlobalHeader {
    uint32_t magic_number;
    uint16_t version_major;
    uint16_t version_minor;
    int32_t  thiszone;
    uint32_t sigfigs;
    uint32_t snaplen;
    uint32_t network;
};
static_assert(sizeof(PcapGlobalHeader) == 24, "PCAP global header must be 24 bytes");

// ─────────────────────────────────────────────────────────────────────────────
// PCAP packet header (16 bytes on disk)
// ─────────────────────────────────────────────────────────────────────────────
struct PcapPacketHeader {
    uint32_t ts_sec;
    uint32_t ts_usec;    // or ts_nsec for nanosecond-resolution captures
    uint32_t incl_len;   // Number of bytes captured
    uint32_t orig_len;   // Original packet length
};
static_assert(sizeof(PcapPacketHeader) == 16, "PCAP packet header must be 16 bytes");

// ─────────────────────────────────────────────────────────────────────────────
// Inline helpers for reading fields with optional byte-swap
// ─────────────────────────────────────────────────────────────────────────────

/// Read a uint16_t from memory, byte-swapping if `swap` is true.
[[nodiscard]] inline uint16_t pcap_read16(const char* p, bool swap) noexcept {
    uint16_t v;
    std::memcpy(&v, p, sizeof(v));
    return swap ? parser_utils::detail::bswap16(v) : v;
}

/// Read a uint32_t from memory, byte-swapping if `swap` is true.
[[nodiscard]] inline uint32_t pcap_read32(const char* p, bool swap) noexcept {
    uint32_t v;
    std::memcpy(&v, p, sizeof(v));
    return swap ? parser_utils::detail::bswap32(v) : v;
}

} // namespace detail

// ─────────────────────────────────────────────────────────────────────────────
// PCAP file metadata (populated after open())
// ─────────────────────────────────────────────────────────────────────────────
struct PcapInfo {
    uint16_t version_major = 0;
    uint16_t version_minor = 0;
    uint32_t snaplen       = 0;
    uint32_t link_type     = 0;
    bool     byte_swap     = false;   // True if PCAP byte order differs from host
    bool     nanosecond    = false;   // True if timestamps are nanosecond resolution
};

// ─────────────────────────────────────────────────────────────────────────────
// PCAP reader statistics (extends FileReaderStats with PCAP-specific fields)
// ─────────────────────────────────────────────────────────────────────────────
struct PcapReaderStats {
    FileReaderStats  base;
    uint64_t         packets_total     = 0;  // Total PCAP packets seen
    uint64_t         packets_udp       = 0;  // UDP packets
    uint64_t         packets_skipped   = 0;  // Non-UDP or malformed packets
    uint64_t         moldudp64_sessions = 0; // MoldUDP64 datagrams processed

    // Forward convenience accessors
    [[nodiscard]] double messages_per_second() const noexcept { return base.messages_per_second(); }
    [[nodiscard]] double gigabytes_per_second() const noexcept { return base.gigabytes_per_second(); }
};

// ─────────────────────────────────────────────────────────────────────────────
// PcapReader — Memory-mapped PCAP file reader with MoldUDP64 extraction
// ─────────────────────────────────────────────────────────────────────────────
class PcapReader {
public:
    PcapReader() = default;

    /// Open a PCAP file and parse the global header. Returns false on failure.
    [[nodiscard]] bool open(const char* path) noexcept {
        if (!file_.open(path)) return false;

        if (file_.size() < sizeof(detail::PcapGlobalHeader)) {
            file_.close();
            return false;
        }

        // Parse global header
        const auto* ghdr = reinterpret_cast<const detail::PcapGlobalHeader*>(file_.data());
        uint32_t magic = ghdr->magic_number;

        if (magic == detail::PCAP_MAGIC_LE || magic == detail::PCAP_MAGIC_NS_LE) {
            info_.byte_swap = false;
            info_.nanosecond = (magic == detail::PCAP_MAGIC_NS_LE);
        } else if (magic == detail::PCAP_MAGIC_BE || magic == detail::PCAP_MAGIC_NS_BE) {
            info_.byte_swap = true;
            info_.nanosecond = (magic == detail::PCAP_MAGIC_NS_BE);
        } else {
            // Unknown magic — not a valid PCAP file
            file_.close();
            return false;
        }

        info_.version_major = detail::pcap_read16(
            reinterpret_cast<const char*>(&ghdr->version_major), info_.byte_swap);
        info_.version_minor = detail::pcap_read16(
            reinterpret_cast<const char*>(&ghdr->version_minor), info_.byte_swap);
        info_.snaplen = detail::pcap_read32(
            reinterpret_cast<const char*>(&ghdr->snaplen), info_.byte_swap);
        info_.link_type = detail::pcap_read32(
            reinterpret_cast<const char*>(&ghdr->network), info_.byte_swap);

        // Validate link type
        if (info_.link_type != detail::LINKTYPE_ETHERNET &&
            info_.link_type != detail::LINKTYPE_RAW &&
            info_.link_type != detail::LINKTYPE_LINUX_SLL) {
            file_.close();
            return false;
        }

        offset_ = sizeof(detail::PcapGlobalHeader);
        return true;
    }

    /// Close the file and release resources.
    void close() noexcept {
        file_.close();
        offset_ = 0;
        info_ = {};
    }

    /// Get PCAP file metadata.
    [[nodiscard]] const PcapInfo& info() const noexcept { return info_; }

    /// Check if the file is open.
    [[nodiscard]] bool is_open() const noexcept { return file_.is_open(); }

    // ─────────────────────────────────────────────────────────────────────
    // Bulk processing: iterate all packets, extract MoldUDP64 payloads,
    // and dispatch ITCH messages through the Handler.
    // ─────────────────────────────────────────────────────────────────────
    template<typename Handler>
    [[nodiscard]] PcapReaderStats process_file(Handler& handler) noexcept {
        PcapReaderStats stats{};
        stats.base.total_file_bytes = file_.size();

        const auto t0 = std::chrono::steady_clock::now();

        offset_ = sizeof(detail::PcapGlobalHeader);
        const char* buf = file_.data();
        const std::size_t file_len = file_.size();

        DispatchTable<Handler> table;

        while (offset_ + sizeof(detail::PcapPacketHeader) <= file_len) {
            // Read packet header
            const auto* phdr = reinterpret_cast<const detail::PcapPacketHeader*>(buf + offset_);
            uint32_t incl_len = detail::pcap_read32(
                reinterpret_cast<const char*>(&phdr->incl_len), info_.byte_swap);

            offset_ += sizeof(detail::PcapPacketHeader);
            if (offset_ + incl_len > file_len) break;  // Truncated capture

            stats.packets_total++;

            const char* pkt_data = buf + offset_;
            std::size_t pkt_len  = incl_len;

            // Strip link-layer header and extract IP payload
            const char* ip_start = nullptr;
            std::size_t ip_avail = 0;

            if (!strip_link_layer(pkt_data, pkt_len, ip_start, ip_avail)) {
                stats.packets_skipped++;
                offset_ += incl_len;
                continue;
            }

            // Parse IP header
            if (ip_avail < 20) {
                stats.packets_skipped++;
                offset_ += incl_len;
                continue;
            }

            uint8_t ip_ver_ihl = static_cast<uint8_t>(ip_start[0]);
            uint8_t ip_version = (ip_ver_ihl >> 4) & 0x0F;
            if (ip_version != 4) {
                stats.packets_skipped++;
                offset_ += incl_len;
                continue;
            }

            std::size_t ip_hdr_len = static_cast<std::size_t>(ip_ver_ihl & 0x0F) * 4;
            if (ip_hdr_len < 20 || ip_hdr_len > ip_avail) {
                stats.packets_skipped++;
                offset_ += incl_len;
                continue;
            }

            // Check for UDP protocol
            uint8_t protocol = static_cast<uint8_t>(ip_start[9]);
            if (protocol != detail::IP_PROTO_UDP) {
                stats.packets_skipped++;
                offset_ += incl_len;
                continue;
            }

            stats.packets_udp++;

            // Strip UDP header
            const char* udp_start = ip_start + ip_hdr_len;
            std::size_t udp_avail = ip_avail - ip_hdr_len;

            if (udp_avail < detail::UDP_HEADER_SIZE) {
                stats.packets_skipped++;
                offset_ += incl_len;
                continue;
            }

            const char* payload = udp_start + detail::UDP_HEADER_SIZE;
            std::size_t payload_len = udp_avail - detail::UDP_HEADER_SIZE;

            // Parse MoldUDP64 payload
            if (payload_len < detail::MOLDUDP64_HEADER_SIZE) {
                stats.packets_skipped++;
                offset_ += incl_len;
                continue;
            }

            // MoldUDP64 header: Session(10) + SequenceNumber(8) + MessageCount(2)
            uint16_t msg_count = parser_utils::detail::read_be16(payload + 18);
            const char* msg_ptr = payload + detail::MOLDUDP64_HEADER_SIZE;
            std::size_t remaining = payload_len - detail::MOLDUDP64_HEADER_SIZE;

            stats.moldudp64_sessions++;

            // Iterate through each message in the MoldUDP64 packet
            for (uint16_t i = 0; i < msg_count; ++i) {
                if (remaining < 2) break;

                uint16_t msg_len = parser_utils::detail::read_be16(msg_ptr);
                msg_ptr   += 2;
                remaining -= 2;

                if (msg_len == 0 || msg_len > remaining) break;

                char type = msg_ptr[0];
                if (table.dispatch(handler, type, msg_ptr)) {
                    stats.base.messages_parsed++;
                } else {
                    stats.base.unknown_messages++;
                }

                msg_ptr   += msg_len;
                remaining -= msg_len;
            }

            offset_ += incl_len;
        }

        stats.base.bytes_processed = offset_;

        const auto t1 = std::chrono::steady_clock::now();
        stats.base.elapsed_seconds = std::chrono::duration<double>(t1 - t0).count();

        return stats;
    }

private:
    detail::MappedFile file_;
    std::size_t        offset_ = 0;
    PcapInfo           info_{};

    // ─────────────────────────────────────────────────────────────────────
    // Strip the link-layer header based on the PCAP link type.
    // Returns false if the packet should be skipped.
    // ─────────────────────────────────────────────────────────────────────
    [[nodiscard]] bool strip_link_layer(
        const char* pkt_data, std::size_t pkt_len,
        const char*& ip_start, std::size_t& ip_avail
    ) const noexcept {
        switch (info_.link_type) {
            case detail::LINKTYPE_ETHERNET: {
                if (pkt_len < detail::ETHERNET_HEADER_SIZE) return false;

                // Check EtherType, handle VLAN tags
                std::size_t eth_offset = 12;  // EtherType field offset
                uint16_t ethertype = parser_utils::detail::read_be16(pkt_data + eth_offset);

                // Strip 802.1Q VLAN tags (one or more)
                while (ethertype == detail::ETHERTYPE_VLAN && eth_offset + 4 <= pkt_len) {
                    eth_offset += 4;
                    if (eth_offset + 2 > pkt_len) return false;
                    ethertype = parser_utils::detail::read_be16(pkt_data + eth_offset);
                }

                if (ethertype != detail::ETHERTYPE_IPV4) return false;

                std::size_t header_end = eth_offset + 2;
                ip_start = pkt_data + header_end;
                ip_avail = pkt_len - header_end;
                return true;
            }

            case detail::LINKTYPE_RAW: {
                // No link-layer header — data starts with IP
                ip_start = pkt_data;
                ip_avail = pkt_len;
                return true;
            }

            case detail::LINKTYPE_LINUX_SLL: {
                // Linux cooked capture: 16-byte header
                constexpr std::size_t SLL_HEADER_SIZE = 16;
                if (pkt_len < SLL_HEADER_SIZE) return false;

                uint16_t proto = parser_utils::detail::read_be16(pkt_data + 14);
                if (proto != detail::ETHERTYPE_IPV4) return false;

                ip_start = pkt_data + SLL_HEADER_SIZE;
                ip_avail = pkt_len - SLL_HEADER_SIZE;
                return true;
            }

            default:
                return false;
        }
    }
};

} // namespace itch50

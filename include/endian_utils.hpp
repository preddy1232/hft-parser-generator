// =============================================================================
// endian_utils.hpp — Hand-written endianness and buffer utilities
// =============================================================================
// This is a manually maintained header with utilities that the generated parser
// depends on. It is NOT generated — it is part of the project infrastructure.
// =============================================================================
#pragma once

#include <cstdint>
#include <cstring>
#include <string_view>
#include <bit>

namespace itch50 {
namespace detail {

// ─────────────────────────────────────────────────────────────────────
// Compile-time endian swap: uses __builtin_bswap on GCC/Clang for a
// single instruction (bswap on x86, rev on ARM). Falls back to
// portable shift-based implementation otherwise.
// ─────────────────────────────────────────────────────────────────────

[[nodiscard]] constexpr uint16_t bswap16(uint16_t v) noexcept {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_bswap16(v);
#else
    return static_cast<uint16_t>((v >> 8) | (v << 8));
#endif
}

[[nodiscard]] constexpr uint32_t bswap32(uint32_t v) noexcept {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_bswap32(v);
#else
    return ((v & 0xff000000u) >> 24) |
           ((v & 0x00ff0000u) >> 8)  |
           ((v & 0x0000ff00u) << 8)  |
           ((v & 0x000000ffu) << 24);
#endif
}

[[nodiscard]] constexpr uint64_t bswap64(uint64_t v) noexcept {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_bswap64(v);
#else
    return ((v & 0xff00000000000000ULL) >> 56) |
           ((v & 0x00ff000000000000ULL) >> 40) |
           ((v & 0x0000ff0000000000ULL) >> 24) |
           ((v & 0x000000ff00000000ULL) >> 8)  |
           ((v & 0x00000000ff000000ULL) << 8)  |
           ((v & 0x0000000000ff0000ULL) << 24) |
           ((v & 0x000000000000ff00ULL) << 40) |
           ((v & 0x00000000000000ffULL) << 56);
#endif
}

// ─────────────────────────────────────────────────────────────────────
// Big-endian → host byte order readers.
// Zero-copy: reads directly from the buffer via memcpy (which the
// compiler eliminates entirely at -O2+, producing a single load
// instruction + optional bswap).
// ─────────────────────────────────────────────────────────────────────

[[nodiscard]] inline uint16_t read_be16(const char* p) noexcept {
    uint16_t v;
    std::memcpy(&v, p, sizeof(v));
    if constexpr (std::endian::native == std::endian::little) {
        return bswap16(v);
    }
    return v;
}

[[nodiscard]] inline uint32_t read_be32(const char* p) noexcept {
    uint32_t v;
    std::memcpy(&v, p, sizeof(v));
    if constexpr (std::endian::native == std::endian::little) {
        return bswap32(v);
    }
    return v;
}

[[nodiscard]] inline uint64_t read_be64(const char* p) noexcept {
    uint64_t v;
    std::memcpy(&v, p, sizeof(v));
    if constexpr (std::endian::native == std::endian::little) {
        return bswap64(v);
    }
    return v;
}

// 48-bit (6-byte) big-endian timestamp → uint64_t
// ITCH timestamps are nanoseconds since midnight, stored in 6 bytes.
[[nodiscard]] inline uint64_t read_be48(const char* p) noexcept {
    const auto* u = reinterpret_cast<const uint8_t*>(p);
    return (static_cast<uint64_t>(u[0]) << 40) |
           (static_cast<uint64_t>(u[1]) << 32) |
           (static_cast<uint64_t>(u[2]) << 24) |
           (static_cast<uint64_t>(u[3]) << 16) |
           (static_cast<uint64_t>(u[4]) << 8)  |
           (static_cast<uint64_t>(u[5]));
}

// ─────────────────────────────────────────────────────────────────────
// Fixed-length string view (zero-copy, points into the buffer).
// Returns a string_view of exactly `N` bytes starting at `p`.
// ─────────────────────────────────────────────────────────────────────

template<std::size_t N>
[[nodiscard]] inline std::string_view read_alpha(const char* p) noexcept {
    return std::string_view(p, N);
}

// Trim trailing spaces from an ITCH alpha field.
[[nodiscard]] inline std::string_view trim_right(std::string_view sv) noexcept {
    const auto pos = sv.find_last_not_of(' ');
    return (pos == std::string_view::npos) ? std::string_view{} : sv.substr(0, pos + 1);
}

} // namespace detail
} // namespace itch50

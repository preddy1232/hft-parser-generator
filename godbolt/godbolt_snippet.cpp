// =============================================================================
// godbolt_snippet.cpp — Minimal self-contained snippet for Compiler Explorer
// =============================================================================
// Paste this into https://godbolt.org with flags: -std=c++20 -O3 -march=native
// to verify the generated parser compiles to tight, branch-free assembly.
// =============================================================================

#include <cstdint>
#include <cstring>
#include <bit>

// --- From endian_utils.hpp ---
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
    return ((v & 0xff000000u) >> 24) | ((v & 0x00ff0000u) >> 8) | ((v & 0x0000ff00u) << 8) | ((v & 0x000000ffu) << 24);
#endif
}
[[nodiscard]] constexpr uint64_t bswap64(uint64_t v) noexcept {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_bswap64(v);
#else
    return ((v & 0xff00000000000000ULL) >> 56) | ((v & 0x00ff000000000000ULL) >> 40) |
           ((v & 0x0000ff0000000000ULL) >> 24) | ((v & 0x000000ff00000000ULL) >> 8)  |
           ((v & 0x00000000ff000000ULL) << 8)  | ((v & 0x0000000000ff0000ULL) << 24) |
           ((v & 0x000000000000ff00ULL) << 40) | ((v & 0x00000000000000ffULL) << 56);
#endif
}

[[nodiscard]] inline uint32_t read_be32(const char* p) noexcept {
    uint32_t v; std::memcpy(&v, p, sizeof(v));
    if constexpr (std::endian::native == std::endian::little) return bswap32(v);
    return v;
}
[[nodiscard]] inline uint64_t read_be64(const char* p) noexcept {
    uint64_t v; std::memcpy(&v, p, sizeof(v));
    if constexpr (std::endian::native == std::endian::little) return bswap64(v);
    return v;
}

// --- From generated_itch_parser.hpp (AddOrder struct only) ---
#pragma pack(push, 1)
struct AddOrder {
    char       message_type_;  // always 'A'
    uint16_t   stock_locate_;
    uint16_t   tracking_number_;
    uint8_t    timestamp_[6];
    uint64_t   order_reference_number_;
    char       buy_sell_indicator_;
    uint32_t   shares_;
    char       stock_[8];
    uint32_t   price_;

    [[nodiscard]] uint64_t order_reference_number() const noexcept { return read_be64(reinterpret_cast<const char*>(&order_reference_number_)); }
    [[nodiscard]] uint32_t shares() const noexcept { return read_be32(reinterpret_cast<const char*>(&shares_)); }
    [[nodiscard]] uint32_t price() const noexcept { return read_be32(reinterpret_cast<const char*>(&price_)); }
};
#pragma pack(pop)

// --- Assembly Analysis Target ---
// We pass a raw buffer. The function parses the order reference number, 
// shares, and price, and returns their sum to prevent dead-code elimination.
// 
// EXPECTED ASSEMBLY (x86-64 -O3 -march=native):
// You should see exactly 3 `movbe` or `bswap` instructions and zero branches.
extern "C" uint64_t parse_add_order_godbolt(const char* buf) {
    auto* msg = reinterpret_cast<const AddOrder*>(buf);
    return msg->order_reference_number() + msg->shares() + msg->price();
}

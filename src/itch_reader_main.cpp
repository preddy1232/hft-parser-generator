// =============================================================================
// itch_reader_main.cpp — CLI tool for reading ITCH binary and PCAP files
// =============================================================================
// Reads NASDAQ ITCH 5.0 data from either raw binary files (2-byte length-
// prefixed messages) or PCAP capture files (with Ethernet/IP/UDP/MoldUDP64
// extraction). Auto-detects file type based on extension.
//
// Usage:
//   itch_reader <filename> [--stats] [--verbose] [--quiet]
//
// Flags:
//   --stats    Print per-message-type counts (default)
//   --verbose  Print each individual message as it is parsed
//   --quiet    Print only the summary line (total messages + throughput)
// =============================================================================
#include "itch_file_reader.hpp"
#include "pcap_reader.hpp"

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cinttypes>

using namespace itch50;

// ─────────────────────────────────────────────────────────────────────────────
// Verbosity modes
// ─────────────────────────────────────────────────────────────────────────────
enum class Verbosity {
    Stats,    // Per-type counts + summary (default)
    Verbose,  // Print each message + counts + summary
    Quiet     // Only summary totals
};

// ─────────────────────────────────────────────────────────────────────────────
// CountingHandler — counts each message type, optionally prints verbose info
// ─────────────────────────────────────────────────────────────────────────────
struct CountingHandler {
    uint64_t  counts[256] = {};  // Index by message type character
    Verbosity verbosity   = Verbosity::Stats;

    // Generic handle for all message types — uses a macro to avoid repetition.
    // Each handle() increments the count and optionally prints info.
#define ITCH_COUNTING_HANDLER(MsgType, TypeChar)                               \
    void handle(const MsgType& /* msg */) noexcept {                           \
        counts[static_cast<uint8_t>(TypeChar)]++;                              \
        if (verbosity == Verbosity::Verbose) {                                 \
            std::printf("[%c] %s #%" PRIu64 "\n",                              \
                TypeChar, message_name(TypeChar),                               \
                counts[static_cast<uint8_t>(TypeChar)]);                        \
        }                                                                      \
    }

    ITCH_COUNTING_HANDLER(SystemEvent,               'S')
    ITCH_COUNTING_HANDLER(StockDirectory,             'R')
    ITCH_COUNTING_HANDLER(StockTradingAction,         'H')
    ITCH_COUNTING_HANDLER(RegSHORestriction,          'Y')
    ITCH_COUNTING_HANDLER(MarketParticipantPosition,  'L')
    ITCH_COUNTING_HANDLER(MWCBDeclineLevel,           'V')
    ITCH_COUNTING_HANDLER(MWCBStatus,                 'W')
    ITCH_COUNTING_HANDLER(IPOQuotingPeriodUpdate,     'K')
    ITCH_COUNTING_HANDLER(LULDAuctionCollar,          'J')
    ITCH_COUNTING_HANDLER(OperationalHalt,            'h')
    ITCH_COUNTING_HANDLER(AddOrder,                   'A')
    ITCH_COUNTING_HANDLER(AddOrderMPIDAttribution,    'F')
    ITCH_COUNTING_HANDLER(OrderExecuted,              'E')
    ITCH_COUNTING_HANDLER(OrderExecutedWithPrice,     'C')
    ITCH_COUNTING_HANDLER(OrderCancel,                'X')
    ITCH_COUNTING_HANDLER(OrderDelete,                'D')
    ITCH_COUNTING_HANDLER(OrderReplace,               'U')
    ITCH_COUNTING_HANDLER(TradeNonCross,              'P')
    ITCH_COUNTING_HANDLER(CrossTrade,                 'Q')
    ITCH_COUNTING_HANDLER(BrokenTrade,                'B')
    ITCH_COUNTING_HANDLER(NOII,                       'I')
    ITCH_COUNTING_HANDLER(RetailInterest,             'N')

#undef ITCH_COUNTING_HANDLER

    // ─────────────────────────────────────────────────────────────────────
    // Print per-type counts
    // ─────────────────────────────────────────────────────────────────────
    void print_counts() const noexcept {
        std::puts("\n  Message Type                    Count");
        std::puts("  ─────────────────────────────────────");
        for (char type : ALL_MESSAGE_TYPES) {
            uint64_t c = counts[static_cast<uint8_t>(type)];
            if (c > 0) {
                std::printf("  [%c] %-28s %'" PRIu64 "\n",
                    type, message_name(type), c);
            }
        }
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// File type detection
// ─────────────────────────────────────────────────────────────────────────────
namespace detail {

[[nodiscard]] inline bool has_extension(const char* path, const char* ext) noexcept {
    const char* dot = nullptr;
    for (const char* p = path; *p; ++p) {
        if (*p == '.') dot = p;
    }
    if (!dot) return false;

    // Case-insensitive comparison
    for (std::size_t i = 0; ext[i] != '\0'; ++i) {
        char a = dot[i];
        char b = ext[i];
        // Lowercase ASCII
        if (a >= 'A' && a <= 'Z') a = static_cast<char>(a + ('a' - 'A'));
        if (b >= 'A' && b <= 'Z') b = static_cast<char>(b + ('a' - 'A'));
        if (a != b) return false;
    }
    return dot[std::strlen(ext)] == '\0';
}

} // namespace detail

// ─────────────────────────────────────────────────────────────────────────────
// Summary printing
// ─────────────────────────────────────────────────────────────────────────────
static void print_summary(const FileReaderStats& stats) noexcept {
    std::puts("\n  ══════════════════════════════════════");
    std::puts("  Summary");
    std::puts("  ──────────────────────────────────────");
    std::printf("  Messages parsed:    %'" PRIu64 "\n", stats.messages_parsed);
    std::printf("  Unknown messages:   %'" PRIu64 "\n", stats.unknown_messages);
    std::printf("  Bytes processed:    %'" PRIu64 "\n", stats.bytes_processed);
    std::printf("  File size:          %'" PRIu64 "\n", stats.total_file_bytes);
    std::printf("  Elapsed time:       %.6f s\n", stats.elapsed_seconds);
    std::printf("  Throughput:         %.2f M msgs/sec\n",
        stats.messages_per_second() / 1e6);
    std::printf("  Bandwidth:          %.3f GB/s\n",
        stats.gigabytes_per_second());
    std::puts("  ══════════════════════════════════════");
}

static void print_pcap_info(const PcapInfo& info) noexcept {
    std::puts("  PCAP Info:");
    std::printf("    Version:      %u.%u\n", info.version_major, info.version_minor);
    std::printf("    Snap length:  %u\n", info.snaplen);
    std::printf("    Link type:    %u", info.link_type);
    if (info.link_type == detail::LINKTYPE_ETHERNET) std::puts(" (Ethernet)");
    else if (info.link_type == detail::LINKTYPE_RAW) std::puts(" (Raw IP)");
    else if (info.link_type == detail::LINKTYPE_LINUX_SLL) std::puts(" (Linux SLL)");
    else std::puts("");
    std::printf("    Byte swap:    %s\n", info.byte_swap ? "yes" : "no");
    std::printf("    Nanosecond:   %s\n", info.nanosecond ? "yes" : "no");
}

static void print_pcap_summary(const PcapReaderStats& stats) noexcept {
    std::puts("\n  PCAP Statistics:");
    std::printf("    Packets total:    %'" PRIu64 "\n", stats.packets_total);
    std::printf("    UDP packets:      %'" PRIu64 "\n", stats.packets_udp);
    std::printf("    Skipped packets:  %'" PRIu64 "\n", stats.packets_skipped);
    std::printf("    MoldUDP64 msgs:   %'" PRIu64 "\n", stats.moldudp64_sessions);
    print_summary(stats.base);
}

// ─────────────────────────────────────────────────────────────────────────────
// Usage
// ─────────────────────────────────────────────────────────────────────────────
static void print_usage(const char* argv0) noexcept {
    std::printf("Usage: %s <filename> [--stats] [--verbose] [--quiet]\n\n", argv0);
    std::puts("  Reads NASDAQ ITCH 5.0 messages from a binary or PCAP file.\n");
    std::puts("  File type is auto-detected by extension:");
    std::puts("    .pcap, .pcapng  →  PCAP reader (Ethernet/IP/UDP/MoldUDP64)");
    std::puts("    (anything else) →  Raw ITCH binary (2-byte length prefix)\n");
    std::puts("  Flags:");
    std::puts("    --stats    Print per-message-type counts (default)");
    std::puts("    --verbose  Print each message as it is parsed");
    std::puts("    --quiet    Print only the summary line");
}

// ─────────────────────────────────────────────────────────────────────────────
// Main
// ─────────────────────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage(argc > 0 ? argv[0] : "itch_reader");
        return 1;
    }

    const char* filename = nullptr;
    Verbosity verbosity = Verbosity::Stats;

    // Parse arguments
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--stats") == 0) {
            verbosity = Verbosity::Stats;
        } else if (std::strcmp(argv[i], "--verbose") == 0) {
            verbosity = Verbosity::Verbose;
        } else if (std::strcmp(argv[i], "--quiet") == 0) {
            verbosity = Verbosity::Quiet;
        } else if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (argv[i][0] == '-') {
            std::fprintf(stderr, "Error: Unknown flag '%s'\n", argv[i]);
            return 1;
        } else {
            if (filename) {
                std::fprintf(stderr, "Error: Multiple filenames specified\n");
                return 1;
            }
            filename = argv[i];
        }
    }

    if (!filename) {
        std::fprintf(stderr, "Error: No filename specified\n");
        print_usage(argv[0]);
        return 1;
    }

    // Detect file type
    bool is_pcap = detail::has_extension(filename, ".pcap") ||
                   detail::has_extension(filename, ".pcapng");

    CountingHandler handler;
    handler.verbosity = verbosity;

    std::puts("HFT ITCH 5.0 File Reader");
    std::puts("════════════════════════════════════════");
    std::printf("  File:      %s\n", filename);
    std::printf("  Format:    %s\n", is_pcap ? "PCAP" : "Raw ITCH binary");
    std::printf("  Verbosity: %s\n",
        verbosity == Verbosity::Verbose ? "verbose" :
        verbosity == Verbosity::Quiet   ? "quiet"   : "stats");

    if (is_pcap) {
        PcapReader reader;
        if (!reader.open(filename)) {
            std::fprintf(stderr, "Error: Failed to open PCAP file '%s'\n", filename);
            return 1;
        }

        if (verbosity != Verbosity::Quiet) {
            print_pcap_info(reader.info());
        }

        std::puts("\n  Processing...");
        auto stats = reader.process_file(handler);
        reader.close();

        if (verbosity != Verbosity::Quiet) {
            handler.print_counts();
        }
        print_pcap_summary(stats);
    } else {
        ItchFileReader reader;
        if (!reader.open(filename)) {
            std::fprintf(stderr, "Error: Failed to open ITCH file '%s'\n", filename);
            return 1;
        }

        std::printf("  File size: %zu bytes\n", reader.file_size());
        std::puts("\n  Processing...");

        auto stats = reader.process_file(handler);
        reader.close();

        if (verbosity != Verbosity::Quiet) {
            handler.print_counts();
        }
        print_summary(stats);
    }

    return 0;
}

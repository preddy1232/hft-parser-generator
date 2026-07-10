#include "udp_receiver.hpp"
#include "moldudp64.hpp"
#include "generated_itch_parser.hpp"
#include <iostream>
#include <vector>
#include <chrono>

using namespace itch50;

// ============================================================================
// Zero-Copy Event Handler for Live Feeds
// ============================================================================
struct LiveHandler {
    uint64_t total_messages = 0;
    uint64_t order_adds = 0;
    uint64_t trades = 0;

    // Generic fallback for all types
    template <typename T>
    inline void handle(const T&) noexcept {
        total_messages++;
    }

    inline void handle(const AddOrder& msg) noexcept {
        (void)msg;
        total_messages++;
        order_adds++;
    }
    
    inline void handle(const OrderExecuted& msg) noexcept {
        (void)msg;
        total_messages++;
        trades++;
    }
    
    inline void handle(const OrderExecutedWithPrice& msg) noexcept {
        (void)msg;
        total_messages++;
        trades++;
    }

    inline void handle(const TradeNonCross& msg) noexcept {
        (void)msg;
        total_messages++;
        trades++;
    }

    inline void handle(const CrossTrade& msg) noexcept {
        (void)msg;
        total_messages++;
        trades++;
    }
};

int main(int argc, char* argv[]) {
    // NASDAQ TotalView-ITCH Example Multicast Group (A feed)
    std::string ip = "233.54.12.111"; 
    uint16_t port = 12345; 

    if (argc > 1) ip = argv[1];
    if (argc > 2) port = static_cast<uint16_t>(std::stoi(argv[2]));

    std::cout << "====================================================\n"
              << " HFT Zero-Copy Live Ingestion (MoldUDP64)\n"
              << "====================================================\n"
              << " Listening on Multicast Group : " << ip << ":" << port << "\n"
              << " Press Ctrl+C to stop.\n"
              << "====================================================\n";

    // Constructor will abort on failure (no exceptions)
    net::UdpMulticastReceiver receiver(ip, port);
    LiveHandler handler;

    // MoldUDP64 packets usually fit within standard Ethernet MTU (1500 bytes)
    // We use 2048 to be safe and avoid allocations during receive
    std::vector<char> buffer(2048);
    
    auto start = std::chrono::steady_clock::now();
    uint64_t packets_received = 0;

    while (true) {
        int bytes = receiver.receive(buffer.data(), buffer.size());
        if (bytes > 0) {
            packets_received++;
            
            // Parse the packet inline
            moldudp64::parse_packet(handler, buffer.data(), static_cast<std::size_t>(bytes));

            // Print stats every 100,000 packets
            if (packets_received % 100000 == 0) {
                auto now = std::chrono::steady_clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();
                std::cout << "[Stats] Packets: " << packets_received 
                          << " | ITCH Msgs: " << handler.total_messages
                          << " | Adds: " << handler.order_adds
                          << " | Trades: " << handler.trades
                          << " | Uptime: " << elapsed << "ms\n";
            }
        }
    }

    return 0;
}

// =============================================================================
// main.cpp — Integration driver for the ITCH 5.0 Zero-Copy Parser
// =============================================================================
#include "generated_itch_parser.hpp"
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <iostream>

using namespace itch50;

// A sample handler that only cares about AddOrder and OrderExecuted messages.
struct OrderBookHandler {
    uint32_t add_count = 0;
    uint32_t exec_count = 0;

    void handle(const AddOrder& msg) {
        add_count++;
        std::cout << "[AddOrder] Ref: " << msg.order_reference_number() 
                  << " | Stock: " << msg.stock_trimmed() 
                  << " | Side: " << msg.buy_sell_indicator()
                  << " | Shares: " << msg.shares() 
                  << " | Price: " << msg.price_double() << "\n";
    }

    void handle(const OrderExecuted& msg) {
        exec_count++;
        std::cout << "[OrderExecuted] Ref: " << msg.order_reference_number() 
                  << " | Executed Shares: " << msg.executed_shares() 
                  << " | Match Num: " << msg.match_number() << "\n";
    }
};

int main() {
    std::puts("HFT ITCH 5.0 Zero-Copy Parser — Driver");
    std::puts("========================================");

    // 1. Construct a mock UDP buffer containing a MoldUDP64 length prefix 
    //    and an ITCH 5.0 AddOrder message.
    // 
    // AddOrder structure (36 bytes):
    // 'A' (1), Locate (2), Tracking (2), Timestamp (6), RefNum (8), 
    // Buy/Sell (1), Shares (4), Stock (8), Price (4)
    
    // Using a raw byte array to simulate a network packet
    uint8_t buffer[64] = {0};
    
    // MoldUDP64 Message Length (36 bytes) - Big Endian
    buffer[0] = 0;
    buffer[1] = 36;
    
    // --- AddOrder Message ---
    uint8_t* msg = buffer + 2;
    msg[0] = 'A'; // Message Type
    
    // Stock Locate: 1234
    msg[1] = (1234 >> 8) & 0xFF;
    msg[2] = 1234 & 0xFF;
    
    // Order Reference Number: 987654321
    uint64_t ref_num = 987654321;
    for(int i=0; i<8; ++i) msg[11 + i] = (ref_num >> (56 - (i*8))) & 0xFF;
    
    // Buy/Sell Indicator
    msg[19] = 'B';
    
    // Shares: 100
    uint32_t shares = 100;
    for(int i=0; i<4; ++i) msg[20 + i] = (shares >> (24 - (i*8))) & 0xFF;
    
    // Stock: "AAPL    "
    std::memcpy(msg + 24, "AAPL    ", 8);
    
    // Price: 150.25 (1502500 in fixed point 4)
    uint32_t price = 1502500;
    for(int i=0; i<4; ++i) msg[32 + i] = (price >> (24 - (i*8))) & 0xFF;

    OrderBookHandler handler;
    
    std::puts("\n--- Using parse_stream ---");
    parse_stream(handler, reinterpret_cast<const char*>(buffer), sizeof(buffer));

    std::puts("\n--- Using O(1) DispatchTable directly ---");
    DispatchTable<OrderBookHandler> table;
    table.dispatch(handler, 'A', reinterpret_cast<const char*>(msg));

    std::puts("\nDone.");
    return 0;
}

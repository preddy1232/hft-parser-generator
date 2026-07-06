#include "market.hpp"
#include "generated_itch_parser.hpp"
#include "itch_file_reader.hpp"
#include <iostream>
#include <string>
#include <chrono>

using namespace hft;
using namespace itch50;

// The handler that bridges ITCH messages to our Limit Order Book
class OrderBookHandler {
public:
    Market market;
    uint64_t message_count = 0;

    // We only implement handle() for messages we care about reconstructing the book.
    // The CanHandle concept inside our dispatcher will ignore the rest.

    void handle(const AddOrder& msg) {
        market.on_add_order(
            msg.order_reference_number(),
            msg.stock_locate(),
            msg.buy_sell_indicator() == 'B' ? Side::BUY : Side::SELL,
            msg.shares(),
            msg.price()
        );
        message_count++;
    }

    void handle(const AddOrderMPIDAttribution& msg) {
        market.on_add_order(
            msg.order_reference_number(),
            msg.stock_locate(),
            msg.buy_sell_indicator() == 'B' ? Side::BUY : Side::SELL,
            msg.shares(),
            msg.price()
        );
        message_count++;
    }

    void handle(const OrderExecuted& msg) {
        market.on_order_executed(
            msg.order_reference_number(),
            msg.executed_shares()
        );
        message_count++;
    }

    void handle(const OrderExecutedWithPrice& msg) {
        // Even though it has a price, it executes against the existing order ID
        market.on_order_executed(
            msg.order_reference_number(),
            msg.executed_shares()
        );
        message_count++;
    }

    void handle(const OrderCancel& msg) {
        market.on_order_cancel(
            msg.order_reference_number(),
            msg.cancelled_shares()
        );
        message_count++;
    }

    void handle(const OrderDelete& msg) {
        market.on_order_delete(msg.order_reference_number());
        message_count++;
    }

    void handle(const OrderReplace& msg) {
        market.on_order_replace(
            msg.original_order_reference_number(),
            msg.new_order_reference_number(),
            msg.shares(),
            msg.price()
        );
        message_count++;
    }
};

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <itch_file.bin>\n";
        return 1;
    }

    const char* filename = argv[1];
    ItchFileReader reader;

    if (!reader.open(filename)) {
        std::cerr << "Error: Could not open " << filename << "\n";
        return 1;
    }

    std::cout << "Reconstructing Limit Order Book from " << filename << "...\n";

    OrderBookHandler handler;

    auto t0 = std::chrono::steady_clock::now();
    auto stats = reader.process_file(handler);
    auto t1 = std::chrono::steady_clock::now();

    double elapsed = std::chrono::duration<double>(t1 - t0).count();

    std::cout << "\nFinished in " << elapsed << " seconds.\n";
    std::cout << "Processed " << stats.messages_parsed << " ITCH messages (" 
              << handler.message_count << " order book updates).\n";
    std::cout << "Throughput: " << (static_cast<double>(handler.message_count) / elapsed / 1'000'000.0) 
              << " Million LOB updates / sec\n\n";

    // Just print the top of book for a few arbitrary stock locates to show it worked
    std::cout << "Sample Top of Book (Locate 1):\n";
    handler.market.get_book(1).print_top_of_book();

    std::cout << "Sample Top of Book (Locate 2):\n";
    handler.market.get_book(2).print_top_of_book();

    return 0;
}

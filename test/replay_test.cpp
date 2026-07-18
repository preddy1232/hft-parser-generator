#include "generated_itch_parser.hpp"
#include "itch_file_reader.hpp"
#include "market.hpp"
#include <gtest/gtest.h>

using namespace hft;
using namespace itch50;

namespace {

struct ReplayHandler {
    Market market;
    void handle(const AddOrder& msg) {
        market.on_add_order(msg.order_reference_number(), msg.stock_locate(),
            msg.buy_sell_indicator() == 'B' ? Side::BUY : Side::SELL,
            msg.shares(), msg.price());
    }
    void handle(const OrderExecuted& msg) {
        market.on_order_executed(msg.order_reference_number(),
                                 msg.executed_shares());
    }
    void handle(const OrderCancel& msg) {
        market.on_order_cancel(msg.order_reference_number(),
                               msg.cancelled_shares());
    }
    void handle(const OrderDelete& msg) {
        market.on_order_delete(msg.order_reference_number());
    }
    void handle(const OrderReplace& msg) {
        market.on_order_replace(msg.original_order_reference_number(),
            msg.new_order_reference_number(), msg.shares(), msg.price());
    }
};

} // namespace

TEST(ReplayTests, SyntheticFileReconstructsExpectedBook) {
    ItchFileReader reader;
    ASSERT_TRUE(reader.open(SYNTHETIC_ITCH_FIXTURE));

    ReplayHandler handler;
    const auto stats = reader.process_file(handler);
    const auto& book = handler.market.get_book(1);

    EXPECT_EQ(stats.messages_parsed, 6U);
    EXPECT_EQ(stats.unknown_messages, 0U);
    EXPECT_EQ(stats.bytes_processed, stats.total_file_bytes);
    EXPECT_EQ(handler.market.active_order_count(), 1U);
    EXPECT_EQ(book.bid_depth(), 0U);
    EXPECT_EQ(book.best_ask(), 1'005'000U);
    EXPECT_EQ(book.ask_size(), 40U);
}

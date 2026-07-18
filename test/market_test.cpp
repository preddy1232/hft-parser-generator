#include "market.hpp"
#include <gtest/gtest.h>
#include <algorithm>
#include <map>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

using namespace hft;

TEST(MarketTests, AggregatesOrdersAtTheSamePrice) {
    Market market;
    market.on_add_order(1, 42, Side::BUY, 100, 1'000'000);
    market.on_add_order(2, 42, Side::BUY, 250, 1'000'000);

    const auto& book = market.get_book(42);
    EXPECT_EQ(book.best_bid(), 1'000'000U);
    EXPECT_EQ(book.bid_size(), 350U);
    EXPECT_EQ(book.bid_depth(), 1U);
    EXPECT_EQ(market.active_order_count(), 2U);
}

TEST(MarketTests, MaintainsBestBidAndAskOrdering) {
    Market market;
    market.on_add_order(1, 7, Side::BUY, 10, 999'900);
    market.on_add_order(2, 7, Side::BUY, 20, 1'000'000);
    market.on_add_order(3, 7, Side::SELL, 30, 1'000'200);
    market.on_add_order(4, 7, Side::SELL, 40, 1'000'100);

    const auto& book = market.get_book(7);
    EXPECT_EQ(book.best_bid(), 1'000'000U);
    EXPECT_EQ(book.bid_size(), 20U);
    EXPECT_EQ(book.best_ask(), 1'000'100U);
    EXPECT_EQ(book.ask_size(), 40U);
    EXPECT_NEAR(book.spread(), 0.01, 1e-12);
}

TEST(MarketTests, PartialExecutionUpdatesQuantityWithoutRemovingOrder) {
    Market market;
    market.on_add_order(1, 3, Side::SELL, 100, 500'000);
    market.on_order_executed(1, 40);

    EXPECT_TRUE(market.contains_order(1));
    EXPECT_EQ(market.get_book(3).ask_size(), 60U);
    EXPECT_EQ(market.active_order_count(), 1U);
}

TEST(MarketTests, FullExecutionRemovesOrderAndPriceLevel) {
    Market market;
    market.on_add_order(1, 3, Side::SELL, 100, 500'000);
    market.on_order_executed(1, 100);

    EXPECT_FALSE(market.contains_order(1));
    EXPECT_EQ(market.get_book(3).ask_depth(), 0U);
    EXPECT_EQ(market.active_order_count(), 0U);
}

TEST(MarketTests, OversizedExecutionIsClampedToRemainingQuantity) {
    Market market;
    market.on_add_order(1, 3, Side::BUY, 100, 500'000);
    market.on_order_executed(1, 150);

    EXPECT_FALSE(market.contains_order(1));
    EXPECT_EQ(market.get_book(3).bid_depth(), 0U);
}

TEST(MarketTests, OversizedCancelIsClampedToRemainingQuantity) {
    Market market;
    market.on_add_order(1, 3, Side::BUY, 100, 500'000);
    market.on_order_cancel(1, 150);

    EXPECT_FALSE(market.contains_order(1));
    EXPECT_EQ(market.get_book(3).bid_depth(), 0U);
}

TEST(MarketTests, DuplicateOrderIdDoesNotCorruptBook) {
    Market market;
    market.on_add_order(1, 3, Side::BUY, 100, 500'000);
    market.on_add_order(1, 3, Side::BUY, 900, 600'000);

    const auto& book = market.get_book(3);
    EXPECT_EQ(market.active_order_count(), 1U);
    EXPECT_EQ(book.bid_depth(), 1U);
    EXPECT_EQ(book.best_bid(), 500'000U);
    EXPECT_EQ(book.bid_size(), 100U);
}

TEST(MarketTests, ZeroShareOrderIsIgnored) {
    Market market;
    market.on_add_order(1, 3, Side::BUY, 0, 500'000);

    EXPECT_FALSE(market.contains_order(1));
    EXPECT_EQ(market.active_order_count(), 0U);
    EXPECT_EQ(market.get_book(3).bid_depth(), 0U);
}

TEST(MarketTests, ReplacePreservesSideAndLocate) {
    Market market;
    market.on_add_order(1, 9, Side::SELL, 100, 1'000'200);
    market.on_order_replace(1, 2, 75, 1'000'100);

    const auto& book = market.get_book(9);
    EXPECT_FALSE(market.contains_order(1));
    EXPECT_TRUE(market.contains_order(2));
    EXPECT_EQ(book.ask_depth(), 1U);
    EXPECT_EQ(book.best_ask(), 1'000'100U);
    EXPECT_EQ(book.ask_size(), 75U);
}

TEST(MarketTests, UnknownOrderEventsAreNoOps) {
    Market market;
    market.on_order_executed(10, 5);
    market.on_order_cancel(10, 5);
    market.on_order_delete(10);
    market.on_order_replace(10, 11, 5, 100);

    EXPECT_EQ(market.active_order_count(), 0U);
    EXPECT_EQ(market.get_book(0).bid_depth(), 0U);
    EXPECT_EQ(market.get_book(0).ask_depth(), 0U);
}

namespace {

struct ReferenceOrder {
    uint16_t locate;
    Side side;
    uint32_t shares;
    uint32_t price;
};

class ReferenceMarket {
public:
    void add(uint64_t id, uint16_t locate, Side side, uint32_t shares,
             uint32_t price) {
        if (shares == 0 || orders.contains(id)) return;
        orders.emplace(id, ReferenceOrder{locate, side, shares, price});
        ladder(locate, side)[price] += shares;
    }

    void reduce(uint64_t id, uint32_t requested) {
        auto it = orders.find(id);
        if (it == orders.end()) return;
        auto& order = it->second;
        const uint32_t removed = std::min(requested, order.shares);
        auto& levels = ladder(order.locate, order.side);
        auto level = levels.find(order.price);
        level->second -= removed;
        if (level->second == 0) levels.erase(level);
        order.shares -= removed;
        if (order.shares == 0) orders.erase(it);
    }

    void erase(uint64_t id) {
        auto it = orders.find(id);
        if (it == orders.end()) return;
        const auto order = it->second;
        auto& levels = ladder(order.locate, order.side);
        auto level = levels.find(order.price);
        level->second -= order.shares;
        if (level->second == 0) levels.erase(level);
        orders.erase(it);
    }

    void replace(uint64_t old_id, uint64_t new_id, uint32_t shares,
                 uint32_t price) {
        auto it = orders.find(old_id);
        if (it == orders.end()) return;
        const auto old = it->second;
        erase(old_id);
        add(new_id, old.locate, old.side, shares, price);
    }

    [[nodiscard]] uint32_t best_bid(uint16_t locate) const {
        const auto& levels = bids[locate];
        return levels.empty() ? 0 : levels.rbegin()->first;
    }
    [[nodiscard]] uint64_t bid_size(uint16_t locate) const {
        const auto& levels = bids[locate];
        return levels.empty() ? 0 : levels.rbegin()->second;
    }
    [[nodiscard]] uint32_t best_ask(uint16_t locate) const {
        const auto& levels = asks[locate];
        return levels.empty() ? 0 : levels.begin()->first;
    }
    [[nodiscard]] uint64_t ask_size(uint16_t locate) const {
        const auto& levels = asks[locate];
        return levels.empty() ? 0 : levels.begin()->second;
    }

    std::unordered_map<uint64_t, ReferenceOrder> orders;
    std::map<uint32_t, uint64_t> bids[4];
    std::map<uint32_t, uint64_t> asks[4];

private:
    std::map<uint32_t, uint64_t>& ladder(uint16_t locate, Side side) {
        return side == Side::BUY ? bids[locate] : asks[locate];
    }
};

void expect_same_state(const Market& actual, const ReferenceMarket& expected,
                       uint32_t step) {
    SCOPED_TRACE("randomized event step " + std::to_string(step));
    EXPECT_EQ(actual.active_order_count(), expected.orders.size());
    for (uint16_t locate = 0; locate < 4; ++locate) {
        const auto& book = actual.get_book(locate);
        EXPECT_EQ(book.best_bid(), expected.best_bid(locate));
        EXPECT_EQ(book.bid_size(), expected.bid_size(locate));
        EXPECT_EQ(book.bid_depth(), expected.bids[locate].size());
        EXPECT_EQ(book.best_ask(), expected.best_ask(locate));
        EXPECT_EQ(book.ask_size(), expected.ask_size(locate));
        EXPECT_EQ(book.ask_depth(), expected.asks[locate].size());
    }
}

} // namespace

TEST(MarketTests, RandomizedDifferentialTestAgainstReferenceModel) {
    constexpr uint32_t event_count = 20'000;
    constexpr uint32_t seed = 0xC0FFEEU;
    std::mt19937 rng(seed);
    Market actual;
    ReferenceMarket expected;
    std::vector<uint64_t> known_ids;
    uint64_t next_id = 1;

    for (uint32_t step = 0; step < event_count; ++step) {
        const uint32_t action = rng() % 100;
        const bool can_modify = !known_ids.empty();
        const uint64_t selected = can_modify
            ? known_ids[rng() % known_ids.size()] : 0;

        if (!can_modify || action < 45) {
            const uint64_t id = next_id++;
            const uint16_t locate = static_cast<uint16_t>(rng() % 4);
            const Side side = (rng() & 1U) == 0 ? Side::BUY : Side::SELL;
            const uint32_t shares = 1 + rng() % 1'000;
            const uint32_t price = 900'000 + (rng() % 2'001) * 100;
            actual.on_add_order(id, locate, side, shares, price);
            expected.add(id, locate, side, shares, price);
            known_ids.push_back(id);
        } else if (action < 70) {
            const uint32_t shares = 1 + rng() % 1'200;
            actual.on_order_executed(selected, shares);
            expected.reduce(selected, shares);
        } else if (action < 85) {
            const uint32_t shares = 1 + rng() % 1'200;
            actual.on_order_cancel(selected, shares);
            expected.reduce(selected, shares);
        } else if (action < 95) {
            actual.on_order_delete(selected);
            expected.erase(selected);
        } else {
            const uint64_t new_id = next_id++;
            const uint32_t shares = 1 + rng() % 1'000;
            const uint32_t price = 900'000 + (rng() % 2'001) * 100;
            actual.on_order_replace(selected, new_id, shares, price);
            expected.replace(selected, new_id, shares, price);
            known_ids.push_back(new_id);
        }

        known_ids.erase(
            std::remove_if(known_ids.begin(), known_ids.end(),
                [&expected](uint64_t id) { return !expected.orders.contains(id); }),
            known_ids.end());
        expect_same_state(actual, expected, step);
    }
}

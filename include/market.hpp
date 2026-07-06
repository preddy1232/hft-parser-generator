#pragma once

#include <cstdint>
#include <unordered_map>
#include <map>
#include <vector>
#include <string_view>
#include <iostream>
#include <iomanip>
#include <algorithm>

namespace hft {

enum class Side { BUY, SELL };

struct Order {
    uint64_t id;
    uint16_t stock_locate;
    Side side;
    uint32_t shares;
    uint32_t price;
};

struct PriceLevel {
    uint32_t price = 0;
    uint64_t total_shares = 0;
    uint32_t order_count = 0;
};

// A single stock's Limit Order Book
class LimitOrderBook {
public:
    void add_order(uint32_t price, uint32_t shares, Side side) {
        if (side == Side::BUY) {
            auto& level = bids_[price];
            level.price = price;
            level.total_shares += shares;
            level.order_count++;
        } else {
            auto& level = asks_[price];
            level.price = price;
            level.total_shares += shares;
            level.order_count++;
        }
    }

    void remove_shares(uint32_t price, uint32_t shares, Side side) {
        if (side == Side::BUY) {
            auto it = bids_.find(price);
            if (it != bids_.end()) {
                it->second.total_shares -= shares;
                if (it->second.total_shares == 0) {
                    bids_.erase(it);
                }
            }
        } else {
            auto it = asks_.find(price);
            if (it != asks_.end()) {
                it->second.total_shares -= shares;
                if (it->second.total_shares == 0) {
                    asks_.erase(it);
                }
            }
        }
    }

    void remove_order(uint32_t price, uint32_t shares, Side side) {
        if (side == Side::BUY) {
            auto it = bids_.find(price);
            if (it != bids_.end()) {
                it->second.total_shares -= shares;
                it->second.order_count--;
                if (it->second.order_count == 0 || it->second.total_shares == 0) {
                    bids_.erase(it);
                }
            }
        } else {
            auto it = asks_.find(price);
            if (it != asks_.end()) {
                it->second.total_shares -= shares;
                it->second.order_count--;
                if (it->second.order_count == 0 || it->second.total_shares == 0) {
                    asks_.erase(it);
                }
            }
        }
    }

    void print_top_of_book() const {
        uint32_t best_bid = bids_.empty() ? 0 : bids_.begin()->first;
        uint32_t best_ask = asks_.empty() ? 0 : asks_.begin()->first;
        uint64_t bid_size = bids_.empty() ? 0 : bids_.begin()->second.total_shares;
        uint64_t ask_size = asks_.empty() ? 0 : asks_.begin()->second.total_shares;

        std::cout << "  Bid: " << bid_size << " @ " << (best_bid / 10000.0) 
                  << "  |  Ask: " << ask_size << " @ " << (best_ask / 10000.0) << "\n";
    }

private:
    // For V1, std::map is used for ordered traversal. 
    // In Phase 3, this will be replaced with a flat array for O(1) cache locality.
    std::map<uint32_t, PriceLevel, std::greater<uint32_t>> bids_;
    std::map<uint32_t, PriceLevel, std::less<uint32_t>> asks_;
};

// Represents the entire exchange market, containing an LOB for every stock
class Market {
public:
    Market() {
        // Preallocate to avoid reallocations on the hot path
        orders_.reserve(10'000'000); 
    }

    void on_add_order(uint64_t id, uint16_t locate, Side side, uint32_t shares, uint32_t price) {
        orders_.emplace(id, Order{id, locate, side, shares, price});
        books_[locate].add_order(price, shares, side);
    }

    void on_order_executed(uint64_t id, uint32_t executed_shares) {
        auto it = orders_.find(id);
        if (it != orders_.end()) {
            Order& o = it->second;
            books_[o.stock_locate].remove_shares(o.price, executed_shares, o.side);
            o.shares -= executed_shares;
            if (o.shares == 0) {
                books_[o.stock_locate].remove_order(o.price, 0, o.side); // cleans up order count
                orders_.erase(it);
            }
        }
    }

    void on_order_cancel(uint64_t id, uint32_t canceled_shares) {
        auto it = orders_.find(id);
        if (it != orders_.end()) {
            Order& o = it->second;
            books_[o.stock_locate].remove_shares(o.price, canceled_shares, o.side);
            o.shares -= canceled_shares;
            if (o.shares == 0) {
                books_[o.stock_locate].remove_order(o.price, 0, o.side);
                orders_.erase(it);
            }
        }
    }

    void on_order_delete(uint64_t id) {
        auto it = orders_.find(id);
        if (it != orders_.end()) {
            Order& o = it->second;
            books_[o.stock_locate].remove_order(o.price, o.shares, o.side);
            orders_.erase(it);
        }
    }

    void on_order_replace(uint64_t original_id, uint64_t new_id, uint32_t new_shares, uint32_t new_price) {
        auto it = orders_.find(original_id);
        if (it != orders_.end()) {
            Order o = it->second; // copy
            
            // Delete old
            books_[o.stock_locate].remove_order(o.price, o.shares, o.side);
            orders_.erase(it);

            // Add new
            on_add_order(new_id, o.stock_locate, o.side, new_shares, new_price);
        }
    }

    LimitOrderBook& get_book(uint16_t locate) {
        return books_[locate];
    }

private:
    // ITCH stock locates are 16-bit integers (1-65535). 
    // A flat array gives us O(1) direct memory access to any stock's order book.
    LimitOrderBook books_[65536];

    // Global hash map to lookup active orders by their reference number.
    // In a fully optimized HFT system, this might be a custom open-addressing hash table.
    std::unordered_map<uint64_t, Order> orders_;
};

} // namespace hft

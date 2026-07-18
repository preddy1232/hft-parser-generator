#pragma once
// ============================================================================
// market.hpp — High-Performance Limit Order Book Engine
// ============================================================================
//
// Phase 4: Replaced std::map (red-black tree) with a contiguous sorted vector
// (FlatPriceLadder) for the bid/ask price levels.
//
// Why std::map is bad for HFT order books:
//   - Each node is heap-allocated separately → terrible cache locality
//   - Tree traversal follows pointers to random heap locations → cache misses
//   - ~48 bytes overhead per node (3 pointers + color + padding)
//   - Allocator overhead on every insert/delete
//
// Why FlatPriceLadder is better:
//   - Contiguous memory: adjacent price levels sit in adjacent cache lines
//   - Hardware prefetcher can predict sequential access patterns
//   - O(1) top-of-book (best bid/ask always at index 0)
//   - O(log n) binary search for price lookup
//   - sizeof(PriceLevel) = 16 bytes vs ~64 bytes per std::map node
//   - Insert/remove is O(n) memmove, but n is typically 50–200 active
//     levels, so the ~1–3 KB memmove cost is negligible vs cache benefits
//
// ============================================================================

#include <cstdint>
#include <cstddef>
#include <type_traits>
#include <unordered_map>
#include <vector>
#include <iostream>

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

// Verify PriceLevel is safe for contiguous storage and memmove optimization.
// std::vector::insert/erase on trivially copyable types compiles to memmove.
static_assert(std::is_trivially_copyable_v<PriceLevel>,
    "PriceLevel must be trivially copyable for cache-friendly flat array storage");

// ============================================================================
// FlatPriceLadder — Cache-Friendly Sorted Price Level Array
// ============================================================================
// Template parameter SortDescending:
//   true  = bids  (highest price at index 0 — best bid first)
//   false = asks  (lowest price at index 0  — best ask first)
// ============================================================================
template<bool SortDescending>
class FlatPriceLadder {
public:
    void add(uint32_t price, uint32_t shares) {
        uint32_t idx = lower_bound(price);
        if (idx < size() && levels_[idx].price == price) {
            levels_[idx].total_shares += shares;
            levels_[idx].order_count++;
        } else {
            PriceLevel lvl{price, shares, 1};
            levels_.insert(levels_.begin() + static_cast<std::ptrdiff_t>(idx), lvl);
        }
    }

    void remove_shares(uint32_t price, uint32_t shares) {
        uint32_t idx = lower_bound(price);
        if (idx < size() && levels_[idx].price == price) {
            auto& lvl = levels_[idx];
            lvl.total_shares = (lvl.total_shares >= shares)
                ? lvl.total_shares - shares : 0;
            if (lvl.total_shares == 0) {
                levels_.erase(levels_.begin() + static_cast<std::ptrdiff_t>(idx));
            }
        }
    }

    void remove_order(uint32_t price, uint32_t shares) {
        uint32_t idx = lower_bound(price);
        if (idx < size() && levels_[idx].price == price) {
            auto& lvl = levels_[idx];
            lvl.total_shares = (lvl.total_shares >= shares)
                ? lvl.total_shares - shares : 0;
            if (lvl.order_count > 0) {
                lvl.order_count--;
            }
            if (lvl.order_count == 0 || lvl.total_shares == 0) {
                levels_.erase(levels_.begin() + static_cast<std::ptrdiff_t>(idx));
            }
        }
    }

    bool empty() const noexcept { return levels_.empty(); }

    uint32_t size() const noexcept {
        return static_cast<uint32_t>(levels_.size());
    }

    uint32_t best_price() const noexcept {
        return levels_.empty() ? 0 : levels_[0].price;
    }

    uint64_t best_size() const noexcept {
        return levels_.empty() ? 0 : levels_[0].total_shares;
    }

    // Pre-allocate capacity to avoid reallocation on the hot path.
    void reserve(uint32_t capacity) { levels_.reserve(capacity); }

    void clear() noexcept { levels_.clear(); }

private:
    std::vector<PriceLevel> levels_;

    // Binary search: finds the position where 'price' is, or where it
    // should be inserted to maintain sort order.
    uint32_t lower_bound(uint32_t price) const noexcept {
        uint32_t lo = 0;
        uint32_t hi = static_cast<uint32_t>(levels_.size());
        while (lo < hi) {
            uint32_t mid = lo + (hi - lo) / 2;
            bool in_order;
            if constexpr (SortDescending) {
                in_order = levels_[mid].price > price;
            } else {
                in_order = levels_[mid].price < price;
            }
            if (in_order) {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        return lo;
    }
};

// ============================================================================
// LimitOrderBook — Per-Stock Order Book
// ============================================================================
class LimitOrderBook {
public:
    void add_order(uint32_t price, uint32_t shares, Side side) {
        if (side == Side::BUY) {
            bids_.add(price, shares);
        } else {
            asks_.add(price, shares);
        }
    }

    void remove_shares(uint32_t price, uint32_t shares, Side side) {
        if (side == Side::BUY) {
            bids_.remove_shares(price, shares);
        } else {
            asks_.remove_shares(price, shares);
        }
    }

    void remove_order(uint32_t price, uint32_t shares, Side side) {
        if (side == Side::BUY) {
            bids_.remove_order(price, shares);
        } else {
            asks_.remove_order(price, shares);
        }
    }

    // ── Top-of-Book Accessors ──────────────────────────────────────────
    uint32_t best_bid() const noexcept { return bids_.best_price(); }
    uint32_t best_ask() const noexcept { return asks_.best_price(); }
    uint64_t bid_size() const noexcept { return bids_.best_size(); }
    uint64_t ask_size() const noexcept { return asks_.best_size(); }
    uint32_t bid_depth() const noexcept { return bids_.size(); }
    uint32_t ask_depth() const noexcept { return asks_.size(); }

    double spread() const noexcept {
        if (bids_.empty() || asks_.empty()) return 0.0;
        return static_cast<double>(asks_.best_price()) / 10000.0
             - static_cast<double>(bids_.best_price()) / 10000.0;
    }

    void print_top_of_book() const {
        uint32_t bb = best_bid();
        uint32_t ba = best_ask();
        uint64_t bs = bid_size();
        uint64_t as_val = ask_size();
        std::cout << "  Bid: " << bs << " @ "
                  << (static_cast<double>(bb) / 10000.0)
                  << "  |  Ask: " << as_val << " @ "
                  << (static_cast<double>(ba) / 10000.0) << "\n";
    }

private:
    FlatPriceLadder<true>  bids_;  // Descending: best bid (highest) at [0]
    FlatPriceLadder<false> asks_;  // Ascending:  best ask (lowest)  at [0]
};

// ============================================================================
// Market — Full Exchange State
// ============================================================================
// Represents the entire exchange: one LimitOrderBook per stock locate,
// plus a global hash map for O(1) order lookup by reference number.
// ============================================================================
class Market {
public:
    // Pre-allocate the order hash map to avoid rehashing on the hot path.
    // Call with the expected number of active orders (e.g., 10M for a full
    // trading day of NASDAQ TotalView-ITCH).
    void reserve_orders(std::size_t n) { orders_.reserve(n); }

    void on_add_order(uint64_t id, uint16_t locate, Side side,
                      uint32_t shares, uint32_t price) {
        if (shares == 0) {
            return;
        }
        const auto [it, inserted] =
            orders_.emplace(id, Order{id, locate, side, shares, price});
        (void)it;
        if (!inserted) {
            return;
        }
        books_[locate].add_order(price, shares, side);
    }

    void on_order_executed(uint64_t id, uint32_t executed_shares) {
        auto it = orders_.find(id);
        if (it != orders_.end()) {
            Order& o = it->second;
            const uint32_t removed =
                executed_shares < o.shares ? executed_shares : o.shares;
            books_[o.stock_locate].remove_shares(o.price, removed, o.side);
            o.shares -= removed;
            if (o.shares == 0) {
                books_[o.stock_locate].remove_order(o.price, 0, o.side);
                orders_.erase(it);
            }
        }
    }

    void on_order_cancel(uint64_t id, uint32_t canceled_shares) {
        auto it = orders_.find(id);
        if (it != orders_.end()) {
            Order& o = it->second;
            const uint32_t removed =
                canceled_shares < o.shares ? canceled_shares : o.shares;
            books_[o.stock_locate].remove_shares(o.price, removed, o.side);
            o.shares -= removed;
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

    void on_order_replace(uint64_t original_id, uint64_t new_id,
                          uint32_t new_shares, uint32_t new_price) {
        auto it = orders_.find(original_id);
        if (it != orders_.end()) {
            Order o = it->second; // copy before erase
            books_[o.stock_locate].remove_order(o.price, o.shares, o.side);
            orders_.erase(it);
            on_add_order(new_id, o.stock_locate, o.side, new_shares, new_price);
        }
    }

    LimitOrderBook& get_book(uint16_t locate) {
        return books_[locate];
    }

    [[nodiscard]] const LimitOrderBook& get_book(uint16_t locate) const {
        return books_[locate];
    }

    [[nodiscard]] std::size_t active_order_count() const noexcept {
        return orders_.size();
    }

    [[nodiscard]] bool contains_order(uint64_t id) const noexcept {
        return orders_.find(id) != orders_.end();
    }

private:
    // ITCH stock locates are 16-bit (1–65535).
    // Flat array gives O(1) direct access to any stock's order book.
    // Each empty LimitOrderBook is ~48 bytes (two empty vectors),
    // so the full array is ~3 MB — fits comfortably in memory.
    LimitOrderBook books_[65536];

    // O(1) order lookup by reference number.
    std::unordered_map<uint64_t, Order> orders_;
};

} // namespace hft

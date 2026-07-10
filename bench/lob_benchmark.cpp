// =============================================================================
// lob_benchmark.cpp — Google Benchmark Suite for Limit Order Book Engine
// =============================================================================
// Measures per-operation latency and throughput for the flat-array LOB.
// These numbers go directly on the resume.
//
// Benchmarks:
//   BM_LOB_AddOrder       — Insert into a pre-warmed 100-level book
//   BM_LOB_AddDeleteCycle — Add + delete round-trip
//   BM_LOB_ExecuteOrder   — Add + full fill execution
//   BM_LOB_ReplaceOrder   — Order replacement (delete old + insert new)
//   BM_LOB_MixedWorkload  — Realistic 40/30/15/10/5 message mix
//   BM_LOB_EndToEnd       — Full pipeline: parse ITCH stream → LOB update
// =============================================================================
#include "market.hpp"
#include "generated_itch_parser.hpp"
#include <benchmark/benchmark.h>
#include <cstdint>
#include <vector>

using namespace hft;
using namespace itch50;

// ── Helpers ─────────────────────────────────────────────────────────────────

// Seed a book with N price levels on each side.
// Bids:  base_price, base_price-tick, base_price-2*tick, ...
// Asks:  base_price+tick, base_price+2*tick, ...
static void seed_book(Market& market, uint16_t locate,
                      uint32_t levels_per_side, uint64_t& next_id) {
    constexpr uint32_t BASE_PRICE = 1000000;  // $100.00
    constexpr uint32_t TICK = 100;             // $0.01
    for (uint32_t i = 0; i < levels_per_side; ++i) {
        market.on_add_order(next_id++, locate, Side::BUY,
                            100, BASE_PRICE - i * TICK);
        market.on_add_order(next_id++, locate, Side::SELL,
                            100, BASE_PRICE + TICK + i * TICK);
    }
}

// ── Individual Operation Benchmarks ─────────────────────────────────────────

static void BM_LOB_AddOrder(benchmark::State& state) {
    Market market;
    uint64_t id = 1;
    seed_book(market, 1, 100, id);  // 100 levels each side

    uint64_t tick = 0;
    for (auto _ : state) {
        uint32_t price = 1000000 + static_cast<uint32_t>(tick % 200) * 100;
        Side side = (tick % 2 == 0) ? Side::BUY : Side::SELL;
        market.on_add_order(id, 1, side, 100, price);
        benchmark::DoNotOptimize(market.get_book(1).best_bid());
        market.on_order_delete(id);  // clean up to keep book stable
        id++;
        tick++;
    }
    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()));
}
BENCHMARK(BM_LOB_AddOrder);

static void BM_LOB_AddDeleteCycle(benchmark::State& state) {
    Market market;
    uint64_t id = 1;
    seed_book(market, 1, 100, id);

    for (auto _ : state) {
        market.on_add_order(id, 1, Side::BUY, 100, 990000);
        market.on_order_delete(id);
        benchmark::ClobberMemory();
        id++;
    }
    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * 2);
}
BENCHMARK(BM_LOB_AddDeleteCycle);

static void BM_LOB_ExecuteOrder(benchmark::State& state) {
    Market market;
    uint64_t id = 1;
    seed_book(market, 1, 100, id);

    for (auto _ : state) {
        market.on_add_order(id, 1, Side::BUY, 100, 990000);
        market.on_order_executed(id, 100);
        benchmark::ClobberMemory();
        id++;
    }
    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * 2);
}
BENCHMARK(BM_LOB_ExecuteOrder);

static void BM_LOB_ReplaceOrder(benchmark::State& state) {
    Market market;
    uint64_t id = 1;
    seed_book(market, 1, 100, id);

    // Insert a standing order to replace repeatedly
    market.on_add_order(id, 1, Side::BUY, 100, 990000);
    uint64_t standing = id++;

    for (auto _ : state) {
        market.on_order_replace(standing, id, 100, 990100);
        benchmark::DoNotOptimize(market.get_book(1).best_bid());
        standing = id;
        id++;
    }
    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()));
}
BENCHMARK(BM_LOB_ReplaceOrder);

// ── Mixed Workload ──────────────────────────────────────────────────────────
// Simulates realistic message distribution:
//   40% AddOrder, 30% Delete, 15% Execute, 10% Cancel, 5% Replace

static void BM_LOB_MixedWorkload(benchmark::State& state) {
    Market market;
    uint64_t id = 1;
    seed_book(market, 1, 50, id);

    // Pre-populate standing orders for modifications
    constexpr uint32_t STANDING = 10000;
    std::vector<uint64_t> active;
    active.reserve(STANDING);
    for (uint32_t i = 0; i < STANDING; ++i) {
        uint32_t price = 900000 + (i % 200) * 100;
        Side side = (i % 2 == 0) ? Side::BUY : Side::SELL;
        market.on_add_order(id, 1, side, 100, price);
        active.push_back(id);
        id++;
    }

    uint64_t op = 0;
    for (auto _ : state) {
        uint32_t action = static_cast<uint32_t>(op % 20);

        if (action < 8) {
            // 40% — Add
            uint32_t price = 900000 + static_cast<uint32_t>(op % 200) * 100;
            Side side = (op % 2 == 0) ? Side::BUY : Side::SELL;
            market.on_add_order(id, 1, side, 100, price);
            active.push_back(id);
            id++;
        } else if (action < 14 && !active.empty()) {
            // 30% — Delete
            auto idx = op % active.size();
            market.on_order_delete(active[idx]);
            active[idx] = active.back();
            active.pop_back();
        } else if (action < 17 && !active.empty()) {
            // 15% — Execute (partial)
            auto idx = op % active.size();
            market.on_order_executed(active[idx], 50);
        } else if (action < 19 && !active.empty()) {
            // 10% — Cancel (partial)
            auto idx = op % active.size();
            market.on_order_cancel(active[idx], 25);
        } else if (!active.empty()) {
            // 5% — Replace
            auto idx = op % active.size();
            uint32_t new_price = 900000 + static_cast<uint32_t>(op % 200) * 100;
            market.on_order_replace(active[idx], id, 100, new_price);
            active[idx] = id;
            id++;
        }
        benchmark::ClobberMemory();
        op++;
    }
    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()));
}
BENCHMARK(BM_LOB_MixedWorkload);

// ── End-to-End Pipeline ─────────────────────────────────────────────────────
// Full path: network buffer → ITCH parser → dispatch → LOB update.
// This is the number that matters most for the resume.

struct LOBBenchHandler {
    Market market;

    void handle(const AddOrder& msg) {
        market.on_add_order(
            msg.order_reference_number(),
            msg.stock_locate(),
            msg.buy_sell_indicator() == 'B' ? Side::BUY : Side::SELL,
            msg.shares(),
            msg.price()
        );
    }

    void handle(const OrderExecuted& msg) {
        market.on_order_executed(
            msg.order_reference_number(),
            msg.executed_shares()
        );
    }

    // Ignore all other message types
    template<typename T>
    void handle(const T&) {}
};

static void BM_LOB_EndToEnd(benchmark::State& state) {
    // Build a stream of 1000 AddOrder messages in ITCH length-prefixed format
    std::vector<uint8_t> stream;
    stream.reserve(1000 * 38);
    for (uint32_t i = 0; i < 1000; ++i) {
        // 2-byte big-endian length prefix
        stream.push_back(0);
        stream.push_back(36);

        // 36-byte AddOrder message
        uint8_t msg[36] = {};
        msg[0] = static_cast<uint8_t>('A');

        // stock_locate = 1 (big-endian)
        msg[2] = 1;

        // order_reference_number (big-endian)
        uint64_t ref = static_cast<uint64_t>(i) + 1;
        for (uint32_t j = 0; j < 8; ++j) {
            msg[11 + j] = static_cast<uint8_t>(
                (ref >> (56 - j * 8)) & 0xFFU);
        }

        // buy_sell_indicator
        msg[19] = static_cast<uint8_t>((i % 2 == 0) ? 'B' : 'S');

        // shares = 100 (big-endian)
        msg[23] = 100;

        // price = $100.00 + i*$0.01 (big-endian, 4 implied decimals)
        uint32_t price = 1000000 + i * 100;
        msg[32] = static_cast<uint8_t>((price >> 24) & 0xFFU);
        msg[33] = static_cast<uint8_t>((price >> 16) & 0xFFU);
        msg[34] = static_cast<uint8_t>((price >>  8) & 0xFFU);
        msg[35] = static_cast<uint8_t>( price        & 0xFFU);

        for (uint32_t j = 0; j < 36; ++j) {
            stream.push_back(msg[j]);
        }
    }

    LOBBenchHandler handler;

    for (auto _ : state) {
        parse_stream(handler,
                     reinterpret_cast<const char*>(stream.data()),
                     stream.size());
        benchmark::ClobberMemory();
    }
    state.SetBytesProcessed(
        static_cast<int64_t>(state.iterations())
        * static_cast<int64_t>(stream.size()));
    state.SetItemsProcessed(
        static_cast<int64_t>(state.iterations()) * 1000);
}
BENCHMARK(BM_LOB_EndToEnd);

BENCHMARK_MAIN();

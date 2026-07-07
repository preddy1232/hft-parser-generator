// =============================================================================
// benchmark.cpp — Google Benchmark for ITCH 5.0 Parser Latency Measurement
// =============================================================================
#include "generated_itch_parser.hpp"
#include <benchmark/benchmark.h>
#include <cstdint>
#include <cstring>
#include <vector>

using namespace itch50;

// A simple do-nothing handler for benchmarking maximum throughput
struct BenchHandler {
    uint64_t last_ref = 0;
    uint32_t last_price = 0;

    void handle(const AddOrder& msg) {
        benchmark::DoNotOptimize(last_ref = msg.order_reference_number());
        benchmark::DoNotOptimize(last_price = msg.price());
    }

    void handle(const OrderExecuted& msg) {
        benchmark::DoNotOptimize(last_ref = msg.order_reference_number());
    }
};

static void BM_ParseAddOrderDirect(benchmark::State& state) {
    uint8_t buf[36] = {0};
    buf[0] = 'A';
    
    // Setup a dummy buffer
    for(int i=0; i<36; ++i) buf[i] = static_cast<uint8_t>(i);

    BenchHandler handler;
    auto* msg = reinterpret_cast<const AddOrder*>(buf);

    for (auto _ : state) {
        handler.handle(*msg);
        benchmark::ClobberMemory();
    }
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(sizeof(AddOrder)));
}
BENCHMARK(BM_ParseAddOrderDirect);

static void BM_ParseStaticDispatch(benchmark::State& state) {
    uint8_t buf[36] = {0};
    buf[0] = 'A';

    BenchHandler handler;

    for (auto _ : state) {
        dispatch_message(handler, 'A', reinterpret_cast<const char*>(buf));
        benchmark::ClobberMemory();
    }
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(sizeof(AddOrder)));
}
BENCHMARK(BM_ParseStaticDispatch);

static void BM_ParseDispatchTable(benchmark::State& state) {
    uint8_t buf[36] = {0};
    buf[0] = 'A';

    BenchHandler handler;
    DispatchTable<BenchHandler> table;

    for (auto _ : state) {
        table.dispatch(handler, 'A', reinterpret_cast<const char*>(buf));
        benchmark::ClobberMemory();
    }
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(sizeof(AddOrder)));
}
BENCHMARK(BM_ParseDispatchTable);

static void BM_ParseStream(benchmark::State& state) {
    // Construct a stream of 1000 AddOrder messages
    std::vector<uint8_t> stream;
    stream.reserve(1000 * 38);
    for(int i=0; i<1000; ++i) {
        stream.push_back(0);
        stream.push_back(36); // length
        stream.push_back('A');
        for(int j=1; j<36; ++j) stream.push_back(0); // dummy data
    }

    BenchHandler handler;

    for (auto _ : state) {
        parse_stream(handler, reinterpret_cast<const char*>(stream.data()), stream.size());
        benchmark::ClobberMemory();
    }
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(stream.size()));
}
BENCHMARK(BM_ParseStream);

BENCHMARK_MAIN();

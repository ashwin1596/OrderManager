#include "../include/SPSCQueue.h"
#include "../include/Orderbook.h"
#include "../include/Command.h"

#include <atomic>
#include <thread>
#include <vector>
#include <cassert>
#include <iostream>
#include <chrono>
#include <algorithm>
#include <cstdint>

#if defined(__linux__)
#include <pthread.h>
#endif

// ===========================================================================
// Shared helpers
// ===========================================================================

inline std::uint64_t now_ns() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

// Best-effort core pinning (stage 5). No-op / warning if unavailable (e.g. WSL
// scheduling quirks). Affinity reduces jitter by preventing thread migration,
// which would cold the cache — matters for latency, not correctness.
void pin_to_core(std::thread& t, int core) {
#if defined(__linux__)
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(core, &set);
    int rc = pthread_setaffinity_np(t.native_handle(), sizeof(set), &set);
    if (rc != 0) {
        std::cerr << "warning: pin to core " << core << " failed (rc=" << rc << ")\n";
    }
#else
    (void)t; (void)core;
#endif
}

// ===========================================================================
// Stage loops (correctness path)
//
// Shutdown protocol (doc 9.6, 9.10):
//   - inbound has an IN-BAND terminator (the Shutdown poison pill, always pushed
//     last by the feed). Matching breaks ONLY on the pill -> no flag, no gap.
//   - outbound has NO in-band terminator (fills carry no "last" marker). Reporting
//     uses the matching_done flag + DRAIN-BEFORE-BREAK to close the check-empty/
//     check-done gap. feed_done was removed: redundant once matching uses the pill.
// ===========================================================================

template <std::size_t N>
void feed_loop(SPSCQueue<Command, N>& inbound,
               const std::vector<Command>& workload) {
    for (const auto& cmd : workload) {
        while (!inbound.push(cmd)) { /* _mm_pause(); */ }      // backpressure: spin if full
    }
    // Poison pill — in-band terminator, rides the queue in FIFO order behind all work.
    while (!inbound.push(Command{Command::Shutdown, 0, 0, 0, Side::Bid})) { /* _mm_pause(); */ }
}

template <std::size_t N, std::size_t M>
void matching_loop(SPSCQueue<Command, N>& inbound,
                   SPSCQueue<Fill, M>& outbound,
                   std::atomic<bool>& matching_done) {
    Orderbook book(1'000'000);
    auto on_fill = [&outbound](const Fill& f) {
        while (!outbound.push(f)) { /* _mm_pause(); */ }       // never drop a fill
    };

    Command cmd;
    for (;;) {
        if (inbound.pop(cmd)) {
            if (cmd.type == Command::Shutdown) break;          // in-band terminator: authoritative exit
            switch (cmd.type) {
                case Command::Add:    book.add(cmd.id, cmd.price, cmd.qty, cmd.side, on_fill); break;
                case Command::Cancel: book.cancel(cmd.id); break;
                case Command::Modify: book.modify(cmd.id, cmd.price, cmd.qty, on_fill); break;
                case Command::Shutdown: break;
            }
        } else {
            /* _mm_pause(); */                                 // empty — spin; the pill will arrive
        }
    }
    matching_done.store(true, std::memory_order_release);      // signal reporting (release after all pushes)
}

template <std::size_t M>
void report_loop(SPSCQueue<Fill, M>& outbound,
                 std::atomic<bool>& matching_done,
                 std::atomic<std::size_t>& fill_count,
                 std::atomic<std::uint64_t>& qty_sum) {
    Fill f{0, 0, 0, 0};
    for (;;) {
        if (outbound.pop(f)) {
            fill_count.fetch_add(1, std::memory_order_relaxed);
            qty_sum.fetch_add(f.qty, std::memory_order_relaxed);
        } else if (matching_done.load(std::memory_order_acquire)) {
            // Drain-before-break: a fill may have landed between our last pop() and
            // observing the flag. matching_done was set release-AFTER all pushes, so
            // this acquire guarantees every push is visible to the final drain.
            while (outbound.pop(f)) {
                fill_count.fetch_add(1, std::memory_order_relaxed);
                qty_sum.fetch_add(f.qty, std::memory_order_relaxed);
            }
            break;
        } else {
            /* _mm_pause(); */
        }
    }
}

template <std::size_t N, std::size_t M>
std::pair<std::size_t, std::uint64_t>
run_pipeline(const std::vector<Command>& workload) {
    SPSCQueue<Command, N> inbound;
    SPSCQueue<Fill, M>    outbound;
    std::atomic<bool>          matching_done{false};
    std::atomic<std::size_t>   fill_count{0};
    std::atomic<std::uint64_t> qty_sum{0};

    // std::ref required: std::thread copies args; queues/atomics are non-copyable.
    std::thread feed(feed_loop<N>, std::ref(inbound), std::cref(workload));
    std::thread match(matching_loop<N, M>, std::ref(inbound), std::ref(outbound),
                      std::ref(matching_done));
    std::thread report(report_loop<M>, std::ref(outbound), std::ref(matching_done),
                       std::ref(fill_count), std::ref(qty_sum));

    feed.join();      // pushes all work + pill, returns
    match.join();     // drains inbound, exits on pill, sets matching_done
    report.join();    // drains outbound, exits
    return {fill_count.load(), qty_sum.load()};
}

// ===========================================================================
// Stage 3 tests — basic threaded correctness
// ===========================================================================

void test_threaded_basic_fill() {
    std::vector<Command> workload = {
        {Command::Add, 1, 100, 10, Side::Ask},
        {Command::Add, 2, 100,  4, Side::Bid},
    };
    auto [count, qty] = run_pipeline<4096, 4096>(workload);
    assert(count == 1); assert(qty == 4);
    std::cout << "test_threaded_basic_fill PASSED\n";
}

void test_threaded_many_fills() {
    std::vector<Command> workload;
    for (OrderId i = 1; i <= 1000; ++i) workload.push_back({Command::Add, i, 100, 1, Side::Ask});
    workload.push_back({Command::Add, 1001, 100, 1000, Side::Bid});
    auto [count, qty] = run_pipeline<4096, 4096>(workload);
    assert(count == 1000); assert(qty == 1000);
    std::cout << "test_threaded_many_fills PASSED\n";
}

void test_threaded_backpressure() {
    std::vector<Command> workload;
    const OrderId pairs = 5000;
    OrderId id = 1;
    for (OrderId i = 0; i < pairs; ++i) {
        workload.push_back({Command::Add, id++, 100, 1, Side::Ask});
        workload.push_back({Command::Add, id++, 100, 1, Side::Bid});
    }
    auto [count, qty] = run_pipeline<1024, 1024>(workload);   // small queues -> heavy backpressure
    assert(count == pairs); assert(qty == pairs);
    std::cout << "test_threaded_backpressure PASSED\n";
}

// ===========================================================================
// Stage 4 tests — shutdown / draining edge cases
// ===========================================================================

// Empty workload: only the pill flows. Pipeline must shut down cleanly, zero fills.
void test_stage4_empty_workload() {
    std::vector<Command> workload;   // nothing
    auto [count, qty] = run_pipeline<4096, 4096>(workload);
    assert(count == 0); assert(qty == 0);
    std::cout << "test_stage4_empty_workload PASSED\n";
}

// All orders rest, none cross: zero fills, but commands must all be processed and
// the pipeline must drain and stop (exercises "done with no outbound traffic").
void test_stage4_all_rest_no_fills() {
    std::vector<Command> workload;
    for (OrderId i = 1; i <= 500; ++i)
        workload.push_back({Command::Add, i, 100 + i, 1, Side::Bid});  // distinct prices, same side
    auto [count, qty] = run_pipeline<4096, 4096>(workload);
    assert(count == 0); assert(qty == 0);
    std::cout << "test_stage4_all_rest_no_fills PASSED\n";
}

// Adds then cancels of everything: zero fills, order_index churns, clean shutdown.
void test_stage4_all_cancels() {
    std::vector<Command> workload;
    for (OrderId i = 1; i <= 300; ++i) workload.push_back({Command::Add, i, 100, 1, Side::Ask});
    for (OrderId i = 1; i <= 300; ++i) workload.push_back({Command::Cancel, i, 0, 0, Side::Ask});
    auto [count, qty] = run_pipeline<4096, 4096>(workload);
    assert(count == 0); assert(qty == 0);
    std::cout << "test_stage4_all_cancels PASSED\n";
}

// All four command paths through the threaded pipeline, incl. modify fast-path.
void test_stage4_mixed_command_types() {
    std::vector<Command> workload = {
        {Command::Add,    1, 100, 10, Side::Ask},   // rest ask 10 @ 100
        {Command::Add,    2,  99,  5, Side::Bid},   // rest bid 5 @ 99 (no cross)
        {Command::Cancel, 2,   0,  0, Side::Bid},   // cancel the bid
        {Command::Modify, 1, 100,  6, Side::Ask},   // fast-path qty decrease 10 -> 6 (no fill)
        {Command::Add,    3, 100,  6, Side::Bid},   // crossing bid -> 1 fill, qty 6 (consumes id 1)
    };
    auto [count, qty] = run_pipeline<4096, 4096>(workload);
    assert(count == 1); assert(qty == 6);
    std::cout << "test_stage4_mixed_command_types PASSED\n";
}

// Burst of fills into a TINY outbound queue, so matching's on_fill spins on a full
// outbound right up to shutdown. Stresses the drain-before-break path: the last
// fills are almost certainly in-queue when matching_done flips.
void test_stage4_shutdown_under_outbound_pressure() {
    std::vector<Command> workload;
    const OrderId pairs = 2000;
    OrderId id = 1;
    for (OrderId i = 0; i < pairs; ++i) {
        workload.push_back({Command::Add, id++, 100, 1, Side::Ask});
        workload.push_back({Command::Add, id++, 100, 1, Side::Bid});  // each -> 1 fill
    }
    // inbound roomy, outbound TINY (16) -> matching constantly waits on outbound,
    // and at shutdown the tail of fills is still in-flight.
    auto [count, qty] = run_pipeline<8192, 16>(workload);
    assert(count == pairs); assert(qty == pairs);
    std::cout << "test_stage4_shutdown_under_outbound_pressure PASSED\n";
}

// ===========================================================================
// Stage 5 — tick-to-trade latency
//
// Measures the time from a command being pushed by the feed to its resulting fill
// being popped by reporting. Timestamping is split by owner thread (no shared
// writes): feed writes send_ts[id]; reporting writes recv_ts[aggressor_id] (first
// fill per aggressor). Both read by main AFTER join (the join is the sync point).
//
// PACING: the feed busy-waits between pushes so the pipeline stays unloaded.
// Without pacing, dumping the whole workload at once makes later commands queue
// behind earlier ones, and we'd measure QUEUEING delay, not per-order tick-to-trade.
// ===========================================================================

template <std::size_t N>
void feed_loop_latency(SPSCQueue<Command, N>& inbound,
                       const std::vector<Command>& workload,
                       std::vector<std::uint64_t>& send_ts,
                       std::uint64_t pace_ns) {
    for (const auto& cmd : workload) {
        std::uint64_t target = now_ns() + pace_ns;            // pace: keep pipeline shallow
        while (now_ns() < target) { /* spin */ }
        send_ts[cmd.id] = now_ns();
        while (!inbound.push(cmd)) { /* _mm_pause(); */ }
    }
    while (!inbound.push(Command{Command::Shutdown, 0, 0, 0, Side::Bid})) { /* _mm_pause(); */ }
}

template <std::size_t M>
void report_loop_latency(SPSCQueue<Fill, M>& outbound,
                         std::atomic<bool>& matching_done,
                         std::vector<std::uint64_t>& recv_ts) {
    Fill f{0, 0, 0, 0};
    auto record = [&](const Fill& fl) {
        if (recv_ts[fl.aggressor_id] == 0) recv_ts[fl.aggressor_id] = now_ns();  // first fill per aggressor
    };
    for (;;) {
        if (outbound.pop(f)) {
            record(f);
        } else if (matching_done.load(std::memory_order_acquire)) {
            while (outbound.pop(f)) record(f);   // drain-before-break (same protocol)
            break;
        } else {
            /* _mm_pause(); */
        }
    }
}

void run_latency_benchmark() {
    constexpr std::size_t N = 8192, M = 8192;
    const OrderId pairs = 50'000;                 // 50k crossing bids = 50k measured tick-to-trades
    const std::uint64_t pace_ns = 500;            // ~0.5us between sends -> pipeline stays unloaded

    std::vector<Command> workload;
    workload.reserve(pairs * 2);
    OrderId id = 1;
    for (OrderId i = 0; i < pairs; ++i) {
        workload.push_back({Command::Add, id++, 100, 1, Side::Ask});  // rests
        workload.push_back({Command::Add, id++, 100, 1, Side::Bid});  // crosses -> fill, aggressor = this id
    }
    const std::size_t max_id = id;

    std::vector<std::uint64_t> send_ts(max_id + 1, 0);
    std::vector<std::uint64_t> recv_ts(max_id + 1, 0);

    SPSCQueue<Command, N> inbound;
    SPSCQueue<Fill, M>    outbound;
    std::atomic<bool>     matching_done{false};

    std::thread feed(feed_loop_latency<N>, std::ref(inbound), std::cref(workload),
                     std::ref(send_ts), pace_ns);
    std::thread match(matching_loop<N, M>, std::ref(inbound), std::ref(outbound),
                      std::ref(matching_done));
    std::thread report(report_loop_latency<M>, std::ref(outbound), std::ref(matching_done),
                       std::ref(recv_ts));

    pin_to_core(feed, 1);     // best-effort; core 0 left for the OS
    pin_to_core(match, 2);
    pin_to_core(report, 3);

    feed.join(); match.join(); report.join();

    std::vector<double> lat;
    lat.reserve(pairs);
    for (std::size_t i = 1; i <= max_id; ++i) {
        if (send_ts[i] != 0 && recv_ts[i] != 0 && recv_ts[i] >= send_ts[i]) {
            lat.push_back(static_cast<double>(recv_ts[i] - send_ts[i]));
        }
    }
    std::sort(lat.begin(), lat.end());

    auto pct = [&](double p) { return lat[static_cast<std::size_t>(p * (lat.size() - 1))]; };
    std::printf("\n=== Tick-to-trade latency (ns), n=%zu, pace=%lluns ===\n",
                lat.size(), (unsigned long long)pace_ns);
    std::printf("  min   : %8.0f\n", lat.front());
    std::printf("  p50   : %8.0f\n", pct(0.50));
    std::printf("  p90   : %8.0f\n", pct(0.90));
    std::printf("  p99   : %8.0f\n", pct(0.99));
    std::printf("  p99.9 : %8.0f\n", pct(0.999));
    std::printf("  max   : %8.0f\n", lat.back());
    std::printf("NOTE: includes ~2x now_ns() (~14ns each) in the measured path; WSL\n"
                "scheduling adds jitter (esp. tail). Treat as illustrative, not absolute.\n");
}

// ===========================================================================
int main() {
    for (int rep = 0; rep < 200; ++rep) {
        test_threaded_basic_fill();
        test_threaded_many_fills();
        test_threaded_backpressure();
        test_stage4_empty_workload();
        test_stage4_all_rest_no_fills();
        test_stage4_all_cancels();
        test_stage4_mixed_command_types();
        test_stage4_shutdown_under_outbound_pressure();
    }
    std::cout << "ALL CORRECTNESS TESTS PASSED (200 reps)\n";

    run_latency_benchmark();   // run once; NOT under TSan/ASan (they distort timing)
    return 0;
}
# OrderManager

A limit order book and lock-free trading pipeline written from scratch in modern C++ (C++17), built to study low-latency systems engineering: cache-aware data structures, the C++ memory model, lock-free concurrency, and measurement-driven optimization.

> Built to go deep on the low-latency C++ that real trading systems rely on: not just to make it work, but to measure it, profile it, and understand *why* it is built the way it is.

Every component is tested under AddressSanitizer / UndefinedBehaviorSanitizer (and ThreadSanitizer for the concurrent code), benchmarked, and profiled. Optimizations are justified by measurement, not intuition, and where an optimization was the wrong call it was deliberately *not* built and the reasoning documented.

## What's here

- **Limit order book** with price-time priority matching: add, cancel, modify (with correct priority semantics), partial fills, multi-level sweeps, and an allocation-free fill-reporting callback. `std::map` for price levels, an intrusive doubly-linked list per level, and an `unordered_map` order index for O(1) lookup.
- **Object pool** — a fixed-capacity, O(1) free-list allocator (intrusive free list, placement new, allocation/construction separation) that eliminates heap traffic on the hot path.
- **Lock-free SPSC queue** — single-producer / single-consumer ring buffer using `std::atomic` indices with release/acquire ordering, power-of-two capacity with bitmask indexing, and cache-line padding to avoid false sharing.
- **Threaded pipeline** — a three-stage feed → match → report pipeline connecting the components with SPSC queues, so each stage is single-threaded and lock-free internally and concurrency is confined to the queue handoffs.

## Engineering highlights

- **Measurement-driven optimization.** Profiled a `std::map` + `std::list` baseline, then replaced the per-order `std::list` allocation with an intrusive list over the object pool:

  | Operation | Baseline (`std::map` + `std::list`) | Pooled intrusive list | Change |
  |---|---|---|---|
  | Blended | ~73 ns/op | ~62 ns/op | **−15%** |
  | add | ~78 ns/op | ~60 ns/op | **−23%** |
  | cancel | ~59 ns/op | ~46 ns/op | **−22%** |

  Cachegrind revealed a counterintuitive result documented in full: cache misses actually *rose* while wall-clock time *fell*, because a pool gives contiguity of storage, not locality of access, and the allocation win dominated.
- **Knowing when not to optimize.** A flat-array price index was designed and analyzed but deliberately not built, because it is a specialization (single hot instrument, tight known price range) whose memory and operational tradeoffs make it the wrong default for a general book. The tradeoff analysis is documented.
- **Lock-free concurrency under test.** The pipeline surfaced the canonical concurrent-queue shutdown bug (a protocol race that drops work under a rare interleaving, invisible to ThreadSanitizer because the memory accesses are correctly synchronized). It was caught by high-iteration stress testing, fixed with a drain-before-break protocol and an in-band poison-pill terminator, and verified clean over 1,000 iterations under TSan.
- **End-to-end latency.** Tick-to-trade latency measured at ~400 ns median through two lock-free queues plus a match. The millisecond tail is diagnosed as OS scheduler preemption of busy-spin threads on non-isolated cores (the reason production low-latency systems isolate cores and bypass the kernel), not the pipeline itself.

## Layout

```
OrderManager/
  include/
    Orderbook.h     # limit order book (matching, cancel, modify, queries)
    ObjectPool.h    # fixed-capacity O(1) free-list allocator
    SPSCQueue.h     # lock-free single-producer/single-consumer ring buffer
    Command.h       # POD command type carried across the pipeline
  src/
    pipeline.cpp            # single-threaded matching-loop test
    pipeline_threaded.cpp   # full threaded pipeline + tests + latency benchmark
  tests/
    orderbook.cpp   # order book test suite
  profiling/
    benchmark.cpp   # order book benchmark harness
```

## Building and running

Requires a C++17 compiler (tested with g++) and a POSIX threads environment.

Order book tests (under sanitizers):
```bash
cd tests
g++ -std=c++17 -Wall -Wextra -fsanitize=address,undefined orderbook.cpp -o test_orderbook && ./test_orderbook
```

Order book benchmark (optimized, no sanitizers):
```bash
cd profiling
g++ -std=c++17 -O2 -DNDEBUG benchmark.cpp -o bench && ./bench
```

Threaded pipeline — correctness (under ThreadSanitizer):
```bash
cd src
g++ -std=c++17 -Wall -Wextra -fsanitize=thread -pthread pipeline_threaded.cpp -o pt_tsan && ./pt_tsan
```

Threaded pipeline — latency (optimized, no sanitizers; sanitizer builds distort timing):
```bash
cd src
g++ -std=c++17 -O2 -pthread pipeline_threaded.cpp -o pt && ./pt
```

## Notes on scope

This is a learning and interview-preparation project, not a production trading system. The goal was to build each component to production-quality standards (correctness under sanitizers, benchmarking, profiling, honest tradeoff analysis) and to understand *why* low-latency systems are built the way they are. Numbers are from a development machine (WSL2) and are illustrative; the methodology and relative results are the point, not absolute figures.

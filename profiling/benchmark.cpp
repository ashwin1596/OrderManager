#include "../include/Orderbook.h"
#include <chrono>
#include <vector>
#include <random>
#include <cstdio>

// Force the compiler to not optimize away a value (Google Benchmark's trick, simplified).
template <typename T>
inline void do_not_optimize(T const& value) {
    asm volatile("" : : "r,m"(value) : "memory");
}

// One operation in the precomputed workload.
struct Op {
    enum Kind { Add, Cancel } kind;
    OrderId id;
    Price   price;
    Quantity qty;
    Side    side;
};

std::vector<Op> generate_workload(std::size_t n) {
    std::vector<Op> ops;
    ops.reserve(n);
    std::mt19937_64 rng(42);                       // fixed seed → reproducible
    // mid ~ 10000 ticks; prices cluster within +/- 50 ticks of mid
    std::normal_distribution<double> price_dist(10000.0, 20.0);
    std::uniform_int_distribution<int> coin(0, 99);
    std::vector<OrderId> live;                     // track live order ids to cancel real ones
    OrderId next_id = 1;

    for (std::size_t i = 0; i < n; ++i) {
        int roll = coin(rng);
        if (roll < 60 || live.empty()) {           // 60% adds (and forced add if nothing to cancel)
            Price p = static_cast<Price>(std::max(1.0, price_dist(rng)));
            Side s = (coin(rng) < 50) ? Side::Bid : Side::Ask;
            Quantity q = 1 + (rng() % 100);
            ops.push_back({Op::Add, next_id, p, q, s});
            live.push_back(next_id);
            ++next_id;
        } else {                                   // 40% cancels of a random live order
            std::size_t idx = rng() % live.size();
            ops.push_back({Op::Cancel, live[idx], 0, 0, Side::Bid});
            live[idx] = live.back();
            live.pop_back();
        }
    }
    return ops;
}

double run_once(const std::vector<Op>& ops) {
    Orderbook book;
    auto noop = [](const Fill&){};
    auto t0 = std::chrono::steady_clock::now();
    for (const auto& op : ops) {
        if (op.kind == Op::Add) {
            book.add(op.id, op.price, op.qty, op.side, noop);
        } else {
            bool r = book.cancel(op.id);
            do_not_optimize(r);
        }
    }
    auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::nano>(t1 - t0).count();
}

struct Timing {
    double add_ns = 0;     std::size_t add_n = 0;
    double cancel_ns = 0;  std::size_t cancel_n = 0;
};

Timing run_split(const std::vector<Op>& ops) {
    Orderbook book;
    auto noop = [](const Fill&){};
    Timing t;
    for (const auto& op : ops) {
        auto a = std::chrono::steady_clock::now();
        if (op.kind == Op::Add) {
            book.add(op.id, op.price, op.qty, op.side, noop);
        } else {
            bool r = book.cancel(op.id);
            do_not_optimize(r);
        }
        auto b = std::chrono::steady_clock::now();
        double ns = std::chrono::duration<double, std::nano>(b - a).count();
        if (op.kind == Op::Add) { t.add_ns += ns; ++t.add_n; }
        else                    { t.cancel_ns += ns; ++t.cancel_n; }
    }
    return t;
}

// Measure clock-read overhead so we can report it and confirm it's small.
double clock_overhead_ns() {
    const int N = 1'000'000;
    double best = 1e18;
    for (int rep = 0; rep < 5; ++rep) {
        auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < N; ++i) {
            auto a = std::chrono::steady_clock::now();
            do_not_optimize(a);
        }
        auto t1 = std::chrono::steady_clock::now();
        best = std::min(best, std::chrono::duration<double, std::nano>(t1 - t0).count() / N);
    }
    return best;   // ns per clock read
}

int main() {
    const std::size_t N = 2'000'000;
    auto ops = generate_workload(N);

    run_once(ops);                                 // warmup (untimed)

    double best = 1e18;
    for (int rep = 0; rep < 7; ++rep) {
        double ns = run_once(ops);
        best = std::min(best, ns);
    }
    std::printf("ops=%zu  total=%.2f ms  per_op=%.2f ns\n",
                N, best / 1e6, best / N);

    double oh = clock_overhead_ns();
    std::printf("clock overhead ~ %.2f ns/read (x2 per op)\n", oh);
    // ... run run_split several times, keep the best add_ns/add_n and cancel_ns/cancel_n ...
    double best_add_per = 1e18;
    double best_cancel_per = 1e18;
    std::size_t add_n = 0;
    std::size_t cancel_n = 0;
    for (int rep = 0; rep < 7; ++rep) {
        Timing t = run_split(ops);
        best_add_per = std::min(best_add_per, t.add_ns / t.add_n);
        best_cancel_per = std::min(best_cancel_per, t.cancel_ns / t.cancel_n);
        add_n = t.add_n;
        cancel_n = t.cancel_n;
    }
    std::printf("add:    %.2f ns/op  (n=%zu)\n", best_add_per, add_n);
    std::printf("cancel: %.2f ns/op  (n=%zu)\n", best_cancel_per, cancel_n);

    return 0;
}
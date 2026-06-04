#pragma once

#include <atomic>
#include <array>
#include <cstddef>

// Single-Producer Single-Consumer lock-free queue.
// CONTRACT: exactly one thread calls push() (the producer), exactly one
// thread calls pop() (the consumer). Violating this (2 producers, etc.)
// is undefined behavior — this design is NOT MPMC-safe.
template <typename T, std::size_t Capacity>
class SPSCQueue {
public:
    static_assert(Capacity > 0, "Capacity must be greater than 0");
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of 2");

    bool push(T&& item){
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        const std::size_t head = head_.load(std::memory_order_acquire);

        if (tail - head == Capacity)
            return false;

        buffer_[tail & (Capacity-1)] = std::move(item);

        tail_.store(tail+1, std::memory_order_release);
        return true;
    }

    // Called ONLY by the producer thread.
    bool push(const T& item) {
        // We own tail_ exclusively (only the producer writes it), so a relaxed
        // load is enough to read our own value — no other thread writes tail_.
        const std::size_t tail = tail_.load(std::memory_order_relaxed);

        // We read head_ (written by the consumer) to check for space.
        // ACQUIRE: pairs with the consumer's release-store of head_. This makes
        // the consumer's slot-read complete-before we reuse the slot, so we don't
        // overwrite a slot the consumer hasn't finished reading.
        const std::size_t head = head_.load(std::memory_order_acquire);

        if (tail - head == Capacity) {
            return false;                 // full
        }

        // (A) Write the data into the slot. Non-atomic; this is the payload.
        buffer_[tail & (Capacity - 1)] = item;

        // (B) Publish the new tail. RELEASE: guarantees the slot write (A) is
        // visible to any consumer that ACQUIRE-loads this same tail_ and sees
        // this value. This is the producer→consumer data handoff.
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    // Called ONLY by the consumer thread.
    bool pop(T& out) {
        // We own head_ exclusively (only the consumer writes it) → relaxed.
        const std::size_t head = head_.load(std::memory_order_relaxed);

        // We read tail_ (written by the producer) to check for data.
        // ACQUIRE: pairs with the producer's release-store of tail_. If we see
        // the producer's new tail, this guarantees the producer's slot write (A)
        // is visible to us before we read the slot (D) below.
        const std::size_t tail = tail_.load(std::memory_order_acquire);

        if (tail == head) {
            return false;                 // empty
        }

        // (D) Read the data. Safe to see the producer's write because the acquire
        // above paired with the producer's release.
        out = buffer_[head & (Capacity - 1)];

        // Publish that the slot is now free. RELEASE: pairs with the producer's
        // acquire-load of head_, ensuring our read (D) completes-before the
        // producer reuses this slot.
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    // NOTE: these are approximate when called concurrently — head_/tail_ may be
    // observed at slightly different times. Fine for monitoring; do NOT use them
    // to make correctness decisions from a third thread.
    bool empty() const {
        return tail_.load(std::memory_order_acquire) ==
               head_.load(std::memory_order_acquire);
    }

    std::size_t size() const {
        return tail_.load(std::memory_order_acquire) -
               head_.load(std::memory_order_acquire);
    }

private:
    static constexpr std::size_t kCacheLine = 64;

    std::array<T, Capacity> buffer_;

    // alignas onto separate cache lines: the producer writes tail_, the consumer
    // writes head_. Without separation they'd share a line and every push/pop
    // would bounce the line between cores (false sharing).
    alignas(kCacheLine) std::atomic<std::size_t> head_{0};   // consumer writes
    alignas(kCacheLine) std::atomic<std::size_t> tail_{0};   // producer writes
};


/*
Q1. Objective: best-price lookup + ordered iteration + insert/remove levels
- std::map<Price, level>: This is a sorted binary search tree. good for searching fast and iterating in an order
- std::priority_queue: This allows constant time access to the best price, and log(N) time for insert/remove and also allows for iterating in order. Once the best price is filled, we can remove that from the book, the next best becomes available at the top and we can keep going like that.
- std::vector: We could keep a sorted vector of (price, level), it will give us the ordered iteration and best-price lookup but the insert and remove would be O(N) each.
- hashmap - It cannot store the price levels in an order, so not good for that, even though it provides constant time insert/removal ops.

So, I would go with std::priority_queue
*/
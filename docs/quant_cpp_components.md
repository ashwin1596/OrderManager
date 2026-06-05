# Quant / Trading-Systems C++ Components

Production-grade C++ components for low-latency trading systems.

The throughline: **on the hot path, you don't allocate, you don't lock if you can avoid it, you respect the cache, and you make latency predictable.** Predictability beats flexibility. Bounded and pre-sized beats dynamic and variable.

## Table of contents

1. [Object Pool (free-list allocator)](#1-object-pool-free-list-allocator)
2. [Fixed-Capacity Ring Buffer (SPSC, single-threaded)](#2-fixed-capacity-ring-buffer-spsc-single-threaded)
3. [Lock-Free SPSC Queue (the C++ memory model)](#3-lock-free-spsc-queue-the-c-memory-model)
4. [Limit Order Book — Design](#4-limit-order-book--design)
5. [Limit Order Book — Implementation (stages 2-4)](#5-limit-order-book--implementation-stages-2-4)
6. [Profiling & Measurement (Area 3 — the order book baseline)](#6-profiling--measurement-area-3--the-order-book-baseline)
7. [Optimization 4a — Intrusive List over Object Pool (results)](#7-optimization-4a--intrusive-list-over-object-pool-results)
8. [Optimization 4b — Flat-Array Price Index (design & decision not to build)](#8-optimization-4b--flat-array-price-index-design--decision-not-to-build)
9. [Threaded Pipeline — Integration Design (feed → match → report)](#9-threaded-pipeline--integration-design-feed--match--report)

## Cross-cutting principles (the trading-systems mindset)

- **Allocation ≠ construction.** Reserving memory and running a constructor are separate operations. High-performance code separates them: allocate raw memory once, construct objects into it on demand via placement new.
- **Never allocate on the hot path.** `new`/`malloc` can take microseconds (lock contention, syscalls, fragmentation) — catastrophic when the latency budget is nanoseconds. Pre-allocate at startup.
- **No exceptions on the hot path.** Throwing/catching is slow and non-deterministic. Prefer error returns (`nullptr`, status codes) or assertions.
- **Predictability > flexibility.** A pool that returns `nullptr` when exhausted is better than one that grows — growth means an unpredictable heap allocation at an unpredictable time, possibly moving memory and invalidating pointers.
- **Money is integers, never floating point.** Prices are in ticks/cents as integers (`int64_t`), never `double`. Floating-point rounding errors are unacceptable for money.
- **Observability is not correctness, but you need both.** The mechanism that *handles* failure (e.g., `nullptr` on exhaustion) is separate from the gauge that *lets you see it coming* (e.g., `available()` utilization metric). Production needs both.
- **Don't make monitoring more expensive than what it monitors.** Maintain O(1) counters rather than O(N) recomputation when a metric is sampled frequently.

---

## 1. Object Pool (free-list allocator)

The foundational latency idiom: **pre-allocate a block of fixed-size slots once, hand them out in O(1), recycle freed slots via an intrusive free list, never touch the heap on the hot path.**

### 1.1 The problem it solves

`new`/`malloc` on the hot path is a latency landmine: it can contend on a global allocator lock, trigger a syscall (`mmap`/`sbrk`), fragment the heap, and take a variable amount of time (nanoseconds to microseconds). In a system where you're racing to respond to a market-data tick in single-digit microseconds, an unpredictable allocation is unacceptable.

The pool eliminates this: allocate a big contiguous block at startup, then `allocate()`/`deallocate()` are O(1), heap-free, and touch a single cache line.

### 1.2 Allocation vs construction — the central distinction

Two separate operations are conflated by `new T()`:

1. **Allocation** — reserving raw bytes to hold the object.
2. **Construction** — running the constructor to make those bytes a valid object.

The pool separates them. It owns **raw, uninitialized, correctly-aligned memory** for `N` objects. No `T` constructors run at pool-creation time. Construction happens on demand, via placement new, when the caller has real data.

Why this matters:
- **Wasteful otherwise**: `std::vector<Order>(1000)` runs `Order()` 1000 times at startup, for objects you haven't filled in. You'd then overwrite that default state when real data arrives — wasted work.
- **Impossible otherwise**: if `Order` has no default constructor (e.g., only `Order(id, price, qty)`), `std::vector<Order>(1000)` won't even compile. The pool doesn't care — it just reserves bytes.

This is exactly what `std::allocator` and standard containers do internally: separate `allocate`/`deallocate` (memory) from `construct`/`destroy` (lifetime).

### 1.3 The union slot

```cpp
union Slot {
    T value;        // when the slot holds a live object
    Slot* next;     // when the slot is free: reuse its bytes as a free-list link
    Slot() {}       // do-nothing: we manage T's lifetime manually
    ~Slot() {}      // do-nothing: ditto
};
```

A `union` is the precise tool for "this memory is *either* a `T` *or* a free-list pointer, never both." That's the intrusive-free-list invariant expressed in the type system. The union is `sizeof(max(T, Slot*))` bytes, `alignof(max(T, Slot*))`-aligned — big enough and aligned correctly for either member.

The empty `Slot()`/`~Slot()` are required: a union with a non-trivial member (a `T` with constructors) has its special members implicitly deleted. We provide no-op versions because lifetime is managed manually via placement new / explicit destructor, not through the union.

### 1.4 The intrusive free list — zero overhead

When a slot is **free**, it holds no live object — its bytes are unused. So store the "next free slot" pointer *in the slot's own bytes*. The free list is a singly-linked list threaded through the free slots themselves; the pool holds one `free_head_` pointer.

- `allocate()`: pop the head (`slot = free_head_; free_head_ = slot->next;`).
- `deallocate(p)`: push onto the front (`p->next = free_head_; free_head_ = p;`).

**Zero memory overhead** — no separate container of free pointers. Contrast with a non-intrusive `std::vector<T*> free_list`, which costs 8 bytes per slot in a *separate* cache region, so every alloc/dealloc touches two distant cache lines. The intrusive list touches only the slot itself — one cache line.

The tradeoff (see 1.8): you lose the ability to cheaply ask "is this slot free?" — there's no out-of-band free flag; freeness is encoded only by reachability from `free_head_`.

### 1.5 Exhaustion policy — return `nullptr`, never grow

When `allocate()` is called with no free slots, the options:

| Policy | Verdict |
|---|---|
| Return `nullptr` | **Preferred.** Caller decides; no exceptions on hot path; deterministic |
| Throw `std::bad_alloc` | Avoid — exceptions on the hot path are slow and non-deterministic |
| Grow the pool | **Never.** Reintroduces the unpredictable heap allocation the pool exists to avoid; may move memory and invalidate pointers |
| Assert / terminate | Reasonable in debug; treat exhaustion as a fatal invariant violation |

Trading-systems mindset: **size the pool for the worst case** ("we will never have more than 100k live orders") and treat exhaustion as either a recoverable drop (`nullptr`) or a fatal bug (assert). Growth defeats the entire purpose.

### 1.6 Reference implementation

```cpp
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>
#include <new>          // placement new
#include <utility>      // std::forward
#include <cassert>

template <typename T>
class ObjectPool {
private:
    union Slot {
        T value;
        Slot* next;
        Slot() {}
        ~Slot() {}
    };

    std::vector<Slot> storage_;   // one-time contiguous allocation
    Slot* free_head_;             // head of intrusive free list (nullptr == exhausted)
    std::size_t free_count_;      // O(1) availability tracking

    bool is_valid_ptr(T* ptr) const {
        // Standards-compliant range check via integer comparison.
        // (Pointer relational comparison across separate allocations is UB.)
        auto addr  = reinterpret_cast<std::uintptr_t>(ptr);
        auto start = reinterpret_cast<std::uintptr_t>(storage_.data());
        auto end   = reinterpret_cast<std::uintptr_t>(storage_.data() + storage_.size());
        if (addr < start || addr >= end) return false;
        return (addr - start) % sizeof(Slot) == 0;   // lands on a slot boundary
    }

public:
    explicit ObjectPool(std::size_t capacity)
        : storage_(capacity)        // runs Slot() (no-op) per slot; NO T constructors
        , free_head_(nullptr)
        , free_count_(capacity)     // all slots free at construction — known directly
    {
        for (std::size_t i = capacity; i-- > 0; ) {   // i-- > 0 avoids size_t underflow
            storage_[i].next = free_head_;
            free_head_ = &storage_[i];
        }
    }

    ObjectPool(const ObjectPool&) = delete;
    ObjectPool& operator=(const ObjectPool&) = delete;

    template <typename... Args>
    T* allocate(Args&&... args) {
        if (free_head_ == nullptr) return nullptr;    // exhausted
        Slot* slot = free_head_;
        free_head_ = slot->next;
        --free_count_;
        // Placement new: construct T into existing storage; no heap allocation.
        return new (&slot->value) T(std::forward<Args>(args)...);
    }

    void deallocate(T* ptr) {
        if (ptr == nullptr) return;
        assert(is_valid_ptr(ptr) && "pointer not from this pool");
        ptr->~T();                                    // manual destruction
        Slot* slot = reinterpret_cast<Slot*>(ptr);    // safe: value is first union member
        slot->next = free_head_;
        free_head_ = slot;
        ++free_count_;
    }

    std::size_t capacity()  const { return storage_.size(); }
    std::size_t available() const { return free_count_; }
};
```

### 1.7 The five mechanisms, line by line

1. **`storage_(capacity)` runs `Slot()`, not `T()`.** The vector holds `Slot`, so it constructs `capacity` no-op `Slot`s. No `T` is alive yet — pure allocation, no construction.
2. **The threading loop** uses `i-- > 0` (not `i >= 0`, which never fails for unsigned `size_t` — the underflow trap). Each slot's `next` points to the previously threaded slot; `free_head_` ends at `storage_[0]`.
3. **Placement new** `new (&slot->value) T(...)` constructs a `T` into already-existing memory. No `malloc`. This is the operation that makes the pool fast.
4. **`std::forward<Args>(args)...`** perfectly forwards constructor arguments, preserving lvalue/rvalue categories — so `allocate(id, price, qty)` flows straight into `T(id, price, qty)`. Works for any `T` constructor signature.
5. **`ptr->~T()` + `reinterpret_cast<Slot*>(ptr)`.** Placement new has no automatic cleanup, so destruction is manual. After the `T` is destroyed, its bytes are free to be a `next` pointer again — the cast is safe because `value` is the first union member, so `&slot->value` and `slot` are the same address.

### 1.8 Why double-free detection is hard with an intrusive list

An intrusive free list has **no out-of-band "is free?" flag** — freeness is encoded *only* by reachability from `free_head_`. You cannot tell a free slot from an allocated one by inspecting the slot's bytes: a free slot's bytes are a `next` pointer; an allocated slot's bytes are a `T`, which could *coincidentally* look like a valid pointer into the storage range (false positive).

This is the cost of the intrusive list's zero-overhead win. Detection approaches, with tradeoffs:

| Approach | Cost | Notes |
|---|---|---|
| Walk the free list, check if `ptr` is already on it | O(N) per dealloc | Debug-only; compile out with `#ifndef NDEBUG` |
| Separate `std::vector<bool> allocated_` bitset | O(1) time, O(N/8) space | Standard debug-allocator approach; double-free = "bit already clear" |
| Canary / magic value in freed slots | O(1), probabilistic | Cheap but not bulletproof (live T could contain the magic) |
| **AddressSanitizer (`-fsanitize=address`)** | Test-build instrumentation | **The production answer** — don't hand-roll, use ASan in the test build |

Systems-design lesson: **every optimization has a cost somewhere.** The intrusive list saves memory and cache misses; it pays by losing O(1) freeness queries. You move the cost based on what the system needs — a trading system tested with ASan happily takes the memory win and treats double-free as a debug-build concern.

### 1.9 Usage

```cpp
struct Order {
    std::uint64_t id;
    std::int64_t  price;     // integer ticks/cents — NEVER float for money
    std::uint32_t quantity;
    Order(std::uint64_t i, std::int64_t p, std::uint32_t q)
        : id(i), price(p), quantity(q) {}
};

ObjectPool<Order> pool(100'000);              // pre-allocate at startup

Order* o = pool.allocate(12345, 10050, 100);  // construct in a pooled slot, O(1), no heap
if (o == nullptr) { /* exhausted — drop/log/terminate per policy */ }
// ... use o ...
pool.deallocate(o);                           // destroy, return slot to free list
```

### 1.10 Interview talking points

1. *"The pool separates allocation from construction. It owns raw aligned memory; objects are constructed on demand via placement new, never at pool-creation time. This avoids running constructors for objects you haven't filled in, and works even for types with no default constructor."*
2. *"Free slots are tracked with an intrusive free list — the 'next free' pointer lives in the free slot's own bytes, since a free slot holds no live object. Zero memory overhead, single-cache-line access per operation, versus a separate free-pointer container that costs 8 bytes per slot and a second cache line."*
3. *"On exhaustion I return `nullptr`, not throw and not grow. Exceptions are slow and non-deterministic on the hot path; growth reintroduces the unpredictable heap allocation the pool exists to eliminate. Size for the worst case."*
4. *"`available()` is an O(1) observability gauge — separate from the `nullptr` correctness contract. Production monitors pool utilization to alert before exhaustion and to detect leaks (slow downward drift in available count)."*
5. *"Double-free detection is hard with an intrusive list because there's no out-of-band free flag. In debug I'd keep an allocated-bitset; in practice I'd rely on AddressSanitizer rather than hand-rolling it."*
6. *"O(1) allocate and deallocate, no heap access, one cache line touched. Single-threaded — a thread-safe version needs atomics or a lock, which is a separate concern."*

### 1.11 Common follow-ups

**"Make it thread-safe."** Two routes: a mutex around `allocate`/`deallocate` (simple, but lock contention is a latency cost), or a lock-free free list using `std::atomic<Slot*> free_head_` with compare-and-swap (complex — the ABA problem appears, requiring tagged pointers or hazard pointers). For SPSC-style usage (one producer, one consumer), simpler structures suffice. Covered in the concurrency section.

**"What about over-aligned types (`alignas(64)` for cache-line alignment)?"** The union handles normal alignment, but a type requiring 64-byte alignment may need an aligned backing allocation (`std::aligned_alloc` or a custom aligned allocator) rather than a plain `std::vector<Slot>`, depending on the standard library's guarantees. Worth flagging when the pooled type is itself cache-line-aligned.

**"What if objects have very different sizes?"** A single pool handles one fixed size (`sizeof(Slot)`). For heterogeneous sizes, use multiple pools (one per size class — like a slab allocator), or a general-purpose arena. Fixed-size pools are preferred when you can categorize allocations by type.

**"How does this compare to a stack allocator / arena / bump allocator?"** A bump allocator hands out memory by advancing a pointer — even faster (no free list), but it can only free *everything at once* (reset the pointer), not individual objects. Good for per-frame or per-request scratch memory with a clear bulk-free point. The pool supports individual deallocation; the arena supports only bulk reset. Choose based on the lifetime pattern.

**"What's the ABA problem in a lock-free version?"** When a CAS on `free_head_` reads value A, another thread pops A and pushes it back (now A again, but the list changed underneath), and the original CAS succeeds incorrectly. Solved with tagged pointers (a version counter packed alongside the pointer) or hazard pointers. A classic lock-free pitfall — covered when we build lock-free structures.

---

## 2. Fixed-Capacity Ring Buffer (SPSC, single-threaded)

A circular FIFO queue on a contiguous fixed-size array, indices wrapping around. The workhorse for passing data between pipeline stages (feed → strategy → gateway). Built single-threaded here; becomes a lock-free SPSC queue in the concurrency section with a small delta (atomics + memory ordering). This stage nails the index discipline and cache-awareness without the complexity of atomics.

### 2.1 How it relates to the Object Pool — orthogonal tools

The pool and the ring buffer solve different problems and compose:

- **Object pool**: a *memory allocator*. "Where do my objects' bytes come from, and how do I recycle them without the heap?" Manages ownership/lifetime of storage for one fixed-size type.
- **Ring buffer**: a *queue / communication channel*. "How do I pass a sequence of items from A to B, in order, with bounded memory?" Manages flow/ordering between a producer and consumer.

They compose in the canonical producer-consumer pipeline:

```
[Feed thread]                              [Strategy thread]
 pool.allocate() → ring.push(ptr) → ... → ring.pop(ptr) → use → pool.deallocate()
```

The pool owns the `Order`'s storage; the ring buffer shuttles a cheap 8-byte *pointer* (not the whole object) across threads. Either is usable without the other (ring of `int`s needs no pool; a pool used inline needs no ring). They're independent, and together they're the skeleton of a trading pipeline.

### 2.2 Power-of-2 capacity and bitmask indexing

Indices increment monotonically (forever); you map them into the array with wraparound. `index % Capacity` is the obvious wrap, but `%` is integer division — slow (tens of cycles, poorly pipelined). If `Capacity` is a power of 2:

```
index & (Capacity - 1)   ≡   index % Capacity
```

A power of 2 in binary is a single 1 followed by zeros (`8 = 0b1000`); `Capacity - 1` is all-ones below that bit (`7 = 0b0111`). `index & (Capacity - 1)` keeps the low bits, which is exactly the remainder mod `Capacity`. One cheap instruction vs. a division.

A good compiler may strength-reduce `% Capacity` to the mask automatically *when `Capacity` is a compile-time power-of-2 constant* — but in low-latency code you write the mask explicitly to document intent and not rely on the optimizer. Enforce the power-of-2 requirement at compile time:

```cpp
static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of 2");
```

**Precedence trap**: `==` binds tighter than `&` in C++ (a C holdover). `Capacity & (Capacity - 1) == 0` parses as `Capacity & ((Capacity - 1) == 0)` — wrong. Always parenthesize: `(Capacity & (Capacity - 1)) == 0`.

### 2.3 The full-vs-empty ambiguity — monotonic counters

If `head` and `tail` are indices in `[0, Capacity)`, then both *empty* and *full* satisfy `head == tail` (the tail wraps around to meet the head). You cannot distinguish them by comparing wrapped indices alone. Three standard resolutions:

| Resolution | Mechanism | Drawback |
|---|---|---|
| Separate `count_` | `full = count == Capacity`, `empty = count == 0` | `count_` is written by *both* threads → contention point in the concurrent version |
| Sacrifice one slot | `full = (tail+1) % Cap == head` | Holds only `Capacity - 1` elements |
| **Monotonic counters** | `head`/`tail` increment forever, mask only when indexing | None for SPSC — the right choice |

**Use monotonic counters.** `head_` and `tail_` are `std::size_t` that increment forever and are masked only at indexing time:

```
size()  = tail_ - head_
empty() = tail_ == head_
full()  = tail_ - head_ == Capacity
index:    buffer_[tail_ & (Capacity - 1)]   // and head_ likewise
```

**Why this beats a shared `count_`**: with monotonic counters, the **producer only writes `tail_`** and the **consumer only writes `head_`** — single-writer-per-variable. A shared `count_` would be written by both (increment on push, decrement on pop), forcing its cache line to bounce between cores on every operation. The monotonic design is what makes the lock-free version efficient. This decision is forward-looking — make it now even though single-threaded doesn't care.

**Unsigned overflow is safe here**: the monotonic counters eventually wrap at `2^64`, but `tail_ - head_` computes correctly *even across a wrap* because unsigned subtraction is modular arithmetic (defined behavior, unlike signed overflow which is UB). As long as the buffer never holds more than `Capacity ≤ 2^63` elements, the difference is always correct. At a billion ops/sec, `2^64` ops is ~585 years — academic, but the *reason* it's safe (modular unsigned arithmetic) is the point.

### 2.4 Storage — plain `std::array`, default construction is fine

Storage is `std::array<T, Capacity>` — an inline array member, zero heap allocation. Constructing the ring buffer default-constructs all `Capacity` elements of `T`.

**This is fine for the ring buffer, unlike the pool.** Why the difference?

- **Pool** holds potentially-heavy or constructor-less objects → default construction is wasteful or impossible → must avoid it (union + placement new).
- **Ring buffer** typically holds cheap-to-construct types — pointers (`Order*`), small PODs, scalars — passed by value. Default-constructing `Capacity` pointers or `int`s is trivial and harmless. So `std::array<T, Capacity>` and assign-on-push is correct and simplest.

(If you ever needed a ring buffer of heavy, constructor-less objects, you'd use the same aligned-storage + placement-new technique as the pool. Rare; optional refinement.)

### 2.5 False sharing and cache-line alignment

A CPU fetches memory in **64-byte cache lines** (x86-64, ARM64). Cache coherence (MESI) tracks ownership at *cache-line granularity, not variable granularity*: a writable line lives in only one core's cache at a time; another core's write invalidates all other copies, forcing a re-fetch (~40-100ns, order-of-magnitude, verify per platform).

**False sharing**: two threads write *different variables that happen to share a cache line*. They're not logically sharing data, but the hardware behaves as if they are — every write by one invalidates the other's line. In the ring buffer, `head_` (consumer-written) and `tail_` (producer-written) are 8 bytes each, so they'd sit on the same line. In the concurrent version, every `push` would invalidate the consumer's line and every `pop` the producer's — a cache-line bounce on *every operation*, despite the threads touching independent variables.

Fix: force `head_` and `tail_` onto separate cache lines via alignment.

```cpp
static constexpr std::size_t kCacheLine = 64;
alignas(kCacheLine) std::size_t head_ = 0;
alignas(kCacheLine) std::size_t tail_ = 0;
```

Options for the cache-line size: hardcode `64` via a named constant (pragmatic — x86-64 and ARM64 are both 64-byte, covers ~all server hardware), or use C++17's `std::hardware_destructive_interference_size` (portable name, but uneven compiler support and ABI-stability warnings). Production codebases commonly hardcode 64 with a named constant.

**`alignas` aligns the *start*, not the *end*.** It guarantees the variable begins a new cache line, but the bytes after it within that line are unused padding. If you added a member right after `head_` *without* its own `alignas`, it would share `head_`'s line. Here `tail_` has its own `alignas`, so they're isolated from each other. For full isolation against future-added adjacent members, you'd pad to a complete line (wrap in an `alignas(64)` struct or add trailing padding).

### 2.6 False sharing vs data races — orthogonal

**Critical caveat**: the `alignas` padding does NOT make the buffer thread-safe. Padding fixes *false sharing* (a performance problem); it does nothing about *data races* (a correctness problem). The single-threaded version with padding is still UB across threads — `head_`/`tail_` are plain `size_t`, not `std::atomic`, and there's no memory ordering to guarantee the consumer sees `buffer_[idx]`'s write before it sees the incremented `tail_`.

- **False sharing**: "correct but slow." Fixed by layout (padding).
- **Data race**: "fast but wrong." Fixed by atomics + memory ordering.

A correct lock-free queue needs *both* — atomics for correctness, padding for performance. They're independent concerns; fixing one doesn't fix the other. (The atomics come in the concurrency section.)

Also distinguish **false sharing** (fixable) from **true sharing** (inherent): the producer must *read* `head_` to check `full()`, and the consumer must *read* `tail_` to check `empty()`. Those cross-reads are genuine data dependencies — padding can't eliminate them, only the false collisions between the two writes.

### 2.7 Reference implementation

```cpp
#pragma once

#include <array>
#include <cstddef>

template <typename T, std::size_t Capacity>
class RingBuffer {
public:
    static_assert(Capacity > 0, "Capacity must be greater than 0");
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of 2");

    bool push(const T& item) {
        if (full()) return false;
        buffer_[tail_ & (Capacity - 1)] = item;
        ++tail_;
        return true;
    }

    bool pop(T& out) {
        if (empty()) return false;
        out = buffer_[head_ & (Capacity - 1)];
        ++head_;
        return true;
    }

    bool empty() const { return tail_ == head_; }
    bool full()  const { return tail_ - head_ == Capacity; }
    std::size_t size() const { return tail_ - head_; }

private:
    static constexpr std::size_t kCacheLine = 64;
    std::array<T, Capacity> buffer_;
    alignas(kCacheLine) std::size_t head_ = 0;   // next read (pop) — consumer writes
    alignas(kCacheLine) std::size_t tail_ = 0;   // next write (push) — producer writes
};
```

O(1) per operation, zero heap allocation, FIFO, correct full/empty via monotonic counters.

### 2.8 Bugs to watch for (encountered while building)

- **`==` vs `&` precedence** in the `static_assert` — parenthesize the `&`. Silent if only tested with `Capacity == 1`.
- **Missing `return true;`** on the success path — UB (falls off a non-void function). Compiler warns with `-Wreturn-type`.
- **`full()` subtraction order**: `tail_ - head_ == Capacity`, NOT `head_ - tail_` (unsigned, would wrap to a huge number and never equal `Capacity` → silent data corruption). Must agree with `size()`. Catch by filling the buffer to capacity in a test and asserting the next push fails.
- **Reintroducing `size_`**: under coding pressure it's easy to revert to the textbook ring-buffer-with-count even after reasoning to the monotonic-counter design. Keep the design rationale visible (a comment) so muscle memory doesn't override it.
- **Sweep all occurrences when fixing a pattern**: updating `push` to use `full()` but leaving `pop` on the old `size_ == 0` check is a classic stale-paste bug.

### 2.9 Interview talking points

1. *"Power-of-2 capacity lets me wrap indices with `index & (Capacity - 1)` instead of `% Capacity` — one instruction vs. integer division. Enforced with a `static_assert`; the compiler might strength-reduce the modulo but I don't rely on it."*
2. *"Full vs empty is ambiguous with wrapped indices — both give `head == tail`. I use monotonic counters that increment forever and mask only when indexing: `size = tail - head`, `empty = tail == head`, `full = tail - head == Capacity`."*
3. *"Monotonic counters over a shared `count_` because the producer only writes `tail` and the consumer only writes `head` — single-writer-per-variable. A shared count would be written by both threads, bouncing its cache line between cores every operation."*
4. *"`head_` and `tail_` are `alignas(64)` onto separate cache lines to prevent false sharing — otherwise the producer's write to `tail` would invalidate the consumer's cache line holding `head`, and vice versa, on every operation."*
5. *"The padding is necessary but not sufficient for thread safety — it fixes false sharing (performance) but not data races (correctness). The lock-free version also needs atomics with acquire/release ordering."*
6. *"`std::array` storage, default-constructed — fine here because ring buffers typically hold cheap types (pointers, PODs), unlike the pool which needed to avoid constructing heavy or constructor-less objects."*

### 2.10 Common follow-ups

**"Make it lock-free for one producer and one consumer."** Change `head_`/`tail_` to `std::atomic<std::size_t>`. Producer: load `tail_` relaxed (it owns it), check fullness by loading `head_` acquire, write the slot, store `tail_` release. Consumer: mirror. The release/acquire pair ensures the slot write is visible before the index update. Covered in the concurrency section.

**"What if there are multiple producers or consumers?"** SPSC (single-producer-single-consumer) is the simplest and fastest. MPMC needs CAS loops on the indices and is substantially more complex (the Vyukov bounded MPMC queue is the canonical design). Don't reach for MPMC unless you actually have multiple producers — SPSC pipelines (one stage to the next) cover most trading architectures.

**"Why not `std::queue` or `boost::lockfree::spsc_queue`?"** `std::queue` heap-allocates per element (node-based `std::deque` backing) — unacceptable on the hot path. `boost::lockfree::spsc_queue` is a production-quality version of exactly this; in real code you'd often use it rather than hand-roll. Building it yourself is for understanding; knowing the library exists is for the interview.

**"What about variable-size messages?"** A ring buffer of fixed-size slots doesn't directly handle variable-size payloads. Options: store pointers/handles to externally-allocated payloads (compose with the pool), or use a byte-oriented ring buffer (a "bip buffer" / circular byte buffer) where messages are length-prefixed. The pointer-handle approach is more common in trading systems.

**"How do you size the capacity?"** Large enough that the consumer never falls so far behind that the producer blocks (or drops). Sized from the burst rate of the producer and the worst-case consumer latency. Too small → drops/backpressure under bursts; too large → wasted memory and worse cache behavior. A capacity-planning decision driven by measured traffic, like the pool's sizing.

---

## 3. Lock-Free SPSC Queue (the C++ memory model)

The single-threaded ring buffer (section 2), made safe for one producer thread and one consumer thread by swapping the indices for `std::atomic` with carefully-chosen memory orders. The structure is identical; the entire substance of this component is **the memory ordering**. This is the deepest topic in the curriculum and the one where "looks correct" and "is correct" diverge most dangerously.

### 3.1 Why the memory model exists — two layers of reordering

Naive model: statements execute in order, and all threads see updates in the order they happened. **Both halves are false on modern hardware.** Two independent reordering layers sit between what you write and what another thread observes:

1. **Compiler reordering.** The optimizer reorders/eliminates/fuses memory operations freely as long as the result is correct *for a single thread in isolation*. `data = 42; ready = true;` may be emitted in either order — single-threaded, the order doesn't matter. Another thread watching `ready` could see `ready == true` while `data` is still garbage.

2. **CPU/hardware reordering.** Even with compiler order preserved, the CPU reorders at runtime. Stores sit in a per-core **store buffer** before reaching shared cache; loads may be satisfied out of order. x86-64 has a relatively strong model (mostly preserves store order); ARM/POWER are weak (reorder aggressively). Code that "works" on an x86 laptop can break on an ARM server.

The C++ memory model is the contract that lets you constrain these reorderings *just enough* for correctness without over-constraining (which costs performance).

**A data race is undefined behavior**, not merely "might read stale." Two threads accessing the same non-atomic location, ≥1 writing, with no synchronization, is UB — the compiler may assume it never happens, and the program may do anything. Atomics make the access *defined*.

### 3.2 What `std::atomic` provides — two separate guarantees

1. **Atomicity** — the operation is indivisible; no thread sees a half-written value. (A plain aligned `size_t` write is often atomic on x86 in practice, but the standard doesn't promise it and the compiler may assume no concurrent reader.)
2. **Ordering** — atomic operations act as *barriers* constraining how surrounding non-atomic operations may be reordered, per the **memory order** specified. This is the part that matters most and is least intuitive — the memory-order argument tunes it.

### 3.3 The memory orders (the three that matter for SPSC)

**`memory_order_relaxed`** — atomicity only, no ordering. The op is indivisible, but surrounding operations may be freely reordered around it. Use when you need only atomicity, not ordering (e.g., a global counter read only at the end). Cheapest — often a plain load/store on x86.

**`release` (store) + `acquire` (load)** — the workhorse pair, establishing a cross-thread *happens-before* edge:
- **`release` store**: all writes *before* this store become visible to any thread that *acquires* the same atomic and reads this value. One-way barrier (earlier ops can't move after it).
- **`acquire` load**: all writes that happened before the matching release become visible *after* this load. One-way barrier the other way (later ops can't move before it).
- **The pairing**: when thread 1 release-stores an atomic and thread 2 acquire-loads that *same* atomic and reads the value thread 1 wrote, then everything thread 1 did before the release is visible to thread 2 after the acquire.

**`memory_order_seq_cst`** — strongest, and the *default* if you omit the order argument. Acquire/release semantics *plus* a single global total order all threads agree on for seq_cst ops. Easiest to reason about (one global interleaving), hence the safe default — but **overkill for SPSC**: enforcing the global total order needs a heavier fence (on x86, a seq_cst store needs `MFENCE` or a `lock`-prefixed instruction; acquire/release stores are often free). You pay for a guarantee you don't need.

### 3.4 Cost hierarchy (x86-64, order-of-magnitude — verify per platform)

| Order | x86 cost |
|---|---|
| `relaxed` | plain load/store — essentially free |
| `acquire` load / `release` store | also often a plain load/store on x86 (the hardware model is already strong enough); cost is a *compiler* barrier preventing reordering. On ARM, a lightweight fence. |
| `seq_cst` | full fence on the store (`MFENCE` / `lock` instruction) — tens of cycles, drains the store buffer |

Punchline: on x86, **release/acquire is nearly free; seq_cst is not.** This is *why* low-latency code uses release/acquire deliberately instead of accepting the seq_cst default — same correctness for the SPSC handoff, lower cost. On ARM the gap is wider.

### 3.5 The correctness proof — the one critical interleaving

```
Producer:  buffer_[i] = item;             (A) non-atomic write — the payload
           tail_.store(i+1, release);     (B) release — publishes A

Consumer:  t = tail_.load(acquire);       (C) acquire — if it reads i+1...
           out = buffer_[i];              (D) non-atomic read
```

(B) release and (C) acquire pair. **If** (C) reads the value (B) stored, the pairing guarantees (A) *happened-before* (D) — the consumer cannot reach (D) and see unwritten memory. That is the entire correctness argument.

With a `relaxed` load at (C), the pairing wouldn't exist: the consumer could observe `tail == i+1` (the flag) yet read stale `buffer_[i]` (the data not yet visible across cores). Intermittent, data-dependent corruption — the worst kind.

**Precise failure framing**: relaxed doesn't mean "the write didn't happen" — the producer may have executed it. It means "no promise about *when other cores see it relative to the index update*." The write may sit in the producer core's store buffer, unpropagated, while the index update *has* propagated. The bug is *visibility ordering across cores*, not wall-clock completion.

### 3.6 Which loads need acquire — the decision rule

The pattern "producer release, consumer acquire" is incomplete. *Which* loads need acquire depends on **whether there is non-atomic data, written by the other thread, that this atomic guards and that I'm about to read.**

| Operation | Thread | Order | Why |
|---|---|---|---|
| Load own index (`tail` in push, `head` in pop) | self | **relaxed** | you're the only writer of your own index; no other thread writes it |
| Load other's index *to access guarded data* (`tail` in pop) | consumer | **acquire** | the slot data is behind `tail`; must pair with producer's release |
| Store own index *to publish data* (`tail` in push, `head` in pop) | producer/consumer | **release** | publishes the slot write/read; pairs with the other thread's acquire |
| Load other's index *only to check capacity* (`head` in push) | producer | **acquire (conventional) / relaxed (optimized)** | see below |

**The `head` load in `push` — the subtle case.** The producer reads `head` *only to check for space*; it touches no consumer-protected data. In the strictest analysis it *could* be `relaxed`: a stale (smaller) `head` only makes the buffer look *fuller* than it is → the producer might spuriously return "full" → safe and self-correcting. A stale head is always ≤ the true head, so `tail - head` is always ≥ the true count — you can only err toward "too full," never "falsely empty" (which would be catastrophic, overwriting unread data).

**But use `acquire` as the default**, because: (1) it's unambiguously correct — the acquire pairs with the consumer's `head` release, guaranteeing the consumer's slot-read completes before the producer reuses that slot (matters for non-trivial `T` or slot-reuse races); (2) on x86 it's free (a plain load); (3) the relaxed optimization requires the careful staleness argument and breaks if `T` or the reuse logic changes. Relax it only after profiling proves the index load is a measured bottleneck *and* you've proven staleness is safe for your specific `T`.

**The mental test for any atomic access**: *"Is there non-atomic data, written by the other thread, that I'm about to read and that this atomic guards?"* Yes → acquire/release pairing. Reading only the value for a capacity decision → relaxed may suffice (with the conservative-staleness argument).

### 3.7 Reference implementation

```cpp
#pragma once

#include <atomic>
#include <array>
#include <cstddef>
#include <utility>      // std::move

// Single-Producer Single-Consumer lock-free queue.
// CONTRACT: exactly one producer thread calls push(), exactly one consumer
// thread calls pop(). Two producers or two consumers → UB. NOT MPMC-safe.
template <typename T, std::size_t Capacity>
class SPSCQueue {
public:
    static_assert(Capacity > 0, "Capacity must be greater than 0");
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of 2");

    bool push(const T& item) {                              // lvalue → copy
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        const std::size_t head = head_.load(std::memory_order_acquire);
        if (tail - head == Capacity) return false;          // full
        buffer_[tail & (Capacity - 1)] = item;              // (A) payload write
        tail_.store(tail + 1, std::memory_order_release);   // (B) publish A
        return true;
    }

    bool push(T&& item) {                                   // rvalue → move
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        const std::size_t head = head_.load(std::memory_order_acquire);
        if (tail - head == Capacity) return false;
        buffer_[tail & (Capacity - 1)] = std::move(item);   // move assign — ordering unchanged
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    bool pop(T& out) {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        const std::size_t tail = tail_.load(std::memory_order_acquire);  // (C) pairs with (B)
        if (tail == head) return false;                     // empty
        out = std::move(buffer_[head & (Capacity - 1)]);    // (D) move out (required for move-only T)
        head_.store(head + 1, std::memory_order_release);   // publish slot free
        return true;
    }

    bool empty() const {
        return tail_.load(std::memory_order_acquire) == head_.load(std::memory_order_acquire);
    }
    std::size_t size() const {
        return tail_.load(std::memory_order_acquire) - head_.load(std::memory_order_acquire);
    }

private:
    static constexpr std::size_t kCacheLine = 64;
    std::array<T, Capacity> buffer_;
    alignas(kCacheLine) std::atomic<std::size_t> head_{0};   // consumer writes
    alignas(kCacheLine) std::atomic<std::size_t> tail_{0};   // producer writes
};
```

Only three changes from the single-threaded ring buffer: indices are `std::atomic`, each access carries an explicit memory order, and the structure is otherwise identical. That small, focused delta is the payoff of building the single-threaded version first.

### 3.8 Move semantics — orthogonal to ordering

The `push(T&&)` overload moves the item into the slot (`std::move`), and `pop` moves out (`std::move(buffer_[...])`). Two points:

- **Move vs. copy is orthogonal to the memory ordering.** Move changes *how the assignment happens* (steal the guts vs. duplicate); the release/acquire governs *when the write is visible across cores*. The atomics are byte-identical between the copy and move overloads.
- **`T&&` here is a plain rvalue reference, NOT a forwarding reference.** `T` is the *class* template parameter, already fixed when the class was instantiated — so `T&&` binds only to rvalues. Hence the two-overload pattern (`const T&` for lvalues/copy, `T&&` for rvalues/move). Contrast with the object pool's `allocate`, which used *perfect forwarding* (`template<class... Args>` + `std::forward`) because it deduced argument types per-call to *construct* a `T`. Here we *assign* into an existing slot, so the two-overload approach is right. Know when each applies.

**Move-only types** (`SPSCQueue<std::unique_ptr<Order>>`) compile *only* because of the move overload and the moving `pop` — `unique_ptr` can't be copied, so the `const T&` overload would fail to compile if instantiated. This enables a clean ownership-transfer design: the queue owns objects via `unique_ptr`, ownership moves across the thread boundary, no separate pool needed. RAII makes it leak-safe — destroying the queue destroys the held `unique_ptr`s, freeing their objects. (Contrast `SPSCQueue<Order*>` with raw pointers: destroying the queue does *not* free the pointees — raw pointers don't own; that's the pool's job.)

### 3.9 What the queue stores — `T` by value, caller's choice

`SPSCQueue<T>` stores `T` *by value* in `buffer_`:

| `T` = | Slot size | `push` copies/moves | When |
|---|---|---|---|
| `Order*` (pointer) | 8 bytes | one pointer | Object lives in a pool; queue shuttles handles. Large objects. |
| `Order` (value) | `sizeof(Order)` | whole struct | No external storage; queue owns the data. |
| `unique_ptr<Order>` | 8 bytes | ownership transfer | Queue owns objects; ownership moves across threads. |

Small POD by value can be *faster* than the pointer approach — copying 24 bytes is cheap and you avoid the pointer indirection (a potential cache miss) on the consumer's read. Large object → pointer wins. **Rule of thumb: small PODs by value (cache-friendly, no indirection); large objects by pointer (cheap transport, pooled storage).**

### 3.10 False sharing vs data races — both addressed

The `alignas(64)` on `head_`/`tail_` (carried over from the ring buffer) fixes **false sharing**: producer writes `tail_`, consumer writes `head_`; on separate cache lines, neither write invalidates the other's line. The **atomics + memory ordering** fix **data races**. These are orthogonal — false sharing is "correct but slow," a data race is "fast but wrong." A correct lock-free queue needs both: atomics for correctness, padding for performance.

### 3.11 Interview talking points

1. *"The structure is the single-threaded ring buffer; the only changes are atomic indices and explicit memory orders. The correctness rests on one release/acquire pairing: the producer release-stores `tail` after writing the slot; the consumer acquire-loads `tail` before reading the slot. If the consumer sees the new tail, the slot write is guaranteed visible."*
2. *"A relaxed store on `tail` would break it — the consumer could see the incremented index but read stale slot data, because relaxed gives no promise about when the slot write becomes visible across cores relative to the index update."*
3. *"Which loads need acquire depends on whether there's data behind the atomic. The consumer's `tail` load needs acquire — the slot data is behind it. The producer's `head` load only checks capacity, so it could be relaxed, but I default to acquire: it's free on x86 and unambiguously correct."*
4. *"I use release/acquire, not the seq_cst default. seq_cst enforces a global total order that SPSC doesn't need and that costs a full fence (store-buffer drain) on x86. Release/acquire is sufficient and nearly free."*
5. *"Move vs. copy is orthogonal to ordering — the move overload changes the assignment mechanism, not the synchronization. The `T&&` is a plain rvalue reference (T is fixed), so it's the two-overload pattern, not perfect forwarding."*
6. *"`alignas(64)` on the indices prevents false sharing (a performance issue); the atomics prevent data races (a correctness issue). Both are needed and they're independent."*

### 3.12 Common follow-ups

**"Make it MPMC (multi-producer multi-consumer)."** Substantially harder. Multiple producers race on `tail_` — two could read the same value and write the same slot. Requires CAS loops (`compare_exchange_weak`) to claim a slot atomically, and you hit the ABA problem and need careful slot-state tracking. The Vyukov bounded MPMC queue is the canonical lock-free design. Don't reach for MPMC unless you actually have multiple producers — most trading pipelines are chains of SPSC stages.

**"What if push/pop should block instead of returning false?"** Blocking reintroduces synchronization: a condition variable (which uses a mutex — defeats lock-freedom) or a spin-wait (`while (!push(x)) { _mm_pause(); }` — burns CPU but stays lock-free and low-latency). Trading systems usually prefer non-blocking with `false`-return so the caller decides: spin, drop, or do other work. Busy-spinning with a CPU `pause` hint is common on a dedicated core.

**"Why not `std::queue` + `std::mutex`?"** `std::queue` heap-allocates per element — unacceptable on the hot path. A mutex adds lock/unlock cost and, worse, *priority inversion* and unbounded latency if the holder is descheduled. The lock-free SPSC queue has bounded, predictable latency and no allocation — the trading-systems requirement.

**"What's `_mm_pause` / the spin-wait hint?"** On a busy-spin loop, `_mm_pause()` (x86 `PAUSE` instruction) hints to the CPU that this is a spin-wait — it reduces power, avoids a memory-order-violation pipeline flush when the loop exits, and yields some resources to a hyperthread sibling. Standard in spin loops on a dedicated polling core.

**"How do you test a lock-free queue?"** Hard — races are nondeterministic. Tools: ThreadSanitizer (`-fsanitize=thread`) detects data races at runtime; stress tests with many iterations across producer/consumer threads checking FIFO order and no lost/duplicated items; and tools like `relacy` or model checkers for exhaustive interleaving exploration. You cannot prove correctness by running it once — the failing interleaving may be rare.

**"Does `std::atomic<std::size_t>` use a lock internally?"** Check `std::atomic<std::size_t>::is_lock_free()` (or `is_always_lock_free` at compile time). For word-sized integers on mainstream platforms, yes — backed by native atomic instructions, no lock. For larger types (e.g., `std::atomic<BigStruct>`), the implementation may fall back to a hidden mutex, silently destroying the lock-freedom. Always verify `is_lock_free()` for non-trivial atomic types.

---

## 4. Limit Order Book — Design

The capstone. A limit order book (LOB) is the matching engine's core data structure — it holds all resting limit orders for one instrument, organized so the exchange can match incoming orders efficiently under **price-time priority**. It composes everything from sections 1-3: the object pool stores orders, intrusive lists give time priority at each level, and the latency discipline runs throughout.

This section documents the **design** — the domain model and the three structural decisions. The reference implementation (types, `add` with matching, `cancel`, `match`) follows in subsequent build stages.

### 4.1 Domain model

- **Bid**: order to *buy* at a price or better (lower). **Ask/offer**: order to *sell* at a price or better (higher).
- **Limit order**: "buy/sell N at price P or better." Rests in the book if it can't fill immediately.
- **Market order**: "buy/sell N at any price." Never rests — matches against best resting orders until filled.
- **Spread**: gap between best (highest) bid and best (lowest) ask. The best bid is always *below* the best ask in a sane book (else they'd have matched).
- **Price level**: all orders at the same price on the same side. The book is a stack of price levels per side.

```
        ASKS (sellers)              ← match incoming BUYs against the LOWEST ask
  ┌──────────────────────┐
  │ 10.03: [orders]       │
  │ 10.02: [orders]       │
  │ 10.01: [orders]       │  ← best ask (lowest sell)
  ├──────────────────────┤   ← spread
  │ 10.00: [orders]       │  ← best bid (highest buy)
  │  9.99: [orders]       │
  └──────────────────────┘
        BIDS (buyers)               ← match incoming SELLs against the HIGHEST bid
```

### 4.2 The four operations and their latency profile

1. **Add (limit)**: insert a resting order. If it *crosses* (buy ≥ best ask, or sell ≤ best bid), it matches immediately against the opposite side; the non-crossing remainder rests.
2. **Cancel**: remove a resting order by ID. **The most frequent operation** — cancel-to-trade ratios of 100:1+ are common. Must be fast.
3. **Modify**: change quantity/price. Often cancel + re-add. Price change loses time priority; a quantity *decrease* may keep it.
4. **Match**: when an incoming order crosses, fill it against resting orders on the opposite side, respecting price-time priority.

### 4.3 Price-time priority — the rule that drives the design

Orders fill in this order:
1. **Price priority**: better prices first. Incoming buys take the lowest asks; incoming sells take the highest bids.
2. **Time priority** (within a price level): among same-price orders, oldest fills first — **FIFO at each level.**

So the book must efficiently answer three questions, and these *are* the design constraints:
- "What's the best price on each side?" → best-price lookup + ordered iteration.
- "At this level, which order is oldest?" → FIFO per level.
- "Where is order #12345?" → O(1) find-by-ID for cancel.

### 4.4 Decision 1 — price-level container: `std::map` per side

**The trap: `std::priority_queue` is wrong**, despite its O(1) best-price access. It fails the book's other needs:
- **Can't iterate in order non-destructively** — only exposes `top()`. Walking levels best→next-best during matching would require popping (which removes) then re-pushing. The book must traverse levels read-only mid-match.
- **Can't remove from the middle** — levels deep in the book empty out constantly (cancels); `priority_queue` only removes the top.
- **Can't look up a specific price** — "is there a level at 10.02?" is unanswerable; only the max is reachable.

`priority_queue` is optimized for "extract-max-and-discard." The book needs find-best *plus* iterate *plus* lookup-by-price *plus* remove-from-middle. Wrong access pattern.

**`std::map<Price, Level>` is the correct answer** — a balanced BST giving all needed operations:

| Operation | `std::map` |
|---|---|
| Best price | `begin()` (asks, ascending) / `rbegin()` (bids) — O(1) amortized |
| Ordered iteration | `++it`, non-destructive — O(1)/step |
| Look up a price | `find(price)` — O(log N) |
| Insert/remove a level | `emplace`/`erase` — O(log N) |

Asks: `std::map<Price, Level>` ascending (best = `begin()`). Bids: `std::map<Price, Level, std::greater<Price>>` descending, or `rbegin()` on an ascending map.

**The HFT optimization — flat array indexed by price.** Domain fact: prices are *discrete and bounded* (integer ticks in a known range, never arbitrary `double`s). So index an array by tick: `levels[price_in_ticks] → Level`. O(1) everything, contiguous, cache-friendly, no pointer chasing or tree balancing. Track best bid/ask with a separate index updated as levels fill/empty; find next-best non-empty level via a bitmap of occupied levels.

| | `std::map` (tree book) | Array (flat book) |
|---|---|---|
| Ops | O(log N) | O(1) |
| Memory | ∝ active levels | ∝ price *range* (sparse if huge) |
| Cache | pointer-chasing, unfriendly | contiguous, friendly |
| Next-best when level empties | `++it` | scan / occupancy bitmap |

**Interview framing**: lead with `std::map` (correct, clean), then offer the flat array as the latency specialization exploiting bounded discrete prices. Same pattern as pool/ring-buffer — replace a general structure with a domain-specialized one that's faster. *We build the `std::map` version first; the array is a well-understood follow-up optimization.*

### 4.5 Decision 2 — orders within a level: `std::list` (→ intrusive over pool)

A price level needs FIFO with arbitrary cancel:

| Operation | Why | `std::list` |
|---|---|---|
| Add to back | new orders arrive (newest = lowest time priority) | `push_back` — O(1) |
| Remove from front | oldest fills first (FIFO) | `pop_front` — O(1) |
| **Remove from middle (given iterator)** | a cancel targets any order, not just the oldest | `erase(it)` — O(1) |

The O(1) middle-erase is the critical property: given an iterator, `std::list::erase` just relinks neighbors, no search. Requires **doubly**-linked (need the `prev` neighbor to relink in O(1); singly-linked would be O(N) to find the predecessor). And `std::list` iterators **stay valid** across other insertions/erasures — essential because the order-ID index (Decision 3) *stores* these iterators. A `std::vector` would be O(N) middle-erase *and* invalidate stored iterators on every shift — disqualified.

**The HFT optimization — intrusive list over the object pool.** `std::list` is node-based: every `push_back` heap-allocates, every `erase` frees — millions of `new`/`delete` on the hot path, the exact landmine section 1's pool eliminates. The intrusive version makes the `Order` *be* the list node:

```cpp
struct Order {
    OrderId  id;
    Price    price;
    Quantity quantity;
    Side     side;
    Order*   prev;   // intrusive links — the order IS the node
    Order*   next;
};
```

The level holds `Order* head, * tail`; orders come from the **object pool** (section 1). `add`: pull from pool (O(1), no heap) + link (O(1)). `cancel`: unlink (O(1)) + return to pool (O(1)). Zero hot-path allocation, cache-friendly (pooled orders contiguous). **This is where the pool and the book compose** — the pool is the book's node allocator.

**Interview framing**: `std::list` for the correct version (O(1) push-back/pop-front/erase-given-iterator = FIFO-with-arbitrary-cancel); intrusive-over-pool for latency (order carries its own prev/next, lives in a pool, no per-node allocation). *We build the `std::list` version first.*

### 4.6 Decision 3 — cancel by ID: `unordered_map<OrderId, OrderLocation>`

A cancel arrives as just "cancel #12345." Need O(1) find of the order's node to unlink it. `std::unordered_map<OrderId, ...>` for the lookup — but the value must be more than a bare iterator.

**Why the iterator alone is insufficient**: `std::list::erase` is called *on a specific list object*. The iterator identifies the *node* but not *which list/level/side* it belongs to. And after erasing, if the level is now empty, you must remove the level from the `std::map` — which needs the price (map key) and side. So "location" needs:

```cpp
struct OrderLocation {
    Side  side;                          // which map (bids/asks)
    Price price;                         // which level (map key) — for erase + empty-level cleanup
    std::list<Order>::iterator it;       // which node — O(1) erase
};
std::unordered_map<OrderId, OrderLocation> order_index_;
```

**Cancel flow:**
1. `order_index_.find(id)` → `{side, price, it}` — O(1).
2. pick map by `side`; `map.find(price)` → `Level` — O(log N).
3. `level.orders.erase(it)` — O(1).
4. if `level.orders.empty()`: `map.erase(price)` — O(log N).
5. `order_index_.erase(id)` — O(1).

Cancel is **O(log N)** here (step 2's tree lookup), not O(1). In the flat-array book, step 2 becomes `levels[price]` — O(1) — making cancel truly O(1).

**Refinement — store `Level*` directly** to skip the map lookup on the common-case unlink:

```cpp
struct OrderLocation {
    Level* level;                        // direct — no map.find needed to erase
    std::list<Order>::iterator it;
    // keep price/side (or in Level) for empty-level cleanup
};
```

Then unlink is `location.level->orders.erase(location.it)` — O(1). Requires **stable `Level` addresses**: `std::map` nodes are stable across other map ops (node-based), so `&map[price]`'s element stays valid. ✓ Works with `std::map`; would *not* work if levels lived in a `std::vector` (reallocation invalidates pointers) — another point for node-based `std::map` despite its cache cost. *We start with `{side, price, iterator}` for clarity, then optimize to `Level*`.*

### 4.7 The composed architecture

```
order_index_ : unordered_map<OrderId, OrderLocation>   // O(1) find-by-ID (cancel)
                          │
                          ▼
bids_ : map<Price, Level, greater>    asks_ : map<Price, Level>   // sorted levels, best at begin()/rbegin()
                          │
                          ▼
Level { list<Order> orders; Quantity total_qty; ... }  // FIFO time priority, O(1) middle-erase
```

Each structure serves exactly one of the three access patterns:
- **`std::map` per side** → price priority + ordered iteration + best-price.
- **`std::list` per level** → time priority (FIFO) + O(1) cancel-given-iterator.
- **`unordered_map<OrderId, location>`** → O(1) find-by-ID.

Matching, adding, and crossing logic all operate on top of these three. The two optimization axes (map→flat-array, list→intrusive-pool) are independent and both exploit domain invariants (bounded discrete prices; recyclable fixed-size order nodes).

### 4.8 Interview talking points (design)

1. *"Three access patterns drive the design: best-price lookup (price priority), FIFO within a level (time priority), and find-by-ID (cancel). Each maps to one structure."*
2. *"Price levels: `std::map` per side, not `priority_queue`. `priority_queue` only exposes the top — it can't iterate non-destructively, can't remove from the middle, can't look up a price. The book needs all three. `std::map` gives O(1) best via begin/rbegin, ordered iteration, and O(log N) lookup/insert/erase."*
3. *"Within a level: `std::list` for O(1) push-back, pop-front, and erase-given-iterator — that's FIFO with arbitrary cancel. Doubly-linked is required for O(1) middle-erase, and stable iterators let me store them in the ID index."*
4. *"Cancel by ID: `unordered_map<OrderId, location>`, where location is side + price + list iterator — the iterator alone doesn't say which list to erase from, and I need price/side to remove an emptied level from the map."*
5. *"Latency optimizations exploit domain facts: prices are bounded discrete ticks, so a flat array indexed by tick replaces the tree (O(1), cache-friendly); and orders are fixed-size and recyclable, so an intrusive list over an object pool replaces `std::list` (zero hot-path allocation). I'd build the clean `std::map`/`std::list` version first, then specialize."*
6. *"Cancel is the most frequent operation — cancel-to-trade ratios are often 100:1 — so the design prioritizes O(1)/O(log N) cancel via the ID index, not just fast matching."*

### 4.9 Build plan (implementation stages, forthcoming)

1. **Types**: `Side` enum, `Price`/`Quantity`/`OrderId` typedefs (integers — money is never float), `Order`, `Level`, the book class skeleton with the three members.
2. **`add`**: the crossing check (does it match the opposite side?), matching loop (price-time priority), resting the remainder. The most complex operation.
3. **`cancel`**: the O(1)/O(log N) unlink + empty-level cleanup via the ID index.
4. **`match`** helper: fill against resting orders, generate fills/trades, handle partial fills and level exhaustion.
5. **Optimizations** (optional follow-ups): flat-array price index; intrusive list over the object pool; measurement/profiling.

---

## 5. Limit Order Book — Implementation (stages 2-4)

The built, tested implementation of the design in section 4. The core book — add (with matching), cancel — is functionally complete, kept in sync across the three structures, all green under AddressSanitizer + UBSanitizer. This section captures the implementation and, importantly, the **bugs encountered while building**, since they cluster into a few recurring C++ systems-programming traps worth internalizing.

### 5.1 The types (stage 1)

```cpp
enum class Side : std::uint8_t { Bid, Ask };       // scoped, 1 byte

using Price    = std::uint64_t;   // fixed-point (ticks / micro-cents) — money is integer, never float
using Quantity = std::uint32_t;   // up to ~4.2B per order
using OrderId  = std::uint64_t;   // unique system-wide

struct Order {
    OrderId  id;        // set-once by convention (no setter) — NOT const (const breaks assignability)
    Price    price;
    Quantity quantity;  // decreases on partial fills
    Side     side;
    Order(OrderId i, Price p, Quantity q, Side s) : id(i), price(p), quantity(q), side(s) {}
};

struct Level {
    std::list<Order> orders;       // FIFO time priority
    Quantity total_quantity = 0;   // running sum — O(1) "size at this level"
};

struct OrderLocation {             // value in order_index_
    Side  side;                    // which map
    Price price;                   // which level (map key) — for erase + empty-level cleanup
    std::list<Order>::iterator it; // which node — O(1) erase
};
```

**Lessons baked in here:**
- **`const` data members break value semantics.** Marking `id`/`side` `const` *deletes* the copy/move assignment operators (you can't assign to a `const` member), which breaks storage in containers and any assign path. Enforce immutability through the *interface* (no setter), not the storage qualifier. `const` members are almost always a mistake on a type you store or assign.
- **`enum class : uint8_t`** — scoped (no implicit int conversion, no namespace pollution) and explicitly 1 byte.
- **Integer money** — `Price`/`Quantity` are unsigned integers (fixed-point). Plain `using` aliases give raw performance but zero type-safety against mixing prices and quantities; strong typedefs (a wrapper struct per type) are a reasonable upgrade.
- **`Level` is a struct, not a bare `std::list` alias** — so it can carry `total_quantity` (and future per-level aggregates). Aliasing to `std::list` boxes you out of per-level state.
- **The book is non-copyable AND non-movable** (all four special members deleted). It owns cross-referencing state (iterators into per-level lists) that makes copying meaningless and moving a correctness hazard. It lives in one place for its lifetime; relocate via `unique_ptr<Orderbook>` if needed.
- **Comparators**: `bids_` uses `std::greater<Price>` (descending, best = highest at `begin()`); `asks_` uses default `less` (ascending, best = lowest at `begin()`). Both have their best price at `begin()`, making the matching code symmetric.

### 5.2 `add` and the matching loop (stage 2)

```cpp
void add(OrderId orderId, Price price, Quantity qty, Side side) {
    if (order_index_.count(orderId) > 0) return;     // duplicate-ID guard — keeps index/book in sync
    Order order(orderId, price, qty, side);
    std::vector<Fill> fills_produced;
    matchOrder(order, fills_produced);               // match against opposite side
    // TODO: surface fills to a consumer (callback / return)
    if (order.quantity > 0) restOrder(order);        // rest the unfilled remainder
}

void matchOrder(Order& order, std::vector<Fill>& fills_produced) {
    if (order.side == Side::Bid) {
        while (order.quantity > 0 && !asks_.empty()) {       // sweep multiple levels
            auto level_it = asks_.begin();
            if (order.price < level_it->first) break;        // no longer crosses — stop
            matchAtLevel(order, level_it->second, fills_produced);
            if (level_it->second.orders.empty()) asks_.erase(level_it);  // level exhausted
        }
    } else {
        while (order.quantity > 0 && !bids_.empty()) {
            auto level_it = bids_.begin();
            if (order.price > level_it->first) break;        // crossing test flips for asks
            matchAtLevel(order, level_it->second, fills_produced);
            if (level_it->second.orders.empty()) bids_.erase(level_it);
        }
    }
}

void matchAtLevel(Order& order, Level& level, std::vector<Fill>& fills_produced) {
    auto it = level.orders.begin();
    while (order.quantity > 0 && it != level.orders.end()) {
        Quantity matching_qty = std::min(order.quantity, it->quantity);
        order.quantity       -= matching_qty;
        it->quantity         -= matching_qty;
        level.total_quantity -= matching_qty;
        fills_produced.emplace_back(it->price, matching_qty, it->id, order.id);  // trade at RESTING price
        if (it->quantity == 0) {
            order_index_.erase(it->id);
            it = level.orders.erase(it);    // erase returns next — do NOT ++it
        } else {
            ++it;                           // partial fill — resting order survives
        }
    }
}
```

**Key design points:**
- **Matching is a loop over price levels**, not a single check — an aggressive order sweeps multiple levels (`begin()` repeatedly, erasing exhausted levels).
- **The crossing test flips by side**: incoming bid crosses asks if `price >= best_ask`; incoming ask crosses bids if `price <= best_bid`. Structure is symmetric (always `begin()`); the comparison operator differs.
- **Trades execute at the RESTING order's price** (`it->price`), not the aggressor's — the resting order set the price; the aggressor gets price improvement. Standard price-time-priority rule; using the aggressor's price is a common bug.
- **Fills accumulate into one caller-owned `std::vector<Fill>&`** threaded through, rather than each level returning a vector that gets merged — avoids per-level allocation.
- **`matchOrder`'s contract**: mutate `order.quantity` down to the unfilled remainder; `add` rests it if nonzero.

### 5.3 `cancel` (stage 3)

```cpp
bool cancel(OrderId id) {
    auto ord_it = order_index_.find(id);
    if (ord_it == order_index_.end()) return false;        // not-found path
    const OrderLocation& loc = ord_it->second;
    return loc.side == Side::Ask
        ? cancelFrom(asks_, id, loc.price, loc.it)
        : cancelFrom(bids_, id, loc.price, loc.it);
}

template <typename BookSide>
bool cancelFrom(BookSide& book, OrderId id, Price price, std::list<Order>::iterator it) {
    auto level_it = book.find(price);
    assert(level_it != book.end() && "index/book desync");  // invariant, not a runtime case
    Level& level = level_it->second;
    level.total_quantity -= it->quantity;   // READ quantity BEFORE erase
    level.orders.erase(it);                 // O(1) unlink via the stored iterator
    if (level.orders.empty()) book.erase(price);   // empty-level cleanup
    order_index_.erase(id);                 // leave BOTH structures
    return true;
}
```

**Key points:**
- **Read `it->quantity` before `erase(it)`** — once erased, the node is gone and reading it is UB.
- **Empty-level cleanup** — cancelling the last order at a price removes the level from the map; otherwise an empty `Level` lingers and corrupts best-price queries.
- **The order leaves both structures** — the level list *and* `order_index_`. The cross-structure sync invariant.
- **The iterator `it` is into the level's `std::list`, not into `order_index_`** — so `order_index_.erase(id)` does not invalidate it (different container). Being deliberate about *which container* an iterator points into is what keeps this safe.
- **`book.find(price)` can't fail if the invariant holds** (index says the order exists → the level exists). An `assert` documents this as an invariant rather than masking a desync bug with a silent `return false`.
- **`cancel`/`cancelFrom` mirror `add`/`insertInto`** — the templated helper over `BookSide` handles the two map types (different comparators) with one body. Clean symmetry.

### 5.4 The recurring bug taxonomy (the real lessons)

Across the build, the bugs clustered into a small number of C++ systems-programming traps. These are the things "compiles and looks right" misses and sanitizers catch:

1. **Pass/assign by value when you meant reference.** `matchAtLevel(Order order, Level level)` mutated *copies* — the real book was untouched (filled orders never left, the match loop never saw progress → infinite loop), and copying a whole `Level` (its `std::list`) per level match was a silent massive cost. `auto level = it->second` (copy) vs `auto& level = it->second` (reference) — the same trap. **`auto` copies; reach for `auto&`/`const auto&` to refer to the original.** In hot-path code that mutates containers, you almost always want the reference.
2. **Iterator invalidation.** `std::list::erase(it)` invalidates `it`; `++it` after is UB. The fix is the **erase-returns-next idiom**: `it = list.erase(it)`, and only `++it` on the non-erased branch. Burn this into muscle memory for erase-while-iterating.
3. **Dangling iterator from copy-then-store.** Building a local `Level`, copying it into the map (`map[price] = local`), then storing an iterator into the *local* — which dies — leaves a dangling iterator in the index. Fix: insert into the map *first* (`Level& level = map[price]`), then take the iterator from the list *that lives in the map*. **Take iterators from the object that will persist, never from a local you copy in.**
4. **Cross-structure sync invariant.** An order entering the book must enter *both* the level list and `order_index_` (and bump `total_quantity`); an order leaving must leave *both* (and drop `total_quantity`). Forgetting either half desyncs the structures → dangling index entries or uncancellable orders. The single most important book-wide invariant.
5. **Side mapping backwards.** A resting bid goes on `bids_`, an ask on `asks_` — easy to swap, and it corrupts everything. A one-line targeted test ("add a non-crossing bid, assert it's in `bids_`") catches it instantly.

The meta-lesson: **for systems C++, "compiles and looks right" is not "correct."** The bugs all live in references-vs-copies and iterator validity — exactly what a human reviewer misses and a sanitizer catches mechanically.

### 5.5 Testing approach

No framework needed to start: `assert` in a `main()`, compiled and run under sanitizers.

```bash
g++ -std=c++17 -Wall -Wextra -fsanitize=address,undefined test_orderbook.cpp -o test && ./test
```

- **`-fsanitize=address,undefined`** is the highest-leverage habit for C++ correctness — ASan catches use-after-free / dangling iterators / heap overflows at runtime with a report pointing at the exact line; UBSan catches signed overflow, misaligned access, etc. Several of the bugs above (dangling iterator especially) are caught instantly by ASan and missed by eyeballing.
- **Read-only accessors double as observability and test hooks**: `has_bid`/`has_ask`/`bid_qty`/`ask_qty`/`has_order` are both the market-data queries a real book exposes *and* the way tests observe private state. Design for observability and testability falls out together.
- **Test the invariants on both sides**: e.g. a resting bid → `has_bid(p) && !has_ask(p) && bid_qty(p) == q && has_order(id)`. Test empty-level cleanup explicitly (cancel the last order → `!has_ask(p)`), partial fills (resting remainder + index state), multi-level sweeps, and the not-found cancel path (returns false).
- **For production**: GoogleTest or Catch2 give test discovery, value-printing assertions (`EXPECT_EQ` shows both sides on failure), fixtures, and parameterized tests. Lock-free / concurrent components additionally want **ThreadSanitizer** (`-fsanitize=thread`) and high-iteration stress tests, since races are nondeterministic and can't be proven absent by a single run.

### 5.6 Status and remaining depth

The **core book is functionally complete and tested**: add (matching, partial fills, multi-level sweep, resting), cancel (empty-level cleanup), three structures kept in sync, integer prices, non-copyable/non-movable, green under sanitizers. The `match` helper (stage 4) was folded into the add path (matching happens *because* of an add) rather than built standalone; "generate fills" exists (`fills_produced`), only the surface-to-consumer callback is stubbed.

Remaining depth, in descending interview value:
1. **Surface fills** — wire the callback or return fills from `add`; test trade *output*, not just resulting book state.
2. **`modify`** — quantity decrease keeps time priority (decrement in place); price change or quantity increase loses it (cancel + re-add at the back). The priority-preservation logic is a classic interview discussion.
3. **Best-bid/ask + spread accessors** — `best_bid()`/`best_ask()`/`spread()`, trivial given `begin()`; the queries a strategy actually calls.
4. **Optimizations** — flat-array price index (exploits bounded discrete ticks → O(1), cache-friendly); intrusive list over the object pool (section 1 — zero hot-path allocation). Where the book composes with the pool and where Area-3 profiling/measurement lives. Biggest effort, biggest low-latency signal.

---

## 6. Profiling & Measurement (Area 3 — the order book baseline)

The "you can't optimize what you don't measure" discipline, applied to the order book before any optimization. This section captures the measurement *methodology* (the transferable skill) and the *baseline results* for the `std::map` + `std::list` book, which become the before-picture for the 4a (pool) and 4b (flat-array) optimizations.

### 6.1 Methodology — the principles that make a benchmark trustworthy

Bad benchmarks give confident wrong answers. Six principles:

1. **Measure realistic traffic, not a microbenchmark of one op.** A real book sees a mix dominated by add+cancel (cancel-to-trade ratios of 100:1), prices clustered around a moving mid (not uniform-random across the whole range, which destroys cache behavior artificially), some crossing orders. Benchmarking "10M adds at the same price" measures one degenerate path.
2. **Separate setup from the timed region.** Precompute the entire workload into a vector of operations *first*, then time only the replay. Generating random numbers inside the timed loop measures the RNG, not the book.
3. **Timing mechanics that don't lie**: warm up (one untimed run for steady-state caches/predictors/allocator); repeat and take the **minimum** (outliers are always slower — scheduler interference — never faster, so min is the truest intrinsic cost); use `steady_clock` (monotonic); prevent dead-code elimination with a `do_not_optimize` sink; report **ns/op**.
4. **Measure the right granularity**: ns/op is the *if-it-got-faster* number; perf/cachegrind counters (cache misses, branch misses per op) are the *why*. Both make the story.
5. **Build with optimization ON** (`-O2 -DNDEBUG`) for perf measurement — a `-O0` debug build is meaningless for speed. Run *correctness* tests separately under sanitizers (`-fsanitize=address,undefined`, which add huge overhead and must never be in the perf build). Two builds: sanitized-debug for correctness, optimized-release for speed.
6. **Fixed RNG seed** so the workload is identical across runs and across versions — essential for fair baseline-vs-optimized comparison.

### 6.2 The `do_not_optimize` idiom

If a benchmark's result is unused, the compiler may delete the whole loop. Force the compiler to assume the value is observed:

```cpp
template <typename T>
inline void do_not_optimize(T const& value) {
    asm volatile("" : : "r,m"(value) : "memory");
}
```

This is what Google Benchmark's `benchmark::DoNotOptimize` does under the hood. Signal that the loop was deleted: an implausibly small per-op time (sub-nanosecond) means the sink isn't biting.

### 6.3 The clock-overhead trap (a real measurement lesson)

`steady_clock::now()` is **not free** — measured at **~14 ns/read** on the test box (it's a function call, possibly a `vDSO` read). When you instrument at *per-operation* granularity (two clock reads per op to split add-time from cancel-time), that's **~28 ns of pure instrument overhead per op** — a first-order effect on a ~73 ns operation.

Consequences and the fix:
- **The blended number** (time the whole loop once, divide by op count — only two clock reads total) is the honest top-line. Zero per-op instrumentation overhead.
- **The per-kind split** (clock around each op) must **subtract `2 × clock_overhead`** from each measured time to be trusted. Measure the overhead explicitly (a tight loop of just clock reads) and subtract.
- **Sanity check**: the overhead-corrected split should reconcile with the blended number via the op mix. Baseline: blended 72.9; corrected add ~78, cancel ~59; `0.6×78 + 0.4×59 ≈ 70` ≈ blended. The reconciliation confirms both the split and the overhead correction are sound.

The deeper lesson: **the instrument's cost is part of what you measure.** On nanosecond-scale operations, a 14 ns clock read is not background noise — it's a measurable fraction of the signal. Instrument coarsely (blended) for the headline; instrument finely (per-op) only with explicit overhead correction.

### 6.4 Baseline results — `std::map` + `std::list` book

Workload: 2M operations, 60% adds / 40% cancels, prices ~N(10000, 20) ticks, cancels target live orders. Build: `g++ -std=c++17 -O2 -DNDEBUG`. (Hardware-specific; absolute numbers vary by box. The *relative* improvements from optimization are what transfer.)

| Metric | Value |
|---|---|
| **Blended** | **~73 ns/op** |
| add (overhead-corrected) | ~78 ns/op |
| cancel (overhead-corrected) | ~59 ns/op |

**Add > cancel** because an add may allocate a `std::list` node, may allocate a `std::map` node (new price level), and may run the matching loop; a cancel unlinks (no allocation) and occasionally frees a level. The allocation asymmetry is visible — the first evidence that the pool (4a) helps adds most.

### 6.5 The "why" — cachegrind diagnostics

WSL2's virtualized kernel does not expose the PMU hardware counters (`perf stat` returns `<not supported>` for cycles/cache/branch). Fallback: **`valgrind --tool=cachegrind --cache-sim=yes --branch-sim=yes`**, which *simulates* a cache+branch model and counts misses. Run at reduced N (~100-200k; cachegrind is 20-50x slower, and miss *rates* are size-stable). **Never read timing from a cachegrind run** — it instruments every instruction (the clock overhead exploded to ~313 ns/read under it), so its per-op time is meaningless; cachegrind is for *counts*, not *time*.

Baseline diagnostics:

| Counter | Value | Interpretation |
|---|---|---|
| **Branch mispredict rate** | **9.0%** | **High** (well-predicted code is <1-2%). Dominated by `std::map` tree comparisons (data-dependent branches the predictor can't guess) and matching-loop conditionals. Each mispredict ≈ 15-20 cycle pipeline flush. |
| D1 (L1) miss rate | 2.1% | ~121M L1 misses — pointer-chasing through node-based structures. |
| LLd (last-level) miss rate | 0.3% | ~18M main-memory misses ≈ **9 LL misses/op** — scattered `std::list`/`std::map` nodes (each a separate allocation). The expensive misses (~100+ cycles). |
| I1 / LLi (instruction) | 0.00% | Code is tiny and hot — no instruction-fetch problem. Cost is all data access + branching. |

**Caveat**: cachegrind approximated the real cache geometry (it warned the LL assoc differed from the hardware). Treat *absolute* miss rates as approximate; the *relative* baseline-vs-optimized comparison (same simulated cache) is apples-to-apples and is what the optimization story uses.

### 6.6 Evidence-based optimization plan

The profiler points at three costs, and the two planned optimizations attack exactly those:

1. **Branch mispredictions (9%, from `std::map` tree traversal)** → **flat-array index (4b)**: array indexing has no data-dependent branching, should slash this.
2. **Cache misses (~9 LL/op, from node-based pointer-chasing)** → **object pool (4a)** (contiguous order storage) **+ flat-array (4b)** (contiguous levels).
3. **Allocation on add (the add>cancel asymmetry)** → **object pool (4a)**: eliminates the per-`add` `std::list` node allocation.

**Stated hypotheses before optimizing** (good discipline — if measurement contradicts the hypothesis, the diagnosis was wrong and that's a finding):
- The pool (4a) should reduce *add* latency most (kills `std::list` node allocation) and reduce LL misses (pooled orders contiguous), but **probably won't help branch mispredictions** — those are mostly `std::map`, which 4a doesn't touch.
- The flat array (4b) should crush the branch-mispredict rate and reduce both miss rates.

This is the entire point of measuring first: the optimizations target *measured* costs, not guesses — and an earlier guess in this build was wrong (the fills vector was assumed costly; the real cost was a by-value `Level` copy), which is exactly why intuition isn't trusted here.

### 6.7 Reusable benchmark-harness shape

```cpp
struct Op { enum Kind { Add, Cancel } kind; OrderId id; Price price; Quantity qty; Side side; };

std::vector<Op> generate_workload(std::size_t n);   // fixed seed, realistic mix, prices ~N(mid,sigma)

double run_once(const std::vector<Op>& ops) {       // blended — two clock reads total
    Orderbook book; auto noop = [](const Fill&){};
    auto t0 = std::chrono::steady_clock::now();
    for (const auto& op : ops) { /* dispatch add/cancel; do_not_optimize(result) */ }
    auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::nano>(t1 - t0).count();
}

int main() {
    auto ops = generate_workload(2'000'000);
    run_once(ops);                                  // warmup, untimed
    double best = 1e18;
    for (int r = 0; r < 7; ++r) best = std::min(best, run_once(ops));   // min over repeats
    // report best / N as ns/op
}
```

Commands:
- Speed: `g++ -std=c++17 -O2 -DNDEBUG benchmark.cpp -o bench && ./bench`
- Cache/branch (WSL fallback): `valgrind --tool=cachegrind --cache-sim=yes --branch-sim=yes ./bench` (reduce N), then `cg_annotate cachegrind.out.<pid>` for per-function attribution.
- Native Linux with PMU: `perf stat -e cycles,instructions,cache-references,cache-misses,branch-misses ./bench`.

---

## 7. Optimization 4a — Intrusive List over Object Pool (results)

The order book's per-level storage changed from `std::list<Order>` (heap-allocated node per order) to an **intrusive doubly-linked list over an `ObjectPool<Order>`** (section 1). This is the composition the curriculum was building toward: the pool is the book's node allocator. The change was a pure implementation swap — all 17 tests passed unchanged under ASan/UBSan, confirming identical observable behavior.

### 7.1 The change

- **`Order` gains `prev`/`next`** — the order *is* the list node (intrusive). No separate node type, no per-order node allocation.
- **`Level` becomes `{Order* head; Order* tail; Quantity total_quantity;}`** — head/tail of an intrusive FIFO list, instead of holding a `std::list`.
- **`OrderLocation` stores `Order*`** instead of a `std::list::iterator`.
- **Orders allocated from `ObjectPool<Order>`** — `restOrder` calls `pool_.allocate(...)`; `cancel`/fills call `pool_.deallocate(...)`. Zero heap traffic on the hot path.
- Pool allocation happens **only in `restOrder`** (the resting remainder), not during matching — if matching fully consumes the incoming order, zero allocations occur. Matching consumes already-pooled resting orders and deallocates them.

### 7.2 Pointer-surgery lessons (the build)

- **Intrusive append** (insert at tail): empty level → head = tail = node; non-empty → `tail->next = node; node->prev = tail; tail = node`. `node->next` defaults to nullptr (member initializer).
- **Head-removal** (matching consumes from head): advance `head = node->next`; if new head is nullptr, set `tail = nullptr` (level empty); else `head->prev = nullptr` (new head has no predecessor). Both must be handled or a pointer dangles.
- **General unlink** (cancel targets any node) — the four-case version, made safe by an `else if` chain that guarantees mutual exclusivity (so the head case knows `next != nullptr` and the deref `head->prev` is safe):
  - only element (`prev==null && next==null`) → head = tail = null
  - head (`prev==null`) → head advances, new head's prev cleared
  - tail (`next==null`) → tail retreats, new tail's next cleared
  - middle → **fix the neighbors' pointers** (`prev->next = next; next->prev = prev`), NOT the node's own
- **The recurring rule**: when unlinking, fix every pointer that referenced the node (predecessor's `next`/head, successor's `prev`/tail), and never dereference a pointer you haven't confirmed non-null. The two-independent-checks idiom (`if (prev) prev->next = next; else head = next;` / `if (next) next->prev = prev; else tail = prev;`) handles all four cases without enumerating them and never dereferences unchecked — arguably safer than four explicit branches, though the explicit version is fine with a correct `else if` chain.
- **Read-before-free**: read `node->quantity` (for the `total_quantity` decrement) *before* `pool_.deallocate(node)`, which destroys the order.
- **Trivially-destructible payoff**: live pooled orders are not destructed on book teardown (the pool's union slots have trivial destructors), but `Order` owns no heap resources, so `~Orderbook() = default` is leak-safe. Would leak if `Order` ever held an owning member.
- The `else` after a partial resting fill is `break`, not advance — a partial resting fill means the incoming order is exhausted, so matching stops (specialization of the general erase-while-iterating idiom).

### 7.3 Results vs baseline

Same workload, same seed, same `-O2 -DNDEBUG` build; only `Orderbook.h` changed. Clock overhead this run ~15 ns/read (subtract ~30 ns from split numbers).

| Metric | Baseline (`std::map`+`std::list`) | 4a (pooled intrusive) | Change |
|---|---|---|---|
| **Blended** | ~73 ns/op | **~62 ns/op** | **−15%** |
| add (corrected) | ~78 ns | ~60 ns | **−23%** |
| cancel (corrected) | ~59 ns | ~46 ns | **−22%** |

Cachegrind (simulated; reduced N):

| Counter | Baseline | 4a | Change |
|---|---|---|---|
| Branch mispredict | 9.0% | 9.9% | ~flat (slightly up) |
| D1 miss rate | 2.1% | 2.5% | up |
| LLd misses (absolute) | 18.2M | **30.2M** | **UP 66%** |

### 7.4 The surprise — misses went UP, time went DOWN (the real lesson)

The hypothesis (section 6.6) was that the pool would *reduce* cache misses via contiguity. **It did the opposite — LL misses rose 66% — yet wall-clock time dropped 15%.** Both are true, and the reconciliation is the insight:

- **A pool gives contiguity of *storage*, not locality of *access*.** Orders are handed out from the free list in whatever order slots are freed and reused, so two orders adjacent in a price-level's FIFO list (accessed together during matching) can live far apart in the pool's array. The intrusive list still chases `next` pointers, now across a huge region (a 1M-slot pool of ~40-byte orders ≈ 40MB, far larger than the ~18MB LL cache). The `std::list` baseline's allocator often returned recently-freed (cache-hot) memory, giving *accidental* temporal locality that the pool's free-list reuse destroyed.
- **Allocation cost dominated cache cost on this workload.** A `malloc`/`free` round-trip (~15-50 ns) is a bigger, more reliable cost than the marginal extra misses — and real hardware (prefetchers, out-of-order execution, memory-level parallelism) hides much of the miss latency that cachegrind's idealized model counts. So eliminating allocation won (−15%) even though locality worsened.
- **ns/op (real hardware) is the truth; cachegrind misses (simulation) are the diagnostic.** Here they diverge — the simulation counts more misses, the real chip still runs faster. Trust the wall clock for "did it get faster"; use the miss counts to understand *why*, not as a proxy for time.

**Hypothesis scorecard:**
- ✅ "Adds improve most" — correct (~23%, biggest mover; allocation eliminated).
- ✅ "Branches barely move" — correct (9.0% → 9.9%; `std::map` untouched).
- ❌ "LL misses drop" — **wrong, instructively.** Rose 66%. The model "pool → contiguous → fewer misses" was naive: storage contiguity ≠ access locality; locality depends on the free-list reuse pattern, which a pool does not control.

Getting a hypothesis wrong via measurement is the most valuable outcome — it corrects a real misconception. **The correct interview claim is not "I used a pool so cache misses went down" (false, and a sharp interviewer would push) but "the pool eliminated hot-path allocation, which dominated; cache misses actually rose because free-list reuse doesn't preserve access locality, but the allocation win was larger."** That answer demonstrates measurement, mechanism understanding, and resistance to folklore.

### 7.5 What this points at next

- Branch mispredictions still ~10%, untouched — that is `std::map`'s tree traversal, the target of **4b (flat-array price index)** (array indexing has no data-dependent branches).
- The worsened access locality is also a 4b target (contiguous level array vs scattered tree nodes) and suggests a further refinement: a locality-aware reuse strategy or per-level slab allocation so FIFO-adjacent orders are address-adjacent.
- The measurement *pointed at the next problem* — exactly the purpose of measuring between optimizations rather than batching them.

---

## 8. Optimization 4b — Flat-Array Price Index (design & decision not to build)

The branch-misprediction rate stayed at ~10% after 4a (section 7.3) — that is `std::map`'s red-black-tree traversal (every node comparison is a data-dependent branch the predictor can't guess). 4b would replace `std::map<Price, Level>` per side with a **flat array indexed by tick price** (`levels[price - kMinTick] → Level`), giving O(1) branch-free access. This section documents the design *and the reasoning for choosing not to build it* — because understanding a specialization's domain of applicability, and declining to apply it prematurely, is itself the senior decision.

### 8.1 The design (what it would be)

- Two `std::vector<Level>` (bids, asks), each sized to a bounded price band `[kMinTick, kMaxTick]`; index = `price - kMinTick`.
- The pool + intrusive list from 4a stay unchanged — 4b swaps only the *level container*, not per-order storage. The two optimizations are orthogonal.
- **What you lose vs `std::map`, and must rebuild by hand:**
  1. *Best price at `begin()`* — an array has no "bestness" ordering. Track `best_bid_idx_`/`best_ask_idx_` explicitly, updated on every op (O(1) compare-and-maybe-update on rest; scan toward worse prices when the best level empties).
  2. *`++it` to next-best* — when the best empties, scan to the next occupied level (occupancy = `level.head != nullptr`; no separate bitmap needed for correctness, though one would speed the scan).
- `OrderLocation` stays `{side, price, Order*}`, but reaching the level becomes `levels[price - kMinTick]` (O(1)) instead of `map.find(price)` (O(log N)) — a side win for cancel/modify.
- **Hypothesis (untested, since not built):** should crush the ~10% branch rate (array indexing has no data-dependent branches) and improve level-access locality (contiguous array vs scattered tree nodes), though the 4a order-node locality issue would persist.

### 8.2 The flat array's two real weaknesses

1. **Memory ∝ price *range*, not active levels.** A book with 50 active levels spanning a range that forces a 20,000-element array is 99.7% empty. Trivial for one instrument (~1MB); but a market-making system runs books for *thousands* of instruments — thousands of mostly-empty arrays, gigabytes of cold memory polluting cache and TLB. The per-book "trivial" cost becomes death by a thousand cuts across the portfolio.
2. **Requires knowing the price range ahead of time.** `[kMinTick, kMaxTick]` is fixed at construction. Limit-up/limit-down days, a stock that 10x's, corporate actions, or a mis-guessed range for a new instrument all break it — you over-provision (worsening #1) or risk rejecting valid orders / crashing (if unguarded). A structure that requires *predicting* the price range is operationally fragile.

(Out-of-range *crashes* are a non-issue — a one-line bounds check on the index handles them. The alternatives below address *memory/range*, not crashes.)

### 8.3 Alternatives and when each applies

- **`std::map` / tree / skip-list** (what the book currently uses): O(log N), but unbounded range, memory ∝ *active* levels, no range-prediction fragility. Good enough for wide/unpredictable ranges, many books, or non-hottest paths. A lot of production books are tree-based for exactly these reasons.
- **Sliding-window banded array**: array covers `[mid − N, mid + N]`, re-centers as the mid moves. Keeps memory ∝ active band. Cost: re-centering is expensive and fiddly (shift/remap levels, fix every price-derived index, periodic O(band) cost). A "if memory became a problem" escalation, not a first cut.
- **Two-tier / radix structure**: split price into high/low bits; top array → second-level chunks, allocating only populated chunks. Memory ∝ occupied regions — the answer for huge/sparse ranges. But *still O(1)* only in the constant sense: two dependent loads (extra potential cache miss, a data dependency), and the multi-level next-best scan **partially reintroduces the branching** the flat array was meant to eliminate — so it may not win on the very metric (branch mispredict) that motivated leaving `std::map`. Measure before adopting.
- **Hybrid (common in practice)**: flat array for a *window around the touch* (the few hundred ticks where ~all activity is, accessed constantly) + a tree/hash fallback for far-from-touch levels. Captures the array's speed where it matters without provisioning the full range. Best-of-both, most complex — build only when measurement says the simple approaches aren't enough.

### 8.4 Why the flat array is a *specialization*, not a universal upgrade

The flat array trades generality and memory for speed on a specific favorable case: **a single hot instrument, with a stable narrow tick range, on the hottest path.** It is the right call there (the fastest single-instrument engines do use price-indexed arrays). It is the *wrong* default for a many-instrument system or wide/unpredictable ranges.

The strong, conditional framing (interview-ready): *"For a single hot instrument with a stable narrow tick range, a price-indexed flat array gives O(1) branch-free access, worth the sparse memory. But it costs memory ∝ price range, requires knowing the range ahead of time, and scales badly across many books — so for wide/unpredictable ranges or many instruments I'd use a tree, or a hybrid: a flat array windowed around the touch with a tree fallback. Driven by instrument count, range stability, and whether this is the hottest path. I'd measure before committing."* This demonstrates the flat array is a *tool with a domain of applicability*, which is more valuable than the implementation.

### 8.5 The decision: not built, and why that's the senior move

For *this* workload (single instrument, prices clustered in a tight band around 10000), the flat array would be the *correct* specialization — the two weaknesses don't bind. So building it would be legitimate and would likely confirm the branch-mispredict win.

It was deliberately **not built**, for a reason that is itself the lesson: **once the specialization's domain is understood, declining to apply it prematurely — when the current tree-based book is correct, tested, and fast enough, and when the specialization's benefits (memory) don't address a binding constraint while its risks (range fragility, many-instrument cost) are real — is the disciplined choice.** The profiling arc's entire point is "optimize for the constraint that binds, measured, not the one that doesn't." 4a was built because allocation was a *measured* hot-path cost with a clean, general win. 4b's branch-mispredict cost is real but the flat-array remedy is a narrow specialization whose downsides outweigh the gain outside its favorable domain — so the senior decision is to understand it, document the tradeoff, and stop.

The transferable takeaway: **knowing when *not* to optimize, and being able to articulate the tradeoff precisely, is as much a senior skill as the optimization itself.**

---

## 9. Threaded Pipeline — Integration Design (feed → match → report)

The capstone that composes everything: the SPSC queue (section 3), the order book (sections 4-7), and the object pool (section 1) assembled into a multi-threaded trading pipeline. This is *systems integration* — it surfaces concerns that exist only at the system level, not in any single component. Documented as design before building.

### 9.1 The architectural principle

**Decompose the work into single-threaded stages connected by SPSC queues. Then no component needs internal locking — the only concurrency is the handoff between stages, which is the lock-free queue's job.**

The order book is single-threaded. The object pool is single-threaded. Neither has an atomic or mutex anywhere — and that is *correct*, because each lives entirely on one thread. The only cross-thread interaction is passing data stage-to-stage, handled lock-free by the SPSC queue. The elegance: a fully concurrent system where every component is internally sequential and lock-free, because concurrency is confined to the seams. This is also why **SPSC** (not MPMC) is the right primitive — a linear pipeline has exactly one producer and one consumer per queue.

### 9.2 Topology

```
   [Feed thread]            [Matching thread]              [Reporting thread]
   parse input          pop Command from inbound        pop Fill from outbound
   build Command        dispatch add/cancel/modify       update P&L / risk
   inbound.push(cmd) ──► (pool allocation happens HERE)  log / persist
                         on_fill → outbound.push(fill) ─►
   core 0 (pinned)       core 1 (pinned)                 core 2 (pinned)

   inbound  : SPSCQueue<Command, N>   (feed produces, matching consumes)
   outbound : SPSCQueue<Fill, M>      (matching produces, reporting consumes)
```

Three threads, two queues, each strictly one-producer-one-consumer. The book, pool, and all state live on the **matching thread** (middle stage). Feed and reporting threads never touch the book directly — only push/pop through the queues.

### 9.3 The crux — ownership, and why commands flow as values

The decision everything depends on. The naive idea — "feed thread allocates an `Order` from the pool and pushes a *pointer* through the queue" — is **wrong**:
- The pool lives on the matching thread. If the feed thread allocated from it, the pool would be touched by two threads → needs to be thread-safe (atomics/lock) → reintroduces the concurrency we confined.
- Cross-thread *lifetime* gets nasty: who frees an order allocated on one thread, consumed on another?

The fix: **the inbound queue carries `Command` *values*, not pre-allocated orders.** A command is a trivially-copyable POD describing intent:

```cpp
struct Command {
    enum Type : std::uint8_t { Add, Cancel, Modify, Shutdown } type;
    OrderId  id;
    Price    price;     // unused for Cancel
    Quantity qty;       // unused for Cancel
    Side     side;      // unused for Cancel/Modify (looked up)
};
```

It flows through the SPSC queue by value. The matching thread pops it and calls `book.add(cmd.id, ...)` — **the pool allocation happens inside `book.add`, on the matching thread, which owns the pool.** Pool stays single-threaded; no cross-thread allocation or lifetime. Same outbound: `Fill` is a POD, `SPSCQueue<Fill, M>` carries fills by value, and the book's `on_fill` callback (built in section 5's depth point 1) becomes the seam:

```cpp
auto on_fill = [&outbound](const Fill& f) {
    while (!outbound.push(f)) { _mm_pause(); }   // never drop a fill — spin
};
```

The test version pushed fills to a vector; production pushes to the outbound queue. **Same hook — the callback was designed queue-shaped on purpose.**

**The generalizable principle**: *pass values (commands, fills) across thread boundaries, and let each thread own its own allocator.* Passing pointers across threads forces shared ownership and a thread-safe allocator; passing values keeps allocation thread-local. This is *why* the queues carry PODs by value, not pointers — a deliberate choice, not a default.

(The SPSC queue *is* the section-2 ring buffer plus atomic indices, so "using the queue" *is* using the ring buffer in its thread-safe form. The single-threaded ring buffer was the stepping stone.)

### 9.4 The stage loop — busy-poll, no blocking

```cpp
void matching_loop(SPSCQueue<Command,N>& inbound, SPSCQueue<Fill,M>& outbound,
                   std::atomic<bool>& feed_done) {
    Orderbook book(1'000'000);
    auto on_fill = [&outbound](const Fill& f) { while (!outbound.push(f)) _mm_pause(); };
    Command cmd;
    for (;;) {
        if (inbound.pop(cmd)) {
            if (cmd.type == Command::Shutdown) break;
            switch (cmd.type) {
                case Command::Add:    book.add(cmd.id, cmd.price, cmd.qty, cmd.side, on_fill); break;
                case Command::Cancel: book.cancel(cmd.id); break;
                case Command::Modify: book.modify(cmd.id, cmd.price, cmd.qty, on_fill); break;
                default: break;
            }
        } else if (feed_done.load(std::memory_order_acquire)) {
            break;                 // feed stopped AND queue drained → done
        } else {
            _mm_pause();           // queue empty — spin hint, keep polling
        }
    }
}
```

**Busy-spin, not block.** On an empty queue, spin with `_mm_pause()` (the x86 `PAUSE` hint, section 3) rather than sleeping — burns a core at 100% but eliminates wakeup latency. The HFT trade: a dedicated core per stage, spinning, for lowest deterministic latency. A latency-insensitive system would block on a condition variable instead (giving the core back, at the cost of a lock + wakeup latency).

### 9.5 Backpressure — the input/output asymmetry

The SPSC queue is bounded; `push` returns `false` when full. What the producer does differs by *which* queue — and the asymmetry is a real design point driven by what the data *means*:

- **Inbound full** (feed faster than matching): dropping a command means an order never placed — bad but *recoverable* (as if never sent; log + alert). You can't backpressure a market-data feed (the exchange sends at its rate), so size for bursts and drop-with-alert as last resort.
- **Outbound full** (matching faster than reporting): dropping a `Fill` means **losing the record of a trade that actually happened** — *unacceptable* (a real economic event). The outbound producer **never drops** — it spin-waits until there is room, applying backpressure *backward* into the matching thread.

**Inbound may drop (with alerting); outbound must never drop.** Recoverable-input-loss vs unacceptable-output-loss drives the different full-queue policies — understanding the *data*, not just the mechanics.

### 9.6 Ordered shutdown — draining without losing in-flight work

Stopping cleanly is harder than starting; you must not lose in-flight work. The order matters:

1. **Stop the feed first** — no new commands enter. The feed pushes a **poison pill** (`Command::Shutdown`) as its last message, then sets `feed_done`. The pill rides the queue *in order*, so it arrives after all real commands.
2. **Matching drains inbound** — processes everything up to the pill (or until `feed_done` AND the queue is empty), then exits. The real termination condition is "feed stopped AND inbound empty," not a bare flag.
3. **Reporting drains outbound** — exits only after matching has finished AND the outbound queue is empty.

The poison pill + ordered drain prevents the classic bug of killing a thread while it still has unprocessed messages. The `join`s in `main` happen in stage order (feed, then matching, then reporting).

### 9.7 Thread affinity

Pin each thread to a dedicated core (`pthread_setaffinity_np` on Linux) so it is not migrated (which would cold its cache), does not share a core with another spinning thread, and keeps its working set hot. Combined with busy-spin: the "core per stage" model — each stage owns a core, spins on its input, never sleeps. Burns three cores at 100%, the deliberate trade for deterministic low latency. In production you would also isolate those cores from the OS scheduler (`isolcpus`/`cpuset`).

### 9.8 What is genuinely new (not in any component)

The components were each internally complete; integration surfaces system-level concerns:

1. **Ownership across threads** — solved by passing values, keeping each allocator thread-local. (The single biggest decision.)
2. **Backpressure policy** — the input-vs-output asymmetry (9.5).
3. **Ordered shutdown / draining** — not losing in-flight work (9.6).
4. **Thread affinity and the spin-vs-block latency trade** (9.4, 9.7).
5. **End-to-end tick-to-trade latency** — time from a command entering inbound to its fill landing in outbound. The number a trading firm actually cares about, which no single-component benchmark captures.

### 9.9 Build plan and effort

Staging (each stage tested before the next):
1. `Command` type + the two typed queue instances. (Trivial.)
2. **Matching loop driven single-threaded** (one thread pushes commands, no real threads). Verifies the loop *logic* deterministically with the existing book-test assertion style — so any failure after step 3 is *definitely* a concurrency bug, not logic. The de-risking move: isolate the hard part.
3. Add real threads + affinity + spin loops.
4. Shutdown / draining (highest correctness risk).
5. Tick-to-trade latency measurement (reuses the section-6 harness patterns).

**Effort reality**: ~150 lines of new code (mostly assembly of done components), but the effort splits ~30% code / ~70% concurrency verification — the inverse of the order book. Concurrency bugs are nondeterministic, so correctness needs **ThreadSanitizer** (`-fsanitize=thread`, a separate build from ASan — it catches data races, different from ASan's use-after-free) plus **high-iteration stress tests** asserting every command produced its expected fills and nothing was lost/duplicated across the shutdown boundary. A single clean run proves nothing for concurrent code. The shutdown-race tests are the long pole.

The transferable lesson: **assembling correct components into a correct *system* is dominated by verifying the *interactions*, not the parts** — which is why the effort inverts from code-heavy to test-heavy.

### 9.10 Build results (stages 1-3) and the shutdown-race bug

Stages 1-3 built and verified. Stage 2 (matching loop, single-threaded) passed deterministically under ASan/UBSan — three tests covering dispatch routing, the `on_fill`→outbound seam, and both exit paths (poison pill, feed-done-drain). Stage 3 (three real threads) initially **failed intermittently** under TSan — and the failure is the most instructive moment of the whole concurrency arc.

**The failure**: `test_threaded_many_fills` (1000 resting asks swept by one bid → expect 1000 fills) passed ~115 reps, then `count == 1000` failed. A single run would have passed and shipped a broken pipeline. **This is why the test loops 200 (then 1000) times** — concurrency bugs are nondeterministic; "ran once and passed" is not evidence of correctness for concurrent code.

**Not a data race**: TSan stayed *silent* — no unsynchronized memory access. The queue's release/acquire atomics were correct. The bug was a **protocol race**: correct synchronization, but an algorithm that drops work under a legal interleaving. Data race ≠ protocol race; TSan catches the former, only logic + repetition catches the latter.

**The bug — the canonical concurrent-queue shutdown gap.** The reporting consumer's exit was:
```cpp
if (outbound.pop(f)) { count++; }
else if (matching_done.load(acquire)) break;   // BUG: breaks with fills still queued
```
Lossy interleaving:
1. Reporting calls `pop()` → false (last fill not yet pushed).
2. Matching pushes the last fill, sets `matching_done = true`, exits.
3. Reporting checks `matching_done` → true → **breaks, last fill still in the queue, never popped.**

The "check-empty THEN check-done" sequence has a **gap** between the two checks where work can arrive. The `pop()` happened *before* the final push; observing `done` afterward doesn't retroactively drain it.

**The fix — two patterns, chosen by whether the channel has an in-band terminator:**

- **Reporting (no in-band terminator — fills carry no "last fill" marker):** must use the `matching_done` flag + **drain-before-break**. After observing the flag, drain the queue completely before exiting. Correct because `matching_done` is set with `release` *after* all pushes, so an `acquire`-load observing it guarantees every push is visible to the final drain:
```cpp
} else if (matching_done.load(acquire)) {
    while (outbound.pop(f)) { fill_count++; qty_sum += f.qty; }   // drain the gap
    break;
}
```
- **Matching (has an in-band terminator — the poison pill):** simpler — rely on the pill, which rides the queue in FIFO order behind all real commands and is *always* pushed last. Loop, dispatch, break only on `Shutdown`. No flag check in the exit path → no gap → no race. The `feed_done` flag becomes a redundant backstop (can be removed).

**The asymmetry is the lesson**: when a channel has an in-band terminator (a sentinel that rides the queue in order), use it — it has no flag-race gap. When it doesn't, the out-of-band done-flag needs drain-before-break to close the gap between the empty-check and the done-check. The reporting channel carries only fills (no terminator), so it needs the flag+drain; the command channel has the pill, so it doesn't.

**Verification**: after the fix, 200 reps clean, then 1000 reps clean under TSan — at iteration counts well past the ~115 where the original failed. 

**The transferable takeaways:**
1. For concurrent code, correctness evidence is "passed N thousand reps under TSan," never "passed once." The rare interleaving is the whole danger.
2. A *protocol* race (drops work under a legal interleaving) is distinct from a *data* race (unsynchronized memory). TSan catches data races; only high-iteration stress testing catches protocol races.
3. The canonical concurrent-queue shutdown bug: "check-empty then check-done" has a gap. Fix = drain-after-observing-done (out-of-band flag) OR an in-band terminator that rides the queue (poison pill). The release-after-all-writes / acquire-before-reads pairing is what makes the final drain provably complete.

### 9.11 Stages 4-5 — edge-case tests and tick-to-trade latency

**Stage 4 (shutdown/draining edge cases)** — five targeted tests beyond stage 3's basics, all clean at 200 reps under TSan and ASan:
- *empty workload* (only the pill flows — degenerate clean shutdown),
- *all rest, no fills* (every command processed, zero outbound traffic — does reporting still exit? yes, via matching_done + empty drain),
- *all cancels* (index churn, zero fills),
- *mixed command types* (all four paths through the threaded pipeline in one workload),
- *shutdown under outbound pressure* (2000 fills into a capacity-**16** outbound queue, so matching's on_fill spins on a full queue right up to shutdown — the targeted stress of the drain-before-break path, the exact protocol the stage-3 bug lived in). This one reliably exercises "tail of fills in-flight at shutdown," far more than large-queue tests.

The `feed_done` flag was removed entirely in the final version: once matching breaks only on the in-band poison pill (which always arrives last), nothing reads `feed_done`. Inbound uses the pill (no flag); outbound uses `matching_done` + drain (no in-band terminator). The asymmetry, fully realized.

**Stage 5 (tick-to-trade latency)** — time from feed pushing a command to reporting popping its fill. Design points:
- *Split-ownership timestamping*: feed writes `send_ts[id]`, reporting writes `recv_ts[aggressor_id]` (first fill per aggressor). Different arrays, one writer each, read by main after join — no shared writes, no atomics needed (join is the sync point).
- *Pacing* (validity-critical): the feed busy-waits ~500ns between sends so the pipeline stays unloaded. Without pacing, dumping the whole workload makes later commands queue behind earlier ones → you measure *queueing delay*, not per-order tick-to-trade. Pacing isolates single-order transit + matching.
- *Affinity* (best-effort `pthread_setaffinity_np`): pin the three stages to cores 1/2/3, core 0 for the OS. Reduces migration jitter.
- *Percentiles* min/p50/p90/p99/p99.9/max — in latency work the **tail** matters more than the median.

**Results (`-O2`, no sanitizers — sanitizer latency tables are pure instrumentation overhead, discarded):**

| pct | latency | reading |
|---|---|---|
| min | 264 ns | the mechanism's floor |
| p50 | 394 ns | **the real tick-to-trade** |
| p90 | 68 µs | scheduler starting to bite |
| p99 | 4.4 ms | scheduler preemption (not the code) |
| max | 4.9 ms | worst preemption |

**Interpretation — the bimodal tell.** Median ~400ns is excellent and believable: feed timestamp → inbound queue → matching pop → `book.add` (match + on_fill) → outbound queue → reporting pop → timestamp, through two lock-free queues plus a match, in ~370ns (minus the ~28ns of two `now_ns()` reads). That is the number that matters — **the mechanism is fast.**

The tail (p99 = 4.4ms vs p50 = 394ns, an 11,000x spread) is a **measurement artifact, not the pipeline**: it is the OS scheduler. The design assumes busy-spin on *isolated* cores, but WSL's virtualized scheduler does not honor true core isolation, so the spinning threads get periodically preempted for a scheduler quantum (several ms). When matching is descheduled mid-pipeline, the in-flight order waits → multi-ms latency. The distribution is **bimodal** (~400ns when uninterrupted, ~4ms when preempted, nothing between) — the signature of scheduler preemption, not algorithmic cost.

**The lesson (interview-grade):** busy-spin + affinity delivers low *tail* latency only if the cores are genuinely isolated from the OS scheduler (`isolcpus`, real pinning, kernel-bypass). On a shared/virtualized box, the median shows the mechanism is fast but the scheduler owns the tail. This is *why* HFT shops isolate cores and bypass the kernel — without it, p50 is great and p99 is unusable. The measurement captured exactly that phenomenon: a fast median proving the design, a blown tail proving the environment can't protect it. On bare-metal isolated cores the tail would tighten to low microseconds. The transferable results are the *methodology* (split-ownership timestamping, pacing for validity, percentile/tail focus) and the *median* — not the environment-bound tail.

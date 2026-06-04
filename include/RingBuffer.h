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
    alignas(kCacheLine) std::size_t head_ = 0;   // next position to read (pop) — consumer writes
    alignas(kCacheLine) std::size_t tail_ = 0;   // next position to write (push) — producer writes
};
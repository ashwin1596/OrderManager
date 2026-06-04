#pragma once

#include <cstddef>
#include <vector>
#include <new>
#include <utility>
#include <cassert>
#include <cstdint>

template <typename T>
class ObjectPool{
private:

	// A slot is either a live T or a pointer to the next free slot.
	// Union enforces "one or the other, never both" - the intrusive free list invariant
	union Slot {
		T value;
		Slot* next;
		Slot() {}  // do nothing: we manage T's lifetime manually
		~Slot() {}  // do nothing: we manage T's lifetime manually
	};

	bool is_valid_ptr(T* ptr) const {
		// Check if the pointer is within the bounds of the storage array

		auto start = reinterpret_cast<uintptr_t>(storage_.data());
		auto end = reinterpret_cast<uintptr_t>(start + storage_.size());
		auto addr = reinterpret_cast<uintptr_t>(ptr);

		return addr >= start && addr < end && (addr - start) % sizeof(Slot) == 0;
	}

	std::vector<Slot> storage_;  // one time contiguous allocation
	Slot* free_head_;  // head of the intrusive free list (nullptr == exhausted)
	size_t free_count_ = 0;  // number of free slots (for tracking availability)

public:
	// Allocate the whole block once, up front. Thread all slots onto the free list.
	explicit ObjectPool(std::size_t capacity)
	: storage_(capacity)  // allocates 'capacity' slots; runs Slots() (no-op) on each 
	, free_head_(nullptr)
	, free_count_(capacity) {
		// Build the initial free list: every slot points to the next, last points to null.
		for (size_t i=capacity; i-- > 0;){
			storage_[i].next = free_head_;
			free_head_ = &storage_[i];
		}
	}

	// No copying - a pool owns raw memory and free list; copying makes no sense
	ObjectPool(const ObjectPool&) = delete;
	ObjectPool& operator=(const ObjectPool&) = delete;

	// Hand out one slot's worth of raw storage, construct a T into it with the given args.
	// Returns nullptr if exhausted (no exceptions on the hot path).
	template <typename... Args>
	T* allocate(Args&&... args){
		if (free_head_ == nullptr){
			return nullptr;  // exhausted - caller decides what to do
		}
		Slot* slot = free_head_;  // pop the head of the free list
		free_head_ = slot->next;  // advance the head to the next free slot
		free_count_--;  // decrement the free count

		// Placement new: construct a T "into the slot's existing memory".
		// No heap allocation happens here - 'slot' already points to valid storage.
		// 'std::forward' perfectly forwards constructor arguments (lvalue/rvalue preserved).
		return new (&slot->value) T(std::forward<Args>(args)...);
	}

	// Destroy the object and return its slot to the free list.
	void deallocate(T* ptr){
		if (ptr == nullptr)
			return;

		if (!is_valid_ptr(ptr)) {
			assert(false && "Pointer does not belong to this ObjectPool");
			return;  // or throw an exception, depending on your error handling strategy
		}

		ptr->~T();  // explicitly run T's destructor (placement new doesn't auto destroy)

		// Reinterpret the storage as a Slot and push it onto the free list.
		// Safe because (&slot->value) has the same address as the slot itself (first union member).
		Slot* slot = reinterpret_cast<Slot*>(ptr);
		slot->next = free_head_;
		free_head_ = slot;
		free_count_++;  // increment the free count
	}

	size_t capacity() const {
		return storage_.size();
	}

	size_t available() const {
		return free_count_;
	}
};
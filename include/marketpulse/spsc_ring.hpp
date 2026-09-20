#pragma once
// SPDX-License-Identifier: MIT
// spsc_ring.hpp — Hand-written lock-free single-producer single-consumer ring buffer.
//
// DESIGN GOALS (from spec):
//   - Fixed capacity, power of two, no allocation after construction
//   - alignas(64) on head, tail, and buffer to eliminate false sharing
//   - Acquire/release atomics, NOT seq_cst
//   - Cached shadow index on each side to avoid atomic load on every operation
//   - push() returns false when full (caller increments drop counter)
//
// THREAD SAFETY:
//   - Exactly ONE producer thread calls push()
//   - Exactly ONE consumer thread calls pop()
//   - No other threads touch this object

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>     // std::memcpy
#include <type_traits>
#include <utility>

namespace marketpulse {

namespace detail {
// GCC 13 warns on std::hardware_destructive_interference_size even when the
// feature-test macro is defined, because the value can vary with -mtune/-mcpu.
// 64 bytes is the correct value for every x86-64 and arm64 CPU we target.
inline constexpr std::size_t CACHELINE = 64;
} // namespace detail

/// SpscRing<T, N>: Single-producer, single-consumer ring buffer.
/// N must be a power of two.
/// T must be trivially copy-assignable and trivially destructible.
/// T does NOT need to be default-constructible (raw byte storage is used).
template <typename T, std::size_t N>
class SpscRing {
    static_assert((N & (N - 1)) == 0, "N must be a power of two");
    // The ring copies items with memcpy (push) and memcpy (pop), so T must be
    // trivially copy-assignable.  Individual elements are never explicitly
    // destroyed (the whole array is), so trivial destructor is required.
    // T does NOT need a default constructor — raw byte storage is used.
    static_assert(std::is_trivially_copy_assignable_v<T>,
                  "T must be trivially copy-assignable for the hot-path push/pop");
    static_assert(std::is_trivially_destructible_v<T>,
                  "T must be trivially destructible for the ring buffer");

    static constexpr std::size_t MASK = N - 1;

public:
    SpscRing() noexcept = default;

    // Non-copyable, non-movable (contains atomics and aligned storage)
    SpscRing(const SpscRing&)            = delete;
    SpscRing& operator=(const SpscRing&) = delete;
    SpscRing(SpscRing&&)                 = delete;
    SpscRing& operator=(SpscRing&&)      = delete;

    //--------------------------------------------------------------------------
    // Producer API (call only from the producer thread)
    //--------------------------------------------------------------------------

    /// Try to push an item.
    /// Returns true on success, false if the queue is full.
    /// Never blocks, never allocates.
    [[nodiscard]] bool push(const T& item) noexcept {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        const std::size_t next = head + 1;

        // Use cached tail to avoid an atomic load on every push.
        // Only refresh the cache when we think the queue might be full.
        if ((next - tail_cached_) > MASK) {
            tail_cached_ = tail_.load(std::memory_order_acquire);
            if ((next - tail_cached_) > MASK) {
                return false; // full
            }
        }

        std::memcpy(slot(head & MASK), &item, sizeof(T));
        head_.store(next, std::memory_order_release);
        return true;
    }

    //--------------------------------------------------------------------------
    // Consumer API (call only from the consumer thread)
    //--------------------------------------------------------------------------

    /// Try to pop an item.
    /// Returns true on success (item written to out), false if empty.
    /// Never blocks, never allocates.
    [[nodiscard]] bool pop(T& out) noexcept {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);

        // Use cached head to avoid an atomic load on every pop.
        if (tail == head_cached_) {
            head_cached_ = head_.load(std::memory_order_acquire);
            if (tail == head_cached_) {
                return false; // empty
            }
        }

        std::memcpy(&out, slot(tail & MASK), sizeof(T));
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    /// Approximate size (not exact under concurrent use, fine for monitoring).
    [[nodiscard]] std::size_t size_approx() const noexcept {
        const std::size_t h = head_.load(std::memory_order_relaxed);
        const std::size_t t = tail_.load(std::memory_order_relaxed);
        return h - t;
    }

    [[nodiscard]] static constexpr std::size_t capacity() noexcept { return N; }

private:
    // Helper: typed pointer into raw storage at slot index i.
    T* slot(std::size_t i) noexcept {
        return reinterpret_cast<T*>(buf_ + i * sizeof(T));
    }
    const T* slot(std::size_t i) const noexcept {
        return reinterpret_cast<const T*>(buf_ + i * sizeof(T));
    }

    // Producer-side data: head index + its cached view of tail.
    // Placed on its own cache line to avoid false sharing with consumer.
    alignas(detail::CACHELINE) std::atomic<std::size_t> head_{0};
    std::size_t tail_cached_{0}; // producer's stale view of tail (OK to be stale)

    // Consumer-side data: tail index + its cached view of head.
    alignas(detail::CACHELINE) std::atomic<std::size_t> tail_{0};
    std::size_t head_cached_{0}; // consumer's stale view of head (OK to be stale)

    // Raw byte storage: T does not need to be default-constructible.
    // Aligned to both a cache line and the alignment of T.
    alignas(detail::CACHELINE) alignas(T) char buf_[sizeof(T) * N];
};

} // namespace marketpulse

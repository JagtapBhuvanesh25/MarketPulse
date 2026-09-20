#pragma once
// SPDX-License-Identifier: MIT
// spsc_ring.hpp — Hand-written lock-free single-producer single-consumer ring buffer.
//
// DESIGN GOALS (from spec):
//   - Fixed capacity, power of two, no allocation after construction
//   - alignas(hardware_destructive_interference_size) on head, tail, and buffer
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
#include <new>      // std::hardware_destructive_interference_size
#include <type_traits>
#include <utility>

namespace marketpulse {

namespace detail {
// Fallback if the compiler doesn't define hardware_destructive_interference_size
#ifdef __cpp_lib_hardware_interference_size
    inline constexpr std::size_t CACHELINE = std::hardware_destructive_interference_size;
#else
    inline constexpr std::size_t CACHELINE = 64;
#endif
} // namespace detail

/// SpscRing<T, N>: Single-producer, single-consumer ring buffer.
/// N must be a power of two. T must be trivially copyable (POD-like).
template <typename T, std::size_t N>
class SpscRing {
    static_assert((N & (N - 1)) == 0, "N must be a power of two");
    static_assert(std::is_trivially_copyable_v<T>,
                  "T must be trivially copyable for the hot path (no allocation)");

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

        buf_[head & MASK] = item;
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

        out = buf_[tail & MASK];
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
    // Producer-side data: head index + its cached view of tail.
    // Placed on its own cache line to avoid false sharing with consumer.
    alignas(detail::CACHELINE) std::atomic<std::size_t> head_{0};
    std::size_t tail_cached_{0}; // producer's stale view of tail (OK to be stale)

    // Consumer-side data: tail index + its cached view of head.
    alignas(detail::CACHELINE) std::atomic<std::size_t> tail_{0};
    std::size_t head_cached_{0}; // consumer's stale view of head (OK to be stale)

    // The ring buffer itself, on its own cache line.
    alignas(detail::CACHELINE) T buf_[N];
};

} // namespace marketpulse

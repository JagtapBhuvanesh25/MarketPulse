#pragma once
// SPDX-License-Identifier: MIT
// book_ladder.hpp — Optimised flat-array order book with bitset occupancy mask.
//
// DESIGN:
//   - A fixed array of Qty indexed by tick offset from a base price.
//   - A parallel uint64_t[] occupancy bitmask (one bit per slot).
//   - best_bid() and best_ask() use countl_zero / countr_zero on the bitmask
//     for O(1) lookup (one word scan + at most a few words for overflow).
//   - apply() is one array store + one bit flip — no allocation, no pointers.
//   - Re-centring: when the best bid or best ask drifts within EDGE_GUARD ticks
//     of the window boundary, the base price is shifted and the array is
//     reorganised. This is O(WINDOW) but happens rarely.
//   - No allocation after construction.
//
// PARAMETERS (see constants below):
//   TICK_SIZE  : smallest price increment (= 1 for $0.01 = 1'000'000 in 1e8)
//   WINDOW     : total number of ticks in the array (must be power of 2)
//   EDGE_GUARD : recentre when best bid/ask within this many ticks of edge
//
// AARCH64 NOTE:
//   Uses std::countl_zero and std::countr_zero (C++20 <bit>), which lower to
//   CLZ / RBIT+CLZ on aarch64. No SSE/AVX, no rdtsc.

#include "book_iface.hpp"
#include <array>
#include <bit>       // std::countl_zero, std::countr_zero
#include <cstring>   // std::memset
#include <cstdint>

namespace marketpulse {

namespace ladder_detail {
    // BTC/USDT: minimum tick is $0.01 → 1'000'000 in 1e8 units.
    inline constexpr Price TICK_SIZE   = 1'000'000LL;   // $0.01 in 1e8
    inline constexpr int   WINDOW      = 16384;          // ±8192 ticks from mid (~±$81.92)
    inline constexpr int   EDGE_GUARD  = 512;            // recentre if within $5.12 of edge
    inline constexpr int   HALF_WINDOW = WINDOW / 2;
    inline constexpr int   WORDS       = WINDOW / 64;   // 256 uint64_t words

    static_assert((WINDOW & (WINDOW - 1)) == 0, "WINDOW must be power of 2");
    static_assert(WINDOW % 64 == 0, "WINDOW must be multiple of 64");
} // namespace ladder_detail

class BookLadder final : public IBook {
public:
    BookLadder() noexcept { clear(); }

    void apply(Side side, Price price, Qty qty) noexcept override;

    [[nodiscard]] BookLevel best_bid() const noexcept override;
    [[nodiscard]] BookLevel best_ask() const noexcept override;

    std::size_t top_levels(Side side,
                           std::span<BookLevel> out) const noexcept override;

    [[nodiscard]] std::size_t level_count(Side side) const noexcept override;

    void clear() noexcept override;

    /// Number of times the base price has been recentred (for tests/diagnostics).
    [[nodiscard]] uint64_t recentre_count() const noexcept { return recentre_count_; }

private:
    using Mask = std::array<uint64_t, ladder_detail::WORDS>;
    using Buf  = std::array<Qty, ladder_detail::WINDOW>;

    // Bid side: index 0 = lowest price, higher indices = higher prices.
    // Bids occupy prices [base_price_, base_price_ + WINDOW*TICK_SIZE).
    // Occupancy bit set means qty > 0. Best bid = highest set bit.
    alignas(64) Buf  bid_qty_{};
    alignas(64) Mask bid_mask_{};

    // Ask side: same indexing.
    alignas(64) Buf  ask_qty_{};
    alignas(64) Mask ask_mask_{};

    Price    base_price_{0};   // price corresponding to index 0
    uint64_t recentre_count_{0};

    // Helpers
    [[nodiscard]] int  price_to_idx(Price p) const noexcept;
    [[nodiscard]] Price idx_to_price(int idx) const noexcept;

    void set_bit(Mask& mask, int idx) noexcept;
    void clear_bit(Mask& mask, int idx) noexcept;

    /// Find the highest set bit in the bid mask (best bid index).
    [[nodiscard]] int best_bid_idx() const noexcept;
    /// Find the lowest set bit in the ask mask (best ask index).
    [[nodiscard]] int best_ask_idx() const noexcept;

    /// Shift the base price and reorganise the arrays.
    void recentre(Price new_base) noexcept;

    /// Check if recentring is needed based on current best bid/ask.
    void maybe_recentre() noexcept;

    [[nodiscard]] bool in_window(Price p) const noexcept;
};

} // namespace marketpulse

#pragma once
// SPDX-License-Identifier: MIT
// book_iface.hpp — Common interface for order book implementations.
//
// Both BookMap (reference) and BookLadder (optimised) implement IBook,
// allowing hot-swap via --book=map|ladder flag.

#include "types.hpp"
#include <span>
#include <cstdint>

namespace marketpulse {

/// IBook: abstract interface for an L2 limit order book.
///
/// Contract:
///   - apply(side, price, qty): if qty == 0, remove the level; else upsert it.
///   - best_bid() / best_ask(): return the best price level, or {PRICE_NONE, 0}
///     if the book is empty on that side.
///   - top_levels(): write up to n levels into out (best price first), return
///     how many were written. Never allocates.
class IBook {
public:
    virtual ~IBook() = default;

    /// Apply one diff entry. qty == 0 means remove the level.
    /// Must not allocate (for the hot path).
    virtual void apply(Side side, Price price, Qty qty) noexcept = 0;

    /// Return the best bid level ({PRICE_NONE, 0} if empty).
    [[nodiscard]] virtual BookLevel best_bid() const noexcept = 0;

    /// Return the best ask level ({PRICE_NONE, 0} if empty).
    [[nodiscard]] virtual BookLevel best_ask() const noexcept = 0;

    /// Write up to n levels into out[], best first.
    /// Returns the number of levels written.
    virtual std::size_t top_levels(Side side,
                                   std::span<BookLevel> out) const noexcept = 0;

    /// Total number of price levels currently in the book (for diagnostics).
    [[nodiscard]] virtual std::size_t level_count(Side side) const noexcept = 0;

    /// Reset to empty state (used during resync).
    virtual void clear() noexcept = 0;
};

} // namespace marketpulse

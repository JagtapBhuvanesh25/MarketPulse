#pragma once
// SPDX-License-Identifier: MIT
// book_map.hpp — Reference order book using std::map.
//
// This is the oracle implementation: deliberately simple and obviously correct.
// Bids: std::map<Price, Qty, std::greater<>> — highest price first.
// Asks: std::map<Price, Qty>               — lowest price first.
// Zero qty → erase the level.
//
// This book is NOT on the hot path when --book=ladder is active.
// It IS on the hot path during --book=map (reference mode) and during
// equivalence tests (always runs alongside BookLadder).

#include "book_iface.hpp"
#include <map>
#include <functional>

namespace marketpulse {

class BookMap final : public IBook {
public:
    BookMap() = default;

    void apply(Side side, Price price, Qty qty) noexcept override;

    [[nodiscard]] BookLevel best_bid() const noexcept override;
    [[nodiscard]] BookLevel best_ask() const noexcept override;

    std::size_t top_levels(Side side,
                           std::span<BookLevel> out) const noexcept override;

    [[nodiscard]] std::size_t level_count(Side side) const noexcept override;

    void clear() noexcept override;

private:
    std::map<Price, Qty, std::greater<Price>> bids_;  // highest bid first
    std::map<Price, Qty>                      asks_;  // lowest ask first
};

} // namespace marketpulse

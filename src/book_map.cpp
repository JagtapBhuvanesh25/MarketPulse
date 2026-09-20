// SPDX-License-Identifier: MIT
// book_map.cpp — Reference std::map-based order book implementation.

#include "marketpulse/book_map.hpp"
#include <algorithm>

namespace marketpulse {

void BookMap::apply(Side side, Price price, Qty qty) noexcept {
    if (side == Side::Bid) {
        if (qty == QTY_ZERO) { bids_.erase(price); }
        else                 { bids_[price] = qty; }
    } else {
        if (qty == QTY_ZERO) { asks_.erase(price); }
        else                 { asks_[price] = qty; }
    }
}

BookLevel BookMap::best_bid() const noexcept {
    if (bids_.empty()) return {PRICE_NONE, QTY_ZERO};
    const auto it = bids_.begin(); // std::greater → highest price first
    return {it->first, it->second};
}

BookLevel BookMap::best_ask() const noexcept {
    if (asks_.empty()) return {PRICE_NONE, QTY_ZERO};
    const auto it = asks_.begin(); // lowest price first
    return {it->first, it->second};
}

std::size_t BookMap::top_levels(Side side,
                                std::span<BookLevel> out) const noexcept {
    std::size_t written = 0;
    if (side == Side::Bid) {
        for (auto it = bids_.begin();
             it != bids_.end() && written < out.size(); ++it, ++written) {
            out[written] = {it->first, it->second};
        }
    } else {
        for (auto it = asks_.begin();
             it != asks_.end() && written < out.size(); ++it, ++written) {
            out[written] = {it->first, it->second};
        }
    }
    return written;
}

std::size_t BookMap::level_count(Side side) const noexcept {
    return (side == Side::Bid) ? bids_.size() : asks_.size();
}

void BookMap::clear() noexcept {
    bids_.clear();
    asks_.clear();
}

} // namespace marketpulse

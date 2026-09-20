// SPDX-License-Identifier: MIT
// book_ladder.cpp — Flat-array + bitmask order book implementation.

#include "marketpulse/book_ladder.hpp"
#include <cstring>   // std::memmove, std::memset
#include <algorithm> // std::min
#include <bit>

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wconversion"
#endif

namespace marketpulse {

using namespace ladder_detail;

//-----------------------------------------------------------------------------
// Helpers
//-----------------------------------------------------------------------------

[[nodiscard]] int BookLadder::price_to_idx(Price p) const noexcept {
    return static_cast<int>((p - base_price_) / TICK_SIZE);
}

[[nodiscard]] Price BookLadder::idx_to_price(int idx) const noexcept {
    return base_price_ + static_cast<Price>(idx) * TICK_SIZE;
}

[[nodiscard]] bool BookLadder::in_window(Price p) const noexcept {
    const int idx = price_to_idx(p);
    return idx >= 0 && idx < WINDOW;
}

void BookLadder::set_bit(Mask& mask, int idx) noexcept {
    mask[idx / 64] |= (uint64_t{1} << (idx % 64));
}

void BookLadder::clear_bit(Mask& mask, int idx) noexcept {
    mask[idx / 64] &= ~(uint64_t{1} << (idx % 64));
}

//-----------------------------------------------------------------------------
// Best bid: highest set bit in bid_mask_
// Bids: bit i is set if bid_qty_[i] > 0.
// Highest price = highest index = highest bit.
// Scan words from the top.
//-----------------------------------------------------------------------------
[[nodiscard]] int BookLadder::best_bid_idx() const noexcept {
    for (int w = WORDS - 1; w >= 0; --w) {
        if (bid_mask_[w] != 0) {
            // Highest set bit in word w
            const int bit = 63 - std::countl_zero(bid_mask_[w]);
            return w * 64 + bit;
        }
    }
    return -1; // empty
}

//-----------------------------------------------------------------------------
// Best ask: lowest set bit in ask_mask_
// Asks: bit i is set if ask_qty_[i] > 0.
// Lowest price = lowest index = lowest bit.
// Scan words from the bottom.
//-----------------------------------------------------------------------------
[[nodiscard]] int BookLadder::best_ask_idx() const noexcept {
    for (int w = 0; w < WORDS; ++w) {
        if (ask_mask_[w] != 0) {
            const int bit = std::countr_zero(ask_mask_[w]);
            return w * 64 + bit;
        }
    }
    return -1; // empty
}

//-----------------------------------------------------------------------------
// Apply
//-----------------------------------------------------------------------------
void BookLadder::apply(Side side, Price price, Qty qty) noexcept {
    // Snap price to tick boundary
    const Price snapped = (price / TICK_SIZE) * TICK_SIZE;

    // If outside window, recentre first around this price if we have no book yet,
    // or silently accept it if the window needs adjusting.
    if (!in_window(snapped)) {
        // Recentre so snapped price lands at HALF_WINDOW.
        const Price new_base = snapped - static_cast<Price>(HALF_WINDOW) * TICK_SIZE;
        recentre(new_base);
    }

    const int idx = price_to_idx(snapped);
    if (idx < 0 || idx >= WINDOW) return; // still out of range after recentre

    if (side == Side::Bid) {
        bid_qty_[idx] = qty;
        if (qty > 0) set_bit(bid_mask_, idx);
        else         clear_bit(bid_mask_, idx);
    } else {
        ask_qty_[idx] = qty;
        if (qty > 0) set_bit(ask_mask_, idx);
        else         clear_bit(ask_mask_, idx);
    }

    maybe_recentre();
}

BookLevel BookLadder::best_bid() const noexcept {
    const int idx = best_bid_idx();
    if (idx < 0) return {PRICE_NONE, QTY_ZERO};
    return {idx_to_price(idx), bid_qty_[idx]};
}

BookLevel BookLadder::best_ask() const noexcept {
    const int idx = best_ask_idx();
    if (idx < 0) return {PRICE_NONE, QTY_ZERO};
    return {idx_to_price(idx), ask_qty_[idx]};
}

std::size_t BookLadder::top_levels(Side side,
                                   std::span<BookLevel> out) const noexcept {
    std::size_t written = 0;
    if (side == Side::Bid) {
        // Scan from top (highest index) downward
        for (int w = WORDS - 1; w >= 0 && written < out.size(); --w) {
            uint64_t word = bid_mask_[w];
            while (word && written < out.size()) {
                const int bit = 63 - std::countl_zero(word);
                const int idx = w * 64 + bit;
                out[written++] = {idx_to_price(idx), bid_qty_[idx]};
                word &= ~(uint64_t{1} << bit);
            }
        }
    } else {
        // Scan from bottom (lowest index) upward
        for (int w = 0; w < WORDS && written < out.size(); ++w) {
            uint64_t word = ask_mask_[w];
            while (word && written < out.size()) {
                const int bit = std::countr_zero(word);
                const int idx = w * 64 + bit;
                out[written++] = {idx_to_price(idx), ask_qty_[idx]};
                word &= ~(uint64_t{1} << bit);
            }
        }
    }
    return written;
}

std::size_t BookLadder::level_count(Side side) const noexcept {
    const Mask& mask = (side == Side::Bid) ? bid_mask_ : ask_mask_;
    std::size_t count = 0;
    for (const auto& w : mask) count += static_cast<std::size_t>(std::popcount(w));
    return count;
}

void BookLadder::clear() noexcept {
    bid_qty_.fill(0);
    ask_qty_.fill(0);
    bid_mask_.fill(0);
    ask_mask_.fill(0);
    // base_price_ intentionally not reset — caller may repopulate
}

//-----------------------------------------------------------------------------
// Re-centring
//-----------------------------------------------------------------------------

void BookLadder::recentre(Price new_base) noexcept {
    // Snap new_base to tick boundary
    new_base = (new_base / TICK_SIZE) * TICK_SIZE;
    if (new_base == base_price_) return;

    const int shift = static_cast<int>((new_base - base_price_) / TICK_SIZE);

    // Build new arrays. We use local temporaries to avoid aliasing issues.
    Buf  new_bid_qty{};
    Mask new_bid_mask{};
    Buf  new_ask_qty{};
    Mask new_ask_mask{};

    // Copy entries that still fall within the new window.
    for (int old_idx = 0; old_idx < WINDOW; ++old_idx) {
        const int new_idx = old_idx - shift;
        if (new_idx < 0 || new_idx >= WINDOW) continue;

        if (bid_qty_[old_idx] > 0) {
            new_bid_qty[new_idx] = bid_qty_[old_idx];
            new_bid_mask[new_idx / 64] |= (uint64_t{1} << (new_idx % 64));
        }
        if (ask_qty_[old_idx] > 0) {
            new_ask_qty[new_idx] = ask_qty_[old_idx];
            new_ask_mask[new_idx / 64] |= (uint64_t{1} << (new_idx % 64));
        }
    }

    bid_qty_   = new_bid_qty;
    bid_mask_  = new_bid_mask;
    ask_qty_   = new_ask_qty;
    ask_mask_  = new_ask_mask;
    base_price_ = new_base;
    ++recentre_count_;
}

void BookLadder::maybe_recentre() noexcept {
    const int bid_idx = best_bid_idx();
    const int ask_idx = best_ask_idx();

    if (bid_idx < 0 && ask_idx < 0) return;

    // Calculate mid index
    int mid_idx = 0;
    if (bid_idx >= 0 && ask_idx >= 0) {
        mid_idx = (bid_idx + ask_idx) / 2;
    } else if (bid_idx >= 0) {
        mid_idx = bid_idx;
    } else {
        mid_idx = ask_idx;
    }

    // Recentre if mid is within EDGE_GUARD of either edge.
    if (mid_idx < EDGE_GUARD || mid_idx >= WINDOW - EDGE_GUARD) {
        const Price mid_price = idx_to_price(mid_idx);
        const Price new_base  = mid_price - static_cast<Price>(HALF_WINDOW) * TICK_SIZE;
        recentre(new_base);
    }
}

} // namespace marketpulse

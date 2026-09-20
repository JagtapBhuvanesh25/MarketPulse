#pragma once
// SPDX-License-Identifier: MIT
// signals.hpp — Microstructure signals computed from the live order book.
//
// Implements exactly three signals (no more, per spec):
//   1. Microprice          — weighted mid based on top-of-book sizes
//   2. Order Flow Imbalance (OFI)  — Cont-Kukanov-Stoikov, top 5 levels
//   3. Rolling Trade Imbalance    — signed traded volume over a time window
//
// Signals use double ONLY at the final presentation step. All intermediate
// arithmetic on Price/Qty uses int64_t.
//
// HOT PATH: compute_microprice and compute_ofi must not allocate.
// They receive book state as spans of BookLevel.

#include "types.hpp"
#include <span>
#include <cstdint>
#include <chrono>
#include <array>

namespace marketpulse {

/// Precomputed signal values, updated after every book update.
struct Signals {
    double microprice{0.0};
    double ofi{0.0};           // Order Flow Imbalance (dimensionless, sign matters)
    double trade_imbalance{0.0}; // Fraction in [-1, 1]

    // Previous OFI state: top-5 levels from the *prior* book update.
    // Stored here so OFI can be computed incrementally without extra allocation.
    std::array<BookLevel, 5> prev_bids{};
    std::array<BookLevel, 5> prev_asks{};
};

//-----------------------------------------------------------------------------
// 1. Microprice
//    = (bid_qty * ask_px + ask_qty * bid_px) / (bid_qty + ask_qty)
//    Uses double only at the division step.
//-----------------------------------------------------------------------------

/// Compute microprice from top-of-book levels.
/// Returns 0.0 if either side is empty.
[[nodiscard]] double compute_microprice(BookLevel bid, BookLevel ask) noexcept;

//-----------------------------------------------------------------------------
// 2. Order Flow Imbalance (OFI)
//    Cont, Kukanov, Stoikov (2014): "The Price Impact of Order Book Events"
//    OFI = sum_{i=1}^{5} (ΔBid_i - ΔAsk_i)
//    where ΔBid_i = bid_qty_i - prev_bid_qty_i if bid_px_i == prev_bid_px_i
//                 = bid_qty_i                   if new level appeared
//                 = -prev_bid_qty_i              if level disappeared
//    (symmetric for asks with sign flipped)
//-----------------------------------------------------------------------------

/// Compute OFI from current and previous top-5 levels.
/// bids[] and asks[] must each have exactly 5 entries (padded with invalid
/// BookLevel if fewer levels exist).
[[nodiscard]] double compute_ofi(
    std::span<const BookLevel, 5> bids,
    std::span<const BookLevel, 5> asks,
    std::span<const BookLevel, 5> prev_bids,
    std::span<const BookLevel, 5> prev_asks) noexcept;

//-----------------------------------------------------------------------------
// 3. Rolling Trade Imbalance
//    signed_volume = sum of qty for buy trades - sum of qty for sell trades
//    imbalance = signed_volume / total_volume   (in [-1, 1])
//    Window = last N nanoseconds (configurable).
//-----------------------------------------------------------------------------

/// A compact trade record for the rolling window (hot-path allocation-free).
struct TradeSample {
    NsPoint t_recv{};
    Qty     qty{0};
    bool    is_buy{false}; // true = buyer is taker (aggressive buy)
};

/// RollingTradeWindow: fixed-size circular buffer of trade samples.
/// Template parameter MAX_SAMPLES must be large enough to hold the window.
template <std::size_t MAX_SAMPLES = 2048>
class RollingTradeWindow {
public:
    explicit RollingTradeWindow(uint64_t window_ns = 30'000'000'000ULL) // 30 seconds
        : window_ns_(window_ns) {}

    /// Add a trade to the window.
    void push(NsPoint t, Qty qty, bool is_buy) noexcept {
        samples_[head_ % MAX_SAMPLES] = {t, qty, is_buy};
        ++head_;
    }

    /// Compute trade imbalance: (buy_vol - sell_vol) / (buy_vol + sell_vol).
    /// Returns 0.0 if no trades in window.
    [[nodiscard]] double compute(NsPoint now) const noexcept;

    void set_window_ns(uint64_t ns) noexcept { window_ns_ = ns; }

private:
    std::array<TradeSample, MAX_SAMPLES> samples_{};
    std::size_t head_{0};
    uint64_t    window_ns_{30'000'000'000ULL};
};

// Template implementation (must be in header)
template <std::size_t MAX_SAMPLES>
[[nodiscard]] double RollingTradeWindow<MAX_SAMPLES>::compute(NsPoint now) const noexcept {
    using namespace std::chrono;
    const auto cutoff = now - nanoseconds(window_ns_);

    int64_t buy_vol  = 0;
    int64_t sell_vol = 0;

    // Iterate from newest to oldest, stop when outside window.
    const std::size_t count = (head_ < MAX_SAMPLES) ? head_ : MAX_SAMPLES;
    for (std::size_t k = 0; k < count; ++k) {
        const std::size_t idx = (head_ - 1 - k) % MAX_SAMPLES;
        const auto& s = samples_[idx];
        if (s.t_recv < cutoff) break;
        if (s.is_buy) buy_vol  += s.qty;
        else          sell_vol += s.qty;
    }

    const int64_t total = buy_vol + sell_vol;
    if (total == 0) return 0.0;
    return static_cast<double>(buy_vol - sell_vol) / static_cast<double>(total);
}

} // namespace marketpulse

// SPDX-License-Identifier: MIT
// signals.cpp — Microstructure signal implementations.

#include "marketpulse/signals.hpp"
#include <cstdint>
#include <cmath>

namespace marketpulse {

//-----------------------------------------------------------------------------
// 1. Microprice
//    = (bid_qty * ask_px + ask_qty * bid_px) / (bid_qty + ask_qty)
//    Uses int64_t arithmetic until the final division.
//-----------------------------------------------------------------------------

double compute_microprice(BookLevel bid, BookLevel ask) noexcept {
    if (!bid.valid() || !ask.valid()) return 0.0;
    if (bid.qty == 0 && ask.qty == 0) return 0.0;

    // Compute in fixed-point then convert at the end.
    // bid_qty * ask_px + ask_qty * bid_px may overflow int64_t for extreme values.
    // Use __int128 intermediates to be safe.
    const __int128 num = static_cast<__int128>(bid.qty) * ask.price
                       + static_cast<__int128>(ask.qty) * bid.price;
    const __int128 den = static_cast<__int128>(bid.qty) + ask.qty;

    if (den == 0) return 0.0;

    // Result is in fixed-point (× FIXED_SCALE).
    // Convert to double, then divide by FIXED_SCALE to get dollars.
    return static_cast<double>(num) / static_cast<double>(den)
           / static_cast<double>(FIXED_SCALE);
}

//-----------------------------------------------------------------------------
// 2. Order Flow Imbalance (Cont–Kukanov–Stoikov)
//    OFI = Σ_{i=1}^{5} (ΔBid_i - ΔAsk_i)
//
//    For each level i:
//      ΔBid_i:
//        If bid_px[i] == prev_bid_px[i]: bid_qty[i] - prev_bid_qty[i]
//        If bid_px[i] > prev_bid_px[i]:  +bid_qty[i]  (new level at higher price)
//        If bid_px[i] < prev_bid_px[i]:  -prev_bid_qty[i]  (level moved away)
//      ΔAsk_i: symmetric (lower ask = more aggressive)
//        If ask_px[i] == prev_ask_px[i]: ask_qty[i] - prev_ask_qty[i]
//        If ask_px[i] < prev_ask_px[i]:  +ask_qty[i]
//        If ask_px[i] > prev_ask_px[i]:  -prev_ask_qty[i]
//-----------------------------------------------------------------------------

double compute_ofi(
    std::span<const BookLevel, 5> bids,
    std::span<const BookLevel, 5> asks,
    std::span<const BookLevel, 5> prev_bids,
    std::span<const BookLevel, 5> prev_asks) noexcept
{
    int64_t ofi = 0;

    for (int i = 0; i < 5; ++i) {
        const auto& b  = bids[i];
        const auto& pb = prev_bids[i];
        const auto& a  = asks[i];
        const auto& pa = prev_asks[i];

        // ΔBid
        int64_t delta_bid = 0;
        if (!b.valid() && !pb.valid()) {
            delta_bid = 0;
        } else if (!pb.valid()) {
            delta_bid = b.qty;   // new level appeared
        } else if (!b.valid()) {
            delta_bid = -pb.qty; // level disappeared
        } else if (b.price == pb.price) {
            delta_bid = b.qty - pb.qty;
        } else if (b.price > pb.price) {
            delta_bid = b.qty;   // bid moved up (more aggressive)
        } else {
            delta_bid = -pb.qty; // bid moved down (less aggressive)
        }

        // ΔAsk
        int64_t delta_ask = 0;
        if (!a.valid() && !pa.valid()) {
            delta_ask = 0;
        } else if (!pa.valid()) {
            delta_ask = a.qty;   // new ask level
        } else if (!a.valid()) {
            delta_ask = -pa.qty;
        } else if (a.price == pa.price) {
            delta_ask = a.qty - pa.qty;
        } else if (a.price < pa.price) {
            delta_ask = a.qty;   // ask moved down (more aggressive)
        } else {
            delta_ask = -pa.qty; // ask moved up (less aggressive)
        }

        ofi += delta_bid - delta_ask;
    }

    // Normalise by FIXED_SCALE so the result is in "natural" quantity units.
    return static_cast<double>(ofi) / static_cast<double>(FIXED_SCALE);
}

} // namespace marketpulse

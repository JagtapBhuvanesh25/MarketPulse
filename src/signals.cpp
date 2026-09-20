// SPDX-License-Identifier: MIT
// signals.cpp — Microstructure signal implementations.

#include "marketpulse/signals.hpp"
#include <cstdint>
#include <cmath>

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#endif

namespace marketpulse {

//-----------------------------------------------------------------------------
// 1. Microprice
//    = (bid_qty * ask_px + ask_qty * bid_px) / (bid_qty + ask_qty)
//    Uses int64_t arithmetic until the final division.
//-----------------------------------------------------------------------------

#if defined(__GNUC__) || defined(__clang__)
__extension__ typedef __int128 int128_t;
#else
using int128_t = int64_t;
#endif

double compute_microprice(BookLevel bid, BookLevel ask) noexcept {
    if (!bid.valid() || !ask.valid()) return 0.0;
    if (bid.qty == 0 && ask.qty == 0) return 0.0;

    // Compute in fixed-point then convert at the end.
    // bid_qty * ask_px + ask_qty * bid_px may overflow int64_t for extreme values.
    // Use __int128 intermediates to be safe.
    const int128_t num = static_cast<int128_t>(bid.qty) * ask.price
                       + static_cast<int128_t>(ask.qty) * bid.price;
    const int128_t den = static_cast<int128_t>(bid.qty) + ask.qty;

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
//        =  b_i.qty           if b_i.price > p_i.price (price stepped up)
//        =  b_i.qty - p_i.qty if b_i.price == p_i.price (qty delta)
//        = -p_i.qty           if b_i.price < p_i.price (price stepped down)
//      ΔAsk_i:
//        = -a_i.qty           if a_i.price < q_i.price (price stepped down = sell pressure)
//        =  a_i.qty - q_i.qty if a_i.price == q_i.price (qty delta)
//        =  q_i.qty           if a_i.price > q_i.price (price stepped up)
//-----------------------------------------------------------------------------

double compute_ofi(
    std::span<const BookLevel, 5> bids,
    std::span<const BookLevel, 5> asks,
    std::span<const BookLevel, 5> prev_bids,
    std::span<const BookLevel, 5> prev_asks) noexcept
{
    int64_t ofi = 0;

    for (std::size_t i = 0; i < 5; ++i) {
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

// SPDX-License-Identifier: MIT
// sim.cpp — Virtual limit order simulation engine.

#include "marketpulse/sim.hpp"
#include <algorithm>

namespace marketpulse {

uint64_t SimEngine::place_order(Side side, Price price, Qty qty,
                                Qty resting_qty, NsPoint now) noexcept {
    const uint64_t id = next_id_++;
    orders_.push_back(VirtualOrder{
        .order_id   = id,
        .side       = side,
        .price      = price,
        .qty        = qty,
        .queue_pos  = resting_qty,
        .filled_qty = 0,
        .pnl_e8     = 0,
        .status     = OrderStatus::Pending,
        .placed_at  = now,
    });
    return id;
}

bool SimEngine::cancel_order(uint64_t order_id) noexcept {
    for (auto& o : orders_) {
        if (o.order_id == order_id && o.status == OrderStatus::Pending) {
            o.status = OrderStatus::Cancelled;
            return true;
        }
    }
    return false;
}

void SimEngine::on_trade(Side maker_side, Price price, Qty traded_qty,
                         NsPoint now) noexcept {
    for (auto& o : orders_) {
        if (o.status != OrderStatus::Pending) continue;
        if (o.price != price) continue;
        // A bid order rests on the bid side; it's the maker when a sell hits it.
        // Maker side of the trade tells us which resting side was hit.
        if (o.side != maker_side) continue;

        if (o.queue_pos > 0) {
            // Decrement queue position
            o.queue_pos -= traded_qty;
            if (o.queue_pos < 0) o.queue_pos = 0;
        } else {
            // Queue position drained — our order fills (optimistic model)
            o.filled_qty = o.qty;
            o.status     = OrderStatus::Filled;
            o.filled_at  = now;
        }
    }
}

void SimEngine::mark_to_market(double microprice_double) noexcept {
    // Convert microprice to fixed point for consistent arithmetic.
    const int64_t mp_e8 = static_cast<int64_t>(microprice_double * FIXED_SCALE);

    for (auto& o : orders_) {
        if (o.status != OrderStatus::Filled) continue;
        // For a filled buy: pnl = (microprice - fill_price) * qty
        // For a filled sell: pnl = (fill_price - microprice) * qty
        // Both in fixed point (×1e16 before dividing by FIXED_SCALE once)
        if (o.side == Side::Bid) {
            o.pnl_e8 = static_cast<int64_t>(
                static_cast<__int128>(mp_e8 - o.price) * o.qty / FIXED_SCALE);
        } else {
            o.pnl_e8 = static_cast<int64_t>(
                static_cast<__int128>(o.price - mp_e8) * o.qty / FIXED_SCALE);
        }
    }
}

int64_t SimEngine::total_pnl_e8() const noexcept {
    int64_t total = 0;
    for (const auto& o : orders_) {
        total += o.pnl_e8;
    }
    return total;
}

void SimEngine::compact(NsPoint now, uint64_t age_ns) noexcept {
    using namespace std::chrono;
    const auto cutoff = now - nanoseconds(age_ns);
    orders_.erase(
        std::remove_if(orders_.begin(), orders_.end(),
            [&](const VirtualOrder& o) {
                return (o.status == OrderStatus::Filled ||
                        o.status == OrderStatus::Cancelled) &&
                       o.filled_at < cutoff;
            }),
        orders_.end());
}

} // namespace marketpulse

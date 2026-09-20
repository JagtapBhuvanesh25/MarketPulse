#pragma once
// SPDX-License-Identifier: MIT
// sim.hpp — Virtual limit order placement and queue-position model.
//
// MODEL:
//   On placement: queue_position = total resting quantity at that price level.
//   On trade at that price: decrement queue_position by traded quantity.
//   Fill: when queue_position <= 0 and additional volume trades through.
//   Mark-to-market PnL: (microprice - fill_price) × qty for buys (inverted for sells).
//
// LIMITATION (documented per spec):
//   An L2 diff feed cannot distinguish cancellations ahead of you from fills.
//   Real queue position decays faster than modelled. Reported fill rates should
//   be read as an upper bound. Correcting this requires an L3/MBO feed.
//
// THREAD SAFETY: SimEngine is called from the book thread only.

#include "types.hpp"
#include <cstdint>
#include <vector>
#include <optional>

namespace marketpulse {

/// State of a single virtual limit order.
enum class OrderStatus : uint8_t {
    Pending,  // Placed, not yet filled
    Filled,   // Fully filled
    Cancelled // Manually cancelled
};

struct VirtualOrder {
    uint64_t    order_id{0};
    Side        side{Side::Bid};
    Price       price{PRICE_NONE};
    Qty         qty{QTY_ZERO};
    Qty         queue_pos{QTY_ZERO};  // remaining queue ahead of us
    Qty         filled_qty{QTY_ZERO};
    int64_t     pnl_e8{0};           // mark-to-market PnL in fixed point × 1e8
    OrderStatus status{OrderStatus::Pending};
    NsPoint     placed_at{};
    NsPoint     filled_at{};
};

/// SimEngine: manages a collection of virtual limit orders.
class SimEngine {
public:
    SimEngine() = default;

    //--------------------------------------------------------------------------
    // Order management
    //--------------------------------------------------------------------------

    /// Place a virtual limit order. Returns a unique order ID.
    /// @param resting_qty  The total resting quantity at `price` at placement time
    ///                     (i.e. what will be assigned as queue_position).
    [[nodiscard]] uint64_t place_order(Side side, Price price, Qty qty,
                                       Qty resting_qty, NsPoint now) noexcept;

    /// Cancel a pending order. Returns false if not found or already filled.
    bool cancel_order(uint64_t order_id) noexcept;

    //--------------------------------------------------------------------------
    // Event processing (called from book thread on every trade)
    //--------------------------------------------------------------------------

    /// Process a trade event: decrement queue position for orders at that price.
    void on_trade(Side maker_side, Price price, Qty traded_qty, NsPoint now) noexcept;

    //--------------------------------------------------------------------------
    // Mark-to-market
    //--------------------------------------------------------------------------

    /// Update PnL for filled orders based on current microprice.
    void mark_to_market(double microprice_double) noexcept;

    //--------------------------------------------------------------------------
    // Accessors
    //--------------------------------------------------------------------------

    [[nodiscard]] const std::vector<VirtualOrder>& orders() const noexcept { return orders_; }
    [[nodiscard]] std::size_t order_count() const noexcept { return orders_.size(); }

    /// Total realised + unrealised PnL across all filled orders (× 1e8).
    [[nodiscard]] int64_t total_pnl_e8() const noexcept;

    /// Compact filled orders older than `age_ns` nanoseconds.
    void compact(NsPoint now, uint64_t age_ns = 3'600'000'000'000ULL) noexcept; // 1 hour

private:
    std::vector<VirtualOrder> orders_;
    uint64_t next_id_{1};
};

} // namespace marketpulse

#pragma once
// SPDX-License-Identifier: MIT
// feed_binance.hpp — Exchange-specific WebSocket feed management for Binance spot.
//
// Streams consumed:
//   wss://stream.binance.com:9443/ws/btcusdt@depth@100ms  (depth diff)
//   wss://stream.binance.com:9443/ws/btcusdt@trade        (trades)
//   wss://stream.binance.com:9443/ws/btcusdt@bookTicker   (cross-check)
//
// REST snapshot:
//   https://api.binance.com/api/v3/depth?symbol=BTCUSDT&limit=1000
//
// The feed layer is behind this header (not inline in main.cpp) so a Coinbase
// or OKX adapter can be swapped in when the exchange blocks cloud IP ranges.
//
// THREADING: All callbacks are called from IXWebSocket's internal thread.
// The only data shared with other threads is the SPSC ring (push-only here).

#include "types.hpp"
#include "sequencer.hpp"
#include "spsc_ring.hpp"
#include "book_iface.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// Forward-declare to avoid including IXWebSocket in every TU.
namespace ix { class WebSocket; }

namespace marketpulse {

/// Message tag — tells the book thread what kind of update arrived.
enum class MsgTag : uint8_t { BookDiff, Trade, BookTicker };

/// Tagged union pushed into the SPSC ring.
/// Must be trivially copyable (ring requirement).
struct RingMsg {
    MsgTag tag{MsgTag::BookDiff};

    union {
        BookUpdate diff;
        Trade      trade;
        // BookTicker: just best bid/ask for live cross-check
        struct { Price bid_px; Qty bid_qty; Price ask_px; Qty ask_qty; } ticker;
    };

    NsPoint t_recv{};
};
static_assert(std::is_trivially_copyable_v<RingMsg>,
              "RingMsg must be trivially copyable for the SPSC ring");

/// Snapshot level from the REST response (a single price rung).
struct SnapLevel { Price price; Qty qty; };

/// Callback invoked once when the REST snapshot is ready.
/// Caller applies the snapshot to IBook, then calls Sequencer::on_snapshot().
using SnapshotReadyCb = std::function<void(uint64_t last_update_id,
                                           std::vector<SnapLevel> bids,
                                           std::vector<SnapLevel> asks)>;

/// FeedBinance: manages all WebSocket connections and the REST snapshot fetch.
///
/// Lifecycle:
///   1. Construct with a ring, sequencer, and snapshot callback.
///   2. Call start(symbol) — opens connections, begins buffering.
///   3. Call stop() — closes all connections cleanly.
///
/// On Sequencer::process_diff() returning Resync, the feed triggers a full
/// restart automatically (closes, reopens, re-fetches snapshot).
class FeedBinance {
public:
    static constexpr std::size_t RING_CAPACITY = 4096; // power of two
    using Ring = SpscRing<RingMsg, RING_CAPACITY>;

    FeedBinance(Ring& ring,
                Sequencer& seq,
                SnapshotReadyCb snapshot_cb,
                std::function<void()> resync_cb = {});
    ~FeedBinance();

    /// Start streaming. Non-blocking: WS threads run in the background.
    void start(std::string_view symbol);

    /// Stop all connections gracefully.
    void stop();

    /// Drop counter: how many RingMsg pushes were dropped due to a full ring.
    [[nodiscard]] uint64_t drop_count() const noexcept {
        return drop_count_.load(std::memory_order_relaxed);
    }

    /// Reset drop counter (called periodically by the broadcast thread).
    void reset_drop_count() noexcept {
        drop_count_.store(0, std::memory_order_relaxed);
    }

private:
    // WebSocket message handlers (called from WS thread)
    void on_depth_message(std::string_view raw, NsPoint t_recv);
    void on_trade_message(std::string_view raw, NsPoint t_recv);
    void on_ticker_message(std::string_view raw, NsPoint t_recv);

    // REST snapshot fetch (called in a dedicated thread after WS connects)
    void fetch_snapshot(std::string_view symbol);

    // Push to ring, increment drop counter on failure
    void push_or_drop(const RingMsg& msg) noexcept;

    // Trigger a full resync (runs on WS thread)
    void trigger_resync() noexcept;

    Ring&           ring_;
    Sequencer&      seq_;
    SnapshotReadyCb snapshot_cb_;
    std::function<void()> resync_cb_;

    std::string symbol_upper_; // "BTCUSDT"

    std::shared_ptr<ix::WebSocket> ws_depth_;
    std::shared_ptr<ix::WebSocket> ws_trade_;
    std::shared_ptr<ix::WebSocket> ws_ticker_;

    std::atomic<uint64_t> drop_count_{0};
    std::atomic<bool>     running_{false};
};

} // namespace marketpulse

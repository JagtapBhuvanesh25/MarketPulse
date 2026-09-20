#pragma once
// SPDX-License-Identifier: MIT
// broadcast.hpp — Downstream WebSocket server (IXWebSocket) at 10 Hz.
//
// DESIGN:
//   The book thread calls update_state() after every tick (potentially 100/s).
//   A dedicated broadcast thread wakes at 10 Hz, serialises the latest state,
//   and sends it to all connected clients (coalescing intermediate ticks).
//
//   Message format (JSON):
//   {
//     "bids": [[price_str, qty_str], ...],  // top 20 levels
//     "asks": [[price_str, qty_str], ...],
//     "microprice": 65432.10,
//     "ofi": 1234.5,
//     "trade_imbalance": 0.12,
//     "latency": { "p50": 1500, "p99": 8000, "p999": 25000, "max": 120000 }, // ns
//     "resyncs": 0,
//     "ring_drops": 0,
//     "uptime_s": 3600,
//     "msgs_per_sec": 95.3,
//     "positions": [ { "side": "bid", "price": "65400.00", "qty": "0.01",
//                      "queue_pos": "0.05", "filled": false, "pnl": "-1.23" } ]
//   }
//
// THREAD SAFETY:
//   update_state() is called from the book thread.
//   The broadcast loop is on a separate thread.
//   A single atomic pointer swap ensures the broadcast thread always sees
//   the latest state without blocking the book thread.

#include "types.hpp"
#include "latency.hpp"
#include "signals.hpp"
#include "sim.hpp"
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>
#include <chrono>

namespace marketpulse {

/// SnapshotState: everything the broadcast thread needs to serialise one frame.
/// The book thread constructs this cheaply and swaps it in atomically.
struct SnapshotState {
    // Top 20 levels per side (best first)
    static constexpr int MAX_LEVELS = 20;
    std::array<BookLevel, MAX_LEVELS> bids{};
    std::array<BookLevel, MAX_LEVELS> asks{};
    int bid_count{0};
    int ask_count{0};

    // Signals
    Signals signals{};

    // Latency percentiles (last 10-second window)
    LatencyPercentiles latency{};

    // Counters
    uint64_t resyncs{0};
    uint64_t ring_drops{0};
    uint64_t uptime_s{0};
    double   msgs_per_sec{0.0};

    // Virtual positions (copy — small enough)
    std::vector<VirtualOrder> positions{};
};

/// BroadcastServer: IXWebSocket HTTP+WS server on a configurable port.
class BroadcastServer {
public:
    explicit BroadcastServer(uint16_t port = 9001);
    ~BroadcastServer();

    /// Start the server (spawns internal threads).
    void start();

    /// Stop the server and wait for threads to finish.
    void stop();

    //--------------------------------------------------------------------------
    // Called from book thread after each tick — must be fast (just an atomic swap)
    //--------------------------------------------------------------------------

    void update_state(SnapshotState state) noexcept;

    //--------------------------------------------------------------------------
    // Stats
    //--------------------------------------------------------------------------

    [[nodiscard]] std::size_t client_count() const noexcept;

private:
    void broadcast_loop();
    [[nodiscard]] std::string serialise(const SnapshotState& s) const;

    uint16_t port_;

    // Ping-pong buffers: book thread writes into inactive, swaps atomically.
    std::unique_ptr<SnapshotState> state_a_;
    std::unique_ptr<SnapshotState> state_b_;
    std::atomic<SnapshotState*>    active_state_{nullptr};
    std::atomic<bool>              state_dirty_{false};

    std::thread broadcast_thread_;
    std::atomic<bool> running_{false};

    // IXWebSocket server (opaque to avoid including ix headers everywhere)
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace marketpulse

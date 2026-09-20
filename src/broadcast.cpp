// SPDX-License-Identifier: MIT
// broadcast.cpp — IXWebSocket server at 10 Hz with atomic state swap.

#include "marketpulse/broadcast.hpp"

#include <ixwebsocket/IXWebSocketServer.h>
#include <ixwebsocket/IXWebSocket.h>

#include <chrono>
#include <cstdio>
#include <cinttypes>
#include <iomanip>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>

namespace marketpulse {

//-----------------------------------------------------------------------------
// Pimpl: hold the IXWebSocket server and the list of connected clients.
//-----------------------------------------------------------------------------

struct BroadcastServer::Impl {
    ix::WebSocketServer server;
    std::mutex          clients_mu;
    std::set<std::shared_ptr<ix::WebSocket>> clients;

    explicit Impl(uint16_t port) : server(port) {}
};

//-----------------------------------------------------------------------------
// Construction
//-----------------------------------------------------------------------------

BroadcastServer::BroadcastServer(uint16_t port)
    : port_(port),
      state_a_(std::make_unique<SnapshotState>()),
      state_b_(std::make_unique<SnapshotState>()),
      impl_(std::make_unique<Impl>(port))
{
    active_state_.store(state_a_.get(), std::memory_order_release);
}

BroadcastServer::~BroadcastServer() { stop(); }

//-----------------------------------------------------------------------------
// Start / Stop
//-----------------------------------------------------------------------------

void BroadcastServer::start() {
    running_.store(true, std::memory_order_release);

    // Configure IXWebSocket server
    // NOTE: IXWebSocket v11 passes a weak_ptr<WebSocket>, not shared_ptr.
    impl_->server.setOnConnectionCallback(
        [this](std::weak_ptr<ix::WebSocket> ws_weak,
               std::shared_ptr<ix::ConnectionState> /*state*/) {
            auto ws = ws_weak.lock();
            if (!ws) return;

            ws->setOnMessageCallback([this, ws_ptr = ws.get()]
                (const ix::WebSocketMessagePtr& msg) {
                // We don't process incoming messages from the browser.
                if (msg->type == ix::WebSocketMessageType::Close) {
                    std::lock_guard<std::mutex> lk(impl_->clients_mu);
                    // Remove by raw pointer comparison
                    for (auto it = impl_->clients.begin();
                         it != impl_->clients.end(); ++it) {
                        if (it->get() == ws_ptr) {
                            impl_->clients.erase(it);
                            break;
                        }
                    }
                }
            });
            {
                std::lock_guard<std::mutex> lk(impl_->clients_mu);
                impl_->clients.insert(ws);
            }
        });

    auto [ok, err] = impl_->server.listen();
    if (!ok) {
        std::fprintf(stderr, "[broadcast] listen() failed: %s\n", err.c_str());
        return;
    }
    impl_->server.start();

    // 10 Hz broadcast loop
    broadcast_thread_ = std::thread([this]() { broadcast_loop(); });
}

void BroadcastServer::stop() {
    running_.store(false, std::memory_order_release);
    if (broadcast_thread_.joinable()) broadcast_thread_.join();
    impl_->server.stop();
}

//-----------------------------------------------------------------------------
// update_state: called from book thread (hot path) — must be fast
//-----------------------------------------------------------------------------

void BroadcastServer::update_state(SnapshotState state) noexcept {
    // Write into the inactive buffer, then atomically swap.
    SnapshotState* current = active_state_.load(std::memory_order_acquire);
    SnapshotState* inactive = (current == state_a_.get()) ? state_b_.get() : state_a_.get();
    *inactive = std::move(state);
    active_state_.store(inactive, std::memory_order_release);
    state_dirty_.store(true, std::memory_order_release);
}

//-----------------------------------------------------------------------------
// broadcast_loop: runs at 10 Hz on the broadcast thread
//-----------------------------------------------------------------------------

void BroadcastServer::broadcast_loop() {
    using namespace std::chrono;
    const auto interval = milliseconds(100); // 10 Hz
    auto next_tick = steady_clock::now() + interval;

    while (running_.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_until(next_tick);
        next_tick += interval;

        if (!state_dirty_.load(std::memory_order_acquire)) continue;
        state_dirty_.store(false, std::memory_order_release);

        const SnapshotState* snap = active_state_.load(std::memory_order_acquire);
        if (!snap) continue;

        const std::string json = serialise(*snap);

        // Broadcast to all connected clients
        std::lock_guard<std::mutex> lk(impl_->clients_mu);
        for (auto& ws : impl_->clients) {
            ws->send(json);
        }
    }
}

//-----------------------------------------------------------------------------
// serialise: build JSON string from snapshot state
//-----------------------------------------------------------------------------

static std::string price_to_str(Price p) {
    // Convert fixed-point price to 2 decimal places (for display)
    const int64_t integer  = p / FIXED_SCALE;
    const int64_t fraction = (p % FIXED_SCALE) / 1'000'000; // 2 decimal places
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%" PRId64 ".%02" PRId64, integer, fraction);
    return buf;
}

static std::string qty_to_str(Qty q) {
    // 8 decimal places (satoshi precision)
    const int64_t integer  = q / FIXED_SCALE;
    const int64_t fraction = q % FIXED_SCALE;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%" PRId64 ".%08" PRId64, integer, fraction);
    return buf;
}

std::string BroadcastServer::serialise(const SnapshotState& s) const {
    std::ostringstream o;
    o << std::fixed << std::setprecision(6);
    o << "{\"bids\":[";
    for (int i = 0; i < s.bid_count; ++i) {
        if (i) o << ',';
        o << "[\"" << price_to_str(s.bids[i].price)
          << "\",\"" << qty_to_str(s.bids[i].qty) << "\"]";
    }
    o << "],\"asks\":[";
    for (int i = 0; i < s.ask_count; ++i) {
        if (i) o << ',';
        o << "[\"" << price_to_str(s.asks[i].price)
          << "\",\"" << qty_to_str(s.asks[i].qty) << "\"]";
    }
    o << "],\"microprice\":" << s.signals.microprice
      << ",\"ofi\":"        << s.signals.ofi
      << ",\"trade_imbalance\":" << s.signals.trade_imbalance
      << ",\"latency\":{"
        << "\"parse_p50\":"  << s.latency.parse_p50_ns
        << ",\"parse_p99\":" << s.latency.parse_p99_ns
        << ",\"parse_p999\":" << s.latency.parse_p999_ns
        << ",\"parse_max\":" << s.latency.parse_max_ns
        << ",\"total_p50\":" << s.latency.total_p50_ns
        << ",\"total_p99\":" << s.latency.total_p99_ns
        << ",\"total_p999\":" << s.latency.total_p999_ns
        << ",\"total_max\":" << s.latency.total_max_ns
        << ",\"count\":"     << s.latency.update_count
      << "}"
      << ",\"resyncs\":"    << s.resyncs
      << ",\"ring_drops\":" << s.ring_drops
      << ",\"uptime_s\":"   << s.uptime_s
      << ",\"msgs_per_sec\":" << s.msgs_per_sec
      << ",\"positions\":[";

    bool first_pos = true;
    for (const auto& pos : s.positions) {
        if (pos.status != OrderStatus::Pending && pos.status != OrderStatus::Filled) continue;
        if (!first_pos) o << ',';
        first_pos = false;
        o << "{\"id\":" << pos.order_id
          << ",\"side\":\"" << (pos.side == Side::Bid ? "bid" : "ask") << "\""
          << ",\"price\":\"" << price_to_str(pos.price) << "\""
          << ",\"qty\":\"" << qty_to_str(pos.qty) << "\""
          << ",\"filled\":" << (pos.status == OrderStatus::Filled ? "true" : "false")
          << ",\"pnl\":" << (static_cast<double>(pos.pnl_e8) / FIXED_SCALE)
          << "}";
    }
    o << "]}";
    return o.str();
}

std::size_t BroadcastServer::client_count() const noexcept {
    std::lock_guard<std::mutex> lk(impl_->clients_mu);
    return impl_->clients.size();
}

} // namespace marketpulse

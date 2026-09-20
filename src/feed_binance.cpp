// SPDX-License-Identifier: MIT
// feed_binance.cpp — Binance WebSocket feed and REST snapshot implementation.

#include "marketpulse/feed_binance.hpp"

// IXWebSocket headers
#include <ixwebsocket/IXWebSocket.h>
#include <ixwebsocket/IXHttpClient.h>

// simdjson on-demand API
#include <simdjson.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace marketpulse {

//-----------------------------------------------------------------------------
// Helpers
//-----------------------------------------------------------------------------

static std::string to_upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c){ return std::toupper(c); });
    return s;
}

static std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c){ return std::tolower(c); });
    return s;
}

//-----------------------------------------------------------------------------
// Parse helpers using simdjson on-demand API
//-----------------------------------------------------------------------------

/// Parse a single depth diff event from the WebSocket message.
/// Returns false on parse error.
static bool parse_depth_event(std::string_view raw, NsPoint t_recv,
                               std::vector<BookUpdate>& out)
{
    thread_local simdjson::ondemand::parser parser;
    simdjson::padded_string ps(raw.data(), raw.size());
    auto doc = parser.iterate(ps);
    if (doc.error()) return false;

    // Expected fields: e, E, s, U, u, pu, b (bids), a (asks)
    uint64_t first_id = 0, last_id = 0, prev_id = 0;

    if (doc["U"].get_uint64().get(first_id)) return false;
    if (doc["u"].get_uint64().get(last_id))  return false;
    // pu is present in @depth@100ms stream (spot)
    (void)doc["pu"].get_uint64().get(prev_id); // ignore error — may not be present

    NsPoint t_parsed = std::chrono::steady_clock::now();

    // Parse bids
    auto bids_arr = doc["b"].get_array();
    if (!bids_arr.error()) {
        for (auto level : bids_arr) {
            auto pair = level.get_array();
            std::string_view px_sv, qty_sv;
            if (pair.error()) continue;
            auto it = pair.begin();
            if ((*it).get_string().get(px_sv)) continue;
            ++it;
            if ((*it).get_string().get(qty_sv)) continue;

            Price price = 0; Qty qty = 0;
            if (!parse_fixed(px_sv, price))  continue;
            if (!parse_fixed(qty_sv, qty))   continue;

            out.push_back(BookUpdate{
                .side     = Side::Bid,
                .price    = price,
                .qty      = qty,
                .first_id = first_id,
                .last_id  = last_id,
                .prev_id  = prev_id,
                .t_recv   = t_recv,
                .t_parsed = t_parsed,
            });
        }
    }

    // Parse asks
    auto asks_arr = doc["a"].get_array();
    if (!asks_arr.error()) {
        for (auto level : asks_arr) {
            auto pair = level.get_array();
            std::string_view px_sv, qty_sv;
            if (pair.error()) continue;
            auto it = pair.begin();
            if ((*it).get_string().get(px_sv)) continue;
            ++it;
            if ((*it).get_string().get(qty_sv)) continue;

            Price price = 0; Qty qty = 0;
            if (!parse_fixed(px_sv, price))  continue;
            if (!parse_fixed(qty_sv, qty))   continue;

            out.push_back(BookUpdate{
                .side     = Side::Ask,
                .price    = price,
                .qty      = qty,
                .first_id = first_id,
                .last_id  = last_id,
                .prev_id  = prev_id,
                .t_recv   = t_recv,
                .t_parsed = t_parsed,
            });
        }
    }

    return true;
}

//-----------------------------------------------------------------------------
// FeedBinance
//-----------------------------------------------------------------------------

FeedBinance::FeedBinance(Ring& ring, Sequencer& seq,
                         SnapshotReadyCb snapshot_cb,
                         std::function<void()> resync_cb)
    : ring_(ring), seq_(seq),
      snapshot_cb_(std::move(snapshot_cb)),
      resync_cb_(std::move(resync_cb))
{}

FeedBinance::~FeedBinance() { stop(); }

void FeedBinance::start(std::string_view symbol) {
    symbol_upper_ = to_upper(std::string(symbol));
    const std::string sym_lower = to_lower(std::string(symbol));
    running_.store(true, std::memory_order_release);

    seq_.on_connected();

    // Open depth stream
    ws_depth_ = std::make_shared<ix::WebSocket>();
    ws_depth_->setUrl("wss://stream.binance.com:9443/ws/" + sym_lower + "@depth@100ms");
    ws_depth_->setOnMessageCallback([this](const ix::WebSocketMessagePtr& msg) {
        if (!running_.load(std::memory_order_relaxed)) return;
        if (msg->type == ix::WebSocketMessageType::Message) {
            const NsPoint t_recv = std::chrono::steady_clock::now();
            on_depth_message(msg->str, t_recv);
        } else if (msg->type == ix::WebSocketMessageType::Close ||
                   msg->type == ix::WebSocketMessageType::Error) {
            seq_.on_disconnected("depth stream closed/error");
            trigger_resync();
        }
    });
    ws_depth_->start();

    // Open trade stream
    ws_trade_ = std::make_shared<ix::WebSocket>();
    ws_trade_->setUrl("wss://stream.binance.com:9443/ws/" + sym_lower + "@trade");
    ws_trade_->setOnMessageCallback([this](const ix::WebSocketMessagePtr& msg) {
        if (!running_.load(std::memory_order_relaxed)) return;
        if (msg->type == ix::WebSocketMessageType::Message) {
            const NsPoint t_recv = std::chrono::steady_clock::now();
            on_trade_message(msg->str, t_recv);
        }
    });
    ws_trade_->start();

    // Open bookTicker stream (for live cross-check)
    ws_ticker_ = std::make_shared<ix::WebSocket>();
    ws_ticker_->setUrl("wss://stream.binance.com:9443/ws/" + sym_lower + "@bookTicker");
    ws_ticker_->setOnMessageCallback([this](const ix::WebSocketMessagePtr& msg) {
        if (!running_.load(std::memory_order_relaxed)) return;
        if (msg->type == ix::WebSocketMessageType::Message) {
            const NsPoint t_recv = std::chrono::steady_clock::now();
            on_ticker_message(msg->str, t_recv);
        }
    });
    ws_ticker_->start();

    // Fetch snapshot in background thread (depth WS is already buffering)
    std::thread([this, sym_lower]() {
        // Small delay to let the WS connection establish and buffer a few messages
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        fetch_snapshot(sym_lower);
    }).detach();
}

void FeedBinance::stop() {
    running_.store(false, std::memory_order_release);
    if (ws_depth_)  { ws_depth_->stop();  ws_depth_.reset(); }
    if (ws_trade_)  { ws_trade_->stop();  ws_trade_.reset(); }
    if (ws_ticker_) { ws_ticker_->stop(); ws_ticker_.reset(); }
}

//-----------------------------------------------------------------------------
// Private: message handlers
//-----------------------------------------------------------------------------

void FeedBinance::on_depth_message(std::string_view raw, NsPoint t_recv) {
    std::vector<BookUpdate> updates;
    updates.reserve(32);

    if (!parse_depth_event(raw, t_recv, updates)) {
        std::fprintf(stderr, "[feed] Failed to parse depth event\n");
        return;
    }

    // For each parsed level, process through sequencer then push to ring.
    // NOTE: All levels in one event share the same first_id/last_id/prev_id,
    // so we only check the sequencer once per event (using the first update).
    if (updates.empty()) return;

    const SeqResult result = seq_.process_diff(updates.front());

    switch (result) {
        case SeqResult::Drop:
            return;
        case SeqResult::Buffer:
            return; // sequencer already buffered it
        case SeqResult::Resync:
            if (resync_cb_) resync_cb_();
            trigger_resync();
            return;
        case SeqResult::Apply:
            for (const auto& upd : updates) {
                push_or_drop(RingMsg{.tag = MsgTag::BookDiff, .diff = upd, .t_recv = t_recv});
            }
            break;
    }
}

void FeedBinance::on_trade_message(std::string_view raw, NsPoint t_recv) {
    thread_local simdjson::ondemand::parser parser;
    simdjson::padded_string ps(raw.data(), raw.size());
    auto doc = parser.iterate(ps);
    if (doc.error()) return;

    // Binance trade event fields:
    // p = price, q = quantity, m = is_buyer_maker
    Trade trade{};
    trade.t_recv = t_recv;

    std::string_view px_sv, qty_sv;
    bool is_buyer_maker = false;

    if (doc["p"].get_string().get(px_sv))   return;
    if (doc["q"].get_string().get(qty_sv))  return;
    (void)doc["m"].get_bool().get(is_buyer_maker);
    (void)doc["T"].get_uint64().get(trade.event_time_ms);
    (void)doc["t"].get_uint64().get(trade.trade_id);

    if (!parse_fixed(px_sv, trade.price))  return;
    if (!parse_fixed(qty_sv, trade.qty))   return;

    // is_buyer_maker = true means buyer is the maker → seller is taker → aggressive sell.
    // We store the maker side (the resting side).
    trade.side = is_buyer_maker ? Side::Bid : Side::Ask;

    RingMsg msg{};
    msg.tag   = MsgTag::Trade;
    msg.trade = trade;
    msg.t_recv = t_recv;
    push_or_drop(msg);
}

void FeedBinance::on_ticker_message(std::string_view raw, NsPoint t_recv) {
    thread_local simdjson::ondemand::parser parser;
    simdjson::padded_string ps(raw.data(), raw.size());
    auto doc = parser.iterate(ps);
    if (doc.error()) return;

    std::string_view b_sv, B_sv, a_sv, A_sv;
    if (doc["b"].get_string().get(b_sv)) return;
    if (doc["B"].get_string().get(B_sv)) return;
    if (doc["a"].get_string().get(a_sv)) return;
    if (doc["A"].get_string().get(A_sv)) return;

    RingMsg msg{};
    msg.tag = MsgTag::BookTicker;
    msg.t_recv = t_recv;
    if (!parse_fixed(b_sv, msg.ticker.bid_px)) return;
    if (!parse_fixed(B_sv, msg.ticker.bid_qty)) return;
    if (!parse_fixed(a_sv, msg.ticker.ask_px)) return;
    if (!parse_fixed(A_sv, msg.ticker.ask_qty)) return;
    push_or_drop(msg);
}

//-----------------------------------------------------------------------------
// Private: REST snapshot fetch
//-----------------------------------------------------------------------------

void FeedBinance::fetch_snapshot(std::string_view symbol) {
    const std::string url = "https://api.binance.com/api/v3/depth?symbol="
                           + to_upper(std::string(symbol)) + "&limit=1000";

    ix::HttpClient http;
    ix::HttpRequestArgsPtr args = http.createRequest(url);
    args->connectTimeout = 10;
    args->transferTimeout = 30;

    const auto response = http.get(url, args);
    if (response->statusCode != 200) {
        std::fprintf(stderr, "[feed] Snapshot HTTP error %d\n", response->statusCode);
        trigger_resync();
        return;
    }

    // Parse snapshot
    simdjson::ondemand::parser parser;
    simdjson::padded_string ps(response->body);
    auto doc = parser.iterate(ps);
    if (doc.error()) {
        std::fprintf(stderr, "[feed] Snapshot parse error\n");
        trigger_resync();
        return;
    }

    uint64_t last_update_id = 0;
    if (doc["lastUpdateId"].get_uint64().get(last_update_id)) {
        std::fprintf(stderr, "[feed] Snapshot missing lastUpdateId\n");
        trigger_resync();
        return;
    }

    std::vector<SnapLevel> bids, asks;

    auto parse_levels = [&](auto arr, Side side) {
        auto& vec = (side == Side::Bid) ? bids : asks;
        for (auto item : arr) {
            auto pair = item.get_array();
            if (pair.error()) continue;
            auto it = pair.begin();
            std::string_view pxsv, qtysv;
            if ((*it).get_string().get(pxsv)) continue;
            ++it;
            if ((*it).get_string().get(qtysv)) continue;
            Price p = 0; Qty q = 0;
            if (parse_fixed(pxsv, p) && parse_fixed(qtysv, q)) {
                vec.push_back({p, q});
            }
        }
    };

    parse_levels(doc["bids"].get_array(), Side::Bid);
    parse_levels(doc["asks"].get_array(), Side::Ask);

    if (snapshot_cb_) {
        snapshot_cb_(last_update_id, std::move(bids), std::move(asks));
    }
}

//-----------------------------------------------------------------------------
// Private: helpers
//-----------------------------------------------------------------------------

void FeedBinance::push_or_drop(const RingMsg& msg) noexcept {
    if (!ring_.push(msg)) {
        drop_count_.fetch_add(1, std::memory_order_relaxed);
    }
}

void FeedBinance::trigger_resync() noexcept {
    // Called when a Resync is needed. Stop current connections, restart.
    // Runs on the WS callback thread — we detach a new thread to avoid deadlock.
    if (!running_.load(std::memory_order_relaxed)) return;
    std::thread([this]() {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        if (running_.load(std::memory_order_relaxed)) {
            // stop() then start() is too invasive from a detached thread;
            // the main resync_cb_ handles the reconnect logic.
            if (resync_cb_) resync_cb_();
        }
    }).detach();
}

} // namespace marketpulse

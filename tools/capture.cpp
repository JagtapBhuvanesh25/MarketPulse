// SPDX-License-Identifier: MIT
// tools/capture.cpp — Record live Binance feed to JSONL for replay.
//
// Usage:
//   ./build/capture --symbol btcusdt -o out.jsonl
//
// Each line written:
//   {"ts_ns":<nanoseconds>,"stream":"<stream_name>","msg":<raw_json>}
//
// Exits cleanly on SIGINT.

#include "marketpulse/types.hpp"

#include <ixwebsocket/IXWebSocket.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

static std::atomic<bool> g_stop{false};
static void handle_signal(int) noexcept { g_stop.store(true, std::memory_order_release); }

static std::string to_lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

int main(int argc, char* argv[]) {
    std::signal(SIGINT,  handle_signal);
    std::signal(SIGTERM, handle_signal);

    std::string symbol = "btcusdt";
    std::string outfile = "capture.jsonl";

    for (int i = 1; i < argc; ++i) {
        std::string_view arg(argv[i]);
        if ((arg == "--symbol" || arg == "-s") && i + 1 < argc) {
            symbol = argv[++i];
        } else if ((arg == "-o" || arg == "--output") && i + 1 < argc) {
            outfile = argv[++i];
        } else if (arg.starts_with("--symbol=")) {
            symbol = std::string(arg.substr(9));
        }
    }

    const std::string sym = to_lower(symbol);
    std::printf("Capturing %s → %s  (Ctrl-C to stop)\n", sym.c_str(), outfile.c_str());

    std::ofstream fout(outfile);
    if (!fout.is_open()) {
        std::fprintf(stderr, "Cannot open output file: %s\n", outfile.c_str());
        return 1;
    }

    std::mutex file_mu;

    auto make_ws = [&](const std::string& url, const std::string& stream_name)
        -> std::shared_ptr<ix::WebSocket>
    {
        auto ws = std::make_shared<ix::WebSocket>();
        ws->setUrl(url);
        ws->setOnMessageCallback([&, stream_name](const ix::WebSocketMessagePtr& msg) {
            if (msg->type != ix::WebSocketMessageType::Message) return;

            const auto ts = std::chrono::steady_clock::now().time_since_epoch().count();
            // Write JSONL line: {"ts_ns":NNN,"stream":"NAME","msg":{...}}
            std::lock_guard<std::mutex> lk(file_mu);
            fout << "{\"ts_ns\":" << ts
                 << ",\"stream\":\"" << stream_name << "\""
                 << ",\"msg\":" << msg->str
                 << "}\n";
        });
        ws->start();
        return ws;
    };

    auto ws_depth  = make_ws("wss://stream.binance.com:9443/ws/" + sym + "@depth@100ms", "depth");
    auto ws_trade  = make_ws("wss://stream.binance.com:9443/ws/" + sym + "@trade",       "trade");
    auto ws_ticker = make_ws("wss://stream.binance.com:9443/ws/" + sym + "@bookTicker",  "bookTicker");

    uint64_t lines = 0;
    while (!g_stop.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        const auto size = fout.tellp();
        std::printf("\r  Recorded %.1f KB  ", static_cast<double>(size) / 1024.0);
        std::fflush(stdout);
    }

    ws_depth->stop();
    ws_trade->stop();
    ws_ticker->stop();
    fout.flush();
    fout.close();

    std::printf("\nDone. Output: %s\n", outfile.c_str());
    return 0;
}

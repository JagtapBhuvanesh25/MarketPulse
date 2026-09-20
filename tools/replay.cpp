// SPDX-License-Identifier: MIT
// tools/replay.cpp — Replay a JSONL fixture through the pipeline.
//
// Usage:
//   ./build/replay <fixture.jsonl> [--speed=0|1]
//     --speed=0  (default): replay as fast as possible
//     --speed=1           : replay at original wall-clock pace
//
// Replays the file and prints throughput/latency summary at the end.

#include "marketpulse/types.hpp"
#include "marketpulse/spsc_ring.hpp"
#include "marketpulse/sequencer.hpp"
#include "marketpulse/book_map.hpp"
#include "marketpulse/book_ladder.hpp"
#include "marketpulse/signals.hpp"
#include "marketpulse/latency.hpp"
#include "marketpulse/sim.hpp"

#include <simdjson.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

using namespace marketpulse;

using Ring = SpscRing<RingMsg, 4096>;

int main(int argc, char* argv[]) {
    std::string fixture;
    bool fast_mode = true; // --speed=0

    for (int i = 1; i < argc; ++i) {
        std::string_view arg(argv[i]);
        if (arg == "--speed=1") fast_mode = false;
        else if (arg[0] != '-') fixture = std::string(arg);
    }

    if (fixture.empty()) {
        std::fprintf(stderr, "Usage: replay <fixture.jsonl> [--speed=0|1]\n");
        return 1;
    }

    std::ifstream fin(fixture);
    if (!fin.is_open()) {
        std::fprintf(stderr, "Cannot open: %s\n", fixture.c_str());
        return 1;
    }

    Ring ring;
    Sequencer seq([](SeqState from, SeqState to, std::string_view reason) {
        std::printf("[seq] %s → %s: %.*s\n",
            seq_state_name(from).data(), seq_state_name(to).data(),
            (int)reason.size(), reason.data());
    });

    BookLadder book;
    LatencyRecorder lat;
    SimEngine sim;

    // Set up sequencer
    seq.on_connected();
    std::vector<BookUpdate> drained;
    uint64_t last_update_id = 0;

    // Read header if present
    std::string first_line;
    std::ifstream::pos_type body_start = 0;
    if (std::getline(fin, first_line) && first_line.starts_with("# SYNTHETIC")) {
        const auto pos = first_line.find("lastUpdateId=");
        if (pos != std::string::npos) {
            last_update_id = std::stoull(first_line.substr(pos + 13));
        }
        body_start = fin.tellg();
    } else {
        fin.seekg(0);
    }

    seq.on_snapshot(last_update_id, drained);

    // Book thread
    std::atomic<bool> stop{false};
    uint64_t total_updates = 0;

    std::thread book_thr([&]() {
        RingMsg msg{};
        RollingTradeWindow<2048> trade_win;
        while (!stop.load(std::memory_order_relaxed) || ring.size_approx() > 0) {
            if (!ring.pop(msg)) continue;
            const NsPoint t_pop = std::chrono::steady_clock::now();
            if (msg.tag == MsgTag::BookDiff) {
                const auto& upd = msg.diff;
                lat.record_queue(elapsed_ns(upd.t_parsed, t_pop));
                book.apply(upd.side, upd.price, upd.qty);
                lat.record_parse(elapsed_ns(upd.t_recv, upd.t_parsed));
                const NsPoint t_sig = std::chrono::steady_clock::now();
                lat.record_total(elapsed_ns(upd.t_recv, t_sig));
                ++total_updates;
            } else if (msg.tag == MsgTag::Trade) {
                trade_win.push(msg.trade.t_recv, msg.trade.qty,
                               msg.trade.side == Side::Ask);
            }
        }
    });

    simdjson::ondemand::parser parser;
    std::string line;
    uint64_t prev_ts_ns = 0;
    uint64_t drop_count = 0;
    auto wall_start = std::chrono::steady_clock::now();

    while (std::getline(fin, line)) {
        if (line.empty() || line[0] == '#') continue;

        simdjson::padded_string ps(line.data(), line.size());
        auto doc = parser.iterate(ps);
        if (doc.error()) continue;

        uint64_t ts_ns = 0;
        if (doc["ts_ns"].get_uint64().get(ts_ns)) {}

        // Wall-clock pacing
        if (!fast_mode && prev_ts_ns > 0 && ts_ns > prev_ts_ns) {
            const uint64_t delay_ns = ts_ns - prev_ts_ns;
            if (delay_ns < 100'000'000ULL) { // cap at 100ms
                const auto deadline = wall_start + std::chrono::nanoseconds(ts_ns);
                std::this_thread::sleep_until(deadline);
            }
        }
        prev_ts_ns = ts_ns;

        std::string_view stream_sv;
        if (doc["stream"].get_string().get(stream_sv)) continue;

        const NsPoint t_recv = std::chrono::steady_clock::now();

        simdjson::ondemand::value msg_val;
        if (doc["msg"].get(msg_val)) continue;
        const auto raw = simdjson::to_json_string(msg_val);
        if (raw.error()) continue;

        if (stream_sv.find("depth") != std::string_view::npos) {
            simdjson::padded_string inner(raw.value().data(), raw.value().size());
            auto inner_doc = parser.iterate(inner);
            uint64_t U = 0, u = 0, pu = 0;
            if (inner_doc["U"].get_uint64().get(U)) continue;
            if (inner_doc["u"].get_uint64().get(u)) continue;
            if (inner_doc["pu"].get_uint64().get(pu)) {}

            BookUpdate proto{};
            proto.first_id = U; proto.last_id = u; proto.prev_id = pu;
            proto.t_recv   = t_recv;
            proto.t_parsed = std::chrono::steady_clock::now();

            auto push_levels = [&](auto arr_res, Side side) {
                auto arr = arr_res;
                if (arr.error()) return;
                for (auto item : arr) {
                    auto pair = item.get_array();
                    if (pair.error()) continue;
                    auto it = pair.begin();
                    std::string_view pxsv, qtysv;
                    if ((*it).get_string().get(pxsv))  continue; ++it;
                    if ((*it).get_string().get(qtysv)) continue;
                    BookUpdate upd = proto;
                    upd.side = side;
                    if (!parse_fixed(pxsv, upd.price)) continue;
                    if (!parse_fixed(qtysv, upd.qty))  continue;
                    const SeqResult res = seq.process_diff(upd);
                    if (res == SeqResult::Apply) {
                        RingMsg rmsg{.tag = MsgTag::BookDiff, .diff = upd, .t_recv = t_recv};
                        if (!ring.push(rmsg)) ++drop_count;
                    }
                }
            };
            push_levels(inner_doc["b"].get_array(), Side::Bid);
            push_levels(inner_doc["a"].get_array(), Side::Ask);

        } else if (stream_sv.find("trade") != std::string_view::npos) {
            simdjson::padded_string inner(raw.value().data(), raw.value().size());
            auto inner_doc = parser.iterate(inner);
            Trade tr{};
            tr.t_recv = t_recv;
            std::string_view p_sv, q_sv;
            bool is_buyer_maker = false;
            if (inner_doc["p"].get_string().get(p_sv)) continue;
            if (inner_doc["q"].get_string().get(q_sv)) continue;
            if (inner_doc["m"].get_bool().get(is_buyer_maker)) {}
            if (parse_fixed(p_sv, tr.price) && parse_fixed(q_sv, tr.qty)) {
                tr.side = is_buyer_maker ? Side::Bid : Side::Ask;
                RingMsg rmsg{.tag = MsgTag::Trade, .trade = tr, .t_recv = t_recv};
                if (!ring.push(rmsg)) ++drop_count;
            }
        }
    }

    stop.store(true, std::memory_order_release);
    book_thr.join();

    const LatencyPercentiles p = lat.snapshot_and_reset();
    const double elapsed_s = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - wall_start).count();

    std::printf("\n=== Replay complete: %s ===\n", fixture.c_str());
    std::printf("Updates:    %lu\n", (unsigned long)total_updates);
    std::printf("Throughput: %.1f updates/sec\n", total_updates / elapsed_s);
    std::printf("Ring drops: %lu\n", drop_count);
    std::printf("Resyncs:    %lu\n", (unsigned long)seq.resync_count());
    std::printf("\nLatency (ns):\n");
    std::printf("              p50       p99      p99.9        max\n");
    std::printf("  parse    %7ld   %7ld    %7ld    %7ld\n",
        (long)p.parse_p50_ns, (long)p.parse_p99_ns, (long)p.parse_p999_ns, (long)p.parse_max_ns);
    std::printf("  queue    %7ld   %7ld    %7ld    %7ld\n",
        (long)p.queue_p50_ns, (long)p.queue_p99_ns, (long)p.queue_p999_ns, (long)p.queue_max_ns);
    std::printf("  total    %7ld   %7ld    %7ld    %7ld\n",
        (long)p.total_p50_ns, (long)p.total_p99_ns, (long)p.total_p999_ns, (long)p.total_max_ns);

    return (seq.resync_count() == 0) ? 0 : 1;
}

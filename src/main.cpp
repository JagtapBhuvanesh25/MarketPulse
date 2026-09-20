// SPDX-License-Identifier: MIT
// main.cpp — MarketPulse entry point.
//
// CLI flags:
//   --symbol <sym>            Live mode (default: btcusdt)
//   --bench <fixture.jsonl>   Replay mode: fast as possible, print histogram, exit
//   --book=map|ladder         Choose book implementation (default: ladder)
//   --pin                     Pin ingest/book threads to CPU 0/1 (disabled by default)
//
// THREADING MODEL:
//   Ingest thread : reads socket / replay file, parses, pushes RingMsg to SPSC ring
//   Book thread   : pops ring, applies diffs, computes signals, records latency
//   Broadcast thd : IXWebSocket server, 10 Hz coalesced snapshot to browsers

#include "marketpulse/types.hpp"
#include "marketpulse/spsc_ring.hpp"
#include "marketpulse/sequencer.hpp"
#include "marketpulse/book_map.hpp"
#include "marketpulse/book_ladder.hpp"
#include "marketpulse/feed_binance.hpp"
#include "marketpulse/signals.hpp"
#include "marketpulse/latency.hpp"
#include "marketpulse/sim.hpp"
#include "marketpulse/broadcast.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#ifdef __linux__
#  include <pthread.h>
#  include <sched.h>
#endif

using namespace marketpulse;

//-----------------------------------------------------------------------------
// Global stop flag (set by SIGINT/SIGTERM)
//-----------------------------------------------------------------------------

static std::atomic<bool> g_stop{false};

static void handle_signal(int) noexcept {
    g_stop.store(true, std::memory_order_release);
}

//-----------------------------------------------------------------------------
// Thread pinning (aarch64 / Linux only)
//-----------------------------------------------------------------------------

static void pin_thread(int cpu) noexcept {
#ifdef __linux__
    cpu_set_t cs;
    CPU_ZERO(&cs);
    CPU_SET(cpu, &cs);
    if (pthread_setaffinity_np(pthread_self(), sizeof(cs), &cs) != 0) {
        std::fprintf(stderr, "[pin] Failed to pin to CPU %d\n", cpu);
    }
#else
    (void)cpu;
#endif
}

//-----------------------------------------------------------------------------
// CLI parsing
//-----------------------------------------------------------------------------

struct Config {
    std::string symbol   {"btcusdt"};
    std::string bench_file{};       // non-empty → bench mode
    bool        use_ladder{true};
    bool        pin_threads{false};
};

static Config parse_args(int argc, char* argv[]) {
    Config cfg;
    for (int i = 1; i < argc; ++i) {
        std::string_view arg(argv[i]);
        if (arg == "--pin") {
            cfg.pin_threads = true;
        } else if (arg.starts_with("--symbol=")) {
            cfg.symbol = std::string(arg.substr(9));
        } else if (arg == "--symbol" && i + 1 < argc) {
            cfg.symbol = argv[++i];
        } else if (arg.starts_with("--book=")) {
            cfg.use_ladder = (arg.substr(7) == "ladder");
        } else if (arg == "--bench" && i + 1 < argc) {
            cfg.bench_file = argv[++i];
        } else if (arg.starts_with("--bench=")) {
            cfg.bench_file = std::string(arg.substr(8));
        } else {
            std::fprintf(stderr, "Unknown flag: %s\n", argv[i]);
        }
    }
    return cfg;
}

//-----------------------------------------------------------------------------
// Book thread: consumes RingMsg from the SPSC ring, applies to book, computes signals.
//-----------------------------------------------------------------------------

struct BookThreadState {
    FeedBinance::Ring&  ring;
    IBook&              book;
    LatencyRecorder&    lat;
    SimEngine&          sim;
    BroadcastServer*    broadcast;   // null in bench mode
    std::atomic<bool>&  stop;
    std::atomic<uint64_t>& resync_count;
    std::atomic<uint64_t>& drop_count;
    std::chrono::steady_clock::time_point start_time;
    bool pin;
    bool bench_mode;
};

static void book_thread_fn(BookThreadState& s) {
    if (s.pin) pin_thread(1);

    Signals          signals{};
    RollingTradeWindow<2048> trade_window;

    // For OFI: keep previous top-5 levels
    std::array<BookLevel, 5> prev_bids{};
    std::array<BookLevel, 5> prev_asks{};

    // Latency report timer
    auto last_report = std::chrono::steady_clock::now();

    // bookTicker cross-check state
    Price ticker_bid = PRICE_NONE, ticker_ask = PRICE_NONE;

    uint64_t msg_count = 0;
    auto bench_start = std::chrono::steady_clock::now();

    RingMsg msg{};
    while (!s.stop.load(std::memory_order_relaxed)) {
        if (!s.ring.pop(msg)) {
            // Ring empty — spin briefly
            continue;
        }

        const NsPoint t_pop = std::chrono::steady_clock::now();
        ++msg_count;

        if (msg.tag == MsgTag::BookDiff) {
            const BookUpdate& upd = msg.diff;

            // Record queue latency (time from parsed to pop)
            const int64_t q_ns = elapsed_ns(upd.t_parsed, t_pop);
            s.lat.record_queue(q_ns);

            // Apply to book
            s.book.apply(upd.side, upd.price, upd.qty);

            // Capture top-5 levels for OFI
            std::array<BookLevel, 5> curr_bids{}, curr_asks{};
            s.book.top_levels(Side::Bid, std::span<BookLevel>(curr_bids));
            s.book.top_levels(Side::Ask, std::span<BookLevel>(curr_asks));

            // Compute signals
            signals.microprice = compute_microprice(curr_bids[0], curr_asks[0]);
            signals.ofi        = compute_ofi(curr_bids, curr_asks, prev_bids, prev_asks);
            signals.trade_imbalance = trade_window.compute(t_pop);

            prev_bids = curr_bids;
            prev_asks = curr_asks;

            const NsPoint t_signal = std::chrono::steady_clock::now();

            // Record parse latency
            const int64_t p_ns = elapsed_ns(upd.t_recv, upd.t_parsed);
            s.lat.record_parse(p_ns);

            // Record total tick-to-signal
            const int64_t total_ns = elapsed_ns(upd.t_recv, t_signal);
            s.lat.record_total(total_ns);

            // Live print top-of-book (non-bench mode)
            if (!s.bench_mode) {
                const BookLevel bid = curr_bids[0];
                const BookLevel ask = curr_asks[0];
                if (bid.valid() && ask.valid()) {
                    std::printf("\r  Bid: %8.2f (%8.6f)  Ask: %8.2f (%8.6f)  Mid: %8.2f  OFI: %+.3f ",
                        static_cast<double>(bid.price) / FIXED_SCALE,
                        static_cast<double>(bid.qty)   / FIXED_SCALE,
                        static_cast<double>(ask.price) / FIXED_SCALE,
                        static_cast<double>(ask.qty)   / FIXED_SCALE,
                        signals.microprice,
                        signals.ofi);
                    std::fflush(stdout);
                }
            }

            // Publish to broadcast server
            if (s.broadcast) {
                SnapshotState snap{};
                snap.bid_count = static_cast<int>(
                    s.book.top_levels(Side::Bid,
                        std::span<BookLevel>(snap.bids)));
                snap.ask_count = static_cast<int>(
                    s.book.top_levels(Side::Ask,
                        std::span<BookLevel>(snap.asks)));
                snap.signals  = signals;
                snap.resyncs  = s.resync_count.load(std::memory_order_relaxed);
                snap.ring_drops = s.drop_count.load(std::memory_order_relaxed);
                snap.uptime_s = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::seconds>(
                        t_signal - s.start_time).count());
                snap.msgs_per_sec = static_cast<double>(msg_count) /
                    std::max(1.0, static_cast<double>(snap.uptime_s));
                snap.positions = std::vector<VirtualOrder>(
                    s.sim.orders().begin(), s.sim.orders().end());
                s.broadcast->update_state(std::move(snap));
            }

        } else if (msg.tag == MsgTag::Trade) {
            const Trade& tr = msg.trade;
            trade_window.push(tr.t_recv, tr.qty, tr.side == Side::Ask);
            s.sim.on_trade(tr.side, tr.price, tr.qty, t_pop);
            s.sim.mark_to_market(signals.microprice);

        } else if (msg.tag == MsgTag::BookTicker) {
            ticker_bid = msg.ticker.bid_px;
            ticker_ask = msg.ticker.ask_px;
            // Cross-check: compare our book's best bid/ask against bookTicker
            if (!s.bench_mode) {
                const BookLevel book_bid = s.book.best_bid();
                const BookLevel book_ask = s.book.best_ask();
                if (book_bid.valid() && book_ask.valid() &&
                    ticker_bid != PRICE_NONE && ticker_ask != PRICE_NONE) {
                    if (book_bid.price != ticker_bid || book_ask.price != ticker_ask) {
                        std::fprintf(stderr,
                            "\n[MISMATCH] book bid=%.2f ask=%.2f  ticker bid=%.2f ask=%.2f\n",
                            static_cast<double>(book_bid.price) / FIXED_SCALE,
                            static_cast<double>(book_ask.price) / FIXED_SCALE,
                            static_cast<double>(ticker_bid) / FIXED_SCALE,
                            static_cast<double>(ticker_ask) / FIXED_SCALE);
                    }
                }
            }
        }

        // Periodic latency report (every 10 seconds)
        const auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - last_report).count() >= 10) {
            last_report = now;
            const LatencyPercentiles p = s.lat.snapshot_and_reset();
            std::printf("\n");
            // Simple inline print to avoid ostream dependency here
            std::printf("[latency] parse p50=%ldns p99=%ldns  total p50=%ldns p99=%ldns  updates=%lu\n",
                (long)p.parse_p50_ns, (long)p.parse_p99_ns,
                (long)p.total_p50_ns, (long)p.total_p99_ns,
                (unsigned long)p.update_count);
        }
    }

    // Bench mode: print final histogram
    if (s.bench_mode) {
        const auto bench_end = std::chrono::steady_clock::now();
        const double elapsed_s = std::chrono::duration<double>(bench_end - bench_start).count();
        const LatencyPercentiles p = s.lat.snapshot_and_reset();

        std::printf("\n=== Benchmark Results ===\n");
        std::printf("Updates:     %lu\n", (unsigned long)p.update_count);
        std::printf("Throughput:  %.1f updates/sec\n",
                    static_cast<double>(p.update_count) / elapsed_s);
        std::printf("\nLatency (nanoseconds):\n");
        std::printf("              p50       p99      p99.9        max\n");
        std::printf("  parse    %7ld   %7ld    %7ld    %7ld\n",
            (long)p.parse_p50_ns, (long)p.parse_p99_ns, (long)p.parse_p999_ns, (long)p.parse_max_ns);
        std::printf("  queue    %7ld   %7ld    %7ld    %7ld\n",
            (long)p.queue_p50_ns, (long)p.queue_p99_ns, (long)p.queue_p999_ns, (long)p.queue_max_ns);
        std::printf("  total    %7ld   %7ld    %7ld    %7ld\n",
            (long)p.total_p50_ns, (long)p.total_p99_ns, (long)p.total_p999_ns, (long)p.total_max_ns);
    }
}

//-----------------------------------------------------------------------------
// Replay mode: read JSONL fixture, push messages through the pipeline
//-----------------------------------------------------------------------------

static void run_bench(const Config& cfg,
                      FeedBinance::Ring& ring,
                      Sequencer& seq,
                      IBook& book,
                      LatencyRecorder& lat,
                      SimEngine& sim)
{
    std::ifstream fin(cfg.bench_file);
    if (!fin.is_open()) {
        std::fprintf(stderr, "Cannot open fixture: %s\n", cfg.bench_file.c_str());
        return;
    }

    // Synthetic snapshot to get sequencer into Synced state
    // (fixtures include a SYNTHETIC_HEADER comment with lastUpdateId)
    uint64_t last_update_id = 0;

    // Parse the fixture header if present (# SYNTHETIC lastUpdateId=NNN)
    // Otherwise default to reading from the first snapshot-type line.
    {
        std::string first_line;
        if (std::getline(fin, first_line) && first_line.starts_with("# SYNTHETIC")) {
            const auto pos = first_line.find("lastUpdateId=");
            if (pos != std::string::npos) {
                last_update_id = std::stoull(first_line.substr(pos + 13));
            }
        } else {
            // Not a header line — seek back
            fin.seekg(0);
        }
    }

    // Apply a synthetic snapshot to the sequencer (book is empty at start)
    seq.on_connected();
    std::vector<BookUpdate> drained;
    seq.on_snapshot(last_update_id, drained);

    // Book thread
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> resync_count{0};
    std::atomic<uint64_t> drop_count{0};
    BookThreadState bts{ring, book, lat, sim, nullptr, stop,
                        resync_count, drop_count,
                        std::chrono::steady_clock::now(),
                        cfg.pin_threads, /*bench_mode=*/true};
    std::thread book_thr([&](){ book_thread_fn(bts); });

    // Read fixture lines and push them into the ring
    simdjson::ondemand::parser parser;
    std::string line;
    while (std::getline(fin, line)) {
        if (line.empty() || line[0] == '#') continue;

        simdjson::padded_string ps(line.data(), line.size());
        auto doc = parser.iterate(ps);
        if (doc.error()) continue;

        std::string_view stream_sv;
        if (doc["stream"].get_string().get(stream_sv)) continue;

        // Get the raw msg sub-object as a string for re-parsing
        std::string_view msg_sv;
        // Use raw_json to get the message field
        simdjson::ondemand::value msg_val;
        if (doc["msg"].get(msg_val)) continue;

        const NsPoint t_recv = std::chrono::steady_clock::now();

        if (stream_sv.find("depth") != std::string_view::npos) {
            // Re-serialize and parse as depth event
            // (In bench mode we parse inline from the fixture)
            const auto raw_msg = simdjson::to_json_string(msg_val);
            std::vector<BookUpdate> updates;
            updates.reserve(32);
            // Parse the embedded msg JSON
            simdjson::padded_string inner(raw_msg.value().data(), raw_msg.value().size());
            auto inner_doc = parser.iterate(inner);
            // Push updates directly into ring
            uint64_t U = 0, u = 0, pu = 0;
            inner_doc["U"].get_uint64().get(U);
            inner_doc["u"].get_uint64().get(u);
            inner_doc["pu"].get_uint64().get(pu);

            BookUpdate proto{};
            proto.first_id = U; proto.last_id = u; proto.prev_id = pu;
            proto.t_recv = t_recv;
            proto.t_parsed = std::chrono::steady_clock::now();

            auto push_levels = [&](auto arr_result, Side side) {
                auto arr = arr_result;
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
                        if (!ring.push(rmsg)) {
                            drop_count.fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                }
            };
            push_levels(inner_doc["b"].get_array(), Side::Bid);
            push_levels(inner_doc["a"].get_array(), Side::Ask);

        } else if (stream_sv.find("trade") != std::string_view::npos) {
            // Push trade
            const auto raw_msg = simdjson::to_json_string(msg_val);
            simdjson::padded_string inner(raw_msg.value().data(), raw_msg.value().size());
            auto inner_doc = parser.iterate(inner);
            Trade tr{};
            tr.t_recv = t_recv;
            std::string_view p_sv, q_sv;
            bool is_buyer_maker = false;
            inner_doc["p"].get_string().get(p_sv);
            inner_doc["q"].get_string().get(q_sv);
            inner_doc["m"].get_bool().get(is_buyer_maker);
            inner_doc["t"].get_uint64().get(tr.trade_id);
            if (parse_fixed(p_sv, tr.price) && parse_fixed(q_sv, tr.qty)) {
                tr.side = is_buyer_maker ? Side::Bid : Side::Ask;
                RingMsg rmsg{.tag = MsgTag::Trade, .trade = tr, .t_recv = t_recv};
                ring.push(rmsg);
            }
        }
    }

    // Signal book thread to stop
    stop.store(true, std::memory_order_release);
    book_thr.join();
}

//-----------------------------------------------------------------------------
// Live mode
//-----------------------------------------------------------------------------

static void run_live(const Config& cfg,
                     FeedBinance::Ring& ring,
                     Sequencer& seq,
                     IBook& book,
                     LatencyRecorder& lat,
                     SimEngine& sim,
                     BroadcastServer& broadcast)
{
    std::atomic<uint64_t> resync_count{0};
    std::atomic<uint64_t> drop_count_atom{0};

    // Snapshot callback: apply REST snapshot to book, notify sequencer
    auto snapshot_cb = [&](uint64_t last_id,
                           std::vector<SnapLevel> bids,
                           std::vector<SnapLevel> asks) {
        book.clear();
        for (const auto& bl : bids) book.apply(Side::Bid, bl.price, bl.qty);
        for (const auto& al : asks) book.apply(Side::Ask, al.price, al.qty);

        std::vector<BookUpdate> drained;
        seq.on_snapshot(last_id, drained);

        // Apply any buffered events that were held pending the snapshot
        for (const auto& upd : drained) {
            RingMsg msg{.tag = MsgTag::BookDiff, .diff = upd, .t_recv = upd.t_recv};
            if (!ring.push(msg)) {
                drop_count_atom.fetch_add(1, std::memory_order_relaxed);
            }
        }
        std::printf("\n[seq] Snapshot applied: lastUpdateId=%lu, bids=%zu, asks=%zu\n",
                    (unsigned long)last_id, bids.size(), asks.size());
    };

    // Resync callback
    auto resync_cb = [&]() {
        resync_count.fetch_add(1, std::memory_order_relaxed);
        std::printf("\n[seq] RESYNC triggered (total: %lu)\n",
                    (unsigned long)resync_count.load());
    };

    FeedBinance feed(ring, seq, std::move(snapshot_cb), std::move(resync_cb));

    BookThreadState bts{ring, book, lat, sim, &broadcast, g_stop,
                        resync_count, drop_count_atom,
                        std::chrono::steady_clock::now(),
                        cfg.pin_threads, /*bench_mode=*/false};

    // Start book thread
    std::thread book_thr([&]() {
        if (cfg.pin_threads) pin_thread(1);
        book_thread_fn(bts);
    });

    // Start broadcast server
    broadcast.start();

    // Start feed (ingest thread is inside IXWebSocket)
    if (cfg.pin_threads) pin_thread(0);
    feed.start(cfg.symbol);

    std::printf("MarketPulse — symbol=%s  book=%s  port=9001\n",
                cfg.symbol.c_str(), cfg.use_ladder ? "ladder" : "map");
    std::printf("Press Ctrl-C to stop.\n");

    // Wait for stop signal
    while (!g_stop.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    feed.stop();
    book_thr.join();
    broadcast.stop();
}

//-----------------------------------------------------------------------------
// main
//-----------------------------------------------------------------------------

int main(int argc, char* argv[]) {
    // Register signal handlers
    std::signal(SIGINT,  handle_signal);
    std::signal(SIGTERM, handle_signal);

    const Config cfg = parse_args(argc, argv);

    // Print clock overhead at startup
    const int64_t clock_ns = LatencyRecorder::measure_clock_overhead_ns();
    std::printf("[clock] steady_clock::now() overhead: %ld ns\n", (long)clock_ns);

    // Instantiate components
    FeedBinance::Ring ring;
    Sequencer         seq([](SeqState from, SeqState to, std::string_view reason) {
        std::printf("[seq] %s → %s: %.*s\n",
            seq_state_name(from).data(), seq_state_name(to).data(),
            (int)reason.size(), reason.data());
    });

    std::unique_ptr<IBook> book;
    if (cfg.use_ladder) {
        book = std::make_unique<BookLadder>();
    } else {
        book = std::make_unique<BookMap>();
    }
    std::printf("[book] Using %s implementation\n", cfg.use_ladder ? "ladder" : "map");

    LatencyRecorder lat;
    SimEngine       sim;
    BroadcastServer broadcast(9001);

    if (!cfg.bench_file.empty()) {
        run_bench(cfg, ring, seq, *book, lat, sim);
    } else {
        run_live(cfg, ring, seq, *book, lat, sim, broadcast);
    }

    return 0;
}

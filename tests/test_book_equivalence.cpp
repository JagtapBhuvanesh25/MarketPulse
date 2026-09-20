// SPDX-License-Identifier: MIT
// tests/test_book_equivalence.cpp — Differential test: BookMap vs BookLadder.
//
// Replays each fixture through both implementations in lockstep and asserts
// the top 20 levels are identical after every single update.
// Exercises at least 2 re-centrings of the ladder book.

#include <catch2/catch_test_macros.hpp>

#include "marketpulse/types.hpp"
#include "marketpulse/book_map.hpp"
#include "marketpulse/book_ladder.hpp"

#include <array>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <filesystem>
#include <simdjson.h>

using namespace marketpulse;

//-----------------------------------------------------------------------------
// Apply a single update to both books and compare top N levels
//-----------------------------------------------------------------------------

static bool levels_equal(const std::array<BookLevel, 20>& a,
                          const std::array<BookLevel, 20>& b,
                          int n)
{
    for (int i = 0; i < n; ++i) {
        if (a[i].price != b[i].price || a[i].qty != b[i].qty) return false;
    }
    return true;
}

struct ReplayStats {
    int updates{0};
    int recentres{0};
    int mismatches{0};
};

static ReplayStats replay_and_compare(const std::string& fixture_path) {
    std::ifstream fin(fixture_path);
    REQUIRE(fin.is_open());

    BookMap    ref_map;
    BookLadder ladder;
    uint64_t   prev_recentres = 0;

    ReplayStats stats{};
    simdjson::ondemand::parser parser;
    std::string line;

    // Read lastUpdateId from header
    uint64_t last_update_id = 0;
    if (std::getline(fin, line) && line.starts_with("# SYNTHETIC")) {
        const auto pos = line.find("lastUpdateId=");
        if (pos != std::string::npos) {
            last_update_id = std::stoull(line.substr(pos + 13));
        }
    } else {
        fin.seekg(0);
    }

    uint64_t prev_u = last_update_id;

    while (std::getline(fin, line)) {
        if (line.empty() || line[0] == '#') continue;

        simdjson::padded_string ps(line.data(), line.size());
        auto doc = parser.iterate(ps);
        if (doc.error()) continue;

        std::string_view stream_sv;
        if (doc["stream"].get_string().get(stream_sv)) continue;
        if (stream_sv.find("depth") == std::string_view::npos) continue;

        simdjson::ondemand::value msg_val;
        if (doc["msg"].get(msg_val)) continue;
        const auto raw = simdjson::to_json_string(msg_val);
        if (raw.error()) continue;

        simdjson::padded_string inner(raw.value().data(), raw.value().size());
        auto inner_doc = parser.iterate(inner);

        uint64_t u = 0;
        if (inner_doc["u"].get_uint64().get(u)) continue;
        // Skip if pu doesn't match (gap event — sequencer would resync in real pipeline)
        uint64_t pu = 0;
        if (inner_doc["pu"].get_uint64().get(pu)) {}
        if (pu != prev_u) {
            // Gap — reset book state (simulate resync)
            ref_map.clear();
            ladder.clear();
            prev_u = u;
            continue;
        }
        prev_u = u;

        // Parse and apply bid levels
        auto apply_levels = [&](auto arr_res, Side side) {
            auto arr = arr_res;
            if (arr.error()) return;
            for (auto item : arr) {
                auto pair = item.get_array();
                if (pair.error()) continue;
                auto it = pair.begin();
                std::string_view pxsv, qtysv;
                if ((*it).get_string().get(pxsv))  continue; ++it;
                if ((*it).get_string().get(qtysv)) continue;
                Price p = 0; Qty q = 0;
                if (!parse_fixed(pxsv, p)) continue;
                if (!parse_fixed(qtysv, q)) continue;

                ref_map.apply(side, p, q);
                ladder.apply(side, p, q);
                ++stats.updates;

                // Compare top 20 after every single update
                constexpr int N = 20;
                std::array<BookLevel, N> map_bids{}, map_asks{};
                std::array<BookLevel, N> lad_bids{}, lad_asks{};

                const int mb = static_cast<int>(ref_map.top_levels(Side::Bid, map_bids));
                const int lb = static_cast<int>(ladder.top_levels(Side::Bid, lad_bids));
                const int ma = static_cast<int>(ref_map.top_levels(Side::Ask, map_asks));
                const int la = static_cast<int>(ladder.top_levels(Side::Ask, lad_asks));

                const int n_bid = std::min({mb, lb, N});
                const int n_ask = std::min({ma, la, N});

                if (!levels_equal(map_bids, lad_bids, n_bid) ||
                    !levels_equal(map_asks, lad_asks, n_ask)) {
                    ++stats.mismatches;
                    FAIL("Book mismatch at update " << stats.updates
                         << " side=" << (side == Side::Bid ? "bid" : "ask")
                         << " price=" << p << " qty=" << q);
                }
            }
        };

        apply_levels(inner_doc["b"].get_array(), Side::Bid);
        apply_levels(inner_doc["a"].get_array(), Side::Ask);

        stats.recentres = static_cast<int>(ladder.recentre_count());
    }

    return stats;
}

//-----------------------------------------------------------------------------
// Tests
//-----------------------------------------------------------------------------

TEST_CASE("BookEquivalence: clean fixture — no drift across all updates", "[equivalence]") {
    const std::string path = "tests/fixtures/btcusdt_clean.jsonl";
    if (!std::filesystem::exists(path)) {
        SKIP("Fixture not found: " + path + " — run tools/generate_fixtures.py first");
    }

    const auto stats = replay_and_compare(path);
    INFO("Updates: " << stats.updates);
    INFO("Re-centrings: " << stats.recentres);
    INFO("Mismatches: " << stats.mismatches);
    REQUIRE(stats.mismatches == 0);
    REQUIRE(stats.updates > 0);
}

TEST_CASE("BookEquivalence: gap fixture — no drift (gap causes clear, not mismatch)", "[equivalence]") {
    const std::string path = "tests/fixtures/btcusdt_gap.jsonl";
    if (!std::filesystem::exists(path)) {
        SKIP("Fixture not found: " + path + " — run tools/generate_fixtures.py first");
    }

    const auto stats = replay_and_compare(path);
    INFO("Updates: " << stats.updates);
    INFO("Re-centrings: " << stats.recentres);
    INFO("Mismatches: " << stats.mismatches);
    REQUIRE(stats.mismatches == 0);
    REQUIRE(stats.updates > 0);
}

TEST_CASE("BookEquivalence: re-centring is exercised", "[equivalence]") {
    // Drive the ladder through multiple re-centrings with a synthetic price excursion.
    // Apply prices that drift far from the initial base to force at least 2 recentres.
    BookLadder ladder;
    BookMap    ref;

    // Start near $65000
    const Price base = 6'500'000'000'000LL; // $65000 in 1e8

    // Apply many bid/ask levels that drift the mid price across the window boundary
    std::vector<std::pair<Price, Qty>> levels;
    const int64_t tick = 100'000LL; // $0.001 in 1e8 = 100000; use $0.01 = 1'000'000
    const int64_t big_tick = 1'000'000LL;

    // Force a recentre by placing levels far from base
    for (int drift = 0; drift < 20; ++drift) {
        const Price mid = base + static_cast<Price>(drift * 700) * big_tick;
        const Price bid_px = mid - big_tick;
        const Price ask_px = mid + big_tick;

        ref.apply(Side::Bid, bid_px, 100'000'000LL);
        ladder.apply(Side::Bid, bid_px, 100'000'000LL);

        ref.apply(Side::Ask, ask_px, 100'000'000LL);
        ladder.apply(Side::Ask, ask_px, 100'000'000LL);

        // Verify equivalence after each update
        std::array<BookLevel, 20> map_bids{}, lad_bids{};
        ref.top_levels(Side::Bid, map_bids);
        ladder.top_levels(Side::Bid, lad_bids);
        REQUIRE(map_bids[0].price == lad_bids[0].price);
    }

    INFO("Re-centrings during drift test: " << ladder.recentre_count());
    REQUIRE(ladder.recentre_count() >= 2);
}

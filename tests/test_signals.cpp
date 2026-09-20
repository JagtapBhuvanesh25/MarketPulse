// SPDX-License-Identifier: MIT
// tests/test_signals.cpp — Unit tests for signals against hand-computed values.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "marketpulse/signals.hpp"
#include "marketpulse/types.hpp"

#include <array>
#include <chrono>

using namespace marketpulse;
using Catch::Approx;

// Helpers to build BookLevels from human-readable prices
static constexpr Price px(double d) {
    return static_cast<Price>(d * FIXED_SCALE);
}
static constexpr Qty qt(double d) {
    return static_cast<Qty>(d * FIXED_SCALE);
}

//-----------------------------------------------------------------------------
// 1. Microprice
//    = (bid_qty * ask_px + ask_qty * bid_px) / (bid_qty + ask_qty)
//-----------------------------------------------------------------------------

TEST_CASE("Microprice: balanced book returns mid", "[signals]") {
    // bid = 100 @ 1.0, ask = 100 @ 2.0
    // microprice = (1 * 2 + 1 * 1) / (1 + 1) = 1.5
    BookLevel bid{px(1.0), qt(1.0)};
    BookLevel ask{px(2.0), qt(1.0)};
    REQUIRE(compute_microprice(bid, ask) == Approx(1.5).epsilon(1e-6));
}

TEST_CASE("Microprice: large bid tilts toward ask price", "[signals]") {
    // bid = 10 @ 65000.00, ask = 1 @ 65001.00
    // microprice = (10 * 65001 + 1 * 65000) / 11 = 715011/11 = 65000.9090...
    const double expected = (10.0 * 65001.0 + 1.0 * 65000.0) / 11.0;
    BookLevel bid{px(65000.00), qt(10.0)};
    BookLevel ask{px(65001.00), qt(1.0)};
    REQUIRE(compute_microprice(bid, ask) == Approx(expected).epsilon(1e-4));
}

TEST_CASE("Microprice: symmetric book at $65000/$65001 returns mid", "[signals]") {
    BookLevel bid{px(65000.00), qt(1.0)};
    BookLevel ask{px(65001.00), qt(1.0)};
    REQUIRE(compute_microprice(bid, ask) == Approx(65000.5).epsilon(1e-4));
}

TEST_CASE("Microprice: empty book returns 0", "[signals]") {
    BookLevel empty{PRICE_NONE, QTY_ZERO};
    BookLevel ask{px(100.0), qt(1.0)};
    REQUIRE(compute_microprice(empty, ask) == 0.0);
    REQUIRE(compute_microprice(ask, empty) == 0.0);
    REQUIRE(compute_microprice(empty, empty) == 0.0);
}

//-----------------------------------------------------------------------------
// 2. OFI (Order Flow Imbalance)
//    Test with a simple book change where we can hand-compute the result.
//-----------------------------------------------------------------------------

// Helper: fill 5-level span with identical level
static std::array<BookLevel, 5> fill5(BookLevel l) {
    std::array<BookLevel, 5> a{};
    a.fill(l);
    return a;
}

TEST_CASE("OFI: identical book → zero imbalance", "[signals]") {
    auto bids = fill5({px(100.0), qt(1.0)});
    auto asks = fill5({px(101.0), qt(1.0)});
    const double ofi = compute_ofi(bids, asks, bids, asks);
    REQUIRE(ofi == Approx(0.0).margin(1e-9));
}

TEST_CASE("OFI: bid quantity increases → positive OFI", "[signals]") {
    std::array<BookLevel, 5> prev_bids = fill5({px(100.0), qt(1.0)});
    std::array<BookLevel, 5> curr_bids = fill5({px(100.0), qt(2.0)}); // doubled
    std::array<BookLevel, 5> asks = fill5({px(101.0), qt(1.0)});

    // ΔBid = 2-1 = +1 per level → 5 levels → OFI = +5 (normalised by FIXED_SCALE)
    const double ofi = compute_ofi(curr_bids, asks, prev_bids, asks);
    REQUIRE(ofi > 0.0);
}

TEST_CASE("OFI: ask quantity increases → negative OFI", "[signals]") {
    std::array<BookLevel, 5> bids = fill5({px(100.0), qt(1.0)});
    std::array<BookLevel, 5> prev_asks = fill5({px(101.0), qt(1.0)});
    std::array<BookLevel, 5> curr_asks = fill5({px(101.0), qt(2.0)});

    // ΔAsk = 2-1 = +1 per level → OFI = -5
    const double ofi = compute_ofi(bids, curr_asks, bids, prev_asks);
    REQUIRE(ofi < 0.0);
}

TEST_CASE("OFI: new bid level appearing at higher price → positive OFI", "[signals]") {
    std::array<BookLevel, 5> prev_bids = fill5({px(100.0), qt(1.0)});
    // Bid moved up (more aggressive)
    std::array<BookLevel, 5> curr_bids = fill5({px(100.1), qt(1.0)});
    std::array<BookLevel, 5> asks = fill5({px(101.0), qt(1.0)});

    const double ofi = compute_ofi(curr_bids, asks, prev_bids, asks);
    REQUIRE(ofi > 0.0); // bid moved up → positive
}

TEST_CASE("OFI: hand-computed single level", "[signals]") {
    // prev: bid1 = 10@100, ask1 = 5@101; curr: bid1 = 10@100, ask1 = 3@101
    // ΔBid = 10-10 = 0; ΔAsk = 3-5 = -2; OFI = 0 - (-2) = +2 (for 1 level)
    // Other 4 levels identical → total OFI = +2 (in qty units)
    std::array<BookLevel, 5> prev_bids{};
    std::array<BookLevel, 5> curr_bids{};
    std::array<BookLevel, 5> prev_asks{};
    std::array<BookLevel, 5> curr_asks{};
    prev_bids.fill({px(100.0), qt(10.0)});
    curr_bids.fill({px(100.0), qt(10.0)});
    prev_asks.fill({px(101.0), qt(5.0)});
    curr_asks.fill({px(101.0), qt(3.0)});

    const double ofi = compute_ofi(curr_bids, curr_asks, prev_bids, prev_asks);
    // ΔBid = 0 per level, ΔAsk = 3-5 = -2 per level
    // OFI = sum (0 - (-2)) * 5 = 10
    REQUIRE(ofi == Approx(10.0).epsilon(1e-4));
}

//-----------------------------------------------------------------------------
// 3. Rolling Trade Imbalance
//-----------------------------------------------------------------------------

TEST_CASE("TradeImbalance: all buys → +1", "[signals]") {
    RollingTradeWindow<128> win(10'000'000'000ULL); // 10s window
    const auto now = std::chrono::steady_clock::now();

    for (int i = 0; i < 10; ++i) {
        win.push(now, qt(1.0), true); // all buys
    }

    REQUIRE(win.compute(now) == Approx(1.0).epsilon(1e-6));
}

TEST_CASE("TradeImbalance: all sells → -1", "[signals]") {
    RollingTradeWindow<128> win(10'000'000'000ULL);
    const auto now = std::chrono::steady_clock::now();

    for (int i = 0; i < 10; ++i) {
        win.push(now, qt(1.0), false); // all sells
    }

    REQUIRE(win.compute(now) == Approx(-1.0).epsilon(1e-6));
}

TEST_CASE("TradeImbalance: equal buy and sell → 0", "[signals]") {
    RollingTradeWindow<128> win(10'000'000'000ULL);
    const auto now = std::chrono::steady_clock::now();

    for (int i = 0; i < 5; ++i) {
        win.push(now, qt(1.0), true);
        win.push(now, qt(1.0), false);
    }

    REQUIRE(win.compute(now) == Approx(0.0).margin(1e-9));
}

TEST_CASE("TradeImbalance: old trades outside window are excluded", "[signals]") {
    RollingTradeWindow<128> win(1'000'000'000ULL); // 1s window
    const auto past = std::chrono::steady_clock::now()
                    - std::chrono::seconds(5);
    const auto now  = std::chrono::steady_clock::now();

    // Add old sells (outside window)
    for (int i = 0; i < 10; ++i) win.push(past, qt(1.0), false);
    // Add recent buys (inside window)
    for (int i = 0; i < 5;  ++i) win.push(now,  qt(1.0), true);

    // Only the 5 recent buys should count → imbalance = 1.0
    REQUIRE(win.compute(now) == Approx(1.0).epsilon(1e-6));
}

TEST_CASE("TradeImbalance: empty window → 0", "[signals]") {
    RollingTradeWindow<128> win;
    REQUIRE(win.compute(std::chrono::steady_clock::now()) == 0.0);
}

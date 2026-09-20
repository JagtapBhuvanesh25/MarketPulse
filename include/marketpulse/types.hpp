#pragma once
// SPDX-License-Identifier: MIT
// types.hpp — All fundamental types for MarketPulse.
//
// CONSTRAINT: Prices and quantities are int64_t scaled by 1e8 (SCALE = 100'000'000).
// Never use float/double for price, quantity, or derived values compared for equality.
// Signals may use double only at the final presentation step.

#include <chrono>
#include <cstdint>
#include <cinttypes>
#include <string_view>
#include <array>

namespace marketpulse {

//-----------------------------------------------------------------------------
// Scalar types
//-----------------------------------------------------------------------------

/// Fixed-point price: actual_price * PRICE_SCALE. e.g. $65432.10 = 6'543'210'000'000
using Price = int64_t;
/// Fixed-point quantity: actual_qty * QTY_SCALE. e.g. 0.001 BTC = 100'000
using Qty   = int64_t;

/// Scale factor applied to both Price and Qty.
inline constexpr int64_t FIXED_SCALE = 100'000'000LL; // 1e8

/// Sentinel for an empty/invalid price level.
inline constexpr Price PRICE_NONE = INT64_MIN;
inline constexpr Qty   QTY_ZERO   = 0;

//-----------------------------------------------------------------------------
// Side
//-----------------------------------------------------------------------------

enum class Side : uint8_t { Bid = 0, Ask = 1 };

inline constexpr Side flip(Side s) noexcept {
    return s == Side::Bid ? Side::Ask : Side::Bid;
}

//-----------------------------------------------------------------------------
// Fixed-point parsing
// Convert a decimal string (e.g. "65432.10000000") directly to int64_t × 1e8.
// Never passes through double. Returns false on overflow or malformed input.
//-----------------------------------------------------------------------------

[[nodiscard]] inline bool parse_fixed(std::string_view sv, int64_t& out) noexcept {
    out = 0;
    if (sv.empty()) return false;

    bool negative = false;
    std::size_t i = 0;
    if (sv[0] == '-') { negative = true; ++i; }

    int64_t integer_part = 0;
    while (i < sv.size() && sv[i] != '.') {
        if (sv[i] < '0' || sv[i] > '9') return false;
        integer_part = integer_part * 10 + (sv[i] - '0');
        ++i;
    }

    int64_t frac_part = 0;
    int64_t frac_scale = 1;
    if (i < sv.size() && sv[i] == '.') {
        ++i;
        int digits = 0;
        while (i < sv.size() && digits < 8) {
            if (sv[i] < '0' || sv[i] > '9') return false;
            frac_part = frac_part * 10 + (sv[i] - '0');
            frac_scale *= 10;
            ++digits;
            ++i;
        }
        // Skip any trailing digits beyond 8 decimal places
        while (i < sv.size() && sv[i] >= '0' && sv[i] <= '9') ++i;
    }
    if (i != sv.size()) return false;

    // Normalise frac to 8 decimal places
    while (frac_scale < FIXED_SCALE) { frac_part *= 10; frac_scale *= 10; }

    out = integer_part * FIXED_SCALE + frac_part;
    if (negative) out = -out;
    return true;
}

//-----------------------------------------------------------------------------
// Book level (one price rung)
//-----------------------------------------------------------------------------

struct BookLevel {
    Price price{PRICE_NONE};
    Qty   qty{QTY_ZERO};

    [[nodiscard]] bool valid() const noexcept { return price != PRICE_NONE; }
};

//-----------------------------------------------------------------------------
// Timestamps captured per update (nanoseconds, steady_clock)
//-----------------------------------------------------------------------------

using NsPoint = std::chrono::time_point<std::chrono::steady_clock>;

struct Timestamps {
    NsPoint t_recv{};    // immediately after socket read returns
    NsPoint t_parsed{};  // after parsing into BookUpdate
    NsPoint t_signal{};  // after signals recomputed
};

//-----------------------------------------------------------------------------
// BookUpdate: a single diff entry from the depth stream
//
// Field mapping from Binance @depth@100ms event:
//   U  = first_id
//   u  = last_id
//   pu = prev_id  (spot stream only; verify pu == previous u for continuity)
//   b  = bids array: [["price","qty"], ...]
//   a  = asks array
//-----------------------------------------------------------------------------

struct BookUpdate {
    Side     side{Side::Bid};
    Price    price{PRICE_NONE};
    Qty      qty{QTY_ZERO};

    uint64_t first_id{0};  // U
    uint64_t last_id{0};   // u
    uint64_t prev_id{0};   // pu

    NsPoint  t_recv{};
    NsPoint  t_parsed{};
};

//-----------------------------------------------------------------------------
// Trade: a single trade from the @trade stream
//-----------------------------------------------------------------------------

struct Trade {
    Side     side{Side::Bid};    // maker side (bid = buyer is maker)
    Price    price{PRICE_NONE};
    Qty      qty{QTY_ZERO};
    uint64_t trade_id{0};
    uint64_t event_time_ms{0};   // 'T' field from wire
    NsPoint  t_recv{};
};

} // namespace marketpulse

// Compatibility aliases
namespace mp = marketpulse;
namespace obl = marketpulse;

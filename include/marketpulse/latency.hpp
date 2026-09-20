#pragma once
// SPDX-License-Identifier: MIT
// latency.hpp — Timestamp capture and HdrHistogram wrapper.
//
// Captures three steady_clock timestamps per update:
//   t_recv   — immediately after socket read returns (in ingest thread)
//   t_parsed — after parsing into BookUpdate (in ingest thread)
//   t_signal — after signals recomputed (in book thread)
//
// Three HdrHistograms are maintained:
//   parse_hist_   : t_parsed - t_recv      (parse latency)
//   queue_hist_   : pop time - t_parsed    (ring queue latency)
//   total_hist_   : t_signal - t_recv      (tick-to-signal)
//
// Configured for 1 ns resolution up to 10 seconds max.
//
// THREAD SAFETY: LatencyRecorder is written from the book thread only.
// A snapshot is taken by the broadcast thread under a brief atomic swap.

#include "types.hpp"
#include <cstdint>
#include <iosfwd>
#include <memory>
#include <chrono>

// Forward-declare hdr_histogram to avoid pulling the C header into every TU.
struct hdr_histogram;

namespace marketpulse {

/// LatencyPercentiles: a snapshot of latency stats suitable for JSON serialisation.
struct LatencyPercentiles {
    int64_t parse_p50_ns{0};
    int64_t parse_p99_ns{0};
    int64_t parse_p999_ns{0};
    int64_t parse_max_ns{0};

    int64_t queue_p50_ns{0};
    int64_t queue_p99_ns{0};
    int64_t queue_p999_ns{0};
    int64_t queue_max_ns{0};

    int64_t total_p50_ns{0};
    int64_t total_p99_ns{0};
    int64_t total_p999_ns{0};
    int64_t total_max_ns{0};

    uint64_t update_count{0};
};

/// LatencyRecorder: wraps HdrHistogram for the three latency measurements.
///
/// Usage (hot path):
///   record_parse(ns)    — from ingest thread (actually from book thread after recv)
///   record_queue(ns)    — from book thread
///   record_total(ns)    — from book thread
///
/// Periodic (every 10s, broadcast thread):
///   snapshot()  — returns LatencyPercentiles and resets counters
///   print_report(ostream&)  — prints human-readable summary
class LatencyRecorder {
public:
    LatencyRecorder();
    ~LatencyRecorder();

    // Non-copyable (hdr_histogram is a C resource)
    LatencyRecorder(const LatencyRecorder&)            = delete;
    LatencyRecorder& operator=(const LatencyRecorder&) = delete;

    //--------------------------------------------------------------------------
    // Hot path — called from book thread, must not allocate
    //--------------------------------------------------------------------------

    void record_parse(int64_t ns) noexcept;
    void record_queue(int64_t ns) noexcept;
    void record_total(int64_t ns) noexcept;

    //--------------------------------------------------------------------------
    // Periodic reporting
    //--------------------------------------------------------------------------

    /// Extract current percentiles and reset all histograms.
    [[nodiscard]] LatencyPercentiles snapshot_and_reset() noexcept;

    /// Print a formatted latency report to the given stream.
    void print_report(std::ostream& os, const LatencyPercentiles& p) const;

    //--------------------------------------------------------------------------
    // Overhead measurement
    //--------------------------------------------------------------------------

    /// Measure the cost of a single steady_clock::now() call in nanoseconds.
    /// Called once at startup; result printed to README / stdout.
    static int64_t measure_clock_overhead_ns() noexcept;

private:
    hdr_histogram* parse_hist_{nullptr};
    hdr_histogram* queue_hist_{nullptr};
    hdr_histogram* total_hist_{nullptr};

    uint64_t update_count_{0};

    static constexpr int64_t MIN_NS   = 1;
    static constexpr int64_t MAX_NS   = 10'000'000'000LL; // 10 seconds
    static constexpr int     SIG_FIGS = 3;
};

//-----------------------------------------------------------------------------
// Inline helpers for timestamp capture (these ARE on the hot path)
//-----------------------------------------------------------------------------

/// Capture a steady_clock timestamp. Compiler can inline this.
[[nodiscard]] inline NsPoint now_ns() noexcept {
    return std::chrono::steady_clock::now();
}

/// Elapsed nanoseconds between two NsPoints.
[[nodiscard]] inline int64_t elapsed_ns(NsPoint from, NsPoint to) noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(to - from).count();
}

} // namespace marketpulse

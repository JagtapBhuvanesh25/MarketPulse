// SPDX-License-Identifier: MIT
// latency.cpp — HdrHistogram wrapper implementation.

#include "marketpulse/latency.hpp"

// HdrHistogram_c C header
#include <hdr/hdr_histogram.h>

#include <ostream>
#include <chrono>
#include <cstring>

namespace marketpulse {

LatencyRecorder::LatencyRecorder() {
    // hdr_init(lowest_trackable_value, highest_trackable_value, significant_figures, &histogram)
    hdr_init(MIN_NS, MAX_NS, SIG_FIGS, &parse_hist_);
    hdr_init(MIN_NS, MAX_NS, SIG_FIGS, &queue_hist_);
    hdr_init(MIN_NS, MAX_NS, SIG_FIGS, &total_hist_);
}

LatencyRecorder::~LatencyRecorder() {
    hdr_close(parse_hist_);
    hdr_close(queue_hist_);
    hdr_close(total_hist_);
}

void LatencyRecorder::record_parse(int64_t ns) noexcept {
    hdr_record_value(parse_hist_, ns > 0 ? ns : 1);
    ++update_count_;
}

void LatencyRecorder::record_queue(int64_t ns) noexcept {
    hdr_record_value(queue_hist_, ns > 0 ? ns : 1);
}

void LatencyRecorder::record_total(int64_t ns) noexcept {
    hdr_record_value(total_hist_, ns > 0 ? ns : 1);
}

LatencyPercentiles LatencyRecorder::snapshot_and_reset() noexcept {
    LatencyPercentiles p{};

    p.parse_p50_ns   = hdr_value_at_percentile(parse_hist_, 50.0);
    p.parse_p99_ns   = hdr_value_at_percentile(parse_hist_, 99.0);
    p.parse_p999_ns  = hdr_value_at_percentile(parse_hist_, 99.9);
    p.parse_max_ns   = hdr_max(parse_hist_);

    p.queue_p50_ns   = hdr_value_at_percentile(queue_hist_, 50.0);
    p.queue_p99_ns   = hdr_value_at_percentile(queue_hist_, 99.0);
    p.queue_p999_ns  = hdr_value_at_percentile(queue_hist_, 99.9);
    p.queue_max_ns   = hdr_max(queue_hist_);

    p.total_p50_ns   = hdr_value_at_percentile(total_hist_, 50.0);
    p.total_p99_ns   = hdr_value_at_percentile(total_hist_, 99.0);
    p.total_p999_ns  = hdr_value_at_percentile(total_hist_, 99.9);
    p.total_max_ns   = hdr_max(total_hist_);

    p.update_count   = update_count_;

    hdr_reset(parse_hist_);
    hdr_reset(queue_hist_);
    hdr_reset(total_hist_);
    update_count_ = 0;

    return p;
}

void LatencyRecorder::print_report(std::ostream& os,
                                   const LatencyPercentiles& p) const {
    os << "Latency (ns) [" << p.update_count << " updates]\n"
       << "              p50      p99     p99.9      max\n"
       << "  parse   "
       << std::setw(8) << p.parse_p50_ns  << ' '
       << std::setw(8) << p.parse_p99_ns  << ' '
       << std::setw(9) << p.parse_p999_ns << ' '
       << std::setw(8) << p.parse_max_ns  << '\n'
       << "  queue   "
       << std::setw(8) << p.queue_p50_ns  << ' '
       << std::setw(8) << p.queue_p99_ns  << ' '
       << std::setw(9) << p.queue_p999_ns << ' '
       << std::setw(8) << p.queue_max_ns  << '\n'
       << "  total   "
       << std::setw(8) << p.total_p50_ns  << ' '
       << std::setw(8) << p.total_p99_ns  << ' '
       << std::setw(9) << p.total_p999_ns << ' '
       << std::setw(8) << p.total_max_ns  << '\n';
}

int64_t LatencyRecorder::measure_clock_overhead_ns() noexcept {
    using clock = std::chrono::steady_clock;

    constexpr int ITERS = 1000;
    const auto start = clock::now();
    for (int i = 0; i < ITERS; ++i) {
        volatile auto t = clock::now();
        (void)t;
    }
    const auto end = clock::now();
    const auto total_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    return total_ns / ITERS;
}

} // namespace marketpulse

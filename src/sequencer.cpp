// SPDX-License-Identifier: MIT
// sequencer.cpp — Snapshot + buffered-diff synchronisation state machine.

#include "marketpulse/sequencer.hpp"

#include <cassert>
#include <cstdio>   // snprintf
#include <algorithm>

namespace marketpulse {

Sequencer::Sequencer(SeqLogFn log_fn) noexcept
    : log_fn_(std::move(log_fn)) {}

//-----------------------------------------------------------------------------
// State transitions
//-----------------------------------------------------------------------------

void Sequencer::transition(SeqState to, std::string_view reason) noexcept {
    if (log_fn_) {
        log_fn_(state_, to, reason);
    }
    state_ = to;
}

void Sequencer::on_connected() noexcept {
    buffer_.clear();
    snapshot_last_id_ = 0;
    last_u_ = 0;
    transition(SeqState::Buffering, "WebSocket connected, buffering diffs");
}

void Sequencer::on_disconnected(std::string_view reason) noexcept {
    buffer_.clear();
    transition(SeqState::Disconnected, reason);
}

void Sequencer::on_snapshot(uint64_t last_update_id,
                             std::vector<BookUpdate>& drained) noexcept {
    // Must be called only in Buffering or AwaitingSnapshot state.
    snapshot_last_id_ = last_update_id;
    drained.clear();

    // Drain the buffer: discard events where u <= lastUpdateId,
    // then hand back the rest in order.
    for (auto& upd : buffer_) {
        if (upd.last_id <= snapshot_last_id_) {
            // Stale — discard
            continue;
        }
        drained.push_back(upd);
    }
    buffer_.clear();

    if (drained.empty()) {
        // No buffered events to apply yet; transition to AwaitingSnapshot
        // (we'll get the first valid event on the next process_diff call).
        transition(SeqState::AwaitingSnapshot,
                   "Snapshot applied, waiting for first live event");
        return;
    }

    // Validate the first drained event per Binance spec:
    // U <= lastUpdateId + 1  AND  u >= lastUpdateId + 1
    const auto& first = drained.front();
    if (first.first_id <= snapshot_last_id_ + 1 &&
        first.last_id  >= snapshot_last_id_ + 1) {
        last_u_ = drained.back().last_id;
        transition(SeqState::Synced, "Snapshot applied, first buffered event validated");
    } else {
        // Buffer has no matching event — need to resync.
        drained.clear();
        ++resync_count_;
        transition(SeqState::Resyncing,
                   "Snapshot applied but no valid continuation event in buffer");
    }
}

//-----------------------------------------------------------------------------
// Hot path
//-----------------------------------------------------------------------------

SeqResult Sequencer::process_diff(const BookUpdate& update) noexcept {
    switch (state_) {
        case SeqState::Disconnected:
        case SeqState::Resyncing:
            // Drop all events until reconnected.
            return SeqResult::Drop;

        case SeqState::Buffering:
            // Store events before snapshot arrives.
            // Allowed to allocate here (not hot path).
            buffer_.push_back(update);
            return SeqResult::Buffer;

        case SeqState::AwaitingSnapshot: {
            // Snapshot has been applied. We're waiting for the first live event
            // that satisfies U <= lastUpdateId+1 AND u >= lastUpdateId+1.
            if (update.last_id <= snapshot_last_id_) {
                return SeqResult::Drop; // stale
            }
            if (update.first_id <= snapshot_last_id_ + 1 &&
                update.last_id  >= snapshot_last_id_ + 1) {
                last_u_ = update.last_id;
                transition(SeqState::Synced, "First valid live event after snapshot");
                return SeqResult::Apply;
            }
            // Gap between snapshot and first live event — resync.
            ++resync_count_;
            transition(SeqState::Resyncing,
                       "Gap between snapshot lastUpdateId and first live event");
            return SeqResult::Resync;
        }

        case SeqState::Synced: {
            // Continuity check: pu must equal last_u_.
            // (Binance spot @depth@100ms includes the 'pu' field.)
            if (update.prev_id != last_u_) {
                char reason[128];
                std::snprintf(reason, sizeof(reason),
                              "Continuity broken: pu=%" PRIu64 " expected %" PRIu64
                              " (last u=%" PRIu64 ")",
                              update.prev_id, last_u_, last_u_);
                ++resync_count_;
                transition(SeqState::Resyncing, reason);
                return SeqResult::Resync;
            }
            last_u_ = update.last_id;
            return SeqResult::Apply;
        }
    }

    // Unreachable
    return SeqResult::Drop;
}

} // namespace marketpulse

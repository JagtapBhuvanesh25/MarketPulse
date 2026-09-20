#pragma once
// SPDX-License-Identifier: MIT
// sequencer.hpp — Snapshot + buffered-diff synchronisation state machine.
//
// Implements the Binance documented procedure for managing a local order book:
// https://binance-docs.github.io/apidocs/spot/en/#diff-depth-stream
//
// State machine:
//
//   Disconnected
//       │ (WS connects, start buffering)
//       ▼
//   Buffering  ──► (REST snapshot fetched) ──► AwaitingSnapshot
//       │                                           │
//       │                              (first valid event found)
//       │                                           │
//       └────────────────────────────────────────── ▼
//                                               Synced
//                                                  │
//                                   (pu mismatch or error)
//                                                  │
//                                                  ▼
//                                              Resyncing ──► (same as Disconnected start)
//
// Every state transition is logged. Resync count is exposed.

#include "types.hpp"
#include <cstdint>
#include <functional>
#include <string_view>
#include <vector>

namespace marketpulse {

/// Result of processing one diff event through the sequencer.
enum class SeqResult : uint8_t {
    Drop,    // Event predates snapshot, discard silently
    Buffer,  // Snapshot not yet received, buffered internally
    Apply,   // Apply this event to the book
    Resync,  // Continuity broken; caller must trigger full resync
};

/// States of the sequencer state machine.
enum class SeqState : uint8_t {
    Disconnected,
    Buffering,
    AwaitingSnapshot,
    Synced,
    Resyncing,
};

[[nodiscard]] inline std::string_view seq_state_name(SeqState s) noexcept {
    switch (s) {
        case SeqState::Disconnected:    return "Disconnected";
        case SeqState::Buffering:       return "Buffering";
        case SeqState::AwaitingSnapshot:return "AwaitingSnapshot";
        case SeqState::Synced:          return "Synced";
        case SeqState::Resyncing:       return "Resyncing";
    }
    return "Unknown";
}

/// Callback type for log messages: (old_state, new_state, reason)
using SeqLogFn = std::function<void(SeqState, SeqState, std::string_view)>;

/// Sequencer: stateful synchronisation logic.
///
/// Usage:
///   1. Call on_connected() when the WebSocket opens. State → Buffering.
///   2. Feed each incoming diff event to process_diff(). The sequencer
///      returns Buffer until the snapshot is fetched.
///   3. Call on_snapshot(lastUpdateId) once the REST snapshot is consumed.
///   4. Continue feeding process_diff(). Returns Apply for each valid event,
///      Resync if the sequence breaks.
///   5. On Resync: restart the whole process (reconnect or re-fetch snapshot).
///   6. Call on_disconnected() when the WS drops.
///
/// The sequencer does NOT hold a copy of the book — it only reasons about IDs.
class Sequencer {
public:
    explicit Sequencer(SeqLogFn log_fn = {}) noexcept;

    //--------------------------------------------------------------------------
    // State transitions (control-plane calls; may allocate for buffered events)
    //--------------------------------------------------------------------------

    /// Call when the WebSocket connection is established.
    void on_connected() noexcept;

    /// Call when the WebSocket connection drops (e.g. network error).
    void on_disconnected(std::string_view reason) noexcept;

    /// Call once after the REST snapshot has been applied to the book.
    /// Drains the internal buffer and returns the events to apply in order.
    /// @param last_update_id  The 'lastUpdateId' field from the REST snapshot.
    /// @param drained         Output: buffered events that should now be applied.
    void on_snapshot(uint64_t last_update_id, std::vector<BookUpdate>& drained) noexcept;

    //--------------------------------------------------------------------------
    // Hot-path call: process one diff event
    //--------------------------------------------------------------------------

    /// Process a diff event from the WebSocket stream.
    /// Returns the action the caller should take.
    ///
    /// CONSTRAINT: On the hot path (Synced state) this must not allocate.
    ///             The Buffer path (before snapshot) pushes into a std::vector —
    ///             that is intentional and acceptable since it's not hot.
    [[nodiscard]] SeqResult process_diff(const BookUpdate& update) noexcept;

    //--------------------------------------------------------------------------
    // Accessors
    //--------------------------------------------------------------------------

    [[nodiscard]] SeqState   state()        const noexcept { return state_; }
    [[nodiscard]] uint64_t   resync_count() const noexcept { return resync_count_; }
    [[nodiscard]] uint64_t   last_applied() const noexcept { return last_u_; }

private:
    void transition(SeqState to, std::string_view reason) noexcept;

    SeqState  state_{SeqState::Disconnected};
    uint64_t  snapshot_last_id_{0};  // lastUpdateId from REST snapshot
    uint64_t  last_u_{0};            // 'u' of the last applied event
    uint64_t  resync_count_{0};

    // Buffer for events arriving before the snapshot is fetched.
    // Allocated lazily; cleared after on_snapshot().
    std::vector<BookUpdate> buffer_;

    SeqLogFn log_fn_;
};

} // namespace marketpulse

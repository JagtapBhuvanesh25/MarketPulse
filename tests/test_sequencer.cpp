// SPDX-License-Identifier: MIT
// tests/test_sequencer.cpp — Unit tests for the Sequencer state machine.

#include <catch2/catch_test_macros.hpp>

#include "marketpulse/sequencer.hpp"

using namespace marketpulse;

// Helper: create a BookUpdate with specific sequence IDs
static BookUpdate make_update(uint64_t U, uint64_t u, uint64_t pu) {
    BookUpdate upd{};
    upd.first_id = U;
    upd.last_id  = u;
    upd.prev_id  = pu;
    upd.side     = Side::Bid;
    upd.price    = 6_500_000_000_000LL; // $65000.00 in 1e8
    upd.qty      = 100_000_000LL;       // 1.0 in 1e8
    return upd;
}

//-----------------------------------------------------------------------------
// 1. State transitions from Disconnected
//-----------------------------------------------------------------------------

TEST_CASE("Sequencer: initial state is Disconnected", "[sequencer]") {
    Sequencer seq;
    REQUIRE(seq.state() == SeqState::Disconnected);
}

TEST_CASE("Sequencer: on_connected() transitions to Buffering", "[sequencer]") {
    Sequencer seq;
    seq.on_connected();
    REQUIRE(seq.state() == SeqState::Buffering);
}

//-----------------------------------------------------------------------------
// 2. Buffering state
//-----------------------------------------------------------------------------

TEST_CASE("Sequencer: events in Buffering state return Buffer", "[sequencer]") {
    Sequencer seq;
    seq.on_connected();

    const auto upd = make_update(1000001, 1000003, 1000000);
    const auto result = seq.process_diff(upd);
    REQUIRE(result == SeqResult::Buffer);
    REQUIRE(seq.state() == SeqState::Buffering);
}

//-----------------------------------------------------------------------------
// 3. Snapshot application and draining the buffer
//-----------------------------------------------------------------------------

TEST_CASE("Sequencer: on_snapshot drains valid buffered events", "[sequencer]") {
    Sequencer seq;
    seq.on_connected();

    // Buffer three events
    // snapshot lastUpdateId = 1000000
    // Valid first event: U <= 1000001 AND u >= 1000001
    const auto upd1 = make_update(999998, 1000002, 999997); // u > snapshot, U <= snapshot+1
    const auto upd2 = make_update(1000003, 1000005, 1000002);
    const auto upd3 = make_update(1000006, 1000008, 1000005);

    REQUIRE(seq.process_diff(upd1) == SeqResult::Buffer);
    REQUIRE(seq.process_diff(upd2) == SeqResult::Buffer);
    REQUIRE(seq.process_diff(upd3) == SeqResult::Buffer);

    std::vector<BookUpdate> drained;
    seq.on_snapshot(1000000, drained);

    // upd1 has u=1000002 > 1000000 and U=999998 <= 1000001, so it's valid first event
    REQUIRE(seq.state() == SeqState::Synced);
    REQUIRE(drained.size() == 3); // all three pass
}

TEST_CASE("Sequencer: stale buffered events (u <= lastUpdateId) are discarded", "[sequencer]") {
    Sequencer seq;
    seq.on_connected();

    // Stale events (u <= snapshot)
    const auto stale1 = make_update(999990, 999995, 999989);
    const auto stale2 = make_update(999996, 999999, 999995);
    // Valid event
    const auto valid  = make_update(999998, 1000002, 999997);

    seq.process_diff(stale1);
    seq.process_diff(stale2);
    seq.process_diff(valid);

    std::vector<BookUpdate> drained;
    seq.on_snapshot(1000000, drained);

    REQUIRE(seq.state() == SeqState::Synced);
    REQUIRE(drained.size() == 1); // only the valid event
}

//-----------------------------------------------------------------------------
// 4. Synced state: normal operation
//-----------------------------------------------------------------------------

TEST_CASE("Sequencer: Synced state applies events with correct pu", "[sequencer]") {
    Sequencer seq;
    seq.on_connected();

    // Snapshot with no buffered events
    std::vector<BookUpdate> drained;
    seq.on_snapshot(1000000, drained);
    REQUIRE(seq.state() == SeqState::AwaitingSnapshot);

    // First live event: U <= 1000001 AND u >= 1000001
    const auto upd1 = make_update(1000000, 1000003, 1000000);
    REQUIRE(seq.process_diff(upd1) == SeqResult::Apply);
    REQUIRE(seq.state() == SeqState::Synced);

    // Next event: pu must equal 1000003 (last u)
    const auto upd2 = make_update(1000004, 1000006, 1000003);
    REQUIRE(seq.process_diff(upd2) == SeqResult::Apply);
    REQUIRE(seq.state() == SeqState::Synced);
    REQUIRE(seq.last_applied() == 1000006);
}

//-----------------------------------------------------------------------------
// 5. Resync detection on pu mismatch
//-----------------------------------------------------------------------------

TEST_CASE("Sequencer: pu mismatch in Synced triggers Resync", "[sequencer]") {
    Sequencer seq;
    seq.on_connected();

    std::vector<BookUpdate> drained;
    seq.on_snapshot(1000000, drained);

    // Get into Synced
    const auto upd1 = make_update(1000000, 1000003, 1000000);
    seq.process_diff(upd1);
    REQUIRE(seq.state() == SeqState::Synced);

    // Now send an event with wrong pu (1000005 instead of 1000003)
    const auto bad = make_update(1000004, 1000006, 1000005); // pu mismatch!
    REQUIRE(seq.process_diff(bad) == SeqResult::Resync);
    REQUIRE(seq.state() == SeqState::Resyncing);
    REQUIRE(seq.resync_count() == 1);
}

//-----------------------------------------------------------------------------
// 6. Gap fixture: replay produces exactly one resync
//    (Tested via the sequencer state machine logic, not the full pipeline)
//-----------------------------------------------------------------------------

TEST_CASE("Sequencer: gap fixture produces exactly one resync", "[sequencer][fixtures]") {
    // Simulate what happens when replaying btcusdt_gap.jsonl:
    // 49 clean events, then one event with a bad pu → exactly 1 resync.

    Sequencer seq;
    seq.on_connected();

    std::vector<BookUpdate> drained;
    seq.on_snapshot(999999, drained);

    // Simulate the first valid event getting us to Synced
    uint64_t last_u = 999999;
    const auto first = make_update(999999, 1000001, 999999);
    REQUIRE(seq.process_diff(first) == SeqResult::Apply);
    last_u = 1000001;

    // 48 clean events
    for (int i = 0; i < 48; ++i) {
        const auto upd = make_update(last_u + 1, last_u + 3, last_u);
        REQUIRE(seq.process_diff(upd) == SeqResult::Apply);
        last_u += 3;
    }
    REQUIRE(seq.resync_count() == 0);

    // Event 50: injected gap (pu doesn't match)
    const auto gap = make_update(last_u + 1, last_u + 3, last_u + 999); // bad pu!
    REQUIRE(seq.process_diff(gap) == SeqResult::Resync);
    REQUIRE(seq.resync_count() == 1);
    REQUIRE(seq.state() == SeqState::Resyncing);

    // No more resyncs should have occurred
    REQUIRE(seq.resync_count() == 1);
}

//-----------------------------------------------------------------------------
// 7. Disconnected state: all events are dropped
//-----------------------------------------------------------------------------

TEST_CASE("Sequencer: events in Disconnected state are dropped", "[sequencer]") {
    Sequencer seq;
    REQUIRE(seq.state() == SeqState::Disconnected);
    const auto upd = make_update(1, 3, 0);
    REQUIRE(seq.process_diff(upd) == SeqResult::Drop);
}

//-----------------------------------------------------------------------------
// 8. Log callback is called on every transition
//-----------------------------------------------------------------------------

TEST_CASE("Sequencer: log callback is invoked on transitions", "[sequencer]") {
    int transition_count = 0;
    Sequencer seq([&](SeqState, SeqState, std::string_view) {
        ++transition_count;
    });

    seq.on_connected();           // Disconnected → Buffering
    REQUIRE(transition_count == 1);

    std::vector<BookUpdate> drained;
    seq.on_snapshot(1000, drained); // → AwaitingSnapshot
    REQUIRE(transition_count == 2);

    const auto upd = make_update(1000, 1002, 1000);
    seq.process_diff(upd);        // → Synced
    REQUIRE(transition_count == 3);

    seq.on_disconnected("test");  // → Disconnected
    REQUIRE(transition_count == 4);
}

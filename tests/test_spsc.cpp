// SPDX-License-Identifier: MIT
// tests/test_spsc.cpp — Unit tests for SpscRing.
//
// Tests:
//   1. Single-threaded FIFO ordering and wraparound
//   2. Two-thread stress test: 1,000,000 items, no loss, no duplication, no reordering

#include <catch2/catch_test_macros.hpp>

#include "marketpulse/spsc_ring.hpp"

#include <atomic>
#include <cstdint>
#include <numeric>
#include <thread>
#include <vector>

using namespace marketpulse;

//-----------------------------------------------------------------------------
// 1. Single-threaded: FIFO ordering and wraparound
//-----------------------------------------------------------------------------

TEST_CASE("SpscRing: FIFO ordering, single thread", "[spsc]") {
    SpscRing<int, 8> ring;

    // Push 8 items (fill the ring)
    for (int i = 0; i < 8; ++i) {
        REQUIRE(ring.push(i));
    }

    // Ring should be full; next push fails
    REQUIRE_FALSE(ring.push(99));

    // Pop all in order
    for (int i = 0; i < 8; ++i) {
        int val = -1;
        REQUIRE(ring.pop(val));
        REQUIRE(val == i);
    }

    // Ring should be empty
    int dummy = -1;
    REQUIRE_FALSE(ring.pop(dummy));
}

TEST_CASE("SpscRing: wraparound — push/pop cycle across boundary", "[spsc]") {
    SpscRing<int, 4> ring;

    // Fill and drain 3 times to exercise wraparound
    for (int round = 0; round < 3; ++round) {
        for (int i = 0; i < 4; ++i) REQUIRE(ring.push(round * 10 + i));
        for (int i = 0; i < 4; ++i) {
            int v = -1;
            REQUIRE(ring.pop(v));
            REQUIRE(v == round * 10 + i);
        }
    }
}

TEST_CASE("SpscRing: capacity is reported correctly", "[spsc]") {
    SpscRing<int, 16> ring;
    REQUIRE(ring.capacity() == 16);
}

//-----------------------------------------------------------------------------
// 2. Two-thread stress test: 1,000,000 items
//    Verifies: no loss, no duplication, correct ordering
//-----------------------------------------------------------------------------

TEST_CASE("SpscRing: two-thread stress test, 1M items", "[spsc][stress]") {
    // Use a larger ring so we rarely hit full
    static SpscRing<uint64_t, 4096> ring;

    constexpr uint64_t N = 1'000'000;

    std::atomic<uint64_t> drop_count{0};

    // Producer thread: push 0..N-1
    std::thread producer([&]() {
        for (uint64_t i = 0; i < N; ++i) {
            while (!ring.push(i)) {
                // Ring full — spin briefly then retry.
                // In production the ingest thread increments a drop counter
                // instead, but for the test we need no drops.
                std::this_thread::yield();
            }
        }
    });

    // Consumer thread: pop and verify ordering
    std::vector<uint64_t> received;
    received.reserve(N);

    std::thread consumer([&]() {
        uint64_t count = 0;
        while (count < N) {
            uint64_t val;
            if (ring.pop(val)) {
                received.push_back(val);
                ++count;
            }
        }
    });

    producer.join();
    consumer.join();

    // Verify: no loss
    REQUIRE(received.size() == N);

    // Verify: no duplication and correct ordering
    for (uint64_t i = 0; i < N; ++i) {
        REQUIRE(received[i] == i);
    }

    // Verify: no drops occurred (producer never gave up)
    REQUIRE(drop_count.load() == 0);
}

//-----------------------------------------------------------------------------
// 3. Drop counter test: ring correctly signals full
//-----------------------------------------------------------------------------

TEST_CASE("SpscRing: push returns false when full", "[spsc]") {
    SpscRing<int, 4> ring;

    uint64_t drops = 0;
    for (int i = 0; i < 8; ++i) {
        if (!ring.push(i)) ++drops;
    }
    // Ring capacity 4, pushed 8, so 4 should drop
    REQUIRE(drops == 4);
}

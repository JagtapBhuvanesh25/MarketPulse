# Build Spec: Live Order Book + Matching Engine

> **How to use this file.** Do not paste the whole thing at once. Give the agent
> the "Project Context" and "Global Constraints" sections first, then one stage
> at a time. Require the acceptance criteria of a stage to pass before moving on.
> Each stage is designed to be independently runnable and demoable.

---

## Project Context

You are building `orderbook-live`, a C++20 service that connects to a public
cryptocurrency exchange WebSocket feed, maintains a correct local limit order
book in memory, computes microstructure signals, simulates limit orders against
the live book, and broadcasts state to a browser dashboard over WebSocket.

It runs 24/7 on a single small ARM64 Linux VM (2 vCPU, 12 GB RAM, Ubuntu 24.04)
behind Caddy for TLS. It must survive network drops, exchange resyncs, and
process restarts without manual intervention.

The point of the project is **measured low-latency engineering**, not features.
Every performance claim in the README must be backed by a number the program
itself produces.

## Global Constraints

These are non-negotiable. Violating any of them is a bug.

1. **C++20.** GCC 13+ / Clang 17+. Compile with
   `-Wall -Wextra -Wpedantic -Werror`. Warnings are errors.
2. **Prices and quantities are `int64_t` fixed-point**, scaled by 1e8. Never
   `float` or `double` for a price, a quantity, or anything derived from them
   that gets compared for equality. Signals may use `double` at the final
   presentation step only.
3. **No mutex, no allocation, no syscall on the book-processing hot path.**
   The path from "bytes received" to "signal computed" must not allocate.
   Verify with a debug allocator counter in tests.
4. **Target is aarch64.** No `rdtsc`, no SSE/AVX intrinsics, no
   `-march=native` assumptions that break cross-compilation. Use
   `std::chrono::steady_clock` for timing.
5. **No exceptions on the hot path.** Parsing and book application return
   error enums. Exceptions are acceptable at startup and in the control plane.
6. **Every stage ships a working binary.** Never leave the repo in a state
   where `cmake --build build && ./build/obl` does not run.
7. **Deterministic tests.** Unit and integration tests must not touch the
   network. They run against recorded JSONL fixtures (see Stage 1b).

## Dependencies

Fetch all of these via CMake `FetchContent` pinned to specific tags. No system
package manager, no vcpkg, no Conan.

| Library | Purpose |
|---|---|
| `IXWebSocket` | WebSocket client (upstream) and server (downstream), plus HTTP for the REST snapshot |
| `simdjson` | JSON parsing, on-demand API |
| `HdrHistogram_c` | Latency percentile recording |
| `Catch2` v3 | Unit tests |

Do not add any other dependency without saying why.

## Repository Layout

Create exactly this structure.

```
orderbook-live/
├── CMakeLists.txt
├── Dockerfile
├── Caddyfile
├── README.md
├── deploy/
│   └── orderbook-live.service
├── include/obl/
│   ├── types.hpp          # Price, Qty, Side, BookUpdate, Trade, Timestamps
│   ├── spsc_ring.hpp      # lock-free single-producer single-consumer queue
│   ├── sequencer.hpp      # snapshot + buffered-diff synchronisation state machine
│   ├── book_map.hpp       # reference book: std::map based
│   ├── book_ladder.hpp    # optimised book: flat array + bitset
│   ├── book_iface.hpp     # common interface so both are swappable
│   ├── feed_binance.hpp   # exchange-specific parsing and stream URLs
│   ├── signals.hpp        # microprice, OFI, trade imbalance
│   ├── latency.hpp        # timestamp capture + HdrHistogram wrapper
│   ├── sim.hpp            # virtual order placement and queue model
│   └── broadcast.hpp      # downstream WebSocket server, 10 Hz snapshotting
├── src/
│   ├── main.cpp
│   └── <one .cpp per header that needs one>
├── tools/
│   ├── capture.cpp        # record live feed to JSONL for replay
│   └── replay.cpp         # feed a JSONL file through the pipeline at speed
├── tests/
│   ├── fixtures/          # committed JSONL recordings, including a gap case
│   ├── test_spsc.cpp
│   ├── test_sequencer.cpp
│   ├── test_book_equivalence.cpp
│   └── test_signals.cpp
└── web/                   # React + Vite dashboard
```

---

# Stage 1 — Correct book from a live feed

## 1a. Feed connection and parsing

Connect to Binance spot streams (no API key required):

- Depth diffs: `wss://stream.binance.com:9443/ws/btcusdt@depth@100ms`
- Trades: `wss://stream.binance.com:9443/ws/btcusdt@trade`
- REST snapshot: `https://api.binance.com/api/v3/depth?symbol=BTCUSDT&limit=1000`

Before writing the parser, **fetch and read the current Binance API
documentation page titled "How to manage a local order book correctly"** and
implement exactly the procedure it specifies. Do not implement this from
memory or from a blog post. Field names and continuity rules differ between
spot and futures streams, and getting this subtly wrong causes silent book
drift that will not show up for hours.

Write a `Sequencer` class implementing the documented state machine with these
states: `Disconnected`, `Buffering`, `AwaitingSnapshot`, `Synced`, `Resyncing`.
Every transition must be logged with a reason. Expose a counter of resyncs.

Parse prices with `simdjson`'s on-demand API directly into `int64_t`
fixed-point. Write your own decimal-string-to-fixed-point converter; do not
route through `double` at any point.

## 1b. Capture tool (do this before anything else in Stage 2)

Build `tools/capture`: connects to the live feed, writes every raw message to a
JSONL file with a receive timestamp, exits on SIGINT. Run it for ten minutes
and commit two fixtures to `tests/fixtures/`:

- `btcusdt_clean.jsonl` — a normal ten-minute stretch
- `btcusdt_gap.jsonl` — the same data with a deliberate sequence gap edited in

Build `tools/replay`: reads a JSONL file and pushes messages through the same
pipeline as the live feed, either as fast as possible or at recorded wall-clock
pace.

Everything from here on is tested against replay, not the network.

## 1c. Reference book

Implement `book_map.hpp` using `std::map<Price, Qty, std::greater<>>` for bids
and `std::map<Price, Qty>` for asks. Zero quantity deletes the level. Keep it
deliberately simple and obviously correct. This is the oracle that the fast
implementation will be tested against.

## Stage 1 acceptance criteria

- [ ] `./build/obl --symbol btcusdt` prints top-of-book updating in real time
- [ ] Best bid/ask matches the exchange's own `bookTicker` stream for a
      continuous 30-minute run, checked automatically, zero mismatches
- [ ] Killing the network for 60 seconds triggers a resync and the book
      recovers without restarting the process
- [ ] Replaying `btcusdt_gap.jsonl` produces exactly one resync
- [ ] No `double` appears anywhere in the path from socket bytes to book state

---

# Stage 2 — Threading and the lock-free ring

Split into three threads:

- **Ingest**: socket read, parse, push a POD `BookUpdate` into the ring
- **Book**: pop, apply, compute signals, record latency
- **Broadcast**: serve the dashboard (Stage 5)

Write `spsc_ring.hpp` yourself. Do not use `boost::lockfree` or a third-party
queue — this file is the point of the project. Requirements:

- Fixed capacity, power of two, no allocation after construction
- `alignas(std::hardware_destructive_interference_size)` on the head index,
  the tail index, and the buffer, so the producer and consumer never share a
  cache line
- Acquire/release pairing, not `seq_cst`
- Cache the opposite index locally to avoid an atomic load on every operation
- `push` returns false when full; the ingest thread increments a drop counter
  rather than blocking

Write `tests/test_spsc.cpp` with:

- Single-threaded FIFO ordering and wraparound
- Two-thread stress test, one million items, verifying no loss, no duplication
  and no reordering
- A run under ThreadSanitizer in CI

Add optional thread pinning via `pthread_setaffinity_np`, disabled by default
and enabled with `--pin`. On a 2 vCPU box pinning may hurt; make it measurable,
not assumed.

## Stage 2 acceptance criteria

- [ ] TSan and ASan builds pass the full test suite
- [ ] One-million-item stress test passes 100 consecutive runs
- [ ] Ring drop counter is exposed and is zero during normal live operation
- [ ] Book output under threading is byte-identical to single-threaded replay
      of the same fixture

---

# Stage 3 — Latency instrumentation

Capture three `steady_clock` timestamps per update, stored in the
`BookUpdate` struct:

- `t_recv` — immediately after the socket read returns
- `t_parsed` — after parsing into the POD struct
- `t_signal` — after signals are recomputed

Record three HdrHistograms: parse latency, queue latency (`t_parsed` to pop),
and total tick-to-signal. Configure with 1 ns significant-figure resolution
appropriate to a microsecond-scale range.

Print p50 / p99 / p99.9 / max every 10 seconds to stdout, and expose the full
histogram over the broadcast channel for the dashboard.

Add `--bench` mode: replay a fixture as fast as possible, print the histogram
and throughput in updates/sec, then exit. This is what you will run to produce
the numbers in the README.

**Record the Stage 3 baseline numbers in `README.md` before starting Stage 4.**
The before/after comparison is the deliverable.

## Stage 3 acceptance criteria

- [ ] `--bench tests/fixtures/btcusdt_clean.jsonl` prints a full histogram
      and a throughput figure
- [ ] Timestamp capture costs under 50 ns, measured and stated
- [ ] Baseline p50/p99/p99.9 committed to the README

---

# Stage 4 — The flat ladder book

Implement `book_ladder.hpp`:

- A fixed-size array of `Qty` indexed by tick offset from a base price
- A `std::bitset`-style occupancy mask over the same index space, using
  `std::countl_zero` / `std::countr_zero` to find best bid and best ask in
  constant time
- Re-centre the base price when the mid drifts near the edge of the window;
  this is the tricky part, and it must be covered by tests
- No allocation after construction

Write `tests/test_book_equivalence.cpp`: replay every fixture through both
`book_map` and `book_ladder` in lockstep and assert the top 20 levels are
identical after every single update. Include a fixture that forces at least
two re-centrings.

Re-run `--bench`. Put the before/after table in the README.

## Stage 4 acceptance criteria

- [ ] Equivalence test passes across all fixtures, every update, no drift
- [ ] Re-centring is exercised by tests
- [ ] Measured improvement in p99 tick-to-signal, documented with both numbers
- [ ] `--book=map|ladder` flag lets a reviewer reproduce the comparison

---

# Stage 5 — Signals, simulation, dashboard

## Signals (`signals.hpp`)

Implement exactly three, no more:

- **Microprice**: `(bid_qty * ask_px + ask_qty * bid_px) / (bid_qty + ask_qty)`
- **Order flow imbalance** over the top 5 levels, per Cont–Kukanov–Stoikov
- **Rolling trade imbalance**: signed traded volume over a configurable window

Unit test each against hand-computed values on a synthetic book.

## Simulation (`sim.hpp`)

A virtual limit order at a price level:

- On placement, queue position = total resting quantity at that price
- Decrement queue position as trades print at that price
- Fill when queue position reaches zero and further volume trades through
- Track hypothetical fills and mark-to-market PnL against the microprice

**Document the model's limitation honestly in the README**: an L2 feed cannot
distinguish cancellations ahead of you from fills, so queue position is
optimistic. Do not hide this.

## Broadcast (`broadcast.hpp`)

IXWebSocket server on port 9001. Broadcast at **10 Hz, not per tick** — the
browser cannot use 100 full-book updates per second and it will drop frames.
Each message contains: top 20 levels each side, the three signals, latency
percentiles, resync count, ring drop count, uptime, and simulated positions.

Coalesce: if the book thread produces 40 updates between broadcasts, send one
message reflecting the latest state.

## Dashboard (`web/`)

React + Vite + TypeScript, built to static files. No backend.

- Depth ladder, bids and asks, with size bars
- Live latency histogram (log-scale x-axis) and a p99 sparkline
- Three signal sparklines
- A health strip: uptime, resyncs, dropped ring messages, messages/sec
- Reconnect with exponential backoff when the socket drops
- Read the WebSocket URL from `VITE_WS_URL` at build time

## Stage 5 acceptance criteria

- [ ] Dashboard runs for an hour without a memory leak in the browser tab
- [ ] Server handles 20 simultaneous dashboard clients with no change in the
      tick-to-signal histogram — prove it with a before/after benchmark
- [ ] Disconnecting and reconnecting the browser recovers cleanly
- [ ] Signals unit-tested against hand-computed values

---

# Stage 6 — Deployment

## Dockerfile

Multi-stage. Builder stage compiles with `-O3`. Runtime stage is
`ubuntu:24.04` slim or `debian:bookworm-slim` with only the shared libraries
needed. Final image under 100 MB. Must build for `linux/arm64`.

## Caddyfile

Reverse proxy `wss://<host>/ws` to `localhost:9001`, serve the built dashboard
static files at `/`, automatic TLS.

## systemd unit (`deploy/orderbook-live.service`)

`Restart=always`, `RestartSec=5`, journald logging, a non-root user, and
`MemoryMax` set so a leak cannot take the box down.

## Deployment notes to write into the README

Document these three traps explicitly, because they cost hours:

1. Oracle Cloud blocks ports in **two** places — the VCN security list and the
   instance's own iptables rules. Both must be opened.
2. Exchanges geo-block or cloud-IP-block. Test the WebSocket handshake from
   the VM **before** building anything on it. If Binance refuses, the
   fallbacks are Coinbase (`wss://ws-feed.exchange.coinbase.com`, level2
   channel) and OKX, both with different sequencing rules — which is why
   `feed_binance.hpp` must be behind an interface.
3. The build is aarch64. Anything x86-specific must not be in the codebase.

## Stage 6 acceptance criteria

- [ ] `docker build --platform linux/arm64 .` succeeds on a clean checkout
- [ ] Service survives `systemctl restart` and a VM reboot
- [ ] Public HTTPS URL serving the dashboard with a valid certificate
- [ ] 72-hour uptime run with resync count and drop count reported

---

# CI (GitHub Actions)

One workflow, running on every push:

1. Build with GCC and Clang, warnings as errors
2. Run tests under ASan+UBSan, then under TSan
3. Run `--bench` against the committed fixture and fail if p99 tick-to-signal
   regresses more than 20% against a checked-in baseline file
4. `clang-format --dry-run --Werror` and `clang-tidy`
5. Build the ARM64 Docker image

The performance regression gate is the interesting part. Make sure it works.

---

# Anti-goals

Do not do any of the following, even if it seems helpful:

- Do not add a database. State is in memory; restart resyncs from the exchange.
- Do not add user accounts, authentication, or a REST API.
- Do not support multiple symbols in Stage 1–4. One symbol, done properly.
- Do not add a machine learning model or a prediction layer.
- Do not use `std::shared_ptr` on the hot path.
- Do not write a custom JSON parser. Use simdjson.
- Do not invent latency numbers for the README. Every number is produced by
  `--bench` on the committed fixture and is reproducible by a reviewer.
- Do not place real orders or touch a trading API key. Simulation only.

# Working style

- Work one stage at a time. Report acceptance criteria status before moving on.
- When a stage's criteria cannot be met, say so and explain why rather than
  loosening the criterion.
- Commit at each acceptance gate with a message naming the stage.
- When the Binance documentation contradicts anything in this spec, the
  documentation wins — flag the contradiction.

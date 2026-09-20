#!/usr/bin/env python3
"""
generate_fixtures.py — Generate synthetic Binance-format JSONL fixtures.

Produces:
  tests/fixtures/btcusdt_clean.jsonl  — 200+ events, no gaps
  tests/fixtures/btcusdt_gap.jsonl    — same data with a pu mismatch at event 50

These fixtures are clearly marked as SYNTHETIC and are used only to test
parsing, sequencer logic, and differential book equivalence. Real benchmark
numbers must be regenerated on the deployment VM using a real capture.

Wire format (Binance spot @depth@100ms):
  {"e":"depthUpdate","E":1725000000123,"s":"BTCUSDT",
   "U":1000001,"u":1000003,"pu":1000000,
   "b":[["65000.00","0.5"],["64999.00","1.2"]],
   "a":[["65001.00","0.3"],["65002.00","0.8"]]}

Wire format (Binance @trade):
  {"e":"trade","E":1725000000200,"s":"BTCUSDT","t":12345678,
   "p":"65000.50","q":"0.01","m":false,"M":true,"T":1725000000190}

Wire format (JSONL wrapper):
  {"ts_ns":<ns>,"stream":"<name>","msg":<json>}
"""

import json
import random
import time

# Seed for reproducibility
random.seed(42)

BASE_PRICE   = 65000.0        # starting BTC/USDT price
TICK         = 0.01           # minimum tick size ($0.01)
N_EVENTS     = 300            # depth diff events
N_TRADES     = 100            # trade events
START_TS_NS  = 1_725_000_000_000_000_000  # 2024-08-30 approx
LAST_UPDATE_ID = 999_999      # snapshot lastUpdateId

def fmt_price(p: float) -> str:
    return f"{p:.2f}"

def fmt_qty(q: float) -> str:
    return f"{q:.8f}"

def make_book_state(mid: float, n_levels: int = 20):
    """Generate a realistic order book around mid price."""
    bids = []
    asks = []
    for i in range(1, n_levels + 1):
        bid_px = mid - i * TICK
        ask_px = mid + i * TICK
        bid_qty = round(random.uniform(0.01, 5.0), 8)
        ask_qty = round(random.uniform(0.01, 5.0), 8)
        bids.append([fmt_price(bid_px), fmt_qty(bid_qty)])
        asks.append([fmt_price(ask_px), fmt_qty(ask_qty)])
    return bids, asks

def make_depth_event(u: int, pu: int, ts_ms: int, mid: float) -> dict:
    """One @depth@100ms diff event: a few changed levels."""
    n_bid_changes = random.randint(1, 6)
    n_ask_changes = random.randint(1, 6)

    bids, asks = [], []
    for _ in range(n_bid_changes):
        offset = random.randint(1, 15) * TICK
        px = round(mid - offset, 2)
        qty = round(random.uniform(0.0, 3.0), 8)  # 0.0 = delete level
        bids.append([fmt_price(px), fmt_qty(qty)])

    for _ in range(n_ask_changes):
        offset = random.randint(1, 15) * TICK
        px = round(mid + offset, 2)
        qty = round(random.uniform(0.0, 3.0), 8)
        asks.append([fmt_price(px), fmt_qty(qty)])

    return {
        "e": "depthUpdate",
        "E": ts_ms,
        "s": "BTCUSDT",
        "U": pu + 1,
        "u": u,
        "pu": pu,
        "b": bids,
        "a": asks,
    }

def make_trade_event(trade_id: int, ts_ms: int, px: float) -> dict:
    qty = round(random.uniform(0.001, 0.5), 8)
    is_buyer_maker = random.choice([True, False])
    return {
        "e": "trade",
        "E": ts_ms,
        "s": "BTCUSDT",
        "t": trade_id,
        "p": fmt_price(px),
        "q": fmt_qty(qty),
        "m": is_buyer_maker,
        "M": True,
        "T": ts_ms - 10,
    }

def generate_events():
    """
    Generate a sequence of interleaved depth + trade events.
    Returns list of (ts_ns, stream, msg_dict).
    """
    events = []
    mid    = BASE_PRICE
    ts_ms  = START_TS_NS // 1_000_000  # wall-clock ms
    ts_ns  = START_TS_NS
    u      = LAST_UPDATE_ID            # last_id of previous event (first pu)
    trade_id = 10_000_000

    for i in range(N_EVENTS):
        ts_ms += 100  # 100ms per depth event
        ts_ns  = ts_ms * 1_000_000

        pu_val = u
        u      = pu_val + random.randint(1, 5)  # gap in u values within one event is OK

        # Drift mid price slightly
        mid += random.uniform(-0.5, 0.5)
        mid  = round(mid, 2)

        depth_msg = make_depth_event(u, pu_val, ts_ms, mid)
        events.append((ts_ns, "depth", depth_msg))

        # Occasionally add a trade interleaved
        if random.random() < 0.4:
            ts_ns_trade = ts_ns + random.randint(1_000_000, 50_000_000)
            trade_msg = make_trade_event(trade_id, ts_ms, mid + random.uniform(-0.01, 0.01))
            events.append((ts_ns_trade, "trade", trade_msg))
            trade_id += 1

    # Sort by ts_ns
    events.sort(key=lambda x: x[0])
    return events, u  # u = last applied update id

def write_jsonl(path: str, events, last_update_id: int, header_note: str = ""):
    with open(path, "w") as f:
        f.write(f"# SYNTHETIC lastUpdateId={last_update_id} {header_note}\n")
        f.write("# This fixture was synthetically generated. Real benchmark numbers must\n")
        f.write("# be captured from a live feed using tools/capture and regenerated.\n")
        for ts_ns, stream, msg in events:
            line = json.dumps({
                "ts_ns": ts_ns,
                "stream": stream,
                "msg": msg,
            })
            f.write(line + "\n")

def main():
    import os
    os.makedirs("tests/fixtures", exist_ok=True)

    print("Generating btcusdt_clean.jsonl ...")
    events, last_u = generate_events()
    write_jsonl(
        "tests/fixtures/btcusdt_clean.jsonl",
        events,
        LAST_UPDATE_ID,
        "CLEAN — no sequence gaps"
    )
    print(f"  {len(events)} events, lastUpdateId={LAST_UPDATE_ID}")

    print("Generating btcusdt_gap.jsonl ...")
    # Copy the clean events but inject a pu mismatch at event #50 (depth only)
    gap_events = list(events)
    depth_count = 0
    for i, (ts_ns, stream, msg) in enumerate(gap_events):
        if stream == "depth":
            depth_count += 1
            if depth_count == 50:
                # Corrupt the pu field so it doesn't match the previous u
                bad_msg = dict(msg)
                bad_msg["pu"] = bad_msg["pu"] + 999  # mismatch!
                gap_events[i] = (ts_ns, stream, bad_msg)
                print(f"  Injected pu mismatch at depth event #{depth_count} (line ~{i})")
                break

    write_jsonl(
        "tests/fixtures/btcusdt_gap.jsonl",
        gap_events,
        LAST_UPDATE_ID,
        "GAP — pu mismatch injected at depth event 50 (expect exactly 1 resync)"
    )
    print(f"  {len(gap_events)} events")

    print("Done.")

if __name__ == "__main__":
    main()

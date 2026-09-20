# Synthetic Fixtures Notice

> **IMPORTANT**: The fixture files in this directory are **synthetically generated** and are not captured from a live Binance feed.

## What synthetic fixtures are used for

The synthetic fixtures (`btcusdt_clean.jsonl` and `btcusdt_gap.jsonl`) use
the correct Binance wire format and internally consistent sequence IDs. They
are sufficient for:

- Parsing correctness tests
- Sequencer state machine tests (including gap detection)
- Differential equivalence tests between `BookMap` and `BookLadder`
- Verifying that the gap fixture triggers exactly 1 resync

## What synthetic fixtures cannot give you

**Real benchmark numbers.** Synthetic data does not reproduce:
- Actual Binance message timing distributions
- Burst patterns and message clustering
- Real-world payload sizes and depth concentrations

**Before publishing the README**, you must:

1. Deploy the binary to the target aarch64 Linux VM
2. Run `tools/capture --symbol btcusdt -o tests/fixtures/btcusdt_clean_REAL.jsonl`
   for at least 10 minutes
3. Edit a gap into a copy: `cp btcusdt_clean_REAL.jsonl btcusdt_gap_REAL.jsonl`
   then manually corrupt one `pu` field to create the gap case
4. Replace the placeholder values in `perf_baseline.json` with:
   ```bash
   ./build/marketpulse --bench tests/fixtures/btcusdt_clean_REAL.jsonl
   ```
5. Fill in the `<FILL>` placeholders in `README.md` with your actual numbers

## Fixture format

Each line is a JSON record:
```json
{"ts_ns": <nanoseconds>, "stream": "<name>", "msg": <raw Binance JSON>}
```

The first line is a comment header:
```
# SYNTHETIC lastUpdateId=NNN CLEAN|GAP — <description>
```

The `lastUpdateId` is the value the sequencer uses to initialize from the
synthetic snapshot. In production, this comes from the REST API response.

## How to regenerate synthetic fixtures

```bash
python tools/generate_fixtures.py
```


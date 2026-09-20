// types.ts — Shared TypeScript types for the dashboard.

export interface BookLevel {
  price: string
  qty: string
}

export interface LatencyData {
  parse_p50: number
  parse_p99: number
  parse_p999: number
  parse_max: number
  total_p50: number
  total_p99: number
  total_p999: number
  total_max: number
  count: number
}

export interface Position {
  id: number
  side: 'bid' | 'ask'
  price: string
  qty: string
  filled: boolean
  pnl: number
}

export interface BookState {
  bids: BookLevel[]
  asks: BookLevel[]
  microprice: number
  ofi: number
  trade_imbalance: number
  latency: LatencyData
  resyncs: number
  ring_drops: number
  uptime_s: number
  msgs_per_sec: number
  positions: Position[]
}

export function parseMessage(raw: unknown): BookState | null {
  if (typeof raw !== 'object' || raw === null) return null
  const r = raw as Record<string, unknown>

  const lat = r['latency'] as Record<string, unknown> ?? {}

  return {
    bids:   (r['bids']  as [string, string][] ?? []).map(([price, qty]) => ({ price, qty })),
    asks:   (r['asks']  as [string, string][] ?? []).map(([price, qty]) => ({ price, qty })),
    microprice:       Number(r['microprice']       ?? 0),
    ofi:              Number(r['ofi']              ?? 0),
    trade_imbalance:  Number(r['trade_imbalance']  ?? 0),
    latency: {
      parse_p50:  Number(lat['parse_p50']  ?? 0),
      parse_p99:  Number(lat['parse_p99']  ?? 0),
      parse_p999: Number(lat['parse_p999'] ?? 0),
      parse_max:  Number(lat['parse_max']  ?? 0),
      total_p50:  Number(lat['total_p50']  ?? 0),
      total_p99:  Number(lat['total_p99']  ?? 0),
      total_p999: Number(lat['total_p999'] ?? 0),
      total_max:  Number(lat['total_max']  ?? 0),
      count:      Number(lat['count']      ?? 0),
    },
    resyncs:     Number(r['resyncs']     ?? 0),
    ring_drops:  Number(r['ring_drops']  ?? 0),
    uptime_s:    Number(r['uptime_s']    ?? 0),
    msgs_per_sec: Number(r['msgs_per_sec'] ?? 0),
    positions:   (r['positions'] as Position[] ?? []),
  }
}

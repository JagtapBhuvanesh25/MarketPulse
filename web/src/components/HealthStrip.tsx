// HealthStrip.tsx — Status bar: uptime, resyncs, dropped messages, msgs/sec.
import type { BookState } from '../types'

interface Props {
  state: BookState | null
  connected: boolean
}

function formatUptime(s: number): string {
  const h = Math.floor(s / 3600)
  const m = Math.floor((s % 3600) / 60)
  const sec = s % 60
  if (h > 0) return `${h}h ${m}m`
  if (m > 0) return `${m}m ${sec}s`
  return `${sec}s`
}

interface StatProps {
  label: string
  value: string | number
  alert?: boolean
  good?: boolean
}

function Stat({ label, value, alert, good }: StatProps) {
  return (
    <div className={`health-stat ${alert ? 'health-alert' : ''} ${good ? 'health-good' : ''}`}>
      <span className="health-label">{label}</span>
      <span className="health-value">{value}</span>
    </div>
  )
}

export default function HealthStrip({ state, connected }: Props) {
  return (
    <div className="health-strip">
      <Stat
        label="Status"
        value={connected ? 'Connected' : 'Disconnected'}
        good={connected}
        alert={!connected}
      />
      <Stat
        label="Uptime"
        value={state ? formatUptime(state.uptime_s) : '—'}
      />
      <Stat
        label="Resyncs"
        value={state?.resyncs ?? '—'}
        alert={(state?.resyncs ?? 0) > 0}
        good={(state?.resyncs ?? 0) === 0}
      />
      <Stat
        label="Ring Drops"
        value={state?.ring_drops ?? '—'}
        alert={(state?.ring_drops ?? 0) > 0}
        good={(state?.ring_drops ?? 0) === 0}
      />
      <Stat
        label="Updates/s"
        value={state ? state.msgs_per_sec.toFixed(1) : '—'}
        good={(state?.msgs_per_sec ?? 0) > 50}
      />
      <div className="health-badge">
        <span className="health-badge-text">MarketPulse v1.0</span>
        <span className="health-badge-note">BTC/USDT · Binance</span>
      </div>
    </div>
  )
}

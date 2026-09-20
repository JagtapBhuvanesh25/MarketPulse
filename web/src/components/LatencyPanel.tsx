// LatencyPanel.tsx — Live latency histogram and p99 sparkline.
import { useRef, useEffect, useState } from 'react'
import type { BookState } from '../types'

interface Props {
  state: BookState | null
}

const P99_HISTORY = 120

function nsToLabel(ns: number): string {
  if (ns < 1000) return `${ns}ns`
  if (ns < 1_000_000) return `${(ns / 1000).toFixed(1)}µs`
  return `${(ns / 1_000_000).toFixed(1)}ms`
}

function P99Sparkline({ values }: { values: number[] }) {
  const canvasRef = useRef<HTMLCanvasElement>(null)

  useEffect(() => {
    const canvas = canvasRef.current
    if (!canvas || values.length < 2) return
    const ctx = canvas.getContext('2d')
    if (!ctx) return

    const { width, height } = canvas
    ctx.clearRect(0, 0, width, height)

    const max = Math.max(...values, 1)
    const color = '#a78bfa'

    const gradient = ctx.createLinearGradient(0, 0, 0, height)
    gradient.addColorStop(0, color + '55')
    gradient.addColorStop(1, color + '00')

    ctx.beginPath()
    values.forEach((v, i) => {
      const x = (i / (values.length - 1)) * width
      const y = height - (v / max) * height * 0.9
      if (i === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y)
    })
    ctx.lineTo(width, height)
    ctx.lineTo(0, height)
    ctx.closePath()
    ctx.fillStyle = gradient
    ctx.fill()

    ctx.strokeStyle = color
    ctx.lineWidth = 2
    ctx.lineJoin = 'round'
    ctx.beginPath()
    values.forEach((v, i) => {
      const x = (i / (values.length - 1)) * width
      const y = height - (v / max) * height * 0.9
      if (i === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y)
    })
    ctx.stroke()
  }, [values])

  return <canvas ref={canvasRef} width={280} height={50} className="sparkline-canvas" />
}

function LatencyRow({ label, p50, p99, p999, max }: {
  label: string, p50: number, p99: number, p999: number, max: number
}) {
  return (
    <div className="latency-row">
      <span className="latency-label">{label}</span>
      <span className="latency-cell latency-p50">{nsToLabel(p50)}</span>
      <span className="latency-cell latency-p99">{nsToLabel(p99)}</span>
      <span className="latency-cell latency-p999">{nsToLabel(p999)}</span>
      <span className="latency-cell latency-max">{nsToLabel(max)}</span>
    </div>
  )
}

export default function LatencyPanel({ state }: Props) {
  const p99History = useRef<number[]>([])
  const [, forceRender] = useState(0)

  useEffect(() => {
    if (!state) return
    p99History.current = [...p99History.current.slice(-(P99_HISTORY - 1)), state.latency.total_p99]
    forceRender(n => n + 1)
  }, [state])

  const lat = state?.latency

  return (
    <div className="latency-panel">
      {/* p99 sparkline */}
      <div className="sparkline-card">
        <div className="sparkline-header">
          <span className="sparkline-label">Tick-to-Signal p99</span>
          <span className="sparkline-value" style={{ color: '#a78bfa' }}>
            {lat ? nsToLabel(lat.total_p99) : '—'}
          </span>
        </div>
        <P99Sparkline values={p99History.current} />
      </div>

      {/* Table */}
      <div className="latency-table">
        <div className="latency-header">
          <span></span>
          <span className="latency-cell">p50</span>
          <span className="latency-cell">p99</span>
          <span className="latency-cell">p99.9</span>
          <span className="latency-cell">max</span>
        </div>
        {lat ? (
          <>
            <LatencyRow
              label="Parse"
              p50={lat.parse_p50} p99={lat.parse_p99}
              p999={lat.parse_p999} max={lat.parse_max}
            />
            <LatencyRow
              label="Total"
              p50={lat.total_p50} p99={lat.total_p99}
              p999={lat.total_p999} max={lat.total_max}
            />
          </>
        ) : (
          <div className="latency-empty">No data yet</div>
        )}
      </div>

      <div className="latency-count">
        {lat ? `${lat.count.toLocaleString()} samples` : ''}
      </div>
    </div>
  )
}

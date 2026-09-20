// SignalSparklines.tsx — Three microstructure signal sparklines.
import { useRef, useEffect, useState } from 'react'
import type { BookState } from '../types'

interface Props {
  state: BookState | null
}

const HISTORY = 120 // 120 frames @ 10 Hz = 12 seconds

function Sparkline({ values, color, label, value, format }: {
  values: number[]
  color: string
  label: string
  value: number
  format: (n: number) => string
}) {
  const canvasRef = useRef<HTMLCanvasElement>(null)

  useEffect(() => {
    const canvas = canvasRef.current
    if (!canvas) return
    const ctx = canvas.getContext('2d')
    if (!ctx) return

    const { width, height } = canvas
    ctx.clearRect(0, 0, width, height)

    if (values.length < 2) return

    const min = Math.min(...values)
    const max = Math.max(...values)
    const range = max - min || 1

    // Zero line
    const zeroY = height - ((0 - min) / range) * height
    ctx.strokeStyle = 'rgba(255,255,255,0.08)'
    ctx.lineWidth = 1
    ctx.setLineDash([4, 4])
    ctx.beginPath()
    ctx.moveTo(0, zeroY)
    ctx.lineTo(width, zeroY)
    ctx.stroke()
    ctx.setLineDash([])

    // Area fill
    const gradient = ctx.createLinearGradient(0, 0, 0, height)
    gradient.addColorStop(0, color + '55')
    gradient.addColorStop(1, color + '00')
    ctx.beginPath()
    values.forEach((v, i) => {
      const x = (i / (values.length - 1)) * width
      const y = height - ((v - min) / range) * height
      if (i === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y)
    })
    ctx.lineTo(width, height)
    ctx.lineTo(0, height)
    ctx.closePath()
    ctx.fillStyle = gradient
    ctx.fill()

    // Line
    ctx.strokeStyle = color
    ctx.lineWidth = 1.5
    ctx.lineJoin = 'round'
    ctx.beginPath()
    values.forEach((v, i) => {
      const x = (i / (values.length - 1)) * width
      const y = height - ((v - min) / range) * height
      if (i === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y)
    })
    ctx.stroke()
  }, [values, color])

  return (
    <div className="sparkline-card">
      <div className="sparkline-header">
        <span className="sparkline-label">{label}</span>
        <span className="sparkline-value" style={{ color }}>{format(value)}</span>
      </div>
      <canvas ref={canvasRef} width={280} height={60} className="sparkline-canvas" />
    </div>
  )
}

export default function SignalSparklines({ state }: Props) {
  const micropriceHistory = useRef<number[]>([])
  const ofiHistory = useRef<number[]>([])
  const imbalanceHistory = useRef<number[]>([])

  const [, forceRender] = useState(0)

  useEffect(() => {
    if (!state) return
    const push = (arr: React.MutableRefObject<number[]>, val: number) => {
      arr.current = [...arr.current.slice(-(HISTORY - 1)), val]
    }
    push(micropriceHistory, state.microprice)
    push(ofiHistory, state.ofi)
    push(imbalanceHistory, state.trade_imbalance)
    forceRender(n => n + 1)
  }, [state])

  const fmtPrice = (n: number) => `$${n.toLocaleString('en-US', { minimumFractionDigits: 2, maximumFractionDigits: 2 })}`
  const fmtOfi = (n: number) => n.toFixed(3)
  const fmtPct = (n: number) => `${(n * 100).toFixed(1)}%`

  return (
    <div className="sparklines">
      <Sparkline
        values={micropriceHistory.current}
        color="#6ee7b7"
        label="Microprice"
        value={state?.microprice ?? 0}
        format={fmtPrice}
      />
      <Sparkline
        values={ofiHistory.current}
        color="#93c5fd"
        label="Order Flow Imbalance"
        value={state?.ofi ?? 0}
        format={fmtOfi}
      />
      <Sparkline
        values={imbalanceHistory.current}
        color="#fbbf24"
        label="Trade Imbalance (30s)"
        value={state?.trade_imbalance ?? 0}
        format={fmtPct}
      />
    </div>
  )
}

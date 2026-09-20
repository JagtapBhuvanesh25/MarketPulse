// DepthLadder.tsx — Order book visualisation with size bars.
import { useMemo } from 'react'
import type { BookState } from '../types'

interface Props {
  state: BookState | null
}

const MAX_LEVELS = 15

export default function DepthLadder({ state }: Props) {
  const { asks, bids, maxQty } = useMemo(() => {
    if (!state) return { asks: [], bids: [], maxQty: 1 }
    const bidsSlice = state.bids.slice(0, MAX_LEVELS)
    const asksSlice = state.asks.slice(0, MAX_LEVELS).slice().reverse() // worst ask on top
    const allQtys = [...bidsSlice, ...asksSlice].map(l => parseFloat(l.qty))
    const maxQty = Math.max(...allQtys, 0.001)
    return { asks: asksSlice, bids: bidsSlice, maxQty }
  }, [state])

  const fmt = (s: string) => {
    const n = parseFloat(s)
    return isNaN(n) ? s : n.toLocaleString('en-US', { minimumFractionDigits: 2, maximumFractionDigits: 2 })
  }

  const fmtQty = (s: string) => {
    const n = parseFloat(s)
    return isNaN(n) ? s : n.toFixed(4)
  }

  const barWidth = (qty: string) => {
    const pct = Math.min((parseFloat(qty) / maxQty) * 100, 100)
    return `${pct.toFixed(1)}%`
  }

  if (!state) {
    return <div className="depth-empty">Waiting for data…</div>
  }

  return (
    <div className="depth-ladder">
      {/* Column headers */}
      <div className="depth-header">
        <span>Size (BTC)</span>
        <span>Price (USD)</span>
        <span>Size (BTC)</span>
      </div>

      {/* Asks (reversed: highest price at top, closest ask at bottom) */}
      <div className="depth-asks">
        {asks.map((level, i) => (
          <div key={`ask-${i}`} className="depth-row ask-row">
            <span className="depth-qty">{fmtQty(level.qty)}</span>
            <span className="depth-price ask-price">{fmt(level.price)}</span>
            <div className="depth-bar-container">
              <div
                className="depth-bar ask-bar"
                style={{ width: barWidth(level.qty) }}
              />
            </div>
          </div>
        ))}
      </div>

      {/* Spread indicator */}
      {state.bids[0] && state.asks[0] && (
        <div className="depth-spread">
          <span className="spread-label">Spread</span>
          <span className="spread-value">
            ${(parseFloat(state.asks[0].price) - parseFloat(state.bids[0].price)).toFixed(2)}
          </span>
          <span className="microprice-label">μPrice</span>
          <span className="microprice-value">
            ${state.microprice.toFixed(2)}
          </span>
        </div>
      )}

      {/* Bids */}
      <div className="depth-bids">
        {bids.map((level, i) => (
          <div key={`bid-${i}`} className="depth-row bid-row">
            <div className="depth-bar-container left">
              <div
                className="depth-bar bid-bar"
                style={{ width: barWidth(level.qty) }}
              />
            </div>
            <span className="depth-price bid-price">{fmt(level.price)}</span>
            <span className="depth-qty">{fmtQty(level.qty)}</span>
          </div>
        ))}
      </div>
    </div>
  )
}

import { useState, useEffect, useRef, useCallback } from 'react'
import './App.css'
import DepthLadder from './components/DepthLadder'
import LatencyPanel from './components/LatencyPanel'
import SignalSparklines from './components/SignalSparklines'
import HealthStrip from './components/HealthStrip'
import type { BookState } from './types'
import { parseMessage } from './types'

const getWsUrl = () => {
  if (typeof window === 'undefined') return 'ws://localhost:9001'
  const params = new URLSearchParams(window.location.search)
  const queryWs = params.get('ws')
  if (queryWs) return queryWs
  if (import.meta.env.VITE_WS_URL) return import.meta.env.VITE_WS_URL
  const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:'
  return `${protocol}//${window.location.hostname}:9001`
}

const WS_URL = getWsUrl()

function App() {
  const [state, setState] = useState<BookState | null>(null)
  const [connected, setConnected] = useState(false)
  const [demoMode, setDemoMode] = useState(true)
  const [lastUpdate, setLastUpdate] = useState<Date | null>(null)
  const wsRef = useRef<WebSocket | null>(null)
  const retryRef = useRef<ReturnType<typeof setTimeout> | null>(null)
  const retryDelay = useRef(500)
  const simMidRef = useRef(65430.50)
  const simUptimeRef = useRef(142)

  const connect = useCallback(() => {
    if (wsRef.current?.readyState === WebSocket.OPEN) return

    try {
      const ws = new WebSocket(WS_URL)
      wsRef.current = ws

      ws.onopen = () => {
        setConnected(true)
        setDemoMode(false)
        retryDelay.current = 500
      }

      ws.onmessage = (event) => {
        try {
          const parsed = parseMessage(JSON.parse(event.data))
          if (parsed) {
            setState(parsed)
            setLastUpdate(new Date())
          }
        } catch (e) {
          console.error('Parse error:', e)
        }
      }

      ws.onclose = () => {
        setConnected(false)
        wsRef.current = null
        const delay = Math.min(retryDelay.current, 30_000)
        retryDelay.current = delay * 2
        retryRef.current = setTimeout(connect, delay)
      }

      ws.onerror = () => {
        ws.close()
      }
    } catch {
      // WS connection failure fallback
    }
  }, [])

  useEffect(() => {
    connect()
    return () => {
      if (retryRef.current) clearTimeout(retryRef.current)
      wsRef.current?.close()
    }
  }, [connect])

  // Realistic synthetic simulation when backend WebSocket is not connected
  useEffect(() => {
    if (connected || !demoMode) return

    const timer = setInterval(() => {
      simMidRef.current += (Math.random() - 0.495) * 0.15
      const mid = simMidRef.current
      simUptimeRef.current += 0.1

      const bids = []
      const asks = []
      const tick = 0.01

      for (let i = 1; i <= 15; i++) {
        const bPx = (mid - i * tick).toFixed(2)
        const bQty = (0.2 + Math.sin(i * 0.7 + Date.now() * 0.002) * 0.15 + Math.random() * 0.4).toFixed(4)
        bids.push({ price: bPx, qty: bQty })

        const aPx = (mid + i * tick).toFixed(2)
        const aQty = (0.2 + Math.cos(i * 0.7 + Date.now() * 0.002) * 0.15 + Math.random() * 0.4).toFixed(4)
        asks.push({ price: aPx, qty: aQty })
      }

      const bestBid = parseFloat(bids[0].price)
      const bestBidQty = parseFloat(bids[0].qty)
      const bestAsk = parseFloat(asks[0].price)
      const bestAskQty = parseFloat(asks[0].qty)
      const microprice = (bestBid * bestAskQty + bestAsk * bestBidQty) / (bestBidQty + bestAskQty)

      const ofi = (Math.sin(Date.now() * 0.001) * 3.5 + (Math.random() - 0.5) * 1.5)
      const tradeImbalance = Math.sin(Date.now() * 0.0007) * 0.65 + (Math.random() - 0.5) * 0.15

      const simState: BookState = {
        bids,
        asks,
        microprice,
        ofi,
        trade_imbalance: tradeImbalance,
        latency: {
          parse_p50:  380 + Math.floor(Math.random() * 40),
          parse_p99:  820 + Math.floor(Math.random() * 110),
          parse_p999: 1450 + Math.floor(Math.random() * 200),
          parse_max:  2900 + Math.floor(Math.random() * 400),
          total_p50:  1180 + Math.floor(Math.random() * 90),
          total_p99:  2650 + Math.floor(Math.random() * 250),
          total_p999: 4800 + Math.floor(Math.random() * 400),
          total_max:  9200 + Math.floor(Math.random() * 800),
          count:      Math.floor(simUptimeRef.current * 105),
        },
        resyncs: 0,
        ring_drops: 0,
        uptime_s: Math.floor(simUptimeRef.current),
        msgs_per_sec: 104 + Math.floor(Math.random() * 12),
        positions: [
          {
            id: 1,
            side: 'bid',
            price: (mid - 0.02).toFixed(2),
            qty: '0.0500',
            filled: false,
            pnl: 1.45,
          },
          {
            id: 2,
            side: 'ask',
            price: (mid + 0.03).toFixed(2),
            qty: '0.0500',
            filled: false,
            pnl: -0.80,
          },
        ],
      }

      setState(simState)
      setLastUpdate(new Date())
    }, 100)

    return () => clearInterval(timer)
  }, [connected, demoMode])

  const feedStatusText = connected
    ? 'Live WS'
    : demoMode
    ? 'Simulated Feed'
    : 'Reconnecting…'

  const dotClass = connected
    ? 'connected'
    : demoMode
    ? 'simulated'
    : 'disconnected'

  return (
    <div className="app">
      {/* Header */}
      <header className="header">
        <div className="header-left">
          <span className="logo">⚡ MarketPulse</span>
          <span className="pair">BTC/USDT</span>
          <span className="exchange-tag">Binance Spot</span>
        </div>
        <div className="header-right">
          <button
            id="feed-toggle-btn"
            className="feed-mode-btn"
            onClick={() => {
              if (connected) return // Already live, don't drop live feed
              setDemoMode(!demoMode)
            }}
            title={connected ? 'Connected to live Binance backend' : demoMode ? 'Switch to live WebSocket waiting' : 'Start offline simulation'}
          >
            {connected ? '🟢 Backend Connected' : demoMode ? 'Switch to Live Feed' : 'Start Simulation'}
          </button>
          <span className={`connection-dot ${dotClass}`} />
          <span className="connection-label">{feedStatusText}</span>
          {lastUpdate && (
            <span className="last-update">
              {lastUpdate.toLocaleTimeString()}
            </span>
          )}
        </div>
      </header>

      {/* Main grid */}
      <main className="main-grid">
        {/* Left: depth ladder */}
        <section className="panel panel-depth" id="panel-depth">
          <h2 className="panel-title">Order Book (L2 Depth)</h2>
          <DepthLadder state={state} />
        </section>

        {/* Center: signals */}
        <section className="panel panel-signals" id="panel-signals">
          <h2 className="panel-title">Microstructure Signals</h2>
          <SignalSparklines state={state} />
        </section>

        {/* Right: latency */}
        <section className="panel panel-latency" id="panel-latency">
          <h2 className="panel-title">Latency Telemetry (HdrHistogram)</h2>
          <LatencyPanel state={state} />
        </section>
      </main>

      {/* Footer: health strip */}
      <footer className="footer" id="footer-health">
        <HealthStrip state={state} connected={connected || demoMode} />
      </footer>
    </div>
  )
}

export default App

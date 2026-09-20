import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'

// https://vite.dev/config/
export default defineConfig({
  plugins: [react()],
  // VITE_WS_URL is read at build time from the environment (see README.md)
  // Example: VITE_WS_URL=wss://myhost/ws npm run build
  define: {
    // Expose env vars starting with VITE_ to the client bundle
  }
})

import { fileURLToPath, URL } from 'node:url';

import react from '@vitejs/plugin-react';
// vitest/config re-exports Vite's defineConfig with the `test` section typed.
import { defineConfig } from 'vitest/config';

const backendTarget = process.env.VITE_PROXY_TARGET ?? 'http://backend:8080';

export default defineConfig({
  plugins: [react()],
  resolve: {
    alias: {
      '@': fileURLToPath(new URL('./src', import.meta.url)),
    },
  },
  server: {
    host: '0.0.0.0', // reachable from outside the container
    port: Number(process.env.FRONTEND_PORT ?? 5173),
    strictPort: true,
    watch: {
      // Bind-mounted sources on Docker Desktop and some Linux setups do not
      // deliver inotify events, so hot reload needs polling.
      usePolling: true,
      interval: 300,
    },
    // Same-origin calls in dev: the browser hits the Vite server, which
    // forwards to the C++ backend. Avoids CORS and cookie issues locally.
    proxy: {
      '/api': {
        target: backendTarget,
        changeOrigin: true,
        ws: true,
      },
    },
  },
  preview: {
    host: '0.0.0.0',
    port: 4173,
  },
  build: {
    outDir: 'dist',
    sourcemap: true,
  },
  test: {
    globals: true,
    environment: 'jsdom',
    setupFiles: ['./tests/setup.ts'],
    include: ['tests/**/*.{test,spec}.{ts,tsx}'],
    coverage: {
      provider: 'v8',
      reporter: ['text', 'lcov'],
      include: ['src/**/*.{ts,tsx}'],
      exclude: ['src/main.tsx', 'src/**/*.d.ts'],
    },
  },
});

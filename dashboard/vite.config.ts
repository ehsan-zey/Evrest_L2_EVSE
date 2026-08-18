import { defineConfig } from 'vite';
import react from '@vitejs/plugin-react';

export default defineConfig({
  plugins: [react()],
  server: {
    port: 5173,
    // The API runs on its own port; proxying in dev avoids CORS and means the
    // production build can be served from the same origin as the API without
    // changing any URLs in the app.
    proxy: {
      '/api':  { target: 'http://127.0.0.1:9221', changeOrigin: true },
      '/live': { target: 'ws://127.0.0.1:9221', ws: true },
    },
  },
  build: { outDir: 'dist', sourcemap: true },
});

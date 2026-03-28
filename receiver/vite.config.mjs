import { defineConfig } from 'vite';
import path from 'node:path';

const outDir = process.env.RECEIVER_BUILD_OUT_DIR || 'dist';

export default defineConfig({
  base: './',
  publicDir: 'third_party',
  build: {
    outDir: path.resolve(outDir),
    emptyOutDir: true,
  },
});

import { defineConfig } from 'tsup';

export default defineConfig({
  entry: ['src/index.ts', 'bin/anyfs-nbd-proxy.ts'],
  format: ['esm'],
  target: 'node20',
  dts: true,
  clean: true,
  sourcemap: true,
});

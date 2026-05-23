import { defineConfig } from 'tsup';

export default defineConfig([
    {
        entry: ['src/index.ts', 'src/node.ts'],
        format: 'esm',
        dts: true,
        clean: true,
        sourcemap: true,
    },
    {
        // Worker entry shipped as a static asset alongside the wasm bundle.
        // Consumers load it via `new Worker(url, { type: 'module' })`.
        entry: { 'anyfs.worker': 'src/worker.ts' },
        format: 'esm',
        outDir: 'wasm',
        clean: false,
        sourcemap: true,
        outExtension: () => ({ js: '.js' }),
    },
]);

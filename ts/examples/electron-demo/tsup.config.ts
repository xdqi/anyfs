import { defineConfig } from 'tsup';

export default defineConfig({
    entry: {
        main: 'src/main.ts',
        preload: 'src/preload.ts',
    },
    format: ['cjs'],
    outExtension: () => ({ js: '.cjs' }),
    target: 'node20',
    platform: 'node',
    external: ['electron'],
    clean: true,
    sourcemap: true,
    splitting: false,
});

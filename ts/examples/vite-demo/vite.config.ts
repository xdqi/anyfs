import { defineConfig } from 'vite';
import type { Plugin } from 'vite';
import react from '@vitejs/plugin-react';
import { createHash } from 'crypto';
import { existsSync, mkdirSync, readFileSync, renameSync } from 'fs';
import path from 'path';

// COOP+COEP are required for SharedArrayBuffer (which -pthread wasm needs).
// `credentialless` lets us serve our own static assets without needing each
// one to carry a CORP header.
const crossOriginIsolated = {
    'Cross-Origin-Opener-Policy': 'same-origin',
    'Cross-Origin-Embedder-Policy': 'credentialless',
};

// The streaming-download SW is served from /assets/sw-download-<hash>.js but
// registers with scope:'/'. A browser only lets a SW claim a scope above its
// own path if the SW's HTTP response carries Service-Worker-Allowed: /. The
// prod Caddy server ships that header (public/Caddyfile @sw); `vite preview`
// and `vite dev` do not, so the SW registration throws a SecurityError there
// and downloads fail (FINDINGS F4). Applying the header to *every* response is
// harmless — browsers only read it on SW script responses — so just merge it
// into the dev/preview header set to match Caddy.
const devServerHeaders = {
    ...crossOriginIsolated,
    'Service-Worker-Allowed': '/',
};

// react-dnd@11 (transitive via chonky) calls ReactDOM.findDOMNode at render
// time. React 19 removed the export, so the file browser crashes the moment
// it mounts. Patch the pre-bundled react-dom CJS entry to re-add a fiber-walk
// implementation. Hook the esbuild pre-bundle step (dev) and the rollup
// build (prod).
const findDOMNodeShim = `
;(function patchFindDOMNode(){
    var __m = module.exports;
    if (!__m || typeof __m !== 'object') return;
    if (__m.findDOMNode) return;
    __m.findDOMNode = function findDOMNode(instance) {
        if (instance == null) return null;
        if (instance.nodeType) return instance;
        var fiber = instance._reactInternals || instance._reactInternalFiber;
        if (!fiber) return null;
        function walk(f) {
            if (f.stateNode && f.stateNode.nodeType) return f.stateNode;
            var c = f.child;
            while (c) { var r = walk(c); if (r) return r; c = c.sibling; }
            return null;
        }
        return walk(fiber);
    };
})();
`;

function reactDomFindDOMNodeEsbuildPlugin() {
    return {
        name: 'react-dom-findDOMNode-shim',
        setup(build: any) {
            build.onLoad({ filter: /node_modules\/react-dom\/index\.js$/ }, (args: any) => {
                const original = readFileSync(args.path, 'utf8');
                return { contents: original + findDOMNodeShim, loader: 'js' };
            });
        },
    };
}

// Same patch for the production rollup build. optimizeDeps' esbuild plugin
// only runs in dev; without this the chonky → react-dnd@11 → findDOMNode
// crash returns the moment you `vite build` and load the bundle.
function reactDomFindDOMNodeRollupPlugin() {
    return {
        name: 'react-dom-findDOMNode-shim-rollup',
        transform(code: string, id: string) {
            if (/node_modules\/react-dom\/index\.js$/.test(id)) {
                return { code: code + findDOMNodeShim, map: null };
            }
            return null;
        },
    };
}

// The wasm bundle (public/wasm/anyfs.{worker.js,mjs,wasm}) has fixed names
// and must be loaded as one matched set: an anyfs.worker.js from one build
// driving the anyfs.mjs/anyfs.wasm of another deadlocks the kernel boot. A
// CDN caches by extension — Cloudflare kept the old .js for hours while it
// passed the new .mjs/.wasm through — so a deploy served exactly that mix.
// The build moves the set into wasm/<content hash>/ and points the app at it
// (__ANYFS_WASM_DIR__). Everything inside resolves relative to that directory
// (the shim's `new URL("anyfs.mjs", import.meta.url)` pthread workers, the
// .wasm via locateFile), so no stale file can mix in. `vite dev` serves
// /wasm/ as-is.
const WASM_SET = ['anyfs.worker.js', 'anyfs.mjs', 'anyfs.wasm'];

function versionedWasmPlugin(): Plugin {
    let wasmDir = '/wasm/';
    let outDir = 'dist';
    return {
        name: 'anyfs-versioned-wasm',
        config(_config, { command }) {
            if (command === 'build') {
                const src = path.resolve(__dirname, 'public/wasm');
                const hash = createHash('sha256');
                for (const f of WASM_SET) {
                    const p = path.join(src, f);
                    if (!existsSync(p)) {
                        throw new Error(
                            `missing ${p}; run scripts/sync_wasm_bundle.sh before vite build`,
                        );
                    }
                    hash.update(readFileSync(p));
                }
                wasmDir = `/wasm/${hash.digest('hex').slice(0, 16)}/`;
            }
            return { define: { __ANYFS_WASM_DIR__: JSON.stringify(wasmDir) } };
        },
        configResolved(config) {
            outDir = path.resolve(config.root, config.build.outDir);
        },
        // Runs after Vite has copied public/ into outDir.
        closeBundle() {
            if (wasmDir === '/wasm/') return;
            const dst = path.join(outDir, wasmDir);
            mkdirSync(dst, { recursive: true });
            for (const f of [...WASM_SET, 'anyfs.worker.js.map']) {
                const p = path.join(outDir, 'wasm', f);
                if (existsSync(p)) renameSync(p, path.join(dst, f));
            }
        },
    };
}

export default defineConfig({
    plugins: [react(), reactDomFindDOMNodeRollupPlugin(), versionedWasmPlugin()],
    server: { headers: devServerHeaders },
    preview: {
        headers: devServerHeaders,
        allowedHosts: ['anyfs.kosaka.moe'],
    },
    optimizeDeps: {
        // The wasm shim does dynamic worker imports; let Vite leave it alone.
        exclude: ['@anyfs/core'],
        esbuildOptions: {
            plugins: [reactDomFindDOMNodeEsbuildPlugin()],
        },
    },
    worker: { format: 'es' },
});

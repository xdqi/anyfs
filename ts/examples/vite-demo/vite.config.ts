import { defineConfig } from 'vite';
import react from '@vitejs/plugin-react';
import { readFileSync } from 'fs';

// COOP+COEP are required for SharedArrayBuffer (which -pthread wasm needs).
// `credentialless` lets us serve our own static assets without needing each
// one to carry a CORP header.
const crossOriginIsolated = {
    'Cross-Origin-Opener-Policy': 'same-origin',
    'Cross-Origin-Embedder-Policy': 'credentialless',
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

export default defineConfig({
    plugins: [react(), reactDomFindDOMNodeRollupPlugin()],
    server: { headers: crossOriginIsolated },
    preview: {
        headers: crossOriginIsolated,
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

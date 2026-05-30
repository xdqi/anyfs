import type { SessionSource } from './types.js';
import { getAnyfsNative } from './native-session.js';
import type { AnyfsNativeBridge } from './native-session.js';

/** Capabilities passed to WasmSession at construction time. */
export interface WasmCaps {
    /** Set → URLFS rewrites cross-origin http(s) URLs via this prefix. */
    urlProxyPrefix?: string;
    /** Set → attachPath delegates to attachUrl(this URL, name).
     *  The factory pre-starts the main-process proxy (via startProxy IPC)
     *  and stores the ready-to-use loopback URL here. WasmSession does not
     *  own the proxy lifecycle — the caller tears it down on session close. */
    pathLoopbackUrl?: string;
}

export type SessionEnv = 'web' | 'electron' | 'node';

export type SessionBackend = 'native' | 'wasm' | 'node-wasm';

export interface DispatchResult {
    /** Which backend to use. The provider's prewarm step switches on this. */
    backend: SessionBackend;
    /** Which SessionSource.kind values are legal for this backend. */
    allowedKinds: Set<SessionSource['kind']>;
    /** Present when backend === 'native'. The preload-injected bridge
     *  (already available at provider mount time). */
    nativeBridge?: AnyfsNativeBridge;
    /** Present when backend === 'wasm'. Caps to forward to the worker
     *  and boot message. */
    wasmCaps?: WasmCaps;
}

/**
 * Pure factory: pick the right backend + declare which source kinds are
 * legal. Does NOT construct a session — construction happens during
 * prewarm (worker creation / addon init are async and heavyweight).
 *
 * Throws if the environment provides no usable backend.
 */
export function createSession(
    env: SessionEnv,
    opts?: {
        disableNative?: boolean;
        electronWasmCaps?: WasmCaps;
    },
): DispatchResult {
    if (env === 'web') {
        return {
            backend: 'wasm',
            wasmCaps: {},
            allowedKinds: new Set(['blob', 'url']),
        };
    }

    if (env === 'electron') {
        const nativeBridge = getAnyfsNative();
        if (nativeBridge && !opts?.disableNative) {
            return {
                backend: 'native',
                nativeBridge,
                allowedKinds: new Set(['path', 'url']),
            };
        }
        // Fall through to wasm. Under electron caps, path is legal (via
        // loopback proxy); blob is not (frontend resolves File→path before
        // producing source).
        const caps = opts?.electronWasmCaps ?? {};
        const kinds = new Set<SessionSource['kind']>(['url']);
        if (caps.pathLoopbackUrl) kinds.add('path');
        return { backend: 'wasm', wasmCaps: caps, allowedKinds: kinds };
    }

    // env === 'node'
    return {
        backend: 'node-wasm',
        allowedKinds: new Set(['path']),
    };
}

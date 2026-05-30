import { createContext, useContext, useEffect, useMemo, useRef, useState } from 'react';
import type { ReactNode } from 'react';
import {
    createSession,
    prewarm,
    prewarmNative,
    NativeSession,
    type AnyfsSession,
    type SessionSource,
    type SessionOpts,
    type WasmCaps,
    type SessionEnv,
    type SessionBackend,
    type DispatchResult,
} from '@anyfs/core';

export type AnyfsDiskStatus =
    | 'idle' // no source picked, no kernel running
    | 'booting' // wasm loading / kernel booting (prewarm in progress)
    | 'booted' // kernel ready, waiting for a source
    | 'attaching' // source selected, attaching disk image into the kernel
    | 'mounting' // running listParts / enter for auto-mount
    | 'ready' // disk + filesystem ready
    | 'error';

/** Re-exported from @anyfs/core — kept as a named type for API compat. */
export type AnyfsBackendMode = SessionBackend;

export interface AnyfsState {
    session: AnyfsSession | null;
    /** Live kernel handle, available as soon as boot completes — i.e. after
     *  prewarm but before any image is attached. Useful for kernel-only reads
     *  like `/proc/filesystems`. Equals `session` once an image is attached;
     *  null while booting or after close. */
    kernel: AnyfsSession | null;
    mountPath: string | null; // null until a partition / whole-disk is mounted
    status: AnyfsDiskStatus;
    /** Short human-readable label for the current sub-step ("booting kernel", etc). */
    step: string | null;
    error: Error | null;
    /** Which backend is in effect. Decided at provider mount based on
     *  `window.anyfsNative` presence; never flips during a session. */
    mode: AnyfsBackendMode;
}

const Ctx = createContext<AnyfsState | null>(null);

export interface AnyfsProviderProps {
    /** The image to mount. Pass `{kind:'blob',blob}` for a local Blob,
     *  `{kind:'url',url,name?}` for an HTTP image with Range support, or
     *  `{kind:'path',path}` for a host filesystem path (Electron only).
     *  Switching this prop (by referential identity) remounts. */
    source: SessionSource | null;
    /** URL of the worker script that hosts the wasm (`@anyfs/core/wasm/anyfs.worker.js`).
     *  Ignored in native mode. */
    workerUrl: string | URL;
    /** URL prefix where `anyfs.mjs` and `anyfs.wasm` live; default `/wasm/`.
     *  Ignored in native mode. */
    wasmBaseUrl?: string;
    /** Override the wasm shim filename (default `anyfs.mjs`). The bundle
     *  always includes the QEMU block layer. Ignored in native mode. */
    wasmModuleName?: string;
    /** Optional kernel options. */
    mountOpts?: SessionOpts;
    /** Start booting the kernel as soon as the provider mounts, even if
     *  `source` is null. In wasm mode that costs ~64 MB RAM + a worker;
     *  in native mode it's a cheap one-shot IPC `init` against the host
     *  kernel that's shared across the process. */
    prewarm?: boolean;
    /** Runtime environment. The factory uses this to select the backend.
     *  Default: 'web' (pure browser, blob + CORS URL only).
     *  'electron' detects the native bridge and disableNative flag. */
    env?: SessionEnv;
    /** When true, force wasm even if the native addon bridge is available
     *  (Electron only). */
    disableNative?: boolean;
    /** Caps passed to WasmSession when the electron wasm path is selected.
     *  Includes the pre-started loopback proxy URL for attachPath support. */
    electronWasmCaps?: WasmCaps;
    children: ReactNode;
}

export function AnyfsProvider({
    source,
    workerUrl,
    wasmBaseUrl,
    wasmModuleName,
    mountOpts,
    prewarm: doPrewarm,
    env = 'web',
    disableNative,
    electronWasmCaps,
    children,
}: AnyfsProviderProps) {
    // Decide backend at mount time via the factory. Never changes.
    const [dispatch] = useState<DispatchResult>(() => {
        const opts: Parameters<typeof createSession>[1] = {};
        if (disableNative !== undefined) opts.disableNative = disableNative;
        if (electronWasmCaps !== undefined) opts.electronWasmCaps = electronWasmCaps;
        return createSession(env, opts);
    });

    const [state, setState] = useState<AnyfsState>({
        session: null,
        kernel: null,
        mountPath: null,
        status: 'idle',
        step: null,
        error: null,
        mode: dispatch.backend,
    });
    // Survives StrictMode's double-effect.
    const desired = useRef<SessionSource | null>(null);
    // Pre-warmed session (kernel booted, no source attached yet). Null when no
    // prewarm requested, or after a source has been attached (then we move it
    // to `current`).
    const prewarmed = useRef<AnyfsSession | null>(null);
    const prewarming = useRef<Promise<AnyfsSession> | null>(null);
    const current = useRef<AnyfsSession | null>(null);
    const inflight = useRef<Promise<AnyfsSession> | null>(null);

    // Helper: start (or reuse) a prewarm. Exposed via ref so the source
    // effect below can reach into it.
    const startPrewarm = useRef<(() => Promise<AnyfsSession>) | null>(null);
    startPrewarm.current = () => {
        if (prewarmed.current) return Promise.resolve(prewarmed.current);
        if (prewarming.current) return prewarming.current;
        setState((s) =>
            s.status === 'ready' || s.status === 'attaching' || s.status === 'mounting'
                ? s
                : {
                      ...s,
                      status: 'booting',
                      step: 'starting worker',
                      error: null,
                  },
        );
        const p: Promise<AnyfsSession> = (() => {
            if (dispatch.backend === 'native') {
                return prewarmNative({
                    ...(mountOpts?.memMb !== undefined ? { memMb: mountOpts.memMb } : {}),
                    ...(mountOpts?.loglevel !== undefined ? { loglevel: mountOpts.loglevel } : {}),
                }).then((s) => {
                    if (!s) throw new Error('native bridge unavailable');
                    return s;
                });
            }
            // wasm (web or electron caps)
            const opts: Parameters<typeof prewarm>[0] = { workerUrl };
            if (wasmBaseUrl !== undefined) opts.wasmBaseUrl = wasmBaseUrl;
            if (wasmModuleName !== undefined) opts.wasmModuleName = wasmModuleName;
            if (mountOpts?.memMb !== undefined) opts.memMb = mountOpts.memMb;
            if (mountOpts?.loglevel !== undefined) opts.loglevel = mountOpts.loglevel;
            if (mountOpts?.forceFstype !== undefined) opts.forceFstype = mountOpts.forceFstype;
            // Forward urlProxyPrefix from caps so the worker applies the proxy
            if (dispatch.wasmCaps?.urlProxyPrefix) {
                (opts as unknown as Record<string, unknown>).urlProxyPrefix =
                    dispatch.wasmCaps.urlProxyPrefix;
            }
            return prewarm(opts).then((session) => {
                const off = session.onProgress((step) => {
                    setState((s) =>
                        s.status === 'booting' ||
                        s.status === 'attaching' ||
                        s.status === 'mounting'
                            ? { ...s, step }
                            : s,
                    );
                });
                void off;
                return session;
            });
        })();
        prewarming.current = p;
        p.then(
            (session) => {
                prewarmed.current = session;
                prewarming.current = null;
                setState((s) =>
                    s.status === 'booting'
                        ? {
                              ...s,
                              kernel: session,
                              status: 'booted',
                              step: 'kernel ready',
                          }
                        : { ...s, kernel: s.kernel ?? session },
                );
            },
            (err) => {
                prewarming.current = null;
                setState({
                    session: null,
                    kernel: null,
                    mountPath: null,
                    status: 'error',
                    step: null,
                    error: err instanceof Error ? err : new Error(String(err)),
                    mode: dispatch.backend,
                });
            },
        );
        return p;
    };

    // Kick off prewarm on provider mount if requested and not already
    // attaching a source.
    useEffect(() => {
        if (!doPrewarm) return;
        if (prewarmed.current || prewarming.current || current.current) return;
        startPrewarm.current?.();
    }, [doPrewarm]);

    useEffect(() => {
        if (!source) {
            desired.current = null;
            const stale = current.current;
            current.current = null;
            inflight.current = null;
            if (stale) void stale.close();
            // If prewarm is on, stay in booted/booting; otherwise idle.
            setState((s) => {
                if (s.status === 'booted' || s.status === 'booting' || s.status === 'error') {
                    return { ...s, session: null, mountPath: null };
                }
                return {
                    session: null,
                    kernel: null,
                    mountPath: null,
                    status: 'idle',
                    step: null,
                    error: null,
                    mode: dispatch.backend,
                };
            });
            return;
        }
        if (desired.current === source && (current.current || inflight.current)) {
            return;
        }
        desired.current = source;
        console.log(
            '[PROVIDER] source effect firing',
            source.kind,
            source.kind === 'url' ? (source as any).url : '',
        );

        const stale = current.current;
        current.current = null;
        if (stale) void stale.close();

        setState((s) => ({
            ...s,
            session: null,
            mountPath: null,
            status: 'attaching',
            step: 'preparing',
            error: null,
        }));

        const p = (async () => {
            console.log('[PROVIDER] calling startPrewarm...');
            const d = startPrewarm.current?.();
            console.log('[PROVIDER] startPrewarm returned', d ? 'promise' : 'null');
            const session = await (d ?? Promise.reject(new Error('no prewarm slot')));
            console.log('[PROVIDER] got session, calling attach...');
            if (!dispatch.allowedKinds.has(source.kind)) {
                throw new Error(
                    `source kind "${source.kind}" is not supported by the ${dispatch.backend} backend`,
                );
            }
            if (source.kind === 'blob') {
                if (session instanceof NativeSession) {
                    throw new Error(
                        'native backend cannot mount a File object — use {kind:"path"} ' +
                            'with an absolute host path instead',
                    );
                }
                await session.attachBlob(source.blob);
            } else if (source.kind === 'url') {
                console.log('[PROVIDER] calling session.attachUrl', source.url, source.name);
                await session.attachUrl(source.url, source.name);
                console.log('[PROVIDER] session.attachUrl returned');
            } else {
                // kind:'path' — native only.
                if (!(session instanceof NativeSession)) {
                    throw new Error(
                        'host paths can only be opened in native mode (Electron); ' +
                            'use {kind:"blob"} or {kind:"url"} in the browser',
                    );
                }
                await session.attachPath(source.path);
            }
            return session;
        })();
        inflight.current = p;

        (async () => {
            try {
                const session = await p;
                if (desired.current !== source) {
                    await session.close();
                    return;
                }
                current.current = session;
                // The prewarmed slot has been consumed by this attach; clear
                // it so a future remount creates a fresh session.
                prewarmed.current = null;
                const mountPath: string | null = null;
                if (desired.current !== source) {
                    current.current = null;
                    await session.close();
                    return;
                }
                setState({
                    session,
                    kernel: session,
                    mountPath,
                    status: 'ready',
                    step: null,
                    error: null,
                    mode: dispatch.backend,
                });
            } catch (err) {
                console.log(
                    '[PROVIDER] error in source effect:',
                    err instanceof Error ? err.message : String(err),
                );
                if (desired.current !== source) return;
                setState({
                    session: null,
                    kernel: null,
                    mountPath: null,
                    status: 'error',
                    step: null,
                    error: err instanceof Error ? err : new Error(String(err)),
                    mode: dispatch.backend,
                });
            }
        })();
    }, [source, workerUrl, wasmBaseUrl, wasmModuleName, mountOpts, dispatch.backend]);

    // Unmount-time cleanup.
    useEffect(() => {
        return () => {
            const stale = current.current ?? prewarmed.current;
            current.current = null;
            prewarmed.current = null;
            desired.current = null;
            if (stale) void stale.close();
        };
    }, []);

    const value = useMemo(() => state, [state]);
    return <Ctx.Provider value={value}>{children}</Ctx.Provider>;
}

export function useAnyfsDisk(): AnyfsState {
    const v = useContext(Ctx);
    if (!v) throw new Error('useAnyfsDisk: missing <AnyfsProvider>');
    return v;
}

/** Same as useAnyfsDisk but returns null when no <AnyfsProvider> above. */
export function useAnyfsDiskMaybe(): AnyfsState | null {
    return useContext(Ctx);
}

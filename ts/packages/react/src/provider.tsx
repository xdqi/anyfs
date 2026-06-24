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

/** Default attach-phase watchdog (ms). Generous enough for the metadata phase of
 *  a multi-GB container over a slow link, but bounded so a truly hung/half-open
 *  source can't spin the UI forever. Override via mountOpts.attachTimeoutMs. */
const DEFAULT_ATTACH_TIMEOUT_MS = 120_000;

/** Reject if `p` hasn't settled within `ms`. Does not cancel `p` itself — the
 *  caller closes the session on timeout, which force-terminates the worker. */
function withTimeout<T>(p: Promise<T>, ms: number): Promise<T> {
    return new Promise<T>((resolve, reject) => {
        const t = setTimeout(() => {
            reject(
                new Error(
                    `Opening the image timed out after ${Math.round(ms / 1000)}s — the source may ` +
                        'be slow, unreachable, or not support Range requests.',
                ),
            );
        }, ms);
        p.then(
            (v) => {
                clearTimeout(t);
                resolve(v);
            },
            (e) => {
                clearTimeout(t);
                reject(e);
            },
        );
    });
}

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

    // The source whose attach is currently desired. `gen` is bumped on every
    // source change AND on close; an in-flight attach captures its gen and bails
    // (closing the session it owns) when a newer change superseded it — so an
    // abandoned/superseded attach never commits and never strands a disposed
    // session in a ref. This is the fix for the open→close-mid-load→reopen and
    // rapid-switch "already disposed" / stuck-attaching wedges.
    const desired = useRef<SessionSource | null>(null);
    const current = useRef<AnyfsSession | null>(null);
    // A booted-but-unattached worker, parked for the next open. Claimed
    // EXCLUSIVELY by consumeSession() (set back to null on claim) so two
    // overlapping opens can never attach onto the same worker.
    const prewarmed = useRef<AnyfsSession | null>(null);
    const prewarming = useRef<Promise<AnyfsSession> | null>(null);
    const gen = useRef(0);
    // The source whose attach is currently in flight (vs. `current`, which is
    // the source already attached). Lets the dedup guard early-return on a
    // re-render that re-fires the effect with the SAME source while its attach
    // is still running (e.g. an inline `mountOpts={{…}}` dep changing). Cleared
    // on settle so a settled source can't masquerade as in-progress (F16-26).
    const inflightSrc = useRef<SessionSource | null>(null);

    // Boot a fresh kernel/worker with no ref bookkeeping. Wires progress so the
    // UI step text updates while booting/attaching/mounting. Reassigned each
    // render via the ref pattern so it always sees the latest props.
    const bootSession = useRef<() => Promise<AnyfsSession>>(() =>
        Promise.reject(new Error('boot')),
    );
    bootSession.current = () => {
        if (dispatch.backend === 'native') {
            return prewarmNative({
                ...(mountOpts?.memMb !== undefined ? { memMb: mountOpts.memMb } : {}),
                ...(mountOpts?.loglevel !== undefined ? { loglevel: mountOpts.loglevel } : {}),
            }).then((s) => {
                if (!s) throw new Error('native bridge unavailable');
                return s;
            });
        }
        const opts: Parameters<typeof prewarm>[0] = { workerUrl };
        if (wasmBaseUrl !== undefined) opts.wasmBaseUrl = wasmBaseUrl;
        if (wasmModuleName !== undefined) opts.wasmModuleName = wasmModuleName;
        if (mountOpts?.memMb !== undefined) opts.memMb = mountOpts.memMb;
        if (mountOpts?.loglevel !== undefined) opts.loglevel = mountOpts.loglevel;
        if (mountOpts?.forceFstype !== undefined) opts.forceFstype = mountOpts.forceFstype;
        if (dispatch.wasmCaps?.urlProxyPrefix) {
            (opts as unknown as Record<string, unknown>).urlProxyPrefix =
                dispatch.wasmCaps.urlProxyPrefix;
        }
        return prewarm(opts).then((session) => {
            session.onProgress((step) => {
                setState((s) =>
                    s.status === 'booting' || s.status === 'attaching' || s.status === 'mounting'
                        ? { ...s, step }
                        : s,
                );
            });
            return session;
        });
    };

    // Park a prewarmed worker for the next open (idempotent). Leaves the session
    // in `prewarmed` for a later consumeSession() to claim. Never consumes.
    const startPark = useRef<() => void>(() => {});
    startPark.current = () => {
        if (prewarmed.current || prewarming.current) return;
        setState((s) =>
            s.status === 'ready' || s.status === 'attaching' || s.status === 'mounting'
                ? s
                : { ...s, status: 'booting', step: 'starting worker', error: null },
        );
        const p = bootSession.current();
        prewarming.current = p;
        p.then(
            (session) => {
                // If a consumer claimed this boot meanwhile (cleared prewarming),
                // it now owns the session — don't park it.
                if (prewarming.current !== p) return;
                prewarming.current = null;
                prewarmed.current = session;
                setState((s) =>
                    s.status === 'booting'
                        ? { ...s, kernel: session, status: 'booted', step: 'kernel ready' }
                        : { ...s, kernel: s.kernel ?? session },
                );
            },
            (err) => {
                if (prewarming.current === p) prewarming.current = null;
                // Only escalate to error if the UI is still waiting on THIS boot.
                // A late rejection after the user closed/navigated away must not
                // clobber an idle/closed UI into a spurious error (finding F16-18).
                setState((s) =>
                    s.status === 'booting'
                        ? {
                              ...s,
                              session: null,
                              kernel: null,
                              mountPath: null,
                              status: 'error',
                              step: null,
                              error: err instanceof Error ? err : new Error(String(err)),
                          }
                        : s,
                );
            },
        );
    };

    // Get a session to attach to, EXCLUSIVELY. Claims the parked prewarm (or its
    // in-flight boot) if present — clearing the ref so no other open can grab the
    // same worker — otherwise boots a fresh one.
    const consumeSession = (): Promise<AnyfsSession> => {
        if (prewarmed.current) {
            const s = prewarmed.current;
            prewarmed.current = null;
            return Promise.resolve(s);
        }
        if (prewarming.current) {
            const p = prewarming.current;
            prewarming.current = null; // claim; startPark's .then sees the mismatch and won't park
            return p;
        }
        return bootSession.current();
    };

    // Kick off prewarm on provider mount if requested and not already busy.
    useEffect(() => {
        if (!doPrewarm) return;
        if (prewarmed.current || prewarming.current || current.current) return;
        startPark.current();
    }, [doPrewarm]);

    useEffect(() => {
        if (!source) {
            // Bump gen so any in-flight attach is superseded and bails.
            ++gen.current;
            desired.current = null;
            inflightSrc.current = null;
            const stale = current.current;
            current.current = null;
            if (stale) void stale.close();
            setState((s) => {
                // Keep a live parked kernel visible as 'booted'; otherwise idle.
                // Always clear any prior error so Close recovers from 'error'
                // instead of stranding the stale message (finding F16-19).
                if (prewarmed.current || prewarming.current || s.status === 'booting') {
                    return {
                        ...s,
                        session: null,
                        mountPath: null,
                        status: s.status === 'booting' ? 'booting' : 'booted',
                        step: s.status === 'booting' ? s.step : 'kernel ready',
                        error: null,
                    };
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

        // Already attached, or already attaching, this exact source object.
        // Dedup BEFORE bumping `gen` — otherwise a spurious effect re-fire (e.g.
        // an inline `mountOpts={{…}}` dep whose identity changed on a parent
        // re-render) would bump the generation and wrongly supersede the
        // legitimate in-flight attach, stranding it in 'attaching' forever.
        if (desired.current === source && (current.current || inflightSrc.current === source))
            return;

        const myGen = ++gen.current;
        const superseded = () => gen.current !== myGen;
        desired.current = source;
        inflightSrc.current = source;

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

        const attachTimeoutMs = mountOpts?.attachTimeoutMs ?? DEFAULT_ATTACH_TIMEOUT_MS;
        const src = source;

        void (async () => {
            let session: AnyfsSession | null = null;
            try {
                session = await consumeSession();
                if (superseded()) {
                    await session.close();
                    return;
                }
                if (!dispatch.allowedKinds.has(src.kind)) {
                    throw new Error(
                        `source kind "${src.kind}" is not supported by the ${dispatch.backend} backend`,
                    );
                }
                const sess = session;
                const attach = (async () => {
                    if (src.kind === 'blob') {
                        if (sess instanceof NativeSession) {
                            throw new Error(
                                'native backend cannot mount a File object — use {kind:"path"} ' +
                                    'with an absolute host path instead',
                            );
                        }
                        await sess.attachBlob(src.blob);
                    } else if (src.kind === 'url') {
                        await sess.attachUrl(src.url, src.name);
                    } else {
                        if (!(sess instanceof NativeSession)) {
                            throw new Error(
                                'host paths can only be opened in native mode (Electron); ' +
                                    'use {kind:"blob"} or {kind:"url"} in the browser',
                            );
                        }
                        await sess.attachPath(src.path);
                    }
                })();
                await (attachTimeoutMs > 0 ? withTimeout(attach, attachTimeoutMs) : attach);

                if (superseded()) {
                    await session.close();
                    return;
                }
                current.current = session;
                if (inflightSrc.current === src) inflightSrc.current = null;
                // This attach consumed the parked worker (if any).
                prewarmed.current = null;
                // A post-ready worker abort/host-error bricks the session — flip
                // the UI to error instead of believing the disk is still fine
                // (finding F16-04).
                const live = session;
                live.onFatal((err) => {
                    if (current.current !== live) return;
                    current.current = null;
                    void live.close();
                    setState((s) => ({
                        ...s,
                        session: null,
                        kernel: null,
                        mountPath: null,
                        status: 'error',
                        step: null,
                        error: err,
                    }));
                });
                setState({
                    session,
                    kernel: session,
                    mountPath: null,
                    status: 'ready',
                    step: null,
                    error: null,
                    mode: dispatch.backend,
                });
            } catch (err) {
                // Drop the worker on ANY failure — never reuse a half-attached or
                // wedged worker. close() force-terminates it (findings F16-02/03/06).
                if (session) {
                    try {
                        await session.close();
                    } catch {
                        /* best effort */
                    }
                }
                if (inflightSrc.current === src) inflightSrc.current = null;
                if (superseded()) return;
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
            ++gen.current;
            const stale = current.current ?? prewarmed.current;
            current.current = null;
            prewarmed.current = null;
            desired.current = null;
            if (stale) void stale.close();
            // A boot still in flight at unmount: close the session once it
            // settles so it can't leak past the provider's lifetime (F16-27).
            const booting = prewarming.current;
            prewarming.current = null;
            if (booting) void booting.then((s) => s.close()).catch(() => {});
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

import { createContext, useContext, useEffect, useMemo, useRef, useState } from 'react';
import type { ReactNode } from 'react';
import {
    getAnyfsNative,
    prewarm,
    prewarmNative,
    NativeAnyfsDisk,
    type AnyfsDisk,
    type DiskSource,
    type MountOpts,
} from '@anyfs/core';

export type AnyfsDiskStatus =
    | 'idle' // no file picked, no kernel running
    | 'booting' // wasm loading / kernel booting (prewarm in progress)
    | 'booted' // kernel ready, waiting for a file
    | 'attaching' // file selected, mounting disk image into the kernel
    | 'mounting' // running listPartitions / mountWhole for auto-mount
    | 'ready' // disk + filesystem ready
    | 'error';

/** Which backend the provider is talking to. `native` means the Electron
 *  preload-injected `window.anyfsNative` bridge is present and we've routed
 *  everything through it; `wasm` is the in-renderer worker. Renderer UI uses
 *  this to gate which input affordances to show (File picker vs path dialog,
 *  URL through native curl vs URLFS, system-drive picker only in native). */
export type AnyfsBackendMode = 'native' | 'wasm';

/** A disk-like object whose surface is broad enough for either backend. The
 *  worker's `WorkerAnyfsDisk` and the native `NativeAnyfsDisk` both satisfy
 *  this; we widen to `AnyfsDisk` here so the few extra native-only methods
 *  (`attachPath`) live behind an instanceof check in the provider itself. */
type AnyDisk = AnyfsDisk | NativeAnyfsDisk;

export interface AnyfsState {
    disk: AnyDisk | null;
    /** Live kernel handle, available as soon as boot completes — i.e. after
     *  prewarm but before any image is attached. Useful for kernel-only reads
     *  like `/proc/filesystems`. Equals `disk` once an image is attached;
     *  null while booting or after dispose. */
    kernel: AnyDisk | null;
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
    /** The image to mount. Pass `{kind:'file',file}` for a local Blob,
     *  `{kind:'url',url,name?}` for an HTTP image with Range support, or
     *  `{kind:'path',path}` for a host filesystem path (Electron only).
     *  Switching this prop (by referential identity) remounts. */
    source: DiskSource | null;
    /** URL of the worker script that hosts the wasm (`@anyfs/core/wasm/anyfs.worker.js`).
     *  Ignored in native mode. */
    workerUrl: string | URL;
    /** URL prefix where `anyfs.mjs` and `anyfs.wasm` live; default `/wasm/`.
     *  Ignored in native mode. */
    wasmBaseUrl?: string;
    /** Override the wasm shim filename (default `anyfs.mjs`). Set to
     *  `anyfs.qemu.mjs` for the QEMU-libblock bundle. Ignored in native mode. */
    wasmModuleName?: string;
    /** Optional kernel options. */
    mountOpts?: MountOpts;
    /** If set and the image is no-PT, auto-mount the whole disk under this fstype. */
    autoMountFstype?: string | undefined;
    /** Start booting the kernel as soon as the provider mounts, even if
     *  `source` is null. In wasm mode that costs ~64 MB RAM + a worker;
     *  in native mode it's a cheap one-shot IPC `init` against the host
     *  kernel that's shared across the process. */
    prewarm?: boolean;
    /** Force a backend regardless of capability. Useful for development /
     *  testing the wasm path in an Electron build. Default: prefer native
     *  when `window.anyfsNative` is present, else wasm. */
    forceMode?: AnyfsBackendMode;
    children: ReactNode;
}

export function AnyfsProvider({
    source,
    workerUrl,
    wasmBaseUrl,
    wasmModuleName,
    mountOpts,
    autoMountFstype,
    prewarm: doPrewarm,
    forceMode,
    children,
}: AnyfsProviderProps) {
    // Decide backend at mount time. `forceMode` wins; otherwise probe.
    const [mode] = useState<AnyfsBackendMode>(() => {
        if (forceMode) return forceMode;
        return getAnyfsNative() ? 'native' : 'wasm';
    });

    const [state, setState] = useState<AnyfsState>({
        disk: null,
        kernel: null,
        mountPath: null,
        status: 'idle',
        step: null,
        error: null,
        mode,
    });
    // Survives StrictMode's double-effect.
    const desired = useRef<DiskSource | null>(null);
    // Pre-warmed disk (kernel booted, no source attached yet). Null when no
    // prewarm requested, or after a source has been attached (then we move it
    // to `current`).
    const prewarmed = useRef<AnyDisk | null>(null);
    const prewarming = useRef<Promise<AnyDisk> | null>(null);
    const current = useRef<AnyDisk | null>(null);
    const inflight = useRef<Promise<AnyDisk> | null>(null);

    // Helper: start (or reuse) a prewarm. Exposed via ref so the source
    // effect below can reach into it.
    const startPrewarm = useRef<(() => Promise<AnyDisk>) | null>(null);
    startPrewarm.current = () => {
        if (prewarmed.current) return Promise.resolve(prewarmed.current);
        if (prewarming.current) return prewarming.current;
        setState((s) =>
            s.status === 'ready' || s.status === 'attaching' || s.status === 'mounting'
                ? s
                : { ...s, status: 'booting', step: 'starting worker', error: null },
        );
        const p: Promise<AnyDisk> =
            mode === 'native'
                ? prewarmNative({
                      ...(mountOpts?.memMb !== undefined ? { memMb: mountOpts.memMb } : {}),
                      ...(mountOpts?.loglevel !== undefined
                          ? { loglevel: mountOpts.loglevel }
                          : {}),
                  }).then((d) => {
                      if (!d) throw new Error('native bridge unavailable');
                      return d;
                  })
                : (() => {
                      const opts: Parameters<typeof prewarm>[0] = { workerUrl };
                      if (wasmBaseUrl !== undefined) opts.wasmBaseUrl = wasmBaseUrl;
                      if (wasmModuleName !== undefined) opts.wasmModuleName = wasmModuleName;
                      if (mountOpts?.memMb !== undefined) opts.memMb = mountOpts.memMb;
                      if (mountOpts?.loglevel !== undefined) opts.loglevel = mountOpts.loglevel;
                      if (mountOpts?.forceFstype !== undefined)
                          opts.forceFstype = mountOpts.forceFstype;
                      return prewarm(opts).then((disk) => {
                          const off = disk.onProgress((step) => {
                              setState((s) =>
                                  s.status === 'booting' ||
                                  s.status === 'attaching' ||
                                  s.status === 'mounting'
                                      ? { ...s, step }
                                      : s,
                              );
                          });
                          // Don't unsubscribe immediately — the same disk handle keeps
                          // emitting progress events during attach. Bound to disk lifetime
                          // via dispose-on-unmount below; the listener leak is negligible.
                          void off;
                          return disk;
                      });
                  })();
        prewarming.current = p;
        p.then(
            (disk) => {
                prewarmed.current = disk;
                prewarming.current = null;
                setState((s) =>
                    s.status === 'booting'
                        ? { ...s, kernel: disk, status: 'booted', step: 'kernel ready' }
                        : { ...s, kernel: s.kernel ?? disk },
                );
            },
            (err) => {
                prewarming.current = null;
                setState({
                    disk: null,
                    kernel: null,
                    mountPath: null,
                    status: 'error',
                    step: null,
                    error: err instanceof Error ? err : new Error(String(err)),
                    mode,
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
            if (stale) void stale.dispose();
            // If prewarm is on, stay in booted/booting; otherwise idle.
            setState((s) => {
                if (s.status === 'booted' || s.status === 'booting' || s.status === 'error') {
                    return { ...s, disk: null, mountPath: null };
                }
                return {
                    disk: null,
                    kernel: null,
                    mountPath: null,
                    status: 'idle',
                    step: null,
                    error: null,
                    mode,
                };
            });
            return;
        }
        if (desired.current === source && (current.current || inflight.current)) {
            return;
        }
        desired.current = source;

        const stale = current.current;
        current.current = null;
        if (stale) void stale.dispose();

        setState((s) => ({
            ...s,
            disk: null,
            mountPath: null,
            status: 'attaching',
            step: 'preparing',
            error: null,
        }));

        const p = (async () => {
            const disk = await (startPrewarm.current?.() ??
                Promise.reject(new Error('no prewarm slot')));
            if (source.kind === 'file') {
                if (disk instanceof NativeAnyfsDisk) {
                    throw new Error(
                        'native backend cannot mount a File object — use {kind:"path"} ' +
                            'with an absolute host path instead',
                    );
                }
                await disk.attach(source.file);
            } else if (source.kind === 'url') {
                if (disk instanceof NativeAnyfsDisk) {
                    await disk.attachUrl(source.url, source.name);
                } else {
                    await disk.attachUrl(source.url, source.name);
                }
            } else {
                // kind:'path' — native only.
                if (!(disk instanceof NativeAnyfsDisk)) {
                    throw new Error(
                        'host paths can only be opened in native mode (Electron); ' +
                            'use {kind:"file"} or {kind:"url"} in the browser',
                    );
                }
                await disk.attachPath(source.path);
            }
            return disk;
        })();
        inflight.current = p;

        (async () => {
            try {
                const disk = await p;
                if (desired.current !== source) {
                    await disk.dispose();
                    return;
                }
                current.current = disk;
                // The prewarmed slot has been consumed by this attach; clear
                // it so a future remount creates a fresh worker.
                prewarmed.current = null;
                let mountPath: string | null = null;
                if (autoMountFstype) {
                    setState((s) => ({ ...s, status: 'mounting', step: 'reading partitions' }));
                    const parts = await disk.listPartitions();
                    if (parts.length === 0) {
                        setState((s) => ({ ...s, step: `mounting ${autoMountFstype}` }));
                        mountPath = await disk.mountWhole(autoMountFstype);
                    }
                }
                if (desired.current !== source) {
                    current.current = null;
                    await disk.dispose();
                    return;
                }
                setState({
                    disk,
                    kernel: disk,
                    mountPath,
                    status: 'ready',
                    step: null,
                    error: null,
                    mode,
                });
            } catch (err) {
                if (desired.current !== source) return;
                setState({
                    disk: null,
                    kernel: null,
                    mountPath: null,
                    status: 'error',
                    step: null,
                    error: err instanceof Error ? err : new Error(String(err)),
                    mode,
                });
            }
        })();
    }, [source, workerUrl, wasmBaseUrl, wasmModuleName, mountOpts, autoMountFstype, mode]);

    // Unmount-time cleanup.
    useEffect(() => {
        return () => {
            const stale = current.current ?? prewarmed.current;
            current.current = null;
            prewarmed.current = null;
            desired.current = null;
            if (stale) void stale.dispose();
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

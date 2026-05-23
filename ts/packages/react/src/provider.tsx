import { createContext, useContext, useEffect, useMemo, useRef, useState } from 'react';
import type { ReactNode } from 'react';
import { prewarm, type AnyfsDisk, type DiskSource, type MountOpts } from '@anyfs/core';

export type AnyfsDiskStatus =
    | 'idle' // no file picked, no kernel running
    | 'booting' // wasm loading / kernel booting (prewarm in progress)
    | 'booted' // kernel ready, waiting for a file
    | 'attaching' // file selected, mounting disk image into the kernel
    | 'mounting' // running listPartitions / mountWhole for auto-mount
    | 'ready' // disk + filesystem ready
    | 'error';

export interface AnyfsState {
    disk: AnyfsDisk | null;
    /** Live kernel handle, available as soon as boot completes — i.e. after
     *  prewarm but before any image is attached. Useful for kernel-only reads
     *  like `/proc/filesystems`. Equals `disk` once an image is attached;
     *  null while booting or after dispose. */
    kernel: AnyfsDisk | null;
    mountPath: string | null; // null until a partition / whole-disk is mounted
    status: AnyfsDiskStatus;
    /** Short human-readable label for the current sub-step ("booting kernel", etc). */
    step: string | null;
    error: Error | null;
}

const Ctx = createContext<AnyfsState | null>(null);

export interface AnyfsProviderProps {
    /** The image to mount. Pass `{kind:'file',file}` for a local Blob or
     *  `{kind:'url',url,name?}` for an HTTP image served with Range support.
     *  Switching this prop (by referential identity) remounts. */
    source: DiskSource | null;
    /** URL of the worker script that hosts the wasm (`@anyfs/core/wasm/anyfs.worker.js`). */
    workerUrl: string | URL;
    /** URL prefix where `anyfs.mjs` and `anyfs.wasm` live; default `/wasm/`. */
    wasmBaseUrl?: string;
    /** Override the wasm shim filename (default `anyfs.mjs`). Set to
     *  `anyfs.qemu.mjs` for the QEMU-libblock bundle. */
    wasmModuleName?: string;
    /** Optional kernel options. */
    mountOpts?: MountOpts;
    /** If set and the image is no-PT, auto-mount the whole disk under this fstype. */
    autoMountFstype?: string | undefined;
    /** Start booting the wasm kernel as soon as the provider mounts, even if
     *  `file` is null. Lets you hide latency behind the landing page; costs
     *  ~64 MB RAM + a worker even if the user never uploads. */
    prewarm?: boolean;
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
    children,
}: AnyfsProviderProps) {
    const [state, setState] = useState<AnyfsState>({
        disk: null,
        kernel: null,
        mountPath: null,
        status: 'idle',
        step: null,
        error: null,
    });
    // Survives StrictMode's double-effect.
    const desired = useRef<DiskSource | null>(null);
    // Pre-warmed disk (kernel booted, no file attached yet). Null when no
    // prewarm requested, or after a file has been attached (then we move it
    // to `current`).
    const prewarmed = useRef<AnyfsDisk | null>(null);
    const prewarming = useRef<Promise<AnyfsDisk> | null>(null);
    const current = useRef<AnyfsDisk | null>(null);
    const inflight = useRef<Promise<AnyfsDisk> | null>(null);

    // Helper: start (or reuse) a prewarm. Exposed via ref so the file effect
    // below can reach into it.
    const startPrewarm = useRef<(() => Promise<AnyfsDisk>) | null>(null);
    startPrewarm.current = () => {
        if (prewarmed.current) return Promise.resolve(prewarmed.current);
        if (prewarming.current) return prewarming.current;
        setState((s) =>
            s.status === 'ready' || s.status === 'attaching' || s.status === 'mounting'
                ? s
                : { ...s, status: 'booting', step: 'starting worker', error: null },
        );
        const opts: Parameters<typeof prewarm>[0] = { workerUrl };
        if (wasmBaseUrl !== undefined) opts.wasmBaseUrl = wasmBaseUrl;
        if (wasmModuleName !== undefined) opts.wasmModuleName = wasmModuleName;
        if (mountOpts?.memMb !== undefined) opts.memMb = mountOpts.memMb;
        if (mountOpts?.loglevel !== undefined) opts.loglevel = mountOpts.loglevel;
        if (mountOpts?.forceFstype !== undefined) opts.forceFstype = mountOpts.forceFstype;
        const p = prewarm(opts).then((disk) => {
            const off = disk.onProgress((step) => {
                setState((s) =>
                    s.status === 'booting' || s.status === 'attaching' || s.status === 'mounting'
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
                });
            },
        );
        return p;
    };

    // Kick off prewarm on provider mount if requested and not already
    // attaching a file.
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
            if (source.kind === 'file') await disk.attach(source.file);
            else await disk.attachUrl(source.url, source.name);
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
                });
            }
        })();
    }, [source, workerUrl, wasmBaseUrl, wasmModuleName, mountOpts, autoMountFstype]);

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

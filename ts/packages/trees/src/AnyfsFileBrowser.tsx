import { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import type { AnyfsDisk, Stat } from '@anyfs/core';
import { useAnyfsDiskMaybe } from '@anyfs/react';
import {
    ChonkyActions,
    ChonkyIconName,
    FileBrowser,
    FileContextMenu,
    FileList,
    FileNavbar,
    FileToolbar,
    defineFileAction,
    setChonkyDefaults,
} from 'chonky';
import type { ChonkyFileActionData, FileArray, FileData } from 'chonky';
import { ChonkyIconFA } from 'chonky-icon-fontawesome';

// Right-click "Properties" — exposes the full Stat dump for a single row.
// requiresSelection so Chonky greys it out unless a file is highlighted.
const ShowPropertiesAction = defineFileAction({
    id: 'show_properties',
    requiresSelection: true,
    button: {
        name: 'Properties',
        contextMenu: true,
        toolbar: false,
        group: 'Info',
        icon: ChonkyIconName.info,
    },
} as const);

// Chonky requires one global init; HMR re-imports this module so guard it.
declare global {
    interface Window {
        __chonkyInitDone?: boolean;
    }
}
if (typeof window !== 'undefined' && !window.__chonkyInitDone) {
    setChonkyDefaults({ iconComponent: ChonkyIconFA });
    window.__chonkyInitDone = true;
}

export interface AnyfsFileBrowserProps {
    /** Override the disk; otherwise read from <AnyfsProvider>. */
    disk?: AnyfsDisk;
    /** Override mount point; otherwise from provider. */
    mountPath?: string;
    /**
     * When true (default), opening a symlink-to-directory resolves through
     * `disk.realpath` and the breadcrumb shows the canonical target (e.g.
     * `/bin` → `/usr/bin`). When false, the breadcrumb stays at the literal
     * link path.
     */
    followSymlinks?: boolean;
    /**
     * When true, Chonky renders with its dark-mode palette. When omitted,
     * Chonky uses its built-in default (light). Pass the resolved (already
     * boolean) value — embedders typically derive this from their own
     * theme setting.
     */
    darkMode?: boolean;
    className?: string;
    /**
     * Fired when the user opens a non-directory file. `relPath` is the path
     * inside the mount (no leading slash, no mount prefix). `mountPath` lets
     * callers resolve to an absolute LKL path when needed.
     */
    onFileActivate?: (info: { relPath: string; mountPath: string }) => void;
    /**
     * Label rendered as the breadcrumb's root crumb. Embedders should pass
     * something user-facing (e.g. "whole disk", "partition #2") rather than
     * leaking the underlying LKL mount path. Defaults to "/".
     */
    rootLabel?: string;
    /**
     * Optional extra crumb rendered *before* the root crumb (e.g. the disk
     * image filename). Clicking it fires `onClick` — the embedder decides
     * what that means (close the disk, return to the partition picker,
     * etc.). When omitted, the chain starts at the root crumb.
     */
    superCrumb?: { label: string; onClick: () => void };
}

function joinAbs(mountPath: string, relPath: string): string {
    const base = mountPath.endsWith('/') ? mountPath.slice(0, -1) : mountPath;
    if (!relPath) return base;
    return `${base}/${relPath}`;
}

function rowIdFor(relPath: string, name: string): string {
    return relPath ? `${relPath}/${name}` : name;
}

interface PropsTarget {
    name: string;
    relPath: string;
    absPath: string;
    /** Raw lstat result — never follows symlinks. */
    stat: Stat;
    /** For symlinks, the verbatim target stored in the link inode. null otherwise. */
    linkTarget: string | null;
}

// Chonky's built-in extension splitter (`_extname`) is buggy: for a name with
// no dot it returns `"." + name`, which renders as ".name" with an empty
// title. Work around by computing `ext` ourselves and passing it on FileData
// — Chonky honors a non-null `file.ext` and skips its own splitter.
//
// Rules:
//   - no dot → no extension (`""`)
//   - leading dot (dotfile like `.pwd.lock`) → only split on a *later* dot
//   - trailing dot → no extension
function splitExt(name: string): string {
    const i = name.lastIndexOf('.');
    if (i <= 0) return '';
    if (i === name.length - 1) return '';
    return name.substring(i);
}

// Convert an absolute LKL path back to a mount-relative path, or null if
// the absolute path falls outside the mount (e.g. a symlink that escapes).
function stripMount(mountPath: string, abs: string): string | null {
    const base = mountPath.replace(/\/+$/, '');
    const target = abs.replace(/\/+$/, '');
    if (target === base) return '';
    if (target.startsWith(base + '/')) return target.slice(base.length + 1);
    return null;
}

// ── URL hash sync ─────────────────────────────────────────────────────
// Format: `#/<seg1>/<seg2>` with each segment URI-encoded. Empty path → `#/`.
// We track only relPath in the hash — the disk image itself can't be
// re-hydrated from URL (it lives in a File object), so a fresh page load
// drops back to the picker regardless. Browser back/forward fires
// `popstate` (and on hash-only navigations, `hashchange`); both feed into
// the same handler which reads location.hash and updates relPath.

function parseHash(): string {
    if (typeof window === 'undefined') return '';
    const h = window.location.hash;
    if (!h) return '';
    const stripped = h.replace(/^#\/?/, '');
    if (!stripped) return '';
    try {
        return stripped.split('/').map(decodeURIComponent).filter(Boolean).join('/');
    } catch {
        return '';
    }
}

function buildHash(rel: string): string {
    if (!rel) return '#/';
    return '#/' + rel.split('/').filter(Boolean).map(encodeURIComponent).join('/');
}

function writeHash(rel: string, mode: 'push' | 'replace') {
    if (typeof window === 'undefined') return;
    const next = buildHash(rel);
    if (window.location.hash === next) return;
    const url = window.location.pathname + window.location.search + next;
    if (mode === 'push') window.history.pushState(null, '', url);
    else window.history.replaceState(null, '', url);
}

export function AnyfsFileBrowser({
    disk: diskProp,
    mountPath: mountProp,
    followSymlinks = true,
    darkMode,
    className,
    onFileActivate,
    rootLabel = '/',
    superCrumb,
}: AnyfsFileBrowserProps) {
    const ctx = useAnyfsDiskMaybe();
    const disk = diskProp ?? ctx?.disk ?? null;
    const mountPath = mountProp ?? ctx?.mountPath ?? null;

    // Seed from URL hash so deep-linking and back/forward work. The hash is
    // the source of truth for "where am I" inside the mount.
    const [relPath, setRelPath] = useState<string>(() => parseHash());
    const relPathRef = useRef(relPath);
    const [files, setFiles] = useState<FileArray>([null]);
    const navGen = useRef(0);

    // Single navigation helper. Sets state, mirrors to hash, and updates the
    // ref synchronously so the popstate/hashchange listener can dedupe the
    // echo that some browsers fire after pushState/replaceState.
    const navigate = useCallback((next: string, mode: 'push' | 'replace' = 'push') => {
        if (next === relPathRef.current) return;
        relPathRef.current = next;
        setRelPath(next);
        writeHash(next, mode);
    }, []);

    // Browser back/forward → hash changes → pull new relPath out of the hash.
    useEffect(() => {
        const handler = () => {
            const next = parseHash();
            if (next === relPathRef.current) return;
            relPathRef.current = next;
            setRelPath(next);
        };
        window.addEventListener('popstate', handler);
        window.addEventListener('hashchange', handler);
        return () => {
            window.removeEventListener('popstate', handler);
            window.removeEventListener('hashchange', handler);
        };
    }, []);

    // Reset to root only when the mount actually *changes* (different
    // partition / disk). On first mount we honor whatever was in the hash
    // so deep-link reloads work for users who can re-supply the same image.
    const prevMountRef = useRef<string | null>(null);
    useEffect(() => {
        const cur = mountPath ?? null;
        if (prevMountRef.current !== null && prevMountRef.current !== cur) {
            // replaceState so back-button doesn't strand the user on a path
            // that belonged to the previous partition.
            navigate('', 'replace');
        }
        prevMountRef.current = cur;
        navGen.current += 1;
        setFiles([null]);
    }, [disk, mountPath, navigate]);

    // Readdir + lazy stat-follow per row.
    useEffect(() => {
        if (!disk || !mountPath) return;
        const myGen = ++navGen.current;
        setFiles([null, null, null]); // chonky shows a loader skeleton

        const abs = joinAbs(mountPath, relPath);
        (async () => {
            let entries;
            try {
                entries = await disk.readdir(abs);
            } catch (err) {
                if (navGen.current !== myGen) return;
                // eslint-disable-next-line no-console
                console.warn(`[anyfs/trees] readdir(${abs}) failed:`, err);
                setFiles([]);
                return;
            }
            if (navGen.current !== myGen) return;
            entries = entries.filter((e) => e.name !== '.' && e.name !== '..');
            entries.sort((a, b) => {
                const ad = a.kind === 'dir' ? 0 : 1;
                const bd = b.kind === 'dir' ? 0 : 1;
                if (ad !== bd) return ad - bd;
                return a.name.localeCompare(b.name);
            });

            const initial: FileData[] = entries.map((e) => ({
                id: rowIdFor(relPath, e.name),
                name: e.name,
                ext: e.kind === 'dir' ? '' : splitExt(e.name),
                isDir: e.kind === 'dir',
                isSymlink: e.kind === 'link',
            }));
            setFiles(initial);

            // Fan out stat per row. Worker ops are serialized so this naturally
            // queues; we just need the nav-generation guard to drop stale fills.
            // followSymlinks=false → use lstat so symlinks show their *own*
            // size/mtime instead of the target's; this matches the user-facing
            // semantic of the toggle (the setting also gates Properties below
            // and navigation in handleFileAction).
            const rowStat = followSymlinks
                ? (p: string) => disk.statFollow(p)
                : (p: string) => disk.stat(p);
            for (const e of entries) {
                if (navGen.current !== myGen) return;
                const absChild = joinAbs(mountPath, rowIdFor(relPath, e.name));
                let st: Stat;
                try {
                    st = await rowStat(absChild);
                } catch {
                    continue;
                }
                if (navGen.current !== myGen) return;
                setFiles((prev) => {
                    const idx = prev.findIndex((f) => f && f.id === rowIdFor(relPath, e.name));
                    if (idx < 0) return prev;
                    const next = prev.slice();
                    const cur = next[idx]!;
                    const merged: FileData = {
                        ...cur,
                        // statFollow on a symlink-to-dir returns kind='dir';
                        // promote the row so dblclick navigates instead of
                        // streaming a (zero-length) "file".
                        isDir: cur.isDir || st.kind === 'dir',
                        size: st.size,
                    };
                    if (st.mtime) merged.modDate = new Date(st.mtime * 1000);
                    next[idx] = merged;
                    return next;
                });
            }
        })();
    }, [disk, mountPath, relPath, followSymlinks]);

    const folderChain = useMemo<FileArray>(() => {
        if (!mountPath) return [];
        const chain: FileData[] = [];
        if (superCrumb) {
            chain.push({ id: '__super__', name: superCrumb.label, isDir: true });
        }
        // Empty rootLabel = icon-only crumb. Two things to work around:
        //   1. Chonky's sanitizer (redux/files-transforms.ts) drops any FileData
        //      whose `name` is falsy — '' included. Use ZWSP so the entry
        //      survives but no glyph renders for the text.
        //   2. Chonky's default first-crumb icon is `folder`, which looks the
        //      same as a regular directory; force `database` (disk-stack) to
        //      make the root visually distinct + obviously the disk root.
        const isEmpty = rootLabel === '';
        chain.push({
            id: '__root__',
            name: isEmpty ? '​' : rootLabel,
            isDir: true,
            folderChainIcon: isEmpty ? ChonkyIconName.database : undefined,
        });
        if (!relPath) return chain;
        const parts = relPath.split('/');
        let acc = '';
        for (const p of parts) {
            acc = acc ? `${acc}/${p}` : p;
            chain.push({ id: acc, name: p, isDir: true });
        }
        return chain;
    }, [mountPath, relPath, rootLabel, superCrumb]);

    const [propsTarget, setPropsTarget] = useState<PropsTarget | null>(null);

    const handleFileAction = useCallback(
        async (data: ChonkyFileActionData) => {
            // Chonky's ChonkyFileActionData is a discriminated union over built-in
            // action IDs, so a custom `defineFileAction` id isn't in the union and
            // a direct `data.id === ShowPropertiesAction.id` narrows `data` to
            // `never`. Compare via a widened string and re-read state from a cast.
            if ((data.id as string) === ShowPropertiesAction.id) {
                // Properties: stat the selected row and pop the modal. Use the
                // same lstat-vs-stat choice as the row metadata so the modal
                // mirrors what the size column shows.
                const anyData = data as unknown as {
                    state: { selectedFilesForAction?: FileData[]; selectedFiles?: FileData[] };
                };
                const sel = (
                    anyData.state.selectedFilesForAction ||
                    anyData.state.selectedFiles ||
                    []
                ).filter(Boolean) as FileData[];
                const tgt = sel[0];
                if (!tgt || !disk || !mountPath) return;
                if (tgt.id === '__super__' || tgt.id === '__root__') return;
                const abs = joinAbs(mountPath, tgt.id);
                let lstat: Stat;
                try {
                    lstat = await disk.stat(abs);
                } catch {
                    return;
                }
                // Symlink: surface the literal target string (readlink), not a
                // followed/canonical path. The user explicitly wants raw lstat
                // metadata, with the link's stored target shown verbatim.
                let linkTarget: string | null = null;
                if (lstat.kind === 'link' && typeof disk.readlink === 'function') {
                    try {
                        linkTarget = await disk.readlink(abs);
                    } catch {
                        /* best effort */
                    }
                }
                setPropsTarget({
                    name: tgt.name ?? tgt.id.split('/').pop() ?? tgt.id,
                    relPath: tgt.id,
                    absPath: abs,
                    stat: lstat,
                    linkTarget,
                });
                return;
            }
            if (data.id !== ChonkyActions.OpenFiles.id) return;
            const tgt = data.payload.targetFile ?? data.payload.files[0];
            if (!tgt) return;
            if (tgt.id === '__super__') {
                superCrumb?.onClick();
                return;
            }
            if (tgt.id === '__root__') {
                navigate('');
                return;
            }

            // Decide file vs dir. `tgt.isDir` is the truth for regular entries
            // (from readdir d_type) and for symlinks-to-dirs whose isDir got
            // promoted by statFollow. Two cases where isDir is stale and we
            // need an extra stat to be sure:
            //   - symlink whose statFollow hasn't completed yet (race)
            //   - followSymlinks=false (rowStat uses lstat → no promotion)
            // Without this, dblclicking a sym-to-file used to fall into the
            // dir branch (because tgt.isSymlink was true), realpath would
            // throw ENOTDIR, and the catch-all `navigate(tgt.id)` would set
            // the URL hash to the symlink path — no download.
            let isEffectivelyDir = !!tgt.isDir;
            if (tgt.isSymlink && !tgt.isDir && disk && mountPath) {
                try {
                    const st = await disk.statFollow(joinAbs(mountPath, tgt.id));
                    isEffectivelyDir = st.kind === 'dir';
                } catch {
                    /* broken link or otherwise — treat as file */
                }
            }

            if (isEffectivelyDir && disk && mountPath) {
                // followSymlinks=true: canonicalize via realpath so the breadcrumb
                // shows the resolved target (e.g. /bin → /usr/bin).
                // followSymlinks=false: keep the literal link path.
                if (followSymlinks && tgt.isSymlink) {
                    try {
                        const canon = await disk.realpath(joinAbs(mountPath, tgt.id));
                        const rel = stripMount(mountPath, canon);
                        if (rel !== null) {
                            navigate(rel);
                            return;
                        }
                    } catch {
                        /* fall through to literal navigate */
                    }
                }
                navigate(tgt.id);
                return;
            }
            // Regular file, or symlink-to-file. Stream the bytes to the host.
            if (onFileActivate && mountPath) {
                onFileActivate({ relPath: tgt.id, mountPath });
            }
        },
        [onFileActivate, mountPath, disk, followSymlinks, navigate, superCrumb],
    );

    if (!disk || !mountPath) {
        return <div className={className}>Loading…</div>;
    }

    return (
        <div
            className={className}
            style={{ minHeight: 200, display: 'flex', flexDirection: 'column' }}
        >
            <FileBrowser
                files={files}
                folderChain={folderChain}
                onFileAction={handleFileAction}
                fileActions={[ShowPropertiesAction]}
                defaultFileViewActionId={ChonkyActions.EnableListView.id}
                {...(darkMode !== undefined ? { darkMode } : {})}
            >
                <FileNavbar />
                <FileToolbar />
                <FileList />
                <FileContextMenu />
            </FileBrowser>
            {propsTarget && (
                <PropertiesModal
                    target={propsTarget}
                    darkMode={!!darkMode}
                    onClose={() => setPropsTarget(null)}
                />
            )}
        </div>
    );
}

// ── Properties modal ─────────────────────────────────────────────────────

function fmtMode(mode: number): string {
    // POSIX mode bits: 9 perm + setuid/setgid/sticky + file type.
    const types: Array<[number, string]> = [
        [0o140000, 's'], // socket
        [0o120000, 'l'], // symlink
        [0o100000, '-'], // regular
        [0o060000, 'b'], // block dev
        [0o040000, 'd'], // dir
        [0o020000, 'c'], // char dev
        [0o010000, 'p'], // fifo
    ];
    let typeCh = '?';
    for (const [m, ch] of types) {
        if ((mode & 0o170000) === m) {
            typeCh = ch;
            break;
        }
    }
    const perm = (bits: number, suid: boolean, gid: boolean, sticky: boolean) => {
        const r = bits & 4 ? 'r' : '-';
        const w = bits & 2 ? 'w' : '-';
        let x = bits & 1 ? 'x' : '-';
        if (suid) x = bits & 1 ? 's' : 'S';
        if (gid) x = bits & 1 ? 's' : 'S';
        if (sticky) x = bits & 1 ? 't' : 'T';
        return r + w + x;
    };
    const u = perm((mode >> 6) & 7, !!(mode & 0o4000), false, false);
    const g = perm((mode >> 3) & 7, false, !!(mode & 0o2000), false);
    const o = perm(mode & 7, false, false, !!(mode & 0o1000));
    return `${typeCh}${u}${g}${o} (0${(mode & 0o7777).toString(8)})`;
}

function fmtBytes(n: number): string {
    if (n < 1024) return `${n} B`;
    if (n < 1024 * 1024) return `${n} B (${(n / 1024).toFixed(1)} KiB)`;
    if (n < 1024 * 1024 * 1024) return `${n} B (${(n / 1024 / 1024).toFixed(1)} MiB)`;
    return `${n} B (${(n / 1024 / 1024 / 1024).toFixed(2)} GiB)`;
}

function fmtTime(sec: number): string {
    if (!sec) return '—';
    const d = new Date(sec * 1000);
    return `${d
        .toISOString()
        .replace('T', ' ')
        .replace(/\.\d+Z$/, ' UTC')} (epoch ${sec})`;
}

// Linux dev_t encoding: 64-bit major:minor split documented in <sys/sysmacros.h>
// (the historic glibc split: major = (dev >> 8) & 0xfff | (dev >> 32) & 0xfffff000;
//  minor = (dev & 0xff) | (dev >> 12) & 0xffffff00). Display "major:minor (raw)".
function fmtDev(dev: number): string {
    const major = ((dev >>> 8) & 0xfff) | ((Math.floor(dev / 0x100000000) >>> 0) & 0xfffff000);
    const minor = (dev & 0xff) | ((dev >>> 12) & 0xffffff00);
    return `${major}:${minor} (${dev})`;
}

function PropertiesModal({
    target,
    darkMode,
    onClose,
}: {
    target: PropsTarget;
    darkMode: boolean;
    onClose: () => void;
}) {
    useEffect(() => {
        const onKey = (e: KeyboardEvent) => {
            if (e.key === 'Escape') onClose();
        };
        window.addEventListener('keydown', onKey);
        return () => window.removeEventListener('keydown', onKey);
    }, [onClose]);

    const { stat, linkTarget } = target;
    // Theme: caller passes darkMode for the file-tree wrapper; mirror that
    // here so the modal blends in with the embedding app.
    const bg = darkMode ? '#18181b' : '#fff';
    const fg = darkMode ? '#e4e4e7' : '#18181b';
    const sub = darkMode ? '#a1a1aa' : '#52525b';
    const border = darkMode ? '#3f3f46' : '#d4d4d8';
    const hdrBg = darkMode ? '#27272a' : '#f4f4f5';

    const Row = ({ label, value }: { label: string; value: React.ReactNode }) => (
        <div style={{ display: 'flex', borderTop: `1px solid ${border}`, padding: '6px 0' }}>
            <div style={{ width: 100, color: sub, fontSize: 12 }}>{label}</div>
            <div
                style={{
                    flex: 1,
                    color: fg,
                    fontSize: 13,
                    fontFamily: 'ui-monospace, monospace',
                    wordBreak: 'break-all',
                }}
            >
                {value}
            </div>
        </div>
    );

    // Show every field the kernel filled in. Optional fields (dev/uid/gid/
    // rdev/blksize/blocks) are emitted by anyfs_ts only when the wasm side
    // includes them — older bundles won't have them, so render conditionally.
    const renderStat = (s: Stat) => (
        <div>
            <Row label="kind" value={s.kind} />
            <Row label="mode" value={fmtMode(s.mode)} />
            <Row label="size" value={fmtBytes(s.size)} />
            <Row label="inode" value={String(s.ino)} />
            <Row label="nlink" value={String(s.nlink)} />
            {s.uid !== undefined && <Row label="uid" value={String(s.uid)} />}
            {s.gid !== undefined && <Row label="gid" value={String(s.gid)} />}
            {s.dev !== undefined && <Row label="dev" value={fmtDev(s.dev)} />}
            {s.rdev !== undefined && s.rdev !== 0 && <Row label="rdev" value={fmtDev(s.rdev)} />}
            {s.blksize !== undefined && <Row label="blksize" value={`${s.blksize} B`} />}
            {s.blocks !== undefined && (
                <Row label="blocks" value={`${s.blocks} (×512 B = ${s.blocks * 512} B)`} />
            )}
            <Row label="mtime" value={fmtTime(s.mtime)} />
            <Row label="atime" value={fmtTime(s.atime)} />
            <Row label="ctime" value={fmtTime(s.ctime)} />
        </div>
    );

    return (
        <div
            onClick={onClose}
            style={{
                position: 'fixed',
                inset: 0,
                zIndex: 50,
                display: 'flex',
                alignItems: 'center',
                justifyContent: 'center',
                background: 'rgba(0,0,0,0.6)',
            }}
            role="dialog"
            aria-modal="true"
        >
            <div
                onClick={(e) => e.stopPropagation()}
                style={{
                    background: bg,
                    color: fg,
                    border: `1px solid ${border}`,
                    borderRadius: 8,
                    width: '100%',
                    maxWidth: 540,
                    margin: '0 16px',
                    boxShadow: '0 10px 25px rgba(0,0,0,0.3)',
                    maxHeight: '85vh',
                    overflow: 'auto',
                }}
            >
                <header
                    style={{
                        padding: '12px 16px',
                        borderBottom: `1px solid ${border}`,
                        background: hdrBg,
                        display: 'flex',
                        alignItems: 'center',
                        justifyContent: 'space-between',
                        borderRadius: '8px 8px 0 0',
                    }}
                >
                    <div>
                        <div style={{ fontSize: 14, fontWeight: 600 }}>{target.name}</div>
                        {stat.kind === 'link' && linkTarget !== null ? (
                            <div
                                style={{
                                    fontSize: 12,
                                    color: sub,
                                    fontFamily: 'ui-monospace, monospace',
                                    marginTop: 2,
                                }}
                            >
                                → {linkTarget}
                            </div>
                        ) : (
                            <div
                                style={{
                                    fontSize: 12,
                                    color: sub,
                                    fontFamily: 'ui-monospace, monospace',
                                    marginTop: 2,
                                }}
                            >
                                /{target.relPath || ''}
                            </div>
                        )}
                    </div>
                    <button
                        onClick={onClose}
                        aria-label="Close"
                        style={{
                            background: 'transparent',
                            border: 'none',
                            color: sub,
                            fontSize: 20,
                            cursor: 'pointer',
                            lineHeight: 1,
                            padding: '0 4px',
                        }}
                    >
                        ×
                    </button>
                </header>
                <div style={{ padding: '8px 16px 16px' }}>{renderStat(stat)}</div>
            </div>
        </div>
    );
}

import { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import type { AnyfsSession, Stat } from '@anyfs/core';
import { fmtBytes, fmtDev, fmtMode, fmtTime, splitExt } from '@anyfs/core';
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
    /** Override the session; otherwise read from <AnyfsProvider>. */
    session?: AnyfsSession;
    /** Override mount point; otherwise from provider. */
    mountPath?: string;
    /**
     * When true (default), opening a symlink-to-directory resolves through
     * `session.realpath` and the breadcrumb shows the canonical target (e.g.
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
    session: sessionProp,
    mountPath: mountProp,
    followSymlinks = true,
    darkMode,
    className,
    onFileActivate,
    rootLabel = '/',
    superCrumb,
}: AnyfsFileBrowserProps) {
    const ctx = useAnyfsDiskMaybe();
    const session = sessionProp ?? ctx?.session ?? null;
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
    }, [session, mountPath, navigate]);

    // Readdir + lazy stat-follow per row.
    useEffect(() => {
        if (!session || !mountPath) return;
        const myGen = ++navGen.current;
        setFiles([null, null, null]); // chonky shows a loader skeleton

        const abs = joinAbs(mountPath, relPath);
        (async () => {
            let entries;
            try {
                entries = await session.readdir(abs);
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
                ? (p: string) => session.statFollow(p)
                : (p: string) => session.stat(p);
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
    }, [session, mountPath, relPath, followSymlinks]);

    const folderChain = useMemo<FileArray>(() => {
        if (!mountPath) return [];
        const chain: FileData[] = [];
        if (superCrumb) {
            chain.push({
                id: '__super__',
                name: superCrumb.label,
                isDir: true,
            });
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
            // Properties: stat the selected row and pop the modal.
            if ((data.id as string) === ShowPropertiesAction.id) {
                const anyData = data as unknown as {
                    state: {
                        selectedFilesForAction?: FileData[];
                        selectedFiles?: FileData[];
                    };
                };
                const sel = (
                    anyData.state.selectedFilesForAction ||
                    anyData.state.selectedFiles ||
                    []
                ).filter(Boolean) as FileData[];
                const tgt = sel[0];
                if (!tgt || !session || !mountPath) return;
                if (tgt.id === '__super__' || tgt.id === '__root__') return;
                const abs = joinAbs(mountPath, tgt.id);
                let lstat: Stat;
                try {
                    lstat = await session.stat(abs);
                } catch {
                    return;
                }
                let linkTarget: string | null = null;
                if (lstat.kind === 'link' && typeof session.readlink === 'function') {
                    try {
                        linkTarget = await session.readlink(abs);
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

            let isEffectivelyDir = !!tgt.isDir;
            if (tgt.isSymlink && !tgt.isDir && session && mountPath) {
                try {
                    const st = await session.statFollow(joinAbs(mountPath, tgt.id));
                    isEffectivelyDir = st.kind === 'dir';
                } catch {
                    /* broken link or otherwise — treat as file */
                }
            }

            if (isEffectivelyDir && session && mountPath) {
                if (followSymlinks && tgt.isSymlink) {
                    try {
                        const canon = await session.realpath(joinAbs(mountPath, tgt.id));
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
            if (onFileActivate && mountPath) {
                onFileActivate({ relPath: tgt.id, mountPath });
            }
        },
        [onFileActivate, mountPath, session, followSymlinks, navigate, superCrumb],
    );

    if (!session || !mountPath) {
        return <div className={className}>Loading…</div>;
    }

    return (
        <div
            className={className}
            style={{
                minHeight: 200,
                display: 'flex',
                flexDirection: 'column',
            }}
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
    const bg = darkMode ? '#18181b' : '#fff';
    const fg = darkMode ? '#e4e4e7' : '#18181b';
    const sub = darkMode ? '#a1a1aa' : '#52525b';
    const border = darkMode ? '#3f3f46' : '#d4d4d8';
    const hdrBg = darkMode ? '#27272a' : '#f4f4f5';

    const Row = ({ label, value }: { label: string; value: React.ReactNode }) => (
        <div
            style={{
                display: 'flex',
                borderTop: `1px solid ${border}`,
                padding: '6px 0',
            }}
        >
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
                        <div
                            style={{
                                fontSize: 14,
                                fontWeight: 600,
                            }}
                        >
                            {target.name}
                        </div>
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
                <div style={{ padding: '8px 16px 16px' }}>
                    <Row label="kind" value={stat.kind} />
                    <Row label="mode" value={fmtMode(stat.mode)} />
                    <Row label="size" value={fmtBytes(stat.size)} />
                    <Row label="inode" value={String(stat.ino)} />
                    <Row label="nlink" value={String(stat.nlink)} />
                    {stat.uid !== undefined && <Row label="uid" value={String(stat.uid)} />}
                    {stat.gid !== undefined && <Row label="gid" value={String(stat.gid)} />}
                    {stat.dev !== undefined && <Row label="dev" value={fmtDev(stat.dev)} />}
                    {stat.rdev !== undefined && stat.rdev !== 0 && (
                        <Row label="rdev" value={fmtDev(stat.rdev)} />
                    )}
                    {stat.blksize !== undefined && (
                        <Row label="blksize" value={`${stat.blksize} B`} />
                    )}
                    {stat.blocks !== undefined && (
                        <Row
                            label="blocks"
                            value={`${stat.blocks} (×512 B = ${stat.blocks * 512} B)`}
                        />
                    )}
                    <Row label="mtime" value={fmtTime(stat.mtime)} />
                    <Row label="atime" value={fmtTime(stat.atime)} />
                    <Row label="ctime" value={fmtTime(stat.ctime)} />
                </div>
            </div>
        </div>
    );
}

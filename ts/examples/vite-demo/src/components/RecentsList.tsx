import type { Recent } from '../recents';
import { formatSize } from '@anyfs/core';

function formatTs(ts: number): string {
    const diff = Date.now() - ts;
    const m = Math.floor(diff / 60000);
    if (m < 1) return 'just now';
    if (m < 60) return `${m}m ago`;
    const h = Math.floor(m / 60);
    if (h < 24) return `${h}h ago`;
    const d = Math.floor(h / 24);
    if (d < 30) return `${d}d ago`;
    return new Date(ts).toLocaleDateString();
}

function RecentsList({
    recents,
    onReopen,
    onRemove,
}: {
    recents: Recent[];
    onReopen: (r: Recent) => void | Promise<void>;
    onRemove: (id: string) => void | Promise<void>;
}) {
    return (
        <div className="space-y-1.5">
            <div className="text-[10px] uppercase tracking-wider text-zinc-500 dark:text-zinc-400 px-1">
                Recently opened
            </div>
            <ul className="rounded-xl border border-zinc-200 dark:border-zinc-800 overflow-hidden divide-y divide-zinc-200 dark:divide-zinc-800 bg-zinc-50 dark:bg-zinc-900/50">
                {recents.map((r) => (
                    <li
                        key={r.id}
                        className="flex items-center gap-2 px-3 py-2 hover:bg-zinc-100 dark:hover:bg-zinc-800/70 transition-colors group"
                    >
                        <button
                            type="button"
                            onClick={() => {
                                void onReopen(r);
                            }}
                            title={
                                r.kind === 'url'
                                    ? r.url
                                    : r.kind === 'path'
                                      ? r.path
                                      : `${r.name} (local file — browsers don’t expose paths)`
                            }
                            className="flex-1 min-w-0 flex items-center gap-2 text-left"
                        >
                            <span className="shrink-0 text-base">
                                {r.kind === 'url' ? '🌐' : r.kind === 'path' ? '📁' : '💾'}
                            </span>
                            <span className="flex-1 min-w-0">
                                <span className="block truncate text-sm text-zinc-800 dark:text-zinc-200">
                                    {r.name}
                                </span>
                                {r.kind === 'url' ? (
                                    <span
                                        className="block truncate text-[11px] font-mono text-zinc-500 dark:text-zinc-400"
                                        dir="ltr"
                                    >
                                        {r.url}
                                    </span>
                                ) : r.kind === 'path' ? (
                                    <span
                                        className="block truncate text-[11px] font-mono text-zinc-500 dark:text-zinc-400"
                                        dir="ltr"
                                    >
                                        {r.path}
                                    </span>
                                ) : (
                                    <span className="block truncate text-[11px] italic text-zinc-400 dark:text-zinc-500">
                                        local file · path hidden by browser
                                    </span>
                                )}
                                <span className="block text-[11px] text-zinc-500 dark:text-zinc-400">
                                    {formatTs(r.ts)}
                                    {r.size !== undefined ? ` · ${formatSize(r.size)}` : ''}
                                </span>
                            </span>
                        </button>
                        <button
                            type="button"
                            onClick={(e) => {
                                e.stopPropagation();
                                void onRemove(r.id);
                            }}
                            className="shrink-0 w-6 h-6 rounded text-zinc-400 hover:text-zinc-700 hover:bg-zinc-200 dark:hover:text-zinc-100 dark:hover:bg-zinc-700 opacity-0 group-hover:opacity-100 focus:opacity-100 transition-opacity"
                            aria-label={`Remove ${r.name} from recents`}
                            title="Remove from recents"
                        >
                            ×
                        </button>
                    </li>
                ))}
            </ul>
        </div>
    );
}

// Surfaced on the landing card so users know up front what we can open.
// Image formats come from the wasm QEMU bundle (build-anyfs-wasm/libblock.a);
// filesystems come from the LKL kernel config — the list below mirrors the
// CONFIG_*_FS=y set actually built into lkl-wasm/.config (run
// `grep _FS=y lkl-wasm/.config` to verify). XFS is the one mainline FS off
// (wasm32 computed-goto limitation); REISERFS / BCACHEFS / SYSV silently
// drop out of the kernel build and aren't listed.
// Image formats come from QEMU's block layer, not the Linux FS registry,
// so they have to stay hardcoded — there's no kernel pseudo-file for them.
// Mirrors the format drivers actually inside libblock.a; to verify run:
//   ls $HOME/qemu/build-anyfs-wasm/libblock.a.p/ | grep '^block_[a-z]\+\.c\.o$'
const IMAGE_FORMATS: Array<{ name: string; title?: string }> = [
    { name: 'raw' },
    { name: 'qcow2' },
    { name: 'qcow', title: 'qcow v1 (legacy, obsoleted by qcow2)' },
    { name: 'vmdk', title: 'VMware Virtual Machine Disk' },
    { name: 'vdi', title: 'VirtualBox Disk Image' },
    { name: 'vhd', title: 'Microsoft Virtual Hard Disk (fixed/dynamic, QEMU vpc driver)' },
    { name: 'vhdx', title: 'Microsoft Hyper-V Virtual Hard Disk v2' },
    {
        name: 'dmg',
        title: 'Apple Disk Image (UDIF, incl. UDBZ bzip2; lzfse-compressed chunks unsupported)',
    },
    { name: 'qed', title: 'QEMU Enhanced Disk (deprecated)' },
    { name: 'parallels', title: 'Parallels Desktop disk image' },
    { name: 'bochs', title: 'Bochs emulator disk image' },
    { name: 'cloop', title: 'Compressed Loopback (Knoppix)' },
];

// Fallback FS list shown when the kernel hasn't booted yet (or /proc/filesystems
// read fails). Should track CONFIG_*_FS=y in scripts/gen_lkl_config.sh.

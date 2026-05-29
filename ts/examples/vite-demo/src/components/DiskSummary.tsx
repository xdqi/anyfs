import type { SessionSource, SessionMeta } from '@anyfs/core';
import { fmtBytes } from '@anyfs/core';

function ptLabel(pt: string): string {
    const k = pt.toLowerCase();
    if (k === 'gpt') return 'GPT';
    if (k === 'dos' || k === 'mbr') return 'MBR';
    if (!k) return 'no partition table';
    return pt;
}

// Header rendered above the partition list: filename / URL, logical (virtual
// block device) size, container size on disk (Content-Length for URL, File.size
// for File — differs from logical for qcow2/vmdk/etc), and partition-table
// flavour. Best-effort; missing pieces hide.
function DiskSummary({
    source,
    meta,
    onDiskSize,
}: {
    source: SessionSource;
    meta: SessionMeta | null;
    onDiskSize: number | null;
}) {
    const name = sourceName(source);
    const logical = meta && meta.logical_size > 0 ? meta.logical_size : null;
    const compressed = onDiskSize !== null && logical !== null && onDiskSize < logical * 0.95;
    return (
        <div className="text-sm text-zinc-600 dark:text-zinc-400 text-center max-w-xl">
            <div className="font-mono text-zinc-800 dark:text-zinc-300 truncate" title={name}>
                {name}
            </div>
            <div className="mt-1 flex flex-wrap items-center justify-center gap-x-3 gap-y-1">
                {logical !== null && (
                    <span>
                        <span className="text-zinc-500">logical:</span>{' '}
                        <span className="text-zinc-800 dark:text-zinc-200">
                            {fmtBytes(logical)}
                        </span>
                    </span>
                )}
                {onDiskSize !== null && (
                    <span>
                        <span className="text-zinc-500">{compressed ? 'on disk:' : 'size:'}</span>{' '}
                        <span className="text-zinc-800 dark:text-zinc-200">
                            {fmtBytes(onDiskSize)}
                        </span>
                    </span>
                )}
                {meta && (
                    <span>
                        <span className="text-zinc-500">format:</span>{' '}
                        <span className="text-zinc-800 dark:text-zinc-200">
                            {ptLabel(meta.pt_type)}
                        </span>
                    </span>
                )}
            </div>
        </div>
    );
}

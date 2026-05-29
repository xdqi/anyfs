import { useEffect, useState, type ReactNode } from 'react';
import { useAnyfsDisk } from '@anyfs/react';

// Fallback FS list shown when the kernel hasn't booted yet (or /proc/filesystems
// read fails). Should track CONFIG_*_FS=y in scripts/gen_lkl_config.sh.
const FALLBACK_FS = [
    'ext2',
    'ext3',
    'ext4',
    'btrfs',
    'f2fs',
    'jfs',
    'gfs2',
    'nilfs2',
    'vfat',
    'exfat',
    'ntfs3',
    'hfs',
    'hfsplus',
    'iso9660',
    'udf',
    'squashfs',
    'erofs',
    'cramfs',
    'minix',
    'romfs',
    'ufs',
    'hpfs',
    'qnx4',
    'qnx6',
    'bfs',
    'omfs',
    'befs',
    'affs',
    'adfs',
    'efs',
    'vxfs',
];

// Pretty group label + name normalization based on the canonical kernel name.
const FS_GROUPS: Array<{ label: string; members: Record<string, string> }> = [
    {
        label: 'Mainstream',
        members: {
            ext2: 'ext2',
            ext3: 'ext3',
            ext4: 'ext4',
            btrfs: 'BTRFS',
            xfs: 'XFS',
            f2fs: 'F2FS',
            jfs: 'JFS',
            gfs2: 'GFS2',
            nilfs2: 'NILFS2',
        },
    },
    {
        label: 'Windows / macOS',
        members: {
            vfat: 'VFAT',
            msdos: 'MS-DOS',
            exfat: 'exFAT',
            ntfs3: 'NTFS',
            ntfs: 'NTFS',
            hfs: 'HFS',
            hfsplus: 'HFS+',
            apfs: 'APFS',
        },
    },
    {
        label: 'Optical / archive',
        members: {
            iso9660: 'ISO 9660',
            udf: 'UDF',
            squashfs: 'SquashFS',
            erofs: 'EROFS',
            cramfs: 'CRAMFS',
        },
    },
    {
        label: 'Legacy Unix',
        members: {
            minix: 'MINIX',
            romfs: 'ROMFS',
            ufs: 'UFS',
            hpfs: 'HPFS',
            qnx4: 'QNX4',
            qnx6: 'QNX6',
            bfs: 'BFS',
            omfs: 'OMFS',
            befs: 'BeFS',
            affs: 'AFFS',
            adfs: 'ADFS',
            efs: 'EFS',
            vxfs: 'VxFS',
        },
    },
];

// /proc/filesystems format: each line is either `nodev\t<name>` (pseudo-FS
// like proc/sysfs/tmpfs) or `\t<name>` (block-backed real FS). We only want
// the block-backed ones — the nodev set is full of internal kernel mounts
// that have nothing to do with reading a disk image.
function parseProcFilesystems(text: string): string[] {
    const out: string[] = [];
    for (const line of text.split('\n')) {
        if (!line) continue;
        const parts = line.split('\t');
        if (parts.length < 2) continue;
        const flag = parts[0]!.trim();
        const name = parts[1]!.trim();
        if (flag === 'nodev') continue;
        if (!name) continue;
        out.push(name);
    }
    return out;
}

export function SupportedFormats() {
    const { kernel, status } = useAnyfsDisk();
    const [kernelFs, setKernelFs] = useState<string[] | null>(null);

    useEffect(() => {
        if (!kernel) return;
        let cancelled = false;
        (async () => {
            try {
                const txt = await kernel.readKernelFile('/proc/filesystems');
                if (cancelled) return;
                setKernelFs(parseProcFilesystems(txt));
            } catch {
                // Kernel not ready or read failed — leave fallback in place.
            }
        })();
        return () => {
            cancelled = true;
        };
    }, [kernel]);

    const fsList = kernelFs ?? FALLBACK_FS;
    const fsSet = new Set(fsList);
    // Track which kernel names we've already shown so the "Other" bucket
    // catches anything compiled in but not categorized above.
    const claimed = new Set<string>();

    const Chip = ({ children, title }: { children: ReactNode; title?: string | undefined }) => (
        <span
            className="inline-block rounded-md bg-zinc-100 dark:bg-zinc-800 border border-zinc-200 dark:border-zinc-700 px-1.5 py-0.5 text-[11px] font-mono text-zinc-700 dark:text-zinc-300"
            title={title}
        >
            {children}
        </span>
    );
    const Group = ({
        label,
        chips,
    }: {
        label: string;
        chips: Array<{ key: string; text: string; title?: string | undefined }>;
    }) =>
        chips.length === 0 ? null : (
            <div>
                <div className="text-[10px] uppercase tracking-wider text-zinc-500 dark:text-zinc-400 mb-1">
                    {label}
                </div>
                <div className="flex flex-wrap gap-1">
                    {chips.map((c) => (
                        <Chip key={c.key} title={c.title}>
                            {c.text}
                        </Chip>
                    ))}
                </div>
            </div>
        );

    const groups = FS_GROUPS.map((g) => {
        const chips: Array<{ key: string; text: string; title?: string }> = [];
        // Coalesce ext2/3/4 into one chip when all three are present.
        if (
            g.label === 'Mainstream' &&
            fsSet.has('ext2') &&
            fsSet.has('ext3') &&
            fsSet.has('ext4')
        ) {
            chips.push({ key: 'ext234', text: 'ext2/3/4', title: 'ext2, ext3, ext4' });
            claimed.add('ext2');
            claimed.add('ext3');
            claimed.add('ext4');
        }
        for (const [name, label] of Object.entries(g.members)) {
            if (claimed.has(name)) continue;
            if (!fsSet.has(name)) continue;
            chips.push({ key: name, text: label, title: name });
            claimed.add(name);
        }
        return { label: g.label, chips };
    });
    const other = fsList.filter((n) => !claimed.has(n)).map((n) => ({ key: n, text: n }));

    const live = kernelFs !== null;
    const sourceNote = live
        ? `From /proc/filesystems (${fsList.length} filesystems)`
        : status === 'booting' || status === 'attaching' || status === 'mounting'
          ? 'Booting kernel — showing default list'
          : 'Default list — kernel not yet booted';

    return (
        <div className="pt-2 border-t border-zinc-200 dark:border-zinc-800 space-y-2">
            <Group
                label="Image formats"
                chips={IMAGE_FORMATS.map((f) => ({ key: f.name, text: f.name, title: f.title }))}
            />
            {groups.map((g) => (
                <Group key={g.label} label={g.label} chips={g.chips} />
            ))}
            <Group label="Other" chips={other} />
            <div className="text-[10px] text-zinc-500 dark:text-zinc-400 italic pt-0.5">
                {sourceNote}
            </div>
        </div>
    );
}

// Image formats come from QEMU's block layer, not the Linux FS registry,
// so they have to stay hardcoded — there's no kernel pseudo-file for them.
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

// `shrink-0 bg-* relative z-10` keeps the bar planted at the bottom and
// painted on top even when the page content above is taller than the
// viewport (e.g. the picker card with Recents + SupportedFormats expanded).
// Without the opaque bg, overflowing content peeks through the transparent
// bar; without shrink-0, flex layout can compress it on very short windows.
export const STATUS_BAR_CLS =
    'shrink-0 relative z-10 bg-white dark:bg-zinc-900 border-t border-zinc-200 dark:border-zinc-800 px-4 py-1.5 text-sm';

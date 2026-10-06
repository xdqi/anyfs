/** Running the image tools: mkfs and friends live in /sbin, off a normal PATH. */
import { spawnSync } from 'node:child_process';
import { existsSync } from 'node:fs';
import { delimiter, join } from 'node:path';

const PATH = ['/usr/sbin', '/sbin', process.env.PATH ?? ''].join(delimiter);

/** The Debian package that ships each tool, for the error message. */
const PROVIDER = {
    mke2fs: 'e2fsprogs',
    debugfs: 'e2fsprogs',
    'mkfs.fat': 'dosfstools',
    mcopy: 'mtools',
    'mkfs.exfat': 'exfatprogs',
    'mkfs.f2fs': 'f2fs-tools',
    mkntfs: 'ntfs-3g',
    'mkfs.btrfs': 'btrfs-progs',
    'mkfs.xfs': 'xfsprogs',
    xorriso: 'xorriso',
    mksquashfs: 'squashfs-tools',
    'qemu-img': 'qemu-utils',
    sfdisk: 'fdisk',
};

export function which(tool) {
    for (const d of PATH.split(delimiter)) {
        if (d && existsSync(join(d, tool))) return join(d, tool);
    }
    return null;
}

/** Throw naming every missing tool and the apt packages that provide them. */
export function requireTools(tools) {
    const missing = tools.filter((t) => !which(t));
    if (missing.length === 0) return;
    const pkgs = [...new Set(missing.map((t) => PROVIDER[t] ?? t))].join(' ');
    throw new Error(
        `missing tools: ${missing.join(', ')} — install with: sudo apt-get install ${pkgs}`,
    );
}

/** Run a tool; return stdout, or throw with its stderr. */
export function run(tool, args, { env = {}, cwd, input } = {}) {
    const res = spawnSync(which(tool) ?? tool, args, {
        cwd,
        input,
        encoding: 'utf-8',
        env: { ...process.env, PATH, ...env },
    });
    if (res.error) throw res.error;
    if (res.status !== 0) {
        throw new Error(`${tool} ${args.join(' ')} failed (exit ${res.status}):\n${res.stderr}`);
    }
    return res.stdout;
}

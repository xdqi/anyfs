/**
 * Base images for the robustness corpus, built rootless from the known
 * tree. Containers and partitioned disks are assembled from the
 * single-filesystem bases listed before them.
 *
 * Reproducible (byte-identical across builds): the e2fs-based images (ext4,
 * ext4panic, ext2, qcow2, and the gpt / mbr / mbrext disks holding them),
 * vfat, iso9660 and squashfs. Not reproducible: btrfs (random UUIDs), xfs
 * (root-inode timestamps and the CRC over them), vmdk (qemu writes a random
 * CID), and exfat, f2fs and ntfs (random serials / UUIDs), so cases.json
 * records every image's sha256. exfat, f2fs and ntfs hold no files; they
 * exist only to check that the filesystem still mounts.
 */
import {
    closeSync,
    mkdirSync,
    openSync,
    readFileSync,
    readdirSync,
    rmSync,
    truncateSync,
    writeFileSync,
    writeSync,
} from 'node:fs';
import { join } from 'node:path';
import { probeLayout } from './layout.mjs';
import { run } from './tools.mjs';
import { EPOCH, TREE, writeTree } from './tree.mjs';

const MiB = 1 << 20;
const UUID = '0a0b0c0d-1111-2222-3333-444455556666';
const E2FS_ENV = { E2FSPROGS_FAKE_TIME: String(EPOCH), SOURCE_DATE_EPOCH: String(EPOCH) };
/** mkfs.xfs refuses filesystems under 300 MB unless it believes fstests runs it. */
const XFS_SMALL_ENV = { TEST_DIR: '1', TEST_DEV: '1', QA_CHECK_FS: '1' };

export const TOOLS = [
    'mke2fs',
    'debugfs',
    'mkfs.fat',
    'mcopy',
    'mkfs.exfat',
    'mkfs.f2fs',
    'mkntfs',
    'mkfs.btrfs',
    'mkfs.xfs',
    'xorriso',
    'mksquashfs',
    'qemu-img',
    'sfdisk',
];

/** A fresh sparse file of `size` bytes. */
function sparse(file, size) {
    rmSync(file, { force: true });
    closeSync(openSync(file, 'w'));
    truncateSync(file, size);
}

/** Copy image `src` into disk `dst` at byte offset `at`. */
function place(dst, src, at) {
    const fd = openSync(dst, 'r+');
    try {
        const b = readFileSync(src);
        writeSync(fd, b, 0, b.length, at);
    } finally {
        closeSync(fd);
    }
}

function mke2fs(file, sizeMiB, type, tree, extra = []) {
    sparse(file, sizeMiB * MiB);
    run(
        'mke2fs',
        [
            '-q',
            '-F',
            '-t',
            type,
            // 1 KiB blocks and 256-byte inodes: root (2) and /docs land in
            // different inode-table blocks.
            '-b',
            '1024',
            '-I',
            '256',
            '-N',
            '128',
            '-U',
            UUID,
            '-E',
            `hash_seed=${UUID}`,
            ...(type === 'ext2' ? [] : ['-O', 'metadata_csum']),
            ...extra,
            '-d',
            tree,
            file,
        ],
        { env: E2FS_ENV },
    );
}

/** mkfs.xfs protofile describing TREE, file sources under `tree`. */
export function xfsProto(tree) {
    const root = new Map();
    for (const e of TREE) {
        const parts = e.path.split('/');
        let m = root;
        for (const d of parts.slice(0, -1)) {
            if (!m.has(d)) m.set(d, new Map());
            m = m.get(d);
        }
        m.set(parts.at(-1), e);
    }
    const lines = ['/dev/null', '0 0', 'd--755 0 0'];
    const emit = (m, rel, depth) => {
        const pad = ' '.repeat(depth);
        for (const [name, v] of [...m].sort(([a], [b]) => a.localeCompare(b))) {
            if (v instanceof Map) {
                lines.push(`${pad}${name} d--755 0 0`);
                emit(v, `${rel}${name}/`, depth + 1);
                lines.push(`${pad}$`);
            } else if (v.symlink) {
                lines.push(`${pad}${name} l--777 0 0 ${v.symlink}`);
            } else {
                lines.push(`${pad}${name} ---644 0 0 ${join(tree, rel, name)}`);
            }
        }
    };
    emit(root, '', 1);
    lines.push('$');
    return `${lines.join('\n')}\n`;
}

const GPT_SCRIPT = `label: gpt
label-id: 0A0B0C0D-1111-2222-3333-444455556666
first-lba: 2048
start=2048, size=32768, type=0FC63DAF-8483-4772-8E79-3D69D8477DE4, uuid=0A0B0C0D-1111-2222-3333-000000000001
start=34816, size=32768, type=EBD0A0A2-B9E5-4433-87C0-68B6B72699C7, uuid=0A0B0C0D-1111-2222-3333-000000000002
`;
const MBR_SCRIPT = `label: dos
label-id: 0x0a0b0c0d
start=2048, size=32768, type=83
start=34816, size=32768, type=c
`;
// p1 ext4, p2 extended, p5 (logical) vfat. The EBR sits at the extended start.
const MBREXT_SCRIPT = `label: dos
label-id: 0x0a0b0c0e
start=2048, size=32768, type=83
start=34816, size=36864, type=5
start=36864, size=32768, type=c
`;

function sfdiskDisk(file, script, parts, ctx) {
    sparse(file, 48 * MiB);
    run('sfdisk', ['-q', '--no-reread', '--no-tell-kernel', file], { input: script });
    for (const [base, sector] of parts) place(file, ctx.built[base], sector * 512);
}

/**
 * name → { fs, ext, probe?, build(file, ctx) }. ctx: { posix, fat, scratch,
 * built } — the two tree dirs, a scratch dir, and paths of bases built so
 * far. Insertion order is build order.
 */
export const BASES = {
    ext4: {
        fs: 'ext4',
        ext: 'img',
        probe: 'ext4',
        build: (f, c) => mke2fs(f, 16, 'ext4', c.posix),
    },
    ext4panic: {
        fs: 'ext4',
        ext: 'img',
        probe: 'ext4',
        // The superblock asks for a panic on the first error (acceptance #4).
        build: (f, c) => mke2fs(f, 16, 'ext4', c.posix, ['-e', 'panic']),
    },
    ext2: { fs: 'ext2', ext: 'img', build: (f, c) => mke2fs(f, 8, 'ext2', c.posix) },
    vfat: {
        fs: 'vfat',
        ext: 'img',
        probe: 'vfat',
        build: (f, c) => {
            sparse(f, 16 * MiB);
            run('mkfs.fat', ['-F', '16', '--invariant', '-n', 'ANYFS', f]);
            const top = readdirSync(c.fat)
                .sort()
                .map((n) => join(c.fat, n));
            run('mcopy', ['-s', '-m', '-i', f, ...top, '::/'], { env: { MTOOLS_SKIP_CHECK: '1' } });
        },
    },
    exfat: {
        fs: 'exfat',
        ext: 'img',
        build: (f) => {
            sparse(f, 16 * MiB);
            run('mkfs.exfat', ['-L', 'ANYFS', f]);
        },
    },
    f2fs: {
        fs: 'f2fs',
        ext: 'img',
        build: (f) => {
            sparse(f, 64 * MiB);
            run('mkfs.f2fs', ['-q', '-f', '-l', 'ANYFS', f]);
        },
    },
    ntfs: {
        fs: 'ntfs',
        ext: 'img',
        build: (f) => {
            sparse(f, 16 * MiB);
            run('mkntfs', ['-F', '-Q', '-q', '-L', 'ANYFS', '-s', '512', f]);
        },
    },
    btrfs: {
        fs: 'btrfs',
        ext: 'img',
        probe: 'btrfs',
        build: (f, c) => {
            sparse(f, 32 * MiB);
            run('mkfs.btrfs', ['-q', '-f', '--mixed', '-U', UUID, '--rootdir', c.posix, f]);
        },
    },
    xfs: {
        fs: 'xfs',
        ext: 'img',
        probe: 'xfs',
        build: (f, c) => {
            const proto = join(c.scratch, 'xfs.proto');
            writeFileSync(proto, xfsProto(c.posix));
            sparse(f, 32 * MiB);
            run('mkfs.xfs', ['-q', '-f', '-m', `uuid=${UUID}`, '-p', proto, f], {
                env: XFS_SMALL_ENV,
            });
        },
    },
    iso9660: {
        fs: 'iso9660',
        ext: 'iso',
        probe: 'iso9660',
        build: (f, c) => {
            rmSync(f, { force: true });
            // Rock Ridge only: a Joliet SVD would shadow the PVD the mutations edit.
            run('xorriso', ['-as', 'mkisofs', '-quiet', '-R', '-V', 'ANYFS', '-o', f, c.posix], {
                env: { SOURCE_DATE_EPOCH: String(EPOCH) },
            });
        },
    },
    squashfs: {
        fs: 'squashfs',
        ext: 'img',
        build: (f, c) => {
            rmSync(f, { force: true });
            run(
                'mksquashfs',
                [
                    c.posix,
                    f,
                    '-quiet',
                    '-no-progress',
                    '-noappend',
                    '-all-root',
                    '-mkfs-time',
                    String(EPOCH),
                    '-all-time',
                    String(EPOCH),
                ],
                {
                    // An exported SOURCE_DATE_EPOCH conflicts with -mkfs-time/-all-time.
                    env: { SOURCE_DATE_EPOCH: undefined },
                },
            );
        },
    },
    qcow2: {
        fs: 'ext4 (qcow2)',
        ext: 'qcow2',
        build: (f, c) =>
            run('qemu-img', ['convert', '-q', '-f', 'raw', '-O', 'qcow2', c.built.ext4, f]),
    },
    vmdk: {
        fs: 'ext4 (vmdk)',
        ext: 'vmdk',
        build: (f, c) =>
            run('qemu-img', [
                'convert',
                '-q',
                '-f',
                'raw',
                '-O',
                'vmdk',
                '-o',
                'subformat=monolithicSparse',
                c.built.ext4,
                f,
            ]),
    },
    gpt: {
        fs: 'ext4+vfat (gpt)',
        ext: 'img',
        build: (f, c) =>
            sfdiskDisk(
                f,
                GPT_SCRIPT,
                [
                    ['ext4', 2048],
                    ['vfat', 34816],
                ],
                c,
            ),
    },
    mbr: {
        fs: 'ext4+vfat (mbr)',
        ext: 'img',
        build: (f, c) =>
            sfdiskDisk(
                f,
                MBR_SCRIPT,
                [
                    ['ext4', 2048],
                    ['vfat', 34816],
                ],
                c,
            ),
    },
    mbrext: {
        fs: 'ext4+vfat (mbr, extended)',
        ext: 'img',
        build: (f, c) =>
            sfdiskDisk(
                f,
                MBREXT_SCRIPT,
                [
                    ['ext4', 2048],
                    ['vfat', 36864],
                ],
                c,
            ),
    },
};

/** Build every base under `scratch`. Returns { files, bufs, layouts }. */
export function buildAllBases(scratch, { log = () => {} } = {}) {
    mkdirSync(scratch, { recursive: true });
    const ctx = {
        posix: join(scratch, 'tree-posix'),
        fat: join(scratch, 'tree-fat'),
        scratch,
        built: {},
    };
    writeTree(ctx.posix);
    writeTree(ctx.fat, { symlinks: false });
    const bufs = {};
    const layouts = {};
    for (const [name, b] of Object.entries(BASES)) {
        const file = join(scratch, `${name}.${b.ext}`);
        b.build(file, ctx);
        ctx.built[name] = file;
        bufs[name] = readFileSync(file);
        if (b.probe) layouts[name] = probeLayout(b.probe, file, bufs[name]);
        log(name);
    }
    return { files: ctx.built, bufs, layouts };
}

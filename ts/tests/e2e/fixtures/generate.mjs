#!/usr/bin/env node
/**
 * Build the three REAL disk-image fixtures the anyfs-reader E2E suite browses:
 *
 *   - multi.img         GPT, p1 ext4 + p2 vfat
 *   - mbr-extended.img  MBR/dos, 2 primary (ext4, vfat) + extended w/ 2 logicals (ext4)
 *   - btrfs-whole.vmdk  whole-disk btrfs (no PT), raw -> vmdk via qemu-img
 *
 * Privileged tooling (sgdisk, sfdisk, mkfs, mount) lives in /sbin, which is NOT on
 * the non-root PATH of this Node process. We therefore run every partition/mkfs/
 * mount/populate step under `sudo` (root PATH includes /sbin). `truncate` and
 * `qemu-img` are on the normal PATH, so they run unprivileged.
 *
 * The script is idempotent: an image whose output file already exists is skipped.
 * Every loop device + mountpoint is torn down in a finally block so a failure never
 * leaks loops or mounts onto the host.
 *
 * On success it prints the exact byte length of every known file it wrote so the
 * manifest (manifest.ts) can be reconciled against generated reality.
 */
import { spawnSync } from 'node:child_process';
import { existsSync, mkdirSync, rmSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, resolve } from 'node:path';

const here = dirname(fileURLToPath(import.meta.url));
const IMAGES_DIR = resolve(here, 'images');

/** Known-file byte sizes, collected during the build, printed at the end. */
const recorded = {};

/** Run a command, inheriting stdio, throwing on non-zero exit. */
function run(cmd, args, opts = {}) {
    const res = spawnSync(cmd, args, { stdio: 'inherit', ...opts });
    if (res.error) {
        throw res.error;
    }
    if (res.status !== 0) {
        throw new Error(`command failed (exit ${res.status}): ${cmd} ${args.join(' ')}`);
    }
    return res;
}

/** Run a command under sudo -n (passwordless), throwing on non-zero exit. */
function sudo(args, opts = {}) {
    return run('sudo', ['-n', ...args], opts);
}

/** Run `sudo -n sh -c <script>`; the script runs with the root PATH (incl. /sbin). */
function sudoSh(script, opts = {}) {
    return sudo(['sh', '-c', script], opts);
}

/** Capture stdout of a command (utf-8), throwing on non-zero exit. */
function capture(cmd, args) {
    const res = spawnSync(cmd, args, { encoding: 'utf-8' });
    if (res.error) {
        throw res.error;
    }
    if (res.status !== 0) {
        throw new Error(
            `command failed (exit ${res.status}): ${cmd} ${args.join(' ')}\n${res.stderr ?? ''}`,
        );
    }
    return res.stdout;
}

/** Verify passwordless sudo is available; exit non-zero with a clear message if not. */
function requireRoot() {
    const res = spawnSync('sudo', ['-n', 'true'], { stdio: 'ignore' });
    if (res.status !== 0) {
        console.error(
            'ERROR: passwordless sudo is required to build the disk-image fixtures.\n' +
                '`sudo -n true` failed. Configure NOPASSWD sudo and retry.',
        );
        process.exit(1);
    }
}

/**
 * Write a string to <mount>/<rel> via sudo (tee), record its byte length under
 * `recorded[key]`, and return the byte length. The trailing-newline policy is the
 * caller's: pass the exact bytes you want on disk.
 */
function writeFile(mount, rel, content, key) {
    const dest = `${mount}/${rel}`;
    const bytes = Buffer.byteLength(content, 'utf-8');
    // tee preserves exact bytes; printf via sh would mangle backslashes.
    sudo(['tee', dest], { input: content, stdio: ['pipe', 'ignore', 'inherit'] });
    recorded[key] = bytes;
    return bytes;
}

/** mkdir -p a path inside the mount, as root. */
function mkdirIn(mount, rel) {
    sudo(['mkdir', '-p', `${mount}/${rel}`]);
}

/** Attach an image with partition scanning, returning the loop device path. */
function loopAttach(img) {
    const out = capture('sudo', ['-n', 'losetup', '--show', '-fP', img]).trim();
    if (!out.startsWith('/dev/loop')) {
        throw new Error(`unexpected losetup output: ${out}`);
    }
    return out;
}

/** Detach a loop device, ignoring errors (best-effort cleanup). */
function loopDetach(loop) {
    if (!loop) {
        return;
    }
    spawnSync('sudo', ['-n', 'losetup', '-d', loop], { stdio: 'ignore' });
}

/** Make a fresh temp mountpoint dir owned by root, return its path. */
function makeMountpoint(tag) {
    const mp = capture('sudo', ['-n', 'mktemp', '-d', `/tmp/anyfs-fixt-${tag}-XXXXXX`]).trim();
    return mp;
}

/** Unmount a path, ignoring errors. */
function umount(mp) {
    if (!mp) {
        return;
    }
    spawnSync('sudo', ['-n', 'umount', mp], { stdio: 'ignore' });
}

/** Remove an (empty) mountpoint dir, ignoring errors. */
function rmdir(mp) {
    if (!mp) {
        return;
    }
    spawnSync('sudo', ['-n', 'rmdir', mp], { stdio: 'ignore' });
}

// ---------------------------------------------------------------------------
// multi.img : GPT, p1 ext4 (+32M) + p2 vfat (rest)
// ---------------------------------------------------------------------------
function buildMultiRaw() {
    const img = resolve(IMAGES_DIR, 'multi.img');
    if (existsSync(img)) {
        console.log(`[multi.img] exists, skipping`);
        return;
    }
    console.log(`[multi.img] building...`);
    // 64 MiB total: 32M ext4 + remainder vfat (FAT32 needs a comfortable floor).
    run('truncate', ['-s', '64M', img]);

    let loop;
    let mpExt;
    let mpFat;
    let ok = false;
    try {
        // GPT: p1 +32M type 8300 (Linux fs), p2 rest type 0700 (Microsoft basic data).
        sudoSh(
            `sgdisk -Z "${img}" >/dev/null 2>&1 || true; ` +
                `sgdisk -n 1:0:+32M -t 1:8300 -c 1:linux ` +
                `-n 2:0:0 -t 2:0700 -c 2:data "${img}"`,
        );

        loop = loopAttach(img);
        const p1 = `${loop}p1`;
        const p2 = `${loop}p2`;

        sudoSh(`mkfs.ext4 -F -q "${p1}"`);
        sudoSh(`mkfs.fat -F32 "${p2}" >/dev/null`);

        mpExt = makeMountpoint('multi-ext');
        sudo(['mount', p1, mpExt]);
        writeFile(mpExt, 'hello.txt', 'hello, world\n', 'multi.hello.txt');
        mkdirIn(mpExt, 'dir');
        sudoSh(`head -c 4096 /dev/zero > "${mpExt}/dir/nested.bin"`);
        recorded['multi.dir/nested.bin'] = Number(
            capture('sudo', ['-n', 'stat', '-c', '%s', `${mpExt}/dir/nested.bin`]).trim(),
        );
        mkdirIn(mpExt, 'empty');
        sudo(['ln', '-s', 'hello.txt', `${mpExt}/link`]);
        sudo(['sync']);
        umount(mpExt);
        mpExt = undefined;

        mpFat = makeMountpoint('multi-fat');
        sudo(['mount', p2, mpFat]);
        writeFile(mpFat, 'README.TXT', 'read me all\n', 'multi.README.TXT');
        sudo(['sync']);
        umount(mpFat);
        mpFat = undefined;
        ok = true;
    } finally {
        umount(mpExt);
        rmdir(mpExt);
        umount(mpFat);
        rmdir(mpFat);
        loopDetach(loop);
        // A failed build must not leave a half-built image that existsSync would
        // later treat as a complete skip. The .img is user-owned (truncate), so a
        // plain rmSync suffices.
        if (!ok) {
            rmSync(img, { force: true });
        }
    }
    console.log(`[multi.img] done`);
}

// ---------------------------------------------------------------------------
// mbr-extended.img : DOS label, 2 primary + extended w/ 2 logicals
// ---------------------------------------------------------------------------
function buildMbrExtended() {
    const img = resolve(IMAGES_DIR, 'mbr-extended.img');
    if (existsSync(img)) {
        console.log(`[mbr-extended.img] exists, skipping`);
        return;
    }
    console.log(`[mbr-extended.img] building...`);
    // 96 MiB: room for 4 small filesystems (ext4 has a ~1-2 MiB floor each).
    run('truncate', ['-s', '96M', img]);

    let loop;
    const mounts = [];
    let ok = false;
    try {
        // sfdisk DOS script. Sizes in 512-byte sectors. Layout:
        //   p1 primary ext4  (type 83)  16 MiB
        //   p2 primary vfat  (type c)   16 MiB
        //   p3 extended      (type 5)   rest  -> holds the logicals
        //   p5 logical ext4  (type 83)  16 MiB
        //   p6 logical ext4  (type 83)  rest of extended
        // Letting sfdisk auto-place starts keeps EBR alignment correct.
        const script = [
            'label: dos',
            'unit: sectors',
            'size=32768, type=83',
            'size=32768, type=c',
            'type=5',
            'size=32768, type=83',
            'type=83',
            '',
        ].join('\n');
        sudoSh(`sfdisk "${img}"`, { input: script, stdio: ['pipe', 'inherit', 'inherit'] });

        loop = loopAttach(img);
        const p1 = `${loop}p1`;
        const p2 = `${loop}p2`;
        const p5 = `${loop}p5`;
        const p6 = `${loop}p6`;

        sudoSh(`mkfs.ext4 -F -q "${p1}"`);
        sudoSh(`mkfs.fat -F16 "${p2}" >/dev/null`);
        sudoSh(`mkfs.ext4 -F -q "${p5}"`);
        sudoSh(`mkfs.ext4 -F -q "${p6}"`);

        const writeOne = (dev, opts, tag, rel, content, key) => {
            const mp = makeMountpoint(`mbr-${tag}`);
            mounts.push(mp);
            sudo(['mount', ...opts, dev, mp]);
            writeFile(mp, rel, content, key);
            sudo(['sync']);
            umount(mp);
            mounts.pop();
            rmdir(mp);
        };

        writeOne(p1, [], 'p1', 'p1.txt', 'p1\n', 'mbr.p1.txt');
        writeOne(p2, [], 'p2', 'P2.TXT', 'p2\n', 'mbr.P2.TXT');
        writeOne(p5, [], 'p5', 'l5.txt', 'l5\n', 'mbr.l5.txt');
        writeOne(p6, [], 'p6', 'l6.txt', 'l6\n', 'mbr.l6.txt');
        ok = true;
    } finally {
        for (const mp of mounts) {
            umount(mp);
            rmdir(mp);
        }
        loopDetach(loop);
        // A failed build must not leave a half-built image that existsSync would
        // later treat as a complete skip. The .img is user-owned (truncate), so a
        // plain rmSync suffices.
        if (!ok) {
            rmSync(img, { force: true });
        }
    }
    console.log(`[mbr-extended.img] done`);
}

// ---------------------------------------------------------------------------
// btrfs-whole.vmdk : whole-disk btrfs (no PT), raw -> vmdk
// ---------------------------------------------------------------------------
function buildBtrfsVmdk() {
    const vmdk = resolve(IMAGES_DIR, 'btrfs-whole.vmdk');
    if (existsSync(vmdk)) {
        console.log(`[btrfs-whole.vmdk] exists, skipping`);
        return;
    }
    console.log(`[btrfs-whole.vmdk] building...`);
    const raw = resolve(IMAGES_DIR, 'btrfs-whole.raw');
    // btrfs has a hard minimum well above 128M in practice; 256M is comfortable.
    run('truncate', ['-s', '256M', raw]);

    let loop;
    let mp;
    try {
        sudoSh(`mkfs.btrfs -f "${raw}" >/dev/null`);
        loop = loopAttach(raw);

        mp = makeMountpoint('btrfs');
        sudo(['mount', loop, mp]);
        writeFile(mp, 'whole.txt', 'whole disk\n', 'btrfs.whole.txt');
        mkdirIn(mp, 'sub');
        sudo(['sync']);
        umount(mp);
        mp = undefined;
        loopDetach(loop);
        loop = undefined;

        // Convert the populated raw image to VMDK (unprivileged).
        run('qemu-img', ['convert', '-O', 'vmdk', raw, vmdk]);
    } finally {
        umount(mp);
        rmdir(mp);
        loopDetach(loop);
        // Drop the intermediate raw image (root-owned).
        spawnSync('sudo', ['-n', 'rm', '-f', raw], { stdio: 'ignore' });
    }
    console.log(`[btrfs-whole.vmdk] done`);
}

function main() {
    requireRoot();
    mkdirSync(IMAGES_DIR, { recursive: true });

    buildMultiRaw();
    buildMbrExtended();
    buildBtrfsVmdk();

    console.log('\n=== recorded known-file byte sizes ===');
    for (const key of Object.keys(recorded).sort()) {
        console.log(`${key}: ${recorded[key]}`);
    }
}

main();

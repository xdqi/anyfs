/*
 * Headless native smoke (ANYFS_NATIVE_SMOKE=1) for packaged and dev builds.
 *
 * Drives the addon the way the renderer does — kernelInit → sessionOpen →
 * sessionMetaJson/sessionListJson → sessionEnter → readdirJson → read a file —
 * then halts the kernel, so a CI runner can prove that a package loads its
 * staged native addon (not the wasm fallback) and that the whole native
 * closure (LKL, QEMU block layer, libblkid) works on the target OS.
 *
 * Inputs (environment):
 *   ANYFS_NATIVE_IMAGE  image to open (default: vite-demo's multi.img, dev only)
 *   ANYFS_NATIVE_PART   partition to mount: table index or label
 *                       (default: the first ext2/ext3/ext4/vfat partition)
 *   ANYFS_NATIVE_READ   file to read inside that partition, relative to its root
 *   ANYFS_NATIVE_OUT    JSON report path (default: <tmpdir>/anyfs-native-smoke.json)
 *
 * The report is written on failure too (ok:false + error). Checking it
 * against expected values is the caller's job (scripts/check-smoke.mjs).
 */
import { createHash } from 'node:crypto';
import { existsSync, readdirSync } from 'node:fs';
import { writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

type SmokeAddon = {
    kernelInit(memMb: number, loglevel: number): Promise<number>;
    kernelHalt(): Promise<number>;
    sessionOpen(imagePath: string, flags: number): Promise<number>;
    sessionClose(h: number): Promise<number>;
    sessionListJson(h: number): Promise<string>;
    sessionMetaJson(h: number): Promise<string>;
    sessionEnter(h: number, part: number, flags: number): Promise<string>;
    readdirJson(path: string): Promise<string>;
    statJson(path: string): Promise<string>;
    fileOpen(path: string, flags: number): Promise<number>;
    pread(fd: number, n: number, off: number): Promise<{ rc: number; data: Uint8Array }>;
    fileClose(fd: number): Promise<number>;
};

type PartInfo = { index: number; fstype?: string; label?: string };

export type NativeSmokeContext = {
    addonPath: string | null;
    load: () => SmokeAddon | null;
    rendererDir: string;
    resourcesPath: string;
    packaged: boolean;
    versions: { app: string; electron: string; node: string };
    defaultImage: string;
};

const RDONLY = 1;
const READ_CHUNK = 1 << 20;

/** Hashed wasm fallback files under <renderer>/wasm/<hash>/ (vite.config.ts). */
function listWasmBundle(rendererDir: string): string[] {
    const root = join(rendererDir, 'wasm');
    if (!existsSync(root)) return [];
    const out: string[] = [];
    for (const dir of readdirSync(root)) {
        for (const f of readdirSync(join(root, dir))) out.push(`wasm/${dir}/${f}`);
    }
    return out.sort();
}

function pickPartition(parts: PartInfo[], want: string | undefined): PartInfo | undefined {
    if (want) {
        return parts.find((p) => String(p.index) === want || p.label === want);
    }
    return parts.find((p) => ['ext2', 'ext3', 'ext4', 'vfat'].includes(p.fstype ?? ''));
}

export async function runNativeSmoke(ctx: NativeSmokeContext): Promise<number> {
    const out = process.env.ANYFS_NATIVE_OUT || join(tmpdir(), 'anyfs-native-smoke.json');
    const report: Record<string, unknown> = {
        ok: false,
        platform: process.platform,
        arch: process.arch,
        versions: ctx.versions,
        packaged: ctx.packaged,
        resourcesPath: ctx.resourcesPath,
        addonPath: ctx.addonPath,
        rendererDir: ctx.rendererDir,
        rendererIndex: existsSync(join(ctx.rendererDir, 'index.html')),
        wasmBundle: listWasmBundle(ctx.rendererDir),
    };
    let m: SmokeAddon | null = null;
    let inited = false;
    let rc = 1;
    try {
        m = ctx.load();
        if (!m) throw new Error('addon not loadable');
        const initRc = await m.kernelInit(512, 4);
        if (initRc !== 0) throw new Error(`kernelInit rc=${initRc}`);
        inited = true;

        const img = process.env.ANYFS_NATIVE_IMAGE || ctx.defaultImage;
        report.image = img;
        const h = await m.sessionOpen(img, RDONLY);
        if (h < 0) throw new Error(`sessionOpen rc=${h}`);
        report.meta = JSON.parse(await m.sessionMetaJson(h));
        const parts = JSON.parse(await m.sessionListJson(h)) as PartInfo[];
        report.partitions = parts;

        const part = pickPartition(parts, process.env.ANYFS_NATIVE_PART);
        if (!part) throw new Error('no mountable partition matched');
        const mount = await m.sessionEnter(h, part.index, RDONLY);
        const entries = JSON.parse(await m.readdirJson(mount)) as { name: string }[];
        report.mounted = { index: part.index, fstype: part.fstype, label: part.label, path: mount };
        report.entries = entries.map((e) => e.name).sort();

        const rel = process.env.ANYFS_NATIVE_READ;
        if (rel) {
            const path = `${mount}/${rel}`;
            const st = JSON.parse(await m.statJson(path)) as { size: number };
            const fd = await m.fileOpen(path, 0);
            if (fd < 0) throw new Error(`fileOpen(${path}) rc=${fd}`);
            const hash = createHash('sha256');
            let off = 0;
            try {
                while (off < st.size) {
                    const { rc: got, data } = await m.pread(fd, READ_CHUNK, off);
                    if (got < 0) throw new Error(`pread(${path}, ${off}) rc=${got}`);
                    if (got === 0) break;
                    hash.update(data.subarray(0, got));
                    off += got;
                }
            } finally {
                await m.fileClose(fd);
            }
            report.file = { path: rel, size: st.size, read: off, sha256: hash.digest('hex') };
        }

        const closeRc = await m.sessionClose(h);
        if (closeRc !== 0) throw new Error(`sessionClose rc=${closeRc}`);
        rc = 0;
    } catch (e) {
        report.error = e instanceof Error ? e.message : String(e);
    }
    // Halt explicitly: a DLL's atexit handlers cannot do it on Windows (F19).
    if (m && inited) {
        const haltRc = await m.kernelHalt();
        report.kernelHalt = haltRc;
        if (haltRc !== 0 && rc === 0) {
            report.error = `kernelHalt rc=${haltRc}`;
            rc = 1;
        }
    }
    report.ok = rc === 0;
    await writeFile(out, JSON.stringify(report, null, 2));
    console.log(`[native:smoke] ${report.ok ? 'ok' : 'FAILED'}; report in ${out}`);
    if (!report.ok) console.error(`[native:smoke] ${String(report.error)}`);
    return rc;
}

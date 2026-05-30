import type { DirEntry, LklFd, SessionMeta, SessionPartInfo, Stat } from './types.js';

export interface AnyfsSession {
    // ── Lifecycle ──────────────────────────────────────
    attachBlob(blob: Blob): Promise<void>;
    attachUrl(url: string, name?: string): Promise<void>;
    attachPath(path: string): Promise<void>;
    close(): Promise<void>;

    // ── Partition / mount ─────────────────────────────
    /** Enter a partition. part=0 mounts the whole disk. */
    enter(part: number, flags?: number): Promise<string>;
    listParts(): Promise<SessionPartInfo[]>;
    meta(): Promise<SessionMeta>;

    // ── Filesystem ops ────────────────────────────────
    readdir(path: string): Promise<DirEntry[]>;
    stat(path: string): Promise<Stat>;
    statFollow(path: string): Promise<Stat>;
    readlink(path: string): Promise<string>;
    realpath(path: string): Promise<string>;
    readKernelFile(path: string, maxBytes?: number): Promise<string>;

    // ── Low-level fd ops ──────────────────────────────
    openFd(path: string): Promise<LklFd>;
    readFd(fd: LklFd, offset: number, length: number): Promise<Uint8Array>;
    closeFd(fd: LklFd): Promise<void>;

    // ── Derived ───────────────────────────────────────
    openReadable(
        path: string,
        opts?: { chunkSize?: number },
    ): Promise<{ stream: ReadableStream<Uint8Array>; size: number }>;
    walk(root: string, chunkSize?: number): AsyncGenerator<string[]>;

    // ── Events ────────────────────────────────────────
    onProgress(cb: (step: string) => void): () => void;
}

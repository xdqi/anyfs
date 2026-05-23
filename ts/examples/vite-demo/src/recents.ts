// Recently-opened images, persisted to IndexedDB.
//
// Files use the File System Access API (Chrome/Edge/Safari 17+): handles are
// structured-cloneable so we store them directly. Re-opening requires a user
// gesture — the browser only re-grants read permission after a real click.
// URLs are trivially serialisable; we keep the same store for both so the
// landing UI has one list to render.
//
// On browsers without FSA (Firefox today, iOS Safari <17) the file path
// degrades gracefully: addRecentFile is never called because the picker
// falls back to <input type=file>, which doesn't expose a persistable
// handle. URLs still work everywhere.
//
// Storage budget: keep the newest MAX_RECENTS entries by `ts`. Handle
// objects are tiny (~hundreds of bytes); URLs are smaller. We never store
// file *contents* — only the handle the browser uses to ask permission.

import type { DiskSource } from '@anyfs/core';

// TS's stock lib.dom only ships the FileSystemFileHandle type itself; the
// permission helpers and the showOpenFilePicker entry are still flagged as
// non-standard. Augment globally so the rest of the file stays type-safe.
declare global {
    interface FileSystemHandle {
        queryPermission(desc?: { mode?: 'read' | 'readwrite' }): Promise<PermissionState>;
        requestPermission(desc?: { mode?: 'read' | 'readwrite' }): Promise<PermissionState>;
    }
    interface Window {
        showOpenFilePicker(opts?: {
            multiple?: boolean;
            excludeAcceptAllOption?: boolean;
            types?: Array<{ description?: string; accept: Record<string, string[]> }>;
        }): Promise<FileSystemFileHandle[]>;
    }
}

const DB_NAME = 'anyfs-recents';
const STORE = 'recents';
const DB_VERSION = 1;
const MAX_RECENTS = 12;

interface RecentBase {
    id: string;
    name: string;
    size?: number;
    ts: number;
}
export interface RecentFile extends RecentBase {
    kind: 'file';
    handle: FileSystemFileHandle;
}
export interface RecentUrl extends RecentBase {
    kind: 'url';
    url: string;
}
export type Recent = RecentFile | RecentUrl;

export function fsaSupported(): boolean {
    return (
        typeof window !== 'undefined' &&
        typeof (window as { showOpenFilePicker?: unknown }).showOpenFilePicker === 'function'
    );
}

// The whole feature is gated on FSA. URL entries work without FSA in
// principle, but the product decision is to hide the entire Recents UI on
// browsers that can't persist local files — having a list that only ever
// remembers URLs is more confusing than valuable.
export function recentsSupported(): boolean {
    return typeof indexedDB !== 'undefined' && fsaSupported();
}

let dbPromise: Promise<IDBDatabase> | null = null;
function openDb(): Promise<IDBDatabase> {
    if (dbPromise) return dbPromise;
    dbPromise = new Promise((resolve, reject) => {
        const req = indexedDB.open(DB_NAME, DB_VERSION);
        req.onupgradeneeded = () => {
            const db = req.result;
            if (!db.objectStoreNames.contains(STORE)) {
                db.createObjectStore(STORE, { keyPath: 'id' });
            }
        };
        req.onsuccess = () => resolve(req.result);
        req.onerror = () => reject(req.error ?? new Error('indexedDB open failed'));
    });
    return dbPromise;
}

function tx(db: IDBDatabase, mode: IDBTransactionMode): IDBObjectStore {
    return db.transaction(STORE, mode).objectStore(STORE);
}

function awaitReq<T>(req: IDBRequest<T>): Promise<T> {
    return new Promise((resolve, reject) => {
        req.onsuccess = () => resolve(req.result);
        req.onerror = () => reject(req.error ?? new Error('idb request failed'));
    });
}

export async function listRecents(): Promise<Recent[]> {
    if (!recentsSupported()) return [];
    try {
        const db = await openDb();
        const all = (await awaitReq(tx(db, 'readonly').getAll())) as Recent[];
        return all.sort((a, b) => b.ts - a.ts);
    } catch {
        // IDB unavailable (private window quota, blocked, …) — degrade silently.
        return [];
    }
}

function newId(): string {
    return `r-${Date.now().toString(36)}-${Math.random().toString(36).slice(2, 10)}`;
}

// IDB transactions auto-commit the moment the event loop turns without a
// pending request. Any await on a non-IDB promise (isSameEntry, network,
// etc) inside a tx will detach all subsequent put/delete calls. So we
// split: read in tx1, compute outside, write in tx2.

async function readAll(db: IDBDatabase): Promise<Recent[]> {
    return (await awaitReq(tx(db, 'readonly').getAll())) as Recent[];
}

async function putOne(db: IDBDatabase, rec: Recent): Promise<void> {
    const store = tx(db, 'readwrite');
    store.put(rec);
    await awaitReq(store.transaction.objectStore(STORE).get(rec.id));
}

async function trim(db: IDBDatabase): Promise<void> {
    const all = await readAll(db);
    if (all.length <= MAX_RECENTS) return;
    all.sort((a, b) => a.ts - b.ts);
    const drop = all.slice(0, all.length - MAX_RECENTS);
    const store = tx(db, 'readwrite');
    for (const d of drop) store.delete(d.id);
    await new Promise<void>((resolve, reject) => {
        store.transaction.oncomplete = () => resolve();
        store.transaction.onerror = () => reject(store.transaction.error);
    });
}

export async function addRecentFile(
    handle: FileSystemFileHandle,
    name: string,
    size?: number,
): Promise<void> {
    if (!recentsSupported()) return;
    const db = await openDb();
    // Dedupe: find existing entry for the same file via isSameEntry. Done
    // OUTSIDE any tx because isSameEntry is async and would drop one.
    const all = await readAll(db);
    let match: RecentFile | null = null;
    for (const r of all) {
        if (r.kind !== 'file') continue;
        try {
            if (await r.handle.isSameEntry(handle)) {
                match = r;
                break;
            }
        } catch {
            // Stale handle (backing file moved/deleted) — treat as miss.
        }
    }
    const rec: RecentFile = match
        ? { ...match, name, ts: Date.now(), ...(size !== undefined ? { size } : {}) }
        : {
              id: newId(),
              kind: 'file',
              handle,
              name,
              ts: Date.now(),
              ...(size !== undefined ? { size } : {}),
          };
    await putOne(db, rec);
    if (!match) await trim(db);
}

export async function addRecentUrl(url: string, name: string, size?: number): Promise<void> {
    if (!recentsSupported()) return;
    const db = await openDb();
    const all = await readAll(db);
    const match = all.find((r): r is RecentUrl => r.kind === 'url' && r.url === url) ?? null;
    const rec: RecentUrl = match
        ? { ...match, name, ts: Date.now(), ...(size !== undefined ? { size } : {}) }
        : {
              id: newId(),
              kind: 'url',
              url,
              name,
              ts: Date.now(),
              ...(size !== undefined ? { size } : {}),
          };
    await putOne(db, rec);
    if (!match) await trim(db);
}

export async function removeRecent(id: string): Promise<void> {
    if (!recentsSupported()) return;
    const db = await openDb();
    tx(db, 'readwrite').delete(id);
}

export type ReopenResult =
    | { kind: 'ok'; source: DiskSource }
    | { kind: 'denied' }
    | { kind: 'missing' };

/**
 * Resolve a Recent entry to something the AnyfsProvider can attach.
 *
 * MUST be called from a user gesture for file entries — the browser only
 * grants `requestPermission()` if there's an active activation. URL entries
 * are gesture-independent.
 */
export async function tryReopen(r: Recent): Promise<ReopenResult> {
    if (r.kind === 'url') {
        return { kind: 'ok', source: { kind: 'url', url: r.url, name: r.name } };
    }
    try {
        // queryPermission returns 'granted' if the user already allowed this
        // handle in the current page lifetime; otherwise 'prompt'. Either way
        // requestPermission() is the canonical entry point — it short-circuits
        // when already granted.
        const opts = { mode: 'read' as const };
        let state: PermissionState = await r.handle.queryPermission(opts);
        if (state !== 'granted') state = await r.handle.requestPermission(opts);
        if (state !== 'granted') return { kind: 'denied' };
        const file = await r.handle.getFile();
        // Re-write the entry with the freshest name/size + bumped ts.
        await addRecentFile(r.handle, file.name, file.size);
        return { kind: 'ok', source: { kind: 'file', file } };
    } catch (e) {
        const err = e as DOMException;
        if (err?.name === 'NotFoundError') return { kind: 'missing' };
        // SecurityError happens when called outside a user gesture; surface
        // as 'denied' so the UI prompts a click.
        if (err?.name === 'SecurityError') return { kind: 'denied' };
        throw e;
    }
}

export interface PickedFile {
    file: File;
    handle?: FileSystemFileHandle;
}

/**
 * Native picker, prefers FSA so we get a handle to persist. Returns null
 * when the user cancels.
 */
export async function pickFile(): Promise<PickedFile | null> {
    if (fsaSupported()) {
        try {
            const [handle] = await window.showOpenFilePicker({
                multiple: false,
                excludeAcceptAllOption: false,
            });
            if (!handle) return null;
            const file = await handle.getFile();
            return { file, handle };
        } catch (e) {
            if ((e as DOMException)?.name === 'AbortError') return null;
            throw e;
        }
    }
    return null;
}

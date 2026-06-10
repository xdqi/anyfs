import { test } from 'node:test';
import assert from 'node:assert/strict';
import { AnyfsSessionBase } from '../dist/index.js';

/** In-memory tree: dirs map path -> entries; files map path -> bytes. */
class FakeSession extends AnyfsSessionBase {
    constructor({ dirs = {}, files = {}, symlinkSizes = {} } = {}) {
        super();
        this.dirs = dirs;
        this.files = files;
        this.symlinkSizes = symlinkSizes; // lstat-size != follow-size repro
        this.openCount = 0;
        this.closedFds = [];
    }
    async attachBlob() {}
    async attachUrl() {}
    async attachPath() {}
    async enter() {
        return '/mnt';
    }
    async listParts() {
        return [];
    }
    async meta() {
        return {};
    }
    async readdir(path) {
        const e = this.dirs[path];
        if (!e) throw new Error(`ENOENT ${path}`);
        return e;
    }
    async stat(path) {
        if (path in this.symlinkSizes) return { size: this.symlinkSizes[path], mode: 0o120777 };
        return { size: this.files[path]?.length ?? 0, mode: 0o100644 };
    }
    async statFollow(path) {
        return { size: this.files[path]?.length ?? 0, mode: 0o100644 };
    }
    async readlink() {
        return '';
    }
    async realpath(p) {
        return p;
    }
    async readKernelFile() {
        return '';
    }
    onProgress() {
        return () => {};
    }
    async _openFdRaw() {
        this.openCount++;
        return this.openCount;
    }
    async _readFdRaw(fd, offset, length) {
        return this.currentBytes.subarray(offset, offset + length);
    }
    async _closeFdRaw(fd) {
        this.closedFds.push(fd);
    }
    async _dispose() {
        this.disposedCalled = true;
    }
}

async function drain(stream) {
    const chunks = [];
    for await (const c of stream) chunks.push(c);
    return chunks;
}

test('openReadable sizes from statFollow, not lstat (symlink truncation bug)', async () => {
    const data = new Uint8Array(100).fill(7);
    const s = new FakeSession({
        files: { '/etc/os-release': data },
        symlinkSizes: { '/etc/os-release': 21 }, // lstat sees link-target length
    });
    s.currentBytes = data;
    const { stream, size } = await s.openReadable('/etc/os-release');
    assert.equal(size, 100); // would be 21 if base used stat()
    const chunks = await drain(stream);
    assert.equal(
        chunks.reduce((n, c) => n + c.length, 0),
        100,
    );
});

test('openReadable chunks and closes the fd exactly once', async () => {
    const data = new Uint8Array(2500).fill(1);
    const s = new FakeSession({ files: { '/f': data } });
    s.currentBytes = data;
    const { stream } = await s.openReadable('/f', { chunkSize: 1000 });
    const chunks = await drain(stream);
    assert.deepEqual(
        chunks.map((c) => c.length),
        [1000, 1000, 500],
    );
    assert.deepEqual(s.closedFds, [1]);
});

test('openReadable cancel closes the fd', async () => {
    const data = new Uint8Array(5000).fill(1);
    const s = new FakeSession({ files: { '/f': data } });
    s.currentBytes = data;
    const { stream } = await s.openReadable('/f', { chunkSize: 1000 });
    const reader = stream.getReader();
    await reader.read();
    await reader.cancel();
    assert.deepEqual(s.closedFds, [1]);
});

test('walk is BFS, skips unreadable dirs, honors chunkSize', async () => {
    const s = new FakeSession({
        dirs: {
            '/': [
                { name: 'a', kind: 'dir' },
                { name: 'f1', kind: 'file' },
            ],
            '/a': [
                { name: 'f2', kind: 'file' },
                { name: 'bad', kind: 'dir' },
            ],
            // '/a/bad' missing -> readdir throws -> silently skipped
        },
    });
    const seen = [];
    for await (const chunk of s.walk('/', 2)) seen.push(...chunk);
    assert.deepEqual(seen, ['/a', '/f1', '/a/f2', '/a/bad']);
});

test('close() closes tracked fds, disposes once, and poisons the session', async () => {
    const data = new Uint8Array(10);
    const s = new FakeSession({ files: { '/f': data } });
    s.currentBytes = data;
    await s.openFd('/f');
    await s.close();
    assert.deepEqual(s.closedFds, [1]);
    assert.equal(s.disposedCalled, true);
    await assert.rejects(() => s.openFd('/f'), /already disposed/);
    await s.close(); // idempotent
});

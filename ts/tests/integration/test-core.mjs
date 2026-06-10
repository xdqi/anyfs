#!/usr/bin/env node
/*
 * test-core.mjs — Node.js core functionality tests.
 *
 * Direct API tests (no browser, no Electron, no CDP).
 * Covers: native+file, native+url, wasm+file, wasm+url.
 *
 * All four combos call the same underlying anyfs_ts_* C ABI; just the
 * module loader (native addon vs WASM) and disk-open target (local path vs
 * http:// URL via QEMU curl) differ.
 *
 * Usage:
 *   node tests/test-core.mjs                    # all 4 combos
 *   node tests/test-core.mjs --only native      # native addon only
 *   node tests/test-core.mjs --only wasm        # WASM only
 *   node tests/test-core.mjs --image path.img   # custom disk image
 */

import { resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { statSync, openSync, readSync } from 'node:fs';
import { Worker } from 'node:worker_threads';
import { createRequire } from 'node:module';

const __dirname = dirname(fileURLToPath(import.meta.url));
const ROOT = resolve(__dirname, '..');
const NATIVE_NODE = resolve(ROOT, 'ts/packages/anyfs-native/build/Release/anyfs_native.node');
const WASM_DIR = resolve(ROOT, 'ts/packages/core/wasm');
const require_ = createRequire(import.meta.url);

const GREEN = '\x1b[32m';
const RED = '\x1b[31m';
const BOLD = '\x1b[1m';
const RST = '\x1b[0m';

let passed = 0, failed = 0;
function pass(msg) { passed++; console.log(`  ${GREEN}✓${RST} ${msg}`); }
function fail(msg) { failed++; console.log(`  ${RED}✗${RST} ${msg}`); process.exitCode = 1; }
function assert(cond, msg) { cond ? pass(msg) : fail(msg); }

function findImage() {
  const arg = process.argv.find(a => a.startsWith('--image='))?.split('=')[1];
  if (arg) { try { statSync(arg); return arg; } catch {} }
  const candidates = [
    resolve(ROOT, 'tests/images/ext4.img'),
  ];
  for (const c of candidates) {
    try { statSync(c); return c; } catch {}
  }
  throw new Error('No disk image found. Pass --image=<path>.');
}

// ═══════════════════════════════════════════════════════════════════════════════
// HTTP file server in a Worker thread.
//
// The main thread is blocked during diskOpen() because QEMU's curl driver
// runs synchronously. A Worker thread has its own libuv event loop, so it
// can serve bytes while the main thread sits inside QEMU curl.
// ═══════════════════════════════════════════════════════════════════════════════

const HTTP_WORKER_CODE = `
import { parentPort, workerData } from 'node:worker_threads';
import { createServer } from 'node:http';
import { openSync, readSync, statSync } from 'node:fs';

const { filePath } = workerData;
const size = statSync(filePath).size;
let fd = null;

const server = createServer((req, res) => {
  try {
    if (fd === null) fd = openSync(filePath, 'r');
    if (req.method === 'HEAD') {
      res.writeHead(200, { 'Content-Length': String(size), 'Accept-Ranges': 'bytes' });
      res.end(); return;
    }
    let [start, end] = [0, size - 1], status = 200;
    const r = req.headers['range'];
    if (r) {
      const m = r.match(/bytes=(\\d+)-(\\d*)/);
      if (m) { start = +m[1]; end = m[2] ? +m[2] : size - 1; status = 206; }
    }
    const len = end - start + 1;
    res.writeHead(status, {
      'Content-Length': String(len),
      'Content-Range': \`bytes \${start}-\${end}/\${size}\`,
      'Accept-Ranges': 'bytes',
      'Content-Type': 'application/octet-stream'
    });
    const buf = Buffer.alloc(65536);
    let pos = start, remaining = len;
    const pump = () => {
      if (remaining <= 0) { res.end(); return; }
      try {
        const n = readSync(fd, buf, 0, Math.min(buf.length, remaining), pos);
        if (n <= 0) { res.end(); return; }
        res.write(buf.subarray(0, n));
        pos += n; remaining -= n;
        setImmediate(pump);
      } catch(e) { res.destroy(); }
    };
    pump();
  } catch(e) { res.writeHead(500); res.end(e.message); }
});

server.listen(0, '127.0.0.1', () => {
  parentPort.postMessage({ port: server.address().port });
});
parentPort.on('message', (msg) => {
  if (msg === 'stop') server.close();
});
`;

function startHttpWorker(filePath) {
  return new Promise((resolve, reject) => {
    const w = new Worker(HTTP_WORKER_CODE, {
      workerData: { filePath },
      eval: true,
    });
    const timer = setTimeout(() => { w.terminate(); reject(new Error('http worker timeout')); }, 10000);
    w.on('message', (msg) => { clearTimeout(timer); resolve({ worker: w, port: msg.port }); });
    w.on('error', (err) => { clearTimeout(timer); reject(err); });
  });
}

// ═══════════════════════════════════════════════════════════════════════════════
// Helpers
// ═══════════════════════════════════════════════════════════════════════════════

function mountAndReaddir(listFn, enterFn, mountWholeFn, readdirFn) {
  const parts = JSON.parse(listFn());
  const label = parts.length > 0
    ? parts.map(p => `${p.num}:${p.fstype || '?'}`).join(', ')
    : '(raw filesystem)';
  console.log(`    partitions: ${label}`);

  let mp;
  if (parts.length === 0) {
    mp = mountWholeFn('auto', 1);
    assert(mp && typeof mp === 'string', `mountWhole → ${mp}`);
  } else {
    mp = enterFn(parts[0].num, 1);
    assert(mp && typeof mp === 'string', `diskEnter → ${mp}`);
  }

  const entries = JSON.parse(readdirFn(mp));
  assert(entries.length > 0, `readdir → ${entries.length} entries`);
  const names = entries.filter(e => e.name !== '.' && e.name !== '..').map(e => e.name);
  console.log(`    files: ${names.join(', ')}`);
  return names;
}

function callJsonString(M, fnName, pathArg) {
  let cap = 4096;
  for (let i = 0; i < 6; i++) {
    const buf = M._malloc(cap);
    try {
      const n = M.ccall(fnName, 'number', ['string', 'number', 'number'], [pathArg, buf, cap]);
      if (n >= 0) return M.UTF8ToString(buf, n);
      if (-n <= cap) throw new Error(`${fnName}(${pathArg}) rc=${n}`);
      cap = Math.max(cap * 2, -n + 256);
    } finally { M._free(buf); }
  }
  throw new Error(`${fnName}: overflow loop`);
}

function callJsonHandle(M, fnName, h) {
  let cap = 4096;
  for (let i = 0; i < 6; i++) {
    const buf = M._malloc(cap);
    try {
      const n = M.ccall(fnName, 'number', ['number', 'number', 'number'], [h, buf, cap]);
      if (n >= 0) return M.UTF8ToString(buf, n);
      if (-n <= cap) throw new Error(`${fnName} rc=${n}`);
      cap = Math.max(cap * 2, -n + 256);
    } finally { M._free(buf); }
  }
  throw new Error(`${fnName}: overflow loop`);
}

function wasmCallMount(M, fnName, h, part, flags) {
  const buf = M._malloc(128);
  try {
    const rc = M.ccall(fnName, 'number', ['number', 'number', 'number', 'number', 'number'], [h, part, flags, buf, 128]);
    if (rc !== 0) throw new Error(`${fnName}(h=${h}) rc=${rc}`);
    return M.UTF8ToString(buf);
  } finally { M._free(buf); }
}

function wasmCallMountWhole(M, fnName, h, fstype, flags) {
  const buf = M._malloc(128);
  try {
    const rc = M.ccall(fnName, 'number', ['number', 'string', 'number', 'number', 'number'], [h, fstype ?? '', flags, buf, 128]);
    if (rc !== 0) throw new Error(`${fnName}(h=${h}) rc=${rc}`);
    return M.UTF8ToString(buf);
  } finally { M._free(buf); }
}

// ═══════════════════════════════════════════════════════════════════════════════
// Native tests (share one kernel boot — init→halt is one-way)
// ═══════════════════════════════════════════════════════════════════════════════

async function testNative(imagePath) {
  const m = require_(NATIVE_NODE);
  const rc = m.init(64, 7);
  assert(rc === 0, `init(64,7) = ${rc}`);

  // ── file ──────────────────────────────────────────────────────────────
  console.log(`\n  ${BOLD}[native+file]${RST} ${imagePath}`);
  let h = m.diskOpen(imagePath, 1);
  assert(h >= 0, `diskOpen(file) = ${h}`);
  mountAndReaddir(
    () => m.diskListJson(h),
    (p, f) => m.diskEnter(h, p, f),
    (fs, f) => m.mountWhole(h, fs, f),
    (p) => m.readdirJson(p),
  );
  m.diskClose(h);

  // ── URL ────────────────────────────────────────────────────────────────
  console.log(`\n  ${BOLD}[native+url]${RST} ${imagePath}`);
  let worker;
  try {
    const r = await startHttpWorker(imagePath);
    worker = r.worker;
    const url = `http://127.0.0.1:${r.port}/disk`;
    console.log(`    http server: ${url}`);

    h = m.diskOpen(url, 1);
    assert(h >= 0, `diskOpen(url) = ${h}`);
    mountAndReaddir(
      () => m.diskListJson(h),
      (p, f) => m.diskEnter(h, p, f),
      (fs, f) => m.mountWhole(h, fs, f),
      (p) => m.readdirJson(p),
    );
    m.diskClose(h);
  } finally {
    if (worker) worker.postMessage('stop');
  }

  m.kernelHalt();
}

// ═══════════════════════════════════════════════════════════════════════════════
// WASM tests (each is standalone — factory creates fresh module each time)
// ═══════════════════════════════════════════════════════════════════════════════

async function testWasmFile(imagePath) {
  console.log(`\n${BOLD}[core wasm+file]${RST} ${imagePath}`);

  const nodePath = await import('node:path');
  const imgDir = nodePath.dirname(imagePath);
  const imgName = nodePath.basename(imagePath);

  const { default: factory } = await import(
    new URL(`file://${WASM_DIR}/anyfs.node.mjs`).href
  );
  const M = await factory({
    preRun: [
      (m) => {
        m.FS.mkdir('/work');
        m.FS.mount(m.NODEFS, { root: imgDir }, '/work');
      },
    ],
  });
  pass('WASM module loaded');

  const rc = M.ccall('anyfs_ts_init', 'number', ['number', 'number'], [64, 7]);
  assert(rc === 0, `anyfs_ts_init = ${rc}`);

  const fsPath = `/work/${imgName}`;
  const dk = M.ccall('anyfs_ts_disk_open', 'number', ['string', 'number'], [fsPath, 1]);
  assert(dk >= 0, `diskOpen(${fsPath}) = ${dk}`);

  mountAndReaddir(
    () => callJsonHandle(M, 'anyfs_ts_disk_list_json', dk),
    (p, f) => wasmCallMount(M, 'anyfs_ts_disk_enter', dk, p, f),
    (fs, f) => wasmCallMountWhole(M, 'anyfs_ts_mount_whole', dk, fs, f),
    (p) => callJsonString(M, 'anyfs_ts_readdir_json', p),
  );

  M.ccall('anyfs_ts_disk_close', 'number', ['number'], [dk]);
  M.ccall('anyfs_ts_kernel_halt', 'number', [], []);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Node.js XHR polyfill for URLFS — uses curl for sync HTTP.
// URLFS needs sync XHR (available in Web Workers but not Node), so we shim
// just enough to support HEAD + Range GET.
// ═══════════════════════════════════════════════════════════════════════════════

async function polyfillXHR() {
  if (typeof XMLHttpRequest !== 'undefined' && XMLHttpRequest.toString().includes('native'))
    return; // browser — real XHR already present

  const { spawnSync } = await import('node:child_process');

  class NodeXHR {
    _method = 'GET';
    _url = '';
    _async = true;
    _reqHeaders = {};
    _respHeaders = {};
    _status = 0;
    _responseData = null;
    _responseType = '';

    get status() { return this._status; }
    get statusText() { return String(this._status); }
    get response() { return this._responseData; }
    set responseType(v) { this._responseType = v; }
    get responseType() { return this._responseType; }

    open(method, url, async) {
      this._method = method;
      this._url = url;
      this._async = async;
    }
    setRequestHeader(k, v) { this._reqHeaders[k] = v; }
    getResponseHeader(k) {
      return this._respHeaders[k.toLowerCase()] || null;
    }

    send() {
      if (this._async) throw new Error('async XHR not implemented in Node polyfill (URLFS needs sync)');
      const args = ['-s', '-S', '-L', '--connect-timeout', '5', '--max-time', '30'];
      if (this._method === 'HEAD') args.push('-I');
      else args.push('-X', this._method);
      for (const [k, v] of Object.entries(this._reqHeaders))
        args.push('-H', `${k}: ${v}`);
      // -D - writes headers to stdout; -o FILE writes body to file
      args.push('-D', '-');
      const tmpfile = `/tmp/xhr-node-${Date.now()}-${Math.random().toString(36).slice(2)}`;
      if (this._method !== 'HEAD') args.push('-o', tmpfile);
      args.push(this._url);

      try {
        const result = spawnSync('curl', args, {
          maxBuffer: 10 * 1024 * 1024,
          timeout: 35000,
          encoding: 'buffer',
          stdio: ['ignore', 'pipe', 'pipe'],
        });
        const stdout = result.stdout || Buffer.alloc(0);
        // Parse headers from stdout (everything before \r\n\r\n)
        const headerEnd = stdout.indexOf('\r\n\r\n');
        const headerText = headerEnd >= 0 ? stdout.subarray(0, headerEnd).toString() : stdout.toString();
        // Parse status line
        const lines = headerText.split('\r\n');
        const statusMatch = lines[0]?.match(/^HTTP\/\S+\s+(\d+)/);
        this._status = statusMatch ? parseInt(statusMatch[1]) : 0;
        // Parse response headers
        for (const line of lines.slice(1)) {
          const m = line.match(/^([^:]+):\s*(.*)/);
          if (m) this._respHeaders[m[1].toLowerCase()] = m[2];
        }
        // Read body from tmpfile
        if (this._method !== 'HEAD') {
          const { readFileSync, unlinkSync } = require_('node:fs');
          try { this._responseData = new Uint8Array(readFileSync(tmpfile)).buffer; }
          catch { this._responseData = new ArrayBuffer(0); }
          try { unlinkSync(tmpfile); } catch {}
        }
      } catch (err) {
        this._status = 0;
        this._responseData = new ArrayBuffer(0);
      }
    }
  }
  globalThis.XMLHttpRequest = NodeXHR;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Inline URLFS — mirrors ts/packages/core/src/url-fs.ts.
// Defined here so the Node test doesn't need the browser-only dist bundle.
// Uses sync XHR (polyfilled above) for HEAD probe + Range GET chunks.
// ═══════════════════════════════════════════════════════════════════════════════

function createUrlFsNode(M) {
  const FS = M.FS;
  const FILE_MODE = 33279;  // S_IFREG | 0o777
  const DIR_MODE  = 16895;  // S_IFDIR  | 0o777
  const CHUNK_SIZE = 512 * 1024;
  const MAX_CACHED_CHUNKS = 32;
  const ENOENT = 44;
  const EPERM  = 63;
  const EINVAL = 28;

  function probeUrl(url) {
    const xhr = new XMLHttpRequest();
    xhr.open('HEAD', url, false);
    xhr.send();
    if (xhr.status < 200 || xhr.status >= 300)
      throw new Error(`URLFS: HEAD ${url} → HTTP ${xhr.status}`);
    const cl = xhr.getResponseHeader('Content-Length');
    if (!cl) throw new Error('URLFS: server did not return Content-Length');
    const size = parseInt(cl, 10);
    if (!Number.isFinite(size) || size <= 0)
      throw new Error(`URLFS: invalid Content-Length: ${cl}`);
    return { size };
  }

  function fetchChunk(backend, idx) {
    const start = idx * CHUNK_SIZE;
    const end = Math.min(start + CHUNK_SIZE, backend.size) - 1;
    const xhr = new XMLHttpRequest();
    xhr.open('GET', backend.url, false);
    xhr.responseType = 'arraybuffer';
    xhr.setRequestHeader('Range', `bytes=${start}-${end}`);
    xhr.send();
    if (xhr.status !== 206 && xhr.status !== 200)
      throw new Error(`URLFS: range ${start}-${end} → HTTP ${xhr.status}`);
    let buf = new Uint8Array(xhr.response);
    if (xhr.status === 200 && buf.length > end - start + 1)
      buf = buf.subarray(start, end + 1);
    return buf;
  }

  function getChunk(backend, idx) {
    const hit = backend.cache.get(idx);
    if (hit) {
      backend.cache.delete(idx);
      backend.cache.set(idx, hit);
      return hit;
    }
    const buf = fetchChunk(backend, idx);
    backend.cache.set(idx, buf);
    if (backend.cache.size > MAX_CACHED_CHUNKS) {
      const oldest = backend.cache.keys().next().value;
      if (oldest !== undefined) backend.cache.delete(oldest);
    }
    return buf;
  }

  const NODE_OPS = {
    getattr(node) {
      return {
        dev: 1, ino: node.id, mode: node.mode, nlink: 1,
        uid: 0, gid: 0, rdev: 0, size: node.size,
        atime: new Date(node.atime), mtime: new Date(node.mtime),
        ctime: new Date(node.ctime), blksize: 4096,
        blocks: Math.ceil(node.size / 4096),
      };
    },
    setattr(node, attr) {
      for (const k of ['mode','atime','mtime','ctime'])
        if (attr[k] != null) node[k] = attr[k];
    },
    lookup() { throw new FS.ErrnoError(ENOENT); },
    mknod()  { throw new FS.ErrnoError(EPERM); },
    rename() { throw new FS.ErrnoError(EPERM); },
    unlink() { throw new FS.ErrnoError(EPERM); },
    rmdir()  { throw new FS.ErrnoError(EPERM); },
    readdir(node) {
      return ['.', '..', ...Object.keys(node.contents)];
    },
    symlink() { throw new FS.ErrnoError(EPERM); },
  };

  const STREAM_OPS = {
    read(stream, buffer, offset, length, position) {
      const backend = stream.node.contents;
      if (position >= backend.size) return 0;
      const end = Math.min(position + length, backend.size);
      let copied = 0, pos = position;
      while (pos < end) {
        const idx = Math.floor(pos / CHUNK_SIZE);
        const chunkStart = idx * CHUNK_SIZE;
        const chunk = getChunk(backend, idx);
        const inChunkOff = pos - chunkStart;
        const want = Math.min(end - pos, chunk.length - inChunkOff);
        buffer.set(chunk.subarray(inChunkOff, inChunkOff + want), offset + copied);
        copied += want; pos += want;
      }
      return copied;
    },
    write() { throw new FS.ErrnoError(29 /* ESPIPE */); },
    llseek(stream, offset, whence) {
      let pos = offset;
      if (whence === 1) pos += stream.position;
      else if (whence === 2 && FS.isFile(stream.node.mode)) pos += stream.node.size;
      if (pos < 0) throw new FS.ErrnoError(EINVAL);
      return pos;
    },
  };

  return {
    mount(mount) {
      const opts = mount.opts;
      const { size } = probeUrl(opts.url);
      const backend = { url: opts.url, size, cache: new Map() };
      const root = FS.createNode(null, '/', DIR_MODE, 0);
      root.mode = DIR_MODE;
      root.node_ops = NODE_OPS;
      root.stream_ops = STREAM_OPS;
      root.atime = root.mtime = root.ctime = Date.now();
      root.size = 4096;
      root.contents = {};
      const fnode = FS.createNode(root, opts.name, FILE_MODE);
      fnode.mode = FILE_MODE;
      fnode.node_ops = NODE_OPS;
      fnode.stream_ops = STREAM_OPS;
      fnode.atime = fnode.mtime = fnode.ctime = Date.now();
      fnode.size = size;
      fnode.contents = backend;
      root.contents[opts.name] = fnode;
      return root;
    },
  };
}

// ═══════════════════════════════════════════════════════════════════════════════

async function testWasmUrl(imagePath) {
  console.log(`\n${BOLD}[core wasm+url]${RST} ${imagePath}`);

  // Polyfill XHR before loading the WASM module (URLFS uses sync XHR).
  await polyfillXHR();

  const { worker, port } = await startHttpWorker(imagePath);
  const url = `http://127.0.0.1:${port}/disk`;
  console.log(`    http server: ${url}`);

  try {
    // Use the non-QEMU wasm build — URLFS replaces QEMU curl entirely.
    // The kernel reads a "local file" backed by sync HTTP Range requests.
    const { default: factory } = await import(
      new URL(`file://${WASM_DIR}/anyfs.node.mjs`).href
    );
    const M = await factory();
    pass('WASM module loaded');

    const rc = M.ccall('anyfs_ts_init', 'number', ['number', 'number'], [64, 7]);
    assert(rc === 0, `anyfs_ts_init = ${rc}`);

    // Mount URLFS at /work — the kernel sees /work/disk as a regular file.
    const URLFS = createUrlFsNode(M);
    M.FS.mkdir('/work');
    M.FS.mount(URLFS, { url, name: 'disk' }, '/work');

    const dk = M.ccall('anyfs_ts_disk_open', 'number', ['string', 'number'], ['/work/disk', 1]);
    assert(dk >= 0, `diskOpen(/work/disk) = ${dk}`);

    mountAndReaddir(
      () => callJsonHandle(M, 'anyfs_ts_disk_list_json', dk),
      (p, f) => wasmCallMount(M, 'anyfs_ts_disk_enter', dk, p, f),
      (fs, f) => wasmCallMountWhole(M, 'anyfs_ts_mount_whole', dk, fs, f),
      (p) => callJsonString(M, 'anyfs_ts_readdir_json', p),
    );

    M.ccall('anyfs_ts_disk_close', 'number', ['number'], [dk]);
    M.ccall('anyfs_ts_kernel_halt', 'number', [], []);
  } finally {
    worker.postMessage('stop');
  }
}

// ═══════════════════════════════════════════════════════════════════════════════

async function main() {
  function cliOpt(name, def) {
    const eq = process.argv.find(a => a.startsWith(`${name}=`));
    if (eq) return eq.split('=')[1];
    const idx = process.argv.indexOf(name);
    if (idx >= 0 && idx + 1 < process.argv.length && !process.argv[idx + 1].startsWith('-'))
      return process.argv[idx + 1];
    return def;
  }
  const only = cliOpt('--only', 'all');
  const imagePath = findImage();
  console.log(`${BOLD}[test-core]${RST} disk: ${imagePath}`);

  if (only === 'all' || only === 'native') {
    try { await testNative(imagePath); }
    catch (e) { fail(`native: ${e.message}`); console.error('  stack:', e.stack?.split('\n').slice(0, 3).join('\n       ')); }
  }

  if (only === 'all' || only === 'wasm') {
    try { await testWasmFile(imagePath); }
    catch (e) { fail(`wasm+file: ${e.message}`); console.error('  stack:', e.stack?.split('\n').slice(0, 3).join('\n       ')); }

    try { await testWasmUrl(imagePath); }
    catch (e) { fail(`wasm+url: ${e.message}`); console.error('  stack:', e.stack?.split('\n').slice(0, 3).join('\n       ')); }
  }

  console.log(`\n${BOLD}[test-core]${RST} ${GREEN}${passed} pass${RST}, ${failed > 0 ? RED : ''}${failed} fail${RST}`);
  if (failed > 0) process.exit(1);
}

main();

# NBD-over-inherited-fd Transport PoC — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Prove that a native QEMU child can open a qcow2 disk image served over an NBD protocol channel carried on an *inherited socketpair fd* (no TCP listen), with format drivers auto-layered atop NBD.

**Architecture:** A Node parent creates a socketpair via a small N-API addon, runs a hand-written read-only NBD server on one end, and spawns the `anyfs-lspart` C binary with the other end inherited (non-CLOEXEC) and passed as `--nbd-fd N`. lspart turns `nbd-fd:N` into a QEMU `blk_new_open` with `server.type=fd, server.str="N"`; QEMU's no-monitor `socket_get_fd` adopts the raw fd via `qio_channel_socket_new_fd`, so **zero QEMU source changes** are needed. Windows falls back to `127.0.0.1` loopback.

**Tech Stack:** C (anyfs core + lspart, meson/ninja), QEMU libblock (prebuilt), Node 24 (`net`/`fs`, ES modules), node-addon-api v8 (N-API addon), `qemu-img` for cross-validation.

**Spec:** `docs/superpowers/specs/2026-05-31-nbd-fd-transport-poc-design.md`

---

## Reference facts (verified — do not re-investigate)

- `anyfs-lspart` source: `src/lspart/lspart_main.c` (125 lines). It opens images with
  `anyfs_session_open(path, ANYFS_SESSION_READONLY, &d)` (line ~95). `image_path` flows
  opaquely through `anyfs_session_open` → `anyfs_disk_add` → `qemu_backend_ops.open`
  → `qemu_blk_open` → `blk_new_open`. **No intermediate layer parses the path string.**
- Flags (`include/anyfs.h`): `ANYFS_SESSION_READONLY = 1u<<0`, `ANYFS_BACKEND_RAW =
  1u<<1`, `ANYFS_BACKEND_QEMU = 1u<<3`, `ANYFS_SESSION_SNAPSHOT = 1u<<4`.
- QEMU backend auto-selects when `ANYFS_HAS_QEMU` is defined (it is, in the linux build:
  `meson.build:242 qemu_args = ['-DANYFS_HAS_QEMU=1']`). So just passing `nbd-fd:N` as
  the image path reaches `qemu_blk_open`.
- `src/core/qemu_backend.c` already `#include "qobject/qdict.h"` and uses
  `qdict_new()` / `qdict_put_str()` / `qdict_put_int()`. The existing `is_url` branch is
  at lines ~175-184; the new `nbd-fd` branch slots in beside it.
- QEMU NBD QDict keys (`block/nbd.c:nbd_config` extracts `server.` subdict and visits it
  as a `SocketAddress`; `FdSocketAddress` has `type` + `str`):
  **`server.type = "fd"`, `server.str = "<N>"`.**
- QEMU's `util/qemu-sockets.c:socket_get_fd()` accepts a bare numeric fd when
  `monitor_cur() == NULL` (our case), then requires `fd_is_socket(fd)`.
- **CONFIRMED IN STAGE 1:** `qemu_blk_open` must call `module_call_init(MODULE_INIT_QOM)`
  (needs `#include "qemu/module.h"`) before `bdrv_init()`. The NBD client adopts the fd
  via a `qio-channel-socket`, a QOM type registered under MODULE_INIT_QOM; `bdrv_init()`
  alone (MODULE_INIT_BLOCK) leaves it unregistered → `unknown type 'qio-channel-socket'`.
  QEMU's own tools call both. This is a latent anyfs-glue init gap, NOT a QEMU source
  change — fix lives in `src/core/qemu_backend.c`, guarded by the one-time
  `qemu_initialized` flag, harmless for plain-file/URL opens. (committed in e183b91)
- Build: `scripts/build_anyfs.sh` runs `meson setup build-anyfs-linux-amd64` + ninja.
  lspart target: `src/lspart/meson.build` → executable `anyfs-lspart`.
- Tooling present: `qemu-img`, `node v24.15.0`, `meson`, `ninja`. **`nbdinfo` is NOT
  installed** — cross-validation uses `qemu-img info nbd:...` instead.
- `node-addon-api` ^8 is already a dependency in `ts/packages/anyfs-native`; its
  `binding.gyp` is the skeleton to mirror for the transport addon.
- Existing test image: `tests/images/ext4.img` (raw, 32 MiB). The PoC will create a
  qcow2 test image from it.

## File structure

| File | New/Mod | Responsibility |
|---|---|---|
| `src/core/qemu_backend.c` | Modify | Parse `nbd-fd:N` / `nbd-port:P` image paths → build `server.*` QDict → `blk_new_open` |
| `src/lspart/lspart_main.c` | Modify | Accept `--nbd-fd N` / `--nbd-port P`, synthesize `nbd-fd:N` pseudo-path |
| `scripts/poc-nbd/native-transport/binding.gyp` | New | gyp build for the transport addon |
| `scripts/poc-nbd/native-transport/transport.c` | New | N-API: `socketpair()` → `[fd0,fd1]`; reserved Windows loopback slot |
| `scripts/poc-nbd/native-transport/package.json` | New | addon package + build script |
| `scripts/poc-nbd/nbd-server.mjs` | New | Hand-written read-only NBD newstyle server over a given fd |
| `scripts/poc-nbd/launch.mjs` | New | Parent: socketpair → spawn lspart (inherit fd) → run server; lifecycle binding |
| `scripts/poc-nbd/make-test-image.mjs` | New | Build a deterministic qcow2 test image + record verification offsets |
| `scripts/poc-nbd/test-stage1.mjs` | New | Stage 1: capacity-match, zero-source-change probe |
| `scripts/poc-nbd/test-stage2.mjs` | New | Stage 2: full Linux chain (partition list + byte verify + READ count) |
| `scripts/poc-nbd/FINDINGS.md` | New | Stage 3 (Windows/wine) findings log |

---

## Task 1: Add the `nbd-fd` / `nbd-port` branch to qemu_backend

**Files:**
- Modify: `src/core/qemu_backend.c` (the `qemu_blk_open` options block, ~lines 172-184)

The existing code builds `options` only for URLs. We add: if `image_path` starts with
`nbd-fd:` or `nbd-port:`, build a `server.*` QDict and pass an empty filename to
`blk_new_open` (QEMU reads the server address from `options`, not from the filename).

- [ ] **Step 1: Read the current options block to anchor the edit**

Run: `sed -n '170,190p' src/core/qemu_backend.c`
Expected: shows the `is_url` block creating `options = qdict_new()` and the
`blk_new_open(image_path, NULL, options, bdrv_flags, &errp)` call.

- [ ] **Step 2: Replace the options block with nbd-fd / nbd-port handling**

Find this block (around lines 172-184):

```c
	/* QEMU's curl block driver defaults to CURLOPT_TIMEOUT=5s. Bump it for
	 * URL images (local files use the raw driver, which ignores "timeout").
	 */
	QDict* options = NULL;
	const int is_url = image_path && strstr(image_path, "://") != NULL;
	if (is_url) {
		options = qdict_new();
		qdict_put_int(options, "file.timeout", 20);
	}

	fprintf(stderr, "[qemu_blk] blk_new_open flags=0x%x timeout=%d\n",
		bdrv_flags, is_url ? 20 : 0);
	BlockBackend* blk =
	    blk_new_open(image_path, NULL, options, bdrv_flags, &errp);
```

Replace it with:

```c
	/* QEMU's curl block driver defaults to CURLOPT_TIMEOUT=5s. Bump it for
	 * URL images (local files use the raw driver, which ignores "timeout").
	 */
	QDict* options = NULL;
	const int is_url = image_path && strstr(image_path, "://") != NULL;

	/* PoC: NBD-over-inherited-fd transport. image_path "nbd-fd:N" means the
	 * NBD client should adopt already-open socket fd N (passed by the parent
	 * via inheritance). "nbd-port:P" is the Windows fallback: connect to a
	 * 127.0.0.1 loopback on port P. In both cases the server address comes
	 * from the options QDict (server.*), so the filename handed to
	 * blk_new_open must be NULL. */
	const char* open_name = image_path;
	if (image_path && strncmp(image_path, "nbd-fd:", 7) == 0) {
		options = qdict_new();
		qdict_put_str(options, "driver", "nbd");
		qdict_put_str(options, "server.type", "fd");
		qdict_put_str(options, "server.str", image_path + 7);
		open_name = NULL;
	} else if (image_path && strncmp(image_path, "nbd-port:", 9) == 0) {
		options = qdict_new();
		qdict_put_str(options, "driver", "nbd");
		qdict_put_str(options, "server.type", "inet");
		qdict_put_str(options, "server.host", "127.0.0.1");
		qdict_put_str(options, "server.port", image_path + 9);
		open_name = NULL;
	} else if (is_url) {
		options = qdict_new();
		qdict_put_int(options, "file.timeout", 20);
	}

	fprintf(stderr, "[qemu_blk] blk_new_open name=%s flags=0x%x\n",
		open_name ? open_name : "(nbd via options)", bdrv_flags);
	BlockBackend* blk =
	    blk_new_open(open_name, NULL, options, bdrv_flags, &errp);
```

- [ ] **Step 3: Build lspart and confirm it compiles**

Run: `bash scripts/build_anyfs.sh --target linux-amd64 lspart 2>&1 | tail -20`
(If `--target`/component syntax differs, fall back to:
`cd ~/anyfs-reader && meson setup build-anyfs-linux-amd64 2>/dev/null; ninja -C build-anyfs-linux-amd64 anyfs-lspart`)
Expected: compiles with no errors; `build-anyfs-linux-amd64/anyfs-lspart` exists.

- [ ] **Step 4: Confirm existing plain-file path still works (regression)**

Run: `build-anyfs-linux-amd64/anyfs-lspart tests/images/ext4.img`
Expected: prints a partition/disk table (the raw ext4 image lists as a whole-disk
filesystem). No crash. This proves the new branch didn't break the default path.

- [ ] **Step 5: Commit**

```bash
git add src/core/qemu_backend.c
git commit -m "feat(qemu_backend): add nbd-fd / nbd-port image-path branch for PoC

nbd-fd:N builds a server.type=fd QDict so QEMU's NBD client adopts an
inherited socket fd; nbd-port:P is the Windows 127.0.0.1 loopback
fallback. Filename to blk_new_open is NULL in both cases (server comes
from options). Zero QEMU source changes.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 2: Add `--nbd-fd` / `--nbd-port` flags to lspart

**Files:**
- Modify: `src/lspart/lspart_main.c`

Add flag parsing that synthesizes an `nbd-fd:N` (or `nbd-port:P`) pseudo-path and pushes
it into the same `images[]` array the existing loop opens. This keeps the open/format
path identical — only the path string differs.

- [ ] **Step 1: Add a small synthesized-path buffer near the top of `main`**

After the existing declarations in `main` (the `const char* images[16]; int n_images =
0;` block), the synthesized path needs storage that outlives the parse loop. Add a
static buffer just below them:

```c
	int json = 0;
	const char* images[16];
	int n_images = 0;
	static char nbd_path[32]; /* holds a synthesized "nbd-fd:N"/"nbd-port:P" */
```

- [ ] **Step 2: Add flag handling inside the argv loop**

In the `for (int i = 1; i < argc; i++)` loop, after the `--json` handler and before the
`if (a[0] == '-' && a[1] != '\0')` unknown-flag check, insert:

```c
		if (strcmp(a, "--nbd-fd") == 0 || strcmp(a, "--nbd-port") == 0) {
			if (i + 1 >= argc) {
				fprintf(stderr, "%s needs an argument\n", a);
				return 2;
			}
			const char* kind = (a[6] == 'f') ? "nbd-fd" : "nbd-port";
			snprintf(nbd_path, sizeof(nbd_path), "%s:%s", kind,
				 argv[++i]);
			if (n_images >= (int)(sizeof(images) /
					      sizeof(images[0]))) {
				fprintf(stderr, "too many images\n");
				return 2;
			}
			images[n_images++] = nbd_path;
			continue;
		}
```

- [ ] **Step 3: Update the usage string to mention the new flags**

In `usage()`, change the first `fprintf` format line from:

```c
		"Usage: %s [--json] [--help] <image>[?<query>] [<image>...]\n"
```

to:

```c
		"Usage: %s [--json] [--help] [--nbd-fd N | --nbd-port P] "
		"<image>[?<query>] [<image>...]\n"
```

- [ ] **Step 4: Rebuild lspart**

Run: `ninja -C build-anyfs-linux-amd64 anyfs-lspart 2>&1 | tail -10`
Expected: compiles cleanly; binary updated.

- [ ] **Step 5: Confirm the flag is accepted (will fail to open — fd N invalid — but must parse)**

Run: `build-anyfs-linux-amd64/anyfs-lspart --nbd-fd 999 2>&1 | head -5`
Expected: it attempts to open `nbd-fd:999` and fails with a QEMU "not a socket" / open
error (fd 999 isn't a valid socket). The point is that **parsing succeeds** and it
reaches `qemu_blk_open` — not an "unknown flag" error.

- [ ] **Step 6: Commit**

```bash
git add src/lspart/lspart_main.c
git commit -m "feat(lspart): accept --nbd-fd N / --nbd-port P flags

Synthesizes an nbd-fd:N (or nbd-port:P) pseudo-path and feeds it through
the existing anyfs_session_open path unchanged.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 3: Native transport addon (socketpair)

**Files:**
- Create: `scripts/poc-nbd/native-transport/transport.c`
- Create: `scripts/poc-nbd/native-transport/binding.gyp`
- Create: `scripts/poc-nbd/native-transport/package.json`

A minimal N-API addon exporting `socketpair()` returning `[fd0, fd1]` (two bare ints).
This is the seed of the unified Node-layer native transport abstraction.

- [ ] **Step 1: Write the addon C source**

Create `scripts/poc-nbd/native-transport/transport.c`:

```c
/*
 * native-transport — minimal N-API addon for the NBD-over-fd PoC.
 *
 * Exports socketpair() -> [fd0, fd1]: a connected AF_UNIX/SOCK_STREAM
 * pair. The parent keeps one fd for the NBD server and lets the child
 * inherit the other (non-CLOEXEC) to use as QEMU's NBD transport.
 *
 * This is the seed of a unified Node-layer native transport abstraction;
 * a Windows loopback-pair helper will be added here later.
 */
#define NAPI_VERSION 8
#include <node_api.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>

static napi_value Socketpair(napi_env env, napi_callback_info info)
{
	int fds[2];
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
		napi_throw_error(env, NULL, "socketpair() failed");
		return NULL;
	}
	/* Clear CLOEXEC on both ends so the child can inherit fds[1].
	 * (socketpair does not set CLOEXEC by default on Linux, but be
	 * explicit so the contract is obvious.) */
	for (int i = 0; i < 2; i++) {
		int fl = fcntl(fds[i], F_GETFD);
		if (fl >= 0)
			fcntl(fds[i], F_SETFD, fl & ~FD_CLOEXEC);
	}

	napi_value arr, a, b;
	napi_create_array_with_length(env, 2, &arr);
	napi_create_int32(env, fds[0], &a);
	napi_create_int32(env, fds[1], &b);
	napi_set_element(env, arr, 0, a);
	napi_set_element(env, arr, 1, b);
	return arr;
}

static napi_value Init(napi_env env, napi_value exports)
{
	napi_value fn;
	napi_create_function(env, "socketpair", NAPI_AUTO_LENGTH, Socketpair,
			     NULL, &fn);
	napi_set_named_property(env, exports, "socketpair", fn);
	return exports;
}

NAPI_MODULE(NODE_GYP_MODULE_NAME, Init)
```

- [ ] **Step 2: Write binding.gyp**

Create `scripts/poc-nbd/native-transport/binding.gyp`:

```python
{
  "targets": [
    {
      "target_name": "native_transport",
      "sources": ["transport.c"],
      "cflags_c": ["-D_GNU_SOURCE"]
    }
  ]
}
```

- [ ] **Step 3: Write package.json**

Create `scripts/poc-nbd/native-transport/package.json`:

```json
{
  "name": "native-transport",
  "version": "0.0.0",
  "private": true,
  "description": "PoC N-API addon: socketpair() for NBD-over-fd transport",
  "main": "index.js",
  "scripts": {
    "build": "node-gyp rebuild"
  }
}
```

- [ ] **Step 4: Build the addon**

Run:
```bash
cd ~/anyfs-reader/scripts/poc-nbd/native-transport && npx node-gyp rebuild 2>&1 | tail -15
```
Expected: builds `build/Release/native_transport.node` with no errors.

- [ ] **Step 5: Smoke-test the addon from Node**

Run:
```bash
cd ~/anyfs-reader/scripts/poc-nbd/native-transport && node -e "const t=require('./build/Release/native_transport.node'); const [a,b]=t.socketpair(); console.log('fds', a, b); const fs=require('fs'); fs.writeSync(a, Buffer.from('hi')); const buf=Buffer.alloc(2); fs.readSync(b, buf, 0, 2, null); console.log('roundtrip', buf.toString());"
```
Expected: prints two distinct fd numbers, then `roundtrip hi` — confirming the pair is a
real connected socket.

- [ ] **Step 6: Commit**

```bash
git add scripts/poc-nbd/native-transport/
git commit -m "feat(poc-nbd): native-transport addon exposing socketpair()

Minimal N-API addon returning a connected AF_UNIX socket pair with
CLOEXEC cleared, for inheriting one end into the QEMU child. Seed of the
Node-layer native transport abstraction.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 4: Hand-written read-only NBD server

**Files:**
- Create: `scripts/poc-nbd/nbd-server.mjs`

Implements the NBD newstyle fixed handshake + `NBD_OPT_GO` + a command loop handling
`NBD_CMD_READ` / `NBD_CMD_FLUSH` / `NBD_CMD_DISC`. PoC: synchronous `fs.readSync`, one
request at a time. Operates on a Node `net.Socket` wrapping an existing fd.

Protocol constants (NBD newstyle):
- Handshake: server sends magic `NBDMAGIC` (0x4e42444d41474943) + `IHAVEOPT`
  (0x49484156454F5054) + 16-bit handshake flags `NBD_FLAG_FIXED_NEWSTYLE`(1) |
  `NBD_FLAG_NO_ZEROES`(2) = 3.
- Client replies 32-bit client flags. Then option haggling: client sends `IHAVEOPT` +
  32-bit option + 32-bit len + data.
- We support `NBD_OPT_GO`(7) and `NBD_OPT_INFO`(6) (treat the same), and `NBD_OPT_ABORT`(2).
  For GO we send `NBD_REP_INFO`(3) with `NBD_INFO_EXPORT`(0): export size (u64) + transmission
  flags (u16: `NBD_FLAG_HAS_FLAGS`(1) | `NBD_FLAG_READ_ONLY`(2) = 3), then
  `NBD_REP_ACK`(1). With NO_ZEROES negotiated, transmission phase begins immediately.
- Transmission: client request = magic 0x25609513 + 16-bit cmd flags + 16-bit type +
  64-bit handle + 64-bit offset + 32-bit length. Reply (simple) = magic 0x67446698 +
  32-bit error + 64-bit handle + (data for READ).
- Commands: `NBD_CMD_READ`(0), `NBD_CMD_WRITE`(1), `NBD_CMD_DISC`(2), `NBD_CMD_FLUSH`(3).

- [ ] **Step 1: Write the NBD server module**

Create `scripts/poc-nbd/nbd-server.mjs`:

```javascript
/*
 * Hand-written read-only NBD newstyle server for the NBD-over-fd PoC.
 *
 * PoC scope: synchronous fs.readSync, one request at a time. The
 * production server (spec §9) is fully async with multiple in-flight
 * requests and a keep-alive http upstream — out of scope here.
 *
 * startNbdServer(socket, imageFd, size, onRead?) drives one client on an
 * already-connected socket. Returns a promise that resolves on clean
 * NBD_CMD_DISC / EOF.
 */
import { Buffer } from 'node:buffer';
import fs from 'node:fs';

const NBDMAGIC = 0x4e42444d41474943n;
const IHAVEOPT = 0x49484156454f5054n;
const REQ_MAGIC = 0x25609513;
const SIMPLE_REPLY_MAGIC = 0x67446698;

const NBD_FLAG_FIXED_NEWSTYLE = 1;
const NBD_FLAG_NO_ZEROES = 2;

const NBD_OPT_ABORT = 2;
const NBD_OPT_INFO = 6;
const NBD_OPT_GO = 7;

const NBD_REP_ACK = 1n;
const NBD_REP_INFO = 3n;
const NBD_REP_FLAG_ERROR = 0x80000000n;
const NBD_REP_ERR_UNSUP = NBD_REP_FLAG_ERROR | 1n;

const NBD_INFO_EXPORT = 0;
const NBD_FLAG_HAS_FLAGS = 1;
const NBD_FLAG_READ_ONLY = 2;

const NBD_CMD_READ = 0;
const NBD_CMD_DISC = 2;
const NBD_CMD_FLUSH = 3;

const EINVAL = 22;

/* Read exactly n bytes from a socket, buffering across 'data' events. */
function makeReader(socket) {
  let chunks = [];
  let buffered = 0;
  let want = 0;
  let resolveWant = null;

  socket.on('data', (d) => {
    chunks.push(d);
    buffered += d.length;
    tryResolve();
  });

  function tryResolve() {
    if (resolveWant && buffered >= want) {
      const all = Buffer.concat(chunks);
      const out = all.subarray(0, want);
      const rest = all.subarray(want);
      chunks = rest.length ? [rest] : [];
      buffered = rest.length;
      const r = resolveWant;
      resolveWant = null;
      r(out);
    }
  }

  return (n) =>
    new Promise((resolve, reject) => {
      want = n;
      resolveWant = resolve;
      socket.once('error', reject);
      socket.once('end', () => {
        if (resolveWant) {
          resolveWant = null;
          reject(new Error('EOF'));
        }
      });
      tryResolve();
    });
}

export async function startNbdServer(socket, imageFd, size, onRead) {
  const read = makeReader(socket);
  const write = (buf) =>
    new Promise((res, rej) => socket.write(buf, (e) => (e ? rej(e) : res())));

  /* --- Handshake --- */
  const hello = Buffer.alloc(18);
  hello.writeBigUInt64BE(NBDMAGIC, 0);
  hello.writeBigUInt64BE(IHAVEOPT, 8);
  hello.writeUInt16BE(NBD_FLAG_FIXED_NEWSTYLE | NBD_FLAG_NO_ZEROES, 16);
  await write(hello);

  await read(4); /* client flags (ignored for PoC) */

  /* --- Option haggling: loop until GO/INFO (enter transmission) or ABORT --- */
  for (;;) {
    const optHdr = await read(16);
    const optMagic = optHdr.readBigUInt64BE(0);
    const opt = optHdr.readUInt32BE(8);
    const optLen = optHdr.readUInt32BE(12);
    if (optMagic !== IHAVEOPT) throw new Error('bad option magic');
    const optData = optLen ? await read(optLen) : Buffer.alloc(0);

    if (opt === NBD_OPT_GO || opt === NBD_OPT_INFO) {
      /* NBD_REP_INFO + NBD_INFO_EXPORT */
      const info = Buffer.alloc(12 /* size+flags */);
      info.writeBigUInt64BE(BigInt(size), 0);
      info.writeUInt16BE(NBD_FLAG_HAS_FLAGS | NBD_FLAG_READ_ONLY, 8);
      info.writeUInt16BE(NBD_INFO_EXPORT, 10);
      /* reorder: INFO type (u16) must precede payload */
      const infoReply = Buffer.alloc(2 + 10);
      infoReply.writeUInt16BE(NBD_INFO_EXPORT, 0);
      infoReply.writeBigUInt64BE(BigInt(size), 2);
      infoReply.writeUInt16BE(NBD_FLAG_HAS_FLAGS | NBD_FLAG_READ_ONLY, 10);
      await sendOptReply(write, opt, NBD_REP_INFO, infoReply);
      await sendOptReply(write, opt, NBD_REP_ACK, Buffer.alloc(0));
      if (opt === NBD_OPT_GO) break; /* INFO-only would loop; GO enters xmit */
      void optData;
    } else if (opt === NBD_OPT_ABORT) {
      await sendOptReply(write, opt, NBD_REP_ACK, Buffer.alloc(0));
      socket.end();
      return;
    } else {
      await sendOptReply(write, opt, NBD_REP_ERR_UNSUP, Buffer.alloc(0));
    }
  }

  /* --- Transmission phase --- */
  for (;;) {
    let hdr;
    try {
      hdr = await read(28);
    } catch {
      return; /* EOF / disconnect */
    }
    const magic = hdr.readUInt32BE(0);
    if (magic !== REQ_MAGIC) throw new Error('bad request magic');
    const type = hdr.readUInt16BE(6);
    const handle = hdr.subarray(8, 16); /* opaque 8 bytes */
    const offset = hdr.readBigUInt64BE(16);
    const length = hdr.readUInt32BE(24);

    if (type === NBD_CMD_DISC) return;

    if (type === NBD_CMD_READ) {
      if (offset + BigInt(length) > BigInt(size)) {
        await sendSimpleReply(write, EINVAL, handle, null);
        continue;
      }
      const data = Buffer.alloc(length);
      fs.readSync(imageFd, data, 0, length, Number(offset));
      if (onRead) onRead(Number(offset), length);
      await sendSimpleReply(write, 0, handle, data);
    } else if (type === NBD_CMD_FLUSH) {
      await sendSimpleReply(write, 0, handle, null);
    } else {
      await sendSimpleReply(write, EINVAL, handle, null);
    }
  }
}

async function sendOptReply(write, opt, repType, payload) {
  const hdr = Buffer.alloc(20);
  hdr.writeBigUInt64BE(0x0003e889045565a9n, 0); /* NBD_REP option-reply magic */
  hdr.writeUInt32BE(opt, 8);
  hdr.writeBigUInt64BE(repType, 12); /* 8 bytes: 4 rep type at off 12? */
  /* NBD option reply: magic(8) + opt(4) + reptype(4) + len(4) */
  const fixed = Buffer.alloc(16);
  fixed.writeBigUInt64BE(0x0003e889045565a9n, 0);
  fixed.writeUInt32BE(opt, 8);
  fixed.writeUInt32BE(Number(repType & 0xffffffffn), 12);
  const lenBuf = Buffer.alloc(4);
  lenBuf.writeUInt32BE(payload.length, 0);
  await write(Buffer.concat([fixed, lenBuf, payload]));
}

async function sendSimpleReply(write, error, handle, data) {
  const hdr = Buffer.alloc(16);
  hdr.writeUInt32BE(SIMPLE_REPLY_MAGIC, 0);
  hdr.writeUInt32BE(error >>> 0, 4);
  handle.copy(hdr, 8);
  await write(data ? Buffer.concat([hdr, data]) : hdr);
}
```

> NOTE: the NBD option-reply magic is `0x0003e889045565a9`. The `sendOptReply` helper
> above contains a leftover dead block; clean it to just the `fixed`/`lenBuf`/`payload`
> path in Step 2.

- [ ] **Step 2: Clean up sendOptReply (remove the dead hdr block)**

Replace the `sendOptReply` function body with the correct single-path version:

```javascript
async function sendOptReply(write, opt, repType, payload) {
  /* NBD option reply: magic(8) + opt(4) + reptype(4) + len(4) + payload */
  const fixed = Buffer.alloc(16);
  fixed.writeBigUInt64BE(0x0003e889045565a9n, 0);
  fixed.writeUInt32BE(opt, 8);
  fixed.writeUInt32BE(Number(repType & 0xffffffffn), 12);
  const lenBuf = Buffer.alloc(4);
  lenBuf.writeUInt32BE(payload.length, 0);
  await write(Buffer.concat([fixed, lenBuf, payload]));
}
```

Also remove the now-unused `info` buffer block inside the GO handler (keep only
`infoReply`).

- [ ] **Step 3: Syntax-check the module**

Run: `node --check scripts/poc-nbd/nbd-server.mjs`
Expected: no output (valid syntax). (Behavior is validated by cross-check in Task 6.)

- [ ] **Step 4: Commit**

```bash
git add scripts/poc-nbd/nbd-server.mjs
git commit -m "feat(poc-nbd): hand-written read-only NBD newstyle server

newstyle fixed handshake + NBD_OPT_GO export info + READ/FLUSH/DISC
command loop. PoC: synchronous fs.readSync, one request at a time.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 5: Test-image generator

**Files:**
- Create: `scripts/poc-nbd/make-test-image.mjs`

Produces a deterministic qcow2 with a GPT + one partition, and records a known byte
pattern at a known offset for Stage-2 verification.

- [ ] **Step 1: Write the generator**

> **PLAN DEVIATION (verified 2026-05-31):** the original idea was to convert
> `tests/images/ext4.img` to qcow2 and verify the ext4 superblock magic. That file is
> in fact **all-zeros** (0 nonzero bytes), and `mke2fs`/`mkfs.ext4`/`sgdisk` are **not
> installed**, so there is no real filesystem to anchor on. Instead we **synthesize a raw
> image with a deterministic, self-generated byte pattern** (no fs tooling needed),
> convert it to qcow2 with `qemu-img` (which IS available), and use that pattern as the
> verification anchor. This still proves the real goal — "qcow2-over-NBD serves the
> genuine image content correctly" — and is actually a stronger Stage-2 check because the
> anchor bytes are read back *through* the qcow2-over-NBD chain (see Task 8), not from a
> side control file.

Create `scripts/poc-nbd/make-test-image.mjs`:

```javascript
/*
 * Build a deterministic qcow2 test image for the NBD-over-fd PoC.
 *
 * Strategy: synthesize a raw image filled with a deterministic byte
 * pattern (no filesystem tooling needed), then convert it to qcow2 with
 * qemu-img. We embed a recognizable marker at a known offset and record
 * it as the verification anchor. Stage 2 reads that marker back through
 * the qcow2-over-NBD chain to prove byte-accurate delivery.
 */
import { execFileSync } from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const outDir = path.join(here, 'fixtures');
const rawImg = path.join(outDir, 'test.raw');
const qcow = path.join(outDir, 'test.qcow2');

const SIZE = 8 * 1024 * 1024; /* 8 MiB — small, fast to convert */
const VERIFY_OFFSET = 0x100000; /* 1 MiB in: well past the qcow2 header region */
const MARKER = Buffer.from('ANYFS-NBD-POC-MARKER-v1', 'ascii');

fs.mkdirSync(outDir, { recursive: true });

/* Deterministic fill: byte i = i & 0xff, so every offset has a predictable
 * value. Then stamp the ASCII marker at VERIFY_OFFSET. */
const raw = Buffer.alloc(SIZE);
for (let i = 0; i < SIZE; i++) raw[i] = i & 0xff;
MARKER.copy(raw, VERIFY_OFFSET);
fs.writeFileSync(rawImg, raw);

execFileSync('qemu-img', ['convert', '-f', 'raw', '-O', 'qcow2', rawImg, qcow], {
  stdio: 'inherit',
});

const meta = {
  qcow2: qcow,
  raw: rawImg,
  size: SIZE,
  verifyOffset: VERIFY_OFFSET,
  verifyBytesHex: MARKER.toString('hex'),
  verifyAscii: MARKER.toString('ascii'),
};
fs.writeFileSync(path.join(outDir, 'meta.json'), JSON.stringify(meta, null, 2));
console.log(JSON.stringify(meta, null, 2));
```

- [ ] **Step 2: Run the generator**

Run: `node scripts/poc-nbd/make-test-image.mjs`
Expected: prints JSON with `qcow2` path, `verifyOffset: 1048576`, `verifyAscii:
"ANYFS-NBD-POC-MARKER-v1"`, and the corresponding `verifyBytesHex`. Both
`fixtures/test.raw` and `fixtures/test.qcow2` exist.

- [ ] **Step 3: Sanity-check the qcow2 with qemu-img**

Run: `qemu-img info scripts/poc-nbd/fixtures/test.qcow2`
Expected: `file format: qcow2`, virtual size 8 MiB. Note the qcow2 file-on-disk is much
smaller than 8 MiB (sparse), which is fine — what matters is the virtual size.

- [ ] **Step 4: Commit (fixtures gitignored)**

```bash
echo "fixtures/" > scripts/poc-nbd/.gitignore
git add scripts/poc-nbd/make-test-image.mjs scripts/poc-nbd/.gitignore
git commit -m "feat(poc-nbd): deterministic qcow2 test-image generator

Synthesizes an 8 MiB raw image with a deterministic byte pattern (i&0xff)
plus an ASCII marker at 1 MiB, then converts to qcow2 via qemu-img. The
marker is the Stage-2 byte-verification anchor read back through the
qcow2-over-NBD chain. (ext4.img is all-zeros and no fs tooling is
installed, so we generate our own deterministic content.)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 6: Cross-validate the NBD server with qemu-img (decouple protocol bugs)

**Files:**
- Create: `scripts/poc-nbd/launch.mjs` (the reusable parent; used here and in Tasks 7-8)

Before involving lspart, prove the hand-written server speaks correct NBD by having the
**real QEMU client** (`qemu-img info`) connect to it. Since `qemu-img` needs a real
endpoint, this cross-check uses a Unix-domain socket path (not the inherited fd) — it
only validates protocol correctness, not the fd transport.

- [ ] **Step 1: Write the launcher with both a fd-pair mode and a unix-socket cross-check mode**

Create `scripts/poc-nbd/launch.mjs`:

```javascript
/*
 * Parent/launcher for the NBD-over-fd PoC.
 *
 * Two entry points:
 *   serveOnFd(imagePath): create a socketpair, run the NBD server on fd0,
 *     return fd1 (the inheritable end) + a stop() handle.
 *   serveOnUnixSocket(imagePath, sockPath): listen on a unix socket and
 *     serve one client — used to cross-validate against qemu-img.
 */
import net from 'node:net';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { execFileSync } from 'node:child_process';
import { startNbdServer } from './nbd-server.mjs';

const here = path.dirname(fileURLToPath(import.meta.url));
const addon = require_addon();

function require_addon() {
  const p = path.join(here, 'native-transport/build/Release/native_transport.node');
  return require_cjs(p);
}
function require_cjs(p) {
  /* ES module: use createRequire for the .node addon */
  return createRequireLocal()(p);
}
import { createRequire } from 'node:module';
function createRequireLocal() {
  return createRequire(import.meta.url);
}

function imageSize(imagePath) {
  /* For qcow2 we report the VIRTUAL size (what QEMU's NBD client maps),
   * but our server serves RAW bytes of the file. The NBD export must be
   * the raw file size so QEMU can probe the qcow2 header itself. */
  return fs.statSync(imagePath).size;
}

export function serveOnFd(imagePath) {
  const [fd0, fd1] = addon.socketpair();
  const imageFd = fs.openSync(imagePath, 'r');
  const size = imageSize(imagePath);
  const sock = new net.Socket({ fd: fd0 });
  let reads = 0;
  const done = startNbdServer(sock, imageFd, size, () => {
    reads++;
  }).catch((e) => {
    if (!/EOF/.test(String(e))) console.error('[nbd-server]', e);
  });
  return {
    fd1,
    stop: () => {
      try {
        sock.destroy();
      } catch {}
      try {
        fs.closeSync(imageFd);
      } catch {}
    },
    readCount: () => reads,
    done,
  };
}

export function serveOnUnixSocket(imagePath, sockPath) {
  const imageFd = fs.openSync(imagePath, 'r');
  const size = imageSize(imagePath);
  try {
    fs.unlinkSync(sockPath);
  } catch {}
  const server = net.createServer((sock) => {
    startNbdServer(sock, imageFd, size).catch((e) => {
      if (!/EOF/.test(String(e))) console.error('[nbd-server]', e);
    });
  });
  return new Promise((resolve) => {
    server.listen(sockPath, () => resolve({ server, imageFd }));
  });
}
```

> NOTE: ESM + a `.node` addon needs `createRequire`. Step 2 fixes the import ordering
> (imports must be top-level) — fold the `createRequire` import to the top.

- [ ] **Step 2: Fix the launcher's require/import ordering**

Replace the top of `launch.mjs` (everything from the first import down to the
`require_addon` helpers) with clean top-level imports:

```javascript
import net from 'node:net';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { createRequire } from 'node:module';
import { startNbdServer } from './nbd-server.mjs';

const here = path.dirname(fileURLToPath(import.meta.url));
const require = createRequire(import.meta.url);
const addon = require(
  path.join(here, 'native-transport/build/Release/native_transport.node'),
);
```

Delete the now-redundant `require_addon`, `require_cjs`, `createRequireLocal` helpers and
the mid-file `import { createRequire }` and `import { execFileSync }` lines (execFileSync
isn't used in this file).

- [ ] **Step 3: Write a one-shot cross-check script inline and run it**

Run:
```bash
cd ~/anyfs-reader && node --input-type=module -e "
import { serveOnUnixSocket } from './scripts/poc-nbd/launch.mjs';
import { execFileSync } from 'node:child_process';
const meta = JSON.parse(require('fs').readFileSync('scripts/poc-nbd/fixtures/meta.json'));
const sockPath = '/tmp/poc-nbd-xcheck.sock';
const { server } = await serveOnUnixSocket(meta.qcow2, sockPath);
try {
  const out = execFileSync('qemu-img', ['info', 'nbd+unix:///?socket=' + sockPath], { encoding: 'utf8' });
  console.log(out);
  if (!/file format:\s*qcow2/.test(out)) { console.error('FAIL: qemu-img did not detect qcow2 over NBD'); process.exit(1); }
  console.log('XCHECK PASS: qemu-img detected qcow2 over hand-written NBD server');
} finally { server.close(); }
" 2>&1 | tail -20
```
(Note: `require` isn't defined in `--input-type=module`; if it errors on `require`,
prepend `import { createRequire } from 'node:module'; const require =
createRequire(import.meta.url);` to the `-e` script.)

Expected: `qemu-img info` reports `file format: qcow2` and a 32 MiB virtual size, then
`XCHECK PASS`. **This is the gate that the NBD protocol implementation is correct** —
isolating any later failure to the fd transport.

- [ ] **Step 4: Commit**

```bash
git add scripts/poc-nbd/launch.mjs
git commit -m "feat(poc-nbd): launcher (fd-pair + unix-socket modes) + qemu-img cross-check

serveOnFd creates a socketpair via the addon and runs the server on one
end; serveOnUnixSocket validates protocol correctness against the real
qemu-img NBD client. Cross-check confirmed qcow2 detected over NBD.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 7: Stage 1 — fd passing with zero QEMU source changes

**Files:**
- Create: `scripts/poc-nbd/test-stage1.mjs`

The highest-leverage check: lspart opens `nbd-fd:N` over the inherited socketpair and
reports the **same capacity** as the plain file, proving fd adoption works without QEMU
source changes.

- [ ] **Step 1: Write the Stage-1 test**

Create `scripts/poc-nbd/test-stage1.mjs`:

```javascript
/*
 * Stage 1: prove the inherited-fd NBD transport works with zero QEMU
 * source changes. Parent creates a socketpair, runs the NBD server on
 * one end, spawns lspart with the other end inherited as fd 3, and
 * checks lspart opens the qcow2 (capacity > 0, table printed) identically
 * to a plain-file open.
 */
import { spawn, execFileSync } from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { serveOnFd } from './launch.mjs';

const here = path.dirname(fileURLToPath(import.meta.url));
const repoRoot = path.resolve(here, '../..');
const lspart = path.join(repoRoot, 'build-anyfs-linux-amd64/src/lspart/anyfs-lspart');
const meta = JSON.parse(fs.readFileSync(path.join(here, 'fixtures/meta.json')));

/* Baseline: lspart over the plain qcow2 file (proves what a successful
 * open looks like, independent of NBD). Captured for comparison. */
const plainOut = execFileSync(lspart, [meta.qcow2], { encoding: 'utf8' });

const { fd1, stop, readCount, done } = serveOnFd(meta.qcow2);

/* Child inherits fd1 at descriptor index 3 (stdio[3]). lspart is told
 * --nbd-fd 3. stdio: 0,1,2 are inherit; index 3 = the socketpair end. */
const child = spawn(lspart, ['--nbd-fd', '3'], {
  stdio: ['inherit', 'pipe', 'inherit', fd1],
});

let out = '';
child.stdout.on('data', (d) => (out += d));

const code = await new Promise((resolve) => child.on('exit', resolve));
stop();
await done;

console.log('--- plain-file lspart (baseline) ---');
console.log(plainOut);
console.log('--- nbd lspart output ---');
console.log(out);
console.log(`--- exit=${code} nbd_reads=${readCount()} ---`);

/* The synthetic image has no partition table / filesystem (it is a
 * deterministic byte pattern), so lspart lists it as a single whole-disk
 * row with FSTYPE '?'. The Stage-1 signal is therefore NOT a specific
 * filesystem string — it is: (a) lspart exits 0, (b) the NBD server
 * actually served reads (data traversed the socketpair), and (c) the NBD
 * run produced the SAME table as the plain-file open (i.e. the disk was
 * detected identically whether opened directly or over NBD). */
function dataRows(s) {
  return s
    .split('\n')
    .filter((l) => l.trim() && !/^Usage|warning:|^PATH\b/.test(l)).length;
}

if (code !== 0) {
  console.error('STAGE1 FAIL: lspart exited non-zero');
  process.exit(1);
}
if (readCount() === 0) {
  console.error(
    'STAGE1 FAIL: NBD server served zero reads (data did not traverse the socketpair)',
  );
  process.exit(1);
}
if (dataRows(out) === 0) {
  console.error('STAGE1 FAIL: nbd lspart printed no data row (disk not detected)');
  process.exit(1);
}
if (dataRows(out) !== dataRows(plainOut)) {
  console.error(
    `STAGE1 FAIL: nbd row count ${dataRows(out)} != plain-file row count ${dataRows(plainOut)}`,
  );
  process.exit(1);
}
console.log(
  'STAGE1 PASS: lspart opened qcow2 over inherited-fd NBD; reads traversed the socketpair; table matches plain-file open',
);
```

- [ ] **Step 2: Run Stage 1**

Run: `cd ~/anyfs-reader && node scripts/poc-nbd/test-stage1.mjs 2>&1 | tail -30`
Expected: prints both lspart tables, `exit=0`, `nbd_reads` > 0, equal row counts, and
`STAGE1 PASS`.

- [ ] **Step 3: If it fails with "not a socket" or EBADF, diagnose**

If lspart prints QEMU error `File descriptor '3' is not a socket`: the child didn't get
the socket at fd 3 — verify the `stdio` array index. If `EBADF`: the fd was closed
(CLOEXEC) — confirm the addon cleared CLOEXEC and `net.Socket({fd:fd0})` didn't close
fd1. Record the resolution; do not proceed to Stage 2 until Stage 1 passes.

- [ ] **Step 4: Commit**

```bash
git add scripts/poc-nbd/test-stage1.mjs
git commit -m "test(poc-nbd): Stage 1 — inherited-fd NBD open, zero QEMU source changes

lspart opens nbd-fd:3 over an inherited socketpair end; asserts non-zero
capacity/partition row and that reads actually traversed the socketpair.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 8: Stage 2 — full Linux chain (partition list + byte verify)

**Files:**
- Create: `scripts/poc-nbd/test-stage2.mjs`

Two checks: (1) lspart-over-NBD produces the same table as lspart-over-plain-file, and
(2) the marker bytes are read back **through the qcow2-over-NBD chain** byte-for-byte.
For (2) we use `qemu-io` as a real qcow2 client connected to our NBD server over a unix
socket: `qemu-io` parses the qcow2 (driver=qcow2) whose protocol child is `nbd:` → our
server → the raw marker bytes. This exercises the exact format-over-NBD layering the PoC
is about, and verifies delivered content, not a side control file.

- [ ] **Step 1: Write the Stage-2 test**

Create `scripts/poc-nbd/test-stage2.mjs`:

```javascript
/*
 * Stage 2: full Linux chain.
 *   (a) lspart over inherited-fd NBD matches lspart over the plain file.
 *   (b) the deterministic marker at meta.verifyOffset reads back
 *       byte-for-byte THROUGH the qcow2-over-NBD chain, using qemu-io as
 *       a real qcow2 client over a unix-socket NBD endpoint.
 */
import { spawn, execFileSync, execFile } from 'node:child_process';
import { promisify } from 'node:util';
import net from 'node:net';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { serveOnFd, serveOnUnixSocket } from './launch.mjs';

const execFileP = promisify(execFile);

const here = path.dirname(fileURLToPath(import.meta.url));
const repoRoot = path.resolve(here, '../..');
const lspart = path.join(repoRoot, 'build-anyfs-linux-amd64/src/lspart/anyfs-lspart');
const meta = JSON.parse(fs.readFileSync(path.join(here, 'fixtures/meta.json')));

function dataRows(s) {
  return s
    .split('\n')
    .filter((l) => l.trim() && !/^Usage|warning:|^PATH\b/.test(l)).length;
}

let fail = false;

/* === Check (a): lspart parity over inherited-fd NBD === */
const plainOut = execFileSync(lspart, [meta.qcow2], { encoding: 'utf8' });
const { fd1, stop, readCount, done } = serveOnFd(meta.qcow2);
const child = spawn(lspart, ['--nbd-fd', '3'], {
  stdio: ['inherit', 'pipe', 'inherit', fd1],
});
let nbdOut = '';
child.stdout.on('data', (d) => (nbdOut += d));
const code = await new Promise((r) => child.on('exit', r));
stop();
await done;

console.log('--- plain-file lspart ---\n' + plainOut);
console.log('--- nbd lspart ---\n' + nbdOut);

if (code !== 0) {
  console.error('STAGE2 FAIL: nbd lspart exited non-zero');
  fail = true;
}
if (dataRows(plainOut) !== dataRows(nbdOut)) {
  console.error(
    `STAGE2 FAIL: row count differs (plain=${dataRows(plainOut)} nbd=${dataRows(nbdOut)})`,
  );
  fail = true;
}
if (readCount() === 0) {
  console.error('STAGE2 FAIL: zero NBD reads over inherited fd');
  fail = true;
}

/* === Check (b): byte verify THROUGH qcow2-over-NBD with qemu-io === */
const sockPath = '/tmp/poc-nbd-stage2.sock';
const { server } = await serveOnUnixSocket(meta.qcow2, sockPath);
try {
  /* qemu-io opens the qcow2 format over the nbd: protocol child. We read
   * meta.verifyOffset for the marker length and dump it as hex. */
  const len = Buffer.from(meta.verifyBytesHex, 'hex').length;
  const nbdUri = `json:{"driver":"qcow2","file":{"driver":"nbd","server":{"type":"unix","path":"${sockPath}"}}}`;
  /* MUST be async: the in-process NBD server runs on this event loop, so a
   * synchronous execFileSync would block it and deadlock the qemu-io read. */
  const { stdout: out } = await execFileP(
    'qemu-io',
    ['-r', '-c', `read -v ${meta.verifyOffset} ${len}`, nbdUri],
    { encoding: 'utf8' },
  );
  /* qemu-io 'read -v' prints a hexdump; collect the hex byte columns. */
  const hex = [...out.matchAll(/^[0-9a-f]{8}:\s+((?:[0-9a-f]{2}\s?)+)/gim)]
    .map((m) => m[1].replace(/\s+/g, ''))
    .join('')
    .slice(0, len * 2);
  console.log('--- qemu-io marker read (through qcow2-over-NBD) ---');
  console.log(out);
  if (hex !== meta.verifyBytesHex) {
    console.error(
      `STAGE2 FAIL: marker mismatch through chain: got ${hex} want ${meta.verifyBytesHex}`,
    );
    fail = true;
  }
} catch (e) {
  console.error('STAGE2 FAIL: qemu-io byte verify errored:', e.message);
  fail = true;
} finally {
  server.close();
}

if (fail) process.exit(1);
console.log(
  `STAGE2 PASS: lspart parity (rows=${dataRows(nbdOut)}), inherited-fd reads=${readCount()}, marker verified through qcow2-over-NBD (${meta.verifyAscii})`,
);
```

- [ ] **Step 2: Run Stage 2**

Run: `cd ~/anyfs-reader && node scripts/poc-nbd/test-stage2.mjs 2>&1 | tail -50`
Expected: both lspart tables shown with equal row counts, `reads` > 0, a qemu-io hexdump
showing the ASCII marker, and `STAGE2 PASS`. If the qemu-io hexdump regex doesn't match
the installed qemu-io output format, inspect the printed hexdump and adjust the regex (the
intent: extract the hex bytes of the first `len` bytes at the offset).

- [ ] **Step 3: Commit**

```bash
git add scripts/poc-nbd/test-stage2.mjs
git commit -m "test(poc-nbd): Stage 2 — full Linux chain parity + through-chain byte verify

Asserts nbd lspart's table equals plain-file lspart and non-zero
inherited-fd reads, then verifies the deterministic marker reads back
byte-for-byte through the qcow2-over-NBD chain via qemu-io.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 9: Stage 3 — Windows/wine scouting (go/no-go, allowed to fail)

**Files:**
- Create: `scripts/poc-nbd/FINDINGS.md`

This stage is a probe. The Linux conclusion does not depend on it. We document whether
the mingw64 lspart can open the qcow2 over the `127.0.0.1` loopback fallback under wine.

- [ ] **Step 1: Check whether a mingw64 lspart and wine exist**

Run:
```bash
ls ~/anyfs-reader/build-anyfs-mingw64/src/lspart/anyfs-lspart.exe 2>/dev/null && echo "mingw lspart present" || echo "no mingw lspart"
which wine 2>/dev/null && wine --version 2>/dev/null || echo "no wine"
```
Expected: records availability. If either is missing, Stage 3 is **blocked** (not
failed) — note it in FINDINGS.md and stop.

- [ ] **Step 2: Write FINDINGS.md documenting the Stage-3 probe and current status**

Create `scripts/poc-nbd/FINDINGS.md`:

```markdown
# NBD-over-fd PoC — Findings

## Stage 1 (Linux, inherited-fd) — <PASS/FAIL>
<result + nbd_reads count>

## Stage 2 (Linux, full chain) — <PASS/FAIL>
<row-count parity + magic verify result>

## Stage 3 (Windows/wine, loopback fallback) — <PASS/FAIL/BLOCKED>

Approach: Windows has no reliable socketpair and QEMU requires the NBD fd
to be a socket; PoC uses the `nbd-port:P` branch (127.0.0.1 loopback,
random port, no outward listen).

- mingw64 anyfs-lspart.exe present: <yes/no>
- wine present: <yes/no + version>
- Result: <opened qcow2 over loopback? partition list parity? or the
  blocking reason>

### Notes / blocking issues
<anything that blocks; per repo rule, High-severity blockers get a fix or
an explicit deferral here>
```

Fill the Stage-1 / Stage-2 lines from the actual results of Tasks 7-8.

- [ ] **Step 3: If mingw lspart + wine are available, run the loopback probe**

Only if Step 1 found both. Run a loopback variant (the launcher's TCP path is the
Windows production transport; for the wine probe we listen on `127.0.0.1:0`, get the
port, and pass `--nbd-port`):

```bash
cd ~/anyfs-reader && node --input-type=module -e "
import net from 'node:net';
import fs from 'node:fs';
import { startNbdServer } from './scripts/poc-nbd/nbd-server.mjs';
import { execFileSync } from 'node:child_process';
const meta = JSON.parse(fs.readFileSync('scripts/poc-nbd/fixtures/meta.json'));
const imageFd = fs.openSync(meta.qcow2, 'r');
const size = fs.statSync(meta.qcow2).size;
const srv = net.createServer((s) => startNbdServer(s, imageFd, size).catch(()=>{}));
await new Promise((r) => srv.listen(0, '127.0.0.1', r));
const port = srv.address().port;
try {
  const out = execFileSync('wine', ['build-anyfs-mingw64/src/lspart/anyfs-lspart.exe', '--nbd-port', String(port)], { encoding: 'utf8' });
  console.log(out);
  console.log('STAGE3 PASS (wine loopback)');
} catch (e) { console.error('STAGE3 result:', e.message); }
finally { srv.close(); }
" 2>&1 | tail -30
```
Expected: either a partition table + `STAGE3 PASS`, or a recorded failure reason. Update
FINDINGS.md Stage-3 line accordingly. **Failure here does not fail the PoC.**

- [ ] **Step 4: Commit findings**

```bash
git add scripts/poc-nbd/FINDINGS.md
git commit -m "docs(poc-nbd): Stage-3 wine/loopback findings + PoC results summary

Records Stage 1/2 Linux results and the Windows/wine loopback probe
outcome (go/no-go; allowed to be BLOCKED without failing the PoC).

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Done criteria

The PoC is complete when:
- Task 6 cross-check passes (NBD protocol correct: `qemu-img` detects qcow2 over the
  hand-written server).
- Task 7 Stage 1 passes (lspart opens `nbd-fd:N` over an inherited socketpair, reads
  traverse it, zero QEMU source changes).
- Task 8 Stage 2 passes (NBD lspart matches plain-file lspart; byte anchor verified).
- Task 9 Stage 3 is recorded in FINDINGS.md (PASS or BLOCKED — not a gate).
- Existing plain-file lspart usage still works (Task 1 Step 4).

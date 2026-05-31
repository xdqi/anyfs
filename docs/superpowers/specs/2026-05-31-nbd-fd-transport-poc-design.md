# NBD-over-inherited-fd Transport — PoC Design

**Date:** 2026-05-31
**Status:** Design approved, pre-implementation
**Scope:** Proof-of-concept only (native, Linux-first)

## 1. Motivation

The long-term goal is to run the QEMU block engine in a **separate, high-privilege
process** so anyfs can read sources that need elevated access (privileged files,
raw physical disks) and so the engine's QEMU `AioContext` stops tangling with the
Electron/Chromium main loop (the deferred hang documented in the
`native-needs-utilityProcess` project memory).

The transport between the parent (Node/Electron) and the QEMU child is deliberately
**not** a listening TCP port. Instead:

- The parent creates a connected socket pair and hands **one end's fd** to the QEMU
  child via process inheritance + a command-line argument.
- QEMU's NBD client driver adopts that fd as its transport channel.
- The Node parent speaks the NBD protocol (read-only subset) over its end, serving
  bytes from whatever privileged data source it has.

This buys two properties that justify the "twisted" transport over plain TCP:

1. **Parent/child lifecycle binding** (primary): when either side dies, the socket
   pair hits EOF and the other side tears down cleanly — no dangling connections,
   no leaked ports, no third party able to connect. This is the strong motivation.
2. **No port occupied/exposed** (secondary): nothing is bound to a listening port
   on the machine; the channel is private to the process pair.

### NBD wraps an image *file*, not a block device

QEMU's `block/nbd.c` registers NBD as a **protocol driver**
(`.protocol_name = "nbd"`), not a format driver. QEMU's automatic format probe
therefore layers a format driver *on top of* the NBD protocol channel:

```
qcow2/vmdk/vhdx/...   (format, auto-probed from the first bytes)
        |
   nbd: (protocol, our inherited-fd channel)
```

So `blk_new_open` over an NBD channel that serves a qcow2 byte stream yields a
fully-parsed qcow2 disk. The NBD layer only needs to serve **reads** (read-only +
future `BDRV_O_SNAPSHOT` overlay for writes), so the server implements a tiny
command subset.

## 2. Key verified facts (foundation of the approach)

These were confirmed by reading the QEMU tree (`~/qemu`) and anyfs source:

- **`block/nbd.c` talks only through `s->ioc` (a `QIOChannel *`)** — it never touches
  a raw socket fd directly. Any `QIOChannel` subclass can back it.
- **QEMU already ships a `SOCKET_ADDRESS_TYPE_FD`** SocketAddress type, and
  `util/qemu-sockets.c:socket_get_fd()` accepts a **bare numeric fd** when there is
  **no monitor** (`monitor_cur() == NULL`, which is exactly our library/`blk_new_open`
  case): it does `qemu_strtoi(fdstr)` then `fd_is_socket(fd)`.
- **`qio_channel_socket_new_fd(fd)`** (`io/channel-socket.c:150`) adopts an existing
  fd as a channel, skipping listen/connect. It calls `getpeername`/`getsockname`, so
  **the fd must be a socket** (a pipe fails). This is why the transport is a
  *socket* pair, not a pipe.

**Consequence:** the native path needs **zero QEMU source changes**. The only C
changes are in anyfs's own glue (`lspart_main.c` + `qemu_backend.c`).

## 3. Approach decision

**Chosen: Approach A — reuse QEMU's `fd:N` SocketAddress, zero QEMU source changes.**

The parent creates `socketpair(AF_UNIX, SOCK_STREAM)`, inherits one end (no
`CLOEXEC`) into the lspart child, and lspart constructs NBD server options as
`server.type=fd, server.str="N"`. QEMU's no-monitor path turns N into the channel via
`qio_channel_socket_new_fd`.

Rejected alternatives:

- **Approach B — custom `QIOChannel` subclass** (io_readv/writev/close over any fd
  incl. pipes). More flexible and pipe-capable, but requires non-trivial QEMU source
  changes and a hook in `nbd_open` to substitute `s->ioc`. **Deferred** as the future
  evolution path if a true pipe/SAB transport is ever needed; out of PoC scope.
- **Approach C — `channel-command.c`** (QEMU spawns the proxy and uses its stdio).
  Inverts the lifecycle direction (QEMU becomes the parent); wrong shape. Rejected.

## 4. Architecture

```
+------------------+        socketpair(AF_UNIX)         +-------------------+
|  Parent (Node)   |  fd0 <---- NBD frames (R/W) ----> fd1 |  lspart child     |
|  hand-written    |        (no TCP listen)              |  (QEMU block layer)|
|  NBD server      |                                     |                   |
|  fs.read(local   |   fd1 inherited (no CLOEXEC),        |  blk_new_open(    |
|  qcow2/img)      |   passed as --nbd-fd N              |   nbd: type=fd     |
|  -> NBD READ     |                                     |   str="N")        |
+------------------+                                     +-------------------+
                                                          | qcow2 driver auto-layered
                                                          v
                                                     list partitions + read a slice
```

### Three-stage verification (increasing risk, gated in order)

1. **fd passing without QEMU source changes** (highest leverage) — confirm the
   `fd:N` SocketAddress adopts an inherited socketpair fd under no-monitor.
2. **Full Linux chain** — Node NBD server (read-only, plain-file data source only)
   → socketpair → lspart opens the qcow2 over NBD → list partitions + read & verify
   a slice.
3. **Windows/wine fd-passing scouting** — separate minimal experiment; socketpair is
   unreliable on Windows so this falls back to TCP loopback.

### Explicitly out of PoC scope (YAGNI)

Physical-disk / privileged reads · wasm `nbdfs` · custom `QIOChannel` (Approach B) ·
N-API/Electron integration · write path (read-only + future `BDRV_O_SNAPSHOT`) · NBD
TLS / structured-reply / advanced features.

## 5. Components & interfaces

### (1) Node NBD server — new, `scripts/poc-nbd-server.mjs`

- **Does:** speaks NBD newstyle (read-only subset) over an already-open socket fd.
- **Interface:** `startNbdServer(fd, imagePath)`. Data source abstracted as
  `read(offset, len) -> Buffer`. **PoC simplification:** synchronous
  `fs.readSync(imageFd, ...)`, serving one request at a time. The production server is
  fully async with multiple in-flight requests — see §9.
- **Protocol subset:** newstyle fixed handshake → `NBD_OPT_GO` (advertise export size
  + flags with `NBD_FLAG_READ_ONLY`) → command loop handling only `NBD_CMD_READ`,
  `NBD_CMD_FLUSH` (no-op), `NBD_CMD_DISC`; everything else returns `EINVAL`. Simple
  replies (no structured reply).
- **Depends on:** Node built-in `net` / `fs` only.

### (2) fd-bridge launcher + native transport addon — new

- `scripts/poc-nbd-launch.mjs` (the parent role) plus a small N-API addon
  (`scripts/poc-native-transport/`, or reusing the anyfs-native build skeleton).
- **Addon exports:**
  - `socketpair() -> [fd0, fd1]` — Linux/macOS: libc
    `socketpair(AF_UNIX, SOCK_STREAM, 0)`, returns two bare fds.
  - reserved Windows abstraction slot, e.g. `createLoopbackPair() ->
    { serverHandle, childPort }` (implementation filled in for the Windows branch).
- **This addon is the seed of a unified "Node-layer native transport abstraction."**
  Future wasm twisted transport and Windows handle passing layer here, exposing one
  interface to callers (launcher today, Electron main process later).
- **Launcher:** calls the addon for fds, inherits `fd1` into the lspart child via
  `child_process.spawn` (`stdio`/fd options), keeps `fd0` for the NBD server.
- **Critical detail:** the inherited fd must **not** be `CLOEXEC`; the child learns
  its number via `--nbd-fd N`.

### (3) lspart NBD entry — modify `src/lspart/lspart_main.c`

- **Does:** parse `--nbd-fd N` (and Windows `--nbd-port P`), and feed a pseudo-path
  (e.g. `nbd-fd:N`) into the existing `anyfs_session_open` → `anyfs_disk_add` →
  `qemu_blk_open` path (today it opens plain image paths at `lspart_main.c:95`).
- **Interface:** reuse existing lspart output (partition table + optional hexdump of a
  slice).
- This plus (4) are the **only C changes**, and per Approach A they do **not** touch
  QEMU source.

### (4) qemu_backend nbd path — modify `src/core/qemu_backend.c`

- **Does:** when `image_path` looks like `nbd-fd:N` (resp. `nbd-port:P` on Windows),
  build a QDict with `server.type=fd, server.str="N"` (resp.
  `server.type=inet, host=127.0.0.1, port=P`) and pass it to `blk_new_open` instead of
  treating `image_path` as a filename.
- **Interface:** add an `is_nbd_fd` branch alongside the existing `is_url` options
  block (`qemu_backend.c:175-184`), reusing the existing QDict-options mechanism.

## 6. Data flow

### Startup & handshake (once)

```
launcher.mjs            addon          lspart (child / QEMU)        NBD server (Node)
   |  socketpair() ----->|                  |                            |
   |<-- [fd0, fd1] ------|                  |                            |
   |  spawn(lspart, --nbd-fd <fd1 num>,                                  |
   |    inherit fd1 non-CLOEXEC) ---------->|                            |
   |  startNbdServer(fd0, imagePath) ----------------------------------> | open imageFd
   |                                        |  qemu_blk_open("nbd-fd:N") |
   |                                        |  QDict server.type=fd,str=N|
   |                                        |  blk_new_open --handshake->|
   |                                        |<-- NBD_OPT_GO: size, RO ---|
   |                                        |  probe: read header --READ>| fs.readSync
   |                                        |<-- qcow2 magic ------------|
   |                                        |  auto-layer qcow2 driver   |
```

### Steady-state read (per partition / byte read)

```
lspart: list partitions / hexdump
   |  blk_pread(offset, len) -> qcow2 driver resolves cluster map
   |     -> emits NBD_CMD_READ(phys offset, len) -> socketpair fd -> NBD server
   |                                                                  fs.readSync(...)
   |<-------------- NBD simple reply (handle + data) <----------------|
   |  verify bytes
```

### Shutdown / lifecycle binding

- **Normal:** lspart done → `blk_unref` → NBD client sends `NBD_CMD_DISC` → socketpair
  EOF → NBD server closes imageFd and exits.
- **Child crash:** lspart dies → inherited fd1 closed by OS → NBD server reads
  EOF/`ECONNRESET` on fd0 → exits. **This is where lifecycle binding pays off** — no
  dangling state, no leaked port.
- **Parent crash:** NBD server dies → fd0 closed → lspart's NBD client reads EOF on
  the socketpair → `blk_pread` returns `-EIO` → lspart errors out.

### Windows branch differences

`socketpair()` is replaced by the addon's `createLoopbackPair()` (`127.0.0.1:0` bind +
accept, no outward listen); the child gets `--nbd-port P` instead of `--nbd-fd N`, and
QEMU uses `server.type=inet, host=127.0.0.1, port=P`. Protocol frames and data flow are
identical — only the transport-endpoint construction differs. Windows anonymous pipes
were ruled out: they are unidirectional and `channel-file`'s POSIX `readv`/poll is
unreliable for pipe fds under win32's HANDLE-based main loop; named pipes would require
a hand-written `QIOChannel` (ReadFile/WriteFile + overlapped + win32 main-loop
integration), which is out of PoC scope.

## 7. Error handling

| Failure point | Symptom | Handling |
|---|---|---|
| fd is not a socket | `socket_get_fd` → `fd_is_socket` fails → "File descriptor 'N' is not a socket" | launcher must pass a socketpair fd; this error is itself the stage-1 probe |
| fd closed by CLOEXEC | child can't see fd, `blk_new_open` → EBADF | spawn without CLOEXEC; test asserts child `fstat(N)` succeeds |
| NBD handshake mismatch | QEMU NBD client protocol error | server strictly follows newstyle fixed handshake; cross-check with `qemu-nbd`/`nbdinfo` |
| image not qcow2 / probe fails | `blk_getlength` < 0 or empty partition list | use a known qcow2 test image; first confirm the plain file path opens the same image as a control |
| read out of bounds | server sees offset+len > size | server returns `NBD_REP_ERR`/`EINVAL`, lspart reports EIO |
| Windows fd path unavailable | (expected) socketpair fails | addon takes the loopback branch directly on win; never attempts socketpair |

## 8. Testing strategy & success criteria

Each stage has one executable criterion and gates the next.

### Stage 1 — fd passing, zero QEMU source changes (highest leverage)

- **Test:** launcher creates the socketpair, spawns a minimal lspart invocation that
  only does `blk_new_open("nbd-fd:N")` then `blk_getlength` and prints capacity.
- **PASS:** lspart prints the **same capacity** as opening the plain file, and QEMU is
  **unmodified at the source level** (only anyfs-side C changed).
- **FAIL handling:** "not a socket" → revisit launcher fd passing. If QEMU source must
  be changed → re-evaluate, possibly escalate to Approach B.

### Stage 2 — full Linux chain

- **Test:** with a known-content qcow2 (GPT + ext4, containing a verifiable magic byte
  pattern), run full lspart:
  1. partition list matches `qemu-img info` / plain-file lspart output **item by item**;
  2. hexdump at a chosen offset is **byte-for-byte** equal to the expected magic.
- **PASS:** partition list matches + byte verification passes + the NBD server log shows
  a non-trivial number of `NBD_CMD_READ`s (proving data actually traversed the
  socketpair, not some other path).

### Stage 3 — Windows/wine scouting

- **Test:** under wine, run the mingw64 lspart through the loopback branch on the same
  qcow2.
- **PASS:** opens + partition list matches.
- This is a go/no-go probe; **failure is allowed and recorded** in FINDINGS.md (per the
  repo rule of documenting blocking findings inline) — Windows does not block the Linux
  main-chain conclusion.

### Cross-validation

Use the system `qemu-nbd` + `nbdinfo`/`qemu-img` to independently validate the
hand-written Node NBD server's protocol correctness: first get a real QEMU client to
connect to the hand-written server successfully, then swap in lspart. This decouples
"protocol implementation bug" from "transport bug."

### Regression guarantee

All changes live on the anyfs side (lspart + qemu_backend branches); the existing
file/URL paths are untouched. Existing lspart plain-file usage must keep passing.

## 9. Production server (post-PoC, design intent)

The PoC server (§5.1) is deliberately synchronous and one-request-at-a-time to keep
the verification minimal. The **production** Node NBD server is **fully asynchronous**
end to end, on both the local-fs and the http/url data sources. This section records
the intent so the PoC interface (`read(offset, len) -> Buffer/Promise<Buffer>`) doesn't
have to be re-shaped later.

### Concurrency model — exploit NBD's multi-in-flight

QEMU's NBD client issues up to **16 concurrent in-flight requests**
(`MAX_NBD_REQUESTS = 16`, `block/nbd.c:50`), pairing requests to replies by `cookie`
(handle) and accepting **out-of-order** replies (`nbd_receive_replies` matches by
cookie, `block/nbd.c:421`). The production server therefore:

- Reads NBD command headers in a loop **without waiting** for the previous command's
  data — it can hold several `NBD_CMD_READ`s outstanding at once.
- Dispatches each read to an async data source, and emits each reply (handle + data) as
  soon as that source resolves, in whatever order they complete. The handle guarantees
  the client re-associates correctly.
- Serializes only the *write* of reply frames onto the socket (one reply on the wire at
  a time), not the *servicing* of reads.

This is what keeps a keep-alive http upstream's throughput up: multiple Range requests
can be in flight to the upstream concurrently instead of a serial request/wait/respond
cycle.

### Async data sources

- **Local fs:** `fs.promises` / async `fs.read` (or a small read pool), never
  `readSync`.
- **http/url:** a persistent keep-alive agent (`http.Agent({ keepAlive: true })` or
  `undici` with a connection pool) so range requests **reuse upstream connections**
  instead of reconnecting per request — the exact reconnect-churn problem URLFS has
  today.

### Shared concern with URLFS

URLFS (`ts/packages/core/src/url-fs.ts`) currently issues a fresh synchronous XHR per
chunk with no upstream connection reuse. The production http data source should solve
keep-alive / connection pooling in a way that URLFS can later adopt the same approach.
This is noted as a **shared future improvement**, not part of this PoC.

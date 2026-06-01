# Production NBD Proxy — Design

**Date:** 2026-06-01
**Status:** Design approved, pre-planning
**Builds on:** the NBD-over-inherited-fd transport PoC (proven 2026-06-01, `scripts/poc-nbd/`, spec `2026-05-31-nbd-fd-transport-poc-design.md`).

## 1. Purpose

A standalone, optionally-privileged Node process that exposes a data source —
**a local file, a physical disk / privileged file, or an http(s) URL** — as a
read-only NBD endpoint. A QEMU engine (separate, low-privilege process) connects
over that NBD channel via an inherited socket fd or a `127.0.0.1` loopback port,
and QEMU's format drivers (qcow2/vmdk/…) auto-layer atop the NBD protocol.

This is the production form of the PoC's hand-written synchronous one-request-at-a-time
server: fully asynchronous, with multiple in-flight requests, and a real data-source
abstraction.

### Why NBD (not the existing HTTP-range proxy)

The repo already has HTTP-range proxies (`ts/packages/anyfs-native/src/http-disk-server.ts`,
`ts/examples/electron-demo/src/http-proxy-worker.ts`) that feed QEMU's **curl** driver
over plain HTTP. We deliberately use **NBD** instead because it gives what those cannot:
a **private channel that is not a listening service** (inherited fd), **lifecycle binding**
to the process pair (channel EOF tears both sides down cleanly), no exposed port, and
NBD's native read-only + up-to-16 in-flight model. These are the original motivations for
the "twisted transport." The existing HTTP proxies remain for their existing URL scenarios.

### Process & privilege model

The proxy is a **separate process** that may be launched as root/Administrator solely to
read privileged sources (physical disks, protected files). The QEMU engine stays in a
**low-privilege** process and only speaks NBD to the proxy. Privilege isolation is clean:
elevation lives entirely in the proxy.

## 2. Architecture

```
+------------------------------+     NBD (inherited fd / 127.0.0.1 loopback)   +------------------+
| anyfs-nbd-proxy (separate)   | <--- multi in-flight, out-of-order replies -> | QEMU engine      |
| may run root/Administrator   |                                               | (low privilege)  |
|                              |                                               | qcow2/vmdk ->    |
|  NbdServer (async, 16-way)   |                                               |   nbd: protocol  |
|        | read(off,len)                                                       +------------------+
|        v                                                                     
|  DataSource (interface)                                                      
|   |- FileSource        (fs.promises)                                         
|   |- BlockDeviceSource (/dev/sdX ; size via drivelist)                       
|   '- HttpSource        (undici keep-alive pool / H2)                         
+------------------------------+
```

### Package layout — `ts/packages/anyfs-nbd-proxy/`

| File | Responsibility |
|---|---|
| `src/nbd-server.ts` | Async multi-in-flight NBD newstyle server (read-only subset). Out-of-order replies keyed by handle; only reply-frame writes are serialized. |
| `src/data-source.ts` | `DataSource` interface (`size()`, `read(offset,len)`, `close()`) + `createDataSource(spec)` factory. |
| `src/sources/file.ts` | `FileSource` — `fs.promises` async pread. |
| `src/sources/blockdev.ts` | `BlockDeviceSource` — open raw device, size via `drivelist`, plain pread (Linux v1). |
| `src/sources/http.ts` | `HttpSource` — undici keep-alive pool (H1) / H2 mux, Range requests. |
| `src/endpoint.ts` | Bind an NbdServer to an inherited fd (`net.Socket({fd})`) or a `127.0.0.1:0` loopback listener. |
| `bin/anyfs-nbd-proxy.ts` | Thin CLI: parse args, build a DataSource + endpoint, run. No business logic. |
| `test/*.test.ts` | Unit + qemu-img/qemu-io integration tests. |

### Relationship to existing code

- The PoC `scripts/poc-nbd/` stays as a runnable regression baseline — untouched.
- Existing `http-disk-server.ts` / `http-proxy-worker.ts` (QEMU curl route) stay for their
  existing URL scenarios — untouched and not in conflict (this package is the NBD route).
- The `DataSource` abstraction is meant to be reusable by the later native-integration step.

## 3. NbdServer — async multi-in-flight concurrency

PoC was strictly serial (read header → sync read → write reply → next). Production reads
headers without blocking, holds multiple in-flight reads, and replies in completion order.

### Read loop (producer)

```
loop:
  hdr = await readExactly(28)            // next request header; does not await in-flight reads
  if DISC: break
  if READ:
    if inFlight >= 16: await slotFreed   // backpressure, aligned to client MAX_NBD_REQUESTS
    inFlight++
    handleRead(handle, offset, length)   // NOT awaited — dispatch and continue reading
      .then(data => enqueueReply(handle, 0, data))
      .catch(err  => enqueueReply(handle, errnoOf(err), null))
      .finally(() => { inFlight--; signalSlotFreed() })
  if FLUSH: enqueueReply(handle, 0, null)   // read-only, no-op
```

### Write side (consumer, serialized)

A socket may carry only one reply frame at a time (header+data must not interleave). All
`enqueueReply` calls feed a FIFO drained by a single writer loop that `await socket.write()`s
each frame. Reply order = completion order (out-of-order vs request order); the 8-byte
handle lets the client re-pair — exactly NBD simple-reply semantics.

### Why this saturates throughput

16 READs can `await` their data sources concurrently (16 parallel Range requests over the
keep-alive pool / H2 streams, or 16 parallel fs preads). Only frame-writing is serialized,
and that is memory→socket — far faster than source IO.

### Reader simplification

The read loop frames request headers **serially** (one after another), so the byte reader
need only support a single outstanding `read()` — concurrency happens only *after* a request
is parsed, in data fetching. We do not parse multiple headers concurrently; we only service
them concurrently.

## 4. DataSource implementations

```ts
interface DataSource {
  size(): Promise<number>;                               // NBD export size (bytes)
  read(offset: number, length: number): Promise<Buffer>; // exactly `length` bytes
  close(): Promise<void>;
}
```

NbdServer depends only on this interface. `createDataSource({kind, target})` dispatches.

### FileSource (`sources/file.ts`)

`fs.promises.open(path,'r')` → `FileHandle`; `size()` via `fh.stat()`; `read()` via
`fh.read(buf, 0, length, offset)`. Pure async (the PoC `readSync` upgraded).

### BlockDeviceSource (`sources/blockdev.ts`) — Linux v1

- **size**: `drivelist.list()` (already integrated in the repo, enumeration-only) → match the
  target device → use its `.size` field. Cross-platform metadata, no `/sys/block` parsing, no
  native ioctl. Fallback to `/sys/block/<dev>/size`×512 on Linux if drivelist is unavailable.
- **read**: plain `fs.promises` `fh.read(buf, 0, len, offset)`.
  - **Linux**: the kernel block layer handles non-aligned reads — **no app-level alignment
    needed**. This is the v1 target.
  - **Windows** (deferred): `\\.\PhysicalDriveN` `ReadFile` requires sector alignment → a later
    version adds an align-expand-and-crop wrapper. `/dev/rdiskN` on macOS similar; use buffered
    `/dev/diskN` to avoid alignment.
- **v1 scope: Linux only.** macOS/Windows raw-device specifics + alignment are deferred; the CLI
  reports a clear "blockdev v1 is Linux-only" error on other platforms rather than failing
  silently. **No native code is required for v1** (drivelist for size, Linux pread for reads).

### HttpSource (`sources/http.ts`)

`undici`: a `Pool` (H1 keep-alive, connection reuse) or automatic H2 multiplexing. `size()`
via HEAD (follow redirects; read `content-length` + `accept-ranges`); `read()` sends
`Range: bytes=off-end`. The 16 in-flight reads map to concurrent pool requests (H1) or H2
streams. Defensive: if a server ignores Range and returns 200 with the full body, crop
(same defense URLFS uses).

> Note: this keep-alive/pool approach is what spec §9 of the PoC design flagged as something
> URLFS *may* later adopt. That is a **future, optional** improvement to URLFS — **not** part of
> this package and not a change to URLFS now.

## 5. Endpoint & CLI

### Endpoint (`src/endpoint.ts`) — reuses the two PoC-proven paths

| Mode | Use | Implementation |
|---|---|---|
| inherited fd | Linux/macOS: parent makes a socketpair, passes one end to the proxy child | `--fd N` → `new net.Socket({fd:N})` → NbdServer serves it |
| loopback | Windows, or when fd-passing is impractical | `--port P` (or `0` to auto-pick) → `net.createServer().listen(P,'127.0.0.1')`, one NbdServer per connection |

These are PoC Stage 1 (fd) and Stage 3 (loopback) lifted into a reusable layer; NbdServer is
agnostic to which backs it.

### CLI (`bin/anyfs-nbd-proxy.ts`) — thin wrapper

```
anyfs-nbd-proxy \
  --source file|blockdev|url \
  --target <path | /dev/sdX | https://...> \
  (--fd <N> | --port <P>) \
  [--export-size <bytes>]      # optional, overrides auto-detected size
```

Flow: parse → `createDataSource({kind,target})` → `await source.size()` → build endpoint per
`--fd`/`--port` → run NbdServer(s) → exit on endpoint EOF / SIGTERM.

`bin/` contains no business logic — NbdServer, DataSource, and endpoint all live in importable
`src/`, so the later native-integration step can either spawn this CLI process (preferred) or
import the library.

### Lifecycle binding (the core "twisted transport" payoff, from the PoC)

- inherited fd: parent dies → fd EOF → NbdServer reads EOF → proxy exits. proxy dies → fd closes
  → QEMU's NBD client reads EOF → EIO. No dangling state, no leaked port.
- loopback: the parent kills the proxy child on its own exit (or the proxy self-exits on stdin EOF).

## 6. Error handling

| Failure | Symptom | Handling |
|---|---|---|
| out-of-bounds read | offset+len > size | NBD reply `EINVAL`; server keeps running |
| DataSource IO error (fs/blockdev/http) | read rejects | that request replies `EIO` (errno-mapped); other in-flight reads continue |
| http upstream 5xx / dropped conn | undici rejects | that read → EIO; pool reconnects for later requests |
| privileged source permission denied | open fails (EACCES) | startup `size()` fails → CLI prints a clear error and exits non-zero (never enters the NBD loop) |
| client disconnect / EOF | read loop hits EOF | clean teardown: close DataSource, proxy exits |
| blockdev on non-Linux in v1 | blockdev on mac/win | CLI reports "blockdev v1 is Linux-only"; no silent failure |

## 7. Testing strategy

- **Unit:** a mock DataSource that completes out of order (verify handle pairing); FileSource
  pread against a known byte pattern; HttpSource against a local Range server incl. pool reuse;
  out-of-bounds and EIO paths.
- **Integration (reuse the PoC fixture approach):**
  - `qemu-img info` / `qemu-io read` open a fixture qcow2 through this server (over a unix socket),
    verifying format detection + byte correctness (as PoC Tasks 6/8, but against the new server).
  - Each data source once: FileSource (qcow2 file), BlockDeviceSource (a loopback-mounted device,
    or skipped if no privilege), HttpSource (local Range server serving the qcow2).
  - **Out-of-order concurrency test:** make HttpSource impose differing per-request delays; assert
    the server still replies correctly keyed by handle.
- **Web regression (must run):** confirm the web demo's blob open and URL open are unaffected
  (this package touches none of those files; verify once at acceptance regardless).

## 8. Non-goals & must-not-break constraints

This package is the **NBD route**, serving only native's private fd/loopback channel for
privileged files / physical disks / (optionally) http sources. It **must not modify or replace**
the existing web paths, which continue to work exactly as before:

- **blob**: browser File/Blob → WORKERFS (`/work/<name>`) → wasm kernel.
- **CORS-relaxed URL**: existing URLFS (sync XHR + Range + LRU); existing `http-disk-server.ts` /
  electron `http-proxy-worker.ts` (QEMU curl driver).

This package introduces **no changes** to `ts/packages/core`, URLFS, or WORKERFS. The HttpSource
keep-alive/pool design is a *future, optional* reference for URLFS — not a change to URLFS now.
Acceptance must confirm zero regression in the web demo's blob and URL opens.

## 9. Future direction (NOT this scope)

Later, the Electron build may route **all** data sources through this NBD proxy (unifying local
file / URL / physical disk behind one NBD channel and moving the QEMU engine fully into the
separate proxy process). That convergence is explicitly **out of scope for this package's first
version** — it is recorded here as intent. For now, NBD is an additive parallel route and the
existing web/Electron data-source paths are left intact.

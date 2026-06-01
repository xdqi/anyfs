# NBD-over-fd PoC — Findings

## Stage 1 (Linux, inherited-fd) — PASS
lspart opened the qcow2 over an inherited socketpair fd (`--nbd-fd 3`), exit 0,
27 reads traversed the socketpair, table parity with the plain-file open.
Required adding `module_call_init(MODULE_INIT_QOM)` to src/core/qemu_backend.c
(needed for the qio-channel-socket QOM type the NBD client adopts) — anyfs glue
only, zero QEMU source changes.

## Stage 2 (Linux, full chain) — PASS
lspart-over-NBD matched plain-file lspart (both 0 data rows; the synthetic
8 MiB image has no partition table). 27 inherited-fd reads. The marker
"ANYFS-NBD-POC-MARKER-v1" at offset 0x100000 read back byte-for-byte THROUGH
the qcow2-over-NBD chain via qemu-io (qcow2 driver → nbd: protocol → server).

## Protocol cross-check (Task 6) — PASS
qemu-img detected qcow2 over the hand-written NBD server (over a unix socket),
independently confirming the NBD newstyle protocol implementation is correct.
(This caught a wrong NBD_REP_MAGIC constant, since fixed.)

## Stage 3 (Windows/wine, loopback fallback) — PASS (2026-06-01)

Approach: Windows has no reliable socketpair and QEMU requires the NBD fd to be
a socket, so the Windows path uses `--nbd-port P` (127.0.0.1 loopback, ephemeral
port, no outward listen) instead of an inherited fd.

**Result with a freshly-rebuilt QEMU-enabled lspart.exe:**
```
[qemu_blk] open(nbd-port:43113) ro=1 snap=0
[qemu_blk] bdrv_init…
[qemu_blk] main loop ready
[qemu_blk] blk_new_open name=(nbd via options) flags=0x0
[qemu_blk] blk_new_open returned 0x...
[qemu_blk] capacity=8716288
--- connections=1 nbd_reads=49 exit=0 ---
STAGE3 PASS (wine loopback): qcow2 opened over NBD, data traversed 127.0.0.1
```
Under wine-10.0, the mingw64 QEMU NBD client connected to the host Node NBD
server on `127.0.0.1`, completed the newstyle handshake, opened the qcow2 over
NBD (capacity detected), and 49 reads traversed the connection. The Windows
loopback fallback transport is validated.

### What it took to get there (instructive)

The first attempts FAILED, and tracing why surfaced three real gotchas:

1. **DLL search path** — without `WINEPATH`, the dependent DLLs
   (libwinpthread-1.dll, libanyfs-qemublk.dll, liblkl.dll) live in
   `build-anyfs-mingw64/bin/` but Windows loads them relative to the module, not
   from a separate bin/ staging dir. Fix: run with
   `WINEPATH=Z:\...\build-anyfs-mingw64\bin` (or stage DLLs next to the .exe).
   Matches the known win64 DLL-search pattern.

2. **Stale binary** — the original 2026-05-29 lspart.exe predated the
   `--nbd-fd`/`--nbd-port` flags and the `MODULE_INIT_QOM` fix. Rebuilt the
   mingw64 lspart.

3. **QEMU backend not compiled in (the real blocker)** — the rebuilt lspart STILL
   failed (`failed to open nbd-port:N`, zero connections) because the
   `build-anyfs-mingw64` meson dir was configured with **`enable_qemu=False`**, so
   `qemu_backend.c` (and its nbd branch) was never compiled into the exe — it used
   the raw backend, treating `nbd-port:N` as a filename. The
   `scripts/build_anyfs.sh` script only emits `-Denable_qemu=true` when the build
   includes the `core` component; a `--components=server`-only build silently
   re-disables QEMU. Fix: rebuild with `--components=core,server` so QEMU is
   enabled and `qemu_backend.c` is compiled into `libanyfs_core.a`.

### Build recipe to reproduce
```
# 1. ensure the mingw QEMU libs + libanyfs-qemublk.dll are built:
bash scripts/build_qemu.sh --targets=mingw64
# 2. build lspart WITH the qemu backend (must include 'core'):
bash scripts/build_anyfs.sh --targets=mingw64 --components=core,server
# 3. run under wine with the DLL dir on WINEPATH:
WINEPATH=$(winepath -w build-anyfs-mingw64/bin) \
  wine build-anyfs-mingw64/src/lspart/anyfs-lspart.exe --nbd-port <P>
```

Both transport variants are now validated: Linux inherited-fd (Stages 1/2) and
Windows 127.0.0.1 loopback (Stage 3).

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

## Stage 3 (Windows/wine, loopback fallback) — FAIL (stale binary; re-build needed)

Approach: Windows has no reliable socketpair and QEMU requires the NBD fd to be
a socket, so the Windows path uses `--nbd-port P` (127.0.0.1 loopback, ephemeral
port, no outward listen) instead of an inherited fd.

- mingw64 anyfs-lspart.exe present: yes (built 2026-05-29)
- wine present: yes (wine-10.0)
- CAVEAT: the .exe predates this PoC's changes (the `--nbd-port` lspart flag and
  the `MODULE_INIT_QOM` fix), so it lacks both.

Two attempts were made:

**Attempt 1** — without WINEPATH (DLLs not on the search path):
```
0124:err:module:import_dll Library libwinpthread-1.dll ... not found
0124:err:module:import_dll Library libanyfs-qemublk.dll ... not found
0124:err:module:import_dll Library liblkl.dll ... not found
0124:err:module:loader_init Importing dlls for L"...\anyfs-lspart.exe" failed, status c0000135
```
The required DLLs (libwinpthread-1.dll, libanyfs-qemublk.dll, liblkl.dll) are in
`build-anyfs-mingw64/bin/` but wine could not find them because they are not
adjacent to the .exe. This matches the known win64 DLL search pattern: Windows
loads dependent DLLs relative to the module, not a separate bin/ staging directory.

**Attempt 2** — with `WINEPATH=Z:\...\build-anyfs-mingw64\bin` (DLLs found):
```
unknown flag: --nbd-port
Usage: Z:\home\kosaka\anyfs-reader\build-anyfs-mingw64\src\lspart\anyfs-lspart.exe [--json] [--help] <image>[?<query>] [<image>...]
```
The DLLs loaded successfully and the binary started. It exited non-zero because
`--nbd-port` is not implemented in the 2026-05-29 build. The binary's usage
string shows only `[--json] [--help] <image>` — no `--nbd-fd` or `--nbd-port`
flags — confirming it predates the PoC's Task 1/2 changes entirely.

The NBD server was listening and accepting connections (the in-process Node server
ran without issue); the failure is entirely on the binary side.

### Notes / next steps

The stale-binary caveat was the anticipated blocker. A fresh mingw64 cross-build
of lspart incorporating:
1. Task 1/2 changes: the `--nbd-fd` / `--nbd-port` CLI flags in lspart
2. The `module_call_init(MODULE_INIT_QOM)` fix in src/core/qemu_backend.c

is required before the Windows loopback path can be properly evaluated. That
cross-build is a heavyweight step (out of scope for this scouting probe) and does
NOT block the Linux PoC conclusion.

When that rebuild is available, the correct invocation is:
```
WINEPATH=Z:\...\build-anyfs-mingw64\bin wine anyfs-lspart.exe --nbd-port <P>
```
(or DLLs staged next to the .exe to avoid the WINEPATH requirement).

The loopback transport design itself is sound: the Node NBD server bound,
accepted, and served without issue; the only gap is the unbuilt Windows client.

# Robustness gate design: corrupt images must never hang or crash the sandbox

**Date:** 2026-10-05
**Status:** implemented (plan: `docs/superpowers/plans/2026-10-06-robustness-gate.md`); see "Amendments during implementation" at the end
**Scope:** `ts/tests/robustness/` (new), `ts/packages/core/src/{session-base,wasm-session,native-session,node-wasm-session}.ts`,
`src/core/anyfs_mount.c`, a few E2E specs

## Goal

anyfs exists to open disk images that users did not make: forensics images, old VM disks, downloads.
Filesystem drivers are the largest attack surface in the stack: about 35 kernel filesystems plus the
out-of-tree NTFS, APFS and ZFS drivers. Upstream Linux does not treat mounting a malicious image as a
security boundary. The project's answer is the wasm sandbox. This design turns that sandbox into a
checked promise:

> **On the wasm backend, no image — corrupt, truncated or hostile — makes the app hang or crash.
> Every failure ends as a clean error or a reported fatal, and the user can open the next image.**

The native backend (the opt-in fast path) is measured against the same corpus, but its crashes are
recorded as findings, not gated. Process isolation for native is a later step.

## Context

- The only corrupt-image test today is a 1 MiB all-zero file (`ts/tests/e2e/fixtures/bad-image.ts`,
  `errors.spec.ts`): no partition table, no filesystem. Nothing exercises a *partly* valid filesystem,
  which is where drivers BUG, loop or panic.
- Timeouts: only attach is bounded (`DEFAULT_ATTACH_TIMEOUT_MS = 120_000` in
  `ts/packages/react/src/provider.tsx`). After attach, enter/readdir/stat/read have no timeout on any
  backend (`WasmSession.call`, `NativeSession`, `NodeWasmSession`). A wedged driver leaves the UI
  spinning forever.
- Since the QEMU-thread work (`2026-10-05-qemu-dedicated-thread-design.md`) the wasm module-owning
  thread never blocks: every op runs on the glue's API thread. So even when the kernel is wedged, the
  JS side of a WasmSession stays responsive and can time out, report and terminate the worker.
- Native: a wedged op holds the addon's `g_op_mutex` forever, so recovery needs an app restart. A
  kernel panic (`lkl_host_ops.panic` → `abort()`) takes the whole process down.
- Corrupt superblocks can *ask* for a panic: ext4 honours a superblock `errors=panic` setting, so a
  hostile image can deliberately panic the kernel on its first error. anyfs mounts read-only with
  `noload` (ext*) or `norecovery` (xfs) but does not override the errors behaviour
  (`src/core/anyfs_mount.c`).
- syzbot attaches the filesystem image each reproducer mounts ("mounted in repro") as
  `https://storage.googleapis.com/syzbot-assets/<id>/mount_N.gz`.

## Decisions (from design review)

| Question | Decision |
|---|---|
| Where the corpus runs | Node batch harness over the wasm bundle and the native addon, each case in its own child process; plus 3–4 E2E cases for the UI |
| Strictness | wasm strict (no hang, no crash); native records hangs/crashes as findings, non-gating |
| Corpus | Images generated rootless at run time + deterministic mutations, **plus** a curated syzbot set |
| Where it runs | **Locally only.** CI stays build-only; the gate is a pre-release check |

## Components

### 1. Corpus generator — `ts/tests/robustness/corpus/generate.mjs`

- **Base images, rootless.** Each holds a small known tree (a few dirs, a symlink, small and
  multi-block files):

  | fs | tool |
  |---|---|
  | ext4 | `mkfs.ext4 -d` |
  | vfat | `mkfs.fat` + `mcopy` |
  | btrfs | `mkfs.btrfs --rootdir` |
  | xfs | `mkfs.xfs -p` protofile |
  | iso9660 | `xorriso` |

  The ext4 base is also wrapped as **qcow2** and **vmdk** with `qemu-img` to cover the container
  layer, and one disk gets an **MBR/GPT** partition table to cover the partition scanner.
- **Mutations** (each a pure function of `(base, seed)`, so the corpus is reproducible):
  1. superblock field corruption: magic kept, sizes/counts/offsets set to extreme values;
  2. zeroed metadata blocks: inode tables, group descriptors, btree roots, chosen by offset tables
     per fs;
  3. random byte flips in the first N MiB (several seeds, several densities);
  4. truncation at 25 %, 50 % and 90 % of the image;
  5. container headers: qcow2 L1/refcount table offsets and cluster bits; vmdk grain directory and
     capacity;
  6. partition table: overlapping and out-of-range entries, an extended-partition loop.
- Target: about 60 generated cases. Output goes to `~/.cache/anyfs-robustness/generated/<case>.img`
  (not `/tmp`, a small tmpfs here), plus `cases.json` describing each case.

### 2. syzbot set — `ts/tests/robustness/syzbot.json` + `fetch-syzbot.mjs`

- The manifest only, committed: `{ extid, title, link, fs, url, sha256, repro_mount_opts, notes }`.
  Images are downloaded at run time into `~/.cache/anyfs-robustness/syzbot/`, never committed or
  redistributed.
- Selection: filesystems anyfs supports (ext4, btrfs, xfs, fat, hfsplus, iso9660, udf, ntfs, f2fs,
  squashfs, exfat). Prefer bugs on mount / lookup / readdir / read paths, since anyfs mounts
  read-only and write-path bugs cannot trigger. Mix fixed and unfixed bugs. About 20 entries, 2–3
  per fs. A one-off helper lists candidates per subsystem from the syzbot dashboard; the final pick
  is curated by hand.
- `fetch-syzbot.mjs` downloads what is missing and verifies sha256. A missing asset or a hash
  mismatch is an error, never a silent skip.

### 3. Product watchdog — `AnyfsSessionBase`

- New `opTimeoutMs` on `SessionOpts` (default **60 000**, `0` disables), passed to every session.
- `protected guard<T>(op: string, p: Promise<T>): Promise<T>`: on timeout the op rejects with
  `"<op> timed out after Ns — the engine is wedged"`, and `fireFatal()` fires with the same error.
- Wrapped ops: `listParts`, `meta`, `enter`, `readdir`, `stat`, `statFollow`, `readlink`,
  `realpath`, `readKernelFile`, `_openFdRaw`, `_readFdRaw`, `_closeFdRaw`. Attach keeps the
  provider's 120 s bound (it includes kernel boot).
- After a fatal:
  - WasmSession: the provider's existing `onFatal` handler closes the session, which terminates
    the worker.
  - NativeSession: the existing `engineFailed` path skips waiting on the wedged engine at
    dispose; the UI tells the user to switch to wasm or restart.
  - NodeWasmSession: the module is process-global, so recovery means a new process (documented).
- **NodeWasmSession abort → fatal:** the module's `onAbort` (kernel panic, wasm trap) calls
  `fireFatal()` and rejects pending API calls. Today an abort in Node surfaces as an unhandled
  error instead of a session fatal.

### 4. Mount hardening — `src/core/anyfs_mount.c`

- Append `errors=continue` (comma-joined with any existing `noload` / `norecovery`) for the
  filesystems that support the option: ext2/3/4, vfat/msdos, exfat, f2fs. A corrupt superblock can
  then no longer request a panic.
- Filesystems without an errors option (btrfs, xfs, iso9660, hfsplus, udf, squashfs) are left as
  they are; the watchdog and the wasm sandbox cover them. The out-of-tree NTFS PLUS and APFS
  drivers get the option only if their parsers accept it (checked during implementation).
- Guard against regressions: every **unmutated** base image must still mount and list with the
  hardened options. That check is part of the gate.

### 5. Harness — `ts/tests/robustness/run.mjs` + `case-runner.mjs`

- `run.mjs --backend wasm|native [--only <glob>] [--jobs N]`:
  1. generates the corpus if missing;
  2. fetches the syzbot set;
  3. forks one `case-runner.mjs` per case.
  It runs cases in parallel up to `--jobs`, with a per-case outer timeout of **3 min**.
- `case-runner.mjs`, in a fresh process:
  1. boot;
  2. open the image with `opTimeoutMs = 20 000`;
  3. list partitions;
  4. for each partition, enter read-only, walk at most 500 entries and 6 levels, and read at most
     64 KiB from each of at most 20 files;
  5. close.
  It reports each step to the parent over IPC; the last step reached is recorded.
  - wasm uses `mountNodeFile` / NodeWasmSession.
  - native uses the addon directly (`ts/packages/anyfs-native/build/Release/anyfs_native.node`),
    wrapped by the same guard logic.

### 6. E2E — 3–4 cases in `ts/tests/e2e/flows/robustness.spec.ts`

Taken from the same generator and syzbot set:
- one case expected to fail cleanly (error);
- one expected to end in a fatal (a syzbot image that panics LKL in wasm);
- one ext4 case with a corrupted inode table that mounts but fails on read.

Each asserts that the UI shows the error and that opening a good fixture afterwards lists its
partitions (recovery). These run on web and electron-wasm.

## Outcome classes and gate rules

Each (case, backend) ends in exactly one class:

| class | meaning |
|---|---|
| `ok` | every step completed |
| `error` | a step rejected with an ordinary error (EIO, unknown fs, …); the process stayed healthy |
| `fatal` | the session fired `onFatal` with a reason (kernel panic → abort, or the watchdog) |
| `hang` | the per-case outer timeout fired: the watchdog failed to catch a wedge |
| `crash` | the child died without reporting an outcome (signal, unreported abort, OOM) |

- **wasm gate:** passes when every case is `ok`, `error` or `fatal`, every `fatal` carries a reason,
  and every unmutated base image is `ok`. Any `hang` or `crash` fails the gate.
- **native:** same classification and report. `hang` and `crash` are findings (recorded in
  `ts/tests/robustness/FINDINGS.md`), not failures.
- Results are deterministic for a given corpus (fixed seeds, pinned syzbot hashes). A case that
  flips between classes across runs is itself a finding.

## Report

`~/.cache/anyfs-robustness/report-<backend>.json` holds one record per case: name, source
(generated | syzbot), fs, mutation, backend, class, last step, duration, reason. `run.mjs` also
prints a summary table: counts per class, and details for every `fatal`, `hang` and `crash`. The
exit status is non-zero when the wasm gate fails.

## Out of scope

- CI integration (CI stays build-only).
- Process isolation for native (Electron `utilityProcess`, option C of the QEMU-thread design).
- Fixing the driver bugs the corpus finds. They are recorded; fixes are separate work.
- Write paths (anyfs mounts read-only), ZFS (needs pool import first), LUKS/LVM.

## Acceptance criteria

1. `node ts/tests/robustness/run.mjs --backend wasm` runs the full corpus (generated plus syzbot)
   locally with no `hang` and no `crash`, and every unmutated base image is `ok`.
2. The same run with `--backend native` produces a report. Its hangs and crashes are recorded in
   `ts/tests/robustness/FINDINGS.md`.
3. Watchdog: unit tests show a wedged op rejects and fires `onFatal` within `opTimeoutMs` on
   WasmSession and NativeSession. NodeWasmSession turns a module abort into `onFatal`.
4. Hardened mount options keep every supported base filesystem mountable. A syzbot ext4 image
   whose superblock requests `errors=panic` no longer panics LKL.
5. The robustness E2E cases pass on web and electron-wasm, including recovery to a good image.

## Amendments during implementation (2026-10-06)

1. Watchdog: `AnyfsSessionBase.guard(op, run)` takes a thunk. WasmSession and NativeSession share a
   base `serialize()` queue, so an op's timer starts when it reaches the engine, not when it is
   queued. In-flight ops reject on any fatal.
2. A dead native engine is latched per bridge, so retries fail fast with `EngineFatalError`. The UI
   hint tells the user to turn on "Disable native module" or restart the app.
3. Read-write mounts get `errors=remount-ro`, not `errors=continue`. Read-only mounts use
   `errors=continue`.
4. Corpus size: 78 generated cases plus 22 syzbot images = 100, with an extra `errors=panic` ext4
   base (`ext4panic`).
5. Acceptance 4: none of the 229 ext4 syzbot bugs has a superblock with `errors=panic`, so the
   generated `ext4panic-zero-docs-inode` case meets the criterion. Before hardening it was fatal on
   wasm and a crash on native.
6. E2E: there is no fatal case, so two cases instead of three (mount failure, read failure), each
   followed by recovery into a good image. They also run on electron-native. The fatal path is
   covered by the Node harness and the core/react unit tests.
7. Harness additions: `failedStep` in each record, `expect` checks on unmutated bases, a build
   fingerprint (`build.engine`) with class-flip reporting, and `--loglevel`.

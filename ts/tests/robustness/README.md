# Robustness gate

A local, pre-release check of anyfs's core promise: on the wasm backend, no corrupt, truncated or
hostile image makes the app hang or crash. Every failure ends as a clean error or a reported fatal.
CI does not run it. Design: `docs/superpowers/specs/2026-10-05-robustness-gate-design.md`.

## Prerequisites

- Image tools: `sudo apt-get install mtools xorriso exfatprogs` on top of e2fsprogs, dosfstools,
  f2fs-tools, ntfs-3g, btrfs-progs, xfsprogs, squashfs-tools, qemu-utils and fdisk. The generator
  names anything missing.
- Built artifacts: `pnpm -C ts -F @anyfs/core build`, the Node wasm bundle
  (`ANYFS_TARGET=node scripts/build_anyfs_wasm.sh`), and for `--backend native` the addon
  (`ts/packages/anyfs-native/scripts/build-linux-electron.sh`).
- Network on the first run, to fetch the syzbot images.

## Run

```sh
node ts/tests/robustness/run.mjs --backend wasm      # the gate: exit 1 on failure
node ts/tests/robustness/run.mjs --backend native    # report only; record findings
node ts/tests/robustness/run.mjs --backend wasm --only 'ext4-*,syz-ext4-*' --jobs 2
node ts/tests/robustness/run.mjs --backend wasm --loglevel 7   # kernel log verbosity (default 4)
node ts/tests/robustness/case-runner.mjs --backend wasm --image <file>   # one case, JSON lines
```

Exit codes of `run.mjs`: `0` = gate passed, `1` = the wasm gate failed, `2` = setup failed or the
harness itself is broken (missing tools or artifacts, unreadable corpus).

Data lives in `~/.cache/anyfs-robustness/` (override with `ANYFS_ROBUSTNESS_DIR`):
`generated/` (78 cases + `cases.json`), `syzbot/` (22 images), `logs/<backend>/<case>.log` and
`report-<backend>.json` (`-partial` for `--only` runs). Regenerate the corpus with
`node ts/tests/robustness/corpus/generate.mjs --force`.

## Corpus

100 cases: 78 generated (16 bases plus 62 mutations) and 22 syzbot images (manifest `syzbot.json`,
fetcher `fetch-syzbot.mjs`). The generator is idempotent and fingerprints the corpus code, so it
regenerates only when that code changes.

Reproducibility: the ext-family bases (ext2/3/4, ext4 with `errors=panic`) and their mutations are
byte-identical across regenerations. btrfs, xfs, vmdk, exfat, f2fs and ntfs bases embed UUIDs or
timestamps from their mkfs tools, so they differ byte-wise between regenerations. Their mutations
are still seeded and deterministic against a given base.

## Outcome classes

| class   | meaning                                                                                 |
| ------- | --------------------------------------------------------------------------------------- |
| `ok`    | every step completed                                                                    |
| `error` | an op failed with an ordinary error; the process stayed healthy                         |
| `fatal` | the session fired `onFatal` with a reason (panic -> abort, op watchdog, attach timeout) |
| `hang`  | no outcome within 3 min: the watchdog missed a wedge                                    |
| `crash` | the child died without an outcome                                                       |

Each record also carries `failedStep`, the step of the first error (for example `enter:0` or
`walk:0`). Unmutated bases carry an `expect` block (`parts`, `entries`, `files`, `bytes`) and the
gate checks the walk against it, so a base that mounts but lists the wrong content fails too.

wasm passes when nothing hangs or crashes, every fatal has a reason, and every unmutated base is
`ok` and matches its `expect`. Native hangs and crashes go in `FINDINGS.md`. A case whose class
changes between two runs is reported as a flip, labelled "(build changed)" when the engine differs
(the report records `build.engine`, a hash of the wasm/addon and the core dist). A flip with the
same engine is a finding.

Notes:

- The Node harness mirrors the browser worker: an uncaught error on the module-owning thread counts
  as a fatal, as `worker.ts` turns it into `host-error`.
- syzbot images are mounted with anyfs's own options, not the reproducer's (`repro_mount_opts` in
  `syzbot.json` is informational).
- Curating the syzbot set: `tools/list-syzbot-candidates.mjs`, then `fetch-syzbot.mjs --pin`.

## Mount options anyfs adds

Code: `src/core/anyfs_mount_opts.{c,h}`.

- `noload` / `norecovery` on read-only ext3/4, xfs and btrfs (no journal or log replay).
- `ufstype=ufs2` for ufs.
- `errors=continue` on read-only mounts, `errors=remount-ro` on read-write mounts, for ext2/3/4,
  FAT (vfat/msdos), exFAT, f2fs and NTFS.

Why: a superblock can request `errors=panic`, and ext4 honours it even on a read-only mount, so a
corrupt image could panic the whole LKL kernel. The `errors=` options replace that with a
per-mount error.

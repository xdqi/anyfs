# Robustness gate: findings

Results of `ts/tests/robustness/run.mjs` that are not a plain `ok` / `error` / `fatal`:
native hangs and crashes (recorded, not gated), and anything the wasm gate caught and was fixed.

Reproduce one case with `node ts/tests/robustness/run.mjs --backend <backend> --only <case>`. Its
kernel log is in `~/.cache/anyfs-robustness/logs/<backend>/<case>.log`.

Status: OPEN · FIXED (commit)

## Native backend (non-gating)

| case | class | last step | reason | status |
| ---- | ----- | --------- | ------ | ------ |

No hangs or crashes on 2026-10-06 (100 cases).

## wasm gate

| case                                                                                          | class                                                                                                                                                                                                                   | last step | root cause                                                             | status                   |
| --------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | --------- | ---------------------------------------------------------------------- | ------------------------ |
| `ext4panic-zero-docs-inode` (generated; also the reproduction of a superblock `errors=panic`) | before the fix: wasm `fatal` (`wasm module aborted: Assertion failed: 0 ... posix-host.c ... panic`, kernel log `Kernel panic - not syncing: EXT4-fs (device vda): panic forced after error`), native `crash` (SIGABRT) | walk:0    | ext4 honours the superblock `s_errors=panic` even on a read-only mount | FIXED (7e4266a, a98e7e7) |

With the fix, the full gate on 2026-10-06 gives ok 40 / error 60 / fatal 0 / hang 0 / crash 0 on both
wasm and native, and a second wasm run changed no case class.

## Residual risk (not covered by the gate)

`errors=` hardening removes the panics that a superblock policy can request. Others remain, found by
reading the LKL tree (`~/linux`) and `~/oot-fs`:

- **BUG()/BUG_ON() still panic.** CONFIG_BUG=y and arch/lkl has no BUG override, so the generic
  BUG() calls `panic("BUG!")`, then `lkl_ops->panic`, which is `assert(0)` in
  `tools/lkl/lib/posix-host.c`. `errors=` has no effect on this. Approximate site counts
  (reachability from crafted images not checked): ext4 ~200 (e.g. `ext4_read_inline_folio` in
  inline.c:195 and :515); jbd2 ~110 J_ASSERT/BUG_ON (read-write mounts only, since read-only mounts
  use `noload`); btrfs ~150, plus `btrfs_panic()`, which ends in BUG() whatever `fatal_errors` is;
  hfs/hfsplus 24 (e.g. hfsplus/bfind.c:64); udf 9 (super.c:1449); f2fs 10 plain BUG_ON
  (checkpoint.c:225, node.c:1228; `f2fs_bug_on` is only a WARN since F2FS_CHECK_FS=n); fat 10;
  NTFS PLUS 21; ufs 3; isofs 1 (compress.c:222); xfs 22, but ASSERT is compiled out (XFS_DEBUG and
  XFS_WARN are off).
- **OOM.** LKL has a fixed memory pool and no user tasks to kill, so the OOM killer ends in
  `panic("System is deadlocked on memory")` (mm/oom_kill.c:1181). An image that forces large kernel
  allocations can reach it.
- **No memory isolation.** An out-of-bounds or NULL access caused by corrupt metadata raises no oops:
  it crashes the native process (SIGSEGV) or traps the wasm module (fatal). A kernel stack overflow
  on LKL thread stacks does the same.
- **Log replay on read-write mounts.** Read-write mounts still run ext3/4 journal replay, xfs log
  recovery and btrfs log replay. Read-only mounts skip them (`noload` / `norecovery`).
- **Remaining explicit `panic()` calls** in ext4, fat, exfat, f2fs, NTFS PLUS, jfs, nilfs2, hpfs and
  gfs2 are reachable only with an `errors=panic` mount option, which anyfs never passes. Among the
  built-in filesystems only ext2/3/4 read the policy from disk. jfs, nilfs2 and hpfs default to
  remount-ro; gfs2 defaults to withdraw.
- **ZFS** can still panic through SPL VERIFY/ASSERT, but a mount cannot reach it, since it needs
  `zpool import` first.
- **ext4 writes the superblock on a read-only mount** (not a panic). `ext4_handle_error` checks
  `bdev_read_only`, not `sb_rdonly`, and LKL's virtio-blk never marks the device read-only, so
  `save_error_info` + `ext4_commit_super` run on error. A read-only session rejects those writes
  (EIO plus log noise). A writable session entered with `ANYFS_MOUNT_RDONLY` can have its superblock
  modified. This was already true for images with `s_errors=continue`, the mke2fs default.
- **Read-write `errors=remount-ro` is not covered end to end.** The corpus only enters read-only; that
  path is covered by the unit test (`tests/unit/test_mount_opts.c`) and the kernel source reading
  above.

## Corpus notes

- None of the 229 ext4 syzbot bugs has a superblock with `errors=panic`, so acceptance criterion 4
  (a corrupt image that requests a panic must not take the process down) is met by the generated
  `ext4panic-*` case.
- Some syzbot images contain names that are not valid UTF-8 (`syz-hfsplus-e76bf3d1`,
  `syz-iso9660-4d7cd7dd`, `syz-exfat-98cc76a7`, `syz-ntfs-cfc6e810`). The walk lists the name, the
  JSON/JS string API turns the bad bytes into U+FFFD or another code point, and the following `stat`
  of that path returns ENOENT (`rc=-2`; the report paths show e.g. `fi\ufffd\ufffd\ufffd\ufffd`,
  `file\u0080`). This is an encoding artifact of the string API, not corruption handling, and it is
  identical on wasm and native.

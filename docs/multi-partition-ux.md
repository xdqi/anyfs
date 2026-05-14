# Multi-Partition UX (Cross-Surface Design)

## Why this doc

Partition-tabled disk images are the common case; every surface
handles them differently today:

| Surface | Today |
|---|---|
| Core API | partition-aware (`anyfs_mount(disk_id, part, …)`, `anyfs_disk_partitions`) |
| GUI (`src/gui/anyfs_gui.c:1454`) | Probes every partition by trial-mount to fill a picker — heavy, prompts for LUKS |
| CLI shell (`src/cli/shell.c`) | `mount [fstype] [part]` — user guesses the partition number |
| FUSE (`src/fuse/anyfs_fuse.c`) | `-o part=N`, no discovery |
| ksmbd / nfsd | `-p N`, one server = one partition |
| 7z plugin (`src/7z-plugin/LklSession.cpp:25`) | Hard-coded `part=0` |
| DC WFX plugin (planned) | Designed for `p1/`, `p2/` |

This doc unifies the UX and pulls the shared logic into one place.

## Design model

### Path layout

```
<image>/                           (synthetic parent — does NOT live inside LKL)
├── p1/                            partition 1
├── p2/                            partition 2
└── p3/                            …
```

If the disk has **no partition table** (single whole-disk filesystem like a
loop-mounted ext4 file), drop the `p<n>/` layer and expose the filesystem
contents at `<image>/` directly.

Partition numbers follow the **partition table**, not discovery order, so
they stay stable: `p1` is what's at table-slot 1, even if `p3` is
unmountable. Unmountable partitions still appear in the listing but
descending into them errors out.

### Lazy mount

**Mount fires on first descent into `p<n>/`, not at disk open.**
Per-partition state: `NEW → MOUNTING → MOUNTED | FAILED`. `FAILED` is
cached so greedy clients don't redrive on every `stat`.

Why lazy:

1. NTFS / btrfs mount is slow (journal replay, subvol scan); skip for unvisited partitions.
2. LUKS partitions shouldn't prompt just to enumerate them.
3. A corrupt partition shouldn't block listing the others.
4. Close-disk is uniform: release whatever happens to be live.

### When does mount actually fire?

`ls -l`, GNOME Files, Windows Explorer, indexers all `opendir`
children to fill size/icon columns. **Lazy degrades to eager the
moment a greedy client expands the disk.** Can't fix this without
lying about partition contents. Rule, by VFS op on `<image>/p<n>`:

| Op | Action | Why |
|---|---|---|
| `getattr` / `stat` / `access` | Synthesize directory. No mount. | Size/icon fills don't fire N mounts. |
| `opendir` + `readdir`, or anything below | Mount. | Have to know the contents. |

Interactive surfaces (CLI `cd`, 7z double-click, DC descent, GUI
picker) are clean — lazy works as designed. Transparent surfaces
(FUSE / SMB / NFS) eat the cost on first parent expansion. Mitigation:

- **Cache FAILED** so retries don't restorm.
- **`--prefetch`** on FUSE: mount all up front in parallel (just
  `anyfs_disk_enter` in a loop). Servers don't need this — their
  explicit `--share` list *is* the prefetch set.

## Layering on top of anyfs-core

The new code is **additive** on `src/core/anyfs.c`. `/lklmnt/<name>/`
stays the only kernel mount root.

```
Surface  ─►  path translation  "<image>/p<n>/foo"  →  "/lklmnt/<sess>/foo"
              │
              └─ anyfs_disk_*  (new, src/core/anyfs_disk.c)
                    │  state machine, fstype/FAILED cache, /lklmnt/ naming
                    └─ anyfs.h  (unchanged: disk_add/remove, mount/umount, partitions)
```

The synthetic `<image>/` parent lives at the surface layer only; LKL
only sees `/lklmnt/<sess>/` for partitions currently `MOUNTED`.

### What anyfs-core gives us free

- `anyfs_disk_add/remove`, `anyfs_mount/umount`, `anyfs_disk_partitions` — the existing primitives.
- `anyfs_mount(…, "auto", …)` does the expensive `/proc/filesystems` fstype-loop; the session layer caches the resulting fstype so a later enter can skip it.
- atexit handler in `anyfs_kernel_init` scans `/proc/mounts` and unmounts everything under `/lklmnt/` — session layer can leak its bookkeeping and resources still get freed.

### Naming inside `/lklmnt/`

```
/lklmnt/anyfs_d<disk_id>_p<part>     e.g.  /lklmnt/anyfs_d3_p1
```

Prefixed `anyfs_` to coexist with the existing hand-picked names
(`share`, `nfs`, …) during migration. Unique by construction within a
process. Fits in `AnyfsMount.mount_point[64]`. Internal detail —
surfaces never expose it.

### Caches

Two caches, both invalidated on `anyfs_disk_close`:

- **fstype**, after first successful mount — lets `enter` after `leave` skip the `/proc/filesystems` loop.
- **FAILED**, with reason — greedy `opendir` clients don't restorm.

No caching of in-partition stat/readdir; the kernel page cache is the truth.

### Cheap probe: sysfs + FS sniffing

Two sources, mirroring what userspace `lsblk` does:

1. **kernel sysfs** for geometry & topology — free. The kernel already
   parsed MBR/GPT/extended tables when the disk was added; sysfs hands
   us partition entries (including MBR logical `p5+`) at
   `/sys/block/<vdN>/<vdN>p<n>/{start,size,partition,ro}`. We mount
   `/sys` in `anyfs_kernel_init` and remember each disk's `vd<x>`
   name on `anyfs_disk_add`.
2. **superblock magic** for content type ("is this LVM, LUKS, ext4?") —
   needed to dispatch `AnyfsPartKind` and to decorate the listing.

For (2) we do the bare minimum in v1, then graduate to **real
libblkid** in v2.

#### v1: inline `kindprobe` (~80 lines)

We only *need* enough to dispatch `AnyfsPartKind` correctly. Three
magics are load-bearing:

| Kind | Magic / offset |
|---|---|
| `LUKS` | `"LUKS\xBA\xBE"` at 0 |
| `LVM_PV` | `"LABELONE"` at +0x218, `"LVM2 001"` at +0x20 of label header |
| `NESTED_PARTITION_TABLE` | `0x55AA` at 510 (MBR) **or** `"EFI PART"` at LBA1 (GPT) |
| `FS` | none of the above |
| `UNKNOWN` | (reserved; emitted only when v2 probing fails) |

Anything that matches none of the container magics is tagged `FS` and
left for the kernel to figure out at mount time. fstype/label/UUID
columns in `.partitions` show `?` for now.

#### v2: integrate real libblkid

util-linux ships a stable C library that detects ~80 filesystems plus
labels, UUIDs, version, used/free hints, GPT type GUID, etc. Feeding
it works fine in LKL: open the partition's block dev via
`lkl_sys_open`, then drive libblkid with `blkid_probe_set_device(pr,
lkl_fd, 0, partition_size)` (libblkid does its own `pread`, so it
doesn't care that the fd points into an LKL kernel rather than the
host). Build cost: add `libblkid` to the meson dep list — it's
already present on every distro we target. Replaces the `FS` /
`UNKNOWN` rows with real fstype/label/UUID; the v1 kind dispatch
above stays as a fast first pass so libblkid doesn't have to probe
every partition just to find the LVM PVs.

The v1→v2 transition is **purely additive** — the API shape and the
session-layer code don't change; only the columns that show `?`
become populated.

We do **not** synthesise `/dev/disk/by-uuid/…` symlinks. UUIDs are
decorative; addressing is by partition path.

## Recursive containers: one abstraction, three flavours

A "partition" doesn't always contain a mountable filesystem. Three
cases where descending one level should produce *more* enterable
sub-partitions:

- **LVM2_member** — partition is an LVM PV; activating its VG exposes N LVs.
- **LUKS** — partition is encrypted; entering it (with key) reveals one inner block device.
- **Nested partition table** — sda1's contents are themselves an MBR or GPT disk (`KIND_NESTED_PARTITION_TABLE`). Less common, but real: user's example "MBR's sda1 contains a GPT-formatted disk", or any time a partition was used as a raw backing store for another disk image.

```c
typedef enum {
    ANYFS_PART_KIND_FS,                     /* plain partition with a filesystem */
    ANYFS_PART_KIND_LVM_PV,                 /* LVM PV → LVs */
    ANYFS_PART_KIND_LUKS,                   /* encrypted → one inner dev */
    ANYFS_PART_KIND_NESTED_PARTITION_TABLE, /* MBR or GPT inside a partition */
    ANYFS_PART_KIND_UNKNOWN,                /* nothing we recognise */
} AnyfsPartKind;
```

### Why the three are the same problem

The unifying insight: in the LKL kernel, each of these resolves to
**"create a new block device, let the kernel handle the rest."** Once a
new block dev exists, the kernel parses any partition table on it
automatically, sysfs populates `/sys/block/<newdev>/<newdev>p<n>/`, and
our existing sysfs walker works unchanged. Recursion at arbitrary depth
becomes the kernel's problem, not ours.

| Container | "Create new block dev" mechanism |
|---|---|
| LVM2_member | `DM_TABLE_LOAD` (target `linear`) per LV, mapping LV byte ranges onto the PV; `DM_DEV_RESUME` activates. Kernel auto-scans table on the resulting dm device. |
| LUKS | `DM_TABLE_LOAD` (target `crypt`) with the unlock key, linear over the partition. Same flow. |
| Nested partition table | `DM_TABLE_LOAD` (target `linear`) single segment, mapping the outer partition's byte range as a "whole disk". Kernel scans MBR/GPT on the new dm device. |

So the same dm-linear plumbing covers LVM (per-LV) and nested partition
tables (one segment for the whole thing). LUKS swaps `linear` for
`crypt`. Concretely there are **two ioctl recipes** (linear, crypt),
not three.

### What this means for the session layer

`anyfs_disk_enter(part)` becomes a small dispatch on `kind`:

| kind | What `enter` does |
|---|---|
| FS | `anyfs_mount` as today; returns the `/lklmnt/...` path |
| LVM_PV | Parse LVM metadata; `DM_TABLE_LOAD` per LV; on success the LVs appear in sysfs and are loaded as child `AnyfsPartInfo`s (kind probed via `kindprobe`). Returns *no* mount path — caller is expected to recurse. |
| LUKS | Prompt for key (surface-specific channel); `DM_TABLE_LOAD` crypt; one child appears in sysfs, probe it, recurse. |
| NESTED_PARTITION_TABLE | `DM_TABLE_LOAD` linear over the partition's byte range; kernel parses the inner table; children appear in sysfs; populate child `AnyfsPartInfo`s. No mount; caller recurses. |
| UNKNOWN | FAILED with reason. |

Tree shape: `AnyfsDisk` becomes implicitly tree-shaped. Each
`AnyfsPartInfo` may have child `AnyfsPartInfo`s materialised on enter.
`anyfs_disk_list(parent)` returns its direct children
(where parent=`NULL` is the disk root).

### Shared dm-helper (v2)

Both the LVM and nested-partition-table paths want the same primitive:
"map a byte range of an existing block device into a new linear dm
device, let the kernel scan it." Pull it out:

```c
/* Build a dm-linear device named `name` whose `length` bytes map starting
 * at `offset` of `parent_blkdev`. Returns the new block dev path
 * (e.g. "/dev/dm-3") on success. */
int anyfs_dm_linear(const char *parent_blkdev, uint64_t offset, uint64_t length,
                    const char *name, char out_blkdev[64]);
int anyfs_dm_remove(const char *name);
```

~100 lines wrapping `ioctl(/dev/mapper/control, DM_TABLE_LOAD …)` etc.
LKL's dm code is already configured in for LUKS support — see the
ksmbd/nfsd kernel configs.

### v2 specifics

- **LVM**: parse on-disk text metadata (PV header at +0x200 + text blob at end of metadata area; ~200 lines), then one `anyfs_dm_linear` per LV segment. Guardrails: single-disk VGs only; linear LVs only (striped/RAID/thinpool/snapshot → `KIND_UNKNOWN`); read-only.
- **Nested partition table**: ~20 lines — one `anyfs_dm_linear` over the whole partition, then walk sysfs on the resulting dm device.
- **LUKS**: `target=crypt` instead of `linear`, plus key handling (uses the `?keyref=…` syntax above). Worth its own doc when implemented.

### Surface scoping for v1 recursion

Nesting supported in v1 on **CLI / GUI / 7z / DC** (they're naturally
recursive UIs). **FUSE / ksmbd / nfsd** v1 lists containers but
descent fails — the path-translation code only walks one level. v2
upgrades it to walk the `AnyfsPartInfo` tree segment-by-segment until
hitting `KIND_FS`.

## Shared core: `anyfs_disk_*` session API

New file `src/core/anyfs_disk.c` + `include/anyfs_disk.h`. Calls the
existing `anyfs.h` primitives; `anyfs.h` itself doesn't grow.

```c
typedef struct AnyfsDisk AnyfsDisk;

typedef struct {
    unsigned int  index;         /* 1-based partition number */
    uint64_t      offset_bytes;
    uint64_t      size_bytes;
    char          ptype[40];     /* MBR hex ("0x83") or GPT type GUID */
    AnyfsPartKind kind;
} AnyfsPartInfo;

typedef enum {
    ANYFS_PART_NEW = 0, ANYFS_PART_MOUNTING, ANYFS_PART_MOUNTED, ANYFS_PART_FAILED,
} AnyfsPartState;

int  anyfs_disk_open(const char *image_path, uint32_t flags, AnyfsDisk **out);
int  anyfs_disk_list(AnyfsDisk *d, AnyfsPartInfo *buf, size_t buf_n, size_t *got);

/* Idempotent. Returns 0 + LKL path on success, FS error on fail (cached).
 * Concurrent callers serialise per-partition; second caller sees the first's result. */
int  anyfs_disk_enter(AnyfsDisk *d, unsigned int part, uint32_t flags, char lkl_path[64]);

AnyfsPartState anyfs_disk_state(AnyfsDisk *d, unsigned int part);
int  anyfs_disk_probe(AnyfsDisk *d, unsigned int part,
                      char fstype[32], char label[64], uint64_t *used);
int  anyfs_disk_leave(AnyfsDisk *d, unsigned int part);
void anyfs_disk_close(AnyfsDisk *d);  /* idempotent; atexit-safe */
```

Concurrency: one mutex per `AnyfsDisk` for the state table, plus a
per-partition condvar so `enter` serialises without holding the table
lock during the actual `anyfs_mount`.

Lift in: `src/gui/anyfs_gui.c:1454-1526` (partition enumeration +
probe) and `src/core/anyfs.c:389-401` (`anyfs_disk_partitions`).

## The path DSL (shared by all surfaces)

```
[disk<N>/]p<n>[?<query>](/p<m>[?<query>])*

p1                              single-disk shorthand → disk0/p1
p2/p1                           partition 1 inside container p2
disk1/p2/p1?keyref=K1           explicit disk + LUKS credentials
```

Rules:
- `disk<N>` is the **only** disk-prefix form (0-indexed by registration order). Absent → `disk0`.
- Component is `p` + digits, optionally followed by `?<query>`.
- Numbering is **per parent**: `p1` of disk0 is unrelated to `p1` inside `p2`.
- Order is deterministic: physical partitions by partition-table order; LVs by LVM metadata order; nested-table children by inner-table order.
- Empty path = "the disk itself" (whole-disk tools only).

### Credentials (LUKS, future encrypted formats)

```
<query>  := <pair>[&<pair>]*
<pair>   := keyref=<envvar>           (recommended — secret stays out of argv)
          | keyfile=<percent-encoded-path>
          | keyfd=<int>               (for systemd/docker — caller inherits the fd)
          | key=<percent-encoded>     (testing only; emits a stderr warning)
```

`keyfile` values URL-encode `/`, `?`, `&`, `#`, `%`, whitespace.
Percent-decoding happens **after** the path is split into components,
so encoded slashes never split a component. Multi-layer credentials
chain naturally — each component carries its own `?...`.

### Multi-disk: positional, indexed `disk<N>`

```
lkl_ksmbd boot.img data.qcow2?keyref=DATA_KEY \
    --share esp=disk0/p1 \
    --share home=disk1/p1
```

The N-th positional image is `disk<N-1>` (0-indexed). Trailing
`?<query>` on the image path is reserved for future disk-level
credentials (qcow2 encryption etc.). In **single-disk mode**, a share
path missing `disk<N>/` is auto-rewritten to `disk0/<path>`; in
**multi-disk mode** it's rejected. Out-of-range `diskN/` rejected.
Same image passed twice → two indices, two opens (rare; we don't
detect).

Inside each surface this is just an `AnyfsDisk *` array indexed by
`N` — no user labels, no collision rules. Surfaces own the array
because lifetimes differ (CLI interactive vs. server fixed).

## Per-surface integration

Each surface wraps `anyfs_disk_*` with path translation:
`<image>/p<n>/foo` ↔ `/lklmnt/<sess>/foo`.

### FUSE — `src/fuse/anyfs_fuse.c`

- Default: positional `<image>` (repeatable for multi-disk), FUSE root shows partition listing (or per-disk subdirs in multi-disk mode).
- `-o part=<path>` accepts canonical path format (`p1`, `p2/p1`, `disk0/p1`, …) and skips the synthetic layer — mounts that partition directly at the FUSE root. Bare integer `-o part=2` → `-o part=p2` for back-compat.
- `-o prefetch` pre-enters every partition at startup (across all disks).
- VFS ops follow the "When does mount actually fire?" table. `.partitions` (and `partitions.txt`) live at the FUSE root, generated from `anyfs_disk_list`; in multi-disk mode the unified `.partitions` is at root and per-disk `.partitions` at `/disk<N>/`.

### CLI shell — `src/cli/shell.c`

- `partitions` — list from `anyfs_disk_list`, no probe.
- `probe [n]` — `anyfs_disk_probe` for one or all partitions.
- `enter [path]` (alias `cd`) — `anyfs_disk_enter`. Keep `mount` as a back-compat alias; bare `mount` on a multi-partition disk prints the table and a hint instead of auto-picking.

### `anyfs-lspart` standalone tool

Open one or more images, walk them (cheap probe only, no mounts, no
LUKS prompt), print a unified table.

```
$ anyfs-lspart boot.img data.qcow2
PATH                SIZE     KIND   FSTYPE  LABEL     UUID
disk0/p1            512 MB   FS     ext4    /boot     9a3c…
disk0/p2            20 GB    LVM_PV -       -         -
disk0/p2/p1         18 GB    FS     ext4    rootfs    1f88…  (--activate, v2)
disk1/p1            100 GB   FS     ext4    home      04C2…
disk1/p2            50 GB    NESTED -       -         -
```

The `PATH` column drops directly into a server `--share` flag —
provided the same images are passed in the same order. With v1
`kindprobe` only, `FSTYPE`/`LABEL`/`UUID` show `?` for non-container
rows; v2 (libblkid) fills them in.

Flags: positional `<image>[?<query>]` repeatable; `--activate` (v2)
descends into containers; `--json` for scripts.

### `.partitions` synthetic file

Same table as `anyfs-lspart`, exposed read-only at the synthetic
root: `<image>/.partitions`. Useful when the user is already
*inside* a mounted disk via FUSE / SMB / NFS / 7zFM / DC. Never
triggers a mount; generated from `anyfs_disk_list`. Also exposed as
`partitions.txt` for protocols that filter dotfiles. Shared printer
with `anyfs-lspart`.

### ksmbd / nfsd — `src/{ksmbd,nfsd}/lkl_*.c`

**Multi-disk, multi-share, leaf-only. No disk-mode, no synthetic
root.** Discovery is `anyfs-lspart`'s job; the server only exposes
the explicit `--share` list.

```
lkl_ksmbd boot.img data.qcow2 \
    --share esp=disk0/p1 \
    --share rootfs=disk0/p2/p1?keyref=ROOT_KEY \
    --share home=disk1/p1
```

- Positional `<image>` repeatable → `disk0`, `disk1`, ….
- `--share name=path` repeatable (`name=` optional; auto-derived `disk0/p1` → `disk0_p1`).
- Each `path` must resolve to a `KIND=FS` leaf; containers (`LVM_PV`, `NESTED_PARTITION_TABLE`) rejected at startup with a pointer to `anyfs-lspart`.
- Bare integer (`--share 2`) = `--share p2` for back-compat (single-disk only).
- The explicit `--share` list *is* the prefetch set — mounted at startup, no lazy mount on the server side.

nfsd is the same: each `--share` becomes an export rooted at
`/<auto-name>` (e.g. `/disk0_p1`).

### 7z plugin — `src/7z-plugin/`

Swap `LklSession.cpp:25`'s `lklb_mount(disk_id, 0, …)` for
`anyfs_disk_open`. `LklFolder`'s root listing becomes the partition
list; descending `p<n>` calls `anyfs_disk_enter`.

### DC WFX plugin — `docs/doublecmd-wfx-plan.md`

`FsFindFirstW("\disk.qcow2\")` enumerates from `anyfs_disk_list`;
`FsFindFirstW("\disk.qcow2\p1\…")` triggers `anyfs_disk_enter`.
`FAILED` → `INVALID_HANDLE_VALUE` + `ERROR_PATH_NOT_FOUND`. Update
the plan doc.

### GUI — `src/gui/anyfs_gui.c`

- Replace the eager trial-mount probe loop with `anyfs_disk_list` (cheap) for the initial render.
- Run `anyfs_disk_probe` in a background thread to fill labels (`Partition 1 (ext4, 512MB)`) as they arrive.
- "Browse whole disk" → synthetic root; user descends in the file list.
- Multiple opened images become top-level `disk0`, `disk1`, … entries (with the filename as display text).

## Edge cases & explicit non-goals

- **No partition table** — `anyfs_disk_list` returns zero entries; surfaces flatten the synthetic layer and expose the filesystem at `<image>/` directly.
- **Single-partition disks** — still get `p1/` in the listing for consistency. Surfaces *may* choose to auto-flatten when there's exactly one partition, but I'd keep it visible — it's predictable, and "where did p1 go?" beats "wait, this was multi-partition yesterday".
- **Unmountable partitions** — listed; descent returns `EIO`. Display label can include partition type (`p3  [swap]`, `p4  [unknown 0x83]`) using the ptype field. Don't hide them.
- **LUKS / dm-crypt** — out of scope for v1. Returns FAILED on enter; document a separate `unlock` CLI command for interactive surfaces; nfsd/smbd cannot prompt, so document that LUKS partitions must be unlocked via the CLI before starting the server.
- **LVM inside a partition** — see "Recursive containers" section above. v1: detected by `kindprobe`, listed with `kind=LVM_PV`, descent fails with a clear error pointing at the future v2 work. v2: activated, LVs exposed as nested partitions following the same `p<n>` path convention, but only on interactive surfaces (CLI/GUI/7z/DC); FUSE/SMB/NFS get recursive path translation in a later increment.
- **Idle eviction** — `anyfs_disk_leave` exists but no surface uses it by default; turn on later if memory pressure shows up.

## File map (when this lands)

| File | Action | Purpose |
|------|--------|---------|
| `include/anyfs_disk.h` | New | Session API declared above |
| `src/core/anyfs_disk.c` | New, ~400 lines | Implementation: session state machine, locking, caches |
| `src/core/kindprobe.c`, `kindprobe.h` | New, ~80 lines (v1) | Three magic checks: LUKS / LVM_PV / nested partition table. Everything else → `KIND_FS`. |
| `src/core/kindprobe.c` | Extend, **v2** | Replace `FS`/`UNKNOWN` rows by driving libblkid against the partition's LKL fd; populates fstype/label/UUID. v1 kind dispatch stays as a fast first pass. |
| `meson.build` (top-level) | Modify | Add `libblkid` to `dependencies` (v2). |
| `src/core/anyfs_sysfs.c`, `anyfs_sysfs.h` | New, ~120 lines | sysfs walker: `/sys/block/<vdN>/<vdN>p<n>/{start,size,partition,ro}` → `AnyfsPartInfo[]`; works on dm-* devs too |
| `src/core/anyfs_dm.c`, `anyfs_dm.h` | New, ~150 lines, **v2** | `anyfs_dm_linear` + `anyfs_dm_crypt` ioctl wrappers; shared by LVM/LUKS/nested |
| `src/core/anyfs_disk_dump.c` | New, ~80 lines | Shared table printer for `anyfs-lspart` + `.partitions` synthetic file |
| `src/core/meson.build` | Modify | Add `anyfs_disk.c`, `kindprobe.c`, `anyfs_sysfs.c`, `anyfs_disk_dump.c` to the core lib |
| `src/lspart/anyfs_lspart.c` | New, ~150 lines | Standalone discovery tool; calls `anyfs_disk_open` + dump |
| `src/lspart/meson.build` | New | Build the binary |
| `src/cli/shell.c` | Modify ~80 lines | `partitions` / `probe` commands; route `mount` / `cd` through `anyfs_disk_enter` accepting path strings |
| `src/fuse/anyfs_fuse.c` | Modify ~140 lines | Synthetic root + lazy enter; `.partitions` synthetic file; `-o part=<path>`, `-o prefetch` |
| `src/gui/anyfs_gui.c` | Modify ~150 lines | Background-probe refactor of the picker |
| `src/ksmbd/lkl_ksmbd.c` | Modify ~90 lines | Repeatable positional disks + repeatable `--share`, multi-disk dispatch, leaf-only validation, path-DSL with credentials |
| `src/nfsd/lkl_nfsd.c` | Modify ~90 lines | Same |
| `src/core/path_dsl.c`, `path_dsl.h` | New, ~120 lines | Parse `disk1/p2/p1?keyref=K1/p3` strings into `(disk_idx, (component, cred)*)`; URL-decode; shared by all surfaces |
| `src/7z-plugin/LklSession.cpp`, `LklFolder.cpp` | Modify ~80 lines | Partition root listing; lazy enter on subfolder open |
| `docs/doublecmd-wfx-plan.md` | Modify | Reference this doc for the lazy-enter semantics |

## Verification

1. **kindprobe + sysfs walker** — feed crafted superblock byte fixtures (`tests/fixtures/kindprobe/`: LUKS, LVM2 PV header, MBR-in-MBR, plain ext4); confirm the three load-bearing magics dispatch and everything else lands `KIND_FS`. Then open a multi-partition raw fixture under LKL and walk `/sys/block/vda/`, confirming entries (including logical `p5+`) match the fixture.
2. **Core session** — open a multi-partition raw fixture, `list` returns N entries with NEW state and zero LKL mounts (`wc -l < /proc/mounts` == 0 inside LKL); `enter(2)` mounts only partition 2, `state(1) == NEW` afterwards, `close()` leaves no LKL mounts. v2: rerun and confirm fstype/label/UUID columns populated by libblkid.
3. **FUSE bare stat** — `anyfs-fuse multi.img /mnt; ls /mnt` shows `p1 p2 …` and `.partitions` with zero LKL mounts; `cat /mnt/.partitions` produces the table, still zero mounts.
4. **FUSE greedy stat** — `ls -l /mnt` does an `opendir` per child → mounts p1, p2, p3 (documented). FAILED partitions stay FAILED on the second `ls -l` (no retry storm).
5. **FUSE prefetch** — `anyfs-fuse -o prefetch multi.img /mnt`: mountinfo shows all partitions immediately; subsequent `ls -l` does zero new mounts.
6. **CLI** — `partitions` shows table without prompting LUKS; `probe 2` probes only p2; `cd p2` enters.
7. **`anyfs-lspart`** — prints the partition table with the canonical `PATH` last column; `--json` produces parseable output; exit code 0 on clean parse.
8. **Path round-trip** — feed `anyfs-lspart`'s `PATH` column directly to `lkl_ksmbd --share` / `lkl_nfsd --share` / `anyfs-fuse -o part=`; all three accept the same string and serve the right partition.
9. **Server multi-share** — `lkl_ksmbd disk.qcow2 --share a=p1 --share b=p3` produces two SMB shares from one process, both mountable concurrently. Same for nfsd. `wc -l < /proc/mounts` inside LKL == 2 at startup.
10. **Leaf-only validation** — `lkl_ksmbd disk.qcow2 --share bad=p2` where p2 is `LVM_PV` exits non-zero with a message naming `anyfs-lspart`; no server starts.
11. **Credentials path-DSL** — `lkl_ksmbd disk.qcow2 --share root=p2/p1?keyref=K1`; with `K1` set, unlock succeeds; with `K1` unset, startup fails clearly. Same end-to-end with `keyfile=…` and `keyfd=…`. `ps` while server is up: literal `key=...` triggers stderr warning, `keyref=...` shows no secret in argv.
12. **smbd / nfsd browsing** — `mount` from a client of each share, `ls` works, file copy round-trips with sha256 match.
13. **Multi-disk server + lspart round-trip** — `lkl_ksmbd a.img b.img --share esp=disk0/p1 --share home=disk1/p1` serves two shares from different images; client sees both, files round-trip; `/proc/mounts` shows 2 mounts. `anyfs-lspart a.img b.img` produces a unified table whose `PATH` column drops back in verbatim. Same for nfsd.
14. **Multi-disk argument errors** — missing prefix (`--share x=p1` with two disks) → "path 'p1' must start with diskN/"; out-of-range (`--share x=disk7/p1` with one disk) → "disk7 not registered". Both exit non-zero, no server starts.
15. **Single-disk default** — `lkl_ksmbd a.img --share x=p1` is equivalent to `--share x=disk0/p1`; both produce the same share.
16. **7zFM** — open a multi-partition image, root view shows partitions, double-click descends and triggers mount.
17. **Corrupt partition** — corrupt one partition's superblock; verify it stays listed, descent errors, other partitions remain usable, `.partitions` reports `FAILED` for it.

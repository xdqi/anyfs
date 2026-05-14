# doublecmd-wfx: Double Commander Filesystem Plugin Plan

## Goal

Expose anyfs-reader to **Double Commander** (DC) as a WFX (filesystem)
plugin so users can open disk images (raw / qcow2 / vmdk / vdi / vhd) and
browse the contained Linux/Windows/etc. filesystems (ext4, btrfs, xfs, f2fs,
FAT, NTFS, …) directly from inside DC — same idea as the existing 7-Zip
File Manager plugin, but inside a real two-pane file manager, on Linux **and**
Windows.

Produces a single shared library `anyfs.wfx` (`.so` on Linux, `.dll` on
Windows), built via meson behind a `-Ddc_plugin=true` option.

## Why a WFX plugin (not FUSE / SMB / out-of-process)

| Option | Verdict |
|---|---|
| **WFX plugin wrapping libanyfs** (this plan) | Cross-platform, in-process, mirrors the 7zFM plugin model. |
| Mount with `anyfs-fuse`, open mount in DC | Linux/macOS only; no DC-side UX; no Windows. |
| anyfs ksmbd/nfsd + DC's SMB/NFS clients | Heavy; kernel servers; nothing DC-specific gained. |
| WFX plugin spawning `anyfs-fuse` per image | Two moving parts, IPC for nothing — LKL is already in-process. |

## Background — what each side offers

### anyfs-reader embedding API (already shipped)

- `include/anyfs.h` — minimal C API:
  - `anyfs_kernel_init / anyfs_kernel_halt`
  - `anyfs_disk_add(image_path, flags) → disk_id` (auto-detects backend)
  - `anyfs_disk_partitions(disk_id)`
  - `anyfs_mount(disk_id, part, fstype|"auto", name, flags, out) → /lklmnt/<name>`
  - `anyfs_umount(name)`, `anyfs_disk_remove(disk_id)`
- After mount, callers use **LKL syscalls** directly:
  `lkl_sys_open`, `lkl_sys_getdents64`, `lkl_sys_read`, `lkl_sys_lseek`,
  `lkl_sys_stat`, `lkl_sys_close`.
- Closest precedent in-tree: `src/7z-plugin/` (the 7-Zip FM plugin).
  - `lkl_bridge.c` — wraps the LKL syscalls into a "browser" shape (readdir,
    open, read, stat). Most of this is directly reusable.
  - `LklFolderManager.cpp` — session registry (image_path ↔ mount name ↔
    disk_id). Map the same shape into C.

### Double Commander WFX plugin ABI

- SDK: `~/doublecmd/sdk/wfxplugin.h` (plain C ABI, calling convention
  via `DCPCALL`). No Pascal/Lazarus required to *build* a plugin.
- Plugin = shared library named `*.wfx` (`.so` / `.dll` / `.dylib`).
- Loader: `~/doublecmd/src/uwfxmodule.pas` (TWFXModule, `dynlibs`-based).
- Mandatory exports: `FsInit`, `FsFindFirst`, `FsFindNext`, `FsFindClose`.
- Optional but needed for a useful read-only filesystem:
  `FsGetFileW`, `FsExecuteFileW`, `FsDisconnectW`, `FsGetDefRootName`,
  `FsExtractCustomIconW`, `ExtensionInitialize`, `ExtensionFinalize`,
  `FsContentGetSupportedField` / `FsContentGetValueW` (optional, exposes
  Unix mode / uid / gid as columns).
- Reference plugins:
  - Minimal: `~/doublecmd/plugins/wfx/sample/src/sample.lpr` (~150 lines, Pascal).
  - Production (mirrors our "list of connections" UX):
    `~/doublecmd/plugins/wfx/ftp/src/ftpfunc.pas`.

## Plugin UX

```
\\anyfs\                          (FsGetDefRootName → "anyfs")
├── <Open image…>                 synthetic entry — F3/Enter → file dialog → anyfs_disk_add
├── ubuntu.qcow2/                 one virtual dir per opened image
│   ├── p1/                       partition 1  (whole-disk mount if no partition table)
│   │   ├── bin/
│   │   ├── etc/
│   │   └── …
│   └── p2/
└── win10.vmdk/
    └── p1/
        └── Windows/
```

- **Root** `FsFindFirstW("\\")` → enumerate session registry + synthetic
  `<Open image…>` entry.
- **Open image** — `FsExecuteFileW(main_win, "\\<Open image…>", "open")`
  prompts via `tRequestProcW(RT_TargetDir, …)`, then `anyfs_disk_add` +
  `anyfs_disk_partitions`. Each partition gets `anyfs_mount(...,
  ANYFS_MOUNT_RDONLY, &out)`; if `anyfs_disk_partitions == 0` use part 0
  whole-disk.
- **Browsing** — translate DC virtual path `\disk.qcow2\p1\foo\bar` to LKL
  path `/lklmnt/<sess>/foo/bar` (one mount per partition, name e.g.
  `dc_<imgid>_p<n>`). For dirs: `lkl_sys_open(O_RDONLY|O_DIRECTORY)` +
  `lkl_sys_getdents64` loop. Populate `WIN32_FIND_DATAW` from `lkl_sys_stat`
  (size, mtime → `FILETIME`, `FILE_ATTRIBUTE_DIRECTORY` /
  `FILE_ATTRIBUTE_UNIX_MODE`).
- **Copy out** — `FsGetFileW`: `lkl_sys_open` RO, loop `lkl_sys_read` into
  local file; call `tProgressProcW` every chunk; honour `FS_COPYFLAGS_OVERWRITE`.
  Resume (`FS_COPYFLAGS_RESUME`) → `lkl_sys_lseek` to local file size first.
- **Eject** — `FsDisconnectW("\\disk.qcow2")` → `anyfs_umount` every partition,
  then `anyfs_disk_remove`. Ctrl-D in DC triggers this.
- **Lifecycle** — `ExtensionInitialize` calls `anyfs_kernel_init({mem_mb:64})`
  once on plugin load. `ExtensionFinalize` unmounts all sessions and calls
  `anyfs_kernel_halt`.

### Read-only first pass

Do **not** export `FsPutFileW`, `FsDeleteFileW`, `FsMkDirW`, `FsRemoveDirW`,
`FsRenMovFileW`, `FsSetAttrW`, `FsSetTimeW` in v1. The WFX API has the hooks
when we want write support later — anyfs LKL already supports rw mounts for
the FS drivers that allow it, so this is a code-add, not a redesign.

## File map (files touched)

| File | Action | Purpose |
|------|--------|---------|
| `src/dc-plugin/anyfs_wfx.c` | New, ~600 lines | DC entry points + path translation |
| `src/dc-plugin/session.c`, `session.h` | New, ~200 lines | Image registry (image_path ↔ mount names ↔ disk_id) |
| `src/dc-plugin/find_handle.c`, `find_handle.h` | New, ~150 lines | `Fs{FindFirst,FindNext,FindClose}W` directory-iter state |
| `src/dc-plugin/win32_compat.h` | New, ~80 lines | `WIN32_FIND_DATAW`, `FILETIME`, `DCPCALL`, etc. for Linux build (vendored from DC's `sdk/`) |
| `src/dc-plugin/meson.build` | New | `shared_module('anyfs', …, name_suffix:'wfx')` |
| `meson.build` (top-level) | Modify ~5 lines | `if get_option('dc_plugin'): subdir('src/dc-plugin')` |
| `meson_options.txt` | Modify ~3 lines | `option('dc_plugin', type:'boolean', value:false)` |
| `docs/doublecmd-wfx-plan.md` | This file | Plan |
| `docs/README.md` | Modify | Index entry |

## Existing code to reuse

- `include/anyfs.h`, `src/core/anyfs.c` — kernel + disk + mount lifecycle.
- `src/7z-plugin/lkl_bridge.c` — readdir loop, stat translation, path
  joining. **Lift wholesale**; the only new piece is mapping `struct stat` →
  `WIN32_FIND_DATAW` (vs. the 7z plugin's `IFolderFolder` shape).
- `src/fuse/anyfs_fuse.c` — `mode_t → FILE_ATTRIBUTE_*` mapping reference.
- `cross-win32.txt` / `cross-win64-fuse.txt` + MinGW toolchain — already
  proven for the 7z-plugin Windows build. The WFX `.dll` reuses them.

## Build

```
# Linux .wfx
meson setup build -Ddc_plugin=true
meson compile -C build
# → build/src/dc-plugin/anyfs.wfx

# Windows .wfx (i686 — match common DC builds; mirror for x86_64 if needed)
meson setup build-win32-dc --cross-file cross-win32.txt -Ddc_plugin=true
meson compile -C build-win32-dc
# → build-win32-dc/src/dc-plugin/anyfs.wfx  (a .dll renamed)
```

The plugin links the existing `anyfs` static lib + LKL + the same block
backends used by the 7z plugin (raw + qemu). `meson.build` should keep
`-Ddc_plugin=false` as default so the rest of the project's builds are
unaffected.

## Install

### Linux

```
mkdir -p ~/.config/doublecmd/plugins/wfx/anyfs
cp build/src/dc-plugin/anyfs.wfx ~/.config/doublecmd/plugins/wfx/anyfs/
```

Then in DC: **Configuration → Plugins → FS Plugins → Add**, pick the
`anyfs.wfx` file. Or edit `~/.config/doublecmd/doublecmd.xml` and add a
`<WfxPlugin>` entry.

### Windows

Drop `anyfs.dll` (renamed `anyfs.wfx` is optional) into
`<DC install dir>\plugins\wfx\anyfs\` and register the same way.

## Verification

1. **Build clean** — `meson setup build -Ddc_plugin=true && ninja -C build`;
   regression-check `-Ddc_plugin=false` is a no-op for the rest of the tree.
2. **Linux smoke test**
   - Open DC, navigate to `\\anyfs\`.
   - `<Open image…>` → pick `tests/fixtures/ext4.qcow2`. Expect a virtual
     directory `ext4.qcow2/p1/` with the fixture's contents.
   - Browse two levels deep; sizes / mtimes match `loop mount + ls -la`.
   - F5-copy a known file out; `sha256sum` matches the reference.
   - Ctrl-D → image disappears from root; re-add works.
3. **Multi-partition** — repeat with a multi-partition raw image (use the
   `tests/` fixtures); confirm `p1/`, `p2/`, … entries.
4. **Format coverage** — also try one btrfs, one ntfs, one fat fixture.
5. **Windows cross-build** — cross-compile, drop into a portable DC,
   repeat steps 2–4 from a Windows VM.
6. **Lifecycle** — start DC, do nothing in `\\anyfs\`, quit. Then start
   again, add image, quit. `ExtensionFinalize` must `anyfs_kernel_halt`
   cleanly (no leaked LKL kernel threads — check with `ps -L`).

## Open items (decide before coding)

- Whether to also expose Unix `mode` / `uid` / `gid` as DC content columns
  via `FsContentGetSupportedField` / `FsContentGetValueW` (small extra; nice
  to have for ext4 etc.).
- Custom icons per filesystem type (`FsExtractCustomIconW` returning
  `FS_ICON_FORMAT_FILE` pointing at a bundled PNG) — purely cosmetic.
- macOS port — DC has a macOS build; in-process linking on macOS needs LKL
  + dlopen check; out of scope for v1.
- Write support (rw mounts) — gated on user demand; design slot already
  reserved in the WFX exports list.

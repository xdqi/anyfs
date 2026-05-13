# anyfs-winfsp: Windows FUSE Port Implementation Plan

## Goal

Port `anyfs-fuse` to native Windows (PE32) using WinFSP as the FUSE backend.
Cross-compile with `i686-w64-mingw32-gcc` from the msys2-cross toolchain.

## File map (files touched)

| File | Action | Purpose |
|------|--------|---------|
| `src/fuse/anyfs_fuse.c` | Modify ~80 lines | Platform portability + type compat |
| `meson.build` | Modify ~15 lines | Win32 FUSE target |
| `cross-win32-fuse.txt` | New | Meson cross file for winfsp build |
| `scripts/mkwinfsp_implib.sh` | New | Generate MinGW import lib from WinFSP .lib or .def |
| `test/run-winfsp.sh` | New | Test script for Wine / Windows VM |

## Step 1 — Generate MinGW Import Library for WinFSP

### Problem

WinFSP distributes MSVC `.lib` files (`winfsp-x86.lib`). MinGW `ld` can't consume these
directly. Need a `.dll.a` import library.

### Chosen approach: extract from MSVC .lib → .def → .dll.a

The MSVC `.lib` contains COFF import objects for each DLL export. We can extract the
symbol list with `objdump -t`, filter for `fsp_fuse*` and `Fsp*` symbols, then use
`i686-w64-mingw32-dlltool` to produce `libwinfsp-x86.dll.a`.

```bash
#!/bin/bash
# scripts/mkwinfsp_implib.sh
# Input: ~/winfsp/opt/fsext/lib/winfsp-x86.lib
# Output: ~/winfsp/opt/fsext/lib/libwinfsp-x86.dll.a

set -e
WINFSP_DIR="$HOME/winfsp"
MINGW_PREFIX="i686-w64-mingw32"

# Step 1: extract DLL export symbols from MSVC .lib
# The .lib is an archive of COFF objects. Each DLL import stub has a symbol like
# __imp__fsp_fuse3_new or fsp_fuse3_new.
# Use objdump to list archive members and their symbols.
ARCHIVE="$WINFSP_DIR/opt/fsext/lib/winfsp-x86.lib"
DEF_FILE="/tmp/winfsp-x86.def"

# List archive members, for each find the imported DLL name and symbols
# The .idata sections contain the DLL name and function names.
# Strategy: use strings + pattern matching to find all exported names from the headers.

# Known exports from WinFSP headers (fuse/fuse.h, fuse3/fuse.h, fuse_opt.h):
cat > "$DEF_FILE" <<'EOF'
LIBRARY winfsp-x86.dll
EXPORTS
  FspFileSystemAcquire
  FspFileSystemBeginCall
  FspFileSystemCreate
  FspFileSystemDelete
  FspFileSystemEndCall
  FspFileSystemGetVolumeInfo
  FspFileSystemMount
  FspFileSystemOpendir
  FspFileSystemReaddir
  FspFileSystemReleasedir
  FspFileSystemSendResponse
  FspFileSystemSetVolumeInfo
  FspFileSystemUnmount
  FspMountPointCreate
  FspMountPointDelete
  FspServiceCreate
  FspServiceDelete
  FspServiceGetName
  FspVersion
  fsp_fuse3_destroy
  fsp_fuse3_exit
  fsp_fuse3_get_context
  fsp_fuse3_get_session
  fsp_fuse3_lib_help
  fsp_fuse3_loop
  fsp_fuse3_loop_mt
  fsp_fuse3_loop_mt_31
  fsp_fuse3_main_real
  fsp_fuse3_mount
  fsp_fuse3_new
  fsp_fuse3_new_30
  fsp_fuse3_unmount
  fsp_fuse_destroy
  fsp_fuse_exit
  fsp_fuse_get_context
  fsp_fuse_get_session
  fsp_fuse_loop
  fsp_fuse_loop_mt
  fsp_fuse_main_real
  fsp_fuse_mount
  fsp_fuse_new
  fsp_fuse_opt_add_arg
  fsp_fuse_opt_add_opt
  fsp_fuse_opt_add_opt_escaped
  fsp_fuse_opt_free_args
  fsp_fuse_opt_insert_arg
  fsp_fuse_opt_match
  fsp_fuse_opt_parse
  fsp_fuse_set_signal_handlers
  fsp_fuse_unmount
EOF

# Step 2: build MinGW import library
$MINGW_PREFIX-dlltool -d "$DEF_FILE" \
    -l "$WINFSP_DIR/opt/fsext/lib/libwinfsp-x86.dll.a" \
    -D winfsp-x86.dll

echo "Created: $WINFSP_DIR/opt/fsext/lib/libwinfsp-x86.dll.a"
```

**Note**: If the exact symbol list is incomplete (some exports may differ between WinFSP
versions), the fix is to check the actual DLL exports on a Windows machine with:
```
gendef winfsp-x86.dll  →  winfsp-x86.def
```
Or check the WinFSP source at `~/winfsp/src/dll/library.c` for `FSP_FUSE_API` exports.

## Step 2 — Platform Type Compatibility Layer

### Problem

Linux libfuse3 uses POSIX types in callbacks:

```c
// Linux libfuse3
int (*getattr)(const char *path, struct stat *stbuf, struct fuse_file_info *fi);
int (*statfs)(const char *path, struct statvfs *stbuf);
int (*utimens)(const char *path, const struct timespec tv[2], ...);
int (*read)(const char *path, char *buf, size_t size, off_t offset, ...);
```

WinFSP FUSE3 uses its own types:

```c
// WinFSP FUSE3 (Windows native)
int (*getattr)(const char *path, struct fuse_stat *stbuf, struct fuse3_file_info *fi);
int (*statfs)(const char *path, struct fuse_statvfs *stbuf);
int (*utimens)(const char *path, const struct fuse_timespec tv[2], ...);
int (*read)(const char *path, char *buf, size_t size, fuse_off_t offset, ...);
```

On Cygwin, WinFSP maps `fuse_stat` → `stat`, `fuse_off_t` → `off_t`, etc.
On native Windows (our target), they're separate types with different field widths.

### Chosen approach: portable typedefs in anyfs_fuse.c

Add a compat block at the top of `anyfs_fuse.c` that normalizes types:

```c
/*
 * Platform compatibility: Linux libfuse3 uses POSIX types (struct stat,
 * off_t, etc.). WinFSP on native Windows uses its own types (struct fuse_stat,
 * fuse_off_t, etc.). On Cygwin these map back to POSIX.
 *
 * Define portable aliases so callback signatures work on both platforms.
 */
#ifdef _WIN32
  // WinFSP types — use as-is from <fuse3/fuse.h> / <fuse/winfsp_fuse.h>
  // fuse_stat, fuse_statvfs, fuse_off_t, fuse_mode_t, fuse_dev_t,
  // fuse_uid_t, fuse_gid_t, fuse_timespec are defined by WinFSP headers.
  // fuse3_file_info → fuse_file_info (via winfsp_fuse.h #define)
#else
  // Linux: map portable names to POSIX types
  typedef struct stat       fuse_stat;
  typedef struct statvfs    fuse_statvfs;
# define fuse_off_t         off_t
# define fuse_mode_t        mode_t
# define fuse_dev_t         dev_t
  typedef unsigned int      fuse_uid_t;    // already matches uid_t
  typedef unsigned int      fuse_gid_t;    // already matches gid_t
  typedef struct timespec   fuse_timespec;
#endif
```

### Impact on callback signatures

Each callback parameter needs a type change. Example transformations:

```c
// Before:
static int anyfs_fuse_getattr(const char *path, struct stat *st,
                               struct fuse_file_info *fi)

// After:
static int anyfs_fuse_getattr(const char *path, fuse_stat *st,
                               struct fuse_file_info *fi)
```

Full list of type changes (13 callbacks affected):

| Callback | Parameter | Old type | New type |
|----------|-----------|----------|----------|
| getattr | st | `struct stat *` | `fuse_stat *` |
| readlink | len | `size_t` | unchanged |
| mknod | mode, dev | `mode_t, dev_t` | `fuse_mode_t, fuse_dev_t` |
| mkdir | mode | `mode_t` | `fuse_mode_t` |
| chmod | mode | `mode_t` | `fuse_mode_t` |
| chown | uid, gid | `uid_t, gid_t` | `fuse_uid_t, fuse_gid_t` |
| truncate | off | `off_t` | `fuse_off_t` |
| read | offset | `off_t` | `fuse_off_t` |
| write | offset | `off_t` | `fuse_off_t` |
| statfs | stat | `struct statvfs *` | `fuse_statvfs *` |
| utimens | tv | `const struct timespec *` | `const fuse_timespec *` |
| fallocate | offset, len | `off_t, off_t` | `fuse_off_t, fuse_off_t` |
| lseek | off, return | `off_t` | `fuse_off_t` |

### xlat_stat() changes

```c
// Before: translates lkl_stat → struct stat
static void xlat_stat(const struct lkl_stat *in, struct stat *st)

// After: translates lkl_stat → fuse_stat (works both platforms)
static void xlat_stat(const struct lkl_stat *in, fuse_stat *st)
```

On Linux: `fuse_stat` = `struct stat`, no functional change.
On Windows: `fuse_stat` = WinFSP's struct, `xlat_stat` fills in the correct fields.

The `xlat_statvfs()` function (currently inline in `statfs` callback) needs the same treatment.

### readdir callback

The `fill` function type changes from libfuse3's `fuse_fill_dir_t` to WinFSP's `fuse3_fill_dir_t`.
On WinFSP, the filler takes `const struct fuse_stat *` (not `const struct stat *`):

```c
// Before:
static int anyfs_fuse_readdir(const char *path, void *buf,
                               fuse_fill_dir_t fill, off_t off,
                               struct fuse_file_info *fi,
                               enum fuse_readdir_flags flags)

// After:
static int anyfs_fuse_readdir(const char *path, void *buf,
                               fuse_fill_dir_t fill, fuse_off_t off,
                               struct fuse_file_info *fi,
                               enum fuse_readdir_flags flags)
```

The `struct stat st = {0}` local variable inside readdir becomes `fuse_stat st = {0}`.

## Step 3 — Remove libfuse3-specific Dependencies

### 3a. fuse_lowlevel.h

`fuse_lowlevel.h` is a libfuse3 header not provided by WinFSP. Our code includes it
(line 31) only for `struct fuse_cmdline_opts` and `fuse_parse_cmdline()`.

**Action**: Remove the include. Define the struct and function ourselves.

```c
// ── Replaces #include <fuse3/fuse_lowlevel.h> ──

struct fuse_cmdline_opts {
    int foreground;
    int singlethread;
    char *mountpoint;
};

static int fuse_parse_cmdline(struct fuse_args *args,
                               struct fuse_cmdline_opts *opts)
{
    memset(opts, 0, sizeof(*opts));

    // Scan for mountpoint (first non-option arg after -o processed ones).
    // Also scan for -f (foreground) and -s (single-threaded).
    for (int i = 1; i < args->argc; i++) {
        const char *arg = args->argv[i];
        if (!arg || !*arg)
            continue;
        if (arg[0] == '-') {
            if (strcmp(arg, "-f") == 0 || strcmp(arg, "-d") == 0)
                opts->foreground = 1;
            else if (strcmp(arg, "-s") == 0)
                opts->singlethread = 1;
            // -o options already handled by fuse_opt_parse, skip here
        } else if (!opts->mountpoint) {
            opts->mountpoint = strdup(arg);
        }
    }
    return opts->mountpoint ? 0 : -1;
}
```

Note: the real libfuse's `fuse_parse_cmdline` also handles `-o` options by calling
`fuse_opt_parse` internally. Our flow already calls `fuse_opt_parse` explicitly before
`fuse_parse_cmdline`, so we only need mountpoint + flag extraction.

### 3b. Signal handlers and daemonize

No-ops on Windows. Wrapped in `#ifdef _WIN32`:

```c
#ifdef _WIN32
  // Windows has no fork(), no POSIX signals. WinFSP's fsp_fuse_env provides
  // its own daemonize/signal stubs that return 0 (no-op).
  // fuse_daemonize / fuse_set_signal_handlers / fuse_remove_signal_handlers
  // are NOT in the WinFSP FUSE3 API header. Define them locally.
  static inline int fuse_daemonize(int fg) { (void)fg; return 0; }
  #define fuse_set_signal_handlers(s)     ((void)(s), 0)
  #define fuse_remove_signal_handlers(s)  ((void)(s))
#endif
```

These are called in `main()` at lines 610, 620, 723. On Windows they compile to
no-ops without changing the call sites.

### 3c. lkl_sys_chroot() availability

`lkl_sys_chroot` is NOT available in the installed LKL headers. It's defined only
in `syscall_defs.h` in the LKL source tree, not in `lkl.h`. Our Linux build worked
because the symbol was exported from `liblkl.so` and resolved at link time.

For the MinGW/cross build, we can't count on this. **Switch to path prefix approach**,
which is already proven in the codebase:

```c
// Instead of:
//   lkl_sys_chroot(g_mount_point);  // then paths are "/file"
//
// Use:
static void lkl_path(char *buf, size_t size, const char *path)
{
    snprintf(buf, size, "%s/%s", g_mount_point, path[0] == '/' ? path + 1 : path);
}
```

Each callback that takes a `path` parameter prepends the mount point:

```c
// Before:
ret = lkl_sys_lstat(path, &lkl_stat);

// After:
char lpath[4096];
lkl_path(lpath, sizeof(lpath), path);
ret = lkl_sys_lstat(lpath, &lkl_stat);
```

This adds ~4 lines per callback that uses `path` (getattr, readlink, mknod, mkdir,
unlink, rmdir, symlink, rename, link, chmod, chown, truncate, open, statfs, access,
utimens, setxattr, getxattr, listxattr, removexattr, opendir).

To minimize churn, add a `lkl_path()` helper and a `WITH_LPATH(path)` macro:

```c
static void lkl_path(char *buf, size_t size, const char *path)
{
    if (path[0] == '/') path++;
    int n = snprintf(buf, size, "%s/%s", g_mount_point, path);
    if (n < 0 || (size_t)n >= size)
        buf[size - 1] = '\0';
}

#define LPATH_BUF_SIZE 4096
#define LPATH_DECL char _lpath[LPATH_BUF_SIZE]
#define LPATH(path) (lkl_path(_lpath, sizeof(_lpath), path), _lpath)
```

Then each callback just wraps the path:
```c
static int anyfs_fuse_getattr(const char *path, fuse_stat *st,
                               struct fuse_file_info *fi)
{
    LPATH_DECL;
    struct lkl_stat lkl_stat;
    long ret;

    if (fi)
        ret = lkl_sys_fstat(fi->fh, &lkl_stat);
    else
        ret = lkl_sys_lstat(LPATH(path), &lkl_stat);

    if (!ret)
        xlat_stat(&lkl_stat, st);
    return ret;
}
```

**Trade-off**: Path prefix approach is slightly slower (snprintf per syscall) but
100% portable and avoids the MinGW symbol export issue with `lkl_sys_chroot`.

**Decision**: Apply path prefix approach to BOTH Linux and Windows builds, simplifying
the codebase. Remove `lkl_sys_chroot`/`lkl_sys_chdir` from the main function. This
is a net code improvement (removes platform-specific behavior).

## Step 4 — Build System

### 4a. meson.build changes

Add a new target for the WinFSP build:

```meson
# --- WinFSP FUSE Frontend (anyfs-winfsp.exe) ---
enable_winfsp = get_option('enable_winfsp')
if enable_winfsp
    winfsp_dep = declare_dependency(
        include_directories: include_directories(
            winfsp_root / 'inc' / 'fuse3',
            winfsp_root / 'inc' / 'fuse',
        ),
        compile_args: ['-DFUSE_USE_VERSION=35'],
    )
    # WinFSP import library (generated by mkwinfsp_implib.sh)
    winfsp_lib = cc.find_library('winfsp-x86',
        dirs: [winfsp_root / 'opt' / 'fsext' / 'lib'],
        required: true,
    )
    lkl_mingw_inc = include_directories(lkl_root / 'include' / 'mingw32')

    anyfs_winfsp = executable('anyfs-winfsp',
        'src/fuse/anyfs_fuse.c',
        include_directories: [anyfs_inc, lkl_mingw_inc],
        dependencies: [winfsp_dep, lkl_dep, thread_dep, ws2_dep, iphlpapi_dep],
        link_with: [anyfs_core],
        c_args: winfsp_args,
        win_subsystem: 'console',
        install: true,
        install_rpath: '',
    )
endif
```

### 4b. meson_options.txt addition

```
option('winfsp_root', type: 'string',
    value: '',
    description: 'Path to WinFSP source tree')
option('enable_winfsp', type: 'boolean',
    value: false,
    description: 'Enable WinFSP FUSE frontend (anyfs-winfsp.exe)')
```

### 4c. Cross-compilation cross file

`cross-win32-fuse.txt`:

```ini
[binaries]
c = 'i686-w64-mingw32-gcc'
cpp = 'i686-w64-mingw32-g++'
ar = 'i686-w64-mingw32-ar'
strip = 'i686-w64-mingw32-strip'
pkg-config = 'i686-w64-mingw32-pkg-config'
exe_wrapper = 'wine'

[built-in options]
c_args = [
    '-I/opt/msys2/mingw32/include',
    '-I/opt/msys2/mingw32/include/glib-2.0',
    '-I/opt/msys2/mingw32/lib/glib-2.0/include',
    '-I${LINUX_SRC}/tools/lkl/include/mingw32',
]

[properties]
sys_root = '/opt/msys2/mingw32'

[host_machine]
system = 'windows'
cpu_family = 'x86'
cpu = 'i686'
endian = 'little'
```

### 4d. Build invocation

```bash
# 1. Generate WinFSP import library
./scripts/mkwinfsp_implib.sh

# 2. Configure
meson setup build-win32-fuse --cross-file cross-win32-fuse.txt \
    -Dlkl_root=$HOME/linux/tools/lkl \
    -Dlkl_shared=true \
    -Dwinfsp_root=$HOME/winfsp \
    -Denable_winfsp=true \
    -Denable_fuse=false \
    -Denable_qemu=false \
    -Denable_gio=false

# 3. Build
ninja -C build-win32-fuse anyfs-winfsp
```

## Step 5 — Testing Strategy

### Phase 1: Link test (Linux host, no Wine needed)
```bash
# Just verify it compiles and links without errors
ninja -C build-win32-fuse anyfs-winfsp
file build-win32-fuse/anyfs-winfsp.exe
# Should show: PE32 executable (console) Intel 80386, for MS Windows
```

### Phase 2: Wine functional test (Linux host with Wine)
```bash
# Need winfsp-x86.dll available via WINEPATH
# Wine can't load kernel driver, so FUSE mount will fail at runtime,
# but we can verify:
# 1. Binary loads and runs (no missing DLL errors)
# 2. --help output prints correctly
WINEPATH="$HOME/linux/tools/lkl/lib" wine anyfs-winfsp.exe --help

# Test kernel init + disk detection (will fail at mount without WinFSP driver):
WINEPATH="$HOME/linux/tools/lkl/lib" wine anyfs-winfsp.exe \
    -o backend=raw -o fstype=ext4 disk.img X: -f
# Expected: "anyfs_kernel_init" OK, "anyfs_disk_add" OK,
#           then FUSE mount fails (no WinFSP driver in Wine)
```

### Phase 3: Native Windows test
```
1. Install WinFSP from https://winfsp.dev (winfsp.msi)
2. Copy anyfs-winfsp.exe + liblkl.dll (and deps) to a folder
3. Run: anyfs-winfsp.exe disk.img Z: -f -o backend=raw -o fstype=ext4
4. Open Z: in Explorer, verify file listing
```

### Phase 4: Regression test on Linux
After changes, rebuild and test the native Linux anyfs-fuse to catch regressions:
```bash
meson setup build --reconfigure \
    -Dlkl_root=$HOME/linux/tools/lkl \
    -Dlkl_shared=true -Denable_fuse=true
ninja -C build anyfs-fuse
# Run the same test as before:
./build/anyfs-fuse disk_single.img /tmp/mnt -f -o backend=raw -o fstype=ext4
ls -la /tmp/mnt/
cat /tmp/mnt/hello.txt
fusermount -u /tmp/mnt
```

## Risk items

| # | Risk | Likelihood | Mitigation |
|---|------|-----------|------------|
| 1 | MinGW import lib has wrong/missing symbols | Low | Verify against `gendef` output from real DLL; fallback: rebuild WinFSP DLL from source with MinGW |
| 2 | `fuse_stat` layout mismatch with LKL stat during xlat | Medium | Add static_assert on field offsets; test with files of known sizes |
| 3 | LKL threads + WinFSP callback thread conflict (same segfault class as before) | Medium | Force `-s` (single-threaded) on Windows; WinFSP works single-threaded |
| 4 | `FUSE_USE_VERSION=35` unsupported by WinFSP | Low | WinFSP's `fuse3_new` signature matches FUSE 3.1+. If broken, switch to `FUSE_USE_VERSION=30` which maps `fuse_new` → `fuse3_new_30` |
| 5 | Path prefix approach breaks symlink or rename semantics | Low | Already tested in earlier anyfs-fuse iterations; lklfuse.c uses same approach |
| 6 | Missing `lkl_sys_*` symbols in `liblkl.dll` (MinGW export) | Low | `liblkl.dll` is built with `--export-all-symbols`, all LKL syscalls are exported |

## Implementation order

1. **Generate WinFSP import lib** — `mkwinfsp_implib.sh` (~15 min)
2. **Add portable type compat** — `anyfs_fuse.c` top matter (~20 min)
3. **Replace chroot with path prefix** — `anyfs_fuse.c` callbacks + main (~30 min)
4. **Replace fuse_lowlevel.h** — `fuse_cmdline_opts` + `fuse_parse_cmdline` (~15 min)
5. **Add #ifdef stubs for signal/daemon** — `anyfs_fuse.c` (~5 min)
6. **Add meson build target** — `meson.build` + options (~20 min)
7. **Cross-compile and fix build errors** — iterative (~30 min)
8. **Test on Linux (regression)** + **Test on Wine** + **Test on Windows** — (~1 hr)

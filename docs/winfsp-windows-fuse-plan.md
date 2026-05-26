# anyfs-fuse Windows Port via WinFSP — Feasibility Analysis

## Overview

Port `anyfs-fuse` to Windows using WinFSP (Windows File System Proxy) as the FUSE layer.
WinFSP consists of a kernel driver (`winfsp-x86.sys` / `winfsp-x64.sys`) and a user-mode DLL
(`winfsp-x86.dll` / `winfsp-x64.dll`) that implements the FUSE API on top of the driver.

## Architecture

```
anyfs-fuse.exe (PE32/PE32+)
  ├── WinFSP FUSE3 API (static inline wrappers in winfsp headers)
  │     └── winfsp-x86.dll (FUSE → WinFSP ioctl translation)
  │           └── winfsp-x86.sys (kernel driver, FUSE device)
  ├── liblkl.dll (LKL kernel, already built for Win32)
  ├── anyfs_core (disk/mount management)
  └── QEMU block backend (optional, libanyfs-qemublk.dll)
```

### WinFSP FUSE Implementation

WinFSP headers (`~/winfsp/inc/`) provide a header-only FUSE implementation:

```
inc/fuse/          — FUSE 2.x API (271 lines fuse.h + 202 lines fuse_common.h + 133 lines fuse_opt.h)
inc/fuse3/         — FUSE 3.x API (338 lines fuse.h + fuse_common + fuse_opt)
  winfsp_fuse.h    — #define fuse3_* → fuse_* for most symbols
```

Key mechanism:
```c
// When CYGFUSE is NOT defined (Windows native):
#define FSP_FUSE_SYM(proto, ...)  static inline proto { __VA_ARGS__ }

// Example: fuse3_new expands to a static inline that calls into the DLL
static inline struct fuse3 *fuse3_new(struct fuse_args *args, ...) {
    return FSP_FUSE_API_CALL(fsp_fuse3_new)(fsp_fuse_env(), args, ops, opsize, data);
}
// FSP_FUSE_API = __declspec(dllimport) → imports from winfsp-x86.dll
```

### Available at `~/winfsp/`

- `inc/fuse/` — FUSE2 headers (fuse.h, fuse_common.h, fuse_opt.h, winfsp_fuse.h)
- `inc/fuse3/` — FUSE3 headers (fuse.h, fuse_common.h, fuse_opt.h, winfsp_fuse.h)
- `opt/fsext/lib/winfsp-x86.lib` — MSVC import library (32-bit)
- `opt/fsext/lib/winfsp-x64.lib` — MSVC import library (64-bit)
- `opt/fsext/lib/winfsp-a64.lib` — MSVC import library (ARM64)
- `src/dll/` — DLL source (can be rebuilt with MinGW if needed)

## Gap Analysis: anyfs_fuse.c vs WinFSP API

### Present in WinFSP FUSE3

| API | Notes |
|-----|-------|
| `fuse_new` / `fuse3_new` | FUSE3 signature: `(args, ops, opsize, data)` — no `fuse_chan *` |
| `fuse3_destroy` | |
| `fuse3_mount` / `fuse3_unmount` | |
| `fuse3_loop` | single-threaded; `fuse3_loop_mt` also available |
| `fuse3_get_session` | returns `(fuse3_session *)f` |
| `fuse_main` | macro → `fuse3_main_real` |
| `fuse_opt_parse` / `fuse_opt_add_arg` / `fuse_opt_free_args` | |
| `FUSE_ARGS_INIT` / `FUSE_OPT_KEY` / `FUSE_OPT_END` | |
| `fuse_operations` / `fuse3_operations` | 29 callbacks: getattr, readlink, mknod, mkdir, unlink, rmdir, symlink, rename, chmod, chown, truncate, open, read, write, statfs, flush, release, fsync, setxattr, getxattr, listxattr, removexattr, opendir, readdir, releasedir, fsyncdir, access, create, utimens, fallocate, lseek, copy_file_range |
| `fuse_config` / `fuse3_config` | `nullpath_ok`, `entry_timeout`, `attr_timeout`, `negative_timeout`, `use_ino` |
| `fuse3_get_context` | |
| `fuse3_exit` | |

### Missing from WinFSP FUSE3

| API | Used in anyfs_fuse.c? | Workaround |
|-----|----------------------|------------|
| `fuse_lowlevel.h` | Yes (`#include`) | Remove include; only needed for `fuse_cmdline_opts` |
| `fuse_cmdline_opts` struct | Yes — `cli_opts` variable | Define our own struct with `mountpoint`, `foreground`, `singlethread` |
| `fuse_parse_cmdline` | Yes | Implement manually (~30 lines): parse `mountpoint` and flags from args |
| `fuse_set_signal_handlers` | Yes | `#ifdef _WIN32` → no-op (signals don't work on Windows) |
| `fuse_remove_signal_handlers` | Yes | `#ifdef _WIN32` → no-op |
| `fuse_daemonize` | Yes | `#ifdef _WIN32` → no-op (no fork on Windows) |

### Type Differences

Our code uses POSIX types. WinFSP on Windows defines its own:

```c
// Our code (Linux)          // WinFSP on Windows
struct stat                  struct fuse_stat   (different layout!)
off_t                        fuse_off_t         (int64_t)
mode_t                       fuse_mode_t        (uint32_t)
dev_t                        fuse_dev_t         (uint32_t)
```

**The `fuse_stat` vs `struct stat` problem is the biggest porting issue.** Our callbacks
(e.g., `anyfs_fuse_getattr`) use `struct stat *st` as parameter type. Linux libfuse3
uses `struct stat` but WinFSP uses `struct fuse_stat`. These have different layouts:

```c
// Linux struct stat (simplified):      // WinFSP fuse_stat (32-bit):
dev_t     st_dev;    // uint64_t        fuse_dev_t     st_dev;    // uint32_t
ino_t     st_ino;    // uint64_t        fuse_ino_t     st_ino;    // uint64_t
nlink_t   st_nlink;  // uint64_t        fuse_nlink_t   st_nlink;  // uint16_t
off_t     st_size;   // int64_t         fuse_off_t     st_size;   // int64_t
blksize_t st_blksize; // int64_t        fuse_blksize_t st_blksize; // int32_t
blkcnt_t  st_blocks;  // int64_t        fuse_blkcnt_t  st_blocks;  // int64_t
```

On Cygwin, `fuse_stat` maps to `struct stat` directly. On native Windows, it's custom.

**Strategy**: Use `-DFSP_FUSE_USE_STAT_EX` to enable `struct fuse_stat_ex` which has
extra padding. Or use a `#ifdef _WIN32` typedef layer that maps our POSIX types to
the WinFSP types consistently. Since our LKL is Linux-on-Windows, the LKL `struct stat`
fields come from the kernel — we need to translate between LKL's stat and WinFSP's
`fuse_stat` in the callbacks.

## Build Plan

### Step 1: Generate MinGW Import Library for WinFSP

The WinFSP DLL exports symbols like `fsp_fuse3_new`, `fsp_fuse3_mount`, etc.
We need a MinGW-compatible `.dll.a` import library.

**Option A — Use gendef + dlltool on Windows:**
```powershell
# On a Windows machine with WinFSP installed:
gendef winfsp-x64.dll                    # generates winfsp-x64.def
dlltool -d winfsp-x64.def -l libwinfsp.a  # generates MinGW import library
```

**Option B — Generate .def manually on Linux:**
The needed exports (from WinFSP headers) are deterministic. We can generate a `.def`
file listing all `fsp_fuse*` and `fsp_fuse3*` functions, then use `i686-w64-mingw32-dlltool`:

```bash
# Create winfsp-x86.def
cat > winfsp-x86.def <<'EOF'
EXPORTS
  fsp_fuse_main_real
  fsp_fuse_new
  fsp_fuse_destroy
  fsp_fuse_mount
  fsp_fuse_unmount
  fsp_fuse_loop
  fsp_fuse_loop_mt
  fsp_fuse_get_context
  fsp_fuse_get_session
  fsp_fuse_exit
  fsp_fuse3_main_real
  fsp_fuse3_new
  fsp_fuse3_new_30
  fsp_fuse3_destroy
  fsp_fuse3_mount
  fsp_fuse3_unmount
  fsp_fuse3_loop
  fsp_fuse3_loop_mt
  fsp_fuse3_loop_mt_31
  fsp_fuse3_get_context
  fsp_fuse3_exit
  fsp_fuse_opt_parse
  fsp_fuse_opt_add_arg
  fsp_fuse_opt_insert_arg
  fsp_fuse_opt_free_args
  fsp_fuse_opt_add_opt
  fsp_fuse_opt_add_opt_escaped
  fsp_fuse_opt_match
  fsp_fuse3_lib_help
  fsp_fuse3_get_session
  fsp_fuse3_start_cleanup_thread
  fsp_fuse3_stop_cleanup_thread
EOF
i686-w64-mingw32-dlltool -d winfsp-x86.def -l libwinfsp-x86.dll.a
```

**Option C — Rebuild WinFSP DLL from source with MinGW:**
The WinFSP source at `~/winfsp/src/dll/` could be compiled with `i686-w64-mingw32-gcc`
to produce a MinGW-compatible DLL and `.dll.a` directly. This avoids the MSVC import
library format issue entirely but requires compiling the WinFSP DLL source.

### Step 2: Cross-compile anyfs-fuse

```bash
cd ~/anyfs-reader
meson setup build-win32-fuse --cross-file cross-win32-fuse.txt \
  -Dlkl_root=$HOME/linux/tools/lkl \
  -Dlkl_shared=true \
  -Denable_fuse=true \
  -Denable_qemu=false \
  -Denable_gio=false
ninja -C build-win32-fuse anyfs-fuse
```

Cross file `cross-win32-fuse.txt` needs:
- `i686-w64-mingw32-gcc` as compiler
- WinFSP include path: `-I${WINFSP_SRC}/inc/fuse3 -I${WINFSP_SRC}/inc/fuse`
- WinFSP lib path: `-L/path/to/libwinfsp-x86.dll.a`
- Define `FUSE_USE_VERSION=35` (or `30` for WinFSP compat mode)

### Step 3: Required Source Changes to anyfs_fuse.c

Estimated changes: ~60 lines.

```c
// 1. Replace fuse_lowlevel.h (line 31)
//    #include <fuse3/fuse_lowlevel.h>  → remove

// 2. Define fuse_cmdline_opts ourselves
struct fuse_cmdline_opts {
    int foreground;
    int singlethread;
    char *mountpoint;
};

// 3. Implement fuse_parse_cmdline manually
static int fuse_parse_cmdline(struct fuse_args *args,
                               struct fuse_cmdline_opts *opts) {
    opts->foreground = 0;
    opts->singlethread = 0;
    opts->mountpoint = NULL;
    for (int i = 1; i < args->argc; i++) {
        if (args->argv[i][0] == '-') {
            if (strcmp(args->argv[i], "-f") == 0) opts->foreground = 1;
            else if (strcmp(args->argv[i], "-s") == 0) opts->singlethread = 1;
            else if (strcmp(args->argv[i], "-d") == 0) opts->foreground = 1;
        } else if (!opts->mountpoint) {
            opts->mountpoint = args->argv[i];
        }
    }
    return opts->mountpoint ? 0 : -1;
}

// 4. Platform-conditional signal/daemon
#ifdef _WIN32
  #define fuse_set_signal_handlers(s)    0
  #define fuse_remove_signal_handlers(s) 0
  #define fuse_daemonize(fg)             ((void)(fg), 0)
#endif

// 5. Stat type compatibility
// On Windows with WinFSP: fuse_operations uses struct fuse_stat*
// On Linux with libfuse3: fuse_operations uses struct stat*
// Since LKL uses Linux stat internally, add a translation layer:
#ifdef _WIN32
static void stat_to_fuse_stat(const struct lkl_stat *lst, struct fuse_stat *fst) {
    memset(fst, 0, sizeof(*fst));
    fst->st_dev     = lst->st_dev;
    fst->st_ino     = lst->st_ino;
    fst->st_mode    = lst->st_mode;
    fst->st_nlink   = lst->st_nlink;
    fst->st_uid     = lst->st_uid;
    fst->st_gid     = lst->st_gid;
    fst->st_rdev    = lst->st_rdev;
    fst->st_size    = lst->st_size;
    fst->st_blksize = lst->st_blksize;
    fst->st_blocks  = lst->st_blocks;
    fst->st_atim.tv_sec  = lst->st_atime;
    fst->st_atim.tv_nsec = lst->st_atime_nsec;
    fst->st_mtim.tv_sec  = lst->st_mtime;
    fst->st_mtim.tv_nsec = lst->st_mtime_nsec;
    fst->st_ctim.tv_sec  = lst->st_ctime;
    fst->st_ctim.tv_nsec = lst->st_ctime_nsec;
}
#endif
```

**Alternative strategy**: Use `#ifdef _WIN32` typedefs to make `struct stat` map to
`struct fuse_stat` on Windows. This avoids touching every callback but risks subtle
layout bugs if we're not careful about struct sizes.

### Step 4: Runtime Dependencies on Windows

```
anyfs-win32-fuse/
├── bin/
│   └── anyfs-fuse.exe
├── lib/
│   ├── liblkl.dll        (already built, ~16MB stripped)
│   ├── libglib-2.0-0.dll (MSYS2)
│   ├── libintl-8.dll     (MSYS2)
│   ├── libiconv-2.dll    (MSYS2)
│   ├── libpcre2-8-0.dll  (MSYS2)
│   ├── libgcc_s_dw2-1.dll
│   └── libwinpthread-1.dll
└── driver/
    └── winfsp-x86.sys     (WinFSP kernel driver, installed separately)
```

WinFSP must be installed on the target machine (`winfsp.msi` from https://winfsp.dev).
The MSI installs the kernel driver and `winfsp-x86.dll` into `C:\Windows\System32\`.

## Risk Assessment

| Risk | Severity | Mitigation |
|------|----------|------------|
| `struct stat` vs `fuse_stat` mismatch | High | Translation layer; test with actual file sizes, permissions, timestamps |
| MinGW import lib from MSVC .lib may miss symbols | Medium | Rebuild WinFSP DLL from source with MinGW (Option C) |
| LKL threads + WinFSP callback threads conflict | Medium | Force single-threaded mode (`-s`); WinFSP supports this |
| `lkl_sys_chroot` not available on Win32 | Low | Use path prefix approach (already working in Linux build) |
| No `fork()` on Windows → daemonize is no-op | Low | Always run in foreground on Windows |

## Conclusion

**Feasibility: High.** The required changes are well-bounded (~60 lines in anyfs_fuse.c):
1. Remove `fuse_lowlevel.h` dependency (replace `fuse_cmdline_opts` / `fuse_parse_cmdline`)
2. Platform-conditional stubs for signal handlers and daemonize
3. `fuse_stat` translation layer for stat callback

The largest unknown is the MinGW import library for WinFSP. Option C (rebuilding the
WinFSP DLL from source with MinGW) eliminates this risk entirely.

Estimated effort: 2-3 hours for the port, plus testing on a Windows VM/Wine.

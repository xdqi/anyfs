# Building 7zFM.exe with LKL (Linux Kernel Library) Integration

Cross-compile 7-Zip File Manager for Win32 (i386) from Linux, with LKL
for browsing disk image filesystems (ext4, btrfs, xfs, FAT, NTFS, etc.).

## Prerequisites

| Component | Path | Notes |
|-----------|------|-------|
| 7-Zip source | `~/7zip` | Clone from https://github.com/ip7z/7zip |
| Linux/LKL | `~/linux` | Branch with LKL support (tools/lkl/) |
| anyfs-reader | `~/anyfs-reader` | This repo — contains CMakeLists.txt and LKL plugin code |
| MinGW cross-compiler | system | `i686-w64-mingw32-gcc` / `i686-w64-mingw32-g++` (GCC 14) |
| CMake | system | >= 3.16 |
| LKL Win32 DLLs | `~/linux/tools/lkl/lib-win32/` | Pre-built `liblkl.dll` + dependencies |

## Quick Build

```bash
cd ~/anyfs-reader
mkdir -p build-7zfm && cd build-7zfm
cmake .. -DCMAKE_TOOLCHAIN_FILE=../mingw32-toolchain.cmake
cmake --build . -j$(nproc)
i686-w64-mingw32-strip 7zFM.exe
```

## Build Options

| Option | Default | Description |
|--------|---------|-------------|
| `LKL_STATIC` | OFF | Link liblkl.a statically (needs Win32 .a) |
| `WITH_ARCHIVE` | ON | Include archive browsing (via 7z DLLs) |
| `SEVENZIP_SRC` | `../7zip` | Path to 7-Zip source tree |
| `LKL_SRC` | `../linux` | Path to Linux kernel source with LKL |
| `LKL_LIB_DIR` | `$LKL_SRC/tools/lkl/lib-win32` | Dir with liblkl.dll and deps |

## Output

- `7zFM.exe` — ~1.6 MB stripped, PE32 i386 GUI
- Needs these DLLs in same directory:
  - `liblkl.dll` (18 MB) — LKL kernel
  - `libslirp-0.dll` — network (used by LKL)
  - `libglib-2.0-0.dll` — GLib (used by slirp)
  - `libintl-8.dll`, `libiconv-2.dll`, `libpcre2-8-0.dll` — GLib deps
  - `libgcc_s_dw2-1.dll` — MinGW runtime (for slirp)

Total package: ~25 MB uncompressed, ~11 MB gzipped.

## Architecture

```
7zFM.exe
├── 7-Zip File Manager UI (panels, menus, dialogs)
├── IFolderFolder interface → filesystem plugins
│   ├── FSFolder (native NTFS/FAT browsing)
│   ├── ArchiveFolder (7z/zip/tar via external codecs)
│   └── LklFolder (disk image browsing via LKL) ← NEW
├── LklSession (mounts disk images)
│   └── lkl_bridge.c → liblkl.dll (Linux kernel in userspace)
└── Agent (archive extraction support)
```

### LKL Plugin Integration

The LKL folder plugin is compiled directly into 7zFM.exe (no separate DLL needed).
It registers as an `IFolderManager` with GUID `{E7A2B5C1-4F3D-4A6E-B8C2-1D5F7A9E3B4D}`.

Supported image extensions: `img raw qcow2 qcow vhd vhdx vmdk vdi iso bin dd wim`

### Key Source Files

| File | Purpose |
|------|---------|
| `src/7z-plugin/LklFolder.cpp` | IFolderFolder — browses dirs in mounted LKL fs |
| `src/7z-plugin/LklFolderManager.cpp` | IFolderManager — opens disk images |
| `src/7z-plugin/LklSession.cpp` | Manages LKL kernel lifecycle & mount |
| `src/7z-plugin/lkl_bridge.c` | C bridge (avoids C++ keyword conflicts in lkl.h) |
| `src/7z-plugin/compat-include/` | Case-sensitivity symlinks for Windows headers |
| `src/7z-plugin/contrib/htmlhelp.c` | Stub for HtmlHelp (not in MinGW) |
| `mingw32-toolchain.cmake` | Cross-compile toolchain file |
| `CMakeLists.txt` | Main CMake build |

## Separate Plugin DLL (Alternative)

If you prefer loading LKL as a 7zFM plugin DLL instead of static integration:

```bash
cd ~/anyfs-reader/src/7z-plugin
make MODE=dynamic    # produces 7z-lkl.dll (109KB)
```

Place `7z-lkl.dll` in 7-Zip's `Plugins/` directory alongside `liblkl.dll`.

## Known Issues

- **No ASM**: Uses C fallback for CRC (no MASM available in cross-compile)
- **ProgressDialog**: Only `ProgressDialog2.cpp` is included (the older `ProgressDialog.cpp` conflicts)
- **HtmlHelp**: Stubbed out (returns NULL) — F1 help won't work
- **Explorer context menu**: Included but non-functional without Windows shell registration
- **Display**: Won't work under headless Wine (needs X11/Wayland)

## Rebuilding LKL for Win32

```bash
cd ~/linux
make ARCH=lkl CC=i686-w64-mingw32-gcc \
     HOSTCC=gcc mrproper defconfig
# Edit .config: CONFIG_LKL_HOST_CONFIG_NT=y
make ARCH=lkl CC=i686-w64-mingw32-gcc -j$(nproc)
# Output: tools/lkl/liblkl.dll (or liblkl.a for static)
```

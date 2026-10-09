# macOS: native backend, command-line tools and Electron addon

Everything for macOS is cross-built on Linux with zig cc. Nothing needs an Apple SDK or a
Mac to build. Running it, and the runtime tests, need a real Mac.

| Status (2026-10-09) | arm64 (Apple Silicon) | x86_64 (Intel) |
|---|---|---|
| Builds and passes the Mach-O gate | yes | yes |
| Runtime-tested on a Mac | **not yet**: see [Testing](#testing-on-a-mac) | **not yet** |

The LKL kernel half was verified on both Macs earlier (21-check smoke,
`scripts/macho/smoke/README.md`). The rest of the stack has only been verified on Linux so
far. The same core, QEMU and kernel code runs there, and a Linux rehearsal exercised the
Mac test scripts end to end.

## What runs on macOS

| Component | Built from | Runtime dependencies |
|---|---|---|
| `anyfs-lspart` | `build_anyfs.sh --targets=macos-<arch>` | `liblkl-kernel.dylib` (shipped in `lib/`) |
| `anyfs-ksmbd` (SMB3 server) | same | same |
| `anyfs-nfsd` (NFSv4 server) | same | same |
| `anyfs-fuse` | same, after `build_macos_sysroot.sh --only=macfuse` | same, plus **macFUSE** (installed by the user, see below) |
| `anyfs_native.node` (Electron addon) | `ts/packages/anyfs-native/scripts/build-macos.sh` | `liblkl-kernel.dylib` next to the `.node` |
| electron-demo app | electron-packager + `scripts/stage-native-macos.sh` | the two files above in `Contents/Resources/native/` |

Deployment targets live in `scripts/macho/macos_target.sh`:

- arm64: macOS 11.0.
- x86_64: macOS 10.13. It was 10.12 for the kernel alone, but GLib 2.88 refuses to build for
  anything older, and QEMU and anyfs need GLib.
- The Electron app needs whatever its Electron requires (macOS 12 for Electron 42).
- macFUSE 5.4's libfuse3 needs macOS 12.

All disk-image formats of the Linux build are there, through QEMU's block layer: raw, qcow2,
vmdk, vdi, vpc/vhd, vhdx, dmg (zlib and bzip2 chunks; lzfse is not available on any platform),
and http(s) URLs (curl + OpenSSL, CA roots from `/etc/ssl/cert.pem`).

## How it fits together

```
LKL kernel   standard ELF build (gen_lkl_config/build_lkl, linux-arm64 or linux-amd64)
             -> scripts/macho/build_kernel_dylib.sh (elf2dylib) -> liblkl-kernel.dylib
LKL host     tools/lkl/lib + lkl_macho_shim.c -> scripts/macho/build_host_lib.sh -> liblkl-host.a
deps         scripts/build_macos_sysroot.sh -> ~/.cache/anyfs-macos-sysroot/<arch>
             zlib bzip2 zstd libffi libiconv glib(+pcre2, proxy-libintl) libblkid OpenSSL curl
QEMU         scripts/build_qemu.sh --targets=macos-<arch> -> static block-layer archives
anyfs        scripts/build_anyfs.sh --targets=macos-<arch> (meson cross file in .toolchain/)
addon        ts/packages/anyfs-native/scripts/build-macos.sh --arch=<arch>
```

The kernel half and its ABI boundary are described in
`docs/superpowers/specs/2026-10-05-lkl-macos-elf2dylib-design.md`. Everything else links
statically. Each tool and the addon load only `@rpath/liblkl-kernel.dylib`, libSystem, and
the CoreFoundation and SystemConfiguration frameworks that curl calls.

The rpath is `@loader_path` and `@loader_path/../lib` for the tools, and `@loader_path` for
the addon.

### Toolchain rules

- **Compiler launchers.** `scripts/macho/<arch>-macos-cc` and `-c++` run
  `scripts/lib/zig-cc.sh` with the arch's zig target. The target is in the launcher's name,
  so meson and QEMU's configure can store the launcher path.
- **Archives.** `scripts/macho/macos-ar` and `macos-ranlib` (zig's llvm-ar) write the Darwin
  symbol table that Mach-O linkers read.
- **`-Werror=unguarded-availability` on every compile.** Without it, the linker silently turns
  a function newer than the deployment target into a weak import, which is NULL on an older
  macOS. QEMU's configure had found `preadv` (macOS 11) and `strchrnul` (15.4). With the flag,
  such probes fail and QEMU uses its own fallbacks. `check_macho.sh` rejects weak imports.
- **SDK stubs.** zig ships the macOS libc headers and `libSystem.tbd`, but no frameworks.
  `scripts/macho/sdk-stubs` has `.tbd` link stubs for the three frameworks the build needs,
  with real install names (see its README). Final links use `-dead_strip_dylibs`.
- **GLib without gio** (`patches/sysroot/glib-2.88.0-darwin-skip-gio.patch`). gio needs
  resolver headers and libresolv, which zig lacks. QEMU (built without system emulators),
  ksmbd-tools and anyfs use only glib and gthread.
- **No libuuid** in the macOS sysroot. Nothing links it, and its ELF alias doesn't build for
  Mach-O.
- **libblkid's `crc32c` is renamed to `anyfs_blkid_crc32c`**, as in the other sysroots.
  QEMU's `crc32c` would otherwise win the static link and break ext4 detection. The sysroot
  script checks this, and `build-macos.sh` checks the addon for it.
- **QEMU patches for macOS** (`patches/qemu/series.native`):
  - 0013 includes `<sys/mount.h>` when IOKit is absent;
  - 0014 guards `pthread_jit_write_protect_np()` for macOS < 11.

### The gate: `scripts/macho/check_macho.sh`

Every macOS build script runs it on its outputs. It checks:

- arch and deployment target of every object, archive member and image;
- load commands, against an allowlist;
- exact LC_RPATHs, so no build directory leaks into a shipped file;
- that every undefined symbol is bound to a dylib (napi_* may use dynamic lookup in the addon);
- no weak imports;
- an ad-hoc signature on arm64 (zig signs arm64 links itself; x86_64 macOS runs unsigned code).

Known limitation: zig's Mach-O linker ignores export lists, so images also export the
symbols of their static libraries. Under the two-level namespace nothing binds to them.

### Darwin-specific code

- `src/core/anyfs_probe.c` hands libblkid the spool file as an fd. macOS has no
  `/proc/self/fd`; without this change fstype, label and UUID came back empty.
- `src/core/raw_backend.c` sizes `/dev/diskN` and `/dev/rdiskN` with
  `DKIOCGETBLOCKCOUNT` × `DKIOCGETBLOCKSIZE`.
- `src/ksmbd/darwin-compat/` provides `<linux/types.h>` and `<endian.h>` for ksmbd-tools.
- `src/fuse/fuse_main.c` handles three Darwin differences:
  - stat timespec names;
  - Linux-only open flags;
  - Linux errno values from LKL, translated to macOS values at the FUSE boundary. ENODATA
    is 61 on Linux, which is ECONNREFUSED on macOS; a missing xattr must be ENOATTR.
- `iconv` (legacy file-name encodings) is GNU libiconv from the sysroot, since libSystem has
  none.

## Build

On Linux, per arch (`ARCH` = arm64 or x86_64, `LKLD` = linux-arm64 or linux-amd64):

```sh
scripts/fetch_zig.sh
scripts/oot_fs.sh stage --macho
scripts/gen_lkl_config.sh --targets=$LKLD && scripts/build_lkl.sh --targets=$LKLD
scripts/macho/build_kernel_dylib.sh --arch=$ARCH
scripts/macho/build_host_lib.sh --arch=$ARCH
scripts/build_macos_sysroot.sh --arch=$ARCH
scripts/build_macos_sysroot.sh --arch=$ARCH --only=macfuse   # optional: anyfs-fuse
scripts/build_qemu.sh --targets=macos-$ARCH
scripts/build_anyfs.sh --targets=macos-$ARCH --components=core,server,fuse
ts/packages/anyfs-native/scripts/build-macos.sh --arch=$ARCH
scripts/package_macos.sh --arch=$ARCH        # build/macos/anyfs-reader-<ver>-macos-$ARCH.tar.gz
```

Host tools:

- zig 0.16.0;
- LLVM 19 or 20: `llvm-nm`, `llvm-otool`, `llvm-objdump`, `llvm-objcopy`, `llvm-lipo`,
  `llvm-readtapi`, `ld.lld`, `clang`;
- **LLVM 19's `ld64.lld`** (Debian/Ubuntu package `lld-19`) for the kernel conversion.
  LLD 20 puts every `pure_instructions` section first in `__TEXT`, so `__lkl_text` lands
  before `__lkl_const`. Each section then sits away from its ELF address + 0x4000.
  `elf2dylib.py` refuses LLD ≥ 20 up front, and its output check would catch the shift
  anyway. Reproduce with two sections in `__TEXT`: LLD 19 keeps the input order, LLD 20
  moves the code section first, and without `pure_instructions` both keep the order. The
  attribute stays, because the Macs verified the kernel with it and debuggers use it;
  `ld64.lld-19` is in the Ubuntu 26.04 archive;
- `aarch64-linux-gnu-gcc` for the arm64 kernel;
- meson, ninja, pkg-config, perl, python3, curl;
- bsdtar and cpio, for macfuse only.

The `--only=macfuse` step also needs the Linux addon
(`ts/packages/anyfs-native/build/Release/anyfs_native.node`). It reads the macFUSE `.dmg`
(an HFS+ volume) with anyfs itself.

Electron app:

```sh
electron-packager <electron-demo> anyfs-demo --platform=darwin --arch=arm64|x64 ...
bash ts/examples/electron-demo/scripts/stage-native-macos.sh <out>/anyfs-demo-darwin-<a> <arch> [<native-dir>]
```

`native-loader.ts` needs no macOS change: `process.resourcesPath` is `Contents/Resources`,
and the `.node` finds the kernel dylib through its rpath. electron-packager on Linux leaves
Electron's signature broken, so on the Mac run `codesign --force --deep --sign -` on the app
before the first launch. Developer ID signing and notarization are not set up.

## anyfs-fuse and macFUSE

anyfs-fuse uses the libfuse 3 API, and on macOS that means **macFUSE**, an external
dependency that anyfs does not install or redistribute:

- The build takes only macFUSE 5.4.0's fuse3 headers, plus a `.tbd` link stub of
  `/usr/local/lib/libfuse3.4.dylib`, into the local sysroot. The release and its sha256 are
  pinned in `scripts/lib/sysroot_sources.sh`.
- macFUSE's headers default to its Darwin extensions. `fuse3.pc` sets
  `FUSE_DARWIN_ENABLE_EXTENSIONS=0`, so anyfs-fuse compiles against the standard API, as on
  Linux.
- To run it, install macFUSE from https://macfuse.github.io. macOS then asks the user to allow
  its system extension (or, on recent macOS, the FSKit backend). The test runner skips
  anyfs-fuse when `/usr/local/lib/libfuse3.4.dylib` is missing, and says so.

FUSE-T (the kext-less alternative) is not supported. Its libfuse3 is a different library,
and nothing here has been tried against it. The SMB and NFS servers need no extension at all.

## Testing on a Mac

`scripts/macho/package_macos_tests.sh --arch=<arch> [--build-dir=DIR] [--native-dir=DIR]
[--ubuntu=<qcow2>] [--app=<staged .app>]` builds `build/macos/anyfs-macos-test-<arch>.tar.gz`
(about 65 MB without the Ubuntu image and the app). It holds:

- the tools and core test programs;
- the addon;
- fixtures from `tests/macos/make-fixtures.sh`: a GPT raw image and the same wrapped as zlib
  and bzip2 DMGs, plus the Ubuntu 26.10 qcow2 if given;
- the app, if given;
- reference data.

The fixtures are byte-for-byte reproducible (fixed UUIDs and volume ids), so their reference
data is committed in `tests/macos/reference/`. It was computed by the Linux build, and the
script re-checks it whenever a Linux build is present. Packaging therefore needs no Linux
build, except for the Ubuntu image, whose reference is computed when it is packaged.
`--build-dir` and `--native-dir` take the inputs from CI artifacts.

`tests/macos/README.md` gives the exact steps. In short, on the Mac:

```sh
tar xzf anyfs-macos-test-<arch>.tar.gz && cd anyfs-macos-test-<arch>
codesign --force --deep --sign - app/anyfs-demo.app
tests/run-tests.sh --app app/anyfs-demo.app 2>&1 | tee ~/anyfs-macos-test.txt
```

It checks:

- core tests;
- `anyfs-lspart` tables against the Linux reference;
- `anyfs-ksmbd` via `mount_smbfs`;
- `anyfs-nfsd` via an NFSv4 mount (sudo);
- `anyfs-fuse` if macFUSE is installed;
- the addon with Electron as node: metadata, mount, list, stat, extraction and SHA-256,
  three passes, halt;
- the Electron main-process smoke.

The GUI check is by hand: open the qcow2, mount, download `/etc/os-release`, quit with a
disk open.

What has been verified so far, on Linux only:

- every Mach-O output passes the gate;
- `test_zig_cc.sh` checks the macOS launchers;
- the Linux reference run is 32/32;
- Electron-as-node with the Linux addon passes 104/104;
- a rehearsal of `run-tests.sh` with Linux binaries passes, with shims for `mount_smbfs`
  (smbclient) and the NFS mount (the Linux NFSv4 client).

None of this counts as a macOS pass.

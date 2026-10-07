# Unicode and legacy-encoded filenames: design

**Date:** 2026-10-07
**Status:** implemented (plan: `docs/superpowers/plans/2026-10-07-unicode-filenames.md`); see "Amendments during implementation" at the end
**Scope:** `src/core/anyfs_mount_opts.c`, `src/core/anyfs_name.{c,h}` (new), `ts/native/anyfs_ts.c`,
`src/win32/` (new), `src/core/{raw_backend,anyfs_probe,anyfs_container,anyfs_tls,anyfs_session,qemu_thread}.c`,
the CLIs (`lspart`, `ksmbd`, `nfsd`, `fuse`), `patches/qemu/0012-*`, `ts/packages/{core,trees}`,
`ts/examples/vite-demo`, tests

## Goal

Every file on an image can be listed, opened and downloaded, whatever bytes its name is made of, and
its name is shown readably: UTF-8 names as they are, names in a legacy encoding (GBK, Big5,
Shift_JIS, ...) decoded with a user-chosen encoding. On Windows, every host path and every piece of
console text crosses the OS boundary through the W (UTF-16) APIs, so a non-ASCII image path,
temporary directory or command-line argument works whatever the system code page is.

## Context

Measured on 2026-10-06 with the native addon (`readdirJson` + `lstatJson`):

| image | name on disk | what the UI got | lstat |
| ----- | ------------ | --------------- | ----- |
| FAT, long name `中文.txt` | UTF-16 LFN | `??.txt` | ok, but the name is lost |
| FAT, long name `café.txt` | UTF-16 LFN | `caf�.txt` | ENOENT |
| ext4, raw GBK `中文.txt` (`D6 D0 CE C4`) | raw bytes | `����.txt` | ENOENT |

Two separate causes:

1. **FAT converts long names with `iocharset=iso8859-1`** (`CONFIG_FAT_DEFAULT_IOCHARSET`, LKL's
   default; not changed per the kernel-config rule, the mount option is ours). CJK becomes `?`;
   Latin-1 comes out as one byte, which is not UTF-8. Short (8.3) names use `codepage=437`.
   The other filesystems already emit UTF-8 for names stored as Unicode: exFAT
   (`CONFIG_EXFAT_DEFAULT_IOCHARSET="utf8"`), NTFS PLUS, HFS+, UDF and Joliet (NLS default `utf8`).
2. **Names are not bytes on the way to JavaScript.** `anyfs_ts.c` writes the raw name into JSON;
   `UTF8ToString` / `TextDecoder` / `Napi::String::New` turn invalid UTF-8 into U+FFFD, and a path
   built from that string no longer names the file. Filesystems that store byte names (ext2/3/4,
   xfs, btrfs, f2fs, iso9660 with Rock Ridge, ...) hit this for any name written by a non-UTF-8
   system. The robustness corpus has four such syzbot images (`ts/tests/robustness/FINDINGS.md`,
   corpus notes).

On Windows, host-side code calls ANSI APIs: `CreateFileA` in `raw_backend.c`, `CreateFile` (=A) in
QEMU's `block/file-win32.c` (three sites) plus `qemu_open`/`unlink`, `getenv("TEMP")` + `mkstemp`
in `anyfs_probe.c`, `fopen`/`getenv` in `anyfs_container.c`, `anyfs_tls.c` and `ksmbd_main.c`, and
`main(argc, argv)` + `printf` in every CLI. A path outside the system code page fails to open, and
UTF-8 console output is garbled. The native addon runs inside `electron.exe`, so an
`activeCodePage=UTF-8` manifest cannot help it; only explicit W calls can. drivelist-anyfs already
uses W calls and GLib already takes UTF-8 filenames on Windows.

## Decisions

- Scope: A (FAT long names), B (byte-preserving transport), C (legacy-encoding display, which also
  selects the FAT short-name codepage), D (W-API host boundary on Windows).
- Transport escape: invalid bytes map into the private-use range U+EF80–U+EFFF.
- Default legacy encoding follows the UI language (CLIs: the system locale / ANSI code page).
- Windows: one shared layer for all CLIs, with no per-call-site edits in the tools.
- Long paths (over `MAX_PATH`) on Windows are a known issue, to be handled in the Electron layer later.

## Part A: kernel side

`anyfs_mount_opts()` takes the enter flags instead of `int rdonly`:
`int anyfs_mount_opts(const char* fstype, uint32_t flags, char* buf, size_t cap)`.

- `vfat`: add `utf8` (UTF-16 long names come out as UTF-8; the kernel documents `utf8` rather than
  `iocharset=utf8`, which breaks case-insensitive lookup).
- `vfat` and `msdos`: add `codepage=NNN` when the flags select one (below); short names use it.
- `ANYFS_MOUNT_OPTS_MAX` grows if the longest combination needs it.

New public flags in `include/anyfs.h`, a 4-bit field at bits 8–11 of the enter flags (bits 0–1 are
`RDONLY` and `REPLACE`):

```c
#define ANYFS_MOUNT_FAT_CP_SHIFT 8
#define ANYFS_MOUNT_FAT_CP_MASK (0xfu << ANYFS_MOUNT_FAT_CP_SHIFT)
#define ANYFS_MOUNT_FAT_CP_437 (0u << ANYFS_MOUNT_FAT_CP_SHIFT) /* default */
#define ANYFS_MOUNT_FAT_CP_936 (1u << ANYFS_MOUNT_FAT_CP_SHIFT)
#define ANYFS_MOUNT_FAT_CP_950 (2u << ANYFS_MOUNT_FAT_CP_SHIFT)
#define ANYFS_MOUNT_FAT_CP_932 (3u << ANYFS_MOUNT_FAT_CP_SHIFT)
#define ANYFS_MOUNT_FAT_CP_949 (4u << ANYFS_MOUNT_FAT_CP_SHIFT)
```

The session layer passes the field through to `anyfs_mount` / `anyfs_mount_blkdev` with `RDONLY`
(it already builds `mflags`; both the partition and the whole-disk paths). All the NLS tables are
already built in (`CONFIG_NLS_CODEPAGE_{437,932,936,949,950}=y`). The codepage applies to new
mounts only: a partition already mounted keeps its codepage until the disk is reopened.

`utf8` on vfat also changes what anyfs-fuse, anyfs-ksmbd and anyfs-nfsd serve, which is the point:
they get correct long names too.

## Part B: byte-preserving transport

New pure-string unit `src/core/anyfs_name.{c,h}` (no LKL; unit-tested like `anyfs_mount_opts.c`):

- `anyfs_name_escape(const char* in, char* out, size_t cap)`: valid UTF-8 is copied unchanged,
  except characters in U+EF80–U+EFFF; every byte of an invalid sequence, and every byte of such a
  character, becomes U+EF00 + byte (always 0x80–0xFF, since bytes below 0x80 are valid UTF-8).
  "Valid" means strict UTF-8: no overlong forms, no surrogates (`ED A0..BF xx`), nothing above
  U+10FFFF.
- `anyfs_name_unescape(const char* in, char* out, size_t cap)`: U+EF80–U+EFFF become the single
  byte 0x80–0xFF; everything else is copied unchanged.
- `unescape(escape(b)) == b` for every byte string `b`, and the escaped form is always valid UTF-8.
  Both return the output length or a negative value on overflow.

`ts/native/anyfs_ts.c` is the only place where escaping happens; all three backends (browser wasm,
Node wasm, native addon) go through it:

- **Out:** readdir `name`; `realpath` and `readlink` results; the string fields of
  `session_list_json` and `session_meta_json` (a FAT volume label is OEM bytes, as libblkid
  returns it).
- **In:** every LKL path argument (`readdir`, `lstat`, `stat`, `realpath`, `readlink`, `open`,
  `read_kernel_file`) is unescaped before it reaches LKL. `session_open`'s image path is a host
  path and is not touched.
- An escaped string is at most 3× its byte length (each escaped byte is a 3-byte UTF-8 character);
  unescaping never grows a string. Escaping writes into a temporary buffer of that size, the result
  goes into the caller's buffer, and an overflow fails the call the same way an oversized result
  does today.

JavaScript never sees an unescaped U+EF80–U+EFFF character coming from a filesystem, so the round
trip is exact. The `jsonw.h` writer and its other users (fuse, sysfs) are unchanged.

## Part C: display and settings

### `@anyfs/core`: `src/names.ts` (pure functions, exported)

- `type LegacyEncoding = 'gb18030' | 'big5' | 'shift_jis' | 'euc-kr' | 'windows-1252' | 'off'`
- `hasEscapedBytes(name)`: any character in U+EF80–U+EFFF.
- `nameToBytes(name): Uint8Array`: escaped characters become their byte, the rest is UTF-8.
- `displayName(name, enc)`: no escaped bytes → `name`. Otherwise, unless `enc` is `'off'`, decode
  all of `nameToBytes(name)` with `new TextDecoder(enc, { fatal: true })`; if that throws (or
  `enc` is `'off'`), show valid runs as they are and each escaped byte as `\xNN`.
  The whole name is decoded, not just the escaped runs: a legacy name can contain byte pairs that
  happen to be valid UTF-8 (GBK `C4 A3`), so run-wise decoding would garble it.
- `defaultLegacyEncoding(lang)`: `zh-CN`/`zh-SG`/`zh-Hans*`/bare `zh` → `gb18030`;
  `zh-TW`/`zh-HK`/`zh-MO`/`zh-Hant*` → `big5`; `ja*` → `shift_jis`; `ko*` → `euc-kr`; anything else
  → `windows-1252`.
- `fatCodepageFlag(enc)`: `gb18030` → `ANYFS_MOUNT_FAT_CP_936`, `big5` → `_950`, `shift_jis` →
  `_932`, `euc-kr` → `_949`, else `_437` (Western OEM short names are 437/850, not 1252).
- The flag constants are exported next to the existing mount-flag constants.

### `@anyfs/trees`: `AnyfsFileBrowser`

New optional prop `formatName?: (name: string) => string`, applied to the row name, the extension,
the sort key (`localeCompare` on the displayed name), breadcrumb segments and the properties dialog.
Row ids, paths and the navigation hash keep the raw (escaped) name, so every operation still
addresses the right file. Without the prop nothing changes.

### vite-demo

- `Settings.legacyEncoding: 'auto' | LegacyEncoding`, default `'auto'` (resolved with
  `defaultLegacyEncoding(navigator.language)`). The settings dialog offers Auto, the five encodings
  and Off, and notes that FAT short names follow the setting from the next time a disk is opened.
  Display follows a change at once.
- `DiskView` passes `fatCodepageFlag(resolved)` in the `enter` flags (`Session.enter(part, flags)`
  already carries flags to the wasm API op, the worker and the Electron `diskEnter` IPC).
- Partition labels and the download filename use `displayName`. The service-worker download
  already sends `filename*=UTF-8''…`; the Electron IPC download writes with Node's `fs`, which uses
  W calls on Windows.
- The deep-link hash keeps raw names; `encodeURIComponent` round-trips the escape characters.

### CLIs

A shared option `--legacy-encoding=auto|gb18030|big5|shift_jis|euc-kr|windows-1252|off` in
`anyfs-lspart`, `anyfs-ksmbd`, `anyfs-nfsd` and `anyfs-fuse`, parsed by helpers in
`anyfs_name.c`:

- `auto` (default): on Windows from `GetACP()` (936 → gb18030, 950 → big5, 932 → shift_jis,
  949 → euc-kr, else windows-1252); elsewhere from the language of `LC_ALL` / `LC_CTYPE` / `LANG`,
  with the same table as `defaultLegacyEncoding`.
- The servers and fuse OR `fatCodepageFlag` into the enter flags they already pass
  (`anyfs_share_open_disks(..., enter_flags, ...)`, `anyfs_session_enter[_path]` in fuse).
- `anyfs-lspart` prints labels decoded with the encoding: on Windows with
  `MultiByteToWideChar(cp)` (gb18030 → 54936, big5 → 950, shift_jis → 932, euc-kr → 949,
  windows-1252 → 1252), elsewhere with GLib `g_convert`; if decoding fails, the `\xNN` form.
  `--json` output uses the Part B escape, so it is always valid UTF-8.
- anyfs-fuse and anyfs-nfsd pass name bytes through unchanged (the Unix convention: the client's
  locale decides). anyfs-ksmbd: see known issues.

## Part D: Windows host boundary

Rule: inside anyfs every string is UTF-8. On Windows, every host call that takes or returns text
converts at the boundary and calls the W API. Nothing depends on the ANSI code page or on a
manifest.

### The layer: `src/win32/anyfs_u8.{c,h}`, static library `anyfs_u8`

Built on Windows only; on other hosts the header maps each function to its POSIX counterpart, so
callers need no `#ifdef`.

- Conversion: `anyfs_u8_to_u16` / `anyfs_u16_to_u8` (strict, `MB_ERR_INVALID_CHARS`; invalid input
  is an error, never a silent `?`).
- Files and environment: `anyfs_u8_open`, `anyfs_u8_fopen`, `anyfs_u8_stat`, `anyfs_u8_access`,
  `anyfs_u8_unlink`, `anyfs_u8_getenv` (result cached per name for the process lifetime, like
  `getenv`), `anyfs_u8_create_file` (`CreateFileW`), `anyfs_u8_tmpfile_fd` (`GetTempPathW` +
  `_wopen(... _O_CREAT | _O_EXCL | _O_TEMPORARY | _O_BINARY)`, removed on close; today's
  `mkstemp` + `unlink` cannot delete an open file on Windows and leaves it behind).
- Entry: `int wmain(int argc, wchar_t** wargv)` converts the arguments to UTF-8 and calls
  `anyfs_tool_main(argc, argv)`. It also installs GLib print/printerr handlers and a log writer
  that use the output functions below, so `g_print`/`g_warning` from GLib, QEMU and ksmbd-tools are
  not converted to the ANSI code page.
- Output: `anyfs_u8_vfprintf` and friends. For `stdout`/`stderr` attached to a console
  (`GetConsoleMode` succeeds): flush the CRT stream, convert, `WriteConsoleW`; an incomplete UTF-8
  sequence at the end of a write is held per stream and joined to the next write. Redirected to a
  file or pipe: the UTF-8 bytes unchanged. Any other `FILE*` goes to the CRT.

### CLIs: force-included, no source edits

meson adds, for every executable target built for Windows (`anyfs-lspart`, `anyfs-ksmbd`,
`anyfs-nfsd`, the test programs), `-municode` to the link arguments and
`-Dmain=anyfs_tool_main -include src/win32/anyfs_u8_redirect.h` to the compile arguments of all
their sources, including the ksmbd-tools sources built into `anyfs-ksmbd`. The header first includes
the system headers (`stdio.h`, `stdlib.h`, `io.h`, `fcntl.h`, `sys/stat.h`), then defines
function-like macros: `printf`, `vprintf`, `fprintf`, `vfprintf`, `puts`, `fputs`, `fputc`, `putc`,
`putchar`, `fwrite`, `perror`, `open`, `fopen`, `stat`, `access`, `unlink`, `getenv`, `mkstemp`.
Function-like macros leave `struct stat` and `.open =` alone.

### core and the addon: explicit calls

`src/core` is also linked into `anyfs_native.node`, and it has member calls such as `ops->open(...)`
that a function-like `open` macro would rewrite, so core is not force-included. Its host-facing
sites call the layer directly: `raw_backend.c` (`anyfs_u8_create_file`), `anyfs_probe.c`
(`anyfs_u8_tmpfile_fd`, `anyfs_u8_getenv`), `anyfs_container.c` (`anyfs_u8_fopen`,
`anyfs_u8_getenv`), `anyfs_tls.c`, `anyfs_session.c` and `qemu_thread.c` (`anyfs_u8_getenv`). The
addon links `anyfs_u8` and gets the same behaviour.

### QEMU: `patches/qemu/0012-win32-utf8-filenames.patch`

Native series only (`series.native`). `block/file-win32.c`: the three `CreateFile` calls become
`CreateFileW` on `g_utf8_to_utf16(filename)`, `unlink` becomes `g_unlink`. `util/osdep.c`: on
Windows, `qemu_open_cloexec` uses `g_open` (GLib converts to `_wopen`). The snapshot overlay is
created by `g_mkstemp`, which is already UTF-8 aware. Per the QEMU patch rule, this is a new patch,
not an edit of an existing one.

### Import gate: `scripts/check_win_imports.sh`

Like `check_linux_abi.sh`: lists the PE imports of the shipped anyfs binaries (`anyfs-*.exe`,
`anyfs_native.node`) with `objdump -p` and fails on ANSI file, path and environment imports
(`CreateFileA`, `FindFirstFileA`, `GetTempPathA`, `GetModuleFileNameA`, `DeleteFileA`,
`MoveFileA`, `CreateDirectoryA`, `GetFileAttributesA`, `fopen`, `_open`/`open`, `_stat*`/`stat`,
`_access`/`access`, `_unlink`/`unlink`, `getenv`, `mkstemp`, `_mktemp*`). Imports from statically
linked third-party code that anyfs never hands a user path (libblkid's `open` if present, for
example) go in an allowlist in the script, one line each with the reason. QEMU's DLL is checked
functionally (test below), since it keeps unused ANSI callers such as the pidfile writer. The gate
runs locally with the wine tests; wiring it into `mingw64.yml` is a CI change and needs separate
approval.

## Testing

| part | test |
| ---- | ---- |
| A | `tests/unit/test_mount_opts.c`: `utf8` + `codepage` for vfat, `codepage` only for msdos, each FAT_CP value, the default |
| A, B | `tests/test_session_names.c` (unit suite, Linux): a hand-written FAT12 image (LFN `中文.txt` and `café.txt`, an 8.3 entry whose name is GBK bytes) and an ext4 image (`mkfs.ext4 -d`, skip with 77 if missing) holding a raw GBK name, a raw Latin-1 name, a name containing the character U+EF80 and one containing `ED A0 80`. Built with `ts/native/anyfs_ts.c`; for every entry: readdir name → lstat → open → pread equals the content, through the glue's string API. FAT with `FAT_CP_936` shows the 8.3 name as `中文`; with the default it does not. |
| A | the same check for the other filesystems the tools can build with non-ASCII names (exfat, ntfs via ntfs-3g tools, iso9660 Joliet and Rock Ridge via xorriso, udf, hfsplus where available), to confirm the defaults the Context section reads from the kernel source |
| B | `tests/unit/test_name_escape.c`: every 1-, 2- and 3-byte string round-trips and escapes to valid UTF-8; 10⁶ random strings up to 64 bytes; known vectors (overlong `C0 AF`, surrogate `ED A0 80`, `F4 90 80 80`, U+EF80 itself) |
| C | core vitest `names.test.ts`: `displayName` per encoding, the `\xNN` fallback, `defaultLegacyEncoding`, `fatCodepageFlag`, `nameToBytes` against the C vectors |
| C | E2E fixture `unicodeNames` (the FAT + ext4 images above) and `flows/unicode-names.spec.ts` on web, electron-wasm and electron-native: displayed names under `gb18030`, a downloaded file's bytes and name |
| B | robustness gate re-run: the four syzbot cases with non-UTF-8 names now stat their entries; their class may change from `error` to `ok` (expected, recorded in `FINDINGS.md`) |
| D | under wine (temporary `WINEPREFIX`, existing recipes): `tests/unit/test_u8.c` (conversion, strictness, the held partial sequence); `anyfs-lspart.exe` with an image in a directory named `测试 café` given on the command line, its stdout through a pipe byte-identical to the expected UTF-8; the win64 addon (the `~/.cache/anyfs-scratch/ewine` probe) opening raw, qcow2 and vmdk images under that directory; a snapshot open with `TEMP` set to a non-ASCII directory; `check_win_imports.sh` |

## Delivery order

A → D → B → C, each committed and pushed on its own with its tests:

1. A: FAT long names are fixed for every surface, CLI servers included.
2. D: Windows host paths and console output.
3. B: the escape in the glue. Every file opens; until C, escaped bytes show as private-use
   characters.
4. C: display, settings, CLI option.

## Known issues and out of scope

- **Long paths on Windows** (over `MAX_PATH`): handled in the Electron layer later.
- **A legacy name that happens to be valid UTF-8** is shown as UTF-8; it cannot be told apart.
- **A name mixing UTF-8 and a legacy encoding** is decoded as a whole with the legacy encoding.
- **anyfs-ksmbd and non-UTF-8 names:** the in-kernel ksmbd converts names with the `utf8` NLS table
  and replaces invalid bytes with `?` (`fs/smb/server/unicode.c`), so such files show as `?` to SMB
  clients and cannot be opened. Fixing it needs an LKL kernel patch; not in this round.
- **Classic HFS** names are Mac Roman bytes (no NLS by default); `displayName` does not offer
  `macintosh`. Out of scope.
- **Windows console input** (`ReadConsoleW`): no CLI reads interactive input today; the layer gets
  it when one does.

## Amendments during implementation

- **A, test image.** The 8.3-only name on the FAT partition is GBK `测试` (`B2 E2 CA D4`), not
  `中文`: FAT lookup is case-insensitive, so under codepage 936 an 8.3 name `中文.TXT` is the same
  name as the long name `中文.txt`, and opening it opened the other file. The image generator is
  `tests/make_names_image.py`; the C test links the glue from a static library `ts_glue`
  built with `-Dmain=anyfs_ts_main_unused` (the glue defines `main()` for wasm).
- **A, other filesystems (checked 2026-10-07 through the native addon, no mount options):**
  exFAT, UDF (`mkudffs`), NTFS PLUS (`ntfscp`), iso9660 with Joliet, and iso9660 with Joliet and
  Rock Ridge list and stat `中文.txt` and `café.txt` as UTF-8, as the kernel source suggested.
- **A, robustness gate:** after the `utf8` option, both backends give the same class for every
  case (ok 40 / error 60).
- **D, layer split.** `anyfs_u8.h` (force-included) must not include `<windows.h>`: its
  macros collide with QEMU's QAPI enums and ksmbd-tools' RPC names. `HANDLE`-typed
  `anyfs_u8_create_file` lives in `anyfs_u8_win.h`, which only `raw_backend.c` includes. Core is
  force-included with the output half only (`anyfs_u8_stdio.h`); the CLIs get
  `anyfs_u8_redirect.h` (output + file + environment), `-Dmain=anyfs_tool_main`, and `wmain()`
  from `anyfs_u8_main.c` (`-municode` at link time only, so `UNICODE` stays undefined).
- **D, import gate.** `scripts/check_win_imports.sh` reads the undefined symbols of anyfs's own
  objects (core archive, the u8 libraries, ksmbd-tools as built into anyfs-ksmbd, host_proxy,
  every CLI object), not the PE import tables: statically linked libblkid and the mingw CRT
  helpers it pulls in (dirent, the stat fallback) import ANSI functions too, and anyfs never
  hands them a path. A deliberately bad object (`fopen`, `CreateFileA`) fails the gate.
- **D, a Win64 bug found on the way.** The session layer hung on every mount under wine. Cause:
  `lkl_sys_ioctl(fd, LKL_BLKROSET, (long)&ro)` from d8f8091 — `long` is 32 bits on Win64, so the
  pointer was truncated. Fixed separately (b27508f) by casting through `uintptr_t`, also in
  `anyfs_dm.c`. `tests/test_open_paths.c` + `tests/wine/u8-cli.sh` catch it (300 s timeout with
  the old cast).
- **D, wine verification (2026-10-07).** Images in `测试 café/`, `TEMP` in `临时 temp/`:
  `anyfs-lspart.exe` lists and types raw, qcow2 and vmdk images, its piped output is UTF-8;
  `test_open_paths.exe` mounts the FAT partition and reads `中文.txt` with the raw backend, the
  QEMU backend (raw, qcow2, vmdk) and QEMU snapshot mode; `test_u8.exe` passes. The Electron
  probe could not be run in this environment (Electron 42 under wine dies in
  `hwnd_util.cc` without a usable display); `test_open_paths.exe` exercises the same core code.
- **B, buffers.** `realpath` / `readlink` results are escaped in place; when the escaped form does
  not fit, the call returns `-(bytes needed)` like the `*_json` helpers (callers that retry do;
  the others report an error). Incoming paths are unescaped into a 16 KiB stack buffer.
- **B, verification.** `tests/test_session_names.c` lists, stats and reads every ext4 byte name
  and checks the escaped GBK FAT label through the glue; the robustness case runner walks the
  names image on wasm and native (2 partitions, 8 files, 80 bytes, all read). No robustness case
  changed class: the four syzbot images with non-UTF-8 names fail on other corruption.
- **C, CLI decoding** lives in `src/core/anyfs_legacy.{c,h}` (`include/anyfs_legacy.h`), not in
  `anyfs_name.c`, which stays pure string logic. Linux uses libc `iconv` directly rather than
  GLib: the shipped binaries link glibc dynamically, so the system's gconv modules provide
  GB18030/Big5/Shift_JIS/EUC-KR/CP1252 (checked with the zig-built toolchain). If a converter is
  missing, names show as `\xNN`. On Windows `MultiByteToWideChar` (GB18030 = code page 54936).
  anyfs-fuse takes the option as `--legacy-encoding=ENC` or `-o legacy_encoding=ENC`.
- **C, test image.** The FAT partition carries a GBK volume label (`测试`) as a root-directory
  volume entry, which is where libblkid reads it (a boot-sector label alone was not reported). The E2E checks it in the partition picker.
- **C, E2E.** `flows/unicode-names.spec.ts` passes on web, electron-native and electron-wasm.
  Drivers gained `setLegacyEncoding`, `listDisplayNames`, `partitionLabel`, and `download()`
  returns the saved file name (Electron: `savedAs`). The vite-demo serves the wasm from
  `public/wasm/`, synced from `@anyfs/core` by `scripts/sync_wasm_bundle.sh`; a stale copy there
  made the first run show `??.txt`.

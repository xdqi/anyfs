# Unicode and legacy-encoded filenames: implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** every file on an image can be listed, opened and downloaded whatever bytes its name is
made of, names are shown readably, and on Windows every host path and console write goes through
the W APIs.

**Architecture:** spec `docs/superpowers/specs/2026-10-07-unicode-filenames-design.md`. Four parts
landed in the order A → D → B → C: FAT mount options (A), a W-API layer for Windows hosts (D), a
byte-preserving escape in the TS glue (B), display + settings + CLI option (C).

**Tech stack:** C (LKL, meson, zig cc / mingw), QEMU patch, TypeScript (node:test, React, Chonky),
Playwright, wine.

**Build/test commands used throughout:**

- Linux core + C tests: `ninja -C build-anyfs-linux-amd64 && meson test -C build-anyfs-linux-amd64 --suite unit --print-errorlogs`
- mingw: `ninja -C build-anyfs-mingw64` (toolchain `/opt/msys2-cross`; QEMU DLL from `~/qemu/build-anyfs-mingw64`)
- wine: temporary `WINEPREFIX`, `WINEDLLOVERRIDES="mscoree,mshtml="`, `WINEPATH` = `~/qemu/build-anyfs-mingw64;lkl-mingw64/tools/lkl/lib;/opt/msys2-cross/mingw64/bin` (exit 53 with no output = missing DLL; `WINEDEBUG=err+module` names it)
- Node wasm bundle: `ANYFS_TARGET=node scripts/build_anyfs_wasm.sh`; addon: `bash ts/packages/anyfs-native/scripts/build-linux-electron.sh`
- TS: `pnpm -C ts -r --filter './packages/*' --filter '!@anyfs/native' build`, `pnpm -C ts --filter @anyfs/core run test:unit`
- E2E: `cd ts/tests/e2e && npx playwright test --project=web <spec>`

---

## Part A — FAT names

### Task A1: mount options carry `utf8` and the short-name codepage

**Files:** modify `src/core/anyfs_mount_opts.{c,h}`, `tests/unit/test_mount_opts.c`, `src/core/anyfs_mount.c`

`anyfs_mount_opts.c` stays LKL-free, so it takes the numeric codepage, not the flags:
`int anyfs_mount_opts(const char* fstype, int rdonly, unsigned fat_cp, char* buf, size_t cap)`;
`fat_cp == 0` means "no codepage option" (kernel default 437).

- [ ] **Step 1: failing tests.** In `test_mount_opts.c`, `expect()` gains a `fat_cp` argument (all
  existing calls pass 0) and the vfat expectations change; add:

```c
	expect("vfat", 1, 0, "utf8,errors=continue");
	expect("vfat", 0, 0, "utf8,errors=remount-ro");
	expect("vfat", 1, 936, "utf8,codepage=936,errors=continue");
	expect("msdos", 1, 936, "codepage=936,errors=continue");
	expect("msdos", 1, 0, "errors=continue");
	expect("ext4", 1, 936, "noload,errors=continue"); /* not FAT: ignored */
```

  and in the "fits ANYFS_MOUNT_OPTS_MAX" loop iterate `fat_cp` over `{0, 437, 932, 936, 949, 950}`.
- [ ] **Step 2:** `meson test -C build-anyfs-linux-amd64 mount_opts` → compile error / FAIL.
- [ ] **Step 3: implement.**

```c
	if (strcmp(fstype, "vfat") == 0 && append(buf, cap, &len, "utf8"))
		goto overflow;
	if (fat_cp && (strcmp(fstype, "vfat") == 0 ||
		       strcmp(fstype, "msdos") == 0)) {
		char cp[16];
		snprintf(cp, sizeof(cp), "codepage=%u", fat_cp);
		if (append(buf, cap, &len, cp))
			goto overflow;
	}
```

  placed before the `errors=` append. Header comment documents `fat_cp`. In `anyfs_mount.c` the two
  call sites pass `anyfs_mount_fat_cp(flags)` (Task A2).
- [ ] **Step 4:** test passes.

### Task A2: `ANYFS_MOUNT_FAT_CP_*` flags through the session layer

**Files:** modify `include/anyfs.h`, `src/core/anyfs_mount.c`, `src/core/anyfs_session.c`

- [ ] **Step 1:** add to `include/anyfs.h` after `ANYFS_MOUNT_REPLACE` (with a doc line in the flags
  comment block):

```c
#define ANYFS_MOUNT_FAT_CP_SHIFT 8
#define ANYFS_MOUNT_FAT_CP_MASK (0xfu << ANYFS_MOUNT_FAT_CP_SHIFT)
#define ANYFS_MOUNT_FAT_CP_437 (0u << ANYFS_MOUNT_FAT_CP_SHIFT)
#define ANYFS_MOUNT_FAT_CP_936 (1u << ANYFS_MOUNT_FAT_CP_SHIFT)
#define ANYFS_MOUNT_FAT_CP_950 (2u << ANYFS_MOUNT_FAT_CP_SHIFT)
#define ANYFS_MOUNT_FAT_CP_932 (3u << ANYFS_MOUNT_FAT_CP_SHIFT)
#define ANYFS_MOUNT_FAT_CP_949 (4u << ANYFS_MOUNT_FAT_CP_SHIFT)
```

- [ ] **Step 2:** `anyfs_mount.c`: static helper

```c
static unsigned anyfs_mount_fat_cp(uint32_t flags)
{
	static const unsigned cps[] = {0, 936, 950, 932, 949};
	unsigned i = (flags & ANYFS_MOUNT_FAT_CP_MASK) >>
		     ANYFS_MOUNT_FAT_CP_SHIFT;
	return i < sizeof(cps) / sizeof(cps[0]) ? cps[i] : 0;
}
```

- [ ] **Step 3:** `anyfs_session.c`: both `mflags` computations (partition path in
  `enter_fs_slot`, whole-disk path in `anyfs_session_enter`) also keep
  `flags & ANYFS_MOUNT_FAT_CP_MASK`.
- [ ] **Step 4:** build; unit suite green.

### Task A3: names test image + session test (FAT half)

**Files:** create `tests/make_names_image.py`, `tests/test_session_names.c`; modify `meson.build`

`make_names_image.py <out.img>` writes a 16 MiB MBR disk: p1 (sector 2048, 8192 sectors) a
hand-written FAT12 filesystem, p2 (sector 10240, 16384 sectors) an ext4 filesystem built by
`mkfs.ext4 -d` from a temp dir holding byte-named files. Exit 77 if `mkfs.ext4` is missing.

FAT12 layout (like `test_session_whole_part.c`): 512-byte sectors, 4 sectors/cluster, 1 reserved,
2 FATs × 6 sectors, 16 root entries (1 sector), data from sector 14. Root directory:

| entry | long name | 8.3 entry bytes | content |
| ----- | --------- | --------------- | ------- |
| 1 | `中文.txt` | `CN~1    TXT` | `fat-cn\n` |
| 2 | `café.txt` | `CAFE~1  TXT` | `fat-cafe\n` |
| 3 | — | `D6 D0 CE C4 20 20 20 20 54 58 54` (GBK 中文) | `fat-gbk\n` |

Each file is one cluster; FAT12 chains end at `0xFFF`. LFN entries: attribute `0x0F`, checksum
`sum = ((sum & 1) << 7) + (sum >> 1) + c` over the 11 short-name bytes, UTF-16LE name split
5/6/2 across `name1/name2/name3`, terminated with `0x0000` then `0xFFFF` padding, sequence number
`0x40 | n` on the last (first stored) slot.

ext4 files (names as bytes, content = `ext4-<n>\n`): `b"\xd6\xd0\xce\xc4.txt"` (GBK),
`b"caf\xe9.txt"` (Latin-1), `"\uef80.txt".encode()` (a real U+EF80), `b"\xed\xa0\x80.txt"`
(surrogate bytes), `b"plain.txt"`.

`tests/test_session_names.c` (unit suite, POSIX-only block in meson, compiled with
`ts/native/anyfs_ts.c` and `include_directories('src/core')`): runs the generator with
`system()`, boots with `anyfs_ts_kernel_init(64, 0)`, opens the image twice with
`anyfs_ts_session_open(img, ANYFS_SESSION_READONLY | ANYFS_BACKEND_RAW)`, and for a partition:
`anyfs_ts_session_enter` → `anyfs_ts_readdir_json` → a small JSON string extractor (handles `\"`,
`\\`, `\uXXXX`) → for each name: `anyfs_ts_lstat_json`, `anyfs_ts_open` + `anyfs_ts_pread`
compared with the expected content. Part A checks:

- p1 with flags 0: names exactly `{"中文.txt", "café.txt", "╓╨╬─.TXT"}`.
- p1 on the second session with `ANYFS_MOUNT_FAT_CP_936`: `{"中文.txt", "café.txt", "中文.TXT"}`.

- [ ] **Step 1:** write the generator and the test; register the test in meson.
- [ ] **Step 2:** run → FAIL (`??.txt`, `caf\xe9.txt`).
- [ ] **Step 3:** with A1/A2 in place → PASS.

### Task A4: verify the other filesystems' defaults

- [ ] Scratch script (`~/.cache/anyfs-scratch/names-matrix.sh`, not committed): exfat
  (`mkfs.exfat` + `mcopy`-like via a loop mount under sudo), ntfs (`mkntfs` + `ntfscp`), iso9660
  Joliet + Rock Ridge (`xorriso -joliet on`), udf (`mkudffs` if present), hfsplus (`mkfs.hfsplus` if
  present), each with `中文.txt` and `café.txt`; list through the addon. Record the result as an
  "Amendments" note in the spec.
- [ ] **Commit + push A** (`fix(core): FAT long names as UTF-8, selectable short-name codepage`),
  after `git fetch && git log origin/main..main` shows only these commits.

---

## Part D — Windows host boundary

### Task D1: `src/win32/anyfs_u8.{c,h}`

**Files:** create `src/win32/anyfs_u8.h`, `src/win32/anyfs_u8.c`, `tests/unit/test_u8.c`; modify `meson.build`

API (header; on non-Windows each maps to the libc call):

```c
int anyfs_u8_to_u16(const char* s, wchar_t** out);   /* 0 / -1, malloc'd */
int anyfs_u16_to_u8(const wchar_t* s, char** out);   /* 0 / -1, malloc'd */
size_t anyfs_u8_complete_prefix(const char* s, size_t n); /* bytes up to the last complete sequence */
int anyfs_u8_open(const char* path, int flags, ...);
FILE* anyfs_u8_fopen(const char* path, const char* mode);
int anyfs_u8_stat(const char* path, struct stat* st);
int anyfs_u8_access(const char* path, int mode);
int anyfs_u8_unlink(const char* path);
char* anyfs_u8_getenv(const char* name);
int anyfs_u8_tmpfile_fd(void);
#ifdef _WIN32
HANDLE anyfs_u8_create_file(const char* path, DWORD access, DWORD share,
			    DWORD disposition, DWORD attrs);
#endif
int anyfs_u8_vfprintf(FILE* f, const char* fmt, va_list ap);
int anyfs_u8_fprintf(FILE* f, const char* fmt, ...);
int anyfs_u8_printf(const char* fmt, ...);
int anyfs_u8_fputs(const char* s, FILE* f);
int anyfs_u8_puts(const char* s);
int anyfs_u8_fputc(int c, FILE* f);
size_t anyfs_u8_fwrite(const void* p, size_t sz, size_t n, FILE* f);
void anyfs_u8_perror(const char* s);
```

Windows: conversions via `MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, …)` /
`WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, …)`; `getenv` caches converted values in a
mutex-guarded list; output functions: if `f` is `stdout`/`stderr` and `GetConsoleMode` succeeds on
its handle, format into a heap buffer, prepend the stream's held partial sequence, write the
`anyfs_u8_complete_prefix` part with `WriteConsoleW`, hold the rest (≤ 3 bytes); else `fwrite`
the bytes. `anyfs_u8_complete_prefix` is pure and built on every platform.

- [ ] **Step 1:** `test_u8.c`: `complete_prefix` on `"a"`, `"\xe4\xb8"` (→ 0), `"\xe4\xb8\xad"`,
  `"x\xf0\x9f"` (→ 1), invalid lead `"\xff"` (→ 1, passes through); on Windows also round-trip
  `中文 café 😀` through `to_u16`/`to_u16`→`to_u8`, reject `"\xed\xa0\x80"`.
- [ ] **Step 2:** FAIL (no library). **Step 3:** implement. **Step 4:** Linux test green; mingw
  build `test_u8.exe` green under wine.

### Task D2: CLIs: `wmain`, force-included redirects

**Files:** create `src/win32/anyfs_u8_main.c`, `src/win32/anyfs_u8_redirect.h`; modify `meson.build`, `src/lspart/meson.build`

- `anyfs_u8_main.c` (Windows only, linked into each CLI):

```c
int anyfs_tool_main(int argc, char** argv);

int wmain(int argc, wchar_t** wargv)
{
	char** argv = calloc((size_t)argc + 1, sizeof(char*));
	for (int i = 0; i < argc; i++)
		if (!argv || anyfs_u16_to_u8(wargv[i], &argv[i]) < 0) {
			fputs("invalid command line\n", stderr);
			return 2;
		}
	anyfs_u8_install_glib_handlers(); /* no-op without GLib */
	return anyfs_tool_main(argc, argv);
}
```

- `anyfs_u8_redirect.h`: includes `stdio.h stdlib.h string.h io.h fcntl.h sys/stat.h
  anyfs_u8.h`, then `#undef` + function-like macros: `printf(...)`, `vprintf`, `fprintf`,
  `vfprintf`, `puts`, `fputs`, `fputc`, `putc`, `putchar`, `fwrite`, `perror`, `open(...)`,
  `fopen`, `stat(p, b)`, `access`, `unlink`, `getenv`, `mkstemp`.
- meson: `win_u8_args = ['-include', meson.project_source_root() / 'src/win32/anyfs_u8_redirect.h']`,
  `win_u8_main = ['-Dmain=anyfs_tool_main']`, `win_u8_link = ['-municode']`, all empty off
  Windows. Add to `anyfs-lspart`, `anyfs-ksmbd`, `anyfs-nfsd`, `ksmbd_tools_lib` (args only, it
  already renames its own `main`), and the Windows-built tests. Link `anyfs_u8_main.c` + `anyfs_u8`.
- [ ] **Step 1:** wine check script `tests/wine/u8-cli.sh` (local): copy `multi.img` into
  `$TMP/测试 café/`, run `anyfs-lspart.exe "$TMP/测试 café/multi.img" | od -c` → today fails to open.
- [ ] **Step 2:** implement; rebuild mingw; the script lists partitions and the pipe output is
  valid UTF-8 (`iconv -f UTF-8 -t UTF-8` succeeds).

### Task D3: core call sites

**Files:** modify `src/core/raw_backend.c`, `src/core/anyfs_probe.c`, `src/core/anyfs_container.c`, `src/core/anyfs_tls.c`, `src/core/anyfs_session.c`, `src/core/qemu_thread.c`, `meson.build` (core links `anyfs_u8`)

- `raw_backend.c` Windows: `HANDLE hFile = anyfs_u8_create_file(path, access, share, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);`
- `anyfs_probe.c` Windows branch: `hfd = anyfs_u8_tmpfile_fd();` (replaces the `getenv("TEMP")` +
  `mkstemp` + `unlink` block).
- `anyfs_container.c`: `getenv` → `anyfs_u8_getenv`, `fopen` → `anyfs_u8_fopen`.
- `anyfs_tls.c`, `anyfs_session.c`, `qemu_thread.c`: `getenv` → `anyfs_u8_getenv`.
- [ ] Build Linux (unchanged behaviour, unit suite green) and mingw.

### Task D4: QEMU patch 0012

**Files:** create `patches/qemu/0012-win32-utf8-filenames.patch`; modify `patches/qemu/series.native`

- `block/file-win32.c`: a helper

```c
static HANDLE win32_open_utf8(const char *filename, DWORD access, DWORD share,
                              DWORD disposition, DWORD flags)
{
    wchar_t *w = g_utf8_to_utf16(filename, -1, NULL, NULL, NULL);
    HANDLE h = w ? CreateFileW(w, access, share, NULL, disposition, flags, NULL)
                 : INVALID_HANDLE_VALUE;
    if (!w) {
        SetLastError(ERROR_INVALID_NAME);
    }
    g_free(w);
    return h;
}
```

  used at the three `CreateFile` sites; `unlink(bs->filename)` → `g_unlink`.
- `util/osdep.c`: in `qemu_open_cloexec`, `#ifdef _WIN32` use `g_open(name, flags, mode)`.
- The wasm build applies every patch in the directory; the changes are `_WIN32`-only, so wasm is
  unaffected but the patch must apply after 0001–0011 (check with `build_qemu_wasm.sh`'s apply step
  on a clean tree).
- [ ] Rebuild `~/qemu/build-anyfs-mingw64`, then the mingw anyfs build.

### Task D5: import gate

**Files:** create `scripts/check_win_imports.sh`

`objdump -p <file> | awk '/DLL Name/{dll=$3} /^\t+[0-9a-f]+ +[0-9]+ +[A-Za-z_]/{print dll, $NF}'`,
fail on the ANSI list from the spec unless the `dll symbol` pair is in the script's `ALLOW` table
(each with a reason). Run on `build-anyfs-mingw64/{anyfs-ksmbd,anyfs-nfsd,src/lspart/anyfs-lspart}.exe`.

### Task D6: wine verification + commit

- [ ] `tests/wine/u8-cli.sh` passes; `check_win_imports.sh` passes; `test_u8.exe` passes.
- [ ] win64 addon probe (`~/.cache/anyfs-scratch/ewine/f9/main.js` pattern) opens raw/qcow2/vmdk
  images under `测试 café/` and lists partitions; snapshot open with `TEMP` set to a non-ASCII dir.
- [ ] Linux: unit suite green (core changed).
- [ ] **Commit + push D** (one commit per task D1–D5 is fine).

---

## Part B — byte-preserving transport

### Task B1: `src/core/anyfs_name.{c,h}` escape/unescape

**Files:** create `src/core/anyfs_name.{c,h}`, `tests/unit/test_name_escape.c`; modify `meson.build` (core sources, wasm core sources in `scripts/build_anyfs_wasm.sh` if it lists core files explicitly)

```c
/* Returns output length (no NUL) or -1 if cap is too small. */
int anyfs_name_escape(const char* in, char* out, size_t cap);
int anyfs_name_unescape(const char* in, char* out, size_t cap);
#define ANYFS_NAME_ESCAPE_MAX(n) (3 * (n) + 1)
```

`escape`: decode strictly (lead `C2–DF` + 1, `E0 A0–BF`, `E1–EC`/`EE–EF` + 2, `ED 80–9F`,
`F0 90–BF`, `F1–F3` + 3, `F4 80–8F`); a valid sequence encoding U+EF80–U+EFFF is treated as
invalid; each invalid byte `b` → `EE, 0xBE + (b >= 0xC0), 0x80 | (b & 0x3F)`.
`unescape`: `EE BE 80–BF` → `0x80–0xBF`, `EE BF 80–BF` → `0xC0–0xFF`, everything else copied.

- [ ] **Step 1:** test: all 1-byte, 2-byte, 3-byte inputs (no NUL) round-trip and escape to strict
  UTF-8 (checked by an independent validator in the test); 10⁶ random strings; vectors
  `C0 AF`, `ED A0 80`, `F4 90 80 80`, `EE BE 80` (U+EF80 → 9 bytes), `中文` unchanged; overflow
  returns -1.
- [ ] **Steps 2–4:** FAIL → implement → PASS.

### Task B2: escape in the glue

**Files:** modify `ts/native/anyfs_ts.c`, `tests/test_session_names.c`

- Out: readdir `name`, `session_list_json` `label`/`fstype`/`uuid`/`ptype`, `meta_json`
  `pt_type`, `realpath`, `readlink` results (escape into a temp buffer, copy to the caller's
  buffer; `realpath`/`readlink` return `-LKL_ENAMETOOLONG` if it does not fit).
- In: a helper `static const char* unesc(const char* in, char* tmp, size_t cap)` used by readdir,
  lstat, stat, realpath, readlink, open, read_kernel_file (tmp of `ANYFS_LKL_PATH_MAX * 4`).
- [ ] **Step 1:** extend `test_session_names.c`: p2 (ext4) names exactly the escaped forms of the
  five files, and each one stats/opens/reads back its content. FAIL today (U+FFFD, ENOENT).
- [ ] **Steps 2–3:** implement → PASS. Unit suite green.

### Task B3: bundles, robustness, commit

- [ ] Rebuild the Node wasm bundle and the addon; `tests/test_wasm_exports.sh`.
- [ ] `node ts/tests/robustness/run.mjs --backend wasm` and `--backend native`: no fatal/hang/crash;
  note class changes of the four non-UTF-8 syzbot cases in `ts/tests/robustness/FINDINGS.md`.
- [ ] Web E2E project green (except the known external `@network` CORS case).
- [ ] **Commit + push B.**

---

## Part C — display, settings, CLI option

### Task C1: `@anyfs/core` `names.ts`

**Files:** create `ts/packages/core/src/names.ts`, `ts/packages/core/test/names.test.mjs`; modify `ts/packages/core/src/index.ts`

```ts
export type LegacyEncoding = 'gb18030' | 'big5' | 'shift_jis' | 'euc-kr' | 'windows-1252' | 'off';
export const MOUNT_FAT_CP_437 = 0, MOUNT_FAT_CP_936 = 1 << 8, MOUNT_FAT_CP_950 = 2 << 8,
    MOUNT_FAT_CP_932 = 3 << 8, MOUNT_FAT_CP_949 = 4 << 8;
export function hasEscapedBytes(name: string): boolean;          // /[\uEF80-\uEFFF]/
export function nameToBytes(name: string): Uint8Array;
export function displayName(name: string, enc: LegacyEncoding): string;
export function defaultLegacyEncoding(lang: string): Exclude<LegacyEncoding, 'off'>;
export function fatCodepageFlag(enc: LegacyEncoding): number;
```

`displayName` fallback: walk the string; escaped char → `\x` + 2 uppercase hex digits, others
unchanged. Tests: GBK `中文.txt` escaped → `中文.txt` with gb18030; `caf\xE9.txt` with
windows-1252 → `café.txt`; `off` → `\xD6\xD0\xCE\xC4.txt`; a lone `\xFF` with shift_jis →
`\xFF`; language table cases; flags; `nameToBytes` against the C vectors.

### Task C2: `@anyfs/trees` `formatName`

**Files:** modify `ts/packages/trees/src/AnyfsFileBrowser.tsx`

Prop `formatName?: (name: string) => string` (default identity). Rows: `name: fmt(e.name)`,
`ext: splitExt(fmt(e.name))`, sort by `fmt(a.name).localeCompare(fmt(b.name))`; crumbs: `name:
fmt(p)`; properties: `name: fmt(...)`, path line shows `relPath.split('/').map(fmt).join('/')`,
link target `fmt`-mapped per segment. Add `formatName` to the readdir effect and `folderChain`
dependency lists.

### Task C3: vite-demo

**Files:** modify `ts/examples/vite-demo/src/Settings.tsx`, `src/components/DiskView.tsx`, `src/components/DownloadingFileTree.tsx`

- `Settings.legacyEncoding: 'auto' | LegacyEncoding` (default `'auto'`), a `<select>` in the
  dialog (Auto (…resolved…), GB18030, Big5, Shift_JIS, EUC-KR, Windows-1252, Off) with the note
  "FAT short names follow this setting the next time a disk is opened."
- helper `resolveLegacyEncoding(s)` → `s === 'auto' ? defaultLegacyEncoding(navigator.language) : s`.
- `DiskView`: `session.enter(selectedPart, fatCodepageFlag(enc))`; label `displayName(p.label, enc)`.
- `DownloadingFileTree`: `formatName={(n) => displayName(n, enc)}`; `fileName =
  displayName(lastSegment, enc)`.

### Task C4: E2E

**Files:** modify `ts/tests/e2e/fixtures/manifest.ts`, `ts/tests/e2e/fixtures/generate.mjs`; create `ts/tests/e2e/flows/unicode-names.spec.ts`

Fixture `unicodeNames` = `images/names.img`, built by running `tests/make_names_image.py`
from `generate.mjs`. `flows/unicode-names.spec.ts` (all three projects): set the legacy encoding to
`gb18030` through the settings dialog, open the image, enter p1 → rows include `中文.txt`,
`café.txt` and `中文.TXT`; enter p2 → rows include `中文.txt` (decoded from GBK) and `plain.txt`;
download `中文.txt` from p2 and compare the bytes with `ext4-1\n`.

### Task C5: CLI `--legacy-encoding`

**Files:** modify `src/core/anyfs_name.{c,h}`, `src/core/anyfs_format.c`, `src/lspart/lspart_main.c`, `src/ksmbd/ksmbd_main.c`, `src/nfsd/nfsd_main.c`, `src/fuse/fuse_main.c`, `tests/unit/test_name_escape.c`, docs (`docs/lkl-servers.md`, `docs/anyfs-fuse.md`)

```c
int anyfs_legacy_encoding_parse(const char* s);   /* ANYFS_LEGACY_* or -1 */
int anyfs_legacy_encoding_auto(void);            /* GetACP() / LC_ALL,LC_CTYPE,LANG */
uint32_t anyfs_legacy_fat_flag(int enc);
void anyfs_legacy_set(int enc);                  /* process-wide, for labels */
int anyfs_legacy_decode(const char* in, char* out, size_t cap); /* label for display */
```

`anyfs_legacy_decode`: valid UTF-8 → copy; else Windows `MultiByteToWideChar(cp)` +
`WideCharToMultiByte(CP_UTF8)`, elsewhere `iconv` (not in wasm); failure → `\xNN` form.
`anyfs_format.c` prints labels through it. Each CLI parses `--legacy-encoding`, calls
`anyfs_legacy_set`, and ORs `anyfs_legacy_fat_flag` into its enter flags (servers:
`enter_flags` to `anyfs_server_resolve_shares` / `anyfs_share_open_disks`; fuse: its
`anyfs_session_enter[_path]` calls). Tests: parse/auto (with `LANG=zh_CN.UTF-8`, `ja_JP`,
`C`), fat flag, decode of GBK bytes under gb18030.

- [ ] **Commit + push C.** Update the spec status to "implemented" with amendments.

---

## Self-review

- Spec coverage: A (A1–A4), B (B1–B3), C (C1–C5), D (D1–D6), known issues unchanged. Import gate
  stays local (CI wiring needs approval). Robustness re-run in B3.
- Types: `anyfs_mount_opts(fstype, rdonly, fat_cp, buf, cap)` everywhere; flags names
  `ANYFS_MOUNT_FAT_CP_*` (C) / `MOUNT_FAT_CP_*` (TS) consistent.

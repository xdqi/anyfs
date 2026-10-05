# LKL on macOS via ELF-to-dylib Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Run the standard ELF LKL kernel on macOS (arm64 and x86_64) by converting
the linked kernel image into a Mach-O dylib at build time, plus a Darwin host library
and a smoke test.

**Architecture:** The kernel keeps its Linux build (Kbuild, `vmlinux.lds`, `objcopy -G`).
`lkl.o` plus a small ELF glue object is linked with `ld.lld -shared` into an image whose
only relocations are `R_*_RELATIVE`. `elf2dylib.py` turns that image into assembly
(`.incbin` per segment, `.quad` per relocation, `.set` per symbol) and lets `ld64.lld`
write the dylib, then checks the result byte for byte. A Mach-O shim in the host
library gives the dylib's `lklk_*` exports their usual `lkl_*` names and keeps
variadic calls off the ELF/Mach-O boundary.

**Tech Stack:** Python 3 (stdlib only), bash, clang-19 / ld.lld-19 / ld64.lld-19 /
llvm-objdump-19 / llvm-objcopy-19 / llvm-nm-19 / llvm-lipo-19, aarch64-linux-gnu-gcc,
zig 0.17 (`/opt/zig/zig`, for Darwin headers and `libSystem.tbd`).

**Spec:** `docs/superpowers/specs/2026-10-05-lkl-macos-elf2dylib-design.md`

**Executed 2026-10-05.** All tasks are done and verified on an Apple Silicon and an Intel
Mac. Reviews changed several files after the code below was written (elf2dylib's padding
and checks, the glue formatter, the build scripts' fail-closed handling, the x86_64 10.12
target, `--unique` in the kernel link), so the code blocks here are the original intent:
the committed files and the spec's "Implementation notes" are authoritative.

---

## Ground rules for every task

- **Shared working tree.** Another Claude session (`anyfs-reader-92`) works in this
  checkout. Stage only the exact paths a step names, never `git add -A` or `git add .`.
  Leave `.github/workflows/mingw64.yml` and any file you did not create or modify alone.
- **Never edit `~/linux` by hand.** Only `scripts/oot_fs.sh` writes there. Edit
  `~/oot-fs/zfs` only through `oot_fs.sh` or `git -C ~/oot-fs/zfs checkout`.
- **Do not push** until Task 10.
- Commit messages are English and end with
  `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.

## File structure

| Path | Status | Responsibility |
|---|---|---|
| `scripts/macho/elf2dylib.py` | create | Generic ELF shared object → Mach-O dylib converter, with input and output checks |
| `scripts/macho/test_elf2dylib.sh` | create | Synthetic-library tests for the converter, positive and negative |
| `scripts/macho/lkl_elf_glue.c` | create | ELF side of the ABI boundary: `lkl_printf`, `lkl_bug`, `lkl_glue_set_host`, `lkl_start_kernel_str` |
| `scripts/macho/lklk.h` | create | Prototypes of the 10 symbols `liblkl-kernel.dylib` exports |
| `scripts/macho/lkl_macho_shim.c` | create | Mach-O side: the public `lkl_*` API forwarding to `lklk_*` |
| `scripts/macho/test_elf_glue.c`, `scripts/macho/test_macho_shim.c`, `scripts/macho/test_glue.sh` | create | Native unit tests for both glue files |
| `scripts/macho/build_kernel_dylib.sh` | create | glue + `lkl.o` → `lkl-kernel.so` → `liblkl-kernel.dylib`; `--universal` |
| `scripts/macho/autoconf/lkl_autoconf.h` | move from `.tmp/macho-exp/host/autoconf/` | Darwin host profile |
| `scripts/macho/darwin-netdev-stubs.c` | move from `.tmp/macho-exp/host/` | Stubs for the Linux-only netdev backends |
| `scripts/macho/build_host_lib.sh` | create | Darwin `liblkl-host.a` with zig cc |
| `scripts/macho/smoke/lkl_macos_smoke.c`, `scripts/macho/smoke/README.md`, `scripts/macho/build_smoke.sh` | create | macOS smoke test and its bundle |
| `scripts/oot_fs.sh` | modify | Drop the Mach-O-only ZFS gates, add the ZFS arm64 gates, trim `--macho` wording |
| `scripts/build_lkl.sh` | modify | `KCFLAGS` for `linux-arm64` |
| `patches/linux/macho/` | trim | Keep `08-posix-host-darwin.patch`, `09-endian-darwin.patch` and a two-line `series` |
| `docs/macos-macho-feasibility.md` | modify | Superseded banner |

Outputs go to `build/macos/` (ignored by `build*/` in `.gitignore`). LKL build trees
`lkl-linux-*/` ignore themselves (Kbuild writes a `*` `.gitignore` into them).

---

### Task 0: Archive the object-port experiment in a local tag

**Files:** none in the working tree. Creates commit + tag `exp/macho-object-port`.

- [ ] **Step 1: Record the current state**

```bash
cd /home/kosaka/anyfs-reader
git rev-parse HEAD > /tmp/macho-tag-head.txt
git status --short > /tmp/macho-tag-status-before.txt
cat /tmp/macho-tag-status-before.txt
```

Expected: includes ` M scripts/oot_fs.sh`, `?? docs/macos-macho-feasibility.md`,
`?? patches/linux/macho/`, `?? scripts/macho/`.

- [ ] **Step 2: Build the archive commit with a temporary index**

```bash
cd /home/kosaka/anyfs-reader
export GIT_INDEX_FILE=/tmp/macho-tag.index
rm -f "$GIT_INDEX_FILE"
git read-tree HEAD
git add -f -- patches/linux/macho scripts/macho scripts/oot_fs.sh \
    docs/macos-macho-feasibility.md \
    .tmp/macho-exp/sweep.py .tmp/macho-exp/macho-support.c .tmp/macho-exp/probe.c \
    .tmp/macho-exp/shim-none \
    .tmp/macho-exp/host/darwin-netdev-stubs.c \
    .tmp/macho-exp/host/autoconf/lkl_autoconf.h \
    .tmp/macho-exp/ok-none.txt .tmp/macho-exp/ok-port.txt \
    .tmp/macho-exp/failed-none.txt .tmp/macho-exp/failed-port.txt \
    .tmp/macho-exp/objs.txt .tmp/macho-exp/objs-nozfs.txt .tmp/macho-exp/objs-port.txt
tree=$(git write-tree)
commit=$(git commit-tree "$tree" -p HEAD -m "exp: Mach-O object-archive port of LKL (archived)

The 2026-07-31 experiment that compiled every kernel object straight to
Mach-O: shim headers, patches/linux/macho 01-09, the sched_class order
file and check script, the oot_fs.sh --macho gates, the feasibility write-up
and the .tmp/macho-exp harness sources. Superseded by the ELF-to-dylib
design (docs/superpowers/specs/2026-10-05-lkl-macos-elf2dylib-design.md).
Local reference only; not on main and never pushed.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>")
unset GIT_INDEX_FILE
rm -f /tmp/macho-tag.index
git tag -a exp/macho-object-port "$commit" -m "Archived Mach-O object-archive port (local only)"
echo "$commit"
```

- [ ] **Step 3: Verify the tag and that nothing else moved**

```bash
cd /home/kosaka/anyfs-reader
git show --stat --format=%s exp/macho-object-port | head -5
git show --name-only --format= exp/macho-object-port | grep -cE '^scripts/macho/shim/|^patches/linux/macho/0[1-9]'
git show exp/macho-object-port:.tmp/macho-exp/sweep.py | head -3
[[ $(git rev-parse HEAD) == $(cat /tmp/macho-tag-head.txt) ]] && echo "HEAD unchanged"
git status --short | diff - /tmp/macho-tag-status-before.txt && echo "status unchanged"
git diff --cached --name-only | wc -l
```

Expected: subject `exp: Mach-O object-archive port of LKL (archived)`; the count is
31 (22 shim files + 9 patches); `sweep.py`'s first lines print; `HEAD unchanged`;
`status unchanged`; `0` staged files.

---

### Task 1: Retire the superseded experiment parts

**Files:**
- Delete: `scripts/macho/shim/`, `scripts/macho/sched_class.order`,
  `scripts/macho/check_sched_class.sh`, `patches/linux/macho/0[1-7]-*.patch`
- Modify: `patches/linux/macho/series`, `scripts/oot_fs.sh`, `docs/macos-macho-feasibility.md`
- Restore in `~/oot-fs/zfs`: `module/lua/setjmp/setjmp_aarch64.S`, `module/lua/ldo.c`

- [ ] **Step 1: Delete the superseded files**

```bash
cd /home/kosaka/anyfs-reader
rm -rf scripts/macho/shim
rm -f scripts/macho/sched_class.order scripts/macho/check_sched_class.sh
rm -f patches/linux/macho/0[1-7]-*.patch
printf '08-posix-host-darwin.patch\n09-endian-darwin.patch\n' > patches/linux/macho/series
ls patches/linux/macho scripts/macho
```

Expected: `08-posix-host-darwin.patch  09-endian-darwin.patch  series`, and an empty
`scripts/macho`.

- [ ] **Step 2: Remove gates 4b-2 and 4b-4 from `scripts/oot_fs.sh`**

In `stage_zfs`, delete everything from the line
`    # 4b-2. lua/setjmp/setjmp_aarch64.S defines its own ENTRY()/END() macros`
through the `    fi` that closes the block ending in
`        log "patched ZFS ldo.c setjmp/longjmp declarations for Mach-O"`, plus the blank
line after it. The next line is then `    # 4c. ICP C sources reference x86_64 ASM symbols ...`.

Then rename the remaining gate so the numbering has no hole: replace
`    # 4b-3. Same for the ARM/aarch64 SIMD-stat block, and for the two sha2 ICP`
with
`    # 4b-2. Same for the ARM/aarch64 SIMD-stat block, and for the two sha2 ICP`.

Check:

```bash
cd /home/kosaka/anyfs-reader
grep -nE 'zfs_lua|setjmp_aarch64|ldo\.c|4b-3|4b-4' scripts/oot_fs.sh
```

Expected: no output.

- [ ] **Step 3: Make `zfs_rewrite_line` immune to backslashes**

`awk -v` interprets backslash escapes, and Task 3 passes a line that ends in `\`. In
`scripts/oot_fs.sh`, replace

```bash
    # Called as an `if` condition, where set -e is off: fail explicitly.
    awk -v from="$from" -v to="$to" '$0 == from { $0 = to } { print }' \
        "$f" > "$f.anyfs.tmp" && mv "$f.anyfs.tmp" "$f" \
        || die "stage_zfs: rewrite of '$from' failed in $f"
```

with

```bash
    # Called as an `if` condition, where set -e is off: fail explicitly.
    # ENVIRON, not awk -v, which would interpret backslashes in the lines.
    ZFS_FROM="$from" ZFS_TO="$to" \
        awk '$0 == ENVIRON["ZFS_FROM"] { $0 = ENVIRON["ZFS_TO"] } { print }' \
        "$f" > "$f.anyfs.tmp" && mv "$f.anyfs.tmp" "$f" \
        || die "stage_zfs: rewrite of '$from' failed in $f"
```

- [ ] **Step 4: Update the `--macho` wording in the header of `scripts/oot_fs.sh`**

Replace these four lines (keep the line count, `--help` prints lines 2–18):

```
#                                     $LINUX_DIR. The macho patches are no-ops
#                                     for the elf/pe/wasm builds: kernel parts
#                                     sit under __MACH__, host-lib parts under
#                                     __APPLE__ or Darwin-only header values.
```

with

```
#                                     $LINUX_DIR. The macho patches touch only
#                                     the LKL host library (macOS port), under
#                                     __APPLE__ or Darwin-only header values:
#                                     no-ops for the elf/pe/wasm builds.
```

Check: `bash -n scripts/oot_fs.sh && bash scripts/oot_fs.sh --help | tail -3`.
Expected: the last two lines are the `unstage` and `status` entries.

- [ ] **Step 5: Restore the two ZFS files the removed gates had rewritten**

```bash
git -C ~/oot-fs/zfs checkout -- module/lua/setjmp/setjmp_aarch64.S module/lua/ldo.c
git -C ~/oot-fs/zfs status --short
```

Expected: no `module/lua/` entries. The SIMD-gate files (`simd.h`, `simd_stat.c`,
`sha256_impl.c`, `sha512_impl.c`, `isa_defs.h`, `asm_linkage.h`) stay modified.

- [ ] **Step 6: Prove the remaining gates still apply and leave lua alone**

```bash
cd /home/kosaka/anyfs-reader
T=/tmp/oottest; rm -rf $T; mkdir -p $T/oot/zfs $T/linux/fs $T/linux/include
touch $T/linux/fs/Kconfig $T/linux/fs/Makefile
git -C ~/oot-fs/zfs archive HEAD | tar -x -C $T/oot/zfs
cp ~/oot-fs/zfs/zfs_config.h $T/oot/zfs/
cp ~/oot-fs/zfs/include/zfs_gitrev.h $T/oot/zfs/include/
OOT_DIR=$T/oot LINUX_DIR=$T/linux bash scripts/oot_fs.sh stage 2>&1 | grep -E 'patched|die|stage complete'
for f in module/lua/setjmp/setjmp_aarch64.S module/lua/ldo.c; do
    git -C ~/oot-fs/zfs show HEAD:$f | cmp - $T/oot/zfs/$f && echo "untouched: $f"
done
OOT_DIR=$T/oot LINUX_DIR=$T/linux bash scripts/oot_fs.sh stage 2>&1 | grep -c patched || true
rm -rf $T
```

Expected: the first run logs the simd, simd_stat, sha256, sha512, isa_defs and
asm_linkage patches and `stage complete`; both lua files are `untouched`; the second run
counts `0` patched lines.

- [ ] **Step 7: Mark the feasibility write-up as superseded**

Insert after the first line (`# Native macOS LKL: Mach-O feasibility experiment`) of
`docs/macos-macho-feasibility.md`:

```markdown

> **Superseded (2026-10-05).** This experiment compiled the kernel straight to Mach-O
> objects and rebuilt the linker script's guarantees by hand. The macOS port now
> converts the standard ELF kernel instead: see
> `docs/superpowers/specs/2026-10-05-lkl-macos-elf2dylib-design.md`. The shims,
> kernel patches 01–07, order file and harness described below are archived in the
> local tag `exp/macho-object-port` (`git show --stat exp/macho-object-port`).
> The traps recorded here still apply to any code compiled for Darwin.
```

- [ ] **Step 8: Commit the write-up**

```bash
cd /home/kosaka/anyfs-reader
git add -- docs/macos-macho-feasibility.md
git commit -m "docs: add the Mach-O object-port feasibility write-up, marked superseded

The experiment it describes is archived in the local tag
exp/macho-object-port; the replacement design is the ELF-to-dylib spec.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>" -- docs/macos-macho-feasibility.md
git show --stat HEAD | tail -2
```

Expected: `1 file changed`.

---

### Task 2: elf2dylib.py and its tests

**Files:**
- Create: `scripts/macho/test_elf2dylib.sh`, `scripts/macho/elf2dylib.py`

- [ ] **Step 1: Write the test script**

Create `scripts/macho/test_elf2dylib.sh`:

```bash
#!/bin/bash
# Tests for elf2dylib.py. A small synthetic shared library is built for arm64
# and x86_64 the way build_kernel_dylib.sh links the kernel; it must convert,
# and the result must pass the independent checks below. Inputs outside the
# accepted shape must be rejected without leaving an output file.
#
# Usage: scripts/macho/test_elf2dylib.sh
# Needs clang, ld.lld, llvm-nm, llvm-objdump, readelf, python3 and zig's
# libSystem.tbd (found next to $ZIG, default /opt/zig/zig).
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
CLANG="${CLANG:-$(command -v clang-19 || command -v clang)}"
LD="${LD:-$(command -v ld.lld-19 || command -v ld.lld)}"
NM="${NM:-$(command -v llvm-nm-19 || command -v llvm-nm)}"
OBJDUMP="${OBJDUMP:-$(command -v llvm-objdump-19 || command -v llvm-objdump)}"
ZIG="${ZIG:-$(command -v zig || echo /opt/zig/zig)}"
LIBSYSTEM="${LIBSYSTEM:-$(dirname "$(readlink -f "$ZIG")")/lib/libc/darwin/libSystem.tbd}"
[[ -f $LIBSYSTEM ]] || { echo "libSystem.tbd not found at $LIBSYSTEM" >&2; exit 1; }

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
failures=0
pass() { echo "ok   $*"; }
fail() { echo "FAIL $*"; failures=$((failures + 1)); }

# The kernel's link flags (build_kernel_dylib.sh); a test may append overrides.
LDFLAGS=(-shared -Bsymbolic -z now -z max-page-size=16384
         -z separate-loadable-segments --no-undefined)

# build ARCH OUT.so [LD-OVERRIDES...] -- SOURCES...
build() {
    local arch="$1" out="$2" target src
    shift 2
    local ldx=() objs=()
    while [[ $1 != -- ]]; do ldx+=("$1"); shift; done
    shift
    local cflags=(-O1 -fPIC -ffreestanding -fno-builtin -fno-stack-protector)
    case "$arch" in
        arm64)  target=aarch64-linux-gnu; cflags+=(-ffixed-x18) ;;
        x86_64) target=x86_64-linux-gnu ;;
    esac
    for src in "$@"; do
        "$CLANG" --target="$target" "${cflags[@]}" -c "$src" -o "$src.$arch.o"
        objs+=("$src.$arch.o")
    done
    "$LD" "${LDFLAGS[@]}" "${ldx[@]}" -o "$out" "${objs[@]}"
}

# convert ARCH IN.so OUT.dylib
convert() {
    python3 "$HERE/elf2dylib.py" --arch "$1" --install-name @rpath/libtest.dylib \
        --libsystem "$LIBSYSTEM" --export exported_add=_lklk_add \
        --export exported_call=_lklk_call -o "$3" "$2"
}

# reject NAME ARCH IN.so
reject() {
    local out="$tmp/$1.$2.dylib"
    if convert "$2" "$3" "$out" > "$tmp/log" 2>&1; then
        fail "$2: $1 was accepted"
    elif [[ -e $out ]]; then
        fail "$2: $1 was rejected but left $out"
    else
        pass "$2: rejects $1: $(grep -m1 'elf2dylib:' "$tmp/log")"
    fi
}

cat > "$tmp/lib_a.c" <<'EOF'
/* Pointers from data into code, rodata, data and bss, plus exports. */
typedef int (*fn_t)(int);
__attribute__((noinline)) static int helper(int x) { return x + 1; }
int other_helper(int x);
int exported_add(int a) { return helper(a) + 41; }
static const char *const words[] = { "alpha", "beta" };
fn_t table[] = { helper, exported_add, other_helper };
int counter;
int *counter_ptr = &counter;
const char *const *words_ptr = words;
int exported_call(int i)
{
	return table[i % 3](i) + *counter_ptr + words_ptr[i & 1][0];
}
EOF
cat > "$tmp/lib_b.c" <<'EOF'
/* A second static function named helper. */
__attribute__((noinline, used)) static int helper(int x) { return x * 2; }
int other_helper(int x) { return helper(x); }
EOF
cat > "$tmp/import.c" <<'EOF'
int ext_fn(int);
int exported_add(int a) { return ext_fn(a); }
int exported_call(int i) { return i; }
EOF
cat > "$tmp/textrel.c" <<'EOF'
int counter;
int exported_add(int a) { return a; }
int exported_call(int i) { return i; }
__asm__(".text\n.p2align 3\n.globl textref\ntextref:\n.quad counter\n");
EOF
cat > "$tmp/x18.c" <<'EOF'
int exported_add(int a)
{
	int r;

	__asm__ volatile("mov %w0, w18" : "=r"(r));
	return a + r;
}
int exported_call(int i) { return i; }
EOF

for arch in arm64 x86_64; do
    so="$tmp/ok.$arch.so" out="$tmp/ok.$arch.dylib"
    build "$arch" "$so" -- "$tmp/lib_a.c" "$tmp/lib_b.c"
    if ! convert "$arch" "$so" "$out" > "$tmp/log" 2>&1; then
        fail "$arch: valid input rejected"; cat "$tmp/log"; continue
    fi
    pass "$arch: converts: $(cat "$tmp/log")"
    rel=$(readelf -rW "$so" | grep -c '_RELATIVE' || true)
    reb=$("$OBJDUMP" --macho --rebase "$out" | grep -c ' pointer$' || true)
    if [[ $rel -gt 0 && $rel -eq $reb ]]; then pass "$arch: $reb rebases for $rel RELATIVE relocations"
    else fail "$arch: $reb rebases for $rel RELATIVE relocations"; fi
    exports=$("$OBJDUMP" --macho --exports-trie "$out" | awk '/^0x/ { print $2 }' | LC_ALL=C sort | tr '\n' ' ')
    if [[ $exports == "_lklk_add _lklk_call " ]]; then pass "$arch: exports $exports"
    else fail "$arch: exports are '$exports'"; fi
    locals=$("$NM" -m "$out" | awk '$3 == "non-external" { print $4 }' | LC_ALL=C sort | tr '\n' ' ')
    if [[ $locals == *"_helper _helper~2 "* ]]; then pass "$arch: both static helpers kept, renamed apart"
    else fail "$arch: local symbols are '$locals'"; fi
    if [[ $("$NM" -u "$out") == dyld_stub_binder ]]; then pass "$arch: only dyld_stub_binder undefined"
    else fail "$arch: undefined symbols: $("$NM" -u "$out" | tr '\n' ' ')"; fi

    build "$arch" "$tmp/import.$arch.so" -z undefs -- "$tmp/import.c"
    reject import "$arch" "$tmp/import.$arch.so"
    build "$arch" "$tmp/textrel.$arch.so" -z notext -- "$tmp/textrel.c"
    reject textrel "$arch" "$tmp/textrel.$arch.so"
    build "$arch" "$tmp/page4k.$arch.so" -z max-page-size=4096 -- "$tmp/lib_a.c" "$tmp/lib_b.c"
    reject page4k "$arch" "$tmp/page4k.$arch.so"
done
build arm64 "$tmp/x18.arm64.so" -- "$tmp/x18.c"
reject x18 arm64 "$tmp/x18.arm64.so"

if [[ $failures -eq 0 ]]; then echo "PASS test_elf2dylib"; else echo "FAILED: $failures"; exit 1; fi
```

```bash
chmod +x scripts/macho/test_elf2dylib.sh
```

- [ ] **Step 2: Run it and watch it fail**

Run: `scripts/macho/test_elf2dylib.sh`
Expected: `FAIL arm64: valid input rejected` with `can't open file ... elf2dylib.py`,
the same for x86_64, every `reject` line passing for the wrong reason, then `FAILED: 2`.

- [ ] **Step 3: Write the converter**

Create `scripts/macho/elf2dylib.py`:

```python
#!/usr/bin/env python3
"""Convert a linked ELF shared object into a Mach-O dylib.

The input must be what `ld.lld -shared -Bsymbolic -z now -z max-page-size=16384
-z separate-loadable-segments --no-undefined` makes of position-independent
code: four PT_LOAD segments (R, RX, RW with PT_GNU_RELRO, RW) on 16 KiB
boundaries, and only R_*_RELATIVE dynamic relocations. Anything else is
rejected.

This tool does not write Mach-O itself. It generates assembly that .incbin's
each segment into a 16 KiB-aligned section, turns every RELATIVE slot into
`.quad <segment label> + offset` (which ld64.lld emits as a rebase), defines
symbols with .set, and links that with ld64.lld. Each segment lands at its
ELF address + DELTA, so PC-relative code inside the image stays valid. The
result is then compared with the ELF byte for byte and discarded on any
mismatch.

Design: docs/superpowers/specs/2026-10-05-lkl-macos-elf2dylib-design.md
"""
import argparse
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile

DELTA = 0x4000  # one page for the Mach-O header and load commands
PAGE = 0x4000   # arm64 macOS page size, used for x86_64 too

PT_LOAD, PT_DYNAMIC, PT_GNU_RELRO = 1, 2, 0x6474E552
PF_X, PF_W, PF_R = 1, 2, 4
SHT_SYMTAB, SHT_DYNSYM = 2, 11
SHN_UNDEF, SHN_LORESERVE = 0, 0xFF00
STT_OBJECT, STT_FUNC = 1, 2
DT_NULL, DT_NEEDED, DT_RELA, DT_RELASZ = 0, 1, 7, 8
DT_REL, DT_TEXTREL, DT_JMPREL, DT_FLAGS, DT_RELR = 17, 22, 23, 30, 36
DF_TEXTREL = 0x4

# --arch: (e_machine, R_*_RELATIVE type, clang triple prefix)
ARCHES = {"arm64": (183, 1027, "arm64"), "x86_64": (62, 8, "x86_64")}

# One Mach-O section per PT_LOAD, in the order R, RX, RELRO, RW.
SHAPE = [PF_R, PF_R | PF_X, PF_R | PF_W, PF_R | PF_W]
SECTIONS = [
    ("__TEXT", "__lkl_const", ""),
    ("__TEXT", "__lkl_text", ",regular,pure_instructions"),
    ("__DATA_CONST", "__lkl_relro", ""),
    ("__DATA", "__lkl_data", ""),
]
BSS = ("__DATA", "__lkl_bss")  # zero-fill tail of the last segment
LIBSYSTEM = "/usr/lib/libSystem.B.dylib"


class Reject(Exception):
    """Input or output outside what this converter accepts."""


def tool(env, *names):
    if os.environ.get(env):
        return os.environ[env]
    for name in names:
        if shutil.which(name):
            return name
    raise Reject(f"none of {', '.join(names)} is on PATH (or set ${env})")


def run(argv):
    r = subprocess.run(argv, capture_output=True, text=True)
    if r.returncode:
        raise Reject(f"{' '.join(argv[:2])} failed:\n{r.stderr.strip()}")
    return r.stdout


class Elf:
    def __init__(self, path):
        with open(path, "rb") as f:
            self.data = f.read()
        d = self.data
        if d[:4] != b"\x7fELF" or d[4] != 2 or d[5] != 1:
            raise Reject("not a 64-bit little-endian ELF file")
        (self.type, self.machine, _, _, phoff, shoff, _, _, phentsize, phnum,
         shentsize, shnum, _) = struct.unpack_from("<HHIQQQIHHHHHH", d, 16)
        # (p_type, p_flags, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align)
        self.phdrs = [struct.unpack_from("<IIQQQQQQ", d, phoff + i * phentsize)
                      for i in range(phnum)]
        # (sh_name, sh_type, sh_flags, sh_addr, sh_offset, sh_size, sh_link, ...)
        self.shdrs = [struct.unpack_from("<IIQQQQIIQQ", d, shoff + i * shentsize)
                      for i in range(shnum)]

    def symbols(self, sh_type):
        """(name, st_info, st_shndx, st_value) of every entry in the table."""
        out = []
        for sh in self.shdrs:
            if sh[1] != sh_type:
                continue
            strtab = self.shdrs[sh[6]][4]
            for off in range(sh[4], sh[4] + sh[5], 24):
                name, info, _, shndx, value, _ = struct.unpack_from("<IBBHQQ", self.data, off)
                end = self.data.index(b"\0", strtab + name)
                out.append((self.data[strtab + name:end].decode(), info, shndx, value))
        return out

    def dynamic(self):
        """{d_tag: [d_val, ...]} from PT_DYNAMIC."""
        tags = {}
        for p in self.phdrs:
            if p[0] == PT_DYNAMIC:
                for off in range(p[2], p[2] + p[5], 16):
                    tag, val = struct.unpack_from("<qQ", self.data, off)
                    if tag == DT_NULL:
                        break
                    tags.setdefault(tag, []).append(val)
        return tags


class Image:
    """The checked input: segments, RELATIVE slots and symbols."""

    def __init__(self, path, arch, exports, objdump):
        machine, rel_type, _ = ARCHES[arch]
        elf = self.elf = Elf(path)
        if elf.type != 3 or elf.machine != machine:
            raise Reject(f"expected ET_DYN for {arch} (e_machine {machine}), "
                         f"got e_type {elf.type}, e_machine {elf.machine}")
        self.loads = [p for p in elf.phdrs if p[0] == PT_LOAD]
        flags = [p[1] for p in self.loads]
        if flags != SHAPE:
            raise Reject(f"PT_LOAD flags are {flags}, expected {SHAPE} (R, RX, RW, RW): "
                         "link with -z now -z separate-loadable-segments")
        for p in self.loads:
            if p[3] % PAGE or p[2] != p[3]:
                raise Reject(f"PT_LOAD at {p[3]:#x} is not on a 16 KiB boundary with "
                             "p_offset == p_vaddr: link with -z max-page-size=16384 "
                             "-z separate-loadable-segments")
        if self.loads[0][3] != 0:
            raise Reject("the first PT_LOAD must start at address 0")
        for p in self.loads[:2]:
            if p[6] != p[5]:
                raise Reject(f"read-only PT_LOAD at {p[3]:#x} has a zero-fill tail")
        relro = [p for p in elf.phdrs if p[0] == PT_GNU_RELRO]
        if len(relro) != 1 or relro[0][3] != self.loads[2][3]:
            raise Reject("expected one PT_GNU_RELRO covering the third PT_LOAD")

        dyn = elf.dynamic()
        for tag, what in ((DT_NEEDED, "DT_NEEDED (it depends on a shared library)"),
                          (DT_TEXTREL, "DT_TEXTREL (relocations in read-only code)"),
                          (DT_JMPREL, "PLT relocations (it imports functions)"),
                          (DT_REL, "DT_REL relocations"),
                          (DT_RELR, "DT_RELR relocations")):
            if tag in dyn:
                raise Reject(f"input has {what}")
        if dyn.get(DT_FLAGS, [0])[0] & DF_TEXTREL:
            raise Reject("input has DF_TEXTREL (relocations in read-only code)")

        self.slots = {}  # r_offset -> r_addend
        if DT_RELA in dyn:
            start, size = dyn[DT_RELA][0], dyn[DT_RELASZ][0]
            for off in range(start, start + size, 24):
                where, info, addend = struct.unpack_from("<QQq", elf.data, off)
                if info != rel_type:
                    raise Reject(f"relocation at {where:#x} has r_info {info:#x}; "
                                 f"only R_*_RELATIVE ({rel_type}) is accepted")
                if not any(p[3] <= where and where + 8 <= p[3] + p[5] for p in self.loads[2:]):
                    raise Reject(f"relocation at {where:#x} is outside the writable "
                                 "segments' file data")
                self.label(addend)  # rejects addends outside every segment
                self.slots[where] = addend

        self.dynsyms = {}
        for name, _, shndx, value in elf.symbols(SHT_DYNSYM):
            if not name:
                continue
            if shndx == SHN_UNDEF:
                raise Reject(f"undefined dynamic symbol {name}: the image must import nothing")
            self.dynsyms[name] = value
        for name in exports:
            if name not in self.dynsyms:
                raise Reject(f"--export {name}: no such defined dynamic symbol")

        self.locals = []
        for name, info, shndx, value in elf.symbols(SHT_SYMTAB):
            if ((info & 0xF) in (STT_OBJECT, STT_FUNC) and SHN_UNDEF < shndx < SHN_LORESERVE
                    and name and not name.startswith("$")):
                self.label(value)
                self.locals.append((name, value))

        if arch == "arm64":
            hits = [line for line in run([objdump, "-d", "--no-show-raw-insn", path]).splitlines()
                    if re.search(r"\b[xw]18\b", line)]
            if hits:
                raise Reject(f"{len(hits)} instructions use x18/w18, which Darwin reserves "
                             "(build with -ffixed-x18), e.g.:\n" + "\n".join(hits[:5]))

    def label(self, addr):
        """Assembler expression for an image address, relative to a segment label."""
        for i, p in enumerate(self.loads):
            vaddr, filesz, memsz = p[3], p[5], p[6]
            if vaddr <= addr <= vaddr + memsz:
                if i == 3 and memsz > filesz and addr >= vaddr + filesz:
                    return f"Lbss + {addr - vaddr - filesz:#x}"
                return f"Lseg{i} + {addr - vaddr:#x}"
        raise Reject(f"address {addr:#x} lies outside every segment")


def write_asm(img, exports, path, workdir):
    lines = []
    for i, p in enumerate(img.loads):
        seg, sect, attrs = SECTIONS[i]
        vaddr, filesz, memsz = p[3], p[5], p[6]
        blob = os.path.join(workdir, f"seg{i}.bin")
        with open(blob, "wb") as f:
            f.write(img.elf.data[p[2]:p[2] + filesz])
        lines += [f"\t.section {seg},{sect}{attrs}", "\t.p2align 14", f"Lseg{i}:"]
        pos = 0
        for where in sorted(w for w in img.slots if vaddr <= w < vaddr + filesz):
            off = where - vaddr
            if off < pos:
                raise Reject(f"relocations at {where - 8:#x}..{where:#x} overlap")
            if off > pos:
                lines.append(f'\t.incbin "{blob}", {pos}, {off - pos}')
            lines.append(f"\t.quad {img.label(img.slots[where])}")
            pos = off + 8
        if filesz > pos:
            lines.append(f'\t.incbin "{blob}", {pos}, {filesz - pos}')
        if memsz > filesz:
            if i == 3:
                lines.append(f"\t.zerofill {BSS[0]},{BSS[1]},Lbss,{memsz - filesz},0")
            else:
                lines.append(f"\t.space {memsz - filesz}")
    used = set(exports.values())
    for name, value in img.locals:
        sym, n = f"_{name}", 1
        while sym in used:
            n += 1
            sym = f"_{name}~{n}"
        used.add(sym)
        lines.append(f'\t.set "{sym}", {img.label(value)}')
    for elfname, sym in exports.items():
        lines += [f'\t.globl "{sym}"', f'\t.set "{sym}", {img.label(img.dynsyms[elfname])}']
    with open(path, "w") as f:
        f.write("\n".join(lines) + "\n")


def link(asm, out, arch, min_os, install_name, libsystem, clang, ld64):
    obj = asm[:-2] + ".o"
    run([clang, "-target", f"{ARCHES[arch][2]}-apple-macos{min_os}", "-c", asm, "-o", obj])
    run([ld64, "-arch", arch, "-platform_version", "macos", min_os, min_os, "-dylib",
         "-install_name", install_name, "-no_fixup_chains", "-adhoc_codesign",
         "-o", out, obj, libsystem])


def check_output(img, exports, out, install_name, workdir, objdump, objcopy, nm):
    sections = {}
    for line in run([objdump, "--macho", "--section-headers", out]).splitlines():
        f = line.split()
        if len(f) >= 4 and f[0].isdigit():
            sections[f[1]] = (int(f[3], 16), int(f[2], 16))

    for i, p in enumerate(img.loads):
        seg, sect, _ = SECTIONS[i]
        vaddr, filesz, memsz = p[3], p[5], p[6]
        size = filesz if i == 3 else memsz
        if sections.get(sect) != (vaddr + DELTA, size):
            raise Reject(f"{seg},{sect} is at {sections.get(sect)}, expected "
                         f"({vaddr + DELTA:#x}, {size:#x}): ld64.lld laid it out differently")
        dump = os.path.join(workdir, f"out{i}.bin")
        run([objcopy, f"--dump-section={seg},{sect}={dump}", out, os.devnull])
        with open(dump, "rb") as f:
            got = f.read()
        want = bytearray(img.elf.data[p[2]:p[2] + filesz]) + bytes(size - filesz)
        for where, addend in img.slots.items():
            if vaddr <= where < vaddr + filesz:
                struct.pack_into("<Q", want, where - vaddr, addend + DELTA)
        if got != want:
            diff = next((k for k in range(min(len(got), len(want))) if got[k] != want[k]),
                        min(len(got), len(want)))
            raise Reject(f"{seg},{sect} differs from ELF segment {i} at offset {diff:#x}")

    p = img.loads[3]
    if p[6] > p[5]:
        want = (p[3] + p[5] + DELTA, p[6] - p[5])
        if sections.get(BSS[1]) != want:
            raise Reject(f"{BSS[0]},{BSS[1]} is at {sections.get(BSS[1])}, expected {want}")

    rebases = set()
    for line in run([objdump, "--macho", "--rebase", out]).splitlines():
        f = line.split()
        if len(f) == 4 and f[3] == "pointer":
            rebases.add(int(f[2], 16))
    if rebases != {w + DELTA for w in img.slots}:
        raise Reject(f"rebase table has {len(rebases)} entries, expected the "
                     f"{len(img.slots)} RELATIVE slots + {DELTA:#x}")

    got = {}
    for line in run([objdump, "--macho", "--exports-trie", out]).splitlines():
        f = line.split()
        if len(f) == 2 and f[0].startswith("0x"):
            got[f[1]] = int(f[0], 16)
    want = {sym: img.dynsyms[e] + DELTA for e, sym in exports.items()}
    if got != want:
        raise Reject(f"exports are {sorted(got)}, expected {sorted(want)} "
                     f"at their ELF addresses + {DELTA:#x}")

    deps = [line.split(" (")[0].strip()
            for line in run([objdump, "--macho", "--dylibs-used", out]).splitlines()[1:]]
    deps = [d for d in deps if d != install_name]
    if deps != [LIBSYSTEM]:
        raise Reject(f"dependent dylibs are {deps}, expected [{LIBSYSTEM}]")
    undefined = run([nm, "-u", out]).split()
    if undefined != ["dyld_stub_binder"]:
        raise Reject(f"undefined symbols are {undefined}, expected only dyld_stub_binder")


def main():
    ap = argparse.ArgumentParser(description="Convert a linked ELF shared object into a Mach-O dylib.")
    ap.add_argument("--arch", required=True, choices=sorted(ARCHES))
    ap.add_argument("--export", action="append", default=[], metavar="ELF=MACHO",
                    help="export ELF symbol ELF as Mach-O symbol MACHO, e.g. lkl_init=_lklk_init")
    ap.add_argument("--install-name", required=True)
    ap.add_argument("--min-os", default="11.0")
    ap.add_argument("--libsystem", required=True, help="libSystem.tbd to link against")
    ap.add_argument("-o", dest="output", required=True)
    ap.add_argument("input")
    a = ap.parse_args()
    if os.path.exists(a.output):
        os.unlink(a.output)
    try:
        exports = {}
        for e in a.export:
            if "=" not in e:
                raise Reject(f"--export {e}: expected ELF=MACHO")
            elfname, sym = e.split("=", 1)
            exports[elfname] = sym
        objdump = tool("OBJDUMP", "llvm-objdump-19", "llvm-objdump")
        objcopy = tool("OBJCOPY", "llvm-objcopy-19", "llvm-objcopy")
        nm = tool("NM", "llvm-nm-19", "llvm-nm")
        clang = tool("CLANG", "clang-19", "clang")
        ld64 = tool("LD64", "ld64.lld-19", "ld64.lld")
        img = Image(a.input, a.arch, exports, objdump)
        with tempfile.TemporaryDirectory() as tmp:
            asm = os.path.join(tmp, "image.S")
            out = os.path.join(tmp, "image.dylib")
            write_asm(img, exports, asm, tmp)
            link(asm, out, a.arch, a.min_os, a.install_name, a.libsystem, clang, ld64)
            check_output(img, exports, out, a.install_name, tmp, objdump, objcopy, nm)
            shutil.move(out, a.output)
    except Reject as e:
        print(f"elf2dylib: {a.input}: {e}", file=sys.stderr)
        return 1
    print(f"elf2dylib: {a.output}: {len(img.slots)} rebases, {len(exports)} exports, "
          f"{len(img.locals)} local symbols")
    return 0


if __name__ == "__main__":
    sys.exit(main())
```

```bash
chmod +x scripts/macho/elf2dylib.py
```

- [ ] **Step 4: Run the tests and watch them pass**

Run: `scripts/macho/test_elf2dylib.sh`
Expected, for both arches: `converts`, `N rebases for N RELATIVE relocations`,
`exports _lklk_add _lklk_call`, `both static helpers kept, renamed apart`,
`only dyld_stub_binder undefined`. Then `rejects import: ... PLT relocations`,
`rejects textrel: ... TEXTREL`, `rejects page4k: ... 16 KiB boundary`,
`arm64: rejects x18: ... instructions use x18/w18`, and `PASS test_elf2dylib`.

If a positive check fails, read the converter's message in the log: every check names
the offending section, address or symbol.

- [ ] **Step 5: Run it on the probe kernel from the design review**

The arm64 probe from 2026-10-05 still exists if `/tmp` was not cleared:

```bash
[[ -f /tmp/arm64probe/k.so ]] && python3 scripts/macho/elf2dylib.py --arch arm64 \
    --install-name @rpath/liblkl-kernel.dylib --libsystem /opt/zig/lib/libc/darwin/libSystem.tbd \
    --export lkl_syscall=_lklk_syscall -o /tmp/arm64probe/probe.dylib /tmp/arm64probe/k.so
```

Expected: `74925 rebases, 1 exports, ...` (or nothing printed if the probe is gone; Task 6
covers the real kernels).

- [ ] **Step 6: Commit**

```bash
cd /home/kosaka/anyfs-reader
git add -- scripts/macho/elf2dylib.py scripts/macho/test_elf2dylib.sh
git commit -m "feat(macho): add elf2dylib, an ELF shared object to Mach-O dylib converter

Checks that the input is a 16 KiB-aligned R/RX/RELRO/RW image with only
R_*_RELATIVE relocations and no imports (and no x18 use on arm64), emits
.incbin/.quad/.set assembly that ld64.lld links into a dylib, then checks
the dylib byte for byte against the ELF. Tests cover both architectures and
each rejection.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>" -- scripts/macho/elf2dylib.py scripts/macho/test_elf2dylib.sh
```

---

### Task 3: The arm64 ELF kernel builds completely

**Files:**
- Modify: `scripts/build_lkl.sh` (in `build_one`), `scripts/oot_fs.sh` (new gate 4b-3)
- Commit with them: `patches/linux/macho/{series,08-posix-host-darwin.patch,09-endian-darwin.patch}`

Today the `linux-arm64` target links `lkl.o` with 11 undefined ZFS symbols and 3 GCC
outline-atomics helpers. Its `tests/boot` fails to link while `build_lkl.sh` still
reports success.

- [ ] **Step 1: Baseline the amd64 kernel before touching ZFS**

`lkl-linux-amd64/` is shared with other sessions; this rebuild is incremental and must
end with the same `lkl.o` it starts from.

```bash
cd /home/kosaka/anyfs-reader
bash scripts/build_lkl.sh --targets=linux-amd64 > /tmp/amd64-before.log 2>&1; echo "exit=$?"
sha256sum lkl-linux-amd64/tools/lkl/lib/lkl.o | tee /tmp/amd64-before.sha
```

Expected: `exit=0` and a hash.

- [ ] **Step 2: Add the arm64 flags to `scripts/build_lkl.sh`**

In `build_one`, after the `cc_arg` lines, add:

```bash
    # linux-arm64 is also the kernel for macOS on Apple Silicon, converted by
    # scripts/macho/build_kernel_dylib.sh. Darwin reserves x18 and may clear
    # it at any time, and GCC's outline atomics need getauxval(), which macOS
    # lacks. Both flags are harmless on Linux.
    local kcflags_arg=()
    [[ "$NAME" == linux-arm64 ]] && kcflags_arg=(KCFLAGS="-ffixed-x18 -mno-outline-atomics")
```

and change the build invocation to:

```bash
    OUTPUT="$OUT" make -C "$LINUX_DIR/tools/lkl" -j"$JOBS" \
         ARCH=lkl "${cross_arg[@]}" "${cc_arg[@]}" "${kcflags_arg[@]}"
```

- [ ] **Step 3: Add ZFS gate 4b-3 to `scripts/oot_fs.sh`**

Insert immediately before the line `    # 4c. ICP C sources reference x86_64 ASM symbols (aes_x86_64_impl,`:

```bash
    # 4b-3. The rest of ZFS's arm64 SIMD: fletcher-4, RAID-Z, BLAKE3 and the
    #       sha2 armv7/NEON/armv8 code list aarch64 implementations whose
    #       objects module/Kbuild builds only for CONFIG_ARM64, which
    #       ARCH=lkl never sets, so an arm64 LKL link ends with 11 undefined
    #       symbols (fletcher_4_aarch64_neon_ops,
    #       vdev_raidz_aarch64_neon{,x2}_impl, zfs_blake3_*_sse{2,41},
    #       zfs_sha{256,512}_block_armv7). Drop those blocks on CONFIG_LKL, as
    #       4a-2 does for the SIMD header. Each rewrite keeps the line count,
    #       so __LINE__ and the x86 objects do not change. Idempotent, and dies
    #       if an anchor line is gone (zfs_rewrite_line).
    local f
    for f in "$src/module/zcommon/zfs_fletcher.c" "$src/module/zfs/vdev_raidz_math.c"; do
        if [[ -f "$f" ]] && zfs_rewrite_line "$f" \
                '#if defined(__aarch64__) && !defined(__FreeBSD__)' \
                '#if defined(__aarch64__) && !defined(__FreeBSD__) && !defined(CONFIG_LKL)'; then
            log "patched ZFS ${f##*/} to skip aarch64 SIMD on CONFIG_LKL builds"
        fi
    done
    local blake3="$src/module/icp/algs/blake3/blake3_impl.c"
    if [[ -f "$blake3" ]] && zfs_rewrite_line "$blake3" \
            '#if defined(__aarch64__) || \' \
            '#if (defined(__aarch64__) && !defined(CONFIG_LKL)) || \'; then
        log "patched ZFS blake3_impl.c to skip aarch64 SIMD on CONFIG_LKL builds"
    fi
    for f in "$sha256impl" "$sha512impl"; do
        [[ -f "$f" ]] || continue
        if zfs_rewrite_line "$f" \
                '#elif defined(__aarch64__) || defined(__arm__)' \
                '#elif (defined(__aarch64__) || defined(__arm__)) && !defined(CONFIG_LKL)'; then
            log "patched ZFS ${f##*/} to skip ARM implementations on CONFIG_LKL builds"
        fi
        if zfs_rewrite_line "$f" \
                '#if defined(__aarch64__) || defined(__arm__)' \
                '#if (defined(__aarch64__) || defined(__arm__)) && !defined(CONFIG_LKL)'; then
            log "patched ZFS ${f##*/} implementation table for CONFIG_LKL builds"
        fi
    done

```

Check the syntax: `bash -n scripts/oot_fs.sh`.

- [ ] **Step 4: Stage it and confirm what changed in `~/oot-fs/zfs`**

```bash
cd /home/kosaka/anyfs-reader
bash scripts/oot_fs.sh stage 2>&1 | grep -E 'patched|stage complete'
git -C ~/oot-fs/zfs diff --stat -- module/zcommon/zfs_fletcher.c module/zfs/vdev_raidz_math.c \
    module/icp/algs/blake3/blake3_impl.c module/icp/algs/sha2/sha256_impl.c module/icp/algs/sha2/sha512_impl.c
bash scripts/oot_fs.sh stage 2>&1 | grep -c patched || true
```

Expected: the first run logs zfs_fletcher.c, vdev_raidz_math.c, blake3_impl.c and two
lines each for sha256_impl.c and sha512_impl.c. The diff stat shows only
`N insertions(+), N deletions(-)` per file. The second run counts `0`.

- [ ] **Step 5: The amd64 kernel is unchanged**

```bash
cd /home/kosaka/anyfs-reader
bash scripts/build_lkl.sh --targets=linux-amd64 > /tmp/amd64-after.log 2>&1; echo "exit=$?"
sha256sum -c /tmp/amd64-before.sha
```

Expected: `exit=0` and `lkl-linux-amd64/tools/lkl/lib/lkl.o: OK`. If it differs, run
`llvm-objdump-19 -d` on both builds of the changed ZFS objects to find the difference
before going on: an x86 change means a rewrite touched a non-aarch64 condition.

- [ ] **Step 6: Build the arm64 kernel**

```bash
cd /home/kosaka/anyfs-reader
bash scripts/gen_lkl_config.sh --targets=linux-arm64 > /tmp/arm64-gen.log 2>&1; echo "gen exit=$?"
bash scripts/build_lkl.sh --targets=linux-arm64 > /tmp/arm64-build.log 2>&1; echo "build exit=$?"
grep -c -- '-ffixed-x18 -mno-outline-atomics' lkl-linux-arm64/init/.main.o.cmd
llvm-nm-19 -u lkl-linux-arm64/tools/lkl/lib/lkl.o
ls -la lkl-linux-arm64/tools/lkl/tests/boot lkl-linux-arm64/tools/lkl/tests/disk
grep -c 'undefined reference' /tmp/arm64-build.log || true
```

Expected: both exits `0`; the flag count is `1`; `llvm-nm -u` prints exactly
`lkl_bug` and `lkl_printf`; `tests/boot` and `tests/disk` exist; `0` undefined references.

- [ ] **Step 7: Commit**

`scripts/oot_fs.sh` carries the whole `--macho` mechanism, so commit the two host
patches it applies together with it.

```bash
cd /home/kosaka/anyfs-reader
git add -- scripts/build_lkl.sh scripts/oot_fs.sh patches/linux/macho/series \
    patches/linux/macho/08-posix-host-darwin.patch patches/linux/macho/09-endian-darwin.patch
git commit -m "build(lkl): make the linux-arm64 kernel link, ready for macOS

- build_lkl.sh: linux-arm64 builds with -ffixed-x18 -mno-outline-atomics
  (Darwin reserves x18; outline atomics need getauxval)
- oot_fs.sh: gate the remaining ZFS aarch64 SIMD tables on CONFIG_LKL
  (11 undefined symbols, tests/boot failed to link); generalize the
  wasm-only patch series into --wasm/--macho flavors; fail when a ZFS
  anchor line is missing
- patches/linux/macho: the two Darwin host-library patches (posix-host.c,
  endian.h), applied by oot_fs.sh stage --macho

The linux-amd64 lkl.o is byte-identical before and after.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>" -- scripts/build_lkl.sh scripts/oot_fs.sh \
    patches/linux/macho/series patches/linux/macho/08-posix-host-darwin.patch \
    patches/linux/macho/09-endian-darwin.patch
```

---

### Task 4: Validate the arm64 ELF kernel on Linux arm64

**Files:** none.

- [ ] **Step 1: Get a user-mode emulator (needs the user)**

`qemu-aarch64` is not installed on the dev box, and installing it needs sudo. Ask the user
to run `sudo apt-get install -y qemu-user`, or to approve running the test on a GitHub
`ubuntu-24.04-arm` runner instead. Do not install packages yourself.

- [ ] **Step 2: Run LKL's boot test**

```bash
cd /home/kosaka/anyfs-reader
qemu-aarch64 -L /usr/aarch64-linux-gnu lkl-linux-arm64/tools/lkl/tests/boot > /tmp/arm64-boot.log 2>&1; echo "exit=$?"
grep -cE '^ok ' /tmp/arm64-boot.log; grep -E '^not ok' /tmp/arm64-boot.log || echo "no failures"
```

Expected: `exit=0`, a positive `ok` count, `no failures`. A `# SKIP` line is fine.

- [ ] **Step 3: Record the result**

Note the date, the ok/skip counts and the emulator version (`qemu-aarch64 --version`)
for the spec's implementation notes (Task 9).

---

### Task 5: The ABI glue on both sides, with native unit tests

**Files:**
- Create: `scripts/macho/lkl_elf_glue.c`, `scripts/macho/lklk.h`,
  `scripts/macho/lkl_macho_shim.c`, `scripts/macho/test_elf_glue.c`,
  `scripts/macho/test_macho_shim.c`, `scripts/macho/test_glue.sh`

- [ ] **Step 1: Write the ELF glue test**

Create `scripts/macho/test_elf_glue.c`:

```c
/* Native unit test for lkl_elf_glue.c: formatter, print/panic routing and
 * the non-variadic start entry point. Built by test_glue.sh. */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

void lkl_glue_set_host(void (*print)(const char *, int), void (*panic)(void));
int lkl_start_kernel_str(const char *cmdline);
int lkl_printf(const char *fmt, ...);
void lkl_bug(const char *fmt, ...);

static char printed[1024];
static int printed_len, panics;
static const char *start_fmt, *start_arg;
static int failures;

#define EXPECT(cond)							\
	do {								\
		if (!(cond)) {						\
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			failures++;					\
		}							\
	} while (0)

static void fake_print(const char *s, int len)
{
	memcpy(printed + printed_len, s, len);
	printed_len += len;
	printed[printed_len] = '\0';
}

static void fake_panic(void)
{
	panics++;
}

/* Stands in for the kernel's lkl_start_kernel(). */
int lkl_start_kernel(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	start_fmt = fmt;
	start_arg = va_arg(ap, const char *);
	va_end(ap);
	return 42;
}

int main(void)
{
	char big[2000];
	int n;

	lkl_glue_set_host(fake_print, fake_panic);

	printed_len = 0;
	n = lkl_printf("%s: unbalanced put\n", "lkl_cpu_put");
	EXPECT(strcmp(printed, "lkl_cpu_put: unbalanced put\n") == 0);
	EXPECT(n == (int)strlen(printed));

	printed_len = 0;
	lkl_printf("d=%d i=%i u=%u x=%x ld=%ld lu=%lu lx=%lx p=%p %% end", -5, 7,
		   3000000000u, 0xbeef, -1234567890123L, 9876543210UL,
		   0xdeadbeefcafeUL, (void *)0x1000);
	EXPECT(strcmp(printed, "d=-5 i=7 u=3000000000 x=beef ld=-1234567890123 "
		   "lu=9876543210 lx=deadbeefcafe p=0x1000 % end") == 0);

	printed_len = 0;
	lkl_printf("%s|%s|%q|", (char *)0, "");
	EXPECT(strcmp(printed, "(null)||%q|") == 0);

	memset(big, 'a', sizeof(big) - 1);
	big[sizeof(big) - 1] = '\0';
	printed_len = 0;
	n = lkl_printf("%s", big);
	EXPECT(n == 511 && printed_len == 511);	/* truncated, not overrun */

	printed_len = 0;
	panics = 0;
	lkl_bug("bad count while changing owner\n");
	EXPECT(strcmp(printed, "bad count while changing owner\n") == 0);
	EXPECT(panics == 1);

	EXPECT(lkl_start_kernel_str("mem=64M loglevel=4") == 42);
	EXPECT(start_fmt && strcmp(start_fmt, "%s") == 0);
	EXPECT(start_arg && strcmp(start_arg, "mem=64M loglevel=4") == 0);

	printf(failures ? "FAILED test_elf_glue\n" : "PASS test_elf_glue\n");
	return failures != 0;
}
```

- [ ] **Step 2: Write the shim interface header and the shim test**

Create `scripts/macho/lklk.h`:

```c
/*
 * What liblkl-kernel.dylib exports: the kernel's non-variadic entry points
 * and lkl_elf_glue.c's two helpers, renamed lkl_X -> lklk_X by
 * build_kernel_dylib.sh. Only lkl_macho_shim.c calls these.
 */
#ifndef LKLK_H
#define LKLK_H

struct lkl_host_operations;

int lklk_init(struct lkl_host_operations *ops);
void lklk_cleanup(void);
long lklk_syscall(long no, long *params);
long lklk_sys_halt(void);
int lklk_is_running(void);
int lklk_get_free_irq(const char *user);
void lklk_put_irq(int irq, const char *name);
int lklk_trigger_irq(int irq);
void lklk_glue_set_host(void (*print)(const char *str, int len), void (*panic)(void));
int lklk_start_kernel_str(const char *cmdline);

#endif /* LKLK_H */
```

Create `scripts/macho/test_macho_shim.c`:

```c
/* Native unit test for lkl_macho_shim.c against fake lklk_* entry points.
 * Built by test_glue.sh. */
#include <stdio.h>
#include <string.h>

#include <lkl_host.h>

#include "lklk.h"

static char trace[256];
static struct lkl_host_operations *init_ops;
static void (*got_print)(const char *, int);
static void (*got_panic)(void);
static char started[8192];
static int start_calls, put_irq_seen = -1, failures;

#define EXPECT(cond)							\
	do {								\
		if (!(cond)) {						\
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			failures++;					\
		}							\
	} while (0)

static void note(const char *what)
{
	strncat(trace, what, sizeof(trace) - strlen(trace) - 1);
}

void lklk_glue_set_host(void (*print)(const char *, int), void (*panic)(void))
{
	note("set_host ");
	got_print = print;
	got_panic = panic;
}

int lklk_init(struct lkl_host_operations *ops)
{
	note("init ");
	init_ops = ops;
	return 7;
}

int lklk_start_kernel_str(const char *cmdline)
{
	start_calls++;
	snprintf(started, sizeof(started), "%s", cmdline);
	return 3;
}

void lklk_cleanup(void) { note("cleanup "); }
long lklk_syscall(long no, long *params) { return no * 100 + params[0]; }
long lklk_sys_halt(void) { return 11; }
int lklk_is_running(void) { return 1; }
int lklk_get_free_irq(const char *user) { return (int)strlen(user); }
void lklk_put_irq(int irq, const char *name) { (void)name; put_irq_seen = irq; }
int lklk_trigger_irq(int irq) { return irq + 1; }

static void fake_print(const char *s, int len) { (void)s; (void)len; }
static void fake_panic(void) { }

int main(void)
{
	struct lkl_host_operations ops = { .print = fake_print, .panic = fake_panic };
	long params[6] = { 5 };
	char big[5000];

	EXPECT(lkl_init(&ops) == 7);
	EXPECT(strcmp(trace, "set_host init ") == 0);	/* glue first */
	EXPECT(init_ops == &ops && got_print == fake_print && got_panic == fake_panic);

	EXPECT(lkl_start_kernel("mem=%dM %s", 64, "loglevel=4") == 3);
	EXPECT(strcmp(started, "mem=64M loglevel=4") == 0);

	memset(big, 'a', sizeof(big) - 1);
	big[sizeof(big) - 1] = '\0';
	start_calls = 0;
	EXPECT(lkl_start_kernel("%s", big) == -LKL_E2BIG);
	EXPECT(start_calls == 0);
	big[4095] = '\0';				/* 4095 + NUL still fits */
	EXPECT(lkl_start_kernel("%s", big) == 3 && start_calls == 1);

	EXPECT(lkl_syscall(2, params) == 205);
	EXPECT(lkl_sys_halt() == 11);
	EXPECT(lkl_is_running() == 1);
	EXPECT(lkl_get_free_irq("virtio") == 6);
	lkl_put_irq(9, "virtio");
	EXPECT(put_irq_seen == 9);
	EXPECT(lkl_trigger_irq(4) == 5);
	lkl_cleanup();
	EXPECT(strstr(trace, "cleanup") != NULL);

	printf(failures ? "FAILED test_macho_shim\n" : "PASS test_macho_shim\n");
	return failures != 0;
}
```

Create `scripts/macho/test_glue.sh`:

```bash
#!/bin/bash
# Native (Linux) unit tests for the two sides of the ELF/Mach-O boundary:
# lkl_elf_glue.c and lkl_macho_shim.c. Neither is Darwin-specific C, so both
# build with the host gcc.
#
# Usage: scripts/macho/test_glue.sh [LKL_OUT]
#   LKL_OUT  LKL build tree with the generated tools/lkl/include headers the
#            shim needs (default: <repo>/lkl-linux-amd64)
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$HERE/../.." && pwd)"
# shellcheck source=../lib/config.sh
source "$REPO_DIR/scripts/lib/config.sh"
LINUX_DIR="${LINUX_DIR:-$ANYFS_PATHS_LINUX_SRC}"
lkl_out="${1:-$REPO_DIR/lkl-linux-amd64}"

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

gcc -O2 -Wall -Wextra -o "$tmp/elf_glue" "$HERE/test_elf_glue.c" "$HERE/lkl_elf_glue.c"
"$tmp/elf_glue"
gcc -O2 -Wall -Wextra -I"$LINUX_DIR/tools/lkl/include" -I"$lkl_out/tools/lkl/include" \
    -o "$tmp/macho_shim" "$HERE/test_macho_shim.c" "$HERE/lkl_macho_shim.c"
"$tmp/macho_shim"
```

```bash
chmod +x scripts/macho/test_glue.sh
```

- [ ] **Step 3: Run the tests and watch them fail**

Run: `scripts/macho/test_glue.sh`
Expected: gcc fails with `lkl_elf_glue.c: No such file or directory`.

- [ ] **Step 4: Write the ELF glue**

Create `scripts/macho/lkl_elf_glue.c`:

```c
/*
 * ELF-side glue linked into lkl-kernel.so for the macOS build
 * (docs/superpowers/specs/2026-10-05-lkl-macos-elf2dylib-design.md).
 *
 * The kernel image is ELF code built for the Linux calling convention and
 * converted into liblkl-kernel.dylib by elf2dylib.py. On arm64 the two
 * conventions disagree about variadic functions (Darwin passes variadic
 * arguments on the stack, AAPCS64 in registers), so none may cross the
 * ELF/Mach-O boundary:
 *
 *  - lkl_printf() and lkl_bug(), which the kernel imports, live here and
 *    print through the host's non-variadic print/panic callbacks;
 *  - lkl_start_kernel_str() lets the host start the kernel without making a
 *    variadic call into ELF code.
 *
 * Freestanding: the kernel's own vsnprintf() is hidden by objcopy -G, so this
 * carries a small formatter for what lkl_printf/lkl_bug callers use (%s) and
 * the common integer conversions.
 */
#include <stdarg.h>
#include <stddef.h>

#define GLUE_BUF 512

int lkl_start_kernel(const char *fmt, ...);

static void (*host_print)(const char *str, int len);
static void (*host_panic)(void);

void lkl_glue_set_host(void (*print)(const char *, int), void (*panic)(void))
{
	host_print = print;
	host_panic = panic;
}

int lkl_start_kernel_str(const char *cmdline)
{
	return lkl_start_kernel("%s", cmdline);
}

struct out {
	char *buf;
	int len;
};

static void put(struct out *o, char c)
{
	if (o->len < GLUE_BUF - 1)
		o->buf[o->len++] = c;
}

static void put_str(struct out *o, const char *s)
{
	if (!s)
		s = "(null)";
	while (*s)
		put(o, *s++);
}

static void put_num(struct out *o, unsigned long long v, unsigned int base)
{
	char digits[24];
	int n = 0;

	do {
		digits[n++] = "0123456789abcdef"[v % base];
		v /= base;
	} while (v);
	while (n)
		put(o, digits[--n]);
}

static int format(char *buf, const char *fmt, va_list ap)
{
	struct out o = { buf, 0 };

	for (; *fmt; fmt++) {
		int is_long = 0;

		if (*fmt != '%') {
			put(&o, *fmt);
			continue;
		}
		while (*++fmt == 'l')
			is_long = 1;
		switch (*fmt) {
		case 's':
			put_str(&o, va_arg(ap, const char *));
			break;
		case 'd':
		case 'i': {
			long long v = is_long ? va_arg(ap, long) : va_arg(ap, int);

			if (v < 0)
				put(&o, '-');
			put_num(&o, v < 0 ? -(unsigned long long)v : (unsigned long long)v, 10);
			break;
		}
		case 'u':
			put_num(&o, is_long ? va_arg(ap, unsigned long) : va_arg(ap, unsigned int), 10);
			break;
		case 'x':
			put_num(&o, is_long ? va_arg(ap, unsigned long) : va_arg(ap, unsigned int), 16);
			break;
		case 'p':
			put_str(&o, "0x");
			put_num(&o, (unsigned long)va_arg(ap, void *), 16);
			break;
		case '%':
			put(&o, '%');
			break;
		case '\0':		/* lone trailing '%': print it and stop */
			put(&o, '%');
			fmt--;
			break;
		default:		/* unsupported conversion: print it verbatim */
			put(&o, '%');
			put(&o, *fmt);
			break;
		}
	}
	buf[o.len] = '\0';
	return o.len;
}

static int emit(const char *fmt, va_list ap)
{
	char buf[GLUE_BUF];
	int n = format(buf, fmt, ap);

	if (host_print)
		host_print(buf, n);
	return n;
}

int lkl_printf(const char *fmt, ...)
{
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = emit(fmt, ap);
	va_end(ap);
	return n;
}

void lkl_bug(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	emit(fmt, ap);
	va_end(ap);
	if (host_panic)
		host_panic();
}
```

- [ ] **Step 5: Write the Mach-O shim**

Create `scripts/macho/lkl_macho_shim.c`:

```c
/*
 * The public LKL API for the macOS build. The kernel is liblkl-kernel.dylib,
 * converted from the standard ELF build by scripts/macho/build_kernel_dylib.sh,
 * which exports its entry points as lklk_* (lklk.h). This file gives them
 * their usual lkl_* names, so the LKL host library and anyfs need no changes,
 * and keeps variadic calls on the Mach-O side: on arm64, Darwin passes
 * variadic arguments on the stack while the ELF code expects them in
 * registers (see lkl_elf_glue.c).
 */
#include <stdarg.h>
#include <stdio.h>

#include <lkl_host.h>

#include "lklk.h"

/* COMMAND_LINE_SIZE in arch/lkl/include/asm/setup.h */
#define LKL_CMDLINE_MAX 4096

int lkl_init(struct lkl_host_operations *ops)
{
	lklk_glue_set_host(ops->print, ops->panic);
	return lklk_init(ops);
}

int lkl_start_kernel(const char *fmt, ...)
{
	char cmdline[LKL_CMDLINE_MAX];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(cmdline, sizeof(cmdline), fmt, ap);
	va_end(ap);
	if (n < 0 || n >= (int)sizeof(cmdline))
		return -LKL_E2BIG;
	return lklk_start_kernel_str(cmdline);
}

void lkl_cleanup(void)
{
	lklk_cleanup();
}

long lkl_syscall(long no, long *params)
{
	return lklk_syscall(no, params);
}

long lkl_sys_halt(void)
{
	return lklk_sys_halt();
}

int lkl_is_running(void)
{
	return lklk_is_running();
}

int lkl_get_free_irq(const char *user)
{
	return lklk_get_free_irq(user);
}

void lkl_put_irq(int irq, const char *name)
{
	lklk_put_irq(irq, name);
}

int lkl_trigger_irq(int irq)
{
	return lklk_trigger_irq(irq);
}
```

- [ ] **Step 6: Run the tests and watch them pass**

Run: `scripts/macho/test_glue.sh`
Expected: `PASS test_elf_glue` then `PASS test_macho_shim`, with no compiler warnings.
If gcc reports a conflicting type for an `lkl_*` function, the shim's signature has
drifted from the LKL header: use the header's.

- [ ] **Step 7: Commit**

```bash
cd /home/kosaka/anyfs-reader
git add -- scripts/macho/lkl_elf_glue.c scripts/macho/lklk.h scripts/macho/lkl_macho_shim.c \
    scripts/macho/test_elf_glue.c scripts/macho/test_macho_shim.c scripts/macho/test_glue.sh
git commit -m "feat(macho): add the ELF and Mach-O glue for the kernel dylib boundary

lkl_elf_glue.c implements lkl_printf/lkl_bug inside the ELF image and a
non-variadic lkl_start_kernel_str; lkl_macho_shim.c gives the dylib's
lklk_* exports their lkl_* names and formats lkl_start_kernel's command
line on the Mach-O side. No variadic call crosses the boundary, which
Darwin arm64 and AAPCS64 lay out differently. Native unit tests for both.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>" -- scripts/macho/lkl_elf_glue.c \
    scripts/macho/lklk.h scripts/macho/lkl_macho_shim.c scripts/macho/test_elf_glue.c \
    scripts/macho/test_macho_shim.c scripts/macho/test_glue.sh
```

---

### Task 6: build_kernel_dylib.sh, for both architectures and universal

**Files:**
- Create: `scripts/macho/build_kernel_dylib.sh`
- Modify: `docs/superpowers/specs/2026-10-05-lkl-macos-elf2dylib-design.md` (one sentence)

- [ ] **Step 1: Write the script**

Create `scripts/macho/build_kernel_dylib.sh`:

```bash
#!/bin/bash
# Build liblkl-kernel.dylib for macOS from the standard ELF LKL kernel.
#
# Usage: build_kernel_dylib.sh --arch=arm64|x86_64 [--lkl-out=DIR] [--out=DIR]
#        build_kernel_dylib.sh --universal [--out=DIR]
#
#   --arch       compile lkl_elf_glue.c with the target's ELF compiler, link it
#                with tools/lkl/lib/lkl.o into lkl-kernel.so, and convert that
#                with elf2dylib.py into OUT/<arch>/liblkl-kernel.dylib
#   --lkl-out    LKL build tree (default: <repo>/lkl-linux-arm64 or
#                <repo>/lkl-linux-amd64, as built by build_lkl.sh)
#   --universal  merge OUT/arm64 and OUT/x86_64 into OUT/liblkl-kernel.dylib
#                with llvm-lipo; each slice must stay byte-identical to the
#                per-arch dylib that was checked
#   --out        output root (default: <repo>/build/macos)
#
# Design: docs/superpowers/specs/2026-10-05-lkl-macos-elf2dylib-design.md
set -euo pipefail

REPO_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
HERE="$REPO_DIR/scripts/macho"
LD="${LD:-$(command -v ld.lld-19 || command -v ld.lld)}"
LIPO="${LIPO:-$(command -v llvm-lipo-19 || command -v llvm-lipo)}"
ZIG="${ZIG:-$(command -v zig || echo /opt/zig/zig)}"
LIBSYSTEM="${LIBSYSTEM:-$(dirname "$(readlink -f "$ZIG")")/lib/libc/darwin/libSystem.tbd}"

# The ELF/Mach-O boundary. Darwin arm64 departs from AAPCS64 for variadic
# calls, arguments narrower than 32 bits, stack-passed arguments and some
# by-value aggregates. Every entry point below, and every member of
# struct lkl_host_operations, takes at most 8 int/long/enum/pointer
# arguments, none narrower than 32 bits and nothing by value, and none is
# variadic: lkl_start_kernel, lkl_printf and lkl_bug are deliberately absent
# (see lkl_elf_glue.c). Keep it that way when adding to this list.
EXPORTS=(
    lkl_init lkl_cleanup lkl_syscall lkl_sys_halt lkl_is_running
    lkl_get_free_irq lkl_put_irq lkl_trigger_irq
    lkl_glue_set_host lkl_start_kernel_str
)

die() { echo "build_kernel_dylib: $*" >&2; exit 1; }

arch="" lkl_out="" out="$REPO_DIR/build/macos" universal=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --arch=*)    arch="${1#--arch=}" ;;
        --lkl-out=*) lkl_out="${1#--lkl-out=}" ;;
        --out=*)     out="${1#--out=}" ;;
        --universal) universal=1 ;;
        -h|--help)   awk 'NR==1{next} /^#/{sub(/^# ?/,""); print; next} {exit}' "$0"; exit 0 ;;
        *)           die "unknown argument: $1" ;;
    esac
    shift
done

if [[ $universal -eq 1 ]]; then
    fat="$out/liblkl-kernel.dylib"
    for slice in arm64 x86_64; do
        [[ -f "$out/$slice/liblkl-kernel.dylib" ]] || die "build --arch=$slice first"
    done
    "$LIPO" -create "$out/arm64/liblkl-kernel.dylib" "$out/x86_64/liblkl-kernel.dylib" \
        -output "$fat.tmp"
    for slice in arm64 x86_64; do
        "$LIPO" -thin "$slice" "$fat.tmp" -output "$fat.$slice"
        if ! cmp -s "$fat.$slice" "$out/$slice/liblkl-kernel.dylib"; then
            rm -f "$fat.tmp" "$fat.$slice"
            die "$slice slice differs from $out/$slice/liblkl-kernel.dylib"
        fi
        rm -f "$fat.$slice"
    done
    mv "$fat.tmp" "$fat"
    echo "build_kernel_dylib: $fat (arm64 + x86_64)"
    exit 0
fi

case "$arch" in
    arm64)  cc=aarch64-linux-gnu-gcc target=linux-arm64
            cflags=(-ffixed-x18 -mno-outline-atomics) ;;
    x86_64) cc=gcc target=linux-amd64 cflags=() ;;
    *)      die "--arch=arm64|x86_64 or --universal is required" ;;
esac
lkl_out="${lkl_out:-$REPO_DIR/lkl-$target}"
lkl_o="$lkl_out/tools/lkl/lib/lkl.o"
[[ -f $lkl_o ]] || die "$lkl_o not found: run gen_lkl_config.sh and build_lkl.sh --targets=$target"

dir="$out/$arch"
mkdir -p "$dir"
"$cc" -O2 -Wall -fPIC -ffreestanding -fno-builtin -fno-stack-protector "${cflags[@]}" \
    -c "$HERE/lkl_elf_glue.c" -o "$dir/lkl_elf_glue.o"
"$LD" -shared -Bsymbolic -z now -z max-page-size=16384 -z separate-loadable-segments \
    --no-undefined -soname liblkl-kernel.so -o "$dir/lkl-kernel.so" "$lkl_o" "$dir/lkl_elf_glue.o"

export_args=()
for e in "${EXPORTS[@]}"; do
    export_args+=(--export "$e=_lklk_${e#lkl_}")
done
python3 "$HERE/elf2dylib.py" --arch "$arch" --install-name @rpath/liblkl-kernel.dylib \
    --min-os 11.0 --libsystem "$LIBSYSTEM" "${export_args[@]}" \
    -o "$dir/liblkl-kernel.dylib" "$dir/lkl-kernel.so"
```

```bash
chmod +x scripts/macho/build_kernel_dylib.sh
```

- [ ] **Step 2: Build both architectures**

```bash
cd /home/kosaka/anyfs-reader
scripts/macho/build_kernel_dylib.sh --arch=arm64
scripts/macho/build_kernel_dylib.sh --arch=x86_64
for a in arm64 x86_64; do
    echo "$a: RELATIVE=$(readelf -rW build/macos/$a/lkl-kernel.so | grep -c _RELATIVE)"
done
```

Expected: each run prints `elf2dylib: .../liblkl-kernel.dylib: N rebases, 10 exports, M local
symbols`. N equals that arch's RELATIVE count (about 75k arm64 and 72k x86_64).

- [ ] **Step 3: Check the exports and the signatures**

```bash
cd /home/kosaka/anyfs-reader
for a in arm64 x86_64; do
    llvm-objdump-19 --macho --exports-trie build/macos/$a/liblkl-kernel.dylib | awk '/^0x/ {print $2}' | sort | tr '\n' ' '; echo
    llvm-otool-19 -l build/macos/$a/liblkl-kernel.dylib | grep -c LC_CODE_SIGNATURE
done
```

Expected, for both: `_lklk_cleanup _lklk_get_free_irq _lklk_glue_set_host _lklk_init
_lklk_is_running _lklk_put_irq _lklk_start_kernel_str _lklk_sys_halt _lklk_syscall
_lklk_trigger_irq`, and `1` signature.

- [ ] **Step 4: Build the universal dylib**

```bash
cd /home/kosaka/anyfs-reader
scripts/macho/build_kernel_dylib.sh --universal
llvm-lipo-19 -info build/macos/liblkl-kernel.dylib
```

Expected: `build_kernel_dylib: .../liblkl-kernel.dylib (arm64 + x86_64)` and
`Architectures in the fat file: ... are: x86_64 arm64`.

- [ ] **Step 5: Align the spec with the universal check**

In the spec's "Error handling" section, replace
``- `build_kernel_dylib.sh` also reruns the output checks after `llvm-lipo`, against each
  slice.``
with
``- After `llvm-lipo`, `build_kernel_dylib.sh --universal` extracts each slice and requires
  it to be byte-identical to the per-arch dylib that passed the output checks.``

- [ ] **Step 6: Commit**

```bash
cd /home/kosaka/anyfs-reader
git add -- scripts/macho/build_kernel_dylib.sh docs/superpowers/specs/2026-10-05-lkl-macos-elf2dylib-design.md
git commit -m "feat(macho): build liblkl-kernel.dylib from the ELF kernel

Links lkl.o with the ELF glue into a RELATIVE-only shared object and
converts it with elf2dylib, for arm64 and x86_64; --universal merges the
two and checks each slice against its verified per-arch dylib.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>" -- scripts/macho/build_kernel_dylib.sh \
    docs/superpowers/specs/2026-10-05-lkl-macos-elf2dylib-design.md
```

---

### Task 7: The Darwin host library

**Files:**
- Create: `scripts/macho/autoconf/lkl_autoconf.h`, `scripts/macho/darwin-netdev-stubs.c`,
  `scripts/macho/build_host_lib.sh`

- [ ] **Step 1: Move the Darwin profile and stubs out of the scratch directory**

```bash
cd /home/kosaka/anyfs-reader
mkdir -p scripts/macho/autoconf
cp .tmp/macho-exp/host/autoconf/lkl_autoconf.h scripts/macho/autoconf/lkl_autoconf.h
cp .tmp/macho-exp/host/darwin-netdev-stubs.c scripts/macho/darwin-netdev-stubs.c
```

In `scripts/macho/autoconf/lkl_autoconf.h`, replace the first comment paragraph

```
 * Prepended to the include path so it shadows the reference build's
 * lkl_autoconf.h, which was generated for a Linux host and enables things macOS
 * does not have. In the real port this is what a Darwin branch in
 * tools/lkl/Makefile.autoconf would emit (alongside the existing posix_host /
 * nt64_host / bsd_host profiles).
```

with

```
 * build_host_lib.sh puts this directory first on the include path, so it
 * shadows the lkl_autoconf.h that the Linux build tree generated for a Linux
 * host. It is what a Darwin branch in tools/lkl/Makefile.autoconf would emit,
 * next to the existing posix_host / nt64_host / bsd_host profiles.
```

In `scripts/macho/darwin-netdev-stubs.c`, replace

```
 * In the real port this belongs in tools/lkl/lib/, selected by the Darwin host
 * profile in Makefile.autoconf, next to the existing per-backend files.
 * anyfs builds LKL with CONFIG_NET off, so nothing reaches these.
```

with

```
 * build_host_lib.sh compiles this in place of virtio_net_tap.c and
 * virtio_net_raw.c. anyfs builds LKL with CONFIG_NET off, so nothing reaches
 * these.
```

- [ ] **Step 2: Write the build script**

Create `scripts/macho/build_host_lib.sh`:

```bash
#!/bin/bash
# Build liblkl-host.a, the LKL host library for macOS, on Linux with zig cc.
#
# Usage: build_host_lib.sh --arch=arm64|x86_64 [--lkl-out=DIR] [--out=DIR]
#
#   --lkl-out  LKL build tree whose tools/lkl/include holds the generated lkl/
#              headers (default: <repo>/lkl-linux-arm64 or <repo>/lkl-linux-amd64)
#   --out      output root (default: <repo>/build/macos); writes
#              OUT/<arch>/liblkl-host.a
#
# Needs the Darwin host patches in $LINUX_DIR (scripts/oot_fs.sh stage --macho).
# The kernel is liblkl-kernel.dylib (build_kernel_dylib.sh), reached through
# lkl_macho_shim.c.
set -euo pipefail

REPO_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
HERE="$REPO_DIR/scripts/macho"
# shellcheck source=../lib/config.sh
source "$REPO_DIR/scripts/lib/config.sh"
LINUX_DIR="${LINUX_DIR:-$ANYFS_PATHS_LINUX_SRC}"
OOT_DIR="${OOT_DIR:-$HOME/oot-fs}"
ZIG="${ZIG:-$(command -v zig || echo /opt/zig/zig)}"

# The host sources tools/lkl/lib/Build selects for a POSIX host with
# VIRTIO_NET_FD, minus virtio_net_tap/raw (darwin-netdev-stubs.c), VFIO and
# macvtap (off in the Darwin profile).
SOURCES=(config fs iomem jmp_buf net posix-host utils virtio virtio_blk
         virtio_net virtio_net_fd virtio_net_pipe)

die() { echo "build_host_lib: $*" >&2; exit 1; }

arch="" lkl_out="" out="$REPO_DIR/build/macos"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --arch=*)    arch="${1#--arch=}" ;;
        --lkl-out=*) lkl_out="${1#--lkl-out=}" ;;
        --out=*)     out="${1#--out=}" ;;
        -h|--help)   awk 'NR==1{next} /^#/{sub(/^# ?/,""); print; next} {exit}' "$0"; exit 0 ;;
        *)           die "unknown argument: $1" ;;
    esac
    shift
done

case "$arch" in
    arm64)  zt=aarch64-macos.11.0.0 target=linux-arm64 ;;
    x86_64) zt=x86_64-macos.11.0.0 target=linux-amd64 ;;
    *)      die "--arch=arm64|x86_64 is required" ;;
esac
lkl_out="${lkl_out:-$REPO_DIR/lkl-$target}"
[[ -d "$lkl_out/tools/lkl/include/lkl" ]] || die "no generated headers in $lkl_out: run build_lkl.sh --targets=$target"
for p in 08-posix-host-darwin.patch 09-endian-darwin.patch; do
    grep -qxF "$p" "$OOT_DIR/.applied.macho" 2>/dev/null \
        || die "$p is not applied to $LINUX_DIR: run scripts/oot_fs.sh stage --macho"
done

dir="$out/$arch/host-obj"
mkdir -p "$dir"
cflags=(-target "$zt" -O2 -Wall -D_FILE_OFFSET_BITS=64 -I"$HERE/autoconf"
        -I"$LINUX_DIR/tools/lkl/include" -I"$lkl_out/tools/lkl/include")
objs=()
for s in "${SOURCES[@]}"; do
    "$ZIG" cc "${cflags[@]}" -c "$LINUX_DIR/tools/lkl/lib/$s.c" -o "$dir/$s.o"
    objs+=("$dir/$s.o")
done
for s in darwin-netdev-stubs lkl_macho_shim; do
    "$ZIG" cc "${cflags[@]}" -c "$HERE/$s.c" -o "$dir/$s.o"
    objs+=("$dir/$s.o")
done
rm -f "$out/$arch/liblkl-host.a"
"$ZIG" ar rcs "$out/$arch/liblkl-host.a" "${objs[@]}"
echo "build_host_lib: $out/$arch/liblkl-host.a (${#objs[@]} objects)"
```

```bash
chmod +x scripts/macho/build_host_lib.sh
```

- [ ] **Step 3: Apply the Darwin host patches**

```bash
cd /home/kosaka/anyfs-reader
bash scripts/oot_fs.sh stage --macho 2>&1 | grep -E 'macho|stage complete'
bash scripts/oot_fs.sh status | tail -4
git -C ~/linux status --short tools/lkl/lib
```

Expected: `applied macho patch: 08-posix-host-darwin.patch` and `09-endian-darwin.patch`;
status lists both; `~/linux` shows ` M tools/lkl/lib/endian.h` and
` M tools/lkl/lib/posix-host.c`.

- [ ] **Step 4: Build both architectures**

```bash
cd /home/kosaka/anyfs-reader
scripts/macho/build_host_lib.sh --arch=arm64 2>&1 | tee /tmp/host-arm64.log
scripts/macho/build_host_lib.sh --arch=x86_64 2>&1 | tee /tmp/host-x86_64.log
grep -E 'error|warning' /tmp/host-*.log || echo "no diagnostics"
llvm-nm-19 build/macos/arm64/liblkl-host.a | grep -E ' T _lkl_(init|start_kernel|syscall)$'
llvm-nm-19 -u build/macos/arm64/liblkl-host.a | grep -c '_lklk_'
```

Expected: `14 objects` per arch. The only allowed diagnostic is
`lkl/linux/icmp.h:100:3: warning: declaration does not declare anything`, which comes
from LKL's generated headers. Any other line means a source needs a Darwin fix in patch
08/09; stop and report it. `nm` shows the three `T _lkl_*` definitions, and there are
undefined `_lklk_*` references (one entry per referencing object).

- [ ] **Step 5: Commit**

```bash
cd /home/kosaka/anyfs-reader
git add -- scripts/macho/autoconf/lkl_autoconf.h scripts/macho/darwin-netdev-stubs.c \
    scripts/macho/build_host_lib.sh
git commit -m "feat(macho): build the LKL host library for macOS with zig cc

liblkl-host.a holds tools/lkl/lib's POSIX host sources (with the Darwin
patches applied by oot_fs.sh stage --macho), the Darwin autoconf profile,
stubs for the Linux-only tap/raw netdevs, and lkl_macho_shim.c.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>" -- scripts/macho/autoconf/lkl_autoconf.h \
    scripts/macho/darwin-netdev-stubs.c scripts/macho/build_host_lib.sh
```

---

### Task 8: The macOS smoke test bundle

**Files:**
- Create: `scripts/macho/smoke/lkl_macos_smoke.c`, `scripts/macho/smoke/README.md`,
  `scripts/macho/build_smoke.sh`

- [ ] **Step 1: Write the smoke program**

Create `scripts/macho/smoke/lkl_macos_smoke.c`:

```c
/*
 * macOS smoke test for the converted LKL kernel (liblkl-kernel.dylib) and the
 * Darwin host library (liblkl-host.a). Boots LKL, mounts an ext4 image
 * read-write, writes a file and reads it back, lists the directory, checks
 * that a 100 ms sleep inside the kernel takes about 100 ms, then unmounts and
 * halts. Prints PASS and exits 0 only if every step succeeds.
 *
 * Usage: lkl-macos-smoke <ext4-image>   (the image is modified)
 */
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <lkl.h>
#include <lkl_host.h>

#define CHECK(cond, ...)						\
	do {								\
		if (!(cond)) {						\
			fprintf(stderr, "FAIL: " __VA_ARGS__);		\
			fputc('\n', stderr);				\
			return 1;					\
		}							\
		printf("ok   %s\n", #cond);				\
	} while (0)

static long elapsed_ms(const struct timespec *a, const struct timespec *b)
{
	return (b->tv_sec - a->tv_sec) * 1000 + (b->tv_nsec - a->tv_nsec) / 1000000;
}

static int run(const char *image)
{
	static const char msg[] = "hello from lkl on macos\n";
	struct __lkl__kernel_timespec nap = { 0, 100 * 1000 * 1000 };
	struct lkl_disk disk = { 0 };
	struct lkl_linux_dirent64 *de;
	struct timespec t0, t1;
	char mnt[64], path[96], buf[64];
	struct lkl_dir *dir;
	int disk_id, fd, err = 0, found = 0;
	long ret, ms;

	CHECK(lkl_init(&lkl_host_ops) == 0, "lkl_init");
	disk.fd = open(image, O_RDWR);
	CHECK(disk.fd >= 0, "open %s", image);
	disk_id = lkl_disk_add(&disk);
	CHECK(disk_id >= 0, "lkl_disk_add: %s", lkl_strerror(disk_id));
	ret = lkl_start_kernel("mem=64M loglevel=4");
	CHECK(ret == 0, "lkl_start_kernel: %s", lkl_strerror(ret));

	ret = lkl_mount_dev(disk_id, 0, "ext4", 0, NULL, mnt, sizeof(mnt));
	CHECK(ret == 0, "lkl_mount_dev: %s", lkl_strerror(ret));
	snprintf(path, sizeof(path), "%s/smoke.txt", mnt);

	fd = lkl_sys_open(path, LKL_O_CREAT | LKL_O_RDWR | LKL_O_TRUNC, 0644);
	CHECK(fd >= 0, "open %s: %s", path, lkl_strerror(fd));
	CHECK(lkl_sys_write(fd, msg, sizeof(msg) - 1) == sizeof(msg) - 1, "write");
	CHECK(lkl_sys_fsync(fd) == 0, "fsync");
	CHECK(lkl_sys_lseek(fd, 0, LKL_SEEK_SET) == 0, "lseek");
	memset(buf, 0, sizeof(buf));
	ret = lkl_sys_read(fd, buf, sizeof(buf));
	CHECK(ret == sizeof(msg) - 1 && memcmp(buf, msg, sizeof(msg) - 1) == 0,
	      "read back %ld bytes: '%s'", ret, buf);
	CHECK(lkl_sys_close(fd) == 0, "close");

	dir = lkl_opendir(mnt, &err);
	CHECK(dir != NULL, "opendir %s: %s", mnt, lkl_strerror(err));
	while ((de = lkl_readdir(dir)))
		found |= strcmp(de->d_name, "smoke.txt") == 0;
	lkl_closedir(dir);
	CHECK(found, "smoke.txt not listed in %s", mnt);

	clock_gettime(CLOCK_MONOTONIC, &t0);
	CHECK(lkl_sys_nanosleep(&nap, NULL) == 0, "nanosleep");
	clock_gettime(CLOCK_MONOTONIC, &t1);
	ms = elapsed_ms(&t0, &t1);
	CHECK(ms >= 95 && ms < 1000, "a 100 ms sleep took %ld ms", ms);

	ret = lkl_umount_dev(disk_id, 0, 0, 1000);
	CHECK(ret == 0, "lkl_umount_dev: %s", lkl_strerror(ret));
	lkl_sys_halt();
	lkl_cleanup();
	close(disk.fd);
	return 0;
}

int main(int argc, char **argv)
{
	if (argc != 2) {
		fprintf(stderr, "usage: %s <ext4-image>\n", argv[0]);
		return 2;
	}
	if (run(argv[1]))
		return 1;
	printf("PASS\n");
	return 0;
}
```

- [ ] **Step 2: Write the bundle script**

Create `scripts/macho/build_smoke.sh`:

```bash
#!/bin/bash
# Cross-build the macOS smoke test bundle on Linux.
#
# Usage: build_smoke.sh --arch=arm64|x86_64 [--lkl-out=DIR] [--out=DIR] [--image=FILE]
#
# Needs OUT/<arch>/liblkl-kernel.dylib (build_kernel_dylib.sh) and
# OUT/<arch>/liblkl-host.a (build_host_lib.sh). Writes OUT/<arch>/smoke/ with
# lkl-macos-smoke, liblkl-kernel.dylib (found through @executable_path) and
# smoke-ext4.img, a copy of --image (default: <repo>/tests/images/ext4.img,
# made by tests/setup.sh). Copy that directory to a Mac and follow
# scripts/macho/smoke/README.md.
set -euo pipefail

REPO_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
HERE="$REPO_DIR/scripts/macho"
# shellcheck source=../lib/config.sh
source "$REPO_DIR/scripts/lib/config.sh"
LINUX_DIR="${LINUX_DIR:-$ANYFS_PATHS_LINUX_SRC}"
ZIG="${ZIG:-$(command -v zig || echo /opt/zig/zig)}"

die() { echo "build_smoke: $*" >&2; exit 1; }

arch="" lkl_out="" out="$REPO_DIR/build/macos" image="$REPO_DIR/tests/images/ext4.img"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --arch=*)    arch="${1#--arch=}" ;;
        --lkl-out=*) lkl_out="${1#--lkl-out=}" ;;
        --out=*)     out="${1#--out=}" ;;
        --image=*)   image="${1#--image=}" ;;
        -h|--help)   awk 'NR==1{next} /^#/{sub(/^# ?/,""); print; next} {exit}' "$0"; exit 0 ;;
        *)           die "unknown argument: $1" ;;
    esac
    shift
done

case "$arch" in
    arm64)  zt=aarch64-macos.11.0.0 target=linux-arm64 ;;
    x86_64) zt=x86_64-macos.11.0.0 target=linux-amd64 ;;
    *)      die "--arch=arm64|x86_64 is required" ;;
esac
lkl_out="${lkl_out:-$REPO_DIR/lkl-$target}"
for f in "$out/$arch/liblkl-kernel.dylib" "$out/$arch/liblkl-host.a" "$image"; do
    [[ -f $f ]] || die "$f not found"
done

dir="$out/$arch/smoke"
rm -rf "$dir"
mkdir -p "$dir"
"$ZIG" cc -target "$zt" -O2 -Wall -I"$HERE/autoconf" -I"$LINUX_DIR/tools/lkl/include" \
    -I"$lkl_out/tools/lkl/include" "$HERE/smoke/lkl_macos_smoke.c" \
    "$out/$arch/liblkl-host.a" -L"$out/$arch" -llkl-kernel \
    -Wl,-rpath,@executable_path -o "$dir/lkl-macos-smoke"
cp "$out/$arch/liblkl-kernel.dylib" "$dir/"
cp "$image" "$dir/smoke-ext4.img"
echo "build_smoke: $dir"
```

```bash
chmod +x scripts/macho/build_smoke.sh
```

- [ ] **Step 3: Write the run instructions**

Create `scripts/macho/smoke/README.md`:

````markdown
# macOS smoke test

Checks that the converted kernel (`liblkl-kernel.dylib`) and the Darwin host library
work on a real Mac: boot, mount an ext4 image read-write, write and read back a file,
list the directory, time a 100 ms in-kernel sleep, unmount, halt.

Build on Linux (after `build_kernel_dylib.sh` and `build_host_lib.sh` for the same arch):

    scripts/macho/build_smoke.sh --arch=arm64     # Apple Silicon
    scripts/macho/build_smoke.sh --arch=x86_64    # Intel

Run on the Mac:

    scp -r <linux-host>:anyfs-reader/build/macos/arm64/smoke ~/lkl-smoke   # x86_64 on Intel
    cd ~/lkl-smoke
    codesign -v liblkl-kernel.dylib && echo signature ok
    ./lkl-macos-smoke smoke-ext4.img; echo "exit=$?"

Expected: one `ok` line per check, then `PASS` and `exit=0`. The image is modified, so
copy a fresh bundle for each run. Files fetched through a browser carry a quarantine
flag; clear it with `xattr -dr com.apple.quarantine ~/lkl-smoke`.

If it fails, collect:

- the full output (the kernel log appears above the `FAIL` line; `loglevel=4` in
  `lkl_macos_smoke.c` limits it to warnings and errors);
- `otool -L lkl-macos-smoke` and `otool -L liblkl-kernel.dylib`;
- `dyld_info -fixups liblkl-kernel.dylib | head -50`;
- for a crash, the report under `~/Library/Logs/DiagnosticReports/`.
````

- [ ] **Step 4: Build both bundles and check them statically**

```bash
cd /home/kosaka/anyfs-reader
ls -la tests/images/ext4.img   # made by tests/setup.sh, which needs mkfs.ext4
scripts/macho/build_smoke.sh --arch=arm64
scripts/macho/build_smoke.sh --arch=x86_64
for a in arm64 x86_64; do
    b=build/macos/$a/smoke/lkl-macos-smoke
    file "$b"
    llvm-objdump-19 --macho --dylibs-used "$b" | tail -n +2
    llvm-otool-19 -l "$b" | grep -A2 LC_RPATH | grep path
    llvm-nm-19 -u "$b" | grep -c _lklk_
done
```

If `tests/images/ext4.img` is missing, `mkfs.ext4` is not installed on the dev box
either: ask the user for an image or to install `e2fsprogs`, then pass `--image=FILE`.

Expected for each arch: a Mach-O 64-bit executable of that arch; dependencies
`@rpath/liblkl-kernel.dylib` and `/usr/lib/libSystem.B.dylib`; `path @executable_path`;
`10` `_lklk_` imports. All of them resolve, or zig's link would have failed.

- [ ] **Step 5: Commit**

```bash
cd /home/kosaka/anyfs-reader
git add -- scripts/macho/smoke/lkl_macos_smoke.c scripts/macho/smoke/README.md scripts/macho/build_smoke.sh
git commit -m "test(macho): add the macOS smoke test bundle

lkl-macos-smoke boots the converted kernel, mounts an ext4 image
read-write, round-trips a file, lists the directory and times an
in-kernel sleep. build_smoke.sh cross-builds it with zig cc next to
liblkl-kernel.dylib and an image; README.md has the Mac-side steps.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>" -- scripts/macho/smoke/lkl_macos_smoke.c \
    scripts/macho/smoke/README.md scripts/macho/build_smoke.sh
```

---

### Task 9: Verify on the user's Macs and record the results

**Files:** Modify: `docs/superpowers/specs/2026-10-05-lkl-macos-elf2dylib-design.md`

- [ ] **Step 1: Hand the bundles to the user**

Tell the user, in Chinese, where the two bundles are (`build/macos/arm64/smoke`,
`build/macos/x86_64/smoke`) and point them to `scripts/macho/smoke/README.md`. Ask them to
run it on the Apple Silicon Mac and the Intel Mac and to paste back the output and the
`codesign -v` result. Wait for both.

- [ ] **Step 2: If a run fails**

Use superpowers:systematic-debugging. Use the collected diagnostics to classify the
failure first, before changing anything:

- dyld refuses the image (conversion);
- a crash on the first `lklk_` call (ABI boundary);
- a hang or crash inside the kernel (host layer, e.g. threads or timers).

Fixes go into the relevant task's files, with a regression test where Linux can express
one.

- [ ] **Step 3: Record the results in the spec**

Append to the spec:

```markdown
## Implementation notes (YYYY-MM-DD)

- arm64 ELF kernel on Linux arm64 (qemu-aarch64 X.Y): N ok, M skipped, 0 failed.
- Conversion: arm64 N rebases, x86_64 N rebases; 10 exports each; universal dylib built.
- macOS smoke: Apple Silicon (macOS X.Y, model) PASS; Intel (macOS X.Y, model) PASS.
- Deviations from the design: (none, or what changed and why)
```

Fill in the real values, set the spec's `**Status:**` line to
`implemented; verified on Apple Silicon and Intel Macs`, and commit:

```bash
cd /home/kosaka/anyfs-reader
git add -- docs/superpowers/specs/2026-10-05-lkl-macos-elf2dylib-design.md
git commit -m "docs(spec): record the macOS verification of the ELF-to-dylib port

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>" -- docs/superpowers/specs/2026-10-05-lkl-macos-elf2dylib-design.md
```

---

### Task 10: Land

- [ ] **Step 1: Run every Linux-side test once more**

```bash
cd /home/kosaka/anyfs-reader
scripts/macho/test_elf2dylib.sh | tail -1
scripts/macho/test_glue.sh
bash -n scripts/oot_fs.sh scripts/build_lkl.sh scripts/macho/*.sh && echo "syntax ok"
git status --short
```

Expected: `PASS test_elf2dylib`, `PASS test_elf_glue`, `PASS test_macho_shim`,
`syntax ok`. `git status` shows none of this plan's files, apart from
`patches/linux/macho/0[1-7]` and `scripts/macho/shim`, which no longer exist.

- [ ] **Step 2: Push**

`main` may also carry `anyfs-reader-92`'s local commits. Ask it first, with SendMessage
to `anyfs-reader-92`: "May I push main to origin now (it includes my macOS commits)?".
After it agrees, run `git push origin main`. The user's landing rule is to push approved
work straight to main, so no PR is needed. Never push the tag `exp/macho-object-port`
and never run `git push --tags`.

- [ ] **Step 3: Update the project memory**

Rewrite `/home/kosaka/.claude/projects/-home-kosaka-anyfs-reader/memory/project_lkl_macho_port.md`
to describe the shipped design:
- the ELF kernel is converted by `scripts/macho/elf2dylib.py` (`build_kernel_dylib.sh`,
  `build_host_lib.sh`, `build_smoke.sh`);
- the old object port lives in the local tag `exp/macho-object-port`;
- the verification results.

Keep the still-valid traps (`;` as an asm comment, `sem_init` ENOSYS, `__common` vs
`__bss`, clang's `-fno-builtin` dropping `returns_twice`), and drop what no longer
applies. Update its line in `MEMORY.md`.

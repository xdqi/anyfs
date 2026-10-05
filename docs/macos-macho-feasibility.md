# Native macOS LKL: Mach-O feasibility experiment

> **Superseded (2026-10-05).** This experiment compiled the kernel straight to Mach-O
> objects and rebuilt the linker script's guarantees by hand. The macOS port now
> converts the standard ELF kernel instead: see
> `docs/superpowers/specs/2026-10-05-lkl-macos-elf2dylib-design.md`. The shims,
> kernel patches 01–07, order file and harness described below are archived in the
> local tag `exp/macho-object-port` (`git show --stat exp/macho-object-port`).
> The traps recorded here still apply to any code compiled for Darwin.

Status: **mechanism verified, work bounded.** Everything below was executed on the Linux
dev box with stock Debian clang/LLVM 19. Nothing has run on Mac hardware yet.

Harness: `.tmp/macho-exp/sweep.py` (untracked scratch). Kernel patches:
`patches/linux/macho/`, applied by `scripts/oot_fs.sh stage --macho`. They depend on the
shim headers in `scripts/macho/shim/` (section names such as `.ic<N>.init` and the
`__DATA,` prefix come from there), so the two only work together.

## Question

Can LKL be built as a native Mach-O `liblkl` for arm64 macOS, so the Electron native
backend works on Apple Silicon the way it already does on Linux and Windows?

The blocker was believed to be structural: the LKL build ends in
`ld -r -T arch/lkl/kernel/vmlinux.lds` followed by
`objcopy -G<11 entry points> --prefix-symbols=`, and macOS `ld64` accepts no GNU linker
script, has no `-r` in `ld64.lld`, and has no `objcopy -G` equivalent.

## Method

`arch/lkl` is architecture-neutral, so the `linux-amd64` build tree is a valid reference
for the file set and per-object flags. The harness reads the command line Kbuild saved for
every object (`savedcmd_<obj> :=` in `.<obj>.o.cmd`), swaps the compiler for
`clang -target arm64-apple-macos11`, drops the five gcc-only codegen flags plus debug info,
and recompiles all 1907 objects. Kernel headers are overridden by prepending a shim include
directory whose headers `#include_next` the real ones — **nothing under `~/linux` is
modified**.

Two independent measurements, so that the known section-naming problem cannot mask an
unknown one:

- Section attributes neutralized → measures every *other* portability problem.
- Section names enumerated statically from the 1907 real ELF objects → the definitive
  rename list, without depending on shim fidelity.

## Results

### 1. The toolchain works, and works from Linux

| Mechanism | Result |
|---|---|
| clang → Mach-O arm64, `-ffreestanding -nostdinc` | works, no macOS SDK needed |
| `section$start$__SEG$__sect` / `section$end$…` boundary symbols | resolve to real addresses |
| Multiple sections laid out contiguously and in order | in this two-section probe only (`__initcall1` then `__initcall2`); at real scale ld64 scatters them — see stage 4 |
| Ordering within one section across several objects | preserved in link order |
| `section$start$` for a section nothing contributes to | still links |
| Section names containing dots, e.g. `__DATA,.init.data` | accepted verbatim |
| 16-character section-name limit | enforced at *compile* time, precise diagnostic |
| `llvm-libtool-darwin` / `llvm-ar` Mach-O archives | work |
| `ld64.lld` final link (executable and dylib) | works |
| `-exported_symbols_list` hiding | works |
| `llvm-objcopy --redefine-sym` on Mach-O | works |
| `ld64.lld -r` | **not implemented** |
| `llvm-objcopy --localize-symbol` / `--keep-global-symbol` on Mach-O | **not supported** |

Consequence for the design: do not try to reproduce `ld -r`. Ship an **archive of Mach-O
objects** and let the final `ld64` link perform section merging and synthesize the boundary
symbols. This also removes two ceilings that a merge-then-translate approach would hit —
the 24-bit `ARM64_RELOC_ADDEND` field and the 255-section `n_sect` cap.

An archive changes what gets linked, though. ld64 pulls an archive member only to resolve an
undefined symbol, and most filesystem and driver objects are reached only through their
`.ic<N>.init` initcall entries, which nothing references by name. A plain link would
silently drop them, and those filesystems would never register. The final link therefore
needs `-all_load` (or `-force_load liblkl.a`). Even then, initcalls within one level run in
the order ld64 lays out the members' atoms, which follows archive member order; that order
has to match Kbuild's link order, as `ld -r` preserves it on ELF.

Replacement for `objcopy -G`: `-exported_symbols_list` at the final link, plus
`llvm-objcopy --redefine-sym` for kernel symbols that collide with libc names
(the same 26-symbol set `scripts/lkl-wasm-tools/wasm_prefix_kernel_symbols.py` maintains).
ZFS adds two more, the lua `setjmp`/`longjmp` in `setjmp_aarch64.S`; `scripts/oot_fs.sh`
renames those at the source level instead (see stage 5, ZFS).

### 2. Compile sweep

**1511 of 1907 objects (79%) compile to Mach-O arm64 unmodified.** The 396 failures reduce
to seven macro sites, one compiler flag, and the out-of-tree ZFS driver:

| Failures | Root cause |
|---|---|
| 305 | `__initcall_section` — `include/linux/init.h:245` builds the section name by stringify+concat, bypassing `__section` |
| 50 | `__SYSCALL_DEFINEx` — `include/linux/syscalls.h:257` uses `__attribute__((alias(…)))`; clang: *aliases are not supported on darwin* |
| 26 | `DECLARE_PCI_FIXUP_SECTION` (`include/linux/pci.h:2361`) |
| 22 + 11 + 2 | out-of-tree ZFS: `#error "Unsupported platform"` in zstd, missing `asm/neon.h`, `unknown OS` |
| 2 | `__cacheline_aligned` (`include/linux/cache.h:72`) |
| 2 | `fs/hfs/hfs_fs.h:266` uses a local variable named `__block`, which is a **clang keyword on Apple targets** — fixed by `-fno-blocks` (verified) |
| 1 each | `_ELFNOTE`, `__alias`, and `.weak` in `kernel/sys_ni.o` inline asm (Mach-O spells it `.weak_definition`) |

`__SYSCALL_DEFINEx` and `__cacheline_aligned` are already `#ifndef`-guarded arch override
points, so they can be supplied from `arch/lkl/include/asm/` rather than by patching generic
headers.

### 3. Section-name inventory (definitive)

Of the **79** distinct section names this config emits, **59 are usable verbatim** with only
a `__SEG,` prefix. Exactly **20 need renaming** to fit 16 characters:

```
.data..cacheline_aligned   .pci_fixup_resume_early   .data..init_thread_info
__tracepoints_strings      .data..shared_aligned     .initcallrootfs.init
.discard.addressable       .data..ro_after_init      .initcallearly.init
__tracepoints_ptrs         __stop_sched_class        .softirqentry.text
.pci_fixup_suspend         __idle_sched_class        __fair_sched_class
.data.rel.ro.local         .pci_fixup_resume         .pci_fixup_header
.pci_fixup_enable          .init_array.00101
```

79 sections is far below Mach-O's 255-section limit.

### 4. External dependency surface — the decisive number

Symbol accounting over all 1511 Mach-O objects: 57052 defined, 2910 unresolved. Cross-
checking against the complete ELF `vmlinux` (71891 defined symbols) shows **2907 of those
2910 are defined inside the 396 objects that failed to compile** — they resolve as soon as
those compile. Genuinely external:

| Symbol | Refs | Nature |
|---|---|---|
| `bzero` | 26 | clang lowers `memset(p,0,n)` to `bzero` on Darwin targets. `-fno-builtin-bzero` does **not** suppress it (nor does the build's existing `-fno-builtin`) — provide the symbol as an alias of `memset` instead |
| `lkl_bug` | 2 | existing LKL host op |
| `lkl_printf` | 2 | existing LKL host op |

**The kernel's external dependency surface on Mach-O is the same two host ops as on ELF,
plus one trivially suppressed clang builtin.** No hidden libc dependency, no missing
compiler runtime, no symbol-level Darwin surprise.

### 5. Link at real scale

`llvm-ar` produced a 23 MB, 1511-member Mach-O archive; `ld64.lld` linked it into a 15 MB
arm64 dylib (8.8 MB `__text`, 15 sections, 20790 exported symbols) in **0.116 s**, with no
diagnostics. The toolchain handles the real kernel at real scale.

## Stage 2: the actual port

A hand-written shim header set (now `scripts/macho/shim/`; 16 headers at this stage) implements the
design above. Each header `#include_next`es the real kernel header and overrides one macro
family, so `~/linux` is still never modified. Result:

**1867 of 1907 objects compile, and they link into a 21 MB arm64 Mach-O dylib with 60
sections.** Excluding the out-of-tree ZFS driver that is 1867/1872 = 99.7%.

Unresolved-symbol accounting on the linked set: 76122 defined, 392 unresolved, of which 389
are defined inside the 40 objects that still fail. Genuinely external, unchanged from
stage 1: `bzero` (30 refs), `lkl_bug` (3), `lkl_printf` (2).

### What the shim covers

| Override | Why |
|---|---|
| `__section(s)` → `"__DATA," s` | default segment prefix |
| `__init`, `__exit`, `__ref`, `__sched`, `__lockfunc`, `__kprobes`, `__irq_entry`, `__softirq_entry`, `__noinstr_section` (also covers `noinstr`/`__cpuidle`) | code must be in `__TEXT` |
| `__define_initcall` + `__initcall_section` | `.initcall<id>.init` → `.ic<id>.init`; also supplies the segment, since `CONFIG_HAVE_ARCH_PREL32_RELOCATIONS` is unset and that path emits a direct section attribute |
| `__PCPU_ATTRS` | bypasses `__section`; all percpu variants collapse to one `.pcpu` section |
| `__cacheline_aligned`, `__ro_after_init`, `__init_thread_info`, `__ADDRESSABLE`, `__TRACEPOINT_ENTRY`, `__DEFINE_TRACE_EXT` | names over 16 chars |
| `DECLARE_PCI_FIXUP_SECTION` + 16 wrappers | over-long names; wrappers keep each phase in its own section |
| `__SYSCALL_DEFINEx` | replaces `__attribute__((alias))` with `.globl`/`.set` asm |
| `MODULE_DEVICE_TABLE` | alias with no consumer (`CONFIG_MODULES=n`) |
| `_ELFNOTE`/`ELFNOTE*` | ELF notes are meaningless in Mach-O |
| `__SYSCALL_DEFINE_ARCH` (`arch/lkl/include/asm/unistd.h`) | LKL's own syscall-signature harvest used GNU `.section name,"a"` syntax; rewritten to `.section __DATA,__sysdefs` |
| `cond_syscall`, `SYSCALL_ALIAS` (`linux/linkage.h`) | `.weak`/`.type @function` do not exist in Mach-O; see stage 3 |

Plus two compiler flags — `-fno-blocks`, `-D__DISABLE_EXPORTS` — and providing `bzero`.

### Two findings from the link stage

**ld64 enforces the text/data split.** Placing a code section in `__DATA` fails the link with
`references section .sched.text which is not in segment __TEXT`. Segment assignment cannot be
silently wrong — a useful property.

**`EXPORT_SYMBOL` silently corrupts the image.** `include/linux/export.h` emits
`asm(".section \".export_symbol\",\"a\"")`; the Mach-O assembler parses that GNU-syntax
directive as *segment* `.export_symbol` and *section* `"a"`, and the link succeeds with a
garbage section in the output. `-D__DISABLE_EXPORTS` (already used by the pe and wasm
targets) removes it. This is the one problem in the whole exercise that produced no
diagnostic.

### Boundary symbols proven on the real kernel

A probe object declaring `section$start$`/`section$end$` for twelve real kernel sections —
`.ic0/.ic1/.ic4/.ic7/.icearly.init`, `.pcpu`, `.data..roai`, `__ex_table`, `__param`,
`__tp_ptrs`, `__TEXT,.init.text`, `__TEXT,.sched.text` — links against the archive with
**zero** of those symbols left undefined (`llvm-nm -u | grep -c 'section$'` = 0), i.e. ld64
synthesized every one. `__ex_table` is empty in this config and still resolves.

## Stage 3: the kernel proper compiles completely

Two of the stragglers turned out to be shimmable after all, and the remaining two became
real patches under `patches/linux/macho/`, applied by `scripts/oot_fs.sh stage --macho`
alongside the existing `--wasm` series. Both are `#ifdef __MACH__` guarded, so they are
byte-for-byte no-ops for the elf/pe/wasm builds. (For 02 that holds only because its
section-name macros sit above the `CONFIG_HAVE_ARCH_PREL32_RELOCATIONS` split, since
`__DEFINE_TRACE_EXT` uses them on both sides. Verified on an x86_64 defconfig, where PREL32
is on: a `DEFINE_TRACE` unit preprocesses to identical output with and without the series.)

**1871 of 1907 objects compile, and every one of the 36 remaining failures is under
`fs/zfs/`** (`grep -vc '^fs/zfs/' failed-port.txt` = 0). The kernel proper is at 100%.

They link into a 21 MB arm64 Mach-O dylib with **66 sections**, including all five
`__sc_{fair,idle,rt,dl,stop}` sched-class sections and both `__tp_ptrs` / `__tp_strings`.
(Stage 5 later folded the five sched-class sections into one `__sched_class` section.)

Unresolved symbols: 180, of which 177 are defined inside the 36 ZFS objects. Genuinely
external: still exactly `bzero`, `lkl_bug`, `lkl_printf`.

### `cond_syscall`: Mach-O weak definitions need a real definition

`include/linux/linkage.h:56` emits `.weak <sym>` + `.type <sym>,@function` + `.set`. Mach-O
has no `.weak` (it is `.weak_definition`), no `@function` type directive, and needs a leading
underscore. The obvious translation does **not** work: a `.weak_definition` applied to a
`.set` alias leaves the symbol strong, and a real definition elsewhere then fails the link
with `duplicate symbol`. Mach-O weak definitions require an actual definition in a section,
so the fallback is emitted as a four-byte tail-branch thunk instead of an alias:

```
.section __TEXT,__text ; .globl _x ; .weak_definition _x ; .p2align 2 ; _x: b _sys_ni_syscall
```

Verified: links cleanly, and a strong definition elsewhere wins. `linkage.h` is reached by a
normal angle-bracket include, so this is a shim, not a patch.

### The two kernel patches

| Patch | Why it cannot be a shim |
|---|---|
| `01-sched-class-macho-section.patch` — `DEFINE_SCHED_CLASS` → one `__sched_class` section (originally `__sc_<name>`; see stage 5) | `kernel/sched/sched.h` is reached by a quoted `#include` from `kernel/sched/*.c`, so quoted-include lookup finds the real header before any `-I` shim |
| `02-tracepoint-macho-section.patch` — `__tracepoints_ptrs`/`__tracepoints_strings` routed through `__TRACEPOINT_PTRS_SEC`/`__TRACEPOINT_STRS_SEC` | shimmable in principle, but only by copying the ~40-line `__DEFINE_TRACE_EXT` macro verbatim; a two-line indirection in the real header is far more robust |

Verified reversible: `stage --macho` twice is idempotent, `unstage --macho` leaves zero
`__MACH__` residue, and `git status` on the kernel tree returns to its exact prior state.

**Trap worth knowing:** `oot_fs.sh unstage --macho` does not mean "revert only the macho
patches" — `cmd_unstage` unconditionally tears down the OOT symlinks and the
`fs/Kconfig`/`fs/Makefile` marker blocks as well. Re-run plain `stage` afterwards to put them
back. (Pre-existing behaviour, not introduced by the `--macho` addition.)

### What is left: ZFS only

| Count | Item |
|---|---|
| 22 | ZFS zstd `#error "Unsupported platform"` |
| 11 | ZFS ICP wants `asm/neon.h`, which `arch/lkl` does not provide |
| 1 | ZFS `os/linux/zfs/policy.c`: `#error "unknown OS"` |
| 1 | ZFS `zfs_replay.c`: no `va_type` in `struct vattr` |
| 1 | ZFS lua `setjmp` inline asm |

All five need Darwin/arm64 guards in the staged out-of-tree tree, which anyfs already owns
via `scripts/oot_fs.sh`. Independent of the Mach-O work — the same tree is already known not
to build for arm64 Linux either.

## Stage 4: the boundary-symbol consumers

Compiling and linking says nothing about whether the kernel will *behave*. The linker-script
symbols are the place that breaks silently, so they were measured next.

### The work list

Cross-referencing the unresolved set against symbols present in `vmlinux` but in no
individual object isolates exactly what the linker script used to create: **83 boundary
symbols**. (94 more come from the unbuilt ZFS objects, and 3 are the genuinely external
`bzero`/`lkl_bug`/`lkl_printf`.)

### The mechanism: asm-label redeclaration, no definitions at all

`extern initcall_entry_t __initcall0_start[] __asm__("section$start$__DATA$.ic0.init");`
renames the symbol at link time, so every existing reference in the kernel resolves to the
boundary ld64 synthesizes. Nothing has to be *defined*. Verified:

- an asm label on the **first** declaration survives the kernel's later plain `extern`, and
  survives being used in between;
- a plain `extern` **first** and the asm label **second** also works, so the redeclaration
  can live either before or after the kernel's own header;
- a type mismatch is a hard error, so a wrong guess fails loudly;
- an incomplete element type (`struct pci_fixup` forward-declared) is rejected, so each
  redeclaration has to sit where its type is complete — i.e. in a shim of the declaring
  header, not one global force-included file.

### The silent breakage this found

Under Mach-O every initcall level is its own section, and **ld64 lays them out in arbitrary
order with unrelated sections in between**. Measured in the linked dylib:

```
.icrootfs .ic4 .ic6 .ic7 .ic1 .icearly .ic5 .ic2 .ic0 .ic5s .ic4s .ic7s
          gaps of 1816 / 58864 / 29552 / 74704 / 14960 / 6888 / 1080 bytes
```

`init/main.c`'s `initcall_levels[]` takes each level's **end from the next level's start**.
On Mach-O that is simply wrong: levels would run in the wrong order and iterate across tens
of kilobytes of unrelated data. It produces no diagnostic at build time — it would only show
up as a corrupt boot.

`03-initcall-macho-ranges.patch` replaces it, under `__MACH__`, with an explicit
(start, end) pair per sub-section and a table spelling out which sub-sections belong to each
level, mirroring `INIT_CALLS()` in `vmlinux.lds.h` — including that each level has an `s`
sync sub-level and that `rootfs` runs inside level 5. Verified: `init/main.c` compiles and
now references all 38 `section$start$`/`section$end$` symbols instead of `__initcallN_start`,
the full tree still builds (1871/1907), and the linked dylib has **zero** unresolved
`section$` symbols — ld64 synthesized every one.

Two details that mattered: a `const` table marked `__initdata` trips
`section type conflict with 'kthreadd_done'` because clang wants const data read-only, so it
uses `__initconst`; and a sub-section nothing contributes to still gets a boundary pair with
`start == end` (verified by disassembly), so empty levels iterate zero times.

### Where it stands

83 → **73** boundary symbols. Remaining, by group:

| Group | Count | Notes |
|---|---|---|
| `jiffies` | 1 (185 refs) | not a boundary — the script did `jiffies = jiffies_64`. An asm label pointing at `_jiffies_64` is enough on little-endian LP64 |
| whole-image markers (`_stext`, `_etext`, `_text`, `_sinittext`, `_einittext`, `_sdata`, `_edata`, `_end`, `__init_begin`, `__init_end`, `__bss_start`, `__bss_stop`, `__start_rodata`, `__end_rodata`) | 14 | the LKL linker script's own FIXME says these are informational |
| PCI fixups | 16 | 8 phases × 2; `struct pci_fixup` is complete after `linux/pci.h` |
| trace/ftrace tables | 10 | declared in `kernel/trace/*.c`, so they need patches or an early shim |
| `__setup_*`, `__con_initcall_*`, `__start___param`/`__stop___param`, `__start___ex_table`/`__stop___ex_table`, `__start___modver`/`__stop___modver`, `__start_notes`/`__stop_notes` | 12 | mechanical |
| `__sched_text_*`, `__cpuidle_text_*`, `__irqentry_text_*`, `__softirqentry_text_*` | 8 | `__TEXT` segment |
| `__sched_class_highest`/`_lowest` | 2 | needs care: the sched classes are laid out in **reverse** order and `for_class_range` assumes contiguity — the same trap as initcalls |
| `kallsyms_*` | 8 | not fixable by declaration: `scripts/link-vmlinux.sh` generates these by iterating over the `ld -r` output, which no longer exists. `CONFIG_KALLSYMS=n` is required, and per project convention that belongs in `scripts/gen_lkl_config.sh` |
| `init_stack`, `init_thread_union` | 2 | the wasm port aliases them in `arch/lkl/kernel/setup.c`; `__attribute__((alias))` is unsupported on Darwin, so use the `.set` asm form |

`__sched_class_highest/_lowest` is the one left that can still break silently, for exactly
the reason initcalls did.

## Stage 5: everything the compiler and linker can settle

**1906 of 1906 objects compile to Mach-O arm64 — the whole kernel including the
out-of-tree ZFS driver — and all 83 linker-script boundary symbols are gone.**

The linked dylib is 22 MB with 75 sections. Four build gates, all passing:

| Gate | Result |
|---|---|
| unresolved `section$…` boundaries | 0 (ld64 synthesized every one) |
| bogus `"a"` section from `EXPORT_SYMBOL` | 0 |
| sched_class layout | order `stop < dl < rt < fair < idle`, uniform 216-byte stride |
| unresolved symbols | 13 — see below |

### The sched_class trap, and how `-order_file` settles it

`for_class_range()` walks the classes with `class++` and `sched_class_above()`
expresses priority as an **address comparison**, so the five classes must be
physically contiguous *and* ordered. `SCHED_DATA` in the linker script normally
guarantees it; nothing does under Mach-O. Worse, the natural layout is hopeless:
`build_policy.c` includes `idle.c`, `rt.c`, `deadline.c` in that order (the reverse
of what is needed), `stop_task.c` is in `build_utility.o`, and `fair.c` is its own
object.

`01-sched-class-macho-section.patch` puts all classes in one `__sched_class`
section and the link forces the order with `ld64 -order_file`
(`scripts/macho/sched_class.order`). Verified on the real kernel: correct order,
uniform stride, contiguous — from objects linked in the wrong order.

Nothing fails at build time if a link forgets `-order_file`, so the result has to be
gated: `scripts/macho/check_sched_class.sh IMAGE` checks that the section holds exactly
the classes in the order file, in that order, at one uniform stride (run it before local
symbols are stripped). Checked against synthetic links: passes with `-order_file`, fails
without it, and fails on a sixth class (`ext_sched_class`, should `SCHED_CLASS_EXT` ever
become possible on LKL).

### The boundary symbols: asm-label redeclaration, and its one hard rule

All 83 resolve with no definitions anywhere. The rule that took two attempts to
get right: **an asm label must be attached before the symbol's first use.** A plain
`extern` first and the label second works only if nothing referenced it in between,
and real headers do reference their own symbols — `linux/jiffies.h` uses `jiffies`
in inline functions further down, and `linux/mm.h:3300` re-declares
`__init_begin`/`__init_end` at block scope and uses them immediately. Those became
patches at the declaration site; the rest are shims.

Where the rest of the boundary symbols ended up:

| Where | Why |
|---|---|
| `04-jiffies-macho-alias.patch` | `jiffies = jiffies_64` is an alias, not a boundary — and the most-referenced script symbol (185 refs). Used inside its own header, so it must be a patch |
| `05-extable-macho-bounds.patch` | `struct exception_table_entry` is only forward-declared in `<linux/extable.h>`; an array of incomplete type is rejected, so the label goes where the type is complete |
| `06-sections-macho-bounds.patch` | the 18 whole-image markers, at their declaration site. `_stext`/`_etext` and `__start_rodata`/`__end_rodata` span the whole `__TEXT` segment, `_sdata`/`_edata`/`_end` the whole `__DATA` segment (`segment$start$`/`segment$end$`): `core_kernel_text()`, `is_kernel_core_data()` and `kstrdup_const()` test addresses against them, and single sections would miss `.sched.text`, string literals in `__cstring`, and zero-initialized globals, which Darwin puts in `__DATA,__common` rather than `__bss` |
| `07-mm-init-bounds-macho.patch` | the block-scope re-declaration in `mm.h` needs the *same* label, since either can be seen first |
| shims | `__setup_*`, `__con_initcall_*` (also respelled: `.con_initcall.init` is 18 chars), `__param`, `__modver`, PCI fixups × 8 phases, trace/ftrace tables, `__sched_text_*`, `__cpuidle_text_*`, `init_stack`, `init_thread_union`, `cond_syscall`, `SYSCALL_ALIAS` |

### Two more silent failures found

**`;` is a comment in the Darwin arm64 assembler.** The kernel's asm macros use `\`
continuations, so their bodies collapse onto one line with `;` between directives.
On Darwin everything after the first `;` is silently discarded — no warning, and the
object comes out with **no symbols at all**. Verified minimal case:
`.text; .globl _one; .balign 2; _one:` defines `_one` for `arm64-linux` and yields
only `ltmp0` for `arm64-apple-macos`. This is what made ZFS's `setjmp`/`longjmp`
vanish. Any `;`-separated one-line asm macro is suspect.

**Hand-written asm needs the leading underscore spelled out.** Mach-O prefixes C
symbols with `_`; `.globl setjmp` in a `.S` file does not satisfy a C reference to
`setjmp`. (`arch/lkl/Makefile` already carries a `prefix=_` concept for `pe-i386`.)

### Flags, and one config change

| Flag | Why |
|---|---|
| `-fno-blocks` | `fs/hfs/hfs_fs.h` names a local `__block`, a clang keyword on Apple targets |
| `-D__DISABLE_EXPORTS` | kills the `EXPORT_SYMBOL` section corruption (as pe/wasm already do) |
| `-D__CYGWIN__` | `lib/crypto/sha256.c:275` guards the HMAC definitions on `!__DISABLE_EXPORTS \|\| __CYGWIN__`, so the flag above silently drops `hmac_sha224/256` while `<crypto/sha2.h>`'s inlines still call them. Same workaround the wasm build uses — and this is *why* it works |
| `-D__linux__=1` | the triple is `*-apple-macos`, so clang does not define `__linux__`, but this is Linux kernel code and ZFS keys its OS detection on it. `build_lkl_wasm.sh:289` already does this |
| `-U__APPLE__` | the flip side: with `__APPLE__` set, `zfs_replay.c` takes the macOS-userland path and reaches for `vap->va_type`, which the Linux `struct vattr` has no member for. We want the object **format**, not macOS semantics — which is why every patch here keys on `__MACH__`, not `__APPLE__` |
| `CONFIG_KALLSYMS=n` | not a boundary problem: `scripts/link-vmlinux.sh` *generates* the eight `kallsyms_*` symbols by iterating `scripts/kallsyms` over the `ld -r` output across several passes. There is no merge step and so no output to iterate. Belongs in `scripts/gen_lkl_config.sh`'s overlay |

`bzero` needs providing rather than suppressing: clang lowers `memset(p,0,n)` to
`bzero` in the backend, so neither the kernel's existing `-fno-builtin` nor an
explicit `-fno-builtin-bzero` stops it (both verified). Three lines over `memset`;
belongs in `arch/lkl/lib/`.

### ZFS

`scripts/oot_fs.sh` gained six more idempotent gates, in the same style as its
existing x86 ones: skip the aarch64 SIMD header (`simd_aarch64.h` wants
`asm/neon.h`, `asm/hwcap.h`, `asm/sysreg.h`, none of which `arch/lkl` has), skip the
ARM SIMD-stat block and the ARM paths in `sha256_impl.c`/`sha512_impl.c` that call
the now-undeclared `zfs_*_available()`, and rewrite `setjmp_aarch64.S`'s four call
sites for Mach-O asm syntax. Unlike the older x86 gates, each one dies if its anchor
line is missing, so a ZFS pin bump cannot turn it into a silent no-op.

The setjmp rewrite also renames the Mach-O symbols to `_zfs_lua_setjmp`/`_zfs_lua_longjmp`,
and the sixth gate gives `module/lua/ldo.c`'s declarations matching asm labels. Under
their own names they are globals called exactly like libc's `_setjmp`/`_longjmp`, and with
no `objcopy -G` to localize them, host code in the same ld64 link binds to them —
`tools/lkl/lib/jmp_buf.c` calls libc `setjmp`/`longjmp`, and the ZFS version saves neither
d8–d15 nor the signal mask. Verified with a minimal ld64.lld link: with the old names the
dylib defines `_setjmp` itself; with the new ones it imports `_setjmp` from libSystem. The
ELF objects are unchanged (identical disassembly).

The Mach-O declaration also carries an explicit `__returns_twice__`. Compilers recognize
`setjmp` by name, but differently: gcc does so even under the kernel's `-fno-builtin`
(its CFG for the caller gets the abnormal-dispatcher edge), while clang recognizes only
the `setjmp` library builtin, which `-fno-builtin` disables. Compiling the real `ldo.c` for
Mach-O, the call had no `returns_twice` without the attribute and has it with it. So the
ELF build (gcc) never had the problem; the clang-only Mach-O build did.

### The 13 that remain

| Count | Symbol(s) | Status |
|---|---|---|
| 2 | `lkl_bug`, `lkl_printf` | by design — LKL's host ops, same as the ELF build |
| 11 | `zfs_blake3_{compress_in_place,compress_xof,hash_many}_sse{2,41}`, `zfs_sha{256,512}_block_armv7`, `fletcher_4_aarch64_neon_ops`, `vdev_raidz_aarch64_neon{,x2}_impl` | ZFS's **aarch64** SIMD implementations |

Those 11 are not a Mach-O problem and not reachable from this harness: the reference
object list is amd64-derived, so ZFS's aarch64 sources are absent from it. (blake3
exposes its NEON code under the `sse2`/`sse41` names, which is why `__aarch64__`
pulls those in.) The four `.S` files do exist; assembling them for Mach-O needs
~40 ELF-only directives changed — 10 `.section` with `@note`/`@progbits` flags,
15 `.type` with `%object`/`%function`, 15 `.size` — plus underscore-prefixing 11
`.globl`s. The two C-side NEON files (`zfs_fletcher_aarch64_neon.c`,
`vdev_raidz_math_aarch64_neon{,x2}.c`) are plain C and just need to be in the build.

This is bounded and specified, but it cannot be *verified* until a real arm64 Kbuild
target exists, so it is deliberately left as a spec rather than unverifiable work.
It also sits on top of a pre-existing gap: this ZFS tree has never been built for
arm64 at all.

## Stage 6: a loadable macOS artifact

**An 18 MB arm64 Mach-O dynamically-linked shared library, `MH_NOUNDEFS` set, 77 sections,
exporting the full LKL API and importing nothing but `/usr/lib/libSystem.B.dylib`.**

```
liblkl-macos.dylib: Mach-O 64-bit arm64 dynamically linked shared library,
  flags:<NOUNDEFS|DYLDLINK|TWOLEVEL|WEAK_DEFINES|BINDS_TO_WEAK|HAS_TLV_DESCRIPTORS>
  exports  _lkl_start_kernel _lkl_init _lkl_cleanup _lkl_syscall _lkl_sys_halt
           _lkl_is_running _lkl_disk_add _lkl_mount_dev
  imports  /usr/lib/libSystem.B.dylib only
```

Verified: **67 undefined symbols, 66 of which are in `libSystem.tbd`**; the 67th is
`dyld_stub_binder`, which dyld itself provides — every macOS dylib has it. The sched_class
gate still holds inside the finished image (correct order, uniform 216-byte stride), and all
24 boundary sections are present, including the LKL-specific `__sysdefs` and `__sched_class`.

Filesystems linked in: ext4 (35 objects), btrfs (62), xfs (104), NTFS-PLUS (27), APFS (24),
f2fs (20), plus FAT.

### The Darwin toolchain problem, and the way around it

Unlike the kernel — freestanding, `-nostdinc`, needing no SDK — the host layer is ordinary
macOS userland C that wants `<pthread.h>`, `<sys/mman.h>`, `<libkern/OSByteOrder.h>`. There
is no macOS SDK on this box. `zig cc -target aarch64-macos` supplies both the Darwin headers
and a `libSystem.tbd` stub, so the whole thing cross-builds from Linux. (Note the triple
needs three version components or none: `aarch64-macos.11` is rejected.)

### The Darwin host layer

All 12 host-layer sources compile. Four gaps, and one of them is a trap:

| Gap | Resolution |
|---|---|
| `timer_create`/`timer_settime`/`timer_delete` | absent on macOS. `posix-host.c` **already** carries a pthread-per-timer emulation under `#ifdef __wasm__`, added because emscripten declares POSIX timers without implementing them — the identical situation. Extended to `__APPLE__` |
| `sem_init` | **the trap.** macOS defines `_POSIX_SEMAPHORES` and exports `sem_init`, but unnamed POSIX semaphores are unimplemented: it fails with `ENOSYS` at runtime. Compiles and links clean, so nothing catches it until boot. The value is the tell: Darwin defines it as `-1`, which POSIX defines as "unsupported", yet `posix-host.c` only tested `#ifdef`. The patch drops the macro when it is negative, which routes to the pthread mutex+condvar path this file already has. glibc, musl/emscripten and FreeBSD define positive values, so their builds are unaffected |
| `pthread_getattr_np` | absent. Darwin exposes `pthread_get_stackaddr_np`/`pthread_get_stacksize_np`, but stackaddr is the stack **base** (highest address) where `pthread_attr_getstack` reports the lowest — so the port subtracts the size, or the kernel gets a stack region one stack-size too high |
| `<endian.h>`, `le16toh` & friends | absent; `endian.h` gained an `__APPLE__` branch over `<libkern/OSByteOrder.h>`, alongside its existing FreeBSD/Android/MINGW32 ones |
| `MAP_FIXED_NOREPLACE` | absent, and macOS has no `MAP_EXCL` either. It becomes 0, so `addr` is a plain hint: when the range is taken, macOS maps elsewhere instead of failing. `lkl_mmap()` already treats `ret != addr` as failure; the patch adds the same check (unmapping `ret`) to `lkl_shmem_mmap()` on Darwin |
| `MAP_NORESERVE` | `<sys/mman.h>` does define it (`0x40`, a Sun-era leftover `mmap(2)` does not document); the patch `#undef`s it to 0 so it stays out of the flags. Redefining it without the `#undef` was a `-Wmacro-redefined` warning |
| `<sys/syscall.h>` | not in the macOS userland header set, and nothing here uses it. Guarded |

Two support pieces sit beside the artifact, whose real homes are noted in their headers:
`macho-support.c` (`bzero`) and `darwin-netdev-stubs.c` (`lkl_netdev_{tap,raw}_create`, which
`config.c` references unconditionally and which cannot work on macOS — tap needs
`<linux/if_tun.h>`, raw needs `AF_PACKET`). A Darwin `lkl_autoconf.h` profile turns off
`VFIO_PCI`, `VIRTIO_NET_MACVTAP` and `FUSE`; that is what a Darwin branch in
`tools/lkl/Makefile.autoconf` would emit next to the existing `posix_host`/`nt64_host`/
`bsd_host` profiles.

### What is excluded, and why

ZFS is not in this artifact (281 objects). Two ZFS-specific problems remain, both bounded:

- the aarch64 SIMD assembly (~40 ELF-only directives across 4 `.S` files, plus underscore
  prefixes — spec in stage 5);
- `dmu_buf_add_ref`/`dmu_buf_try_add_ref` come out **`W` (weak global) in ELF but `t`
  (local) in Mach-O**, so cross-TU references do not resolve. A header-`inline` linkage
  difference between the two formats.

Neither is a blocker for the anyfs Electron path, which needs ext4/btrfs/xfs/NTFS/APFS first.

## Caveats

- Compile and link only. Nothing has executed under XNU; page size (16 KiB), thread stack
  sizes, and runtime ordering are unverified.
- The 396 objects that did not compile had their *own* outgoing references unmeasured. They
  are ordinary kernel C files, so new external dependencies are unlikely but not excluded.
- Only `ld64.lld` was exercised. Apple's `ld` (ld-prime) is stricter in places — notably
  zero-size sections and custom segment permissions.
- ZFS compiles completely (stage 5) but is not in the stage-6 artifact; its aarch64 SIMD
  assembly and the `dmu_buf_*_ref` linkage difference remain (stage 6).
- `CONFIG_KALLSYMS=y` currently works because `scripts/link-vmlinux.sh` iterates over the
  `ld -r` output. With no merge step there is nothing to iterate; kallsyms has to be turned
  off or reworked.
- The final link must use `-all_load`/`-force_load` and `-order_file`, and should run
  `scripts/macho/check_sched_class.sh` on the result. No tracked build script performs that
  link yet.
- Mach-O has no `/DISCARD/`, so `.exit.text`, `.exitcall.exit`, `.disc.addr`, `.modinfo` and
  the constructor array survive into the image. `.init_array` becomes
  `__DATA,__mod_init_func`, which dyld will **run** at load; the ELF build discards it.
- Inherited from code the other hosts share, and left as is because it is not
  Mach-O-specific:
  - the pthread timer emulation (from the wasm port) clears `armed_ns` before
    `pthread_cond_timedwait()`, so a spurious wakeup with no re-arm drops the pending
    timer;
  - `lkl_mmap()`'s failure path unmaps `addr` rather than `ret`, and tests `ret != NULL`
    where `mmap()` returns `MAP_FAILED`. On macOS the hint semantics make that path
    reachable whenever `addr` is taken.

## Prior art

`HuanchuanTech/xlinuxfs` (GPL-2.0, App Store, 2026-07-08) ships an FSKit extension backed by
a **native Mach-O `liblkl.a`**, arm64 only, ext2/3/4 read-write and XFS/Btrfs read-only. Its
README says the archive is assembled by `scripts/build-liblkl.sh` "from the LKL tree's macOS
build objects" — an archive-of-objects design, matching what this experiment independently
arrived at. The public repo contains only README/LICENSE/PRIVACY; the build script, the
kernel patches, and `MACOS_PORT_NOTES.md` are unpublished. Requesting the GPL-2.0
Corresponding Source is cheap and would likely shortcut much of the remaining work.

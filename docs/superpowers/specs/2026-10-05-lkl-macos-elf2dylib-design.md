# LKL on macOS via ELF-to-dylib conversion

**Date:** 2026-10-05
**Status:** implemented and verified on an Apple Silicon Mac and an Intel Mac (2026-10-05)
**Scope:** `scripts/macho/` (new tools and build scripts), `scripts/oot_fs.sh` (ZFS arm64
gates, trimmed `--macho` series), `scripts/build_lkl.sh` (linux-arm64 flags),
`patches/linux/macho/` (host-side patches only), `docs/macos-macho-feasibility.md`
(marked superseded)

## Goal

Run LKL natively on macOS, on both Apple Silicon (arm64) and Intel (x86_64), as the
first layer of a native macOS backend for anyfs. This spec delivers the kernel as a
Mach-O dylib, the LKL host library for Darwin, and a smoke test. Porting anyfs core,
QEMU, the native addon and Electron to macOS is a separate follow-up.

The kernel keeps its standard Linux build: Kbuild, `arch/lkl/kernel/vmlinux.lds` and
`objcopy -G` stay unchanged. A build-time converter turns the linked ELF kernel image
into a Mach-O dylib.

## Context — why the previous approach is replaced

The 2026-07-31 experiment (`docs/macos-macho-feasibility.md`) compiled every kernel
object directly to Mach-O and shipped an archive of Mach-O objects. It reached a linked
dylib, but it dropped the linker script, and everything the script guaranteed had to be
rebuilt by hand. That cost 22 shim headers that override kernel macros, 7 kernel
patches, an `-order_file` for `sched_class`, and per-level initcall tables. Section
ordering and boundaries were emulated, and whether the emulation is right shows only at
runtime. The compile was also a replay of Kbuild's saved command lines outside Kbuild.

All three problems come from leaving the linker script behind. Keeping the ELF build
removes them.

### Evidence that the linked ELF kernel converts cleanly

`lkl.o` from the standard build, plus a small glue object, was linked with
`ld.lld -shared -Bsymbolic -z now -z max-page-size=16384 -z separate-loadable-segments
--no-undefined`:

| | x86_64 (`lkl-linux-amd64`) | arm64 (`linux-arm64` target) |
|---|---|---|
| `PT_LOAD` segments | 4 (R, RX, RELRO, RW), 16 KiB aligned | 4 (R, RX, RELRO, RW), 16 KiB aligned |
| dynamic relocations | 71857, all `R_X86_64_RELATIVE` | 74900, all `R_AARCH64_RELATIVE` |
| `TEXTREL` / undefined dynamic symbols | none / none | none / none |
| instructions using `x18`/`w18` | n/a | 0, built with `-ffixed-x18` |

The relocation counts are those of the final build. An earlier arm64 probe counted
74925.

`lkl.o` is already built with `-fPIC` (`arch/lkl/Makefile`). Its only external
references are `lkl_printf` and `lkl_bug`, plus `_GLOBAL_OFFSET_TABLE_`, which the linker
provides.

A layout probe fed the four arm64 segments to `ld64.lld` as `.incbin` sections, each
aligned to 16 KiB. Every section, including the zero-fill bss, landed at its ELF address
plus 0x4000, and `ld64.lld` emitted the export trie, `LC_UUID`, `LC_BUILD_VERSION` and an
ad-hoc code signature by itself.

### Prior art

- systemd's `tools/elf2efi.py` uses the same technique to make UEFI PE images. It links
  the ELF as a shared object so the only relocations left are `R_*_RELATIVE`, then turns
  each one into a PE base relocation. davmac314/elf2efi and go-coff/pectl do the same.
- Apple-Eloquence-ELF's `macho2elf.py` converts in the other direction. Instead of
  writing ELF itself, it generates assembly: `.incbin` of the original bytes, `.quad`
  for pointers that need fixing up, labels for symbols. A real linker then writes the
  file. elf2dylib does the reverse with `ld64.lld`.
- No existing general tool converts an ELF shared object into a Mach-O dylib. objconv is
  x86-only and works on object files; binutils `objcopy -O mach-o` has no arm64
  relocation writer.

## Decisions (from design review)

- **Convert at build time, not load at runtime.** An in-process ELF loader would need
  executable memory, and lldb could not see kernel symbols. A converted dylib is
  signable, debuggable and loads through dyld.
- **Let `ld64.lld` write the Mach-O.** elf2dylib only reads and checks the ELF, generates
  assembly and checks the result. No hand-written export trie, symbol-table ordering or
  code signature.
- **The dylib is an ordinary one.** It links libSystem even though it imports nothing,
  rather than relying on dyld accepting an image with no dependencies.
- **No variadic function crosses the ELF/Mach-O boundary.** On arm64, Darwin passes all
  variadic arguments on the stack while AAPCS64 passes them in registers.
- **Both architectures use the same pipeline and the same glue**, even though x86_64 uses
  the System V convention on both sides.
- **Deployment targets are macOS 11.0 on arm64 and 10.12 on x86_64**, defined once in
  `scripts/macho/macos_target.sh`. 11.0 is the first macOS on Apple Silicon. 10.12 is
  the first with `clock_gettime`/`CLOCK_MONOTONIC`, which the host library needs. The
  `preadv`/`pwritev` fallback in patch 08 (see Host library) is what lets x86_64 go
  below 11.0.
- **Distribution is self-use for now.** Ad-hoc signing is enough; Developer ID signing
  and notarization come later.
- **The previous experiment is archived in a local tag**, `exp/macho-object-port`, before
  its superseded parts are removed. The tag is not pushed.

## Invariants

1. The kernel half is a standard ELF build. Nothing under `~/linux` changes for it beyond
   what `scripts/oot_fs.sh` already stages.
2. elf2dylib refuses any input outside the accepted shape (see Components). It never
   writes a partial or best-effort dylib.
3. Every byte of every ELF segment reaches the dylib unchanged, at ELF address + Δ
   (Δ = 0x4000), except 8-byte `RELATIVE` slots, which hold `addend + Δ` before dyld
   applies the slide.
4. The dylib exports only the names in the export map. None of them is variadic.
5. On arm64, no kernel or glue instruction reads or writes `x18`.

## Architecture

```
kernel (per arch, standard Linux pipeline)
  gen_lkl_config.sh + build_lkl.sh  →  lkl.o          (vmlinux.lds, objcopy -G)
  lkl_elf_glue.c                    →  glue.o
  ld.lld -shared ...                →  lkl-kernel.so  (RELATIVE only, no imports)
        │
        │  elf2dylib.py (Linux, build time)
        ▼
  liblkl-kernel.dylib   (ad-hoc signed; llvm-lipo can merge arm64 + x86_64)
        ▲  ordinary dynamic linking
host (native Mach-O, zig cc on Linux)
  tools/lkl/lib host sources (+ patches 08/09) + lkl_macho_shim.c  →  liblkl-host.a
  lkl-macos-smoke                                                    (test program)
```

### Components

**ELF build flags (`scripts/build_lkl.sh`).** The `linux-arm64` target always gets
`KCFLAGS="-ffixed-x18 -mno-outline-atomics"`, appended after any exported `KCFLAGS` so
that they win. Darwin reserves `x18`, and the OS may zero it. GCC's outline atomics call
libgcc helpers that detect CPU features with `getauxval`, which macOS does not have.
Both flags are harmless on Linux arm64, so one arm64 kernel serves both systems.
`linux-amd64` is unchanged.

**ZFS arm64 gates (`scripts/oot_fs.sh`).** The arm64 kernel currently leaves 11 ZFS
symbols undefined: `fletcher_4_aarch64_neon_ops`, `vdev_raidz_aarch64_neon{,x2}_impl`,
`zfs_blake3_{compress_in_place,compress_xof,hash_many}_sse{2,41}`,
`zfs_sha{256,512}_block_armv7`. This is the known gap in ZFS's arm64 support, not a
Mach-O issue. New gates remove the references on `CONFIG_LKL`, using
`zfs_rewrite_line` like gates 4a-2 and 4b-2.

**ELF glue (`scripts/macho/lkl_elf_glue.c`).** Freestanding C, compiled with
`-O2 -fPIC -ffreestanding -fno-builtin -fno-stack-protector`, plus
`-ffixed-x18 -mno-outline-atomics` on arm64:

- `int lkl_printf(const char *fmt, ...)` and `void lkl_bug(const char *fmt, ...)`. The
  kernel calls these by name. They format with a built-in minimal formatter: the
  kernel's own `vsnprintf` is hidden by `objcopy -G`, and all 8 call sites use only
  `%s`. It supports `%s %d %i %u %x %X %p %c %%`, skips flags, width and precision, and
  accepts the `h hh l ll z t j` modifiers. On any other conversion it prints the rest
  of the format verbatim and reads no further argument. `lkl_printf` passes the text
  to the host's `print`. `lkl_bug` passes it to `print` and then calls `panic`, or
  traps if no panic hook is set.
- `void lkl_glue_set_host(void (*print)(const char *, int), void (*panic)(void))`.
- `int lkl_start_kernel_str(const char *cmdline)`, which calls
  `lkl_start_kernel("%s", cmdline)`. The variadic call stays inside ELF code.

**Kernel link (`scripts/macho/build_kernel_dylib.sh --arch=arm64|x86_64`).** Compiles the
glue with the target's ELF compiler, then runs
`ld.lld -shared -Bsymbolic -z now -z max-page-size=16384 -z separate-loadable-segments
--unique --no-undefined -soname liblkl-kernel.so lkl.o glue.o`, then elf2dylib.
`--universal` merges both architectures with `llvm-lipo -create`.

The link must keep `lkl.o`'s sections in input order. `vmlinux.lds` defines some markers
between output sections (`__init_begin`, `__init_end`, `_sdata`, `__end_rodata`, `_edata`,
`__start_ro_after_init`, `__bss_start`), so in `lkl.o` each one is relative to a
neighbouring section. All of `lkl.o`'s data sections, including `.rodata` (it holds
`.data.rel.ro`), are writable and not RELRO to lld. They share one rank, so they keep
the order in which lld creates their output sections. Without `--unique`, lld folds
`.data..percpu` and `.data..ro_after_init` into one `.data`. It creates that `.data` at
`.data..percpu`, ahead of `.rodata`, which gave `_sdata > _edata` and
`__start_rodata > __end_rodata`. The kernel still booted, but printed garbage rwdata
and rodata sizes, and `is_kernel_rodata()` was always false.
`--unique` gives every input section its own output section, and lld's stable sort by
segment rank then keeps input order within each segment. After the link, the script
requires start <= end for `_stext`/`_etext`, `_sinittext`/`_einittext`,
`__init_begin`/`__init_end`, `_sdata`/`_edata`, `__start_rodata`/`__end_rodata`,
`__bss_start`/`__bss_stop`, `__start_ro_after_init`/`__end_ro_after_init` and
`__bss_stop`/`_end`, and fails otherwise. One known gap remains: `__init_begin` is the end
of `.text`, so `[__init_begin, __init_end)` also spans the RELRO segment, about 35 KiB.
This is harmless because LKL's `free_initmem()` frees nothing.

**elf2dylib (`scripts/macho/elf2dylib.py`).** A generic tool; it knows nothing about
LKL.

```
elf2dylib.py --arch arm64|x86_64 --export ELFNAME=MACHONAME ... \
             --install-name @rpath/liblkl-kernel.dylib --min-os VERSION \
             --libsystem PATH -o OUT.dylib IN.so
```

`--min-os` and `--libsystem PATH` are required and have no default.
`build_kernel_dylib.sh` passes the arch's deployment target from
`scripts/macho/macos_target.sh` and zig's `libSystem.tbd`.

1. *Input checks.* Each failure aborts with a message naming the offending item.
   - `ET_DYN`, machine `EM_AARCH64` or `EM_X86_64` matching `--arch`.
   - Exactly the segment shape `ld.lld -z separate-loadable-segments -z now` produces:
     R, RX, RW (`PT_GNU_RELRO`), RW. Each starts on a 16 KiB boundary with
     `p_offset == p_vaddr`, and the first `PT_LOAD` starts at 0. R and RX have no
     zero-fill tail. `PT_GNU_RELRO` covers exactly the third `PT_LOAD`.
   - Every dynamic tag is on an allowlist, which rejects init arrays, `DT_RELR` and
     Android-packed relocations.
   - Dynamic relocations are only `R_AARCH64_RELATIVE` / `R_X86_64_RELATIVE`, and the
     `RELA` table lies inside the first `PT_LOAD`.
   - Every relocated slot lies in the file data, not the bss, of the RELRO or RW
     segment.
   - Every addend lies inside some segment's address range, including bss and
     one-past-the-end.
   - No undefined dynamic symbols.
   - Every `--export` name exists as a defined dynamic symbol, and no ELF or Mach-O
     name appears twice in `--export`.
   - arm64: `llvm-objdump -d` of the RX segment mentions no `x18`/`w18`.
2. *Assembly generation.* One section per segment, each `.p2align 14`:
   `__TEXT,__lkl_const` (R), `__TEXT,__lkl_text,regular,pure_instructions` (RX),
   `__DATA_CONST,__lkl_relro` (RELRO), `__DATA,__lkl_data` (RW file bytes), and
   `.zerofill __DATA,__lkl_bss` (RW zero-fill tail). Segment bytes are `.incbin`ed from
   files extracted from the ELF.
   - The `.incbin` is split only at `RELATIVE` slots, each of which becomes
     `.quad L<seg> + (addend - seg_vaddr)`.
   - Symbols are defined with `.set "<name>", L<seg> + offset`, which needs no split.
     Each `.symtab` function or object symbol becomes a local `_<name>`. A repeated
     name gets a `~<n>` suffix, because ELF names already use `.<n>` (`__func__.1`).
     AArch64 mapping symbols (`$x`, `$d`) are dropped.
   - Each `--export` becomes `.globl MACHONAME` plus its `.set`.
3. *Link.* `clang -target <arch>-apple-macos<min> -c`, then
   `ld64.lld -arch <arch> -platform_version macos <min> <min> -dylib
   -install_name <name> -no_fixup_chains -adhoc_codesign <zig>/lib/libc/darwin/libSystem.tbd`.
   `ld64.lld` produces the rebase opcodes, export trie, symbol table, `LC_UUID`,
   `LC_BUILD_VERSION` and the ad-hoc signature. For targets up to 10.13, such as
   x86_64's 10.12, it writes `LC_VERSION_MIN_MACOSX` instead of `LC_BUILD_VERSION`.
   `-adhoc_codesign` signs x86_64 too, which `ld64.lld` otherwise does only for arm64.
   The image imports nothing,
   but it links libSystem like any ordinary dylib, using the same `libSystem.tbd`
   stub that the host-library build takes from zig.
4. *Output checks.* Each failure deletes the output and aborts.
   - Each `__lkl_*` section's address equals the ELF segment's address + Δ. The sizes
     of `__lkl_const`, `__lkl_text` and `__lkl_relro` run to the next segment's
     address + Δ, with the padding checked to be zero; `__lkl_data` equals the RW
     segment's file size, and `__lkl_bss` matches its zero-fill tail. The bytes
     (dumped with `llvm-objcopy --dump-section`) equal the segment's bytes. The only
     exception is the `RELATIVE` slots, which must hold `addend + Δ`.
   - The set of rebase locations equals the set of `RELATIVE` slots + Δ.
   - Each exported symbol's address equals its ELF address + Δ.
   - The image's only dependent dylib is `/usr/lib/libSystem.B.dylib`. Its only
     undefined symbol is `dyld_stub_binder`, which `ld64.lld` adds to every
     classic-opcode image that links libSystem (verified with the layout probe).

**Export map.** Each exported ELF name `lkl_X` becomes the Mach-O symbol `_lklk_X`
(C name `lklk_X`), passed as `--export lkl_X=_lklk_X`. That covers the 8 non-variadic
APIs (`lkl_init`, `lkl_cleanup`, `lkl_syscall`, `lkl_sys_halt`, `lkl_is_running`,
`lkl_get_free_irq`, `lkl_put_irq`, `lkl_trigger_irq`) and the two glue entry points
(`lkl_glue_set_host`, `lkl_start_kernel_str`). `lkl_start_kernel`, `lkl_printf` and
`lkl_bug` are never exported. The list is defined once, as `KERNEL_EXPORTS` in
`scripts/macho/kernel_exports.sh`, which `build_kernel_dylib.sh` and `build_host_lib.sh`
source.

**Mach-O shim (`scripts/macho/lkl_macho_shim.c`).** Part of `liblkl-host.a`. It defines
the public LKL API under its usual names, so the host library and anyfs code are
unchanged:

- `lkl_init(ops)` calls `lklk_glue_set_host(ops->print, ops->panic)`, then
  `lklk_init(ops)`.
- `lkl_start_kernel(fmt, ...)` formats with Darwin `vsnprintf` into a 4096-byte buffer
  (`COMMAND_LINE_SIZE` in `arch/lkl/include/asm/setup.h`). It returns `-LKL_E2BIG` if the
  result does not fit, and otherwise calls `lklk_start_kernel_str`.
- The other 7 APIs forward directly.

**Host library (`scripts/macho/build_host_lib.sh --arch=arm64|x86_64`).** Compiles the
`tools/lkl/lib` host sources with `zig cc -target aarch64-macos.11.0.0` or
`x86_64-macos.10.12.0` (from `macos_target.sh`) and `-g -fno-strict-aliasing
-Werror=deprecated-declarations -Werror=unguarded-availability`, against the Darwin
`lkl_autoconf.h` profile (`scripts/macho/autoconf/lkl_autoconf.h`: VFIO, macvtap and FUSE
off), together with the Mach-O shim and the netdev stubs
(`scripts/macho/darwin-netdev-stubs.c`). The tap and raw netdevs need
`<linux/if_tun.h>`/`AF_PACKET`. The result is `liblkl-host.a`. Patches 08 (`posix-host.c`)
and 09 (`endian.h`) stay and are still applied by `oot_fs.sh stage --macho`. `zig cc`
links zig's own `libSystem.tbd`; `build_kernel_dylib.sh` finds the same file for
elf2dylib through the lib dir that `zig env` reports.

`preadv`/`pwritev` need macOS 11.0. When `__ENVIRONMENT_MAC_OS_X_VERSION_MIN_REQUIRED__`
is below 110000, patch 08 replaces them with a `pread`/`pwrite` loop over the iovecs
that returns what `preadv` would: the total byte count, stopping at the first short
transfer, or -1 with `errno` if nothing was transferred. `blk_request` then redoes a
short request with `do_rw`, as before. The x86_64 objects import neither function;
arm64 still calls both.

### ABI boundary audit

The boundary is the 8 exported APIs, the 40 function pointers in
`struct lkl_host_operations`, the callbacks the kernel hands to the host (thread entry,
timer, TLS destructor, `jmp_buf_set`), and the 3 variadic functions. The audit on
2026-10-05 found:

- Parameters are only `int`, `long`, `unsigned long[ long]`, `enum` and pointers.
  Nothing is narrower than 32 bits, which Darwin requires the caller to extend. No
  function takes more than 8 integer arguments, whose stack layout differs. No struct,
  float or union is passed by value.
- `jmp_buf_set`/`jmp_buf_longjmp` use Darwin `setjmp`/`longjmp` across kernel frames.
  This is safe because both conventions save the same callee-saved set (x19–x28, d8–d15,
  fp, lr) and the kernel never touches `x18`.
- `char` signedness differs (unsigned on Linux arm64, signed on Darwin) but only crosses
  the boundary behind pointers.
- `pci_ops` points to a `struct lkl_dev_pci_ops` table. It stays NULL on Darwin because
  VFIO is off; enabling VFIO would bring that table into the boundary.

Any future change to `lkl_host_operations` or the API must keep these properties. They
are recorded in a comment next to the export list in `scripts/macho/kernel_exports.sh`.

### Known issues

- Shared with the other hosts and deliberately not fixed here:
  - The pthread timer emulation in `posix-host.c` (wasm and Darwin) drops a pending
    timer on a spurious condvar wakeup.
  - `lkl_mmap()`'s failure path unmaps `addr` rather than `ret`. It is dormant here
    because MMU and KASAN are off.
- The init range in `lkl-kernel.so` also covers the RELRO gap, about 35 KiB (see Kernel
  link). This is harmless because `free_initmem()` is a no-op.

## Error handling

- elf2dylib and the build scripts fail closed: any failed check aborts the build and
  leaves no output file.
  - `build_kernel_dylib.sh --arch` deletes that arch's glue object, `.so` and dylib,
    and the universal dylib, before it builds, and deletes its outputs again if a step
    fails. `--universal` deletes the old universal dylib first, and its scratch files
    on exit.
  - `build_host_lib.sh` deletes the archive and its objects first and writes the
    archive through a temporary file.
  - `build_smoke.sh` deletes the bundle first, builds it in `smoke.tmp` and renames that
    to `smoke` only when it is complete.
  - A missing tool is named in the error, rather than ending the script silently under
    `set -e`. Each tool is looked up only by the step that needs it, so only
    `--universal` needs `llvm-lipo`. `libSystem.tbd` is found through the lib dir that
    `zig env` reports.
- The build scripts check what they depend on:
  - `build_host_lib.sh` checks patches 08 and 09 by reverse-applying them to
    `$LINUX_DIR` with `patch -R --dry-run`, not by reading `oot_fs.sh`'s log.
  - It refuses a `lkl_autoconf.h` in `$LINUX_DIR/tools/lkl/include`, which a quoted
    include would find before the Darwin profile. It also refuses a kernel whose
    generated `lkl_autoconf.h` defines `LKL_HOST_CONFIG_MMU` when the Darwin profile
    does not.
  - `lkl-kernel.so` may define no dynamic symbols outside the export map except
    `lkl_start_kernel`, `lkl_printf` and `lkl_bug`, so a new LKL API needs an ABI
    decision. `liblkl-host.a` must import exactly the 10 `_lklk_*` names.
- After `llvm-lipo`, `build_kernel_dylib.sh --universal` extracts each slice and requires
  it to be byte-identical to the per-arch dylib that passed the output checks.
- At runtime, dyld reports a missing or unsigned dylib at load time, before any kernel
  code runs. `lkl_start_kernel` reports an over-long command line instead of truncating
  it.

## Testing and verification

1. **elf2dylib unit tests** (`scripts/macho/test_elf2dylib.sh`, Linux, CI-friendly).
   - A synthetic shared library per architecture, containing function-pointer tables
     into code, rodata and data, bss, exported functions, and two `static` functions
     with the same name. It must convert and pass the output checks.
   - Each of these inputs must be rejected, among others: an undefined import; an
     undefined dynamic symbol that nothing relocates; a non-PIC object (text
     relocation); a 4 KiB page-size link; RELR, Android-packed relocations and init
     arrays; arm64 inline assembly that uses `x18`; a tool named in the environment
     that does not exist.
2. **Real kernels** (Linux).
   - Both architectures convert. The rebase counts equal the `RELATIVE` counts (74900
     on arm64, 71857 on x86_64), and 10 symbols are exported.
   - `scripts/macho/test_kernel_so.sh` (CI-friendly) boots the exact `lkl-kernel.so`
     that is converted, x86_64 natively and arm64 under `qemu-aarch64-static`, and runs
     the smoke program (item 4) against it through `lkl_macho_shim.c` and the stock
     Linux host library, on a scratch copy of the ext4 image. It requires
     `PASS (21 checks)` and a `Memory:` boot line with rwdata, rodata and init each
     below 1 GiB, which a link without `--unique` fails. The Darwin host paths of patch
     08 run only on a Mac.
3. **arm64 ELF kernel on Linux arm64.** LKL's own `tools/lkl/tests` ran under
   `qemu-aarch64` user mode on 2026-10-05: boot 35/35 and disk 10/10. The anyfs Linux
   tests have not been run on arm64. This proves the new arm64 flags and ZFS gates, so
   the Mac only has to test the conversion and the ABI boundary.
4. **macOS smoke test** (`scripts/macho/smoke/`, run by hand on an Apple Silicon Mac and
   an Intel Mac).
   - `build_smoke.sh` cross-builds `lkl-macos-smoke` on Linux and bundles it with
     `liblkl-kernel.dylib` and `smoke-ext4.img`, a copy of `tests/images/ext4.img`.
   - It boots the kernel, adds the disk, mounts it read-write, creates, writes, fsyncs
     and reads back a file, unmounts and remounts (dropping the page cache) and reads
     the file again from the block device, lists the directory, sleeps 100 ms in the
     kernel and checks the elapsed time, unmounts and halts. That is 21 checks, ending
     in `PASS (21 checks)`. A 120 s watchdog turns a hang into a failure.
   - `codesign -v` must pass on both binaries on arm64, and on the dylib on x86_64.
   - The run commands are documented next to the test.
   - Passed 21/21 on Apple Silicon (macOS 27.0.1, 26A434; 103 ms sleep) and Intel (macOS
     15.0, 24A335; 102 ms sleep), with the build from before the `--unique` fix. A rerun
     is pending.
5. **No regressions elsewhere.**
   - Patches 08/09 are `__APPLE__`-guarded. The glibc preprocessed output of
     `posix-host.c` is unchanged apart from `__LINE__` (verified 2026-10-05, and again
     after the `preadv`/`pwritev` fallback and the `mmap` comment fix).
   - After adding the ZFS arm64 gates, a `linux-amd64` rebuild must produce the same
     `lkl.o` (verified by hash in f5e0f51).

## Phasing

0. **Archive and retire.**
   - Commit the experiment's source to an annotated local tag `exp/macho-object-port`.
     It contains `patches/linux/macho/` 01–09, `scripts/macho/`, the current
     `scripts/oot_fs.sh`, `docs/macos-macho-feasibility.md`, and the harness sources from
     `.tmp/macho-exp/` (`sweep.py`, `macho-support.c`, `probe.c`, `shim-none/`, the host
     stubs and autoconf, and the result lists).
   - The commit is built with a temporary index and has the main HEAD as parent. It does
     not move `main`, the index or the working tree, and is never pushed.
   - Then remove the superseded parts:
     - `scripts/macho/shim/`;
     - patches 01–07;
     - `sched_class.order` and `check_sched_class.sh`;
     - the two Mach-O-only ZFS gates (the `setjmp_aarch64.S` rewrite and the `ldo.c` asm
       labels), restoring those two files in `~/oot-fs/zfs` with `git checkout`.
   - Mark `docs/macos-macho-feasibility.md` as superseded, pointing to this spec.
1. **elf2dylib** with its unit tests.
2. **arm64 ELF kernel**: the `build_lkl.sh` flags and the ZFS arm64 gates. Validate on
   Linux arm64.
3. **Kernel dylib**: ELF glue and `build_kernel_dylib.sh`. Convert both architectures.
4. **Darwin host library**: `build_host_lib.sh`, the Mach-O shim, and the smoke program.
5. **macOS verification** on the user's two Macs.

## Out of scope

- anyfs core, QEMU, the native addon and Electron on macOS. That is the next
  sub-project.
- Developer ID signing, notarization and the Mac App Store.
- Chained fixups, DWARF debug info for the kernel, `__unwind_info`.
- Running macOS tests in CI.

## Acceptance criteria

- `build_kernel_dylib.sh --universal` produces a signed, universal
  `liblkl-kernel.dylib` from the standard ELF kernels, with every elf2dylib check
  passing. **Met.**
- `test_elf2dylib.sh` passes, including every rejection case. **Met.**
- The arm64 ELF kernel passes the LKL boot test on Linux arm64. **Met** (under
  `qemu-aarch64`).
- `lkl-macos-smoke` passes on an Apple Silicon Mac and an Intel Mac. **Met.**
- The tag `exp/macho-object-port` exists locally. `scripts/macho/shim/` and patches 01–07
  are gone from the working tree. The `linux-amd64` `lkl.o` is unchanged. **Met.**

## Implementation notes (2026-10-05)

- **arm64 ELF kernel on Linux arm64** (qemu-aarch64 10.0.13): LKL `tests/boot` 35 ok,
  `tests/disk` on ext4 10 ok, 0 failed.
- **Conversion:** arm64 74900 rebases, x86_64 71857; 10 exports each; universal dylib built.
  `scripts/macho/test_kernel_so.sh` boots the exact `lkl-kernel.so` on Linux (x86_64
  native, arm64 under qemu): 21 checks each, with a sane `Memory:` line.
- **macOS smoke**, 21 checks, then the README's 10-run loop, all PASS:

  | Machine | macOS | Deployment target | `Memory:` | 100 ms sleep |
  |---|---|---|---|---|
  | Apple Silicon | 27.0.1 (26A434) | 11.0 | 2835K rwdata, 4179K rodata, 273K init | 100 ms |
  | Intel | 15.0 (24A335) | 10.12 (pread/pwrite fallback) | 2864K rwdata, 4260K rodata, 265K init | 101 ms |

- **Deviations from the design as first written**, all reflected in the sections above:
  - x86_64 targets macOS 10.12, not 11.0, with a preadv/pwritev fallback in patch 08;
    targets live in `scripts/macho/macos_target.sh`.
  - elf2dylib pads each section exactly to the next segment (ld64.lld aligns x86_64
    segments to 4 KiB only, and llvm-objdump decodes rebases from a segment's first
    section), allowlists dynamic tags, and installs its output atomically.
  - The kernel link adds `--unique`: without it, lld merged lkl.o's `.data.*` sections
    into one output section ahead of `.rodata`, which inverted `_sdata`/`_edata` and
    `__start_rodata`/`__end_rodata` (the first Mac runs printed an absurd `Memory:`
    line). `build_kernel_dylib.sh` now checks the boundary pairs.
  - The glue formatter handles flags, width, precision and the `z t j h hh` modifiers,
    so an unsupported conversion can no longer desynchronize its arguments.


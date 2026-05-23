#!/bin/bash
# Stage out-of-tree filesystem drivers (and wasm-only kernel patches)
# into $LINUX_DIR. Reversible — `unstage` puts the kernel tree back to
# pristine.
#
# Subcommands:
#   fetch [--update]                  clone or refresh ~/oot-fs/{zfs,apfs,ntfsplus}
#   stage [--wasm] [--targets=LIST]   apply OOT symlinks + Kconfig/Makefile
#                                     hooks. With --wasm, also apply
#                                     patches/linux/wasm/series against $LINUX_DIR.
#   unstage [--wasm]                  reverse everything stage applied.
#   status                            show currently staged drivers + patches.
#
# Layout assumptions:
#   $LINUX_DIR              kernel tree (default ~/linux)
#   $OOT_DIR                ~/oot-fs/ — OOT FS git checkouts
#   $REPO_DIR/patches/linux/wasm/series — quilt-style series file
#   $REPO_DIR/scripts/oot_fs/      — per-driver helper data (apfs.Kconfig.in, etc.)
#
# Anything we add to ~/linux is wrapped in marker blocks so unstage can
# remove them deterministically:
#
#   # === BEGIN anyfs-reader OOT ===
#   ... added lines ...
#   # === END anyfs-reader OOT ===
#
# Patch application is tracked in $OOT_DIR/.applied.<target> so re-staging
# is idempotent and unstage knows exactly what to roll back.
set -e

REPO_DIR="$(cd "$(dirname "$0")/.." && pwd)"
LINUX_DIR="${LINUX_DIR:-$HOME/linux}"
OOT_DIR="${OOT_DIR:-$HOME/oot-fs}"

BEGIN_MARK="# === BEGIN anyfs-reader OOT ==="
END_MARK="# === END anyfs-reader OOT ==="

ZFS_REPO="https://github.com/openzfs/zfs"
APFS_REPO="https://github.com/linux-apfs/linux-apfs-rw"
NTFSPLUS_REPO="https://github.com/namjaejeon/linux-ntfs"

die()  { echo "oot_fs: $*" >&2; exit 1; }
log()  { echo "oot_fs: $*"; }

# ── helpers ─────────────────────────────────────────────────────────────────

ensure_linux() {
    [[ -d "$LINUX_DIR/fs" ]] || die "no fs/ under LINUX_DIR=$LINUX_DIR"
}

# Strip our marker block (if present) from a file.
strip_marker_block() {
    local file="$1"
    [[ -f "$file" ]] || return 0
    if grep -qF "$BEGIN_MARK" "$file"; then
        local tmp
        tmp="$(mktemp)"
        awk -v b="$BEGIN_MARK" -v e="$END_MARK" '
            $0==b {skip=1; next}
            skip && $0==e {skip=0; next}
            !skip {print}
        ' "$file" > "$tmp"
        mv "$tmp" "$file"
    fi
}

# Append a marker block to a file (after stripping any existing one).
append_marker_block() {
    local file="$1"; shift
    strip_marker_block "$file"
    {
        echo "$BEGIN_MARK"
        printf '%s\n' "$@"
        echo "$END_MARK"
    } >> "$file"
}

# Apply patches/linux/wasm/series to $LINUX_DIR. Records each applied patch
# in $OOT_DIR/.applied.wasm so unstage --wasm can reverse exactly the same set.
apply_wasm_patches() {
    local series="$REPO_DIR/patches/linux/wasm/series"
    [[ -f "$series" ]] || die "no patch series at $series"

    mkdir -p "$OOT_DIR"
    local applied_log="$OOT_DIR/.applied.wasm"
    : > "$applied_log.new"

    while IFS= read -r p; do
        [[ -z "$p" || "$p" == \#* ]] && continue
        local patch_file="$REPO_DIR/patches/linux/wasm/$p"
        [[ -f "$patch_file" ]] || die "missing $patch_file"

        # Idempotent — if a forward dry-run fails, but a reverse dry-run
        # succeeds, treat the patch as already applied.
        if (cd "$LINUX_DIR" && patch -p1 --dry-run --silent < "$patch_file") >/dev/null 2>&1; then
            (cd "$LINUX_DIR" && patch -p1 --silent < "$patch_file") || die "apply $p failed"
            log "applied wasm patch: $p"
        elif (cd "$LINUX_DIR" && patch -p1 -R --dry-run --silent < "$patch_file") >/dev/null 2>&1; then
            log "wasm patch already applied: $p"
        else
            die "wasm patch $p neither applies forward nor is already applied"
        fi
        echo "$p" >> "$applied_log.new"
    done < "$series"

    mv "$applied_log.new" "$applied_log"
}

revert_wasm_patches() {
    local applied_log="$OOT_DIR/.applied.wasm"
    [[ -f "$applied_log" ]] || { log "no wasm patches recorded"; return 0; }

    # Reverse-apply in reverse order.
    tac "$applied_log" | while IFS= read -r p; do
        [[ -z "$p" ]] && continue
        local patch_file="$REPO_DIR/patches/linux/wasm/$p"
        [[ -f "$patch_file" ]] || { log "WARN: missing $patch_file for unstage"; continue; }

        if (cd "$LINUX_DIR" && patch -p1 -R --dry-run --silent < "$patch_file") >/dev/null 2>&1; then
            (cd "$LINUX_DIR" && patch -p1 -R --silent < "$patch_file") || die "revert $p failed"
            log "reverted wasm patch: $p"
        elif (cd "$LINUX_DIR" && patch -p1 --dry-run --silent < "$patch_file") >/dev/null 2>&1; then
            log "wasm patch already reverted: $p"
        else
            log "WARN: $p did not reverse cleanly (kernel tree changed?)"
        fi
    done

    rm -f "$applied_log"
}

# ── subcommands ─────────────────────────────────────────────────────────────

cmd_fetch() {
    local update=0
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --update) update=1; shift ;;
            *) die "fetch: unknown arg $1" ;;
        esac
    done
    mkdir -p "$OOT_DIR"
    fetch_one() {
        local name="$1" url="$2"
        local dir="$OOT_DIR/$name"
        if [[ ! -d "$dir/.git" ]]; then
            log "cloning $name from $url"
            git clone --depth=1 "$url" "$dir"
        elif [[ $update -eq 1 ]]; then
            log "pulling $name"
            (cd "$dir" && git pull --ff-only)
        else
            log "$name present at $dir (use --update to refresh)"
        fi
    }
    fetch_one ntfsplus "$NTFSPLUS_REPO"
    fetch_one apfs     "$APFS_REPO"
    fetch_one zfs      "$ZFS_REPO"
}

# Stage out-of-tree FS drivers. Phase-aware — for the initial wasm/XFS
# phase 0 work, only the wasm patch layer matters; the OOT symlinking
# branches below are no-ops until ~/oot-fs/{ntfsplus,apfs,zfs} exist.
cmd_stage() {
    ensure_linux
    local want_wasm=0
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --wasm) want_wasm=1; shift ;;
            *) die "stage: unknown arg $1" ;;
        esac
    done

    # OOT symlinks (phases 1-3) ────────────────────────────────────────────
    stage_ntfsplus
    stage_apfs
    stage_zfs

    # fs/Kconfig + fs/Makefile marker blocks (rebuilt each time) ────────────
    rebuild_fs_kconfig_block
    rebuild_fs_makefile_block

    # wasm-only kernel patches ─────────────────────────────────────────────
    if [[ $want_wasm -eq 1 ]]; then
        apply_wasm_patches
    fi

    log "stage complete"
}

cmd_unstage() {
    ensure_linux
    local want_wasm=0
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --wasm) want_wasm=1; shift ;;
            *) die "unstage: unknown arg $1" ;;
        esac
    done

    if [[ $want_wasm -eq 1 ]]; then
        revert_wasm_patches
    fi

    strip_marker_block "$LINUX_DIR/fs/Kconfig"
    strip_marker_block "$LINUX_DIR/fs/Makefile"

    unstage_ntfsplus
    unstage_apfs
    unstage_zfs

    log "unstage complete"
}

cmd_status() {
    echo "LINUX_DIR=$LINUX_DIR"
    echo "OOT_DIR=$OOT_DIR"
    echo
    for d in ntfsplus apfs zfs; do
        if [[ -e "$OOT_DIR/$d" ]]; then
            local pin=""
            [[ -d "$OOT_DIR/$d/.git" ]] && pin=$(cd "$OOT_DIR/$d" && git rev-parse --short HEAD 2>/dev/null || echo "?")
            echo "  oot/$d  ${pin:+@$pin}"
        else
            echo "  oot/$d  (not fetched)"
        fi
    done
    echo
    echo "  staged into $LINUX_DIR:"
    for target in fs/ntfsplus fs/apfs fs/zfs include/zfs; do
        if [[ -L "$LINUX_DIR/$target" ]]; then
            echo "    $target -> $(readlink "$LINUX_DIR/$target")"
        elif [[ -e "$LINUX_DIR/$target" ]]; then
            echo "    $target (exists, NOT a symlink — unexpected)"
        fi
    done
    echo
    if [[ -f "$OOT_DIR/.applied.wasm" ]]; then
        echo "  wasm patches applied:"
        sed 's/^/    /' "$OOT_DIR/.applied.wasm"
    else
        echo "  wasm patches: none"
    fi
}

# ── per-driver staging (phases 1-3 fill these in) ───────────────────────────
# All four functions are intentionally no-ops when the corresponding
# ~/oot-fs/<name> directory doesn't exist yet. That lets phase 0 run
# `oot_fs.sh stage --wasm` without forcing the user to clone repos that
# aren't needed until phase 1+.

stage_ntfsplus() {
    local src="$OOT_DIR/ntfsplus"
    [[ -d "$src" ]] || return 0

    # The OOT driver uses CONFIG_NTFS_FS, which collides with the in-tree
    # backward-compat shim at fs/ntfs3/Kconfig:50 that does `select NTFS3_FS`.
    # Rename the driver's Kconfig symbols to CONFIG_NTFSPLUS_* in place so
    # both drivers' symbols are disjoint and we can keep NTFS3 cleanly off.
    #
    # Idempotent — repeating these seds against an already-renamed tree is a
    # no-op (the source pattern no longer matches anything).
    if grep -q '\bNTFS_FS\b' "$src/Kconfig" 2>/dev/null; then
        log "renaming NTFS PLUS Kconfig symbols (NTFS_* -> NTFSPLUS_*)"
        # Order matters: do POSIX_ACL first so the bare NTFS_FS rename
        # doesn't accidentally chop into NTFS_FS_POSIX_ACL.
        sed -i \
            -e 's/\bNTFS_FS_POSIX_ACL\b/NTFSPLUS_FS_POSIX_ACL/g' \
            -e 's/\bNTFS_DEBUG\b/NTFSPLUS_DEBUG/g' \
            -e 's/\bNTFS_FS\b/NTFSPLUS_FS/g' \
            "$src/Kconfig"
        # Makefile: rename the Kconfig refs only. Leave the
        # `-DCONFIG_NTFS_FS_POSIX_ACL=1` C-side macro alone — the driver's
        # *.c files still test that exact symbol.
        sed -i \
            -e 's/CONFIG_NTFS_DEBUG/CONFIG_NTFSPLUS_DEBUG/g' \
            -e 's/CONFIG_NTFS_FS\b/CONFIG_NTFSPLUS_FS/g' \
            "$src/Makefile"
    fi

    # Symlink into the kernel tree (replace any stale entry first).
    rm -f "$LINUX_DIR/fs/ntfsplus"
    ln -s "$src" "$LINUX_DIR/fs/ntfsplus"
    log "staged fs/ntfsplus -> $src"
}
unstage_ntfsplus() {
    rm -f "$LINUX_DIR/fs/ntfsplus"
}

stage_apfs() {
    local src="$OOT_DIR/apfs"
    [[ -d "$src" ]] || return 0

    # 1. Generate version.h (super.c includes it; the upstream Makefile
    #    runs genver.sh in its default target — we skip that target so do it
    #    here). Idempotent: re-running just refreshes the header.
    (cd "$src" && ./genver.sh) >/dev/null 2>&1 || \
        echo '#define GIT_COMMIT "anyfs-reader-staged"' > "$src/version.h"

    # 2. Drop in our hand-authored Kconfig (no upstream Kconfig ships).
    cp "$REPO_DIR/scripts/oot_fs/apfs.Kconfig" "$src/Kconfig"

    # 3. Replace the upstream `obj-m = apfs.o` Makefile with a clean
    #    Kbuild fragment driven by CONFIG_APFS_FS. The upstream file
    #    mixes OOT-build helpers (default/install/clean targets) with
    #    Kbuild syntax — fine for `make -C`, but we want a pure in-tree
    #    module here.
    cp "$REPO_DIR/scripts/oot_fs/apfs.Makefile" "$src/Makefile"

    # 4. Symlink into the kernel tree (replace any stale entry first).
    rm -f "$LINUX_DIR/fs/apfs"
    ln -s "$src" "$LINUX_DIR/fs/apfs"
    log "staged fs/apfs -> $src"
}
unstage_apfs() {
    rm -f "$LINUX_DIR/fs/apfs"
}

stage_zfs() {
    local src="$OOT_DIR/zfs"
    [[ -d "$src" ]] || return 0

    # 1. Run ZFS configure if zfs_config.h hasn't been produced yet. ZFS's
    #    configure expects a "normal" prepared kernel build dir (it tries
    #    `make modules` against a conftest object, which LKL build dirs can't
    #    satisfy because they're ARCH=lkl). So we prepare a dedicated x86_64
    #    build dir under $OOT_DIR/.zfs-configure-build purely for this probe.
    if [[ ! -f "$src/zfs_config.h" ]]; then
        local cfg_build="$OOT_DIR/.zfs-configure-build"
        if [[ ! -f "$cfg_build/include/generated/utsrelease.h" ]]; then
            log "preparing $cfg_build (one-time, ~30s) for ZFS configure"
            mkdir -p "$cfg_build"
            make -C "$LINUX_DIR" O="$cfg_build" defconfig >/dev/null 2>&1
            "$LINUX_DIR/scripts/config" --file "$cfg_build/.config" -e MODULES
            make -C "$LINUX_DIR" O="$cfg_build" olddefconfig >/dev/null 2>&1
            make -C "$LINUX_DIR" O="$cfg_build" prepare >/dev/null 2>&1
        fi
        if [[ ! -f "$src/configure" ]]; then
            log "ZFS autogen.sh (one-time)"
            (cd "$src" && bash autogen.sh) >/dev/null 2>&1 || die "ZFS autogen failed"
        fi
        log "ZFS configure (one-time, several minutes)"
        (cd "$src" && ./configure \
            --with-linux="$LINUX_DIR" \
            --with-linux-obj="$cfg_build" \
            --enable-linux-builtin \
            --with-config=kernel) >/dev/null 2>&1 \
            || die "ZFS configure failed (see $src/config.log)"
    fi
    if [[ ! -f "$src/include/zfs_gitrev.h" ]]; then
        log "ZFS make gitrev"
        (cd "$src" && make gitrev) >/dev/null 2>&1 || die "ZFS make gitrev failed"
    fi

    # 2. ZFS Kbuild includes $(zfs_include)/zfs_config.h, where zfs_include
    #    resolves to $(srctree)/include/zfs (which we symlink to OOT include/).
    #    The generated zfs_config.h lives at the OOT top level — copy it
    #    into include/ so the symlinked path picks it up.
    cp -f "$src/zfs_config.h" "$src/include/zfs_config.h"
    # Append LKL carve-outs. zfs_config.h was generated against a "normal"
    # x86_64 kernel (the .zfs-configure-build prep dir) so it sets
    # HAVE_KERNEL_OBJTOOL/HAVE_KERNEL_OBJTOOL_HEADER — both pull in
    # <asm/frame.h>, which doesn't exist under arch/lkl/. Disable on
    # LKL builds via autoconf-provided CONFIG_LKL.
    cat >> "$src/include/zfs_config.h" <<'EOF'

/* anyfs-reader LKL carve-out — appended by scripts/oot_fs.sh stage_zfs.
 *
 * zfs_config.h is generated against a "normal" x86_64 kernel build (see
 * the .zfs-configure-build prep dir). It therefore sets several flags
 * that assume the host's x86 toolchain and arch headers — but ARCH=lkl
 * has no arch/lkl/include/asm/{cpufeature.h,fpu/api.h,frame.h} etc.
 *
 * On CONFIG_LKL builds:
 *   - Disable HAVE_KERNEL_OBJTOOL{,_HEADER}: sys/asm_linkage.h / sys/frame.h
 *     pull in <asm/frame.h> when this is set.
 *   - Disable HAVE_KERNEL_<SIMD>: these gate simd_x86.h's
 *     zfs_*_available() declarations, which the C source files in
 *     icp/algs/{aes,blake3,sha2,modes} reference. simd_x86.h itself is
 *     already gated off for LKL in include/os/linux/kernel/linux/simd.h.
 *   - Disable HAVE_KERNEL_FPU{,_API_HEADER}: paired with the above.
 *   - Disable HAVE_VFS_MIGRATE_FOLIO: LKL builds without CONFIG_MIGRATION
 *     (its Kconfig depends on NUMA/COMPACTION/CMA, none of which LKL
 *     selects), so the migrate_folio symbol referenced by zpl_file.c is
 *     not declared.
 */
#if defined(CONFIG_LKL)
# undef HAVE_KERNEL_OBJTOOL
# undef HAVE_KERNEL_OBJTOOL_HEADER
# undef HAVE_KERNEL_AES
# undef HAVE_KERNEL_AVX
# undef HAVE_KERNEL_AVX2
# undef HAVE_KERNEL_AVX512BW
# undef HAVE_KERNEL_AVX512F
# undef HAVE_KERNEL_AVX512VL
# undef HAVE_KERNEL_FPU
# undef HAVE_KERNEL_FPU_API_HEADER
# undef HAVE_KERNEL_MOVBE
# undef HAVE_KERNEL_PCLMULQDQ
# undef HAVE_KERNEL_SHA512EXT
# undef HAVE_KERNEL_SSE2
# undef HAVE_KERNEL_SSE4_1
# undef HAVE_KERNEL_SSSE3
# undef HAVE_KERNEL_VAES
# undef HAVE_KERNEL_VPCLMULQDQ
# undef HAVE_VFS_MIGRATE_FOLIO
#endif
EOF

    # 3. Generate the Kconfig stanza (copy-builtin writes this inline; we
    #    write it into module/ so the symlink target has it). Match the
    #    upstream copy-builtin heredoc, plus we drop the
    #    `depends on EFI_PARTITION` line — LKL builds EFI_PARTITION=y too,
    #    but stating it explicitly here lets us drop the dep if we ever
    #    need to enable ZFS on a config without GPT support.
    cat > "$src/module/Kconfig" <<'EOF'
config ZFS
	tristate "ZFS filesystem support"
	depends on BLOCK
	select ZLIB_INFLATE
	select ZLIB_DEFLATE
	help
	  The ZFS filesystem from the OpenZFS project.
	  See https://github.com/openzfs/zfs
EOF

    # 4. LKL-on-x86 carry: ZFS's os/linux/kernel/linux/simd.h dispatches on
    #    `__x86` (set by isa_defs.h from __x86_64__) and pulls in
    #    simd_x86.h, which #include's <asm/cpufeature.h> and <asm/fpu/api.h>.
    #    LKL has no arch/lkl/include/asm/cpufeature.h — building against
    #    LKL on an x86_64 host therefore fails. Gate the x86 branch behind
    #    !CONFIG_LKL so LKL falls through to the SIMD-disabled stub.
    #    Idempotent: only patch if CONFIG_LKL isn't already mentioned.
    local simdh="$src/include/os/linux/kernel/linux/simd.h"
    if [[ -f "$simdh" ]] && ! grep -q 'CONFIG_LKL' "$simdh"; then
        sed -i 's|^#if defined(__x86)$|#if defined(__x86) \&\& !defined(CONFIG_LKL)|' "$simdh"
        log "patched ZFS simd.h to skip x86 SIMD on CONFIG_LKL builds"
    fi

    # 4b. simd_stat.c references every zfs_*_available() declared in
    #     simd_x86.h directly from an `#if defined(__x86_64__) || defined(__i386__)`
    #     block. Now that simd_x86.h is excluded on LKL, those references
    #     become implicit-decl errors. Gate the x86 block behind !CONFIG_LKL
    #     too — same shape as the simd.h patch above. Idempotent.
    local simdstat="$src/module/zcommon/simd_stat.c"
    if [[ -f "$simdstat" ]] && ! grep -q 'CONFIG_LKL' "$simdstat"; then
        sed -i 's@^#if defined(__x86_64__) || defined(__i386__)$@#if (defined(__x86_64__) || defined(__i386__)) \&\& !defined(CONFIG_LKL)@' "$simdstat"
        log "patched ZFS simd_stat.c to skip x86 SIMD-stat on CONFIG_LKL builds"
    fi

    # 4c. ICP C sources reference x86_64 ASM symbols (aes_x86_64_impl,
    #     zfs_sha{256,512}_transform_x64, etc.) gated on
    #     `#if defined(__x86_64)`. The matching .S files are gated on
    #     CONFIG_X86_64 in module/Kbuild, which is NOT set under ARCH=lkl
    #     — so the C references become undefined symbols at link time.
    #
    #     `__x86_64` (no trailing `__`) and `__amd64` are *compiler builtins*
    #     on x86_64 hosts (not just `__x86_64__`), so gating isa_defs.h's
    #     internal #define of those names is insufficient. Splice an
    #     `#undef` prologue right after the header guard so any TU that
    #     pulls in isa_defs.h drops the builtins for the remainder of the
    #     file. _LP64 is still set by the x86_64 branch below.
    #
    #     Also force-define `__linux__` for ZFS source under CONFIG_LKL. ZFS
    #     uses `#if defined(_KERNEL) && defined(__linux__)` to pick the
    #     Linux-kernel code paths (HAVE_SIMD reads HAVE_KERNEL_* instead of
    #     HAVE_TOOLCHAIN_*; zfs_file.h gets `typedef struct file zfs_file_t`).
    #     mingw64-cross doesn't define __linux__ — without this, mingw builds
    #     fall through to `#error "unknown OS"` and pull in AVX paths that
    #     reference undefined `zfs_*_available()` stubs. Same lever the kernel
    #     itself implicitly uses: we are building Linux kernel code, the host
    #     OS the binary later runs on is irrelevant for source selection.
    #
    #     Idempotent — guarded by a one-shot marker comment.
    local isadefs="$src/include/os/linux/spl/sys/isa_defs.h"
    if [[ -f "$isadefs" ]] && ! grep -q 'anyfs-reader LKL prologue' "$isadefs"; then
        awk '
            /^#define[[:space:]]+_SPL_ISA_DEFS_H/ && !done {
                print
                print ""
                print "/* anyfs-reader LKL prologue: undo x86 compiler builtins so ZFS C"
                print " * source `#if defined(__x86_64)` checks dont reference x86_64 ASM"
                print " * symbols whose .S files are gated on CONFIG_X86_64 (unset on LKL),"
                print " * and force `__linux__` so mingw64-cross picks the Linux kernel paths"
                print " * (HAVE_SIMD -> HAVE_KERNEL_*, zfs_file_t -> struct file). */"
                print "#if defined(CONFIG_LKL)"
                print "# undef __x86_64"
                print "# undef __amd64"
                print "# undef __x86"
                print "# undef __i386"
                print "# ifndef __linux__"
                print "#  define __linux__ 1"
                print "# endif"
                print "#endif"
                done = 1
                next
            }
            { print }
        ' "$isadefs" > "$isadefs.new" && mv "$isadefs.new" "$isadefs"
        log "patched ZFS isa_defs.h to undef x86 builtins + force __linux__ on CONFIG_LKL builds"
    fi

    # 4b. Override ENTRY_ALIGN/SET_SIZE/ENTRY/... in ia32 asm_linkage.h on
    #     non-ELF assemblers (mingw PE/COFF). The original macros emit
    #     `.type x, @function` and `.size x, .-x` unconditionally — both
    #     ELF-only pseudo-ops. The mingw assembler rejects them outright
    #     when building fs/zfs/lua/setjmp/setjmp_x86_64.S. The override
    #     drops those two directives but keeps `.text`/`.balign`/`.globl x:`
    #     so the symbol still gets emitted at the right place. Functionally
    #     equivalent — `.type @function` and `.size` are diagnostic-only
    #     (objdump readability; ld doesn't need them on PE). Idempotent.
    local asml="$src/include/os/linux/spl/sys/ia32/asm_linkage.h"
    if [[ -f "$asml" ]] && ! grep -q 'anyfs-reader PE/COFF override' "$asml"; then
        awk '
            /^#endif[[:space:]]+\/\*[[:space:]]+_ASM[[:space:]]+\*\// && !done {
                print "/* anyfs-reader PE/COFF override: drop ELF-only .type/.size pseudo-ops"
                print " * for mingw cross-asm. .S files include this via the standard ZFS path. */"
                print "#if defined(_ASM) && !defined(__ELF__)"
                print "#undef  ENTRY"
                print "#define ENTRY(x) \\"
                print "        .text; \\"
                print "        .balign ASM_ENTRY_ALIGN; \\"
                print "        .globl  x; \\"
                print "x:      MCOUNT(x)"
                print "#undef  ENTRY_NP"
                print "#define ENTRY_NP(x) \\"
                print "        .text; \\"
                print "        .balign ASM_ENTRY_ALIGN; \\"
                print "        .globl  x; \\"
                print "x:"
                print "#undef  ENTRY_ALIGN"
                print "#define ENTRY_ALIGN(x, a) \\"
                print "        .text; \\"
                print "        .balign a; \\"
                print "        .globl  x; \\"
                print "x:"
                print "#undef  FUNCTION"
                print "#define FUNCTION(x) \\"
                print "x:"
                print "#undef  ENTRY2"
                print "#define ENTRY2(x, y) \\"
                print "        .text; \\"
                print "        .balign ASM_ENTRY_ALIGN; \\"
                print "        .globl  x, y; \\"
                print "x:; \\"
                print "y:      MCOUNT(x)"
                print "#undef  ENTRY_NP2"
                print "#define ENTRY_NP2(x, y) \\"
                print "        .text; \\"
                print "        .balign ASM_ENTRY_ALIGN; \\"
                print "        .globl  x, y; \\"
                print "x:; \\"
                print "y:"
                print "#undef  SET_SIZE"
                print "#define SET_SIZE(x)"
                print "#undef  SET_OBJ"
                print "#define SET_OBJ(x)"
                print "#endif /* _ASM && !__ELF__ */"
                print ""
                done = 1
            }
            { print }
        ' "$asml" > "$asml.new" && mv "$asml.new" "$asml"
        log "patched ZFS asm_linkage.h to drop ELF .type/.size on PE/COFF asm"
    fi

    # 5. Symlink module/ + include/ into $LINUX_DIR. Replace any stale
    #    entries first.
    rm -rf "$LINUX_DIR/fs/zfs" "$LINUX_DIR/include/zfs"
    ln -s "$src/module" "$LINUX_DIR/fs/zfs"
    ln -s "$src/include" "$LINUX_DIR/include/zfs"
    log "staged fs/zfs -> $src/module, include/zfs -> $src/include"
}
unstage_zfs() {
    rm -f "$LINUX_DIR/fs/zfs" "$LINUX_DIR/include/zfs"
}

rebuild_fs_kconfig_block() {
    # Only emit a marker block if at least one OOT driver is staged.
    local lines=()
    [[ -L "$LINUX_DIR/fs/ntfsplus" ]] && lines+=('source "fs/ntfsplus/Kconfig"')
    [[ -L "$LINUX_DIR/fs/apfs"     ]] && lines+=('source "fs/apfs/Kconfig"')
    [[ -L "$LINUX_DIR/fs/zfs"      ]] && lines+=('source "fs/zfs/Kconfig"')
    if [[ ${#lines[@]} -eq 0 ]]; then
        strip_marker_block "$LINUX_DIR/fs/Kconfig"
    else
        append_marker_block "$LINUX_DIR/fs/Kconfig" "${lines[@]}"
    fi
}

rebuild_fs_makefile_block() {
    local lines=()
    [[ -L "$LINUX_DIR/fs/ntfsplus" ]] && lines+=('obj-$(CONFIG_NTFSPLUS_FS) += ntfsplus/')
    [[ -L "$LINUX_DIR/fs/apfs"     ]] && lines+=('obj-$(CONFIG_APFS_FS)     += apfs/')
    [[ -L "$LINUX_DIR/fs/zfs"      ]] && lines+=('obj-$(CONFIG_ZFS)         += zfs/')
    if [[ ${#lines[@]} -eq 0 ]]; then
        strip_marker_block "$LINUX_DIR/fs/Makefile"
    else
        append_marker_block "$LINUX_DIR/fs/Makefile" "${lines[@]}"
    fi
}

# ── dispatch ────────────────────────────────────────────────────────────────

case "${1:-}" in
    fetch)   shift; cmd_fetch   "$@" ;;
    stage)   shift; cmd_stage   "$@" ;;
    unstage) shift; cmd_unstage "$@" ;;
    status)  shift; cmd_status  "$@" ;;
    ""|-h|--help)
        sed -n '2,15p' "$0" | sed 's/^# \{0,1\}//'
        ;;
    *) die "unknown subcommand: $1 (try: fetch|stage|unstage|status)" ;;
esac

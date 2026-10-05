#!/bin/bash
# Stage out-of-tree filesystem drivers (and target-specific kernel patches)
# into $LINUX_DIR. Reversible — `unstage` puts the kernel tree back to
# pristine.
#
# Subcommands:
#   fetch [--update]                  clone ~/oot-fs/{zfs,apfs,ntfsplus} at the
#                                     pinned revs; --update also moves existing
#                                     checkouts to the pins (discarding edits).
#   stage [--wasm] [--macho]          apply OOT symlinks + Kconfig/Makefile
#                                     hooks. With --wasm / --macho, also apply
#                                     patches/linux/<flavor>/series against
#                                     $LINUX_DIR. The macho patches touch only
#                                     the LKL host library (macOS port), under
#                                     __APPLE__ or Darwin-only header values:
#                                     no-ops for the elf/pe/wasm builds.
#   unstage [--wasm] [--macho]        reverse everything stage applied.
#   status                            show currently staged drivers + patches.
#
# Layout assumptions:
#   $LINUX_DIR              kernel tree (default ~/linux)
#   $OOT_DIR                ~/oot-fs/ — OOT FS git checkouts
#   $REPO_DIR/patches/linux/<flavor>/series — quilt-style series file (wasm, macho)
#   $REPO_DIR/scripts/oot_fs/      — per-driver helper data (apfs.Kconfig.in, etc.)
#
# Anything we add to ~/linux is wrapped in marker blocks so unstage can
# remove them deterministically:
#
#   # === BEGIN anyfs-reader OOT ===
#   ... added lines ...
#   # === END anyfs-reader OOT ===
#
# Patch application is tracked in $OOT_DIR/.applied.<flavor> so re-staging
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

# Pinned upstream revisions. The stage_* carve-outs below (sed/awk on exact
# source lines) are written against these trees, so a pin bump is a
# deliberate, tested edit. CI's oot-fs cache key hashes this file, so moving
# a pin also invalidates the cached checkouts.
ZFS_REV="8f6f4bcb544ca650fd3796f059dd209dbe0bafa2"
APFS_REV="628b6810e46bcdd423189d2c66295258e10090dc"
NTFSPLUS_REV="5893a4b30e4a821348ab158f594f2c3c9409694e"

die()  { echo "oot_fs: $*" >&2; exit 1; }
log()  { echo "oot_fs: $*"; }

# ── helpers ─────────────────────────────────────────────────────────────────

ensure_linux() {
    [[ -d "$LINUX_DIR/fs" ]] || die "no fs/ under LINUX_DIR=$LINUX_DIR"
    # Callers pass relative paths (CI: --linux=deps/linux), but stage_zfs
    # runs ZFS configure from inside $OOT_DIR/zfs.
    LINUX_DIR="$(cd "$LINUX_DIR" && pwd)"
}

# Run "$@" with its output appended to $1 (label $2); on failure show the
# tail of that log, so CI shows why a quiet one-time step failed.
run_logged() {
    local logf="$1" what="$2"
    shift 2
    "$@" >>"$logf" 2>&1 || {
        tail -n 30 "$logf" >&2
        die "$what failed (full log: $logf)"
    }
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

# Apply patches/linux/<flavor>/series to $LINUX_DIR. Records each applied patch
# in $OOT_DIR/.applied.<flavor> so unstage --<flavor> can reverse exactly the
# same set. $1 is the flavor: wasm | macho.
apply_patch_series() {
    local flavor="$1"
    local dir="$REPO_DIR/patches/linux/$flavor"
    local series="$dir/series"
    [[ -f "$series" ]] || die "no patch series at $series"

    mkdir -p "$OOT_DIR"
    local applied_log="$OOT_DIR/.applied.$flavor"
    : > "$applied_log.new"

    while IFS= read -r p; do
        [[ -z "$p" || "$p" == \#* ]] && continue
        local patch_file="$dir/$p"
        [[ -f "$patch_file" ]] || die "missing $patch_file"

        # Idempotent — if a forward dry-run fails, but a reverse dry-run
        # succeeds, treat the patch as already applied.
        if (cd "$LINUX_DIR" && patch -p1 --dry-run --silent < "$patch_file") >/dev/null 2>&1; then
            (cd "$LINUX_DIR" && patch -p1 --silent < "$patch_file") || die "apply $p failed"
            log "applied $flavor patch: $p"
        elif (cd "$LINUX_DIR" && patch -p1 -R --dry-run --silent < "$patch_file") >/dev/null 2>&1; then
            log "$flavor patch already applied: $p"
        else
            die "$flavor patch $p neither applies forward nor is already applied"
        fi
        echo "$p" >> "$applied_log.new"
    done < "$series"

    mv "$applied_log.new" "$applied_log"
}

revert_patch_series() {
    local flavor="$1"
    local dir="$REPO_DIR/patches/linux/$flavor"
    local applied_log="$OOT_DIR/.applied.$flavor"
    [[ -f "$applied_log" ]] || { log "no $flavor patches recorded"; return 0; }

    # Reverse-apply in reverse order.
    tac "$applied_log" | while IFS= read -r p; do
        [[ -z "$p" ]] && continue
        local patch_file="$dir/$p"
        [[ -f "$patch_file" ]] || { log "WARN: missing $patch_file for unstage"; continue; }

        if (cd "$LINUX_DIR" && patch -p1 -R --dry-run --silent < "$patch_file") >/dev/null 2>&1; then
            (cd "$LINUX_DIR" && patch -p1 -R --silent < "$patch_file") || die "revert $p failed"
            log "reverted $flavor patch: $p"
        elif (cd "$LINUX_DIR" && patch -p1 --dry-run --silent < "$patch_file") >/dev/null 2>&1; then
            log "$flavor patch already reverted: $p"
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
        local name="$1" url="$2" rev="$3"
        local dir="$OOT_DIR/$name"
        if [[ ! -d "$dir/.git" ]]; then
            log "cloning $name @ ${rev:0:12} from $url"
            git -c init.defaultBranch=main init -q "$dir"
            git -C "$dir" remote add origin "$url"
            git -C "$dir" fetch -q --depth=1 origin "$rev"
            git -C "$dir" checkout -q --detach FETCH_HEAD
        elif [[ "$(git -C "$dir" rev-parse HEAD)" == "$rev" ]]; then
            log "$name present at pinned ${rev:0:12}"
        elif [[ $update -eq 1 ]]; then
            # stage patches these trees in place and ZFS configure output is
            # tied to the old sources, so move to the pin from a clean tree.
            log "moving $name to pinned ${rev:0:12}"
            git -C "$dir" fetch -q --depth=1 origin "$rev"
            git -C "$dir" checkout -q --force --detach FETCH_HEAD
            git -C "$dir" clean -q -fdx
        else
            log "WARN: $name is at $(git -C "$dir" rev-parse --short=12 HEAD), pinned ${rev:0:12};" \
                "run 'fetch --update' to move it (discards local edits in $dir)"
        fi
    }
    fetch_one ntfsplus "$NTFSPLUS_REPO" "$NTFSPLUS_REV"
    fetch_one apfs     "$APFS_REPO"     "$APFS_REV"
    fetch_one zfs      "$ZFS_REPO"      "$ZFS_REV"
}

# Stage out-of-tree FS drivers. Phase-aware — for the initial wasm/XFS
# phase 0 work, only the wasm patch layer matters; the OOT symlinking
# branches below are no-ops until ~/oot-fs/{ntfsplus,apfs,zfs} exist.
cmd_stage() {
    ensure_linux
    local want_wasm=0 want_macho=0
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --wasm) want_wasm=1; shift ;;
            --macho) want_macho=1; shift ;;
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

    # target-specific kernel patches ───────────────────────────────────────
    if [[ $want_wasm -eq 1 ]]; then
        apply_patch_series wasm
    fi
    if [[ $want_macho -eq 1 ]]; then
        apply_patch_series macho
    fi

    log "stage complete"
}

cmd_unstage() {
    ensure_linux
    local want_wasm=0 want_macho=0
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --wasm) want_wasm=1; shift ;;
            --macho) want_macho=1; shift ;;
            *) die "unstage: unknown arg $1" ;;
        esac
    done

    # Reverse of cmd_stage's apply order, in case the two series ever
    # touch the same file.
    if [[ $want_macho -eq 1 ]]; then
        revert_patch_series macho
    fi
    if [[ $want_wasm -eq 1 ]]; then
        revert_patch_series wasm
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
    for flavor in wasm macho; do
        if [[ -f "$OOT_DIR/.applied.$flavor" ]]; then
            echo "  $flavor patches applied:"
            sed 's/^/    /' "$OOT_DIR/.applied.$flavor"
        else
            echo "  $flavor patches: none"
        fi
    done
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

# Rewrite every line of ZFS source file $1 that reads exactly $2 into $3.
# Returns 0 if it patched, 1 if no $2 line is left and $3 is already there
# (re-stage). Dies if neither is present, so a ZFS pin bump that moves the
# anchor fails here instead of logging success over an unpatched file.
zfs_rewrite_line() {
    local f="$1" from="$2" to="$3"
    if ! grep -qxF -- "$from" "$f"; then
        grep -qxF -- "$to" "$f" && return 1
        die "stage_zfs: anchor '$from' not found in $f"
    fi
    # Called as an `if` condition, where set -e is off: fail explicitly.
    # ENVIRON, not awk -v, which would interpret backslashes in the lines;
    # concatenating "" forces a string comparison.
    ZFS_FROM="$from" ZFS_TO="$to" \
        awk '($0 "") == (ENVIRON["ZFS_FROM"] "") { $0 = ENVIRON["ZFS_TO"] } { print }' \
        "$f" > "$f.anyfs.tmp" && mv "$f.anyfs.tmp" "$f" \
        || die "stage_zfs: rewrite of '$from' failed in $f"
}

stage_zfs() {
    local src="$OOT_DIR/zfs"
    [[ -d "$src" ]] || return 0

    # 1. Run ZFS configure if zfs_config.h hasn't been produced yet. ZFS's
    #    configure expects a "normal" prepared kernel build dir (it tries
    #    `make modules` against a conftest object, which LKL build dirs can't
    #    satisfy because they're ARCH=lkl). So we prepare a dedicated x86_64
    #    build dir under $OOT_DIR/.zfs-configure-build purely for this probe.
    local logf="$OOT_DIR/.zfs-configure.log"
    if [[ ! -f "$src/zfs_config.h" ]]; then
        local cfg_build="$OOT_DIR/.zfs-configure-build"
        : > "$logf"
        # Stamp, not a generated header: prepare writes utsrelease.h before
        # it builds objtool, so a half-failed prepare would look complete.
        if [[ ! -f "$cfg_build/.anyfs-prepared" ]]; then
            log "preparing $cfg_build (one-time, ~30s) for ZFS configure"
            mkdir -p "$cfg_build"
            run_logged "$logf" "x86_64 defconfig" \
                make -C "$LINUX_DIR" O="$cfg_build" defconfig
            "$LINUX_DIR/scripts/config" --file "$cfg_build/.config" -e MODULES
            run_logged "$logf" "x86_64 olddefconfig" \
                make -C "$LINUX_DIR" O="$cfg_build" olddefconfig
            # x86_64 prepare builds objtool, so the host needs libelf-dev.
            run_logged "$logf" "x86_64 prepare" \
                make -C "$LINUX_DIR" O="$cfg_build" prepare
            touch "$cfg_build/.anyfs-prepared"
        fi
        if [[ ! -f "$src/configure" ]]; then
            log "ZFS autogen.sh (one-time)"
            (cd "$src" && run_logged "$logf" "ZFS autogen" bash autogen.sh)
        fi
        log "ZFS configure (one-time, several minutes)"
        (cd "$src" && run_logged "$logf" "ZFS configure" ./configure \
            --with-linux="$LINUX_DIR" \
            --with-linux-obj="$cfg_build" \
            --enable-linux-builtin \
            --with-config=kernel)
    fi
    if [[ ! -f "$src/include/zfs_gitrev.h" ]]; then
        log "ZFS make gitrev"
        (cd "$src" && run_logged "$logf" "ZFS make gitrev" make gitrev)
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

    # 4a. LKL-on-x86 carry: ZFS's os/linux/kernel/linux/simd.h dispatches on
    #    `__x86` (set by isa_defs.h from __x86_64__) and pulls in
    #    simd_x86.h, which #include's <asm/cpufeature.h> and <asm/fpu/api.h>.
    #    LKL has no arch/lkl/include/asm/cpufeature.h — building against
    #    LKL on an x86_64 host therefore fails. Gate the x86 branch behind
    #    !CONFIG_LKL so LKL falls through to the SIMD-disabled stub.
    #    Idempotent, and dies if the anchor line is gone (zfs_rewrite_line).
    local simdh="$src/include/os/linux/kernel/linux/simd.h"
    if [[ -f "$simdh" ]] && zfs_rewrite_line "$simdh" \
            '#if defined(__x86)' \
            '#if defined(__x86) && !defined(CONFIG_LKL)'; then
        log "patched ZFS simd.h to skip x86 SIMD on CONFIG_LKL builds"
    fi

    # 4a-2. Same problem on the aarch64 branch, which matters for the
    #       linux-arm64 LKL target (also the macOS kernel): simd_aarch64.h
    #       #include's <asm/neon.h>, <asm/hwcap.h> and <asm/sysreg.h>, none of
    #       which arch/lkl provides.
    #       Gate it behind !CONFIG_LKL so LKL falls through to the same
    #       SIMD-disabled stub the x86 gate relies on. Idempotent.
    if [[ -f "$simdh" ]] && zfs_rewrite_line "$simdh" \
            '#elif defined(__aarch64__)' \
            '#elif defined(__aarch64__) && !defined(CONFIG_LKL)'; then
        log "patched ZFS simd.h to skip aarch64 SIMD on CONFIG_LKL builds"
    fi

    # 4b. simd_stat.c references every zfs_*_available() declared in
    #     simd_x86.h directly from an `#if defined(__x86_64__) || defined(__i386__)`
    #     block. Now that simd_x86.h is excluded on LKL, those references
    #     become implicit-decl errors. Gate the x86 block behind !CONFIG_LKL
    #     too — same shape as the simd.h patch above. Idempotent, and dies if
    #     the anchor line is gone (zfs_rewrite_line).
    local simdstat="$src/module/zcommon/simd_stat.c"
    if [[ -f "$simdstat" ]] && zfs_rewrite_line "$simdstat" \
            '#if defined(__x86_64__) || defined(__i386__)' \
            '#if (defined(__x86_64__) || defined(__i386__)) && !defined(CONFIG_LKL)'; then
        log "patched ZFS simd_stat.c to skip x86 SIMD-stat on CONFIG_LKL builds"
    fi

    # 4b-2. Same for the ARM/aarch64 SIMD-stat block in simd_stat.c. It calls
    #       zfs_neon_available() and friends, which only simd_aarch64.h
    #       declares -- and 4a-2 just excluded that header on LKL. Gate the
    #       block behind !CONFIG_LKL so it disappears together with the
    #       declarations. The two sha2 ICP files are handled by 4b-3.
    #       Idempotent, and dies if the anchor line is gone (zfs_rewrite_line).
    if [[ -f "$simdstat" ]] && zfs_rewrite_line "$simdstat" \
            '#if defined(__arm__) || defined(__aarch64__)' \
            '#if (defined(__arm__) || defined(__aarch64__)) && !defined(CONFIG_LKL)'; then
        log "patched ZFS simd_stat.c to skip arm SIMD-stat on CONFIG_LKL builds"
    fi
    local sha256impl="$src/module/icp/algs/sha2/sha256_impl.c"
    local sha512impl="$src/module/icp/algs/sha2/sha512_impl.c"

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
        # The prologue's `#undef __x86_64/__amd64/__x86/__i386` is necessary
        # but not sufficient: the file immediately below has a
        # `#if defined(__x86_64) || defined(__x86_64__) ... #if !defined(__x86_64)
        # # define __x86_64 #endif` block that re-defines the bare-name macros
        # back to an empty body. (`__x86_64__` is a separate GCC builtin and
        # we deliberately leave it alone — ZFS uses it to detect the x86_64
        # ABI, which is correct here.) Gate those re-defines behind
        # !CONFIG_LKL so the undef sticks for ZFS C source. Same shape as
        # the simd.h / simd_stat.c patches above. Idempotent — the marker
        # added by sed is "&& !defined(CONFIG_LKL)" itself.
        sed -i \
            -e 's|^#if !defined(__x86_64)$|#if !defined(__x86_64) \&\& !defined(CONFIG_LKL)|' \
            -e 's|^#if !defined(__amd64)$|#if !defined(__amd64) \&\& !defined(CONFIG_LKL)|' \
            -e 's|^#if !defined(__x86)$|#if !defined(__x86) \&\& !defined(CONFIG_LKL)|' \
            "$isadefs"
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

    # 4d. Insert a wasm32 branch into the ISA ladder in isa_defs.h so
    #     wasm32 builds don't hit `#error "Unsupported ISA type"`.
    #     The block must appear immediately before the final `#else` of the
    #     ladder (the one preceding the "Unsupported ISA type" #error).
    #     Idempotent — skip if __wasm__ is already present.
    #     Hard-fails if the anchor (#error "Unsupported ISA type") or the
    #     preceding #else are not found.
    local f="$src/include/os/linux/spl/sys/isa_defs.h"
    if [[ -f "$f" ]] && grep -q '__wasm__' "$f"; then
        log "ZFS isa_defs.h wasm32 branch already present — skipping"
    elif [[ -f "$f" ]]; then
        # Verify the anchor exists before attempting the transform.
        grep -q '"Unsupported ISA type"' "$f" \
            || die "stage_zfs: anchor '#error \"Unsupported ISA type\"' not found in $f"
        awk '
            # Buffer every line. When we see the #error anchor, walk back
            # through the buffer to find the nearest preceding bare #else,
            # splice the wasm block before it, then flush.
            {
                buf[NR] = $0
            }
            /#error "Unsupported ISA type"/ {
                anchor = NR
            }
            END {
                if (!anchor) {
                    print "awk: anchor not found" > "/dev/stderr"
                    exit 1
                }
                # Find the #else immediately before the anchor.
                else_line = 0
                for (i = anchor - 1; i >= 1; i--) {
                    if (buf[i] ~ /^#else[[:space:]]*$/) {
                        else_line = i
                        break
                    }
                }
                if (!else_line) {
                    print "awk: preceding #else not found before anchor at line " anchor > "/dev/stderr"
                    exit 1
                }
                # Print lines 1..(else_line-1), then the wasm block, then rest.
                for (i = 1; i < else_line; i++) print buf[i]
                print "/*"
                print " * WebAssembly (wasm32) \342\200\224 anyfs-reader LKL/emscripten target."
                print " * 32-bit, little-endian, no SIMD/FPU intrinsics visible to ZFS."
                print " */"
                print "#elif defined(__wasm__) || defined(__wasm32__)"
                print ""
                print "#if !defined(_ILP32)"
                print "#define\t_ILP32"
                print "#endif"
                print ""
                print "#define\t_ZFS_LITTLE_ENDIAN"
                print ""
                for (i = else_line; i <= NR; i++) print buf[i]
            }
        ' "$f" > "$f.new" \
            && mv "$f.new" "$f" \
            || die "stage_zfs: awk transform failed for $f"
        log "patched ZFS isa_defs.h to add wasm32 ISA branch"
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
        sed -n '2,18p' "$0" | sed 's/^# \{0,1\}//'
        ;;
    *) die "unknown subcommand: $1 (try: fetch|stage|unstage|status)" ;;
esac

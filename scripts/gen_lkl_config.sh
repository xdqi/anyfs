#!/bin/bash
# Generate LKL kernel configs for anyfs-reader (out-of-tree builds).
#
# Usage: ./gen_lkl_config.sh [OPTIONS]
#
# Options:
#   --linux=DIR         Kernel source tree (default: ~/linux)
#   --out=DIR           Parent dir for build trees (default: ~/anyfs-reader)
#   --targets=LIST      Comma-separated subset of:
#                         linux-amd64,linux-arm64,mingw32,mingw64
#                       (default: linux-amd64,mingw32,mingw64)
#
# Produces, under $OUT:
#   lkl-linux-amd64/    native Linux x86_64
#   lkl-linux-arm64/    Linux aarch64 cross (aarch64-linux-gnu-)
#   lkl-mingw32/        Win32 cross (i686-w64-mingw32-)
#   lkl-mingw64/        Win64 cross (x86_64-w64-mingw32-)
#
# Each tree contains its own .config plus tools/lkl/{Makefile.conf,
# include/lkl_autoconf.h, ...}. Nothing is written into the kernel source tree.
#
# To build the configured target(s), use the companion script: build_lkl.sh
set -e

LINUX_DIR="$HOME/linux"
OUT_PARENT="$HOME/anyfs-reader"
# Patched binutils-2.46 install (LKL weak-symbol fixes). The bundled
# binutils under $LINUX_DIR/tools/lkl/bin is 2.25.1, below the 2.30
# minimum kernel 6.13+ Kconfig requires. We override LD/AS/etc per-arch
# below so the kernel sub-make uses the 2.46 install regardless of the
# stale shadowed tools that the LKL Makefile prepends to PATH.
BINUTILS_DIR="${BINUTILS_DIR:-$HOME/binutils-gdb/build-combined/install/bin}"
TARGETS_REQ=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --linux=*)   LINUX_DIR="${1#--linux=}"; shift ;;
        --linux)     LINUX_DIR="$2"; shift 2 ;;
        --out=*)     OUT_PARENT="${1#--out=}"; shift ;;
        --out)       OUT_PARENT="$2"; shift 2 ;;
        --targets=*) TARGETS_REQ="${1#--targets=}"; shift ;;
        --targets)   TARGETS_REQ="$2"; shift 2 ;;
        -h|--help)
            sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'
            exit 0
            ;;
        *) echo "Unknown argument: $1" >&2; exit 1 ;;
    esac
done

TARGETS_REQ="${TARGETS_REQ:-linux-amd64,mingw32,mingw64}"

SCRIPTS_CONFIG="$LINUX_DIR/scripts/config"
if [[ ! -x "$SCRIPTS_CONFIG" ]]; then
    echo "Error: $SCRIPTS_CONFIG not found. Is --linux=$LINUX_DIR correct?" >&2
    exit 1
fi

# ── Apply the anyfs-reader feature set onto $1=.config file ──────────────────
apply_common_config() {
    local DOTCONFIG="$1"
    cfg() { "$SCRIPTS_CONFIG" --file "$DOTCONFIG" "$@"; }

    # Core / General
    cfg -d LOCALVERSION_AUTO
    cfg -e NO_HZ_IDLE
    cfg -e HIGH_RES_TIMERS
    cfg -e EXPERT
    cfg -d SYSFS_SYSCALL
    cfg -d FUTEX
    cfg -d SIGNALFD
    cfg -d TIMERFD
    cfg -d AIO
    cfg -d ADVISE_SYSCALLS
    cfg -e KALLSYMS_ALL
    cfg -d COMPAT_BRK
    cfg -d VM_EVENT_COUNTERS
    cfg -e PRINTK_TIME

    # Networking
    cfg -e NET
    cfg -e INET
    cfg -e IP_MULTICAST
    cfg -e IP_ADVANCED_ROUTER
    cfg -e IP_MULTIPLE_TABLES
    cfg -e IP_ROUTE_MULTIPATH
    cfg -e IP_PNP
    cfg -e IP_PNP_DHCP
    cfg -e TCP_CONG_ADVANCED
    cfg -e TCP_CONG_BBR
    cfg -e IPV6_MULTIPLE_TABLES
    cfg -e NET_SCHED
    cfg -e NET_SCH_FQ
    cfg -d WIRELESS

    # Block / Virtio
    cfg -d FW_LOADER
    cfg -e VIRTIO_BLK
    cfg -e BLK_DEV_NVME
    cfg -e NETDEVICES
    cfg -e VIRTIO_NET
    cfg -d ETHERNET
    cfg -d WLAN
    cfg -d VT
    cfg -e VIRTIO_MMIO
    cfg -e VIRTIO_MMIO_CMDLINE_DEVICES

    # Device Mapper / dm-crypt (LUKS) / LVM
    cfg -e MD
    cfg -e BLK_DEV_DM
    cfg -e DM_CRYPT
    cfg -e DM_SNAPSHOT
    cfg -e DM_THIN_PROVISIONING
    cfg -e DM_CACHE
    cfg -e DM_CACHE_SMQ
    cfg -e DM_MIRROR
    cfg -e DM_RAID
    cfg -e DM_ZERO

    # Crypto (needed for LUKS/dm-crypt)
    cfg -e CRYPTO
    cfg -e CRYPTO_AES
    cfg -e CRYPTO_XTS
    cfg -e CRYPTO_SHA1
    cfg -e CRYPTO_SHA256
    cfg -e CRYPTO_SHA512
    cfg -e CRYPTO_ESSIV
    cfg -e CRYPTO_HMAC
    cfg -e CRYPTO_CMAC
    cfg -e CRYPTO_CBC
    cfg -e CRYPTO_ECB
    cfg -e CRYPTO_CTS
    cfg -e CRYPTO_LRW
    cfg -e CRYPTO_ANSI_CPRNG
    cfg -e CRYPTO_USER_API
    cfg -e CRYPTO_USER_API_HASH
    cfg -e CRYPTO_USER_API_SKCIPHER

    # Filesystems: ext
    cfg -e EXT4_FS
    cfg -e EXT4_FS_POSIX_ACL
    cfg -e EXT4_FS_SECURITY

    # Filesystems: journaling/COW
    cfg -e REISERFS_FS
    cfg -e JFS_FS
    cfg -e XFS_FS
    cfg -e XFS_POSIX_ACL
    cfg -e BTRFS_FS
    cfg -e BTRFS_FS_POSIX_ACL
    cfg -e BCACHEFS_FS
    cfg -e NILFS2_FS
    cfg -e F2FS_FS
    cfg -e GFS2_FS

    # Filesystems: optical/archive
    cfg -e ISO9660_FS
    cfg -e JOLIET
    cfg -e ZISOFS
    cfg -e UDF_FS

    # Filesystems: FAT/Windows
    cfg -e MSDOS_FS
    cfg -e VFAT_FS
    cfg --set-val FAT_DEFAULT_UTF8 1
    cfg -e EXFAT_FS
    # NTFS3 (the in-tree driver) is replaced by NTFS PLUS — the out-of-tree
    # driver at github.com/namjaejeon/linux-ntfs, staged under fs/ntfsplus/
    # by scripts/oot_fs.sh. The two drivers cannot coexist (both register
    # filesystem name "ntfs" via the shim), so NTFS3 is forced off here.
    cfg -d NTFS3_FS
    cfg -d NTFS3_LZX_XPRESS
    cfg -d NTFS3_FS_POSIX_ACL
    cfg -e NTFSPLUS_FS
    cfg -e NTFSPLUS_FS_POSIX_ACL

    # APFS (OOT, staged from github.com/linux-apfs/linux-apfs-rw).
    cfg -e APFS_FS

    # OpenZFS (OOT, staged from github.com/openzfs/zfs).
    cfg -e ZFS

    # Filesystems: misc / legacy
    cfg -e ADFS_FS
    cfg -e AFFS_FS
    cfg -e HFS_FS
    cfg -e HFSPLUS_FS
    cfg -e BEFS_FS
    cfg -e BFS_FS
    cfg -e EFS_FS
    cfg -e CRAMFS
    cfg -e SQUASHFS
    cfg -e SQUASHFS_ZLIB
    cfg -e SQUASHFS_LZ4
    cfg -e SQUASHFS_LZO
    cfg -e SQUASHFS_XZ
    cfg -e SQUASHFS_ZSTD
    cfg -e VXFS_FS
    cfg -e MINIX_FS
    cfg -e OMFS_FS
    cfg -e HPFS_FS
    cfg -e QNX4FS_FS
    cfg -e QNX6FS_FS
    cfg -e ROMFS_FS
    cfg -e SYSV_FS
    cfg -e UFS_FS
    cfg -e EROFS_FS
    cfg -e EROFS_FS_ZIP
    cfg -e EROFS_FS_ZIP_LZMA
    cfg -e EROFS_FS_ZIP_DEFLATE
    cfg -e EROFS_FS_ZIP_ZSTD

    # Filesystems: network servers
    # FILE_LOCKING is a hard dep of NFSD and SMB_SERVER; FSNOTIFY is a hard
    # dep of NFSD. defconfig disables both under EXPERT, so re-enable
    # explicitly or NFSD/SMB_SERVER silently drop out of the config.
    cfg -e FILE_LOCKING
    # FSNOTIFY itself is def_bool n (no prompt); flip it on by enabling a
    # consumer that 'select's it. We pick FANOTIFY rather than INOTIFY_USER
    # because LKL's headers_install.py only prefixes __NR_* symbols that
    # appear as #define in some header it scans. asm-generic/unistd.h has
    # __NR_inotify_init1 / __NR_fanotify_{init,mark} but NOT the legacy
    # __NR_inotify_init — so syscall_defs.h emits an unprefixed
    # `#ifdef __NR_inotify_init` that the host's x86 unistd.h satisfies,
    # then references an undefined __lkl__NR_inotify_init in the body.
    # FANOTIFY's syscall NRs are all in asm-generic/unistd.h → clean prefix.
    cfg -e FANOTIFY
    cfg -e NFSD
    cfg -e NFSD_V4
    cfg -e SMB_SERVER
    cfg -d SMB_SERVER_CHECK_CAP_NET_ADMIN

    # Unicode / NLS
    cfg -e UNICODE
    cfg -e NLS_UTF8
    cfg --set-str NLS_DEFAULT "utf8"
    cfg -e NLS_CODEPAGE_437
    cfg -e NLS_CODEPAGE_737
    cfg -e NLS_CODEPAGE_775
    cfg -e NLS_CODEPAGE_850
    cfg -e NLS_CODEPAGE_852
    cfg -e NLS_CODEPAGE_855
    cfg -e NLS_CODEPAGE_857
    cfg -e NLS_CODEPAGE_860
    cfg -e NLS_CODEPAGE_861
    cfg -e NLS_CODEPAGE_862
    cfg -e NLS_CODEPAGE_863
    cfg -e NLS_CODEPAGE_864
    cfg -e NLS_CODEPAGE_865
    cfg -e NLS_CODEPAGE_866
    cfg -e NLS_CODEPAGE_869
    cfg -e NLS_CODEPAGE_936
    cfg -e NLS_CODEPAGE_950
    cfg -e NLS_CODEPAGE_932
    cfg -e NLS_CODEPAGE_949
    cfg -e NLS_CODEPAGE_874
    cfg -e NLS_ISO8859_8
    cfg -e NLS_CODEPAGE_1250
    cfg -e NLS_CODEPAGE_1251
    cfg -e NLS_ASCII
    cfg -e NLS_ISO8859_1
    cfg -e NLS_ISO8859_2
    cfg -e NLS_ISO8859_3
    cfg -e NLS_ISO8859_4
    cfg -e NLS_ISO8859_5
    cfg -e NLS_ISO8859_6
    cfg -e NLS_ISO8859_7
    cfg -e NLS_ISO8859_9
    cfg -e NLS_ISO8859_13
    cfg -e NLS_ISO8859_14
    cfg -e NLS_ISO8859_15
    cfg -e NLS_KOI8_R
    cfg -e NLS_KOI8_U
    cfg -e NLS_MAC_ROMAN
    cfg -e NLS_MAC_CELTIC
    cfg -e NLS_MAC_CENTEURO
    cfg -e NLS_MAC_CROATIAN
    cfg -e NLS_MAC_CYRILLIC
    cfg -e NLS_MAC_GAELIC
    cfg -e NLS_MAC_GREEK
    cfg -e NLS_MAC_ICELAND
    cfg -e NLS_MAC_INUIT
    cfg -e NLS_MAC_ROMANIAN
    cfg -e NLS_MAC_TURKISH

    cfg -d DNOTIFY
}

# ── Per-target arch tweaks, post-Makefile.conf, autoconf shims ──────────────
configure_target() {
    local NAME="$1"
    local CROSS="$2"           # empty for native
    local OUT="$OUT_PARENT/lkl-$NAME"
    local DOTCONFIG="$OUT/.config"
    local LKL_OUT="$OUT/tools/lkl"

    echo
    echo "=============================================================="
    echo "  Configuring lkl-$NAME (CROSS=${CROSS:-<native>})"
    echo "  OUT: $OUT"
    echo "=============================================================="

    mkdir -p "$OUT" "$LKL_OUT/include" "$OUT/scripts/mod"

    # Start fresh — but only inside the out-of-tree build dir.
    rm -f "$DOTCONFIG" "$DOTCONFIG.old"
    rm -rf "$OUT/include/config"

    # defconfig — kconfig itself doesn't need CROSS_COMPILE, but Kconfig's
    # default for OUTPUT_FORMAT shells out to cc-objdump-file-format.sh which
    # invokes $CC. For non-native targets that probe must hit the cross
    # compiler so OUTPUT_FORMAT picks up the right elf64-<arch> string.
    # mingw is an exception: its toolchain is Wine-based and breaks
    # Kconfig's $(shell,...) probes.
    local CROSS_KCONFIG=""
    if [[ "$NAME" != mingw* ]]; then
        CROSS_KCONFIG="CROSS_COMPILE=$CROSS"
    fi
    make -C "$LINUX_DIR" ARCH=lkl O="$OUT" $CROSS_KCONFIG defconfig 2>/dev/null || true

    apply_common_config "$DOTCONFIG"

    # Arch-specific bits
    cfg() { "$SCRIPTS_CONFIG" --file "$DOTCONFIG" "$@"; }
    # Helper: switch the "Debug information" choice to NONE. scripts/config
    # can't change a choice atomically, so disable the alternatives by hand.
    debug_info_none() {
        cfg -e DEBUG_INFO_NONE
        cfg -d DEBUG_INFO_DWARF_TOOLCHAIN_DEFAULT
        cfg -d DEBUG_INFO_DWARF4
        cfg -d DEBUG_INFO_DWARF5
        cfg -d DEBUG_INFO
    }

    case "$NAME" in
        linux-amd64)
            cfg -e 64BIT
            debug_info_none
            ;;
        linux-arm64)
            cfg -e 64BIT
            debug_info_none
            ;;
        mingw32)
            cfg -d 64BIT
            debug_info_none
            ;;
        mingw64)
            cfg -e 64BIT
            cfg --set-str OUTPUT_FORMAT "pe-x86-64"
            # MSYS2 gcc preserves DWARF in COFF, so keep it on for 64-bit.
            cfg -d DEBUG_INFO_NONE
            cfg -e DEBUG_INFO_DWARF4
            ;;
    esac

    make -C "$LINUX_DIR" ARCH=lkl O="$OUT" $CROSS_KCONFIG olddefconfig 2>/dev/null

    # tools/lkl/Makefile.conf — hand-rolled for mingw (autoconf can't probe a
    # PE/Wine target) and for linux-arm64 (autoconf would still see the host's
    # /usr/include and enable fuse/macvtap whose aarch64 libs aren't
    # installed → link failures). Native linux-amd64 still uses autoconf.
    local CONF="$LKL_OUT/Makefile.conf"
    if [[ "$NAME" == "linux-arm64" ]]; then
        cat > "$CONF" <<EOF
  export CROSS_COMPILE := ${CROSS}
  export CC := ${CROSS}gcc
  export LD := ${CROSS}ld
  export AR := ${CROSS}ar
  export LKL_HOST_CONFIG_POSIX=y
  export LKL_HOST_CONFIG_AARCH64=y
  export LKL_HOST_CONFIG_VIRTIO_NET=y
  export LKL_HOST_CONFIG_VIRTIO_NET_FD=y
  LDFLAGS += -pie -z noexecstack
  CFLAGS += -fPIC -pthread
  SOSUF := .so
  LDLIBS += -lrt -lpthread
EOF
        cat > "$LKL_OUT/include/lkl_autoconf.h" <<EOF
#define LKL_HOST_CONFIG_POSIX y
#define LKL_HOST_CONFIG_AARCH64 y
#define LKL_HOST_CONFIG_VIRTIO_NET y
#define LKL_HOST_CONFIG_VIRTIO_NET_FD y
EOF
    elif [[ "$NAME" == mingw* ]]; then
        local ELFCLASS MSYS_ARCH LDFLAGS_EXTRA
        local NT64_CONF="" NT64_AUTOCONF=""
        if [[ "$NAME" == "mingw64" ]]; then
            ELFCLASS="ELFCLASS64"
            MSYS_ARCH="mingw64"
            LDFLAGS_EXTRA="LDFLAGS += -Wl,--image-base,0x10000"
            # NT64 pulls in virtio_net_wintap.c so lkl_netdev_wintap_create
            # (referenced by tests/net-test.c) actually has a definition.
            NT64_CONF="  export LKL_HOST_CONFIG_NT64=y"
            NT64_AUTOCONF="#define LKL_HOST_CONFIG_NT64 y"
        else
            ELFCLASS="ELFCLASS32"
            MSYS_ARCH="mingw32"
            LDFLAGS_EXTRA=""
        fi
        cat > "$CONF" <<EOF
  export CROSS_COMPILE := ${CROSS}
  export CC := ${CROSS}gcc
  export LD := ${BINUTILS_DIR}/${CROSS}ld
  export AR := ${BINUTILS_DIR}/${CROSS}ar
  export LKL_HOST_CONFIG_NT=y
${NT64_CONF}
  export LKL_HOST_CONFIG_VIRTIO_NET=y
  # Override kernel-make's CROSS_COMPILE-derived tool vars with absolute
  # paths to the patched 2.46 binutils. KOPT entries are appended as CLI
  # args to the inner \$(MAKE) and therefore override the kernel root
  # Makefile's LD = \$(CROSS_COMPILE)ld assignment, sidestepping the
  # stale 2.25.1 ld that tools/lkl/Makefile prepends to PATH.
  KOPT += "LD=${BINUTILS_DIR}/${CROSS}ld"
  KOPT += "AS=${BINUTILS_DIR}/${CROSS}as"
  KOPT += "AR=${BINUTILS_DIR}/${CROSS}ar"
  KOPT += "NM=${BINUTILS_DIR}/${CROSS}nm"
  KOPT += "OBJCOPY=${BINUTILS_DIR}/${CROSS}objcopy"
  KOPT += "OBJDUMP=${BINUTILS_DIR}/${CROSS}objdump"
  KOPT += "READELF=${BINUTILS_DIR}/${CROSS}readelf"
  KOPT += "STRIP=${BINUTILS_DIR}/${CROSS}strip"
  KOPT += "KALLSYMS_EXTRA_PASS=1"
  KOPT += "HOSTCFLAGS=-Wno-char-subscripts"
  KOPT += "HOSTLDFLAGS=-s"
  # ZFS on mingw cross: __linux__ isn't a mingw builtin (target triple is
  # x86_64-w64-mingw32), so simd_config.h — which is command-line -include'd
  # before any source-level header can define it — falls through to its
  # HAVE_TOOLCHAIN_* branch. That bakes HAVE_SIMD(AVX/AES/PCLMULQDQ) as 1
  # and lights up CAN_USE_GCM_ASM in modes.h, which then references
  # zfs_avx_available / zfs_movbe_available / etc. — symbols only defined
  # in the x86 .S files that LKL doesn't build. Force __linux__ globally
  # so simd_config.h takes the HAVE_KERNEL_* branch, which our LKL carve-out
  # at the bottom of zfs_config.h already undef's.
  KOPT += "KCFLAGS=-D__linux__=1"
  LDLIBS += -lws2_32 -liphlpapi
  ${LDFLAGS_EXTRA}
  EXESUF := .exe
  SOSUF := .dll
  CFLAGS += -Iinclude/mingw32
EOF
        # mk_elfconfig cannot parse PE objects — pre-seed elfconfig.h.
        echo "#define KERNEL_ELFCLASS ${ELFCLASS}" > "$OUT/scripts/mod/elfconfig.h"
        cat > "$LKL_OUT/include/lkl_autoconf.h" <<EOF
#define LKL_HOST_CONFIG_NT y
${NT64_AUTOCONF}
#define LKL_HOST_CONFIG_VIRTIO_NET y
EOF
    else
        # Native POSIX — let autoconf regenerate Makefile.conf/lkl_autoconf.h
        # by probing the host. Provoke it now (instead of leaving it to the
        # first real build) because the Makefile.conf rule, when it fires,
        # truncates kernel.config to 0 bytes — and we're about to populate
        # kernel.config below as our config overlay. Do the wipe here, once,
        # then refill afterwards.
        rm -f "$CONF" "$LKL_OUT/include/lkl_autoconf.h"
        OUTPUT="$OUT" make -C "$LINUX_DIR/tools/lkl" ARCH=lkl \
             "$LKL_OUT/Makefile.conf" >/dev/null 2>&1 || true
    fi

    # ── kernel.config overlay ────────────────────────────────────────────
    # tools/lkl/Makefile rebuilds .config on demand via:
    #     make defconfig          (wipes .config)
    #     cat kernel.config >> .config
    #     make olddefconfig
    # Instead of hand-maintaining a list of overrides (which inevitably
    # drifts from apply_common_config), auto-generate kernel.config as
    # the diff between our customized .config and a clean defconfig.
    # This guarantees kernel.config is always complete and in sync.
    echo "  Generating kernel.config (diff vs defconfig)..."
    cp "$DOTCONFIG" "$OUT/.config.our"
    make -C "$LINUX_DIR" ARCH=lkl O="$OUT" $CROSS_KCONFIG defconfig 2>/dev/null
    # Capture lines present in our config but absent from the fresh
    # defconfig: newly-enabled features, disabled features that defconfig
    # left unset, and string/int overrides.  Sorted comparison so
    # olddefconfig line reordering doesn't create spurious diffs.
    comm -13 \
      <(sort "$DOTCONFIG") \
      <(sort "$OUT/.config.our") \
      > "$LKL_OUT/kernel.config"
    # Restore our full config for the actual build.
    mv "$OUT/.config.our" "$DOTCONFIG"
    echo "  kernel.config: $(wc -l < "$LKL_OUT/kernel.config") lines"
    # Bump Makefile.conf's mtime so the autoconf rule (which would wipe
    # kernel.config back to empty) does not re-fire on subsequent builds.
    touch "$LKL_OUT/Makefile.conf"
}

# Map name → cross prefix
cross_for() {
    case "$1" in
        linux-amd64) echo "" ;;
        linux-arm64) echo "aarch64-linux-gnu-" ;;
        mingw32)     echo "i686-w64-mingw32-" ;;
        mingw64)     echo "x86_64-w64-mingw32-" ;;
        *) echo "Unknown target: $1" >&2; return 1 ;;
    esac
}

IFS=',' read -ra TARGETS_ARR <<< "$TARGETS_REQ"

# Validate up front
for T in "${TARGETS_ARR[@]}"; do
    cross_for "$T" >/dev/null
done

mkdir -p "$OUT_PARENT"

# Apply the LKL rpcbind guard to net/sunrpc/svc.c. Without this, anyfs-nfsd
# hangs at startup because svc_register() and svc_uses_rpcbind() try to talk
# to localhost:111 (rpcbind), which has no listener inside LKL. Idempotent:
# guarded by a "LKL_RPCBIND_GUARD" marker comment.
patch_sunrpc_lkl_guard() {
    local svc_c="$LINUX_DIR/net/sunrpc/svc.c"
    [[ -f "$svc_c" ]] || return 0
    if grep -q "LKL_RPCBIND_GUARD" "$svc_c"; then
        return 0
    fi
    awk '
        BEGIN { st = 0 }
        /^int svc_register\(/                 { st = 1; print; next }
        /^static int svc_uses_rpcbind\(/      { st = 2; print; next }
        (st == 1 || st == 2) && /^\{$/ {
            print
            print "#ifdef CONFIG_LKL"
            if (st == 1)
                print "\treturn 0;  /* LKL_RPCBIND_GUARD: skip rpcbind localhost:111 */"
            else
                print "\treturn 0;  /* LKL_RPCBIND_GUARD: no rpcbind under LKL */"
            print "#endif"
            st = 0
            next
        }
        { print }
    ' "$svc_c" > "$svc_c.tmp" && mv "$svc_c.tmp" "$svc_c"
    if ! grep -q "LKL_RPCBIND_GUARD" "$svc_c"; then
        echo "ERROR: failed to inject LKL_RPCBIND_GUARD into $svc_c" >&2
        exit 1
    fi
    echo "Patched LKL rpcbind guard into net/sunrpc/svc.c"
}
patch_sunrpc_lkl_guard

# Stage OOT filesystems (NTFS PLUS, etc.) into $LINUX_DIR. Idempotent —
# re-running between iterations is safe. No --wasm here; the wasm-only
# kernel patches (e.g. XFS computed-goto fix) are applied by the wasm
# generator. See scripts/oot_fs.sh.
LINUX_DIR="$LINUX_DIR" "$(dirname "$0")/oot_fs.sh" stage

# Clean stale lkl_autoconf.h from the source include dir. The kernel
# build's autoconf step occasionally leaves an empty (or stale) copy
# behind; `#include "lkl_autoconf.h"` in lkl.h then prefers the
# source-tree neighbour over our out-of-tree one (since C resolves
# quoted includes relative to the including file first), which silently
# drops every LKL_HOST_CONFIG_* macro and breaks the lib/virtio_net.c
# guards. The file is gitignored (not part of source), so removing it
# does not violate the "don't modify ~/linux" rule.
rm -f "$LINUX_DIR/tools/lkl/include/lkl_autoconf.h"

for T in "${TARGETS_ARR[@]}"; do
    configure_target "$T" "$(cross_for "$T")"
done

echo
echo "=== Configuration complete for: ${TARGETS_ARR[*]} ==="
echo "To build, run: $(dirname "$0")/build_lkl.sh --targets=${TARGETS_REQ}"

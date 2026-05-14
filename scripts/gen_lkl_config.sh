#!/bin/bash
# Generate LKL kernel configs for anyfs-reader (out-of-tree builds).
#
# Usage: ./gen_lkl_config.sh [OPTIONS]
#
# Options:
#   --linux=DIR         Kernel source tree (default: ~/linux)
#   --out=DIR           Parent dir for build trees (default: ~/anyfs-reader)
#   --targets=LIST      Comma-separated subset of: linux-amd64,mingw32,mingw64
#                       (default: all three)
#
# Produces, under $OUT:
#   lkl-linux-amd64/    native Linux x86_64
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
    cfg -e NTFS3_FS
    cfg -e NTFS3_LZX_XPRESS
    cfg -e NTFS3_FS_POSIX_ACL

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

    # defconfig — kconfig itself doesn't need CROSS_COMPILE, and a Wine-based
    # toolchain would break $(shell,...) probes in Kconfig.
    make -C "$LINUX_DIR" ARCH=lkl O="$OUT" defconfig 2>/dev/null || true

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

    make -C "$LINUX_DIR" ARCH=lkl O="$OUT" olddefconfig 2>/dev/null

    # tools/lkl/Makefile.conf — for mingw targets we write it by hand since
    # autoconf can't probe a PE/Wine target.
    local CONF="$LKL_OUT/Makefile.conf"
    if [[ "$NAME" == mingw* ]]; then
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
  export LD := ${CROSS}ld
  export AR := ${CROSS}ar
  export LKL_HOST_CONFIG_NT=y
${NT64_CONF}
  export LKL_HOST_CONFIG_VIRTIO_NET=y
  export LKL_HOST_CONFIG_VIRTIO_NET_SLIRP=y
  KOPT += "KALLSYMS_EXTRA_PASS=1"
  KOPT += "HOSTCFLAGS=-Wno-char-subscripts"
  KOPT += "HOSTLDFLAGS=-s"
  LDLIBS += -lws2_32 -liphlpapi
  LDLIBS += -L/opt/msys2-cross/${MSYS_ARCH}/lib -lslirp
  ${LDFLAGS_EXTRA}
  EXESUF := .exe
  SOSUF := .dll
  CFLAGS += -Iinclude/mingw32
  CFLAGS += -I/opt/msys2-cross/${MSYS_ARCH}/include/slirp
EOF
        # mk_elfconfig cannot parse PE objects — pre-seed elfconfig.h.
        echo "#define KERNEL_ELFCLASS ${ELFCLASS}" > "$OUT/scripts/mod/elfconfig.h"
        cat > "$LKL_OUT/include/lkl_autoconf.h" <<EOF
#define LKL_HOST_CONFIG_NT y
${NT64_AUTOCONF}
#define LKL_HOST_CONFIG_VIRTIO_NET y
#define LKL_HOST_CONFIG_VIRTIO_NET_SLIRP y
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
    #     make defconfig          (wipes .config to arch/lkl/configs/defconfig)
    #     cat kernel.config >> .config
    #     make olddefconfig
    # Any settings we applied to .config above survive only while that rule
    # doesn't fire. To make our overrides robust to `make clean` (and to
    # avoid editing anything under $LINUX_DIR), seed kernel.config with the
    # critical feature flags that defconfig disables. olddefconfig then
    # re-derives the same .config we built here.
    cat > "$LKL_OUT/kernel.config" <<'EOF'
# Anyfs-reader overlay — re-applied on every .config rebuild.
# Must match the corresponding cfg lines in apply_common_config so a clean
# rebuild produces the same .config we hand-rolled.
CONFIG_FILE_LOCKING=y
CONFIG_FANOTIFY=y
CONFIG_NFSD=y
CONFIG_NFSD_V4=y
CONFIG_SMB_SERVER=y
# CONFIG_SMB_SERVER_CHECK_CAP_NET_ADMIN is not set
# Device Mapper / LUKS — needed by anyfs container recursion (v2).
CONFIG_MD=y
CONFIG_BLK_DEV_DM=y
CONFIG_DM_CRYPT=y
# Crypto primitives consumed by dm-crypt for LUKS.
CONFIG_CRYPTO=y
CONFIG_CRYPTO_AES=y
CONFIG_CRYPTO_XTS=y
CONFIG_CRYPTO_SHA1=y
CONFIG_CRYPTO_SHA256=y
CONFIG_CRYPTO_SHA512=y
CONFIG_CRYPTO_ESSIV=y
CONFIG_CRYPTO_HMAC=y
CONFIG_CRYPTO_CBC=y
CONFIG_CRYPTO_ECB=y
EOF

    # Bump Makefile.conf's mtime so the autoconf rule (which would wipe
    # kernel.config back to empty) does not re-fire on subsequent builds.
    touch "$LKL_OUT/Makefile.conf"
}

# Map name → cross prefix
cross_for() {
    case "$1" in
        linux-amd64) echo "" ;;
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

for T in "${TARGETS_ARR[@]}"; do
    configure_target "$T" "$(cross_for "$T")"
done

echo
echo "=== Configuration complete for: ${TARGETS_ARR[*]} ==="
echo "To build, run: $(dirname "$0")/build_lkl.sh --targets=${TARGETS_REQ}"

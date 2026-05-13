#!/bin/bash
# Generate LKL kernel config for anyfs-reader
#
# Usage: ./gen_lkl_config.sh [OPTIONS] [LINUX_DIR]
#
# Options:
#   --cross=PREFIX   Cross-compile (e.g. --cross=i686-w64-mingw32-)
#   --build          Also build the kernel after configuring
#
# Creates a .config in LINUX_DIR (default: ~/linux) with:
#   - All supported filesystems (including experimental/deprecated)
#   - NFSv4 server, SMB/KSMBD server
#   - Basic networking (IPv4/IPv6, TCP/UDP)
#   - Virtio block/net
#   - dm-crypt (LUKS), LVM (thin, cache, raid, snapshot, mirror)
#   - Full NLS/Unicode support
#   - No debug, optimized for size
#
set -e

CROSS_COMPILE=""
DO_BUILD=0
LINUX_DIR=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --cross=*) CROSS_COMPILE="${1#--cross=}"; shift ;;
        --cross)   CROSS_COMPILE="$2"; shift 2 ;;
        --build)   DO_BUILD=1; shift ;;
        -h|--help)
            echo "Usage: $0 [--cross=PREFIX] [--build] [LINUX_DIR]"
            echo ""
            echo "Options:"
            echo "  --cross=PREFIX  Cross-compile (e.g. i686-w64-mingw32-)"
            echo "  --build         Build kernel after configuring"
            echo ""
            echo "Examples:"
            echo "  $0                              # Native build config"
            echo "  $0 --cross=i686-w64-mingw32-    # Win32 cross config"
            echo "  $0 --cross=i686-w64-mingw32- --build ~/linux"
            exit 0
            ;;
        *)  LINUX_DIR="$1"; shift ;;
    esac
done

LINUX_DIR="${LINUX_DIR:-$HOME/linux}"
SCRIPTS="$LINUX_DIR/scripts/config"
MAKE_ARGS="ARCH=lkl"
[[ -n "$CROSS_COMPILE" ]] && MAKE_ARGS="$MAKE_ARGS CROSS_COMPILE=$CROSS_COMPILE"

if [[ ! -x "$SCRIPTS" ]]; then
    echo "Error: $SCRIPTS not found. Is LINUX_DIR correct?" >&2
    exit 1
fi

cd "$LINUX_DIR"

# Remove old config to avoid stale options
rm -f .config .config.old
rm -rf include/config

# Start from LKL defconfig (no CROSS_COMPILE — kconfig doesn't need it,
# and Wine-based toolchain causes $(shell,...) to fail in Kconfig)
make ARCH=lkl defconfig 2>/dev/null || true

cfg() { "$SCRIPTS" "$@"; }

echo "=== Configuring LKL kernel for anyfs-reader ==="

# ── Core / General ────────────────────────────────────────────────
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

# ── Networking ────────────────────────────────────────────────────
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

# ── Block / Virtio ────────────────────────────────────────────────
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

# ── Device Mapper / dm-crypt (LUKS) / LVM ─────────────────────────
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

# ── Crypto (needed for LUKS/dm-crypt) ────────────────────────────
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

# ── Filesystems: ext ─────────────────────────────────────────────
cfg -e EXT4_FS
cfg -e EXT4_FS_POSIX_ACL
cfg -e EXT4_FS_SECURITY

# ── Filesystems: journaling/COW ──────────────────────────────────
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

# ── Filesystems: optical/archive ─────────────────────────────────
cfg -e ISO9660_FS
cfg -e JOLIET
cfg -e ZISOFS
cfg -e UDF_FS

# ── Filesystems: FAT/Windows ─────────────────────────────────────
cfg -e MSDOS_FS
cfg -e VFAT_FS
cfg --set-val FAT_DEFAULT_UTF8 1
cfg -e EXFAT_FS
cfg -e NTFS3_FS
cfg -e NTFS3_LZX_XPRESS
cfg -e NTFS3_FS_POSIX_ACL

# ── Filesystems: misc / legacy ───────────────────────────────────
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

# ── Filesystems: network servers ──────────────────────────────────
cfg -e NFSD
cfg -e NFSD_V4
cfg -e SMB_SERVER
cfg -d SMB_SERVER_CHECK_CAP_NET_ADMIN

# ── Unicode / NLS ─────────────────────────────────────────────────
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

# ── 64-bit support ────────────────────────────────────────────────
if [[ "$CROSS_COMPILE" == x86_64* ]]; then
    cfg -e 64BIT
    cfg --set-str OUTPUT_FORMAT "pe-x86-64"
    # Enable debug info for x86_64 (MSYS2 gcc preserves it in COFF)
    cfg -d DEBUG_INFO_NONE
    cfg -e DEBUG_INFO_DWARF4
else
    # ── Disable debug for 32-bit ──────────────────────────────────
    cfg -d DEBUG_INFO
    cfg -e DEBUG_INFO_NONE
fi
cfg -d DNOTIFY

# ── Resolve dependencies ──────────────────────────────────────────
make ARCH=lkl olddefconfig 2>/dev/null

# ── Generate tools/lkl/Makefile.conf ──────────────────────────────
CONF="tools/lkl/Makefile.conf"
if [[ -n "$CROSS_COMPILE" ]] && [[ "$CROSS_COMPILE" == *mingw* || "$CROSS_COMPILE" == *cygwin* ]]; then
    # Windows cross-compile: write Makefile.conf manually since autoconf can't
    # detect the target environment
    if [[ "$CROSS_COMPILE" == x86_64* ]]; then
        ELFCLASS="ELFCLASS64"
        LDFLAGS_EXTRA="LDFLAGS += -Wl,--image-base,0x10000"
    else
        ELFCLASS="ELFCLASS32"
        LDFLAGS_EXTRA=""
    fi
    cat > "$CONF" <<EOF
  export CROSS_COMPILE := ${CROSS_COMPILE}
  export CC := ${CROSS_COMPILE}gcc
  export LD := ${CROSS_COMPILE}ld
  export AR := ${CROSS_COMPILE}ar
  export LKL_HOST_CONFIG_NT=y
  export LKL_HOST_CONFIG_VIRTIO_NET=y
  export LKL_HOST_CONFIG_VIRTIO_NET_SLIRP=y
  KOPT = "KALLSYMS_EXTRA_PASS=1"
  KOPT += "HOSTCFLAGS=-Wno-char-subscripts"
  KOPT += "HOSTLDFLAGS=-s"
  LDLIBS += -lws2_32 -liphlpapi
  LDLIBS += -L${LIBSLIRP_SRC}/build-mingw32 -lslirp-0
  ${LDFLAGS_EXTRA}
  EXESUF := .exe
  SOSUF := .dll
  CFLAGS += -Iinclude/mingw32
  CFLAGS += -I${LIBSLIRP_SRC}/build-mingw32/include
EOF
    # Pre-generate elfconfig.h since mk_elfconfig cannot parse PE objects
    echo "#define KERNEL_ELFCLASS ${ELFCLASS}" > scripts/mod/elfconfig.h
    # Generate lkl_autoconf.h for NT build
    mkdir -p tools/lkl/include
    cat > tools/lkl/include/lkl_autoconf.h <<EOF
#define LKL_HOST_CONFIG_NT y
#define LKL_HOST_CONFIG_VIRTIO_NET y
#define LKL_HOST_CONFIG_VIRTIO_NET_SLIRP y
EOF
else
    # Native POSIX build: remove stale Makefile.conf so autoconf regenerates it
    rm -f "$CONF" tools/lkl/include/lkl_autoconf.h
fi

echo ""
echo "=== Configuration complete ==="
echo "Enabled filesystems:"
grep "=y" .config | grep "_FS=\|_FS_" | sed 's/CONFIG_/  /' | sed 's/=y//' | sort
echo ""
echo "Servers: NFSD_V4, SMB_SERVER (KSMBD)"
echo "LUKS/LVM: BLK_DEV_DM, DM_CRYPT, DM_THIN, DM_RAID, DM_CACHE"
[[ -n "$CROSS_COMPILE" ]] && echo "Cross:   $CROSS_COMPILE"
echo ""

if [[ $DO_BUILD -eq 1 ]]; then
    echo "=== Building kernel ==="
    make -C tools/lkl -j$(nproc) $MAKE_ARGS
    echo ""
    echo "Build complete:"
    ls -lh tools/lkl/lib/liblkl.* 2>/dev/null || true
else
    echo "To build:"
    echo "  cd $LINUX_DIR"
    echo "  make -C tools/lkl -j\$(nproc) $MAKE_ARGS"
fi

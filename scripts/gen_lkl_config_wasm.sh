#!/bin/bash
# Generate an LKL kernel config for the WebAssembly target (browser disk viewer).
#
# Usage: ./gen_lkl_config_wasm.sh [OPTIONS]
#
# Options:
#   --linux=DIR   Kernel source tree (default: ~/linux)
#   --out=DIR     Parent dir for the build tree (default: ~/anyfs-reader)
#   --emsdk=DIR   emsdk install root (default: ~/emsdk)
#
# Produces, under $OUT:
#   lkl-wasm/             # out-of-tree kernel build dir
#     .config             # final kernel config
#     tools/lkl/
#       Makefile.conf     # hand-rolled — autoconf can't probe wasm
#       include/lkl_autoconf.h
#       kernel.config     # overlay re-applied on .config rebuild
#
# Why hand-rolled Makefile.conf: tools/lkl/Makefile.autoconf probes the host's
# CC via $(LD) -r -print-output-format; that prints "wasm" but the rest of
# autoconf only dispatches on elf*/pe* so we skip it entirely (same as the
# mingw path in the sibling gen_lkl_config.sh).
#
# To build, use the companion script: build_lkl_wasm.sh
set -e

LINUX_DIR="$HOME/linux"
OUT_PARENT="$HOME/anyfs-reader"
EMSDK_DIR="$HOME/emsdk"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --linux=*) LINUX_DIR="${1#--linux=}"; shift ;;
        --linux)   LINUX_DIR="$2"; shift 2 ;;
        --out=*)   OUT_PARENT="${1#--out=}"; shift ;;
        --out)     OUT_PARENT="$2"; shift 2 ;;
        --emsdk=*) EMSDK_DIR="${1#--emsdk=}"; shift ;;
        --emsdk)   EMSDK_DIR="$2"; shift 2 ;;
        -h|--help) sed -n '2,18p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "Unknown argument: $1" >&2; exit 1 ;;
    esac
done

SCRIPTS_CONFIG="$LINUX_DIR/scripts/config"
if [[ ! -x "$SCRIPTS_CONFIG" ]]; then
    echo "Error: $SCRIPTS_CONFIG not found. Is --linux=$LINUX_DIR correct?" >&2
    exit 1
fi
if [[ ! -d "$EMSDK_DIR/upstream/emscripten" ]]; then
    echo "Error: $EMSDK_DIR/upstream/emscripten not found. Is --emsdk=$EMSDK_DIR correct?" >&2
    exit 1
fi

# Stage OOT filesystems + wasm-only kernel patches into $LINUX_DIR. The
# script is idempotent; re-running gen_lkl_config_wasm.sh between iterations
# is safe. See scripts/oot_fs.sh.
LINUX_DIR="$LINUX_DIR" "$(dirname "$0")/oot_fs.sh" stage --wasm

NAME="wasm"
OUT="$OUT_PARENT/lkl-$NAME"
DOTCONFIG="$OUT/.config"
LKL_OUT="$OUT/tools/lkl"

echo "=============================================================="
echo "  Configuring lkl-$NAME"
echo "  OUT:   $OUT"
echo "  EMSDK: $EMSDK_DIR"
echo "=============================================================="

mkdir -p "$OUT" "$LKL_OUT/include" "$OUT/scripts/mod"

# Start fresh — but only inside the out-of-tree build dir.
rm -f "$DOTCONFIG" "$DOTCONFIG.old"
rm -rf "$OUT/include/config"

# defconfig — Kconfig's cc-objdump-file-format.sh would probe the host CC and
# default OUTPUT_FORMAT to elf64-x86-64. We override OUTPUT_FORMAT below, so
# don't bother setting CROSS_COMPILE here (same trick the mingw path uses).
make -C "$LINUX_DIR" ARCH=lkl O="$OUT" defconfig 2>/dev/null || true

# ── Apply the wasm-target feature set onto .config ─────────────────────────
cfg() { "$SCRIPTS_CONFIG" --file "$DOTCONFIG" "$@"; }

# Core / general
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
cfg -d KALLSYMS
cfg -d KALLSYMS_ALL
cfg -d COMPAT_BRK
cfg -d VM_EVENT_COUNTERS
cfg -e PRINTK_TIME

# wasm32 — explicitly 32-bit
cfg -d 64BIT
cfg --set-str OUTPUT_FORMAT "wasm32"

# Networking: OFF — disk viewer doesn't need it
cfg -d NET
cfg -d INET
cfg -d IPV6
cfg -d NETDEVICES
cfg -d WIRELESS
cfg -d ETHERNET
cfg -d WLAN

# Block / Virtio: keep just BLK + virtio_blk (kernel needs a way to add disks)
cfg -d FW_LOADER
cfg -e VIRTIO_BLK
cfg -d BLK_DEV_NVME
cfg -d VT
cfg -e VIRTIO_MMIO
cfg -e VIRTIO_MMIO_CMDLINE_DEVICES
cfg -d VIRTIO_NET

# Device Mapper / LUKS / LVM — same as native: enables anyfs container
# recursion (v2). dm-crypt unlocks LUKS, dm-linear stages inner partitions.
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

# FILE_LOCKING is implicitly needed by several FS drivers (NFSD is gone here
# but defconfig disables FILE_LOCKING under EXPERT). Re-enable so the FS layer
# isn't quietly degraded.
cfg -e FILE_LOCKING

# Filesystems: ext
cfg -e EXT4_FS
cfg -e EXT4_FS_POSIX_ACL
cfg -e EXT4_FS_SECURITY

# Filesystems: journaling / COW
cfg -e REISERFS_FS
cfg -e JFS_FS
# XFS used to be forced off on wasm because __this_address (fs/xfs/xfs_linux.h)
# relies on GCC's &&label operator, which clang's wasm32 backend rejects with
# "WebAssembly hasn't implemented computed gotos". patches/linux/wasm/
# 01-xfs-this-address-wasm.patch stubs that macro out for __wasm__ — applied
# by oot_fs.sh stage --wasm — so XFS_FS can now be enabled here too.
cfg -e XFS_FS
cfg -e BTRFS_FS
cfg -e BTRFS_FS_POSIX_ACL
cfg -e BCACHEFS_FS
cfg -e NILFS2_FS
cfg -e F2FS_FS
cfg -e GFS2_FS

# Filesystems: optical / archive
cfg -e ISO9660_FS
cfg -e JOLIET
cfg -e ZISOFS
cfg -e UDF_FS

# Filesystems: FAT / Windows
cfg -e MSDOS_FS
cfg -e VFAT_FS
cfg --set-val FAT_DEFAULT_UTF8 1
cfg -e EXFAT_FS
# NTFS3 (the in-tree driver) is replaced by NTFS PLUS — the out-of-tree
# driver at github.com/namjaejeon/linux-ntfs, staged under fs/ntfsplus/
# by scripts/oot_fs.sh stage. Both drivers register filesystem name "ntfs"
# via the shim, so NTFS3 is forced off here.
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

# No NFSD / SMB_SERVER on wasm — networking is disabled.
cfg -d NFSD
cfg -d SMB_SERVER

# Unicode / NLS — full codepage set so FAT/exFAT/NTFS handle non-ASCII names.
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

# Debug: off (smaller wasm)
cfg -e DEBUG_INFO_NONE
cfg -d DEBUG_INFO_DWARF_TOOLCHAIN_DEFAULT
cfg -d DEBUG_INFO_DWARF4
cfg -d DEBUG_INFO_DWARF5
cfg -d DEBUG_INFO

make -C "$LINUX_DIR" ARCH=lkl O="$OUT" olddefconfig 2>/dev/null

# ── Hand-rolled tools/lkl/Makefile.conf (autoconf can't probe wasm) ────────
CONF="$LKL_OUT/Makefile.conf"
cat > "$CONF" <<EOF
# WebAssembly target (emscripten). Used by tools/lkl when CC/LD/AR are
# provided as emcc/emar from build_lkl_wasm.sh. No autoconf probes — we
# set the feature flags by hand below.
export LKL_HOST_CONFIG_WASM=y
# Reuse the POSIX host path: emscripten provides a posix-shaped libc
# (sys/uio.h, sys/types.h, pthread, setjmp), so lkl_host.h's POSIX branch
# is the right starting point. wasm-specific divergences live in the
# exec_format=wasm32 branch of arch/lkl/Makefile.
export LKL_HOST_CONFIG_POSIX=y
# - lkl.h uses mode_t/dev_t without including sys/types.h (glibc pulls it
#   transitively via sys/uio.h; emscripten doesn't)
# - PTHREAD_KEYS_MAX in emscripten's limits.h needs a hosted libc; -ffreestanding
#   would gate it out even with _GNU_SOURCE, so we omit -ffreestanding entirely
#   (host code links against emscripten's libc — it's not freestanding).
# - LKL_HOST_CONFIG_POSIX/WASM are set via -D rather than lkl_autoconf.h
#   because lkl.h does \`#include "lkl_autoconf.h"\` (quoted), which prefers
#   the empty stub in the kernel source tree over our OUTPUT copy
CFLAGS += -pthread -fno-builtin -D_GNU_SOURCE \\
          -DLKL_HOST_CONFIG_POSIX=1 -DLKL_HOST_CONFIG_WASM=1 \\
          -include sys/types.h -include limits.h
LDFLAGS += -pthread
SOSUF :=
EOF

cat > "$LKL_OUT/include/lkl_autoconf.h" <<EOF
#define LKL_HOST_CONFIG_WASM y
#define LKL_HOST_CONFIG_POSIX y
EOF

# Pre-seed scripts/mod/elfconfig.h — mk_elfconfig can't parse wasm objects
# (same workaround the mingw path uses).
echo "#define KERNEL_ELFCLASS ELFCLASS32" > "$OUT/scripts/mod/elfconfig.h"

	# kernel.config overlay — auto-generated diff vs defconfig.
	# Re-applied on every .config rebuild by tools/lkl/Makefile:
	#     defconfig → cat kernel.config >> .config → olddefconfig
	# This guarantees kernel.config is always complete and in sync
	# with the cfg lines above — no hand-maintained list to drift.
	echo "  Generating kernel.config (diff vs defconfig)..."
	cp "$DOTCONFIG" "$OUT/.config.our"
	make -C "$LINUX_DIR" ARCH=lkl O="$OUT" defconfig 2>/dev/null
	comm -13 \
	  <(sort "$DOTCONFIG") \
	  <(sort "$OUT/.config.our") \
	  > "$LKL_OUT/kernel.config"
	mv "$OUT/.config.our" "$DOTCONFIG"
	echo "  kernel.config: $(wc -l < "$LKL_OUT/kernel.config") lines"

# Bump Makefile.conf's mtime so tools/lkl/Makefile.autoconf doesn't re-fire
# and wipe kernel.config (the autoconf rule truncates it).
touch "$CONF"

echo
echo "=== wasm config ready at $OUT ==="
echo "To build: scripts/build_lkl_wasm.sh"

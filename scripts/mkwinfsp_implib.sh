#!/bin/bash
# Generate MinGW import library (libwinfsp-x86.dll.a) from WinFSP headers.
# The MSVC .lib at ~/winfsp/opt/fsext/lib/ can't be consumed directly by
# MinGW ld, so we build a .def from the known FSP_FUSE_API exports and
# use dlltool to produce the import library.
#
# Usage: ./scripts/mkwinfsp_implib.sh [--arch=i686|x86_64]
#
set -euo pipefail

ARCH="${1:-i686}"
case "$ARCH" in
    i686)   MINGW_PREFIX="i686-w64-mingw32"; DLL_NAME="winfsp-x86.dll" ;;
    x86_64) MINGW_PREFIX="x86_64-w64-mingw32"; DLL_NAME="winfsp-x64.dll" ;;
    *)      echo "Unknown arch: $ARCH"; exit 1 ;;
esac

WINFSP_DIR="${WINFSP_DIR:-$HOME/winfsp}"
OUTPUT="$WINFSP_DIR/opt/fsext/lib/lib${DLL_NAME%.dll}.dll.a"
DEF_FILE="/tmp/${DLL_NAME%.dll}.def"

cat > "$DEF_FILE" <<EOF
; MinGW import library definition for ${DLL_NAME}
; Generated from WinFSP headers (fuse.h, fuse3/fuse.h, fuse_opt.h, winfsp.h)
LIBRARY ${DLL_NAME}
EXPORTS
  ; WinFSP core API (winfsp.h)
  FspFileSystemAcquire
  FspFileSystemBeginCall
  FspFileSystemCreate
  FspFileSystemDelete
  FspFileSystemEndCall
  FspFileSystemGetVolumeInfo
  FspFileSystemMount
  FspFileSystemOpendir
  FspFileSystemReaddir
  FspFileSystemReleasedir
  FspFileSystemSendResponse
  FspFileSystemSetVolumeInfo
  FspFileSystemUnmount
  FspMountPointCreate
  FspMountPointDelete
  FspServiceCreate
  FspServiceDelete
  FspServiceGetName
  FspVersion
  ; FUSE3 API (fuse3/fuse.h) — DLL exports called by static inline wrappers
  fsp_fuse3_destroy
  fsp_fuse3_exit
  fsp_fuse3_get_context
  fsp_fuse3_lib_help
  fsp_fuse3_loop
  fsp_fuse3_loop_mt
  fsp_fuse3_loop_mt_31
  fsp_fuse3_main_real
  fsp_fuse3_mount
  fsp_fuse3_new
  fsp_fuse3_new_30
  fsp_fuse3_unmount
  ; FUSE2 API (fuse/fuse.h)
  fsp_fuse_destroy
  fsp_fuse_exit
  fsp_fuse_exited
  fsp_fuse_get_context
  fsp_fuse_is_lib_option
  fsp_fuse_loop
  fsp_fuse_loop_mt
  fsp_fuse_main_real
  fsp_fuse_new
  fsp_fuse_notify
  ; FUSE opt API (fuse_opt.h)
  fsp_fuse_opt_add_arg
  fsp_fuse_opt_add_opt
  fsp_fuse_opt_add_opt_escaped
  fsp_fuse_opt_free_args
  fsp_fuse_opt_insert_arg
  fsp_fuse_opt_match
  fsp_fuse_opt_parse
EOF

echo "=== ${DLL_NAME} export symbols ==="
cat "$DEF_FILE"

echo ""
echo "=== Generating import library ==="
"$MINGW_PREFIX-dlltool" -d "$DEF_FILE" -l "$OUTPUT" -D "$DLL_NAME"

echo ""
echo "Created: $OUTPUT"
ls -lh "$OUTPUT"

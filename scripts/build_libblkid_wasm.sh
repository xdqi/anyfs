#!/usr/bin/env bash
# Thin compatibility wrapper: the canonical libblkid wasm recipe now lives in
# build_wasm_sysroot.sh (which sets the mandatory -O3 -pthread flags and the
# util-linux 2.40.4 version gate). This wrapper just delegates.
#
# Environment variables UL_SRC and SYSROOT are honoured by the delegate
# (build_wasm_sysroot.sh reads UL_SRC and SYSROOT with the same names), so
# existing callers that set those variables continue to work unchanged.
# BLD_DIR (previously accepted by this script) has no equivalent in the
# delegate; the delegate always uses WORK (default: <repo>/build-wasm-sysroot)
# for its per-library build directories.
set -euo pipefail
exec "$(dirname "$0")/build_wasm_sysroot.sh" --only=blkid "$@"

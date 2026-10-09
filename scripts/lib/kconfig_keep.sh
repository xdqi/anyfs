# shellcheck shell=bash
# Keep a cached LKL build tree incremental across config regeneration.
#
# gen_lkl_config*.sh rebuild .config from defconfig + overlay on every run.
# Deleting the old .config and kbuild's include/config/ (the per-symbol
# stamps every object's .cmd depends on) made a restored build tree
# recompile the whole kernel even when nothing changed: 1934 objects plus a
# full vmlinux link on an exact cache hit (macos.yml run 37926578359).
#
#   kconfig_keep_begin <out>  move .config and include/config/ aside
#   kconfig_keep_end <out>    after the new .config is written: if it equals
#                             the old one apart from toolchain probes, put
#                             the old file (with its mtime) and
#                             include/config/ back; otherwise drop them, and
#                             kbuild resyncs as before.

kconfig_keep_begin() {
    local out="$1"
    rm -rf "$out/.config.keep" "$out/include/config.keep" "$out/.config.old"
    if [[ -f "$out/.config" ]]; then mv "$out/.config" "$out/.config.keep"; fi
    if [[ -d "$out/include/config" ]]; then mv "$out/include/config" "$out/include/config.keep"; fi
    return 0
}

# Toolchain-probe symbols: Kconfig computes them from the compiler it finds,
# and kbuild re-syncs them against the build's real toolchain anyway. The
# config step and the build may see different tools (mingw64: gen sees the
# runner's rustc, the build does not, so .config came back with
# RUSTC_VERSION=0 and never matched again). Header comments name the
# compiler too.
kconfig_keep_norm() {
    grep -vE '^#( [^C]|$)|^# Compiler' "$1" |
        grep -vE '^(# )?CONFIG_(CC_VERSION_TEXT|CC_IS_[A-Z]+|GCC_VERSION|CLANG_VERSION|AS_IS_[A-Z]+|AS_VERSION|LD_IS_[A-Z]+|LD_VERSION|LLD_VERSION|RUSTC_[A-Z0-9_]+|RUST_IS_AVAILABLE|BINDGEN_VERSION_TEXT|PAHOLE_VERSION|CC_CAN_[A-Z0-9_]+|CC_HAS_[A-Z0-9_]+|AS_HAS_[A-Z0-9_]+|LD_CAN_[A-Z0-9_]+|LD_HAS_[A-Z0-9_]+|TOOLS_SUPPORT_[A-Z0-9_]+|GCC_ASM_[A-Z0-9_]+|GCC_SUPPORTS_[A-Z0-9_]+)[= ]' || true
}

kconfig_keep_end() {
    local out="$1"
    if [[ -f "$out/.config.keep" ]] &&
        cmp -s <(kconfig_keep_norm "$out/.config") <(kconfig_keep_norm "$out/.config.keep"); then
        mv "$out/.config.keep" "$out/.config"
        if [[ -d "$out/include/config.keep" ]]; then
            rm -rf "$out/include/config"
            mv "$out/include/config.keep" "$out/include/config"
        fi
        echo "  .config unchanged: kept the build tree's config stamps"
    else
        rm -rf "$out/.config.keep" "$out/include/config.keep"
    fi
}

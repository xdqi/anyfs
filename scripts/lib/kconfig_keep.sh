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
#   kconfig_keep_end <out>    after the new .config is written: if it is
#                             byte-identical, put the old file (with its
#                             mtime) and include/config/ back; otherwise
#                             drop them, and kbuild resyncs as before.

kconfig_keep_begin() {
    local out="$1"
    rm -rf "$out/.config.keep" "$out/include/config.keep" "$out/.config.old"
    if [[ -f "$out/.config" ]]; then mv "$out/.config" "$out/.config.keep"; fi
    if [[ -d "$out/include/config" ]]; then mv "$out/include/config" "$out/include/config.keep"; fi
    return 0
}

kconfig_keep_end() {
    local out="$1"
    if [[ -f "$out/.config.keep" ]] && cmp -s "$out/.config" "$out/.config.keep"; then
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

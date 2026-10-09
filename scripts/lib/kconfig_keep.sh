# shellcheck shell=bash
# Keep a cached LKL build tree incremental across config regeneration.
#
# gen_lkl_config*.sh rebuild .config from defconfig + overlay on every run.
# Deleting the old .config and kbuild's include/config/ (the per-symbol
# stamps every object's .cmd depends on) made a restored build tree
# recompile the whole kernel even when nothing changed: 1934 objects plus a
# full vmlinux link on an exact cache hit (macos.yml run 37926578359).
#
# The generated .config cannot simply be compared with the tree's .config:
# the build re-syncs it against the real toolchain (mingw64: the cygwin
# compiler's version, no compressed debug info, LKL_HOST_MEM* off, no
# rustc), so it never matches what the config step writes. Instead the
# config step's own output is kept as .config.gen next to it.
#
#   kconfig_keep_begin <out>  move .config and include/config/ aside
#   kconfig_keep_end <out>    after the new .config is written: if it equals
#                             the previous run's .config.gen, put the old
#                             (build-synced) .config, with its mtime, and
#                             include/config/ back; otherwise drop them, and
#                             kbuild resyncs as before. Records .config.gen.

kconfig_keep_begin() {
    local out="$1"
    rm -rf "$out/.config.keep" "$out/include/config.keep" "$out/.config.old"
    if [[ -f "$out/.config" ]]; then mv "$out/.config" "$out/.config.keep"; fi
    if [[ -d "$out/include/config" ]]; then mv "$out/include/config" "$out/include/config.keep"; fi
    return 0
}

kconfig_keep_end() {
    local out="$1"
    # A tree saved before .config.gen existed: its own .config may still
    # match (linux-amd64, macOS targets).
    local ref="$out/.config.gen"
    [[ -f "$ref" ]] || ref="$out/.config.keep"
    if [[ -f "$out/.config.keep" && -f "$ref" ]] && cmp -s "$out/.config" "$ref"; then
        cp "$out/.config" "$out/.config.gen"
        mv "$out/.config.keep" "$out/.config"
        if [[ -d "$out/include/config.keep" ]]; then
            rm -rf "$out/include/config"
            mv "$out/include/config.keep" "$out/include/config"
        fi
        echo "  .config unchanged: kept the build tree's config stamps"
        return 0
    fi
    if [[ -f "$out/.config.gen" ]]; then
        echo "  .config changed since the last config run (kbuild resyncs):"
        diff "$out/.config.gen" "$out/.config" | head -40 | sed 's/^/    /' || true
    elif [[ -f "$out/.config.keep" ]]; then
        echo "  no record of the last config run in this tree (kbuild resyncs)"
    fi
    cp "$out/.config" "$out/.config.gen"
    rm -rf "$out/.config.keep" "$out/include/config.keep"
}

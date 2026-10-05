# shellcheck shell=bash
# scripts/lib/qemu_patches.sh — apply patches/qemu/*.patch to a QEMU source
# tree idempotently (same mechanism as oot_fs.sh): a patch that applies
# forward is applied; one whose reverse applies is already in place; anything
# else is a hard error.
#
# Which patches a build applies:
#   - wasm (build_qemu_wasm.sh): every patches/qemu/*.patch.
#   - native (build_qemu.sh):    only those listed in patches/qemu/series.native
#     (the rest are emscripten-only).

# qemu_apply_patch <qemu-src> <patch-file>
qemu_apply_patch() {
    local src="$1" p="$2" name
    name="$(basename "$p")"
    if (cd "$src" && patch -p1 --dry-run --silent < "$p") >/dev/null 2>&1; then
        (cd "$src" && patch -p1 --silent < "$p")
        echo "applied qemu patch: $name"
    elif (cd "$src" && patch -p1 -R --dry-run --silent < "$p") >/dev/null 2>&1; then
        echo "qemu patch already applied: $name"
    else
        echo "qemu patch $name neither applies forward nor is already applied" >&2
        return 1
    fi
}

# qemu_apply_series <qemu-src> <series-file>
# Applies each non-comment line of <series-file>, resolved next to it.
qemu_apply_series() {
    local src="$1" series="$2" dir line
    dir="$(dirname "$series")"
    while IFS= read -r line; do
        [[ -z "$line" || "$line" == \#* ]] && continue
        qemu_apply_patch "$src" "$dir/$line" || return 1
    done < "$series"
}

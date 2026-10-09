# llvm_tool NAME: print the first of NAME-20, NAME-19 and NAME on PATH (the
# LLVM binutils the macOS build reads Mach-O with: llvm-nm, llvm-otool,
# llvm-objdump, llvm-ar, llvm-strip), or fail. An environment override, as
# NM=... for the tool lookups in the scripts, goes before the call.
# Sourced by the macOS build and check scripts; assign the result to a
# variable (v="$(llvm_tool llvm-nm)"), so set -e sees a failure.
# shellcheck shell=bash

llvm_tool() {
    local c
    for c in "$1-20" "$1-19" "$1"; do
        if command -v "$c" > /dev/null; then
            echo "$c"
            return 0
        fi
    done
    echo "llvm_tool: none of $1-20, $1-19, $1 on PATH (install LLVM 19 or 20)" >&2
    return 1
}

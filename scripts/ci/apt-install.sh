#!/usr/bin/env bash
# apt-get install for CI runners, with the .deb archive in a directory that
# actions/cache keeps, and bounded, retried downloads. The Azure Ubuntu
# mirror sometimes serves a few packages at ~20 kB/s: mingw64 run
# 37926578369 spent 471 s on libssl, and macos run 37944321276 spent 4.5 min
# on gcc-15-aarch64-linux-gnu and was still downloading LLVM at 21 min.
# Downloading is retried under a timeout (safe to interrupt); installing then
# runs from the local archive without a timeout, so dpkg is never killed.
#
# Usage: apt-install.sh <package>...   (APT_ARCHIVES overrides the directory)
set -euo pipefail

archives="${APT_ARCHIVES:-$HOME/.cache/apt-archives}"
mkdir -p "$archives/partial"
opts=(-o "Dir::Cache::Archives=$archives" -o Acquire::Retries=3
      -o Acquire::http::Timeout=30 -o Acquire::https::Timeout=30)

for i in 1 2 3; do
    timeout 180 sudo apt-get update && break
    echo "apt-install: apt-get update attempt $i failed or timed out"
    [[ $i -lt 3 ]] || exit 1
done
for i in 1 2 3; do
    if timeout 300 sudo apt-get "${opts[@]}" install -y --no-install-recommends --download-only "$@"; then
        break
    fi
    echo "apt-install: download attempt $i failed or timed out after 300 s"
    [[ $i -lt 3 ]] || exit 1
done
sudo apt-get "${opts[@]}" install -y --no-install-recommends "$@"
sudo rm -rf "$archives/partial"/* "$archives/lock"
sudo chown -R "$(id -u):$(id -g)" "$archives"

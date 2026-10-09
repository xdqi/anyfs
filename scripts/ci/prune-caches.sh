#!/usr/bin/env bash
# Delete this repository's older GitHub Actions caches on main, keeping the
# newest <keep> entries per key prefix. Build-tree and sccache entries are
# keyed by their inputs, so each change leaves the previous entry behind;
# without pruning the repository sits at the 10 GB limit and GitHub evicts
# by last use, which can hit the trees that are still current.
#
# Usage: prune-caches.sh <keep> <key-prefix>...
# Needs GH_TOKEN with actions: write and GITHUB_REPOSITORY.
set -euo pipefail

keep="${1:?usage: prune-caches.sh <keep> <key-prefix>...}"
shift
for prefix in "$@"; do
    gh cache list -R "$GITHUB_REPOSITORY" --ref refs/heads/main --key "$prefix" \
        --sort created_at --order desc --limit 100 \
        --json id,key,sizeInBytes,createdAt \
        --jq '.[] | "\(.id) \(.sizeInBytes) \(.createdAt) \(.key)"' |
        tail -n +"$((keep + 1))" |
        while read -r id size created key; do
            echo "prune-caches: delete $key ($((size / 1048576)) MB, $created)"
            gh cache delete "$id" -R "$GITHUB_REPOSITORY" || true
        done
done

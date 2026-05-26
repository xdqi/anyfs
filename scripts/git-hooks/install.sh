#!/usr/bin/env bash
# Install the in-tree git hooks by pointing core.hooksPath at this dir.
#
# Run once per clone:
#   ./scripts/git-hooks/install.sh
#
# To revert:
#   git config --unset core.hooksPath

set -euo pipefail

REPO_ROOT="$(git rev-parse --show-toplevel)"
HOOKS_DIR="$REPO_ROOT/scripts/git-hooks"

chmod +x "$HOOKS_DIR/pre-commit"
git -C "$REPO_ROOT" config core.hooksPath scripts/git-hooks

echo "[hooks] core.hooksPath = $(git -C "$REPO_ROOT" config core.hooksPath)"
echo "[hooks] enabled: pre-commit (clang-format + prettier on staged files)"
echo "[hooks] bypass with: SKIP_FORMAT_HOOK=1 git commit ..."

#!/bin/bash
# Run a GitHub Actions workflow locally with `act`
# (https://github.com/nektos/act). Useful for debugging CI failures without
# burning push cycles. Uses the catthehacker images so the toolchain (apt
# universe, sudo, git, curl, gcc) is close to a real GitHub-hosted runner.
#
# Usage: ./scripts/run_action_local.sh [OPTIONS] [WORKFLOW]
#
#   WORKFLOW            File under .github/workflows/ (default: linux.yml).
#                       Pass just the basename — e.g. `linux.yml` or `linux`.
#
# Options:
#   --job NAME          Only run job NAME (default: all jobs in WORKFLOW)
#   --image SIZE        Runner image to use, one of:
#                         small  — catthehacker/ubuntu:act-latest  (~1 GB)
#                         medium — catthehacker/ubuntu:act-22.04   (~5 GB,  jammy)
#                         24     — catthehacker/ubuntu:act-24.04   (~5 GB,  noble)
#                         large  — catthehacker/ubuntu:full-22.04  (~70 GB, jammy,
#                                  preinstalled Node/Python/etc.)
#                         large24 — catthehacker/ubuntu:full-24.04 (~70 GB, noble)
#                       Default: 24 (matches our linux.yml `runs-on: ubuntu-24.04`).
#   --image-ref REF     Pass a fully-qualified image reference instead of a
#                       size keyword (e.g. `mylocal/wrapper:noble-fixed`).
#   --event EVENT       Trigger event (default: push). Other useful: pull_request,
#                       workflow_dispatch.
#   --reuse             Pass `--reuse` to act so containers persist across
#                       runs (much faster iteration; otherwise act tears
#                       them down each invocation).
#   --pull              Force `docker pull` of the runner image before run.
#   --secret KEY=VAL    Add a secret (may be repeated). Things like
#                       GITHUB_TOKEN go here if the workflow needs them.
#   --                  Forward remaining args verbatim to `act`.
#   -h, --help
#
# Caveats:
#   - Requires Docker and `act` (https://github.com/nektos/act).
#     Install with: curl -fsSL https://raw.githubusercontent.com/nektos/act/master/install.sh | bash -s -- -b ~/.local/bin
#   - `runs-on: ubuntu-24.04` is remapped to the chosen image. The mapping
#     is set per-invocation via -P, so workflow files do not need editing.
#   - `actions/cache@v4` becomes a no-op under act (data path differs); first
#     run will rebuild everything that the real CI would have cached.
#   - Some pre-installed tools the GitHub-hosted image bundles (e.g.
#     `gh`, `aws`) may be absent from the small/medium images. Use
#     --image large for highest fidelity at the cost of pull time.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

WORKFLOW="linux.yml"
JOB=""
IMAGE_SIZE="24"
IMAGE_REF=""
EVENT="push"
EXTRA_ACT_ARGS=()
SECRETS=()
REUSE=0
PULL=0

usage() {
    awk 'NR==1{next} /^#/{sub(/^# ?/,""); print; next} {exit}' "$0"
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --job)    JOB="$2"; shift 2 ;;
        --job=*)  JOB="${1#--job=}"; shift ;;
        --image)  IMAGE_SIZE="$2"; shift 2 ;;
        --image=*) IMAGE_SIZE="${1#--image=}"; shift ;;
        --image-ref)   IMAGE_REF="$2"; shift 2 ;;
        --image-ref=*) IMAGE_REF="${1#--image-ref=}"; shift ;;
        --event)  EVENT="$2"; shift 2 ;;
        --event=*) EVENT="${1#--event=}"; shift ;;
        --secret) SECRETS+=("--secret" "$2"); shift 2 ;;
        --secret=*) SECRETS+=("--secret" "${1#--secret=}"); shift ;;
        --reuse)  REUSE=1; shift ;;
        --pull)   PULL=1; shift ;;
        --)       shift; EXTRA_ACT_ARGS+=("$@"); break ;;
        -h|--help) usage; exit 0 ;;
        -*) echo "Unknown option: $1" >&2; usage >&2; exit 2 ;;
        *)  WORKFLOW="$1"; shift ;;
    esac
done

# Accept either `linux` or `linux.yml`.
[[ "$WORKFLOW" == *.yml || "$WORKFLOW" == *.yaml ]] || WORKFLOW="${WORKFLOW}.yml"

WORKFLOW_PATH="$REPO_ROOT/.github/workflows/$WORKFLOW"
if [[ ! -f "$WORKFLOW_PATH" ]]; then
    echo "ERROR: workflow not found: $WORKFLOW_PATH" >&2
    echo "       Available:" >&2
    ls -1 "$REPO_ROOT/.github/workflows/" 2>/dev/null | sed 's/^/         /' >&2
    exit 2
fi

if [[ -n "$IMAGE_REF" ]]; then
    IMAGE="$IMAGE_REF"
else
    case "$IMAGE_SIZE" in
        small)   IMAGE="catthehacker/ubuntu:act-latest" ;;
        medium)  IMAGE="catthehacker/ubuntu:act-22.04" ;;
        24)      IMAGE="catthehacker/ubuntu:act-24.04" ;;
        large)   IMAGE="catthehacker/ubuntu:full-22.04" ;;
        large24) IMAGE="catthehacker/ubuntu:full-24.04" ;;
        *) echo "Unknown --image: $IMAGE_SIZE (expected small|medium|24|large|large24)" >&2; exit 2 ;;
    esac
fi

if ! command -v act >/dev/null 2>&1; then
    if [[ -x "$HOME/.local/bin/act" ]]; then
        export PATH="$HOME/.local/bin:$PATH"
    else
        echo "ERROR: 'act' not in PATH. Install with:" >&2
        echo "  curl -fsSL https://raw.githubusercontent.com/nektos/act/master/install.sh | bash -s -- -b ~/.local/bin" >&2
        exit 2
    fi
fi

if ! docker info >/dev/null 2>&1; then
    echo "ERROR: docker daemon not reachable. act needs Docker." >&2
    exit 2
fi

# Map every `runs-on` label this repo's workflows actually use to the chosen
# image. Keep this list in sync as workflows are added.
PLATFORMS=(
    -P "ubuntu-latest=$IMAGE"
    -P "ubuntu-24.04=$IMAGE"
    -P "ubuntu-22.04=$IMAGE"
    -P "ubuntu-20.04=$IMAGE"
)

ACT_ARGS=(
    "$EVENT"
    -W "$WORKFLOW_PATH"
    "${PLATFORMS[@]}"
    # Persist actions/upload-artifact outputs to a host-visible dir so we can
    # inspect them post-run. Mirrors `actions/upload-artifact@v4` outputs.
    --artifact-server-path "/tmp/act-artifacts"
)
[[ -n "$JOB" ]] && ACT_ARGS+=(-j "$JOB")
[[ $REUSE -eq 1 ]] && ACT_ARGS+=(--reuse)
# act defaults to --pull=true (force-pulls every run, breaks local-only images
# like our security.ubuntu.com workaround wrapper). Flip the default so the
# wrapper only pulls when the user explicitly asks.
if [[ $PULL -eq 1 ]]; then
    ACT_ARGS+=(--pull=true)
else
    ACT_ARGS+=(--pull=false)
fi
ACT_ARGS+=("${SECRETS[@]}" "${EXTRA_ACT_ARGS[@]}")

mkdir -p /tmp/act-artifacts

echo "=== act: $WORKFLOW${JOB:+ (job=$JOB)} on $IMAGE ==="
echo "  cmd: act ${ACT_ARGS[*]}"
echo "  repo: $REPO_ROOT"
echo

cd "$REPO_ROOT"
exec act "${ACT_ARGS[@]}"

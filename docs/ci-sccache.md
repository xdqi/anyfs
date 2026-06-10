# CI compile farm (sccache-dist)

The `linux` and `mingw64` workflows can distribute C compilation across an
ephemeral farm of GitHub Actions runners connected over Tailscale, using
[`xdqi/sccache-dist-action`](https://github.com/xdqi/sccache-dist-action)
(currently `@v0.0.4`). The farm is strictly best-effort: every degradation
path ends in a normal local compile and a green build.

## Required secret

| Secret | Where | What |
|---|---|---|
| `TS_OAUTH_SECRET` | `xdqi/anyfs` **and** `xdqi/llvm-wasm` | Tailscale OAuth client secret with the `auth_keys` write scope, advertising tag `tag:ci-sccache` |

The OAuth client secret is used directly as a Tailscale auth key (this works
as long as tags are advertised). Without it the farm cannot form.

> Status 2026-06-10: the secret is **not yet configured on either repo**
> (`gh secret list` is empty for both). All farm steps therefore skip and
> every build compiles locally. Add the secret to turn the farm on — no
> workflow change needed.

## Topology

Per workflow run (`linux.yml`, `mingw64.yml`):

- **2 worker jobs** (`sccache-workers`, matrix `idx: [1, 2]`) — each runs
  `sccache-dist-action` in `worker` mode and serves until the coordinator
  goes offline.
- **Coordinator inside the build job** — `mode: coordinator`,
  `expected-workers: 2`, `min-workers: 1`, `wait-timeout: 180s`. It brings
  up the scheduler + local sccache client and puts the `sccache` engine
  binary on `PATH`.
- The build steps then opt in with `--cc="sccache gcc"` (linux) or
  `--cc="sccache /opt/msys2-cross/bin/x86_64-w64-mingw32-gcc"` (mingw64 —
  absolute path so sccache-dist hashes and ships the msys2-cross toolchain,
  not whatever the name resolves to on a worker).

The planned wasm-ld release pipeline on `xdqi/llvm-wasm` uses **3 workers**
(LLVM is a much larger compile).

## Fallback contract

The farm must never break a build. Three independent layers guarantee that:

1. **PRs skip the farm entirely.** All farm jobs/steps carry
   `github.event_name != 'pull_request'`. Fork PRs have no secrets, and
   same-repo PRs don't need the farm for cache-warm builds. PR checks
   validate the plain local-compile path.
2. **Missing secret skips the farm.** All farm steps are additionally gated
   on `TS_OAUTH_SECRET` being non-empty (laundered through job-level env as
   `FARM_SECRET_SET` — the `secrets` context is not allowed in `if`
   expressions; the workflow fails to parse if you try). Worker jobs run a
   single notice step and exit green.
3. **Coordinator failure degrades to local compile.** The coordinator step
   is `continue-on-error: true`, and every build step uses the
   `command -v sccache` contract:

   ```sh
   CC_ARGS=()
   if command -v sccache >/dev/null 2>&1; then
     CC_ARGS=(--cc="sccache gcc")
   fi
   ```

   If the coordinator died after putting the engine on `PATH`, the build
   still works — sccache falls back to local compilation (with its local
   disk cache); if `sccache` never made it onto `PATH`, the build uses plain
   `gcc`. Both observed working.

## First real-Actions results (2026-06-10, branch `build-and-test-hardening`)

No `TS_OAUTH_SECRET` was configured, so these runs validate the degradation
paths rather than actual distribution:

- **linux** — build job green end-to-end (C unit suite, wasm export gate,
  qcow2 smoke test, artifact). In the first dispatch (before the secret
  gate existed) the coordinator failed fast (`config: oauth-secret is
  required`) *after* putting the engine on `PATH`, so the build ran
  `sccache gcc` in local mode: 113 compile requests, 0 hits / 63 misses,
  cache on local disk — confirming layer 3 works in practice. Worker jobs
  failed on the missing secret and marked the run red, which is why layer 2
  (clean skip) was added. After the gate landed, a second dispatch was
  fully green: farm steps skipped, worker jobs green no-ops, build on
  plain `gcc`.
- **mingw64** — never reached the farm: the msys2-cross toolchain install
  fails with `error: target not found: msys-cross-pkgconfig`. The
  msys.kosaka.moe pacman repo currently doesn't serve that package (repo
  content regression on the msys2-cross side; `main` only stays green via a
  stale `actions/cache` of the toolchain). **Cross-distribution of the
  mingw64 toolchain remains unproven**; the leg is marked experimental in
  the workflow. Until the repo regression is fixed and the secret is added,
  mingw64 compiles locally (or fails at toolchain install on a cache miss —
  unrelated to the farm).
- **ts** — no farm (pure TS unit suites). First runs surfaced two CI-only
  bugs (workspace `file:` dependency on a sibling `drivelist-anyfs`
  checkout + `@anyfs/native`'s node-gyp script needing the LKL tree;
  missing gitignored nbd-proxy qcow2 fixture); fixed by excluding both
  packages from the CI install and generating the fixture on the runner.
  Green after the fixes.

## wasm-ld release pipeline (xdqi/llvm-wasm)

The browser bundle needs a patched `wasm-ld`. The fork branch
`ci/wasm-ld-release` on `xdqi/llvm-wasm` carries a `wasm-ld-release.yml`
workflow that builds it with `zig cc` (target gnu.2.11 for old-glibc
compatibility) on a 3-worker farm and publishes a tagged release (e.g.
`wasm-ld-18.1.2-anyfs-r1`).

Prerequisites before it can run:

1. **Actions must be enabled manually on the fork** — forks have Actions
   disabled by default and the API exposes no workflows until a human
   clicks "I understand my workflows, go ahead and enable them" in the
   Actions tab. As of 2026-06-10 this has not been done
   (`gh api repos/xdqi/llvm-wasm/actions/workflows` → `total_count: 0`),
   so the release run could not be triggered.
2. `TS_OAUTH_SECRET` on the fork (see above) — without it the farm-backed
   build would be impractically slow.

Trigger once both are in place:

```sh
gh workflow run wasm-ld-release.yml -R xdqi/llvm-wasm \
  --ref ci/wasm-ld-release -f tag=wasm-ld-18.1.2-anyfs-r1
```

## Known warts

- **No remote cache backend.** The action's engine is built without the S3
  feature, so `[cache.s3]` config would silently fall back to local disk.
  The object cache is therefore per-runner local disk, persisted between
  runs via `actions/cache` (`sccache-<target>-<sha>` with prefix
  restore-keys). Cross-run sharing is only as good as the GitHub cache.
- **mingw64 distribution is experimental** (see above): shipping the
  msys2-cross toolchain to workers is supported by the engine in principle
  but has not yet been exercised end-to-end.
- The first-dispatch behavior (failed coordinator still leaving `sccache`
  on `PATH`) means "farm down" and "farm absent" take slightly different
  code paths; both are green, but log output differs (`sccache stats` vs
  none).

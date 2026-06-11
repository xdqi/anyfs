# CI compile farm (sccache-dist)

The `linux` and `mingw64` workflows can distribute C compilation across an
ephemeral farm of GitHub Actions runners connected over Tailscale, using
[`xdqi/sccache-dist-action`](https://github.com/xdqi/sccache-dist-action)
(currently `@v0.0.5` — pin v0.0.5 or later, see warts). The farm is strictly
best-effort: every degradation path ends in a normal local compile and a
green build.

## Required secret

| Secret | Where | What |
|---|---|---|
| `TS_OAUTH_SECRET` | `xdqi/anyfs` **and** `xdqi/llvm-wasm` | Tailscale OAuth client secret with the `auth_keys` write scope, owning tag `tag:ci` |

The OAuth client secret is used directly as a Tailscale auth key (this works
as long as tags are advertised). Without it the farm cannot form.

**Every farm step must pass `tags: 'tag:ci'`.** The tailnet OAuth client
owns `tag:ci`, not the action's default `tag:ci-sccache`; with the default
tag the auth key is rejected and workers silently fail to join (found and
fixed live on the llvm-wasm leg). Both anyfs workflows and the llvm-wasm
release workflow set this explicitly.

> Status 2026-06-10: the secret is configured on both repos and the farm is
> live (see results below).

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

The wasm-ld release pipeline on `xdqi/llvm-wasm` uses **10 workers**
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

Historical: no `TS_OAUTH_SECRET` was configured yet, so these runs validated
the degradation paths rather than actual distribution:

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
  failed with `error: target not found: msys-cross-pkgconfig`. Not a repo
  regression after all — the msys.kosaka.moe repo had moved to per-target
  pkgconf packages; fixed by renaming the package to
  `msys-cross-mingw64-pkgconf` in the workflow (`5b26d20`), after which the
  PR leg went green (20m26s cold). Cross-distribution was proven later, see
  below.
- **ts** — no farm (pure TS unit suites). First runs surfaced two CI-only
  bugs (workspace `file:` dependency on a sibling `drivelist-anyfs`
  checkout + `@anyfs/native`'s node-gyp script needing the LKL tree;
  missing gitignored nbd-proxy qcow2 fixture); fixed by excluding both
  packages from the CI install and generating the fixture on the runner.
  Green after the fixes.

## Farm-mode results (2026-06-11, `workflow_dispatch` on `build-and-test-hardening`)

With `TS_OAUTH_SECRET` configured and the action at `@v0.0.5` +
`tags: 'tag:ci'`, both legs were dispatched (PR runs still skip the farm by
design and validate the fallback path — all three PR checks at `5b26d20`
are green: ts 39s, linux 8m07s, mingw64 20m26s cold):

- **linux** (run 27314323591, 3m22s, green) — both workers registered
  (`[coord] 2/2 workers registered`), LKL/QEMU/anyfs build steps ran with
  `--cc="sccache gcc"`, and `sccache stats` showed 47 compile requests with
  38 cache hits / 0 misses (100% hit rate — the local-disk cache restored
  via `actions/cache` was fully warm, so there was nothing left to
  distribute; 0 failed distributed compilations). Distribution at scale on
  this engine is proven by the llvm-wasm leg below.
- **mingw64** (run 27314325180, ~5m45s build job, green) — both workers
  registered, build steps used the absolute-path
  `sccache /opt/msys2-cross/bin/x86_64-w64-mingw32-gcc`, and **26 jobs were
  successfully executed on a worker** (0 failed distributed, 0 dist
  errors) — first end-to-end proof that the msys2-cross toolchain ships to
  and runs on farm workers. Caveat: LKL/QEMU build dirs were
  `actions/cache`-warm, so the distributed jobs were mostly meson/configure
  feature probes (26 of 27 compilations were expected-failure checks); a
  large cold mingw64 build has not yet exercised distribution at volume,
  which is why the leg stays marked experimental.

## wasm-ld release pipeline (xdqi/llvm-wasm)

The browser bundle needs a patched `wasm-ld`. The fork branch
`ci/wasm-ld-release` on `xdqi/llvm-wasm` carries a `wasm-ld-release.yml`
workflow that builds it with `zig cc` (target gnu.2.11 for old-glibc
compatibility) on a 10-worker farm and publishes a tagged release.

**Status: shipped.** Release
[`wasm-ld-18.1.2-anyfs-r1`](https://github.com/xdqi/llvm-wasm/releases/tag/wasm-ld-18.1.2-anyfs-r1)
(asset `wasm-ld-linux-amd64.tar.xz`) is live and consumed by
`scripts/fetch_wasm_ld.sh` (wired through the config `.toolchain` hook,
commit `6086876`).

The release run (27290137879, 2026-06-10) is also the large-scale proof of
the farm: building lld took **27.5 min** end to end with **1486
successfully distributed compiles spread across all 10 workers** (per-worker
counts 78–339), 172 failed-distributed jobs that fell back to local compile,
and 1787 compile requests total. This run is also where the `tag:ci`
requirement was discovered: with the action-default `tag:ci-sccache` no
worker could join; after switching to `tags: 'tag:ci'` the farm formed
(coordinator proceeded at 4/10 registered — `min-workers` — and the
remaining workers joined and served jobs during the build).

Re-trigger if a new wasm-ld is needed:

```sh
gh workflow run wasm-ld-release.yml -R xdqi/llvm-wasm \
  --ref ci/wasm-ld-release -f tag=wasm-ld-18.1.2-anyfs-r2
```

## Known warts

- **Pin `@v0.0.5` or later.** Earlier action versions shipped an engine
  built without the S3 feature, so `[cache.s3]` config silently fell back
  to local disk; v0.0.5 fixed the engine. The anyfs workflows still use
  per-runner local-disk cache persisted via `actions/cache`
  (`sccache-<target>-<sha>` with prefix restore-keys), so cross-run sharing
  is only as good as the GitHub cache.
- **`tags: 'tag:ci'` is mandatory** (see "Required secret"): the OAuth
  client owns `tag:ci`, and the action default `tag:ci-sccache` makes
  worker join fail silently.
- **mingw64 distribution is experimental**: toolchain shipping + remote
  execution is now proven (26 distributed jobs, 2026-06-11 dispatch), but
  only at configure-probe volume; a cold full build over the farm is still
  unexercised.
- The first-dispatch behavior (failed coordinator still leaving `sccache`
  on `PATH`) means "farm down" and "farm absent" take slightly different
  code paths; both are green, but log output differs (`sccache stats` vs
  none).

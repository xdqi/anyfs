# ghcr web image CI design

**Date:** 2026-07-07
**Status:** approved (architecture, Dockerfile, CI job, tag mapping, smoke test)
**Workflow file:** `.github/workflows/wasm.yml` (extends existing)
**Dockerfile:** `ts/examples/vite-demo/Dockerfile` (new)

## Goal

The `wasm.yml` workflow already produces a complete, self-contained web
package artifact (`anyfs-web-dist` — the vite-demo build with the wasm bundle
synced in, plus `Caddyfile`). This design adds a job that bakes that dist into
a Docker image and pushes it to ghcr, so deployment is `docker run` + serve
static files.

## Context

- `wasm.yml`'s `web-package` job (`.github/workflows/wasm.yml:217`) already:
  builds `@anyfs/core` + `react` + `trees`, syncs the browser wasm bundle into
  `vite-demo/public/wasm/` via `scripts/sync_wasm_bundle.sh`, runs
  `pnpm --filter vite-demo build`, and tars the dist into `anyfs-web-dist`.
- The dist tarball includes `Caddyfile` (`public/Caddyfile` rides along into
  `dist/`), which binds `:4173`, sets `Cross-Origin-Opener-Policy:
  same-origin` + `Cross-Origin-Embedder-Policy: require-corp`, declares the
  `.webmanifest` MIME, and ships `Service-Worker-Allowed: /` for the streaming
  download SW — i.e. everything needed to serve the static files correctly.
- Latest green run (`075bb17`, 2026-06-23): `anyfs-web-dist` = 25.9 MB.
- No existing Dockerfile or ghcr config in the repo.

## Decisions (from design review)

| Question | Decision |
|---|---|
| Registry/package name | `ghcr.io/xdqi/anyfs-web` (image lives under source repo `xdqi/anyfs` via `github.repository_owner`, package name `anyfs-web`) |
| Tag strategy | `latest` + `sha-<short>` + git tag (`v*` → semver) |
| Triggers | `push: [main]` + `push: tags: [v*]` + `workflow_dispatch`; non-main branches do not run the wasm chain |
| `latest` movement | Only on `refs/heads/main`; tag pushes never move `latest` |
| Base image | `caddy:2-alpine` (reuses the existing `public/Caddyfile` verbatim) |
| Multi-arch | `linux/amd64,linux/arm64` — the Dockerfile is `COPY`-only, so each platform is built without QEMU emulation |
| electron dependency | `ghcr-image` needs only `web-package`; runs parallel to `electron-package`; electron failure does not block image publication |
| Build cache | None — the only variable layer is the dist content (changes every build via wasm hash), so GHA cache can never hit; base `caddy:2-alpine` already served via Docker Hub |

## Architecture

```
wasm-build ──→ web-package ──┬──→ electron-package (matrix)
                              └──→ ghcr-image        (NEW)
```

`ghcr-image` is a parallel sibling to `electron-package`, both needing only
`web-package`. It consumes the existing `anyfs-web-dist` artifact — no wasm
rebuild, no vite rebuild. An image push failure is independent of electron
and retryable on its own.

## Dockerfile

Single-stage, repo-tracked at `ts/examples/vite-demo/Dockerfile`. The build
context is the unpacked `anyfs-web-dist` tarball (dist content + `Caddyfile`),
NOT a repo checkout:

```dockerfile
# Serves the pre-built vite-demo static dist. The build context must be the
# unpacked anyfs-web-dist tarball (produced by wasm.yml's web-package job),
# NOT a repo checkout — the wasm bundle + vite build happen upstream in CI.
FROM caddy:2-alpine
COPY Caddyfile /etc/caddy/Caddyfile
COPY . /srv
EXPOSE 4173
```

Why this works:
- `caddy:2-alpine` default `CMD` is `caddy run --config /etc/caddy/Caddyfile`
  with `WORKDIR /srv`. The first `COPY` puts the config where Caddy looks; the
  second puts the static files where `root * .` resolves (CWD → `/srv`).
- The existing `public/Caddyfile` binds `:4173` (unprivileged, >1024), so no
  root is required; `EXPOSE 4173` is documentation only.
- No `.dockerignore` — the dist context is already clean (no `node_modules`, no
  `public/disks/` in CI checkouts per design doc L124).
- CI-only artifact: local dev uses `vite dev`/`vite preview`; the Dockerfile
  lives in the repo for reviewability and manual `docker build` of an unpacked
  dist.

## CI job

Trigger expansion (`on:` block):

```yaml
on:
  push:
    branches: [main]
    tags: ['v*']
  workflow_dispatch:
```

New `ghcr-image` job (inserted after `web-package`, before `electron-package`):

```yaml
  ghcr-image:
    needs: web-package
    runs-on: ubuntu-24.04
    timeout-minutes: 15
    permissions:
      contents: read
      packages: write
    steps:
      - uses: actions/checkout@v4
      - uses: actions/download-artifact@v4
        with:
          name: anyfs-web-dist
          path: dist
      - name: Unpack dist into build context
        run: tar -xzf dist/anyfs-web-dist.tar.gz -C dist && rm dist/anyfs-web-dist.tar.gz
      - uses: docker/setup-qemu-action@v3
      - uses: docker/setup-buildx-action@v3
      - uses: docker/login-action@v3
        with:
          registry: ghcr.io
          username: ${{ github.actor }}
          password: ${{ secrets.GITHUB_TOKEN }}
      - id: meta
        uses: docker/metadata-action@v5
        with:
          images: ghcr.io/${{ github.repository_owner }}/anyfs-web
          tags: |
            type=sha,prefix=sha-,format=short
            type=semver,pattern={{version}}
            type=semver,pattern={{major}}.{{minor}}
            type=raw,value=latest,enable=${{ github.ref == 'refs/heads/main' }}
          # exposed to the smoke step as METADATA_TAGS (newline-joined tag list)
      - name: Smoke test (amd64 only — image already pushed)
        env:
          METADATA_TAGS: ${{ steps.meta.outputs.tags }}
          IMAGE: ghcr.io/${{ github.repository_owner }}/anyfs-web
      - uses: docker/build-push-action@v6
        with:
          context: dist
          file: ts/examples/vite-demo/Dockerfile
          platforms: linux/amd64,linux/arm64
          push: true
          tags: ${{ steps.meta.outputs.tags }}
          labels: ${{ steps.meta.outputs.labels }}
      - name: Smoke test (amd64 only — image already pushed)
        env:
          IMAGE: ghcr.io/${{ github.repository_owner }}/anyfs-web
        run: |
          # metadata-action emits the short-sha tag as sha-<short>; reuse its
          # output rather than re-deriving it from GITHUB_SHA.
          SHA_TAG=$(printf '%s\n' "$METADATA_TAGS" | grep -E '^sha-[0-9a-f]{7,}$' | head -1)
          [ -n "$SHA_TAG" ] || { echo "no sha- tag in metadata output" >&2; exit 1; }
          docker pull "$IMAGE:$SHA_TAG"
          docker run -d --name anyfs-web-smoke -p 4173:4173 "$IMAGE:$SHA_TAG"
          sleep 2
          curl -sf http://localhost:4173/ -o /dev/null
          curl -sI http://localhost:4173/ | grep -i 'cross-origin-opener-policy'
          curl -sI http://localhost:4173/ | grep -i 'cross-origin-embedder-policy'
          curl -sf http://localhost:4173/wasm/anyfs.worker.js -o /dev/null
          docker stop anyfs-web-smoke
```

Design points:

1. **Job-scoped `permissions`** — only `ghcr-image` gets `packages: write`;
   the other jobs keep the default read-only token. No workflow-level
   `permissions:` block is added.
2. **Image name via `github.repository_owner`** — resolves to `xdqi` here, but
   survives a repo transfer. Package is `anyfs-web` (distinct from any future
   native/binary container package on the same repo).
3. **`sha-` always, `latest` only on main** — `type=sha,prefix=sha-` tags every
   build with a traceable commit; the `enable=` gate keeps `latest` immobile on
   tag pushes.
4. **Multi-arch without penalty** — the Dockerfile is `COPY`-only, so buildx
   materializes each platform's layers by pulling that platform's
   `caddy:2-alpine` manifest + adding file layers — no QEMU emulation of `RUN`
   steps. `setup-qemu` is included as the documented buildx prerequisite.
5. **Build context = `dist`**, not the repo checkout. `build-push-action`'s
   `context: dist` points at the unpacked tarball; `file:` resolves relative to
   the checkout, so `ts/examples/vite-demo/Dockerfile` is found. `COPY . /srv`
   copies the unpacked dist into the image.

## Tag mapping

| Trigger | Image tags pushed | `latest` moved? |
|---|---|---|
| push to `main` | `sha-<short>`, `latest` | ✅ yes |
| push tag `v1.2.3` | `sha-<short>`, `1.2.3`, `1.2` | ❌ no |
| push tag `v1.0.0` | `sha-<short>`, `1.0.0`, `1.0` | ❌ no |
| `workflow_dispatch` on main | `sha-<short>`, `latest` | ✅ yes |
| `workflow_dispatch` on a tag/other | `sha-<short>` | ❌ no |

`sha-<short>` is always present, so any build can be pulled and rolled back to
regardless of trigger.

## Smoke test

After push, the job pulls the `sha-<short>` tag (the artifact just built, not
a stale `latest` from a prior run) and runs it on the amd64 runner (native, no
QEMU). It asserts:

1. HTTP 200 on `/` (Caddy is serving).
2. `Cross-Origin-Opener-Policy` header present.
3. `Cross-Origin-Embedder-Policy` header present.
4. HTTP 200 on `/wasm/anyfs.worker.js` (wasm path reachable through Caddy).

This catches "built fine but Caddyfile/runtime is broken" (e.g. an accidental
COEP removal, a COPY-layer path mistake) that would otherwise only surface at
deploy time. The smoke runs only on amd64; the arm64 image is the same Dockerfile
so platform parity is structural.

## Out of scope

- Non-main branch image builds (use `workflow_dispatch` to manually run).
- Auto-deploy / continuous deployment to a live host — this design publishes the
  image; pulling and running it is operator action.
- Signing (cosign) / SBOM / attestation — follow-up if supply-chain hardening
  is needed.
- A `.dockerignore` — the dist context is already minimal.

## Acceptance criteria

1. A `workflow_dispatch` run on `main`: `web-package` + `ghcr-image` green;
   `ghcr.io/xdqi/anyfs-web:latest` and `:sha-<short>` exist on ghcr.
2. A `v*` tag push: `ghcr-image` green; `:v<ver>`, `:v<major>.<minor>`, and
   `:sha-<short>` exist; `:latest` is NOT moved.
3. `docker run -p 4173:4173 ghcr.io/xdqi/anyfs-web:latest` serves the app and
   a browser can open a disk image (end-to-end manual check).
4. The smoke test step passes (HTTP 200 + both COOP/COEP headers + wasm path
   reachable) on every successful `ghcr-image` run.
5. Image manifest lists both `linux/amd64` and `linux/arm64`.

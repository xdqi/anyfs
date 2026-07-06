# ghcr Web Image Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a `ghcr-image` job to `wasm.yml` that bakes the existing `anyfs-web-dist` artifact into a multi-arch `caddy:2-alpine` image and pushes it to `ghcr.io/xdqi/anyfs-web`, so deployment is `docker run` + serve static files.

**Architecture:** One new Dockerfile (repo-tracked, `COPY`-only, multi-arch-safe) + one new job in `wasm.yml` that downloads the existing `anyfs-web-dist` tarball, unpacks it as the build context, and uses `docker/build-push-action` to push `linux/amd64` + `linux/arm64` manifests. Tags: `sha-<short>` always, `latest` only on `main`, semver on `v*` tags. A pull+curl smoke test asserts COOP/COEP headers + wasm path reachability.

**Tech Stack:** GitHub Actions, `docker/build-push-action@v6`, `docker/metadata-action@v5`, `docker/login-action@v3`, `docker/setup-buildx-action@v3`, `docker/setup-qemu-action@v3`, Caddy 2 (alpine), Bash.

**Spec:** [docs/superpowers/specs/2026-07-07-ghcr-web-image-design.md](../specs/2026-07-07-ghcr-web-image-design.md)

---

## File Structure

- **Create** `ts/examples/vite-demo/Dockerfile` — single-stage `caddy:2-alpine`; build context = unpacked `anyfs-web-dist` tarball. `COPY Caddyfile` to `/etc/caddy/`, `COPY .` to `/srv`. `EXPOSE 4173`.
- **Modify** `.github/workflows/wasm.yml` — three edits: (1) `on:` block adds `tags: ['v*']`; (2) insert `ghcr-image` job after `web-package` (line 258) before `electron-package` (line 259); the job has job-scoped `permissions: packages: write`.
- No other files change. No `.dockerignore` (dist context is already minimal). No workflow-level `permissions:` block (stays per-job).

---

## Task 1: Add the Dockerfile

**Files:**
- Create: `ts/examples/vite-demo/Dockerfile`

- [ ] **Step 1: Create the Dockerfile**

Create `ts/examples/vite-demo/Dockerfile` with this exact content:

```dockerfile
# Serves the pre-built vite-demo static dist. The build context must be the
# unpacked anyfs-web-dist tarball (produced by wasm.yml's web-package job),
# NOT a repo checkout — the wasm bundle + vite build happen upstream in CI.
#
# Local smoke (without CI): from ts/examples/vite-demo, after `pnpm build`:
#   docker build -t anyfs-web-local -f Dockerfile dist/
#   docker run --rm -p 4173:4173 anyfs-web-local
FROM caddy:2-alpine
COPY Caddyfile /etc/caddy/Caddyfile
COPY . /srv
EXPOSE 4173
```

- [ ] **Step 2: Verify the Dockerfile builds locally against the existing dist**

The local `ts/examples/vite-demo/dist/` already exists (built 2026-06-25, includes `Caddyfile`). Build against it:

```bash
docker build -t anyfs-web-local -f ts/examples/vite-demo/Dockerfile ts/examples/vite-demo/dist/
```

Expected: build succeeds, ends with `Successfully tagged anyfs-web-local:latest` (or `exporting to image ... done`). If `Caddyfile` is reported missing, the local dist is stale — run `pnpm --filter vite-demo build` from `ts/` and retry.

- [ ] **Step 3: Smoke-test the local image (HTTP 200 + COOP/COEP + wasm path)**

```bash
docker run -d --name anyfs-web-smoke -p 4173:4173 anyfs-web-local
sleep 2
curl -sf http://localhost:4173/ -o /dev/null && echo "root OK"
curl -sI http://localhost:4173/ | grep -i 'cross-origin-opener-policy' && echo "COOP OK"
curl -sI http://localhost:4173/ | grep -i 'cross-origin-embedder-policy' && echo "COEP OK"
curl -sf http://localhost:4173/wasm/anyfs.worker.js -o /dev/null && echo "worker OK"
docker stop anyfs-web-smoke
```

Expected: all four `OK` lines print. If `worker OK` fails, the local dist may not have `public/wasm/` synced — run `./scripts/sync_wasm_bundle.sh` then `pnpm --filter vite-demo build` and rebuild.

- [ ] **Step 4: Commit**

```bash
git add ts/examples/vite-demo/Dockerfile
git commit -m "feat(docker): add vite-demo static-serving Dockerfile (caddy:2-alpine)

COPY-only single stage: Caddyfile -> /etc/caddy, dist -> /srv. Build context
is the unpacked anyfs-web-dist tarball from wasm.yml's web-package job.
Local smoke: docker build -f Dockerfile dist/

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 2: Expand the `wasm.yml` trigger to include `v*` tags

**Files:**
- Modify: `.github/workflows/wasm.yml:22-24` (the `on:` → `push:` block)

- [ ] **Step 1: Read the exact current `on:` block**

Run:
```bash
sed -n '22,26p' .github/workflows/wasm.yml
```
Expected output (current state):
```
on:
  push:
    branches: [main]
  workflow_dispatch:
```

- [ ] **Step 2: Add the `tags:` line under `push:`**

Edit `.github/workflows/wasm.yml`. Replace:
```yaml
on:
  push:
    branches: [main]
  workflow_dispatch:
```
with:
```yaml
on:
  push:
    branches: [main]
    tags: ['v*']
  workflow_dispatch:
```

- [ ] **Step 3: Verify the YAML parses and the diff is exactly one line**

```bash
python3 -c "import yaml,sys; yaml.safe_load(open('.github/workflows/wasm.yml')); print('YAML OK')"
git diff .github/workflows/wasm.yml
```
Expected: `YAML OK` and a diff showing only `+    tags: ['v*']` inserted between `branches: [main]` and `workflow_dispatch:`.

- [ ] **Step 4: Lint with the repo's shellcheck/yaml gate if present, else skip**

```bash
ls scripts/lint-*.sh 2>/dev/null
# wasm.yml is YAML not shell, so shellcheck does not apply; just re-confirm parse:
python3 -c "import yaml; yaml.safe_load(open('.github/workflows/wasm.yml'))" && echo "parse OK"
```
Expected: lists `lint-no-hardcoded-paths.sh` and `lint-shellcheck.sh` (neither covers YAML), then `parse OK`.

- [ ] **Step 5: Commit**

```bash
git add .github/workflows/wasm.yml
git commit -m "ci(wasm): trigger on v* tags so ghcr-image can publish semver tags

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 3: Add the `ghcr-image` job

**Files:**
- Modify: `.github/workflows/wasm.yml` — insert new job after the `web-package` job's closing step (after line 257 `if-no-files-found: error`, before the blank line + `electron-package:` at line 259).

- [ ] **Step 1: Read the exact insertion point**

Run:
```bash
sed -n '250,260p' .github/workflows/wasm.yml
```
Expected (current state):
```
      - name: Pack web dist
        run: tar -czf anyfs-web-dist.tar.gz -C ts/examples/vite-demo/dist .
      - name: Upload web dist
        uses: actions/upload-artifact@v4
        with:
          name: anyfs-web-dist
          path: anyfs-web-dist.tar.gz
          if-no-files-found: error

  electron-package:
```

- [ ] **Step 2: Insert the `ghcr-image` job before `electron-package:`**

Edit `.github/workflows/wasm.yml`. Replace this exact text:
```yaml
          if-no-files-found: error

  electron-package:
```
with:
```yaml
          if-no-files-found: error

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
          METADATA_TAGS: ${{ steps.meta.outputs.tags }}
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

  electron-package:
```

- [ ] **Step 3: Verify the YAML parses and the job is well-formed**

```bash
python3 -c "import yaml; d=yaml.safe_load(open('.github/workflows/wasm.yml')); j=d['jobs']; print('jobs:', list(j.keys())); print('ghcr-image needs:', j['ghcr-image']['needs']); print('ghcr-image perms:', j['ghcr-image']['permissions']); print('platforms:', [s.get('platforms') for s in j['ghcr-image']['steps'] if s.get('uses','').startswith('docker/build-push')])"
```
Expected output:
```
jobs: ['wasm-build', 'web-package', 'ghcr-image', 'electron-package']
ghcr-image needs: web-package
ghcr-image perms: {'contents': 'read', 'packages': 'write'}
platforms: ['linux/amd64,linux/arm64']
```

- [ ] **Step 4: Verify the job graph ordering**

```bash
python3 -c "
import yaml
d=yaml.safe_load(open('.github/workflows/wasm.yml'))
j=d['jobs']
print('electron-package needs:', j['electron-package']['needs'])
# ghcr-image should be a sibling of electron-package (both need web-package), NOT chained.
assert j['ghcr-image']['needs']==['web-package'], 'ghcr-image must need only web-package'
assert 'ghcr-image' not in j['electron-package']['needs'], 'electron must not depend on ghcr-image'
print('job graph OK: ghcr-image || electron-package, both need web-package')
"
```
Expected: `job graph OK: ghcr-image || electron-package, both need web-package`

- [ ] **Step 5: Commit**

```bash
git add .github/workflows/wasm.yml
git commit -m "ci(wasm): publish multi-arch web image to ghcr.io/xdqi/anyfs-web

New ghcr-image job (needs web-package, parallel to electron-package):
- unpacks anyfs-web-dist as build context for ts/examples/vite-demo/Dockerfile
- builds linux/amd64 + linux/arm64 (COPY-only, no QEMU-emulated RUN)
- tags: sha-<short> always, latest on main only, semver on v* tags
- smoke: pull sha- tag, curl COOP/COEP headers + /wasm/anyfs.worker.js

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 4: Validate the full workflow file end-to-end (local)

**Files:** none modified — verification only.

- [ ] **Step 1: Confirm trigger coverage**

```bash
python3 -c "
import yaml
d=yaml.safe_load(open('.github/workflows/wasm.yml'))
on=d['on']
assert on['push']['branches']==['main'], on['push']['branches']
assert on['push']['tags']==['v*'], on['push']['tags']
assert 'workflow_dispatch' in on, 'workflow_dispatch missing'
print('triggers OK:', on)
"
```
Expected: `triggers OK: {'push': {'branches': ['main'], 'tags': ['v*']}, 'workflow_dispatch': None}` (or similar dict form).

- [ ] **Step 2: Confirm the Dockerfile matches what the job references**

```bash
test -f ts/examples/vite-demo/Dockerfile && echo "Dockerfile present"
grep -q '^FROM caddy:2-alpine' ts/examples/vite-demo/Dockerfile && echo "base image OK"
grep -q 'COPY Caddyfile /etc/caddy/Caddyfile' ts/examples/vite-demo/Dockerfile && echo "Caddyfile COPY OK"
grep -q 'COPY . /srv' ts/examples/vite-demo/Dockerfile && echo "dist COPY OK"
grep -q 'EXPOSE 4173' ts/examples/vite-demo/Dockerfile && echo "EXPOSE OK"
```
Expected: all five lines print.

- [ ] **Step 3: Re-run the local Docker smoke to confirm nothing regressed**

```bash
docker build -t anyfs-web-local -f ts/examples/vite-demo/Dockerfile ts/examples/vite-demo/dist/
docker run -d --name anyfs-web-smoke2 -p 4174:4173 anyfs-web-local
sleep 2
curl -sf http://localhost:4174/ -o /dev/null && echo "root OK"
curl -sI http://localhost:4174/ | grep -i 'cross-origin-opener-policy' >/dev/null && echo "COOP OK"
curl -sI http://localhost:4174/ | grep -i 'cross-origin-embedder-policy' >/dev/null && echo "COEP OK"
curl -sf http://localhost:4174/wasm/anyfs.worker.js -o /dev/null && echo "worker OK"
docker stop anyfs-web-smoke2
```
Expected: all four `OK` lines. (Port 4174 avoids clashing with any leftover container from Task 1.)

- [ ] **Step 4: Push and trigger CI**

Push the branch (the commits are on `main` locally; push to origin):
```bash
git push origin main
```
Then watch the run:
```bash
gh run watch -w wasm.yml --exit-status
```
Expected: the `wasm` workflow runs; `web-package` and `ghcr-image` go green; `ghcr-image`'s smoke step passes. (The full `wasm-build` cold start can take ~15 min warm.)

- [ ] **Step 5: Verify the image landed on ghcr with the right tags**

```bash
SHORT=$(git rev-parse --short=7 HEAD)
docker pull ghcr.io/xdqi/anyfs-web:sha-$SHORT
docker manifest inspect ghcr.io/xdqi/anyfs-web:sha-$SHORT | python3 -c "import sys,json; m=json.load(sys.stdin); print('platforms:', sorted('%s/%s'%(x['platform']['architecture'],x['platform'].get('os','linux')) for x in m.get('manifests',[])))"
docker pull ghcr.io/xdqi/anyfs-web:latest
docker run --rm -p 4175:4173 ghcr.io/xdqi/anyfs-web:latest &
sleep 2
curl -sf http://localhost:4175/ -o /dev/null && echo "deployed latest OK"
kill %1 2>/dev/null
```
Expected: `sha-<short>` and `latest` both pull; platforms list shows `amd64/linux` and `arm64/linux`; `deployed latest OK` prints. (If `latest` is missing, the push that triggered was not on `main` — confirm `git rev-parse --abbrev-ref HEAD` shows `main`.)

---

## Self-Review (completed during authoring)

**Spec coverage:**
- Dockerfile (spec "Dockerfile" section) → Task 1.
- Trigger expansion to `v*` tags (spec "CI job" → trigger) → Task 2.
- `ghcr-image` job with job-scoped `permissions: packages: write`, image name `ghcr.io/${{ github.repository_owner }}/anyfs-web`, multi-arch `linux/amd64,linux/arm64`, tag rules (`sha-`, semver, `latest`-on-main-only), smoke test → Task 3.
- Tag mapping table (spec "Tag mapping") → enforced by Task 3's `metadata-action` config; verified by Task 4 Step 5 (sha always; latest only on main).
- Smoke test (spec "Smoke test") → local in Task 1 Step 3 + Task 4 Step 3; in-CI in Task 3 Step 2's smoke step.
- Acceptance criteria 1–5 (spec) → Task 4 Steps 3–5 cover multi-arch manifest, latest immobility (via running on main), COOP/COEP, wasm path, and `docker run` serving.

**Placeholder scan:** No TBD/TODO; every code step has full content; every command has expected output.

**Type/name consistency:** Image name `ghcr.io/${{ github.repository_owner }}/anyfs-web` used identically in Task 3's `metadata-action` `images:`, `login-action`, and smoke `IMAGE:` env. Dockerfile path `ts/examples/vite-demo/Dockerfile` matches between Task 1 (create) and Task 3 (`file:`). `sha-<short>` tag form matches between `metadata-action` `type=sha,prefix=sha-` and the smoke `grep -E '^sha-[0-9a-f]{7,}$'`. Smoke container name differs across tasks (`anyfs-web-smoke` vs `anyfs-web-smoke2`) intentionally to avoid name collisions on re-run.

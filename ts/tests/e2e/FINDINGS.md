# E2E Suite — Findings (real app behavior surfaced by the tests)

These are **real application defects** the E2E suite surfaced while being built. Per the
project decision, tests that hit these are marked `test.fixme(...)` with a `// FINDING:` comment
rather than having their assertions weakened — the suite stays green while the bugs stay
documented. Each entry: what was observed, how to reproduce, suspected area.

Status legend: OPEN (unconfirmed root cause) · CONFIRMED · FIXED

---

## F1 — ext4 partition mount hangs in the wasm backend (OPEN, High)

**Observed (Phase 3.2, web/wasm):** Opening `multiRaw` (multi.img), `session.enter(1)` on the
ext4 partition dispatches `op=enter`, EXT4 probes run (`couldn't mount as ext3/ext2` — normal
probe noise), then the worker **never logs `op=enter ... took Xms`** and `enter` never resolves
or rejects. DiskView stays on "mounting partition #1…" indefinitely. Reproduced across multiple
runs.

**Repro:** web app `/?e2e=1` → load multi.img → select partition #1 (ext4) → hangs.

**Suspected area:** wasm LKL `session.enter` / ext4 mount path in the Web Worker (core
`worker.ts` `enter` op, or the LKL ext4 driver under wasm). Contrast: vfat (#2) mounts fine
(`op=enter took 13ms`).

---

## F2 — whole-disk #0 (no filesystem) hangs instead of erroring (OPEN, High)

**Observed:** Entering partition #0 (the whole disk, which has no filesystem on multi.img) —
all FS probes correctly fail to find a filesystem, but instead of surfacing a mount error, the
app **hangs** on "mounting partition #0…"; `enter` never settles.

**Repro:** web app → load multi.img → select whole-disk #0 → hangs (expected: a clean "no
filesystem / can't mount" error).

**Suspected area:** same `enter` path as F1; additionally a missing error path when no FS is
detected (should reject, not hang).

---

## F3 — btrfs whole-disk mount hangs in the wasm backend (OPEN, High)

**Observed:** Opening `btrfsVmdk` (whole-disk btrfs, no PT). dmesg shows `Btrfs loaded`, then
the app is stuck on "mounting…", no rows render, no error. `enter` never settles. Same shape
as F1/F2.

**Repro:** web app → load btrfs-whole.vmdk → enter the whole-disk btrfs → hangs.

**Suspected area:** same wasm `enter`/mount path. Net effect with F1: in this wasm build, only
**vfat** mounts; the directory-bearing filesystems (ext4, btrfs) wedge.

---

## F4 — Service-Worker download fails under `vite preview` (OPEN, Medium; partly env)

**Observed:** `download()` on the web path triggers SW registration of the hashed
`/assets/sw-download-*.js`, which throws
`SecurityError: ... scope ('/') is not under the max scope allowed ('/assets/')`. The browser
download event never fires; app logs `download failed: SecurityError`.

**Cause:** the SW needs the `Service-Worker-Allowed: /` response header to claim a scope above
its own path. `stream-download.ts` notes only the Caddy config ships that header; the
`vite preview` server (and the Electron `anyfs://` protocol) do not. So under Playwright's
preview-served prod build, the streaming SW download cannot register.

**Repro:** web app (vite preview) → mount any FS → open a file to download → SecurityError.

**Suspected area:** partly environment (preview server header), partly app (download path
assumes a `/`-scoped SW that only Caddy enables). For E2E: either (a) serve the preview with
`Service-Worker-Allowed: /` (a preview-middleware/header tweak), or (b) treat web download as a
known-fixme and assert the Electron IPC download instead. Decision pending.

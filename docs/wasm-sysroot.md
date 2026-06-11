# wasm sysroot provisioning

The wasm build links against a sysroot of 17 prebuilt static libraries
(glib/gio, pcre2, libffi, zlib, bzip2, zstd, blkid/uuid, a libresolv stub —
see `scripts/lib/wasm_sysroot.manifest`). It is provisioned the same way as
the mingw toolchain, with three roles:

| Role | mingw analogue | wasm piece |
|---|---|---|
| Recipe (how to build it from pinned sources) | PKGBUILD | `scripts/build_wasm_sysroot.sh` |
| Prebuilt artifact (what consumers download) | `bootstrap.tar.xz` | `wasm-sysroot-linux.tar.xz` GitHub release asset, built by `.github/workflows/wasm-sysroot.yml` |
| Installer / integrity check | pacman | `scripts/fetch_wasm_sysroot.sh` + manifest-completeness check |

`scripts/fetch_wasm_sysroot.sh` downloads the release tarball into
`<repo>/.toolchain/wasm-sysroot/` (gitignored). It short-circuits if every
manifest lib is already present, and re-validates the manifest after
extraction. `scripts/lib/config.sh` auto-prefers that location when
`paths.wasm_sysroot` is unset/empty; an explicit `build.user.toml` value
always wins.

## Per-target dependency provisioning

| Target | Library dependencies come from |
|---|---|
| linux-amd64 | apt packages (distro `-dev` packages) |
| mingw32 / mingw64 | msys.kosaka.moe pacman repo (msys2-cross packages) |
| wasm | this tarball (`fetch_wasm_sysroot.sh`) |

## Version-bump procedure

1. Edit the version pins (`*_V` / `*_URL` / `*_SHA`) in
   `scripts/build_wasm_sysroot.sh`, and update
   `scripts/lib/wasm_sysroot.manifest` if the lib set or pins change.
2. Dispatch `.github/workflows/wasm-sysroot.yml` with a new tag
   (`wasm-sysroot-rN`). The workflow rebuilds from source — the recipe's
   built-in manifest parity check is the acceptance gate — and publishes the
   release.
3. Bump the `WASM_SYSROOT_TAG` default in `scripts/fetch_wasm_sysroot.sh`.

## The two excavated glib hacks

Both were reverse-engineered from the known-good hand-built sysroot and are
applied by the recipe (see the comments in `build_glib()` in
`scripts/build_wasm_sysroot.sh`):

1. **`scripts/lib/glib-2.88.0-emscripten-fd-query-path.patch`** — Emscripten
   port patch applied to the glib tree before `meson setup` (rationale in the
   patch header).
2. **config.h post-edit** — Emscripten's libc *declares* `posix_spawn{,p}`
   and `pthread_getname_np` (so meson's compile-only checks pass) but does
   not *define* them, which breaks the tool executables at wasm-ld. The
   recipe deletes `HAVE_POSIX_SPAWN` and `HAVE_PTHREAD_GETNAME_NP` from the
   generated `_build/config.h` after setup, matching the known-good sysroot's
   preserved config.h.

## Local rebuild

```sh
./scripts/build_wasm_sysroot.sh                     # into paths.wasm_sysroot
SYSROOT=/tmp/sysroot ./scripts/build_wasm_sysroot.sh
```

Takes ~12 minutes. Requires emsdk (the recipe was validated with emcc 5.0.7
— the same version `.github/workflows/wasm-sysroot.yml` pins), meson, ninja,
pkg-config, and a util-linux v2.40.4 checkout with a generated `./configure`
(`./autogen.sh`); the tree is located via `paths.util_linux` or the `UL_SRC`
env override. Full runs end with the manifest parity check and fail on any
drift.

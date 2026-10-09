# macOS runtime tests

These run on a real Mac, from a bundle that is cross-built on Linux by
`scripts/macho/package_macos_tests.sh` (see `docs/macos.md`). The reference data in the
bundle (partition tables, `anyfs-lspart` output, file sizes and SHA-256) was computed by the
Linux build from the same fixture bytes; the Mac must reproduce it exactly.

What is tested:

| step | what | needs |
|---|---|---|
| 0 | `codesign -v` (arm64), dyld finds `liblkl-kernel.dylib` | — |
| 1 | core test programs from the meson unit suite (session reopen, whole disk vs partition, QEMU thread + watchdog, name escaping, legacy encodings via iconv, …) | — |
| 2 | `anyfs-lspart` on a GPT raw image, the same wrapped as zlib and bzip2 DMGs, and the Ubuntu 26.10 qcow2: table, partition kinds, fstype, label, UUID; run twice | — |
| 3 | `anyfs-ksmbd`: start, `mount_smbfs` as guest, SHA-256 of files over SMB, `umount`, SIGINT, clean exit | — |
| 4 | `anyfs-nfsd`: start, NFSv4 mount, SHA-256 over NFS, `umount`, SIGINT, clean exit | `sudo` (mounting NFS needs root) |
| 5 | the Node addon: kernel boot, partition metadata, mount, readdir, stat, extract to a host file + SHA-256, close, 3 passes in one kernel, halt | `--app` (Electron as node) or `node` |
| 6 | the Electron main process loads the staged addon (`ANYFS_NATIVE_SMOKE`) | `--app` |

## Run

Copy `anyfs-macos-test-<arch>.tar.gz` to the Mac (arm64 for Apple Silicon, x86_64 for Intel),
then in Terminal:

    cd ~ && rm -rf anyfs-macos-test-<arch> && tar xzf anyfs-macos-test-<arch>.tar.gz
    cd anyfs-macos-test-<arch>
    shasum -a 256 -c SHA256SUMS | grep -v ': OK$'       # prints nothing when intact
    xattr -dr com.apple.quarantine .                    # if fetched with a browser
    # the app, if the bundle has one (app/): electron-packager on Linux leaves its signature broken
    codesign --force --deep --sign - app/anyfs-demo.app
    tests/run-tests.sh --app app/anyfs-demo.app 2>&1 | tee ~/anyfs-macos-test.txt

Without an app in the bundle: `tests/run-tests.sh` (uses `node` from PATH for step 5 if there
is one). `--skip-nfs` skips the sudo step. The run ends with `PASS (...)` or `FAIL (...)`; all
logs are in `~/anyfs-macos-test-logs/<date>/`. Send that directory and the output back.

Always start from a fresh extract: XNU caches code signatures per inode, so overwriting a
signed binary in place can get it killed with "Killed: 9".

## Then, by hand: the GUI

    open app/anyfs-demo.app

Settings must not have "disable native" set. Drop `fixtures/ubuntu-26.10.qcow2` on the window
(or File > Open): the partition picker lists `#1 cloudimg-rootfs ext4 Linux root (x86-64)`,
`#13 BOOT ext4`, `#14 BIOS boot`, `#15 UEFI vfat EFI System`. Open #1, go to `/etc`, download
`os-release`; it must contain `VERSION_ID="26.10"`. Repeat with `fixtures/parts-zlib.dmg`
(#3 `fixroot`, file `hello.txt`). Quit with Cmd-Q while a disk is open; the app must exit
within a few seconds.

## If something fails

- `sw_vers; uname -m`, and the log directory.
- dyld errors: rerun the failing command with `DYLD_PRINT_LIBRARIES=1 DYLD_PRINT_SEARCHING=1`.
- "Killed: 9": `log show --last 5m --predicate 'eventMessage CONTAINS "anyfs"'` and
  `~/Library/Logs/DiagnosticReports/`.
- A hang: `sample <pid> 10 -file ~/anyfs-hang.txt` from another terminal.
- `mount_smbfs` refusing the port: try Finder > Go > Connect to Server >
  `smb://guest@127.0.0.1:14455/anyfs` while `anyfs-ksmbd` runs, and note the result.

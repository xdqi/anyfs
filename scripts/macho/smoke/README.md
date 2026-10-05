# macOS smoke test

Checks that the converted kernel (`liblkl-kernel.dylib`) and the Darwin host library
work on a real Mac: boot, mount an ext4 image read-write, write a file, remount (drops
the page cache) and read it back from the block device, list the directory, time a
100 ms in-kernel sleep, unmount, halt.

Build on Linux (after `build_kernel_dylib.sh` and `build_host_lib.sh` for the same arch):

    scripts/macho/build_smoke.sh --arch=arm64     # Apple Silicon
    scripts/macho/build_smoke.sh --arch=x86_64    # Intel

Run on the Mac, with a fresh copy every time:

    rm -rf ~/lkl-smoke && scp -r <linux-host>:anyfs-reader/build/macos/arm64/smoke ~/lkl-smoke   # x86_64 on Intel
    cd ~/lkl-smoke
    shasum -a 256 *        # compare with the same command on the Linux host
    codesign -v liblkl-kernel.dylib && echo signature ok
    codesign -v lkl-macos-smoke                  # arm64 only
    ./lkl-macos-smoke smoke-ext4.img; echo "exit=$?"

Why a fresh copy: `scp -r` into an existing directory nests the bundle inside it, and
XNU caches code signatures per inode, so overwriting a signed file in place can get the
process killed with "Killed: 9". The image is also modified by each run. On Intel,
`codesign -v lkl-macos-smoke` printing "code object is not signed at all" is expected.
Files fetched through a browser carry a quarantine flag; clear it with
`xattr -dr com.apple.quarantine ~/lkl-smoke`.

Expected: kernel log lines (`loglevel=8`, set in `lkl_macos_smoke.c`), one `ok` line per
check, then `PASS (21 checks)` and `exit=0`.

Hang: if there is no output for 30 s, run `sample lkl-macos-smoke 10 -file ~/lkl-hang.txt`
in another terminal, then Ctrl-C. The program also aborts by itself after 120 s with
`FAIL: timed out after 120 s (hang)`.

If it fails, the kernel log appears above the first `FAIL` line. Collect:

- `sw_vers; uname -m`;
- the full output: `./lkl-macos-smoke smoke-ext4.img 2>&1 | tee ~/lkl-smoke.log`;
- if dyld fails to load, a rerun with `DYLD_PRINT_LIBRARIES=1 DYLD_PRINT_SEARCHING=1`;
- for "Killed: 9": `log show --last 5m --predicate 'eventMessage CONTAINS "lkl-macos-smoke"'`
  and the crash report under `~/Library/Logs/DiagnosticReports/`;
- for a crash, the same crash report.

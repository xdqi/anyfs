# macOS smoke test

Checks that the converted kernel (`liblkl-kernel.dylib`) and the Darwin host library
work on a real Mac: boot, mount an ext4 image read-write, write and read back a file,
list the directory, time a 100 ms in-kernel sleep, unmount, halt.

Build on Linux (after `build_kernel_dylib.sh` and `build_host_lib.sh` for the same arch):

    scripts/macho/build_smoke.sh --arch=arm64     # Apple Silicon
    scripts/macho/build_smoke.sh --arch=x86_64    # Intel

Run on the Mac:

    scp -r <linux-host>:anyfs-reader/build/macos/arm64/smoke ~/lkl-smoke   # x86_64 on Intel
    cd ~/lkl-smoke
    codesign -v liblkl-kernel.dylib && echo signature ok
    ./lkl-macos-smoke smoke-ext4.img; echo "exit=$?"

Expected: one `ok` line per check, then `PASS` and `exit=0`. The image is modified, so
copy a fresh bundle for each run. Files fetched through a browser carry a quarantine
flag; clear it with `xattr -dr com.apple.quarantine ~/lkl-smoke`.

If it fails, collect:

- the full output (the kernel log appears above the `FAIL` line; `loglevel=4` in
  `lkl_macos_smoke.c` limits it to warnings and errors);
- `otool -L lkl-macos-smoke` and `otool -L liblkl-kernel.dylib`;
- `dyld_info -fixups liblkl-kernel.dylib | head -50`;
- for a crash, the report under `~/Library/Logs/DiagnosticReports/`.

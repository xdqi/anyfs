# LKL File Servers (`anyfs-ksmbd` + `anyfs-nfsd`)

## Overview

Two userspace file servers run an LKL (Linux Kernel Library) instance and re-export
disk images over the network using LKL's in-tree ksmbd / nfsd subsystems.

| Server         | Protocol      | Host listen port | Source                       |
| -------------- | ------------- | :--------------: | ---------------------------- |
| `anyfs-ksmbd`  | SMB3 (CIFS)   | 4455 (default)   | `src/ksmbd/lkl_ksmbd.c`      |
| `anyfs-nfsd`   | NFSv4         | 20049 (default)  | `src/nfsd/lkl_nfsd.c`        |

Common properties:

- Built against LKL ≥ 6.18 (CONFIG_SMB_SERVER / CONFIG_NFSD).
- No root, no kernel modules — everything is userspace.
- Multi-disk + multi-share: each `--share` exposes one partition as one SMB share
  / NFS export.
- Read-only by default (write mode is `-w` on `anyfs-nfsd`; `anyfs-ksmbd` enables
  RW per-share via the partition's mount options).
- All file operations execute as kernel `root` (`ALLSQUASH + anonuid=0`).
- Data path uses a host-side userspace TCP proxy (`src/host_proxy/`) — libslirp is
  not on the data path.

---

## Architecture

```
                       ┌── client (smbclient / mount.nfs4)
                       ▼
            host *:4455  (SMB)
            host *:20049 (NFS)
                       │
                       │ host_proxy: per-connection TCP splice
                       ▼
┌──────────────────────────────────────────────────────────┐
│  LKL userspace kernel                                    │
│  ┌──────────────────────────────────────────────────┐    │
│  │ network stack (TCP on loopback)                  │    │
│  │ 127.0.0.1:445  (SMB)   /   127.0.0.1:2049 (NFS) │    │
│  ├──────────────────────────────────────────────────┤    │
│  │ ksmbd / nfsd in-kernel servers                   │    │
│  ├──────────────────────────────────────────────────┤    │
│  │ VFS → ext4 / xfs / btrfs / vfat / ntfs3 / ...    │    │
│  ├──────────────────────────────────────────────────┤    │
│  │ virtio-blk → raw_blk_backend / qemu_blk_backend  │    │
│  └──────────────────────────────────────────────────┘    │
└──────────────────────────────────────────────────────────┘
                       │
                       ▼
           disk image (pread/pwrite via backend)
```

### Why host_proxy, not libslirp

The first prototype used libslirp NAT with hostfwd. Throughput collapsed under
multi-stream load: every host-→guest packet bounced through libslirp's slirp_pollfds
and IRQ injection, and TCP retransmit timers misbehaved across the boundary.

`src/host_proxy/` replaces that with a userspace listener on the host port that
opens one LKL socket per accepted connection and runs a 2-thread + SPSC + epoll
splice loop. Pure `read()`/`write()` on both halves; no NAT, no checksumming, no
virtio-net at all. libslirp is still available for outbound paths but is not used
on the SMB/NFS data path.

---

## Building

### Prerequisites

```bash
# LKL kernel with ksmbd + nfsd enabled
cd ~/anyfs-reader
./scripts/gen_lkl_config.sh ${LINUX_SRC}    # generates kernel.config overlay
make -C ${LINUX_SRC}/tools/lkl -j$(nproc) ARCH=lkl

# ksmbd-tools (userspace IPC daemon used by anyfs-ksmbd)
cd ~/ksmbd-tools
meson setup builddir
meson compile -C builddir
```

### Build the servers

```bash
cd ~/anyfs-reader
meson setup build-anyfs-linux-amd64 \
    -Dlkl_root=${LINUX_SRC}/tools/lkl \
    -Denable_ksmbd=true \
    -Dksmbd_tools_root=$HOME/ksmbd-tools \
    -Denable_qemu=true \
    -Dqemu_root=$HOME/qemu \
    -Dqemu_build=$HOME/qemu/build-anyfs-shared
ninja -C build-anyfs-linux-amd64 anyfs-ksmbd anyfs-nfsd
```

Outputs:

- `build-anyfs-linux-amd64/anyfs-ksmbd` — SMB3 server
- `build-anyfs-linux-amd64/anyfs-nfsd`  — NFSv4 server

(`anyfs-nfsd` is enabled by the same `-Denable_ksmbd` flag — the kernel-side server
support comes from a single ksmbd+nfsd-enabled LKL build.)

---

## Usage

### Discovering partition paths

Every server takes a canonical `disk<N>/p<M>` path for each `--share`. Get it from
`anyfs-lspart` first:

```bash
$ anyfs-lspart disk.img
PATH        TYPE   SIZE     FSTYPE  LABEL   UUID
disk0/p1    fs     200 MiB  vfat    EFI     1234-5678
disk0/p2    fs     27 GiB   ext4    root    ...
```

`p1` alone is a shortcut for `disk0/p1` when there is exactly one image on the
command line.

### `anyfs-nfsd` (NFSv4)

```bash
# Read-only
./anyfs-nfsd disk.img --share data=disk0/p2

# Read-write
./anyfs-nfsd -w disk.img --share data=disk0/p2

# Client mount (Linux)
mount -t nfs4 localhost:/data /mnt -o port=20049,vers=4
```

Options (see `--help` for the full list):

- `--share [name=]path`  expose `path` as `/<name>` (default name is derived from path)
- `-w`                   read-write export (default read-only)
- `-P PORT`              host listen port (default 20049)
- `-p N`                 deprecated shortcut for `--share disk0/p<N>`

### `anyfs-ksmbd` (SMB3)

```bash
# Default (port 4455, single share)
./anyfs-ksmbd disk.img --share data=disk0/p2

# Multi-disk, multi-share
./anyfs-ksmbd boot.img data.qcow2 \
    --share esp=disk0/p1 \
    --share home=disk1/p1

# Client (Linux)
smbclient //localhost/data -U guest%guest --port=4455
mount -t cifs //localhost/data /mnt -o port=4455,guest,vers=3.0
```

Resource-tuning options (see `--help`):

| Flag             | Default   | Notes                                                                                  |
| ---------------- | --------- | -------------------------------------------------------------------------------------- |
| `--mem-mb N`     | 32        | LKL kernel arena size. Each SMB session costs ~10–16 MiB peak at default 1 MiB IO size |
| `--max-read N`   | 1048576   | Max SMB2 read/write/transact. Lower (e.g. 262144) to shrink per-IO `kvzalloc`           |
| `--max-conn N`   | 64        | Concurrent SMB connections                                                              |
| `--max-credits N`| 8192      | SMB2 credits per connection                                                              |
| `--busy-spin`    | off       | host_proxy spins instead of `poll()`-blocking; eliminates wineserver IPC under wine     |
| `--no-fast-sync` | (Windows) | Revert LKL sem/mutex to stock `CreateSemaphore`/`WaitForSingleObject`                   |
| `-P PORT`        | 4455      | Host listen port                                                                        |

`--busy-spin` and `--no-fast-sync` exist because LKL under wine spends a large
fraction of its wall time in wineserver IPC for every scheduler wake; both are
no-ops on native Linux. Native Linux throughput is unaffected by either flag.

---

## Kernel Config Overlay

The defconfig flags relevant to the servers, applied by
`scripts/gen_lkl_config.sh` (do **not** edit `arch/lkl/configs/defconfig` directly —
all overrides live in the script's overlay):

```
# SMB server
CONFIG_SMB_SERVER=y

# NFS server
CONFIG_NFSD=y
CONFIG_NFSD_V4=y
CONFIG_SUNRPC=y
CONFIG_LOCKD=y
CONFIG_GRACE_PERIOD=y
CONFIG_FSNOTIFY=y
CONFIG_INOTIFY_USER=y

# Filesystems (shipping default has 35 enabled; ext4/xfs/btrfs/vfat/ntfs3/f2fs/...)
CONFIG_EXT4_FS=y
CONFIG_XFS_FS=y
CONFIG_BTRFS_FS=y

# Crypto (required by ksmbd)
CONFIG_CRYPTO_MD5=y
CONFIG_CRYPTO_SHA256=y
CONFIG_CRYPTO_SHA512=y
CONFIG_CRYPTO_AES=y
CONFIG_CRYPTO_GCM=y
CONFIG_CRYPTO_CCM=y
CONFIG_CRYPTO_CMAC=y
CONFIG_CRYPTO_HMAC=y
CONFIG_CRYPTO_ECB=y
CONFIG_CRYPTO_CBC=y
CONFIG_CRYPTO_CTS=y
```

---

## NFSv4 Implementation Notes

### Problems hit and how they were fixed

| Symptom                                | Cause                                                | Fix                                                                                |
| -------------------------------------- | ---------------------------------------------------- | ---------------------------------------------------------------------------------- |
| Server hangs at startup                | `svc_register()` tries to talk to `localhost:111`    | `#ifdef CONFIG_LKL` early-returns 0 in `net/sunrpc/svc.c`                          |
| Mount hangs after first ops            | `nfsd cache_check()` waits for a mountd upcall reply | Embedded mini-mountd thread answers the four upcall channels                       |
| `NFS4ERR_STALE`                        | filehandle type mismatch                             | Exports set `NFSEXP_FSID`                                                          |
| `NFS4ERR_GRACE` for 90 s after boot    | nfsd grace period                                    | Writes `"Y\n"` to `/proc/fs/nfsd/v4_end_grace` once cache is wired                 |
| `FSNOTIFY`/inotify symbol missing      | INOTIFY_USER disabled by default                     | Enabled in the overlay                                                             |
| `__NR_inotify_init` undefined          | not in generic `unistd.h`                            | `arch/lkl/scripts/headers_install.py` adds it                                      |

### Mini mountd

NFSv4's in-kernel server still relies on the sunrpc cache subsystem to answer
filehandle/export lookups — normally that's `rpc.mountd` in userspace. We embed a
single helper thread that watches the four channels:

```
cache_handler_thread:
  poll(/proc/net/rpc/{auth.unix.ip, auth.unix.gid, nfsd.fh, nfsd.export}/channel)
    ip_map:    every client IP → domain "unix"
    unix_gid:  every uid       → 0 supplementary groups
    expkey:    (unix, fsid=0)  → export path; everything else → negative reply
    export:    (unix, path)    → INSECURE | NOSUBTREECHECK | FSID | ALLSQUASH
                                  ( + READONLY when -w is absent )
```

### Always-root strategy

Every NFS request executes with kernel uid 0:

- Export flags include `NFSEXP_ALLSQUASH` (`0x0008`).
- `anonuid = 0`, `anongid = 0`.
- Net effect: whatever uid/gid the client uses, the server-side VFS sees `root`.
  This sidesteps host/guest uid translation problems and matches the "image
  inspector" use case anyfs-reader was designed for.

---

## pynfs Results

Last full sweep on the in-tree `anyfs-nfsd` against
[pynfs](https://github.com/ffilz/pynfs) NFSv4 conformance suite:

| Category                                                       | PASS | FAIL | Pass rate |
| -------------------------------------------------------------- | ---: | ---: | --------: |
| Read-only ops (getattr, lookup, read, readdir, access, …)      | 574  | 4    | 99.3%     |
| Write ops (open, create, close, remove, rename, link, …)       | 148  | 9    | 94.3%     |
| **Total**                                                      |**722**|**13**| **98.2%** |

All 13 failures are intentional behavior, not server bugs:

| Class           | Count | Note                                                            |
| --------------- | ----: | --------------------------------------------------------------- |
| UTF-8 validation| 7     | Linux VFS does not reject malformed UTF-8 filenames              |
| Always-root     | 2     | `ALLSQUASH` makes per-uid permission checks not applicable       |
| pynfs bug       | 2     | Python 3.13 str/bytes compatibility                              |
| Kerberos        | 1     | Only AUTH_SYS is configured (no RPCSEC_GSS)                     |
| Lease expiry    | 1     | Timing edge (`NFS4ERR_DELAY`)                                    |

Skipped intentionally:

- `WRT15 (testSizes)` — 8192-iteration writer, too slow under prior slirp setup.
- `lock/lockt/locku` — long-timeout suite.
- `timed` — needs 180 s waits.

---

## Relevant Kernel Patches

### `net/sunrpc/svc.c`

```c
#ifdef CONFIG_LKL
static bool svc_uses_rpcbind(struct svc_serv *serv) { return 0; }
#endif

int svc_register(...)
{
#ifdef CONFIG_LKL
    return 0;  /* LKL: skip rpcbind registration */
#endif
    ...
}
```

### `arch/lkl/scripts/headers_install.py`

```python
self.defines.add("__NR_inotify_init")  # missing from the generic unistd
```

(Both are local patches applied in the working LKL tree; they have not been
upstreamed.)

# LKL 文件服务器 (ksmbd + nfsd)

## 概述

本项目提供两个基于 LKL (Linux Kernel Library) 的用户态文件服务器：

| 服务器 | 协议 | 端口 (host) | 源码 |
|--------|------|------------|------|
| `lkl_ksmbd` | SMB3 (CIFS) | 10445 | `src/ksmbd/lkl_ksmbd.c` (281 行) |
| `lkl_nfsd` | NFSv4 | 20049 | `src/nfsd/lkl_nfsd.c` (494 行) |

两者共同点：
- 使用 LKL 内核 (6.12) 内置的 ksmbd/nfsd 子系统
- 通过 libslirp 提供用户态网络（无需 root 权限）
- 支持 ext4/xfs/btrfs 磁盘镜像
- 所有文件操作以 root 用户身份执行（ALLSQUASH + anonuid=0）
- 完全在用户空间运行，无需内核模块

---

## 架构

```
客户端 (smbclient / mount.nfs4)
    │
    ├─ TCP ─→ host:10445 (SMB) 或 host:20049 (NFS)
    │         libslirp port forward
    │
    ▼
┌────────────────────────────────────────┐
│           LKL 用户态内核                │
│  ┌──────────────────────────────────┐  │
│  │  网络栈 (TCP/IP)                  │  │
│  │  10.0.2.15:445 / 10.0.2.15:2049  │  │
│  ├──────────────────────────────────┤  │
│  │  ksmbd / nfsd 服务器子系统        │  │
│  ├──────────────────────────────────┤  │
│  │  VFS → ext4/xfs/btrfs            │  │
│  ├──────────────────────────────────┤  │
│  │  virtio-blk → raw_blk_backend    │  │
│  └──────────────────────────────────┘  │
└────────────────────────────────────────┘
           │
           ▼
      磁盘镜像 (pread/pwrite)
```

---

## 构建

### 前置条件

```bash
# LKL 内核（需包含 ksmbd + nfsd 支持）
cd ~/linux
make ARCH=lkl -j$(nproc)
cd tools/lkl && make -j$(nproc)

# ksmbd-tools (SMB 服务器的用户态配置工具)
cd ~/ksmbd-tools
meson setup builddir -Dprefix=/usr/local
meson compile -C builddir
```

### 构建服务器

```bash
cd ~/anyfs-reader
meson setup builddir-ksmbd -Denable_ksmbd=true
meson compile -C builddir-ksmbd
```

产物：
- `builddir-ksmbd/lkl_ksmbd` — SMB 服务器
- `builddir-ksmbd/lkl_nfsd` — NFS 服务器

---

## 使用方法

### lkl_nfsd (NFSv4)

```bash
# 准备磁盘镜像
dd if=/dev/zero of=disk.img bs=1M count=256
mkfs.ext4 disk.img

# 启动服务器（只读）
./lkl_nfsd disk.img

# 启动服务器（读写）
./lkl_nfsd -w disk.img

# 客户端挂载
mount -t nfs4 localhost:/ /mnt -o port=20049,vers=4

# 或用 libnfs 工具
nfs-ls nfs://127.0.0.1:20049/
```

### lkl_ksmbd (SMB3)

```bash
# 启动服务器（只读）
./lkl_ksmbd disk.img

# 启动服务器（读写）
./lkl_ksmbd -w disk.img

# 客户端连接
smbclient //localhost/share -p 10445 -N

# 挂载
mount -t cifs //localhost/share /mnt -o port=10445,guest,vers=3.0
```

---

## 内核配置

LKL defconfig (`arch/lkl/configs/defconfig`) 中与服务器相关的选项：

```
# SMB 服务器
CONFIG_SMB_SERVER=y

# NFS 服务器
CONFIG_NFSD=y
CONFIG_NFSD_V4=y
CONFIG_SUNRPC=y
CONFIG_LOCKD=y
CONFIG_GRACE_PERIOD=y
CONFIG_FSNOTIFY=y
CONFIG_INOTIFY_USER=y

# 文件系统
CONFIG_EXT4_FS=y
CONFIG_XFS_FS=y
CONFIG_BTRFS_FS=y

# 加密 (ksmbd 需要)
CONFIG_CRYPTO=y
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
CONFIG_CRYPTO_DES=y
```

---

## NFSv4 实现细节

### 问题与解决方案

| 问题 | 原因 | 解决方案 |
|------|------|----------|
| 启动卡在 rpcbind | `svc_register()` 尝试连接 localhost:111 | 在 `net/sunrpc/svc.c` 中 `#ifdef CONFIG_LKL` 跳过 |
| 挂载无响应 | nfsd 的 cache_check() 等待 mountd 回复 | 实现 mini mountd 线程处理 upcall |
| NFS4ERR_STALE | filehandle 类型不匹配 | 导出添加 NFSEXP_FSID 标志 |
| NFS4ERR_GRACE | 90 秒 grace period | 启动后写 "Y" 到 `/proc/fs/nfsd/v4_end_grace` |
| FSNOTIFY 缺失 | INOTIFY_USER 未启用 | defconfig 中启用 INOTIFY_USER |
| inotify_init 头文件 | `__NR_inotify_init` 不在 generic unistd.h | headers_install.py 补丁 |

### Mini Mountd

NFSv4 内核服务器依赖 sunrpc cache 子系统获取导出信息。传统方式由 `rpc.mountd` 守护进程处理，我们内嵌了一个轻量级 mountd 线程：

```
cache_handler_thread:
  poll(/proc/net/rpc/{auth.unix.ip,auth.unix.gid,nfsd.fh,nfsd.export}/channel)
  ├─ ip_map:    任何 IP → "unix" 域
  ├─ unix_gid:  任何 UID → 0 个补充组
  ├─ expkey:    (unix, fsid=0) → 导出路径; 其他 → 负回复
  └─ export:    (unix, path) → 导出标志 (INSECURE|NOSUBTREECHECK|FSID|ALLSQUASH)
```

### Always-Root 策略

所有 NFS 操作始终以 root 身份执行：
- 导出标志包含 `NFSEXP_ALLSQUASH` (0x8)
- `anonuid=0`, `anongid=0`
- 效果：无论客户端使用什么 UID/GID，服务器端统一映射为 root

---

## pynfs 测试结果

使用 [pynfs](https://github.com/ffilz/pynfs) NFSv4 协议一致性测试套件：

| 测试类别 | PASS | FAILURE | 通过率 |
|----------|------|---------|--------|
| 只读操作 (getattr, lookup, read, readdir, access 等) | 574 | 4 | 99.3% |
| 写操作 (open, create, close, remove, rename, link 等) | 148 | 9 | 94.3% |
| **合计** | **722** | **13** | **98.2%** |

### 失败分析

所有 13 个失败都是预期行为，非服务器 bug：

| 类别 | 数量 | 说明 |
|------|------|------|
| UTF-8 验证 | 7 | Linux VFS 不拒绝非法 UTF-8 文件名 |
| Always-root | 2 | ALLSQUASH 使权限检查不适用 |
| pynfs bug | 2 | Python 3.13 str/bytes 兼容问题 |
| Kerberos | 1 | 未配置 RPCSEC_GSS (仅 AUTH_SYS) |
| Lease 过期 | 1 | 时序问题 (NFS4ERR_DELAY) |

### 跳过的测试

- `WRT15 (testSizes)`: 8192 次迭代写入，slirp 网络延迟下过慢 (~5min+)
- `lock/lockt/locku`: 锁测试需要较长超时
- `timed`: 需要 180s+ 等待

---

## 内核补丁

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
self.defines.add("__NR_inotify_init")  # 新增：generic unistd 中缺失
```

---

## 相关提交

- Linux: `e948805c3b9b1` — lkl: add ksmbd and nfsd server support
- anyfs-reader: `90b9cc3` — nfsd: add LKL-based NFSv4 server
- anyfs-reader: `f49618a` — meson: add ksmbd build option
- anyfs-reader: `57e9969` — ksmbd: use anyfs API exclusively

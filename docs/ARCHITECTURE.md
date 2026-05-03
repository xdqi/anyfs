# AnyFS-Reader 架构设计

## 1. 设计决策

| 决策項 | 结论 | 原因 |
|--------|------|------|
| API 风格 | 薄封装 + 直接 LKL syscall | 旧 API 是无意义的 passthrough |
| Host Operations | 保持原生 posix-host | 不引入 GLib 替换 |
| QEMU 集成 | 直接静态链接 libblock.a | blk_pread 同步调用，零 LKL 修改 |
| I/O 模式 | **纯同步** | LKL 异步支持坑太大 |
| MMU | CONFIG_MMU=n | 文件系统不需要 MMU |
| LKL 修改 | **不改 LKL** | 同步方案无需任何修改 |

---

## 2. 架构概览

```
┌─────────────────────────────────────────────────┐
│  应用 (anyfs-shell / 自定义程序)                  │
│  直接调用 LKL syscalls:                          │
│    lkl_sys_open, lkl_sys_read, lkl_sys_lstat,   │
│    lkl_opendir, lkl_sys_statfs, ...              │
├─────────────────────────────────────────────────┤
│  anyfs.h  (4 函数: init/halt + disk_add/remove) │
├─────────────────────────────────────────────────┤
│  libanyfs_core.a                                 │
│  ├── anyfs.c             内核生命周期 + 磁盘管理  │
│  ├── raw_blk_backend.c   pread 后端 (.img)      │
│  ├── gio_blk_backend.c   GIO 同步后端 (跨平台)   │
│  └── qemu_blk_backend.c  QEMU blk_pread (qcow2) │
├─────────────────────────────────────────────────┤
│  liblkl.a (LKL 6.12, CONFIG_MMU=n)             │
├─────────────────────────────────────────────────┤
│  libblock.a + libqemuutil.a (QEMU block layer)  │
└─────────────────────────────────────────────────┘
```

### API 设计哲学

旧设计 (`anyfs_api.h`) 封装了 `anyfs_open/read/close/opendir/readdir/closedir` 等十几个函数——但它们只是 `lkl_sys_*` 的 1:1 wrapper，没有附加价值。

新设计 (`anyfs.h`) 只封装两件事：
1. **内核生命周期** — `anyfs_kernel_init/halt`（隐藏 `lkl_init` + `lkl_start_kernel` 的参数处理）
2. **多后端磁盘注册** — `anyfs_disk_add/remove`（根据 flags 选择 raw/gio/qemu ops，调 `lkl_disk_add`）

之后用户直接使用 LKL 的完整 syscall API，没有功能限制。

---

## 3. I/O 路径

LKL 的 virtio I/O 路径在**同一宿主机线程**中同步完成：

```
lkl_sys_read()
  → VFS → submit_bio()
  → virtqueue_notify → writel(QUEUE_NOTIFY)
  → iomem_access → virtio_process_queue
  → ops->request(disk, &req)   ← 后端在这里执行
  → virtio_req_complete → lkl_trigger_irq
  → writel 返回 → bio 完成
```

每个后端的 `request()` 实现：
- **raw**: `pread(fd, buf, len, offset)`
- **gio**: `g_seekable_seek()` + `g_input_stream_read()`
- **qemu**: `blk_pread(blk, offset, len, buf, 0)` (内部用协程)

---

## 4. 块后端接口

```c
// LKL 的块设备接口 (来自 lkl_host.h)
struct lkl_dev_blk_ops {
    int (*get_capacity)(struct lkl_disk disk, unsigned long long *res);
    int (*request)(struct lkl_disk disk, struct lkl_blk_req *req);
};

// anyfs 后端抽象 (src/core/anyfs_backend.h)
struct anyfs_backend_ops {
    const char *name;
    int (*open)(const char *path, int readonly, struct lkl_disk *disk_out);
    void (*close)(struct lkl_disk *disk);
};
```

`anyfs_disk_add()` 内部：
1. 根据 flags 选择 `anyfs_backend_ops`（默认 QEMU > raw）
2. 调用 `ops->open()` 得到填充好的 `lkl_disk`（包含 `lkl_dev_blk_ops`）
3. 调用 `lkl_disk_add()` 注册到 LKL 内核

---

## 5. QEMU Block Backend 细节

### 5.1 符号隔离

- 编译 `libanyfs_core.a` 时 `-fvisibility=hidden`
- QEMU 内部数千符号全部 hidden

### 5.2 AioContext 线程问题

LKL 的 `request()` 可能从任意线程调用。QEMU 的 `blk_pread` 需要当前线程有
AioContext。解决：在 `request()` 入口检查并设置：

```c
static __thread bool aio_ctx_set = false;
if (!aio_ctx_set) {
    qemu_set_current_aio_context(qemu_get_aio_context());
    aio_ctx_set = true;
}
```

### 5.3 格式驱动注册

QEMU 格式驱动用 `block_init()` 注册 (`__attribute__((constructor))`)。
链接时必须 `--whole-archive libblock.a`。

### 5.4 QEMU 构建

```bash
cd ~/qemu && mkdir -p build-anyfs2 && cd build-anyfs2
../configure --disable-system --disable-user --enable-tools \
    --disable-guest-agent --disable-docs --disable-gtk --disable-sdl \
    --disable-iscsi --disable-nfs --disable-ssh --disable-curl \
    --target-list=""
ninja libqemuutil.a libqom.a libauthz.a libcrypto.a libio.a \
      libblock.a libevent-loop-base.a
```

---

## 6. 性能数据

详细基准测试结果见 [README.md](../README.md#benchmarks)。

概要: raw ~1.3 GB/s, gio ~1.1 GB/s, qemu ~0.8 GB/s (150 文件 ~150MB ext4)。
单次 4KB 延迟: raw ~29µs, gio ~34µs。

---

## 7. 放弃异步的原因

尝试过的异步方案：
1. **GIO async backend** — 线程桥接开销 +15µs/op，对同步 LKL 无收益
2. **Linux AIO (io_submit) threadless** — O_DIRECT 绕过 host cache，mount 慢 6x
3. **QEMU blk_aio_preadv** — `lkl_trigger_irq` 从外部线程无法唤醒 idle loop

根本问题：LKL 的 `posix_idle` 用 `poll()` 等待事件，外部线程设置 IRQ pending
后无法唤醒 poll()。修改 LKL idle loop 工作量大且侵入性强。

**结论**: 同步方案对绝大多数场景够用 (29µs/op ≈ 34K IOPS)。

---

## 8. 路线图

| Phase | 内容 | 状态 |
|-------|------|:----:|
| PoC | LKL 6.12 编译, raw mount 验证 | ✅ |
| 2 | anyfs API + raw 后端 | ✅ |
| 2a | GIO 同步后端 | ✅ |
| 3 | QEMU block 后端 (qcow2/vmdk/vdi) | ✅ |
| 4 | CLI Shell (guestfish 风格) | ✅ |
| 5 | API 精简 (anyfs.h 4 函数) | ✅ |
| 6 | lkl-busybox (文件操作 applet) | 构想中 |
| 7 | ksmbd SMB3 服务器 | ✅ |
| 8 | nfsd NFSv4 服务器 | ✅ (pynfs 98.2% pass) |
| 9 | GTK3 GUI 文件管理器 | ✅ |

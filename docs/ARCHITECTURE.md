# AnyFS-Reader 架构设计

## 1. 设计决策

| 决策項 | 结论 | 原因 |
|--------|------|------|
| Host Operations | 保持原生 posix-host/nt-host | 不引入 GLib 替换 |
| QEMU 集成 | 直接静态链接 libblock.a | blk_pread 同步调用，零 LKL 修改 |
| I/O 模式 | **纯同步** | LKL 异步支持坑太大 (idle loop/IRQ 唤醒问题) |
| MMU | CONFIG_MMU=n | 文件系统不需要 MMU |
| LKL 修改 | **不改 LKL** | 同步方案无需任何修改 |

---

## 2. 架构概览

```
┌─────────────────────────────────────────────────┐
│  应用 (CLI / GUI / Server)                       │
├─────────────────────────────────────────────────┤
│  anyfs_api.h  (ABI 防火墙, 纯 stdint.h 类型)    │
├─────────────────────────────────────────────────┤
│  libanyfs_core.a                                 │
│  ├── anyfs_core.c      API 实现, LKL 生命周期    │
│  ├── raw_blk_backend.c   pread 后端 (.img)      │
│  ├── gio_blk_backend.c   GIO 同步后端 (跨平台)   │
│  └── qemu_blk_backend.c  QEMU blk_pread (qcow2) │
├─────────────────────────────────────────────────┤
│  liblkl.a (LKL 6.12, CONFIG_MMU=n)             │
├─────────────────────────────────────────────────┤
│  libblock.a + libqemuutil.a (QEMU block layer)  │
└─────────────────────────────────────────────────┘
```

---

## 3. I/O 路径

LKL 的 virtio I/O 路径在**同一宿主机线程**中同步完成：

```
lkl_sys_read()
  → VFS → submit_bio()
  → virtqueue_notify → writel(QUEUE_NOTIFY)
  → iomem_access → virtio_process_queue
  → ops->request(disk, &req)   ← 我们的后端在这里执行
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
struct lkl_dev_blk_ops {
    int (*get_capacity)(struct lkl_disk disk, unsigned long long *res);
    int (*request)(struct lkl_disk disk, struct lkl_blk_req *req);
};

struct lkl_blk_req {
    unsigned int type;          // READ=0, WRITE=1, FLUSH=4
    unsigned long long sector;  // 偏移 (512B 扇区)
    struct iovec *buf;
    int count;
};
```

---

## 5. QEMU Block Backend 细节

### 5.1 符号隔离

- 编译 `libanyfs_core.a` 时 `-fvisibility=hidden`
- 仅 `anyfs_api.h` 中的函数用 `ANYFS_API` 导出
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

### 5.4 构建

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

## 6. ABI 设计规则

- 固定宽度整数: `int32_t`, `int64_t`, `uint32_t`, `uint64_t`
- 绝无 `long`, `size_t` (Win32 LLP64 vs Linux LP64 宽度不同)
- 不透明句柄: `AnyfsContext*`, `AnyfsMount*`, `AnyfsDir*`
- Windows: `__cdecl` 调用约定 + `__declspec(dllexport)`

---

## 7. 性能数据

大文件 (128MB ext4, 超过 LKL 64MB page cache):

| Backend | 10000 reads (4KB seq) | per-op |
|---------|----------------------|--------|
| raw | 286 ms | 29 µs |
| gio-sync | 343 ms | 34 µs |

小文件 (page cache 命中):

| Backend | 10000 open+read+close | per-op |
|---------|----------------------|--------|
| raw | 477 ms | 48 µs |
| gio-sync | 142 ms | 14 µs |

---

## 8. 放弃异步的原因

尝试过的异步方案：
1. **GIO async backend** — 线程桥接开销 +15µs/op，对同步 LKL 无收益
2. **Linux AIO (io_submit) threadless** — O_DIRECT 绕过 host cache，mount 慢 6x
3. **QEMU blk_aio_preadv** — `lkl_trigger_irq` 从外部线程无法唤醒 idle loop

根本问题：LKL 的 `posix_idle` 用 `poll()` 等待事件，外部线程设置 IRQ pending
后无法唤醒 poll()。修改 LKL idle loop 工作量大且侵入性强。

**结论**: 同步方案对绝大多数场景够用 (29µs/op ≈ 34K IOPS)。

---

## 9. 路线图

| Phase | 内容 | 状态 |
|-------|------|:----:|
| PoC | LKL 6.12 编译, raw mount 验证 | ✅ |
| 2 | anyfs API + raw 后端 | ✅ |
| 2a | GIO 同步后端 | ✅ |
| 3 | QEMU block 后端 (qcow2/vmdk/vdi) | ✅ |
| 4 | CLI Shell | 待定 |
| 5 | Server 模式 (ksmbd/nfsd) | 待定 |

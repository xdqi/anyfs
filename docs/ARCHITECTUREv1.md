# AnyFS-Reader 详细架构设计

## 1. 调研结论：依赖层关键接口摘要

### 1.1 LKL Host Operations (`arch/lkl/include/uapi/asm/host_ops.h`)

```c
struct lkl_host_operations {
    const char *virtio_devices;       // virtio mmio 设备列表字符串
    void (*print)(const char *str, int len);
    void (*panic)(void);

    // --- 信号量 ---
    struct lkl_sem* (*sem_alloc)(int count);
    void (*sem_free)(struct lkl_sem *sem);
    void (*sem_up)(struct lkl_sem *sem);
    void (*sem_down)(struct lkl_sem *sem);

    // --- 互斥锁 ---
    struct lkl_mutex *(*mutex_alloc)(int recursive);
    void (*mutex_free)(struct lkl_mutex *mutex);
    void (*mutex_lock)(struct lkl_mutex *mutex);
    void (*mutex_unlock)(struct lkl_mutex *mutex);

    // --- 线程 ---
    lkl_thread_t (*thread_create)(void (*f)(void *), void *arg);
    void (*thread_detach)(void);
    void (*thread_exit)(void);
    int (*thread_join)(lkl_thread_t tid);
    lkl_thread_t (*thread_self)(void);
    int (*thread_equal)(lkl_thread_t a, lkl_thread_t b);
    void *(*thread_stack)(unsigned long *size);

    // --- TLS ---
    struct lkl_tls_key *(*tls_alloc)(void (*destructor)(void *));
    void (*tls_free)(struct lkl_tls_key *key);
    int (*tls_set)(struct lkl_tls_key *key, void *data);
    void *(*tls_get)(struct lkl_tls_key *key);

    // --- 内存 ---
    void* (*mem_alloc)(unsigned long);
    void (*mem_free)(void *);
    void* (*page_alloc)(unsigned long size);
    void (*page_free)(void *addr, unsigned long size);

    // --- 时钟与定时器 ---
    unsigned long long (*time)(void);
    void* (*timer_alloc)(void (*fn)(void));
    int (*timer_set_oneshot)(void *timer, unsigned long delta);
    void (*timer_free)(void *timer);

    // --- I/O 内存 ---
    void* (*ioremap)(long addr, int size);
    int (*iomem_access)(const volatile void *addr, void *val, int size, int write);

    // --- 跳转缓冲 ---
    void (*jmp_buf_set)(struct lkl_jmp_buf *jmpb, void (*f)(void));
    void (*jmp_buf_longjmp)(struct lkl_jmp_buf *jmpb, int val);

    // --- 内存操作 ---
    void* (*memcpy)(void *dest, const void *src, unsigned long count);
    void* (*memset)(void *s, int c, unsigned long count);
    void* (*memmove)(void *dest, const void *src, unsigned long count);

    // --- 虚拟内存 ---
    void* (*mmap)(void *addr, unsigned long size, enum lkl_prot prot);
    int (*munmap)(void *addr, unsigned long size);

    // --- 共享内存 (MMU) ---
    void (*shmem_init)(unsigned long size);
    void *(*shmem_mmap)(void *addr, unsigned long pg_off, unsigned long size, enum lkl_prot prot);

    // --- PCI ---
    struct lkl_dev_pci_ops *pci_ops;
};
```

### 1.2 LKL 块设备接口 (`tools/lkl/include/lkl_host.h` + `lkl.h`)

```c
struct lkl_disk {
    void *dev;          // 内部 virtio_blk_dev 指针
    union {
        int fd;         // POSIX 文件描述符
        void *handle;   // Windows NT handle
    };
    struct lkl_dev_blk_ops *ops;  // 可选，自定义块操作
};

struct lkl_dev_blk_ops {
    int (*get_capacity)(struct lkl_disk disk, unsigned long long *res);
    int (*request)(struct lkl_disk disk, struct lkl_blk_req *req);  // 当前是同步的!
};

struct lkl_blk_req {
    unsigned int type;              // READ=0, WRITE=1, FLUSH=4
    unsigned int prio;
    unsigned long long sector;      // 以 512B 为单位的偏移
    struct iovec *buf;
    int count;
};
```

**关键发现**: LKL 当前的 virtio_blk `blk_enqueue()` 假设 `ops->request()` 是同步的，
调用后立即 `virtio_req_complete(req, 0)`。要实现异步，需要修改此处。

### 1.3 QEMU Block Layer 关键接口

```c
// 打开镜像 (qcow2/raw/vhdx 等)
BlockBackend *blk_new_open(const char *filename, const char *reference,
                           QDict *options, int flags, Error **errp);

// 异步读写
BlockAIOCB *blk_aio_preadv(BlockBackend *blk, int64_t offset,
                           QEMUIOVector *qiov, BdrvRequestFlags flags,
                           BlockCompletionFunc *cb, void *opaque);

// 完成回调签名
typedef void BlockCompletionFunc(void *opaque, int ret);

// AioContext 是 GSource 的子类!
struct AioContext {
    GSource source;  // 第一个成员，可直接挂到 GMainContext
    ...
};

// 获取 GSource 以挂载到 GMainLoop
GSource *aio_get_g_source(AioContext *ctx);
```

**关键发现**: QEMU 的 `AioContext` 本身就是一个 `GSource`，通过
`g_source_attach(aio_get_g_source(ctx), g_main_context)` 即可将 QEMU 的全部 I/O
完成事件接入 GLib 主循环。

---

## 2. 详细架构设计

### 2.1 整体分层图

```
┌─────────────────────────────────────────────────────────────┐
│  上层应用 (CLI / Server / GUI)                               │
│  只依赖 anyfs_api.h (stdint.h 类型，无 long/size_t)          │
├─────────────────────────────────────────────────────────────┤
│  ABI 防火墙 (anyfs_core.dll / libanyfs_core.so)              │
│  ┌──────────────────────────────────────────────────────┐   │
│  │  anyfs_core.c                                         │   │
│  │  - 初始化 GMainLoop                                   │   │
│  │  - 初始化 QEMU AioContext + attach to GMainContext    │   │
│  │  - 实现 lkl_host_operations (GLib 后端)               │   │
│  │  - 启动 LKL 内核                                      │   │
│  │  - 提供 anyfs_api.h 暴露的函数                         │   │
│  ├──────────────────────────────────────────────────────┤   │
│  │  lkl_env.c  (GLib 环境劫持层)                         │   │
│  │  - struct lkl_host_operations 全部映射到 GLib          │   │
│  ├──────────────────────────────────────────────────────┤   │
│  │  qemu_blk_backend.c (QEMU 块设备桥接层)               │   │
│  │  - 实现 struct lkl_dev_blk_ops                        │   │
│  │  - request() → bdrv_aio_preadv + 返回 -EINPROGRESS   │   │
│  │  - 完成回调 → virtio_req_complete()                   │   │
│  └──────────────────────────────────────────────────────┘   │
├─────────────────────────────────────────────────────────────┤
│  LKL (vmlinux 静态链接)  │  QEMU libblock (静态/动态链接)    │
│  ~/linux                  │  ~/qemu                          │
├─────────────────────────────────────────────────────────────┤
│  GLib-2.0 (系统库 / subproject)                              │
│  提供: GMainLoop, GMutex, GThread, g_malloc, GSource ...     │
└─────────────────────────────────────────────────────────────┘
```

### 2.2 文件结构设计

```
anyfs-reader/
├── meson.build                  # 顶层构建
├── meson_options.txt            # 构建选项
├── ARCHITECTURE.md              # 本文件
│
├── include/
│   └── anyfs_api.h              # 公开 ABI 接口 (纯 stdint.h)
│
├── src/
│   ├── core/
│   │   ├── meson.build          # 构建 libanyfs_core.so/dll
│   │   ├── anyfs_core.c         # 核心初始化/销毁，API 实现
│   │   ├── lkl_env.c            # lkl_host_operations 的 GLib 实现
│   │   ├── lkl_env.h            # 内部头文件
│   │   ├── qemu_blk_backend.c   # QEMU Block Layer 桥接
│   │   ├── qemu_blk_backend.h   # 内部头文件
│   │   └── raw_blk_backend.c    # Phase 2: 简单 pread 同步后端
│   │
│   ├── cli/
│   │   ├── meson.build
│   │   └── main.c              # CLI 交互 shell
│   │
│   └── common/
│       └── platform.h          # 平台检测, ABI 标注宏
│
├── subprojects/                 # meson wrap files
│   ├── lkl.wrap
│   └── qemu-block.wrap
│
└── tests/
    ├── test_lkl_boot.c         # Phase 1 验证
    ├── test_raw_mount.c        # Phase 2 验证
    └── test_qcow2_mount.c     # Phase 3 验证
```

### 2.3 ABI 防火墙接口设计 (`anyfs_api.h`)

```c
#ifndef ANYFS_API_H
#define ANYFS_API_H

#include <stdint.h>

#ifdef _WIN32
  #ifdef ANYFS_CORE_BUILDING
    #define ANYFS_API __declspec(dllexport)
  #else
    #define ANYFS_API __declspec(dllimport)
  #endif
  #define ANYFS_CALL __cdecl
#else
  #define ANYFS_API __attribute__((visibility("default")))
  #define ANYFS_CALL
#endif

/* === 不透明句柄 === */
typedef struct AnyfsContext AnyfsContext;
typedef struct AnyfsMount  AnyfsMount;
typedef struct AnyfsDir    AnyfsDir;
typedef int64_t            anyfs_fd_t;

/* === 初始化/销毁 === */
ANYFS_API int32_t ANYFS_CALL anyfs_init(AnyfsContext **ctx_out);
ANYFS_API void    ANYFS_CALL anyfs_destroy(AnyfsContext *ctx);

/* === 镜像操作 === */
ANYFS_API int32_t ANYFS_CALL anyfs_open_image(
    AnyfsContext *ctx,
    const char *image_path,     /* UTF-8 */
    uint32_t flags              /* ANYFS_OPEN_READONLY = 1 */
);

ANYFS_API int32_t ANYFS_CALL anyfs_mount(
    AnyfsContext *ctx,
    const char *fs_type,        /* "ext4", "xfs", "btrfs", NULL=auto */
    uint32_t part_index,        /* 0 = 整个磁盘 */
    AnyfsMount **mount_out
);

ANYFS_API int32_t ANYFS_CALL anyfs_umount(AnyfsMount *mnt);

/* === 文件操作 === */
ANYFS_API anyfs_fd_t ANYFS_CALL anyfs_open(
    AnyfsMount *mnt,
    const char *path,           /* UTF-8, 绝对路径 */
    uint32_t flags
);

ANYFS_API int64_t ANYFS_CALL anyfs_read(
    AnyfsMount *mnt,
    anyfs_fd_t fd,
    void *buf,
    uint64_t count
);

ANYFS_API int32_t ANYFS_CALL anyfs_close(AnyfsMount *mnt, anyfs_fd_t fd);

/* === 目录操作 === */
typedef struct {
    uint8_t  type;              /* DT_REG, DT_DIR, DT_LNK ... */
    uint64_t inode;
    uint64_t size;
    char     name[256];         /* UTF-8 */
} AnyfsEntry;

ANYFS_API AnyfsDir* ANYFS_CALL anyfs_opendir(AnyfsMount *mnt, const char *path);
ANYFS_API int32_t   ANYFS_CALL anyfs_readdir(AnyfsDir *dir, AnyfsEntry *entry_out);
ANYFS_API int32_t   ANYFS_CALL anyfs_closedir(AnyfsDir *dir);

/* === 事件循环控制 (高级) === */
ANYFS_API int32_t ANYFS_CALL anyfs_run_once(AnyfsContext *ctx, int32_t timeout_ms);

/* === 错误码 === */
#define ANYFS_OK             0
#define ANYFS_ERR_NOMEM     (-1)
#define ANYFS_ERR_IO        (-2)
#define ANYFS_ERR_INVAL     (-3)
#define ANYFS_ERR_NOENT     (-4)
#define ANYFS_ERR_NOTDIR    (-5)
#define ANYFS_ERR_BUSY      (-6)
#define ANYFS_ERR_NOSYS     (-7)
#define ANYFS_ERR_FORMAT    (-8)

#endif /* ANYFS_API_H */
```

**注意事项**:
- 所有整数使用 `int32_t`, `int64_t`, `uint32_t`, `uint64_t`
- 绝无 `long`, `size_t`, `ssize_t` 出现
- `AnyfsEntry.name` 使用固定长度数组避免跨 ABI 的指针/分配问题
- 回调如需暴露给 Windows，需使用 `__attribute__((ms_abi))` 标注

---

## 3. 异步 I/O 数据流闭环 (详细时序)

```
┌──────────┐        ┌──────────────┐       ┌─────────────────┐      ┌──────────────┐
│ LKL VFS  │        │ virtio_blk   │       │ qemu_blk_backend│      │ GMainLoop    │
│ (内核态) │        │ (tools/lkl)  │       │ (我们的桥接)     │      │ (GLib)       │
└────┬─────┘        └──────┬───────┘       └────────┬────────┘      └──────┬───────┘
     │                     │                        │                       │
     │ lkl_sys_read()      │                        │                       │
     │ ──────────────────► │                        │                       │
     │                     │ blk_enqueue(req)       │                       │
     │                     │ ─────────────────────► │                       │
     │                     │                        │                       │
     │                     │                        │ blk_aio_preadv(       │
     │                     │                        │   blk, offset, qiov,  │
     │                     │                        │   0, aio_complete_cb, │
     │                     │                        │   req_ctx)            │
     │                     │                        │ ─────────────────────►│
     │                     │                        │                       │
     │                     │  return -EINPROGRESS   │                       │
     │                     │ ◄───────────────────── │                       │
     │                     │                        │                       │
     │  LKL 线程挂起      │                        │                       │
     │  (sem_down)         │                        │                       │
     │ ...                 │                        │                       │
     │                     │                        │    [I/O 完成]          │
     │                     │                        │ ◄─────────────────────│
     │                     │                        │                       │
     │                     │                        │ aio_complete_cb() {   │
     │                     │  virtio_req_complete() │   copy data           │
     │                     │ ◄───────────────────── │   virtio_req_complete │
     │                     │                        │   sem_up(完成信号)    │
     │                     │                        │ }                     │
     │  LKL 线程唤醒       │                        │                       │
     │ ◄────────────────── │                        │                       │
     │  返回数据            │                        │                       │
     ▼                     ▼                        ▼                       ▼
```

### 3.1 对 LKL virtio_blk 的必要修改

当前 `tools/lkl/lib/virtio_blk.c` 中 `blk_enqueue()`:
```c
// 现状: 同步
t->status = blk_dev->ops->request(blk_dev->disk, &lkl_req);
virtio_req_complete(req, 0);  // 立即完成
```

需要改为支持异步返回:
```c
// 修改后: 支持异步
int status = blk_dev->ops->request(blk_dev->disk, &lkl_req);
if (status != LKL_DEV_BLK_STATUS_PENDING) {
    // 同步完成 (兼容原有 raw 后端)
    t->status = status;
    virtio_req_complete(req, 0);
} else {
    // 异步: request() 已保存 req 指针, 稍后由回调调用 virtio_req_complete
    return 0;  // 不调用 virtio_req_complete，等回调
}
```

新增状态码:
```c
#define LKL_DEV_BLK_STATUS_PENDING  255  // 请求已入队，异步完成
```

### 3.2 QEMU 桥接层设计 (`qemu_blk_backend.c`)

```c
struct qemu_blk_context {
    BlockBackend *blk;           // QEMU BlockBackend 句柄
    AioContext   *aio_ctx;       // QEMU AioContext (即 GSource)
    GMainContext *gmain_ctx;     // GLib 主上下文
    uint64_t      capacity;      // 镜像容量 (bytes)
};

struct async_req {
    struct virtio_req *vreq;     // LKL virtio request
    struct lkl_blk_req *lkl_req;
    QEMUIOVector qiov;
    struct qemu_blk_context *qctx;
};

// 完成回调 - 在 GMainLoop 迭代中被调用
static void aio_complete_cb(void *opaque, int ret) {
    struct async_req *ar = opaque;
    struct _virtio_req *_req = container_of(ar->vreq, struct _virtio_req, req);

    // 设置完成状态
    struct virtio_blk_req_trailer *t =
        ar->vreq->buf[ar->vreq->buf_count - 1].iov_base;
    t->status = (ret == 0) ? LKL_DEV_BLK_STATUS_OK : LKL_DEV_BLK_STATUS_IOERR;

    // 唤醒 LKL
    virtio_req_complete(ar->vreq, 0);

    qemu_iovec_destroy(&ar->qiov);
    g_free(ar);
}

// request 回调 - 在 LKL 的 virtio 线程中被调用
static int qemu_blk_request(struct lkl_disk disk, struct lkl_blk_req *req) {
    struct qemu_blk_context *qctx = disk.handle;
    struct async_req *ar = g_new0(struct async_req, 1);

    // 构建 QEMUIOVector
    qemu_iovec_init_external(&ar->qiov, req->buf, req->count);
    ar->qctx = qctx;
    // ar->vreq 需要从调用者传入 (需要修改接口)

    int64_t offset = (int64_t)req->sector * 512;

    switch (req->type) {
    case LKL_DEV_BLK_TYPE_READ:
        blk_aio_preadv(qctx->blk, offset, &ar->qiov, 0,
                       aio_complete_cb, ar);
        break;
    case LKL_DEV_BLK_TYPE_WRITE:
        blk_aio_pwritev(qctx->blk, offset, &ar->qiov, 0,
                        aio_complete_cb, ar);
        break;
    case LKL_DEV_BLK_TYPE_FLUSH:
        blk_aio_flush(qctx->blk, aio_complete_cb, ar);
        break;
    }
    return LKL_DEV_BLK_STATUS_PENDING;
}
```

### 3.3 GLib 主循环线程模型

```
┌─────────────────────────────────────────────┐
│            主线程 (GMainLoop 线程)           │
│                                             │
│  GMainLoop *loop;                           │
│  GMainContext *ctx = g_main_context_default()│
│                                             │
│  // 挂载 QEMU AioContext 作为 GSource       │
│  g_source_attach(aio_get_g_source(aio_ctx), │
│                  ctx);                       │
│                                             │
│  g_main_loop_run(loop);                     │
│  // 或在 CLI 同步模式下:                     │
│  // while (pending) g_main_context_iteration│
│                                             │
├─────────────────────────────────────────────┤
│            LKL 虚拟线程 (GThread)            │
│                                             │
│  // 由 lkl_host_ops.thread_create 创建      │
│  // 执行内核调度器、VFS、文件系统代码         │
│  // 通过 sem_down() 等待 I/O 完成           │
│                                             │
└─────────────────────────────────────────────┘
```

**CLI 同步模式实现**:
```c
// 用户调用 anyfs_read() 时
int64_t anyfs_read(AnyfsMount *mnt, anyfs_fd_t fd, void *buf, uint64_t count) {
    // 在 LKL 线程中发起 lkl_sys_read (会触发 block I/O)
    // LKL 线程通过 sem_down 挂起

    // 主线程驱动 GMainLoop 直到 LKL 操作完成
    while (!operation_complete) {
        g_main_context_iteration(ctx, TRUE);  // blocking=TRUE
    }

    return result;
}
```

---

## 4. GLib 环境劫持映射表 (`lkl_env.c`)

| LKL host_ops 字段 | GLib 实现 | 备注 |
|---|---|---|
| `sem_alloc` | `g_cond_new` + `g_mutex_new` + counter | GLib 无原生 semaphore |
| `sem_free` | `g_cond_free` + `g_mutex_free` | |
| `sem_up` | `g_mutex_lock` + counter++ + `g_cond_signal` | |
| `sem_down` | `g_mutex_lock` + while(count<=0) `g_cond_wait` | |
| `mutex_alloc` | `g_rec_mutex_new` / `g_mutex_new` | recursive 参数决定 |
| `mutex_free/lock/unlock` | 对应 GLib 函数 | |
| `thread_create` | `g_thread_new` | |
| `thread_join` | `g_thread_join` | |
| `thread_self` | `g_thread_self` → cast to `lkl_thread_t` | |
| `tls_alloc` | `g_private_new` (GPrivate) | |
| `tls_set/get` | `g_private_set/get` | |
| `mem_alloc` | `g_malloc` | |
| `mem_free` | `g_free` | |
| `page_alloc` | `g_aligned_alloc(size, 4096, 0)` 或平台 mmap | |
| `page_free` | `g_aligned_free` 或 munmap | |
| `time` | `g_get_monotonic_time() * 1000` (转为 ns) | GLib 返回 μs |
| `timer_alloc` | `g_timeout_source_new` + attach to GMainContext | |
| `timer_set_oneshot` | 动态创建 `GSource` with timeout | |
| `timer_free` | `g_source_destroy` + `g_source_unref` | |
| `print` | `g_print` 或直接 write(2) | |
| `panic` | `g_error("LKL panic")` / abort() | |
| `memcpy/memset/memmove` | 标准库或 GLib `g_memmove` | |
| `jmp_buf_set/longjmp` | 使用 ucontext 或直接 setjmp/longjmp | 平台相关 |
| `mmap/munmap` | 平台原生 (不走 GLib) | |

---

## 5. 构建系统设计 (Meson)

### 5.1 顶层 `meson.build`

```meson
project('anyfs-reader', 'c',
    version: '0.1.0',
    default_options: [
        'c_std=c11',
        'warning_level=2',
        'buildtype=debugoptimized',
    ]
)

# 依赖
glib_dep = dependency('glib-2.0', version: '>= 2.56')

# LKL (预编译的 liblkl.a 或 vmlinux.o)
lkl_inc = include_directories('<LKL_ROOT>/include')
lkl_lib = declare_dependency(
    include_directories: lkl_inc,
    # link_with 或 link_args 取决于 LKL 的编译产物
)

# QEMU Block Layer (需要从 QEMU 构建中提取静态库)
qemu_inc = include_directories('<QEMU_SRC>/include')
qemu_block_dep = declare_dependency(
    include_directories: qemu_inc,
    # link_args: [libblock.a, libqemuutil.a, ...]
)

subdir('src/core')
subdir('src/cli')
subdir('tests')
```

### 5.2 依赖编译策略

| 依赖 | 编译方式 | 产物 |
|---|---|---|
| GLib | 系统 pkg-config | libglib-2.0.so |
| LKL | `make -C tools/lkl` 在 ~/linux | liblkl.a (含 vmlinux) |
| QEMU Block | 自定义 meson 配置编译 ~/qemu | libblock.a + libqemuutil.a |

QEMU 编译命令 (仅编译 block layer):
```bash
cd ~/qemu
mkdir build-block && cd build-block
../configure --disable-system --disable-user \
    --enable-tools --disable-guest-agent \
    --disable-docs --disable-gtk --disable-sdl \
    --target-list=""
ninja qemu-img  # 验证 block layer 可编译
```

---

## 6. Phase 实施详细步骤

### Phase 1: GLib 环境劫持 (lkl_env.c)
1. 创建 `src/core/lkl_env.c`，实现完整的 `struct lkl_host_operations`
2. 创建测试 `tests/test_lkl_boot.c`
3. 编译 LKL (`make -C tools/lkl`)
4. 链接 liblkl.a + lkl_env.c + libglib-2.0
5. 验证: `lkl_init(&ops)` + `lkl_start_kernel("mem=64M")` 成功引导

### Phase 2: 同步 RAW 挂载
1. 创建 `src/core/raw_blk_backend.c`，用 `pread`/`pwrite` 实现 `lkl_dev_blk_ops`
2. 创建测试磁盘: `dd if=/dev/zero of=test.img bs=1M count=64 && mkfs.ext4 test.img`
3. `lkl_disk_add()` → `lkl_mount_dev()` → `lkl_sys_open/read`
4. 打印文件内容验证

### Phase 3: 异步改造
1. 修改 LKL 的 `virtio_blk.c` 支持 `STATUS_PENDING`
2. 创建 `src/core/qemu_blk_backend.c`
3. 初始化 QEMU: `bdrv_init()` + `blk_new_open()` 打开 qcow2
4. 用 `aio_get_g_source()` 挂载到 GMainContext
5. 用 CLI 同步模式: `g_main_context_iteration()` 驱动 I/O
6. 验证 qcow2 内的 ext4 可读

### Phase 4: CLI Shell
1. 创建 `src/cli/main.c`
2. 实现 readline 集成
3. 命令: `open <image>`, `mount [fs_type]`, `ls <path>`, `cat <file>`, `exit`
4. 每个命令通过 `anyfs_api.h` 接口调用

---

## 7. 关键风险与缓解

| 风险 | 影响 | 缓解措施 |
|---|---|---|
| LKL virtio_blk 同步假设 | Phase 3 阻塞 | 需 patch LKL，添加 PENDING 状态 |
| QEMU block layer 依赖过重 | 链接困难 | 从 qemu-img 的依赖列表精简，只取 libblock |
| Windows 上的 GLib timer 精度 | 影响 LKL timer | 可用 Win32 CreateTimerQueueTimer |
| LKL 内部 `unsigned long` 在 Win32 | 数据结构错乱 | 严格的 DLL 隔离，编译 LKL 时强制 LP64 模型 |
| virtio_req_complete 跨线程调用 | 竞态条件 | QEMU 回调在 GMainLoop 线程，需确保线程安全 |

---

## 8. 跨线程安全分析

```
Thread A: GMainLoop (主线程)
  - 运行 g_main_context_iteration()
  - QEMU AIO 完成回调在此线程触发
  - 调用 virtio_req_complete() → lkl_trigger_irq()

Thread B/C/...: LKL 内核线程 (由 thread_create 创建)
  - 运行 VFS / 文件系统代码
  - 发起 blk_enqueue → ops->request()
  - sem_down() 等待完成

关键同步点:
  1. virtio_req_complete() 中的 virtio_deliver_irq() 是线程安全的
     (通过 __sync_synchronize + lkl_trigger_irq)
  2. GLib 的 g_main_context_iteration() 是线程安全的 (单线程使用模式)
  3. blk_aio_preadv() 从非 AioContext 线程调用需要:
     - 使用 aio_bh_schedule_oneshot() 将请求投递到 AioContext 线程
     - 或使用 blk_aio_preadv 本身支持跨线程 (需验证)
```

**解决方案**: 在 `qemu_blk_backend.c` 的 `request()` 中，如果当前线程不是
GMainLoop 线程，则使用 `aio_bh_schedule_oneshot(aio_ctx, submit_io_fn, req_data)`
将 I/O 提交委托给主循环线程。

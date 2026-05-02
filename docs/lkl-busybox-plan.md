# lkl-busybox 原型计划

## 1. 目标

基于 BusyBox 的 applet 框架，将文件相关的 applet 改为调用 LKL syscall，从而实现「给一个 disk image，直接用标准 Unix 命令操作里面的文件系统」。

## 2. 最终方案: Fork BusyBox + ash NOFORK 模式

### 核心思路

Fork BusyBox，将所有文件 I/O 相关 applet 标记为 **NOFORK**，并在 libbb 的 I/O wrapper 中将 POSIX syscall 转发到 LKL。使用 BusyBox 自带的 `ash` shell 作为交互界面。

```bash
lkl-busybox ash --image disk.qcow2 --fs ext4
$ ls /etc                    # NOFORK: ash 进程内直接调 ls_main()
$ cat /etc/passwd            # 同上
$ ls /etc | grep conf        # 管道全在进程内
$ cp /etc/passwd ./output/   # guest→host 下载
```

### 为什么选 ash + NOFORK

- **NOFORK applet** 在 ash shell 进程内直接调用 `applet_main()`，不 fork
- LKL 内核只 init 一次（ash 启动时），整个 shell 生命周期共享
- 免费获得 ash 的管道、重定向、脚本、变量、通配符等
- 启动延迟 ~200ms 只发生一次

### 架构

```
lkl-busybox (multicall binary)
├─ main()                      LKL init + BusyBox dispatch
├─ shell/ash.c                 BusyBox ash (原始，几乎不改)
├─ libbb/
│   ├─ lkl_io.c              NEW: LKL I/O wrapper 层 (~300 行)
│   ├─ xfuncs.c              修改: xopen → lbb_open
│   ├─ read.c                修改: safe_read → lbb_read
│   └─ wfopen.c              修改: fopen_or_warn → lbb_fopen
├─ coreutils/                  原始 BusyBox applets (几乎不改)
│   ├─ ls.c, cat.c, cp.c, stat.c, find.c, ...
│   └─ (全部标记 NOFORK)
├─ lkl_applets/                NEW: 额外 applets
│   ├─ download.c            guest→host (保留 mode/owner/mtime)
│   └─ upload.c              host→guest (rw mount 时)
└─ anyfs.h + backends          kernel + disk init
```

## 3. Wrapper 层设计

### 路径转发 (path → mount_point + path)

```c
// libbb/lkl_io.c
static char g_mount_point[32];  // 设置一次

static void lkl_fullpath(const char *path, char *out, size_t outlen) {
    snprintf(out, outlen, "%s%s", g_mount_point, path);
}

int lbb_open(const char *path, int flags, mode_t mode) {
    char full[4096];
    lkl_fullpath(path, full, sizeof(full));
    int fd = lkl_sys_open(full, flags, mode);
    return (fd >= 0) ? fd + LKL_FD_OFFSET : -1;
}
```

### fd 隔离

LKL fd 和 host fd 独立编号（都从 0 开始），需要偏移隔离：

```c
#define LKL_FD_OFFSET 10000

ssize_t lbb_read(int fd, void *buf, size_t count) {
    if (fd >= LKL_FD_OFFSET)
        return lkl_sys_read(fd - LKL_FD_OFFSET, buf, count);
    return read(fd, buf, count);  // host fd (stdout, stderr, etc.)
}

ssize_t lbb_write(int fd, const void *buf, size_t count) {
    if (fd >= LKL_FD_OFFSET)
        return lkl_sys_write(fd - LKL_FD_OFFSET, buf, count);
    return write(fd, buf, count);  // host fd
}
```

### struct stat 转换

```c
static void lkl_stat_to_posix(const struct lkl_stat *lk, struct stat *st) {
    st->st_mode  = lk->st_mode;
    st->st_ino   = lk->st_ino;
    st->st_nlink = lk->st_nlink;
    st->st_uid   = lk->st_uid;
    st->st_gid   = lk->st_gid;
    st->st_size  = lk->st_size;
    st->st_atime = lk->st_atime;
    st->st_mtime = lk->st_mtime;
    st->st_ctime = lk->st_ctime;
}
```

### 需要 wrap 的函数 (22 个)

| 类型 | 函数 |
|------|------|
| 文件打开 | `lbb_open`, `lbb_creat` |
| 文件 I/O | `lbb_read`, `lbb_write`, `lbb_close`, `lbb_lseek`, `lbb_ftruncate` |
| 元数据 | `lbb_stat`, `lbb_lstat`, `lbb_fstat` |
| 权限 | `lbb_chmod`, `lbb_chown`, `lbb_lchown` |
| 时间 | `lbb_utimensat` |
| 目录 | `lbb_opendir`, `lbb_readdir`, `lbb_closedir`, `lbb_mkdir`, `lbb_rmdir` |
| 链接 | `lbb_link`, `lbb_symlink`, `lbb_readlink`, `lbb_unlink`, `lbb_rename` |
| FS | `lbb_statfs` |

## 4. 适用的 Applet 清单

### 只读 applets (~15 个)
ls, cat, stat, find, du, head, tail, od, cksum, sum, readlink, realpath, basename, dirname, grep

### 读写 applets (~15 个)
cp, mv, rm, mkdir, rmdir, chmod, chown, chgrp, ln, link, unlink, touch, truncate, install, dd

### 额外 applets (新写)
download (guest→host with attrs), upload (host→guest)

## 5. 启动流程

```c
// main.c (lkl-busybox 的 main)
int main(int argc, char **argv) {
    // 检查是否有 --image 参数或 LKL_IMAGE 环境变量
    const char *image = getenv("LKL_IMAGE");
    const char *fstype = getenv("LKL_FSTYPE");
    // ... 或从 argv 解析

    // LKL init (一次性)
    anyfs_kernel_init(NULL);
    int disk_id = anyfs_disk_add(image, ANYFS_DISK_READONLY);
    lkl_mount_dev(disk_id, 0, fstype, LKL_MS_RDONLY, NULL,
                  g_mount_point, sizeof(g_mount_point));

    // 设置 wrapper 层的 mount point
    lbb_set_mount_point(g_mount_point);

    // 正常 BusyBox dispatch (ash 或单命令)
    return busybox_main(argc, argv);
}
```

## 6. NOFORK 安全性

BusyBox 对 NOFORK 要求：
- 不 leak memory
- 不 leak fd
- 不阻塞太久

在 LKL 场景中可以放松这些约束：
- Memory leak：shell 退出时一起释放（使用场景是交互+退出）
- fd leak：LKL fd 有限（max ~1024），实际上 applet 都会 close
- 阻塞：对大文件操作（cat HUGEFILE）可以接受

## 7. 未解决问题

1. **`/proc` 和 `/sys`**: BusyBox 的 `df` 读 `/proc/mounts`，但 LKL 内核没有 procfs 挂载
   - 方案: 为 df 使用 `statfs()` 而不是读 mtab

2. **管道 + fork**: `ls | grep foo` 在 ash 中仍需 fork（用于管道两端）
   - NOFORK 只在简单命令时生效，管道会 fork
   - fork 后子进程继承 LKL 状态？**不行，LKL 不是 fork-safe**
   - 方案: 管道在进程内模拟（内存 pipe），或接受限制

3. **信号处理**: ash 的 ^C 需要中断 NOFORK applet
   - LKL syscall 不响应 POSIX 信号
   - 可能需要 `longjmp` 或超时机制

4. **Windows 支持**: 后续，LKL 有 nt-host 后端

## 8. 替代方案对比

| 方案 | 优点 | 缺点 | 工作量 |
|------|------|------|--------|
| A. Fork BusyBox + NOFORK | 免费获得 30+ applet + ash | wrapper 层 + fork 问题 | ~500 行 |
| B. FUSE mount + stock tools | 零代码改动 | 需要 FUSE 模块 | ~0 行 |
| C. 自写 multicall (当前 anyfs-shell) | 干净、无 fork 问题 | 每个 applet 手写 | ~3000 行 |
| D. Daemon + socket | 延迟最低 | IPC 复杂 | ~1000 行 |

## 9. 推荐路线

1. **当前**: 继续完善 anyfs-shell（方案 C），它就是 lkl-busybox 的原型
2. **短期**: 如果需要管道/脚本能力，尝试 fork BusyBox（方案 A）
3. **长期**: 如果 FUSE 可用，方案 B 是终极解决方案（零侵入）

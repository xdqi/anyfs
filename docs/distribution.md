# 分发方案

## 目标平台

| 平台 | 架构 | LKL 工具链 |
|------|------|-----------|
| Linux | amd64 | 原生 gcc |
| Windows | i386 | `i686-w64-mingw32-` 交叉编译，需 `~/binutils-gdb` (v2.25.1) 的补丁版 binutils |

## 二进制命名

所有可执行文件统一 `anyfs-*` 前缀：

| 原名 | 分发名 |
|------|--------|
| anyfs-gui | anyfs-gui |
| anyfs-shell | anyfs-shell |
| lkl_ksmbd | anyfs-ksmbd |
| lkl_nfsd | anyfs-nfsd |
| busybox | anyfs-busybox (仅 Linux) |

## 目录布局

### Linux (`anyfs-reader-linux-amd64.tar.gz`)

```
anyfs-reader/
├── bin/
│   ├── anyfs-gui          (rpath=$ORIGIN/../lib)
│   ├── anyfs-shell
│   ├── anyfs-ksmbd
│   ├── anyfs-nfsd
│   └── anyfs-busybox      (动态链接)
└── lib/
    ├── liblkl.so           (~20MB, 35 文件系统 + nfsd + ksmbd)
    ├── libanyfs-qemublk.so    (~10MB, QEMU block 层)
    ├── libslirp.so.0       (liblkl 网络依赖)
    ├── liburing.so.2
    └── libaio.so.1t64
```

系统依赖（不打包，要求目标系统安装）：
- libglib-2.0, libz, libzstd, libbz2, libm, libc
- GTK3 栈（仅 anyfs-gui 需要）
- libreadline（仅 anyfs-shell 需要）

### Windows (`anyfs-reader-win32.zip`)

```
anyfs-reader/
├── anyfs-gui.exe
├── anyfs-shell.exe
├── anyfs-ksmbd.exe
├── anyfs-nfsd.exe
├── lkl.dll
├── glib-2.0-0.dll
├── libslirp-0.dll
├── anyfs-qemublk.dll         (QEMU block 层合并)
└── (mingw 运行时 dll)
```

注意: Windows 版不包含 busybox。

## 构建配置

### 后端选择

分发版本**仅包含 QEMU backend**（支持 raw/qcow2/vmdk/vdi 等所有格式）。
GIO backend 和 raw backend 不包含在分发中。

### LKL 内核配置

启用所有能编译通过的文件系统：

```bash
make ARCH=lkl menuconfig
```

必须启用：
- ext2, ext3, ext4 (含 journal)
- xfs
- btrfs
- fat, vfat, exfat
- ntfs3
- f2fs
- squashfs
- iso9660, udf
- hfsplus
- minix, reiserfs, jfs
- nilfs2, erofs
- proc, sysfs (LKL 内部需要)

服务器相关：
- nfsd (v4)
- ksmbd

优化：
- CONFIG_DEBUG_INFO_NONE=y (缩小体积, liblkl.so 约 20MB)

### QEMU 动态库

将 QEMU 的 `libblock.a`, `libqemuutil.a`, `libio.a`, `libqom.a`, `libcrypto.a`, `libauthoriz.a`
以及 `libevent-loop-base.a` 合并为单一共享库 `libanyfs-qemublk.so` / `anyfs-qemublk.dll`。

前置条件：QEMU 需用 `-fPIC` + `b_pie=false` 构建，且需打补丁 `util/fdmon-poll.c`
移除 `static __thread` 中的 `static`（GCC 对 static __thread 始终使用 local-exec TLS，
产生 R_X86_64_TPOFF32 重定位，无法放入共享库）。

```bash
# 构建 QEMU（PIC 模式）
cd ~/qemu
mkdir build-anyfs-shared && cd build-anyfs-shared
../configure --disable-system --disable-user --enable-tools \
    --disable-docs --disable-gtk --disable-sdl --disable-spice \
    --disable-vnc --disable-curses --disable-opengl \
    --extra-cflags="-fPIC"
meson configure -Db_pie=false
ninja

# 合并为共享库（使用 --start-group 解决循环依赖）
# 见 scripts/build_qemu_shared.sh
gcc -shared -o libanyfs-qemublk.so \
    -Wl,--whole-archive libblock.a \
    -Wl,--no-whole-archive \
    -Wl,--start-group libqemuutil.a libio.a libqom.a libcrypto.a libauthz.a libevent-loop-base.a -Wl,--end-group \
    -lglib-2.0 -lz -lzstd -luring -laio -lbz2
```

注意：只对 `libblock.a` 使用 `--whole-archive`，其他用 `--start-group`。
若对 `libqemuutil.a` 也用 whole-archive 会拉入 QMP 命令注册导致需要完整系统模拟器符号。

## 构建步骤

### Phase 1: LKL 内核重新配置

```bash
cd ~/linux
make ARCH=lkl menuconfig    # 启用所有文件系统
make -C tools/lkl clean
make -C tools/lkl            # 生成 liblkl.so + liblkl.a
```

### Phase 2: Linux amd64

```bash
cd ~/anyfs-reader
meson setup builddir-dist \
    -Dlkl_root=$HOME/linux/tools/lkl \
    -Dlkl_shared=true \
    -Denable_qemu=true \
    -Dqemu_root=$HOME/qemu \
    -Dqemu_build=$HOME/qemu/build-anyfs-shared \
    -Dqemu_shared=true \
    -Denable_ksmbd=true \
    -Dksmbd_tools_root=$HOME/ksmbd-tools \
    --prefix=$HOME/anyfs-reader/dist

meson compile -C builddir-dist

# 打包（自动收集依赖、设置 RUNPATH、创建 tar.gz）
./scripts/package_linux.sh builddir-dist
```

### Phase 3: Windows i386 交叉编译

```bash
# 1. 构建补丁版 binutils
cd ~/binutils-gdb
./configure --target=i686-w64-mingw32 --prefix=$HOME/mingw-patched
make -j$(nproc) && make install
export PATH=$HOME/mingw-patched/bin:$PATH

# 2. 交叉编译 LKL
cd ~/linux
make CROSS_COMPILE=i686-w64-mingw32- -C tools/lkl

# 3. 交叉编译 GLib (meson cross file)
# 4. 交叉编译 QEMU block 层
# 5. 交叉编译 anyfs-reader (meson cross file)
# 6. 收集 DLL，打包
zip -r anyfs-reader-win32.zip anyfs-reader/
```

## 依赖关系

### Linux 运行时依赖

| 库 | 来源 | 说明 |
|----|------|------|
| liblkl.so | 自建 | LKL 内核 |
| libanyfs-qemublk.so | 自建 | QEMU block 层 |
| libglib-2.0.so | 系统 | GLib (GTK3 也依赖) |
| libslirp.so | 系统 | 用户态网络 |
| libgtk-3.so | 系统 | GUI (anyfs-gui) |
| libreadline.so | 系统 | Shell (anyfs-shell) |

### Windows 运行时依赖

| 库 | 来源 |
|----|------|
| lkl.dll | 自建 |
| anyfs-qemublk.dll | 自建 |
| glib-2.0-0.dll | 交叉编译 |
| libslirp-0.dll | 交叉编译 |
| mingw 运行时 | mingw-w64 |

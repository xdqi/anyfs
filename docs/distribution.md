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
    ├── liblkl.so
    ├── libglib-2.0.so.0
    ├── libslirp.so.*
    └── libanyfs-qemu.so   (QEMU block 层合并为单一动态库)
```

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
├── anyfs-qemu.dll         (QEMU block 层合并)
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
- 关闭 CONFIG_DEBUG_INFO (缩小体积)

### QEMU 动态库

将 QEMU 的 `libblock.a`, `libqemuutil.a`, `libio.a`, `libqom.a`, `libcrypto.a`, `libauthoriz.a`
以及 `libevent-loop-base.a` 合并为单一共享库 `libanyfs-qemu.so` / `anyfs-qemu.dll`。

```bash
# 示例: 合并静态库为共享库
gcc -shared -o libanyfs-qemu.so \
    -Wl,--whole-archive libblock.a libqemuutil.a libio.a libqom.a libcrypto.a libauthz.a libevent-loop-base.a \
    -Wl,--no-whole-archive \
    -lglib-2.0 -lz -lzstd -laio -lbz2 -lpixman-1
```

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
# 配置 meson (仅 QEMU backend)
meson setup builddir-dist \
    -Dlkl_root=$HOME/linux/tools/lkl \
    -Denable_qemu=true \
    -Dqemu_root=$HOME/qemu \
    -Dqemu_build=$HOME/qemu/build-anyfs2 \
    -Denable_ksmbd=true \
    -Dksmbd_tools_root=$HOME/ksmbd-tools \
    --prefix=/usr/local

meson compile -C builddir-dist

# 打包
mkdir -p dist/anyfs-reader/{bin,lib}
# 复制二进制到 bin/，设置 rpath
# 复制 .so 到 lib/
tar czf anyfs-reader-linux-amd64.tar.gz -C dist anyfs-reader
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
| libanyfs-qemu.so | 自建 | QEMU block 层 |
| libglib-2.0.so | 系统 | GLib (GTK3 也依赖) |
| libslirp.so | 系统 | 用户态网络 |
| libgtk-3.so | 系统 | GUI (anyfs-gui) |
| libreadline.so | 系统 | Shell (anyfs-shell) |

### Windows 运行时依赖

| 库 | 来源 |
|----|------|
| lkl.dll | 自建 |
| anyfs-qemu.dll | 自建 |
| glib-2.0-0.dll | 交叉编译 |
| libslirp-0.dll | 交叉编译 |
| mingw 运行时 | mingw-w64 |

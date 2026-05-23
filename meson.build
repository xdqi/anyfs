project('anyfs-reader', 'c',
    version: '0.1.0',
    default_options: [
        'c_std=gnu11',
        'warning_level=2',
        'b_pie=true',
    ],
)

cc = meson.get_compiler('c')

# --- Dependencies ---

# LKL (pre-built). Either pick an out-of-tree distribution via lkl_dist
# (which maps to ./lkl-<dist>/tools/lkl/) or pass an explicit absolute lkl_root.
# lkl_root_rel is kept relative when derived from lkl_dist so meson is
# happy to pass it to include_directories(); lkl_root is the absolute form
# used everywhere else (find_library dirs, link_args, etc.).
lkl_root = get_option('lkl_root')
lkl_dist = get_option('lkl_dist')
if lkl_root == ''
    if lkl_dist == ''
        error('Set either lkl_dist or lkl_root.')
    endif
    lkl_root_rel = 'lkl-' + lkl_dist + '/tools/lkl'
    lkl_root = meson.current_source_dir() / lkl_root_rel
else
    lkl_root_rel = lkl_root
endif
lkl_shared = get_option('lkl_shared')
# Generated headers (lkl_autoconf.h, lkl/{asm,asm-generic,linux}/) live in
# the dist tree; the public source headers (lkl.h, lkl_host.h, mingw32/)
# live in the kernel source tree at $lkl_src/tools/lkl/include/.
lkl_src = get_option('lkl_src')
lkl_inc = include_directories(lkl_root_rel / 'include')
lkl_src_inc = include_directories(lkl_src / 'tools' / 'lkl' / 'include')
if host_machine.system() == 'windows'
    lkl_mingw_inc = include_directories(lkl_src / 'tools' / 'lkl' / 'include' / 'mingw32')
else
    lkl_mingw_inc = []
endif
if lkl_shared
    if host_machine.system() == 'windows'
        # mingw build emits liblkl.dll into tools/lkl/lib/ (alongside the .so
        # on Linux). Link against it directly — mingw-ld synthesises the
        # import library on the fly. Must be copied next to the exes for
        # the runtime loader to find it; build_anyfs.sh handles staging.
        lkl_dll = lkl_root / 'lib' / 'liblkl.dll'
        lkl_lib = declare_dependency(link_args: [lkl_dll])
    else
        lkl_lib = cc.find_library('lkl',
            dirs: [lkl_root / 'lib'],
        )
    endif
else
    lkl_lib = cc.find_library('lkl',
        dirs: [lkl_root],
        static: true,
    )
endif
# LKL needs pthread, rt (Linux), ws2_32 (Windows), and slirp (Linux only)
thread_dep = dependency('threads')
if host_machine.system() == 'windows'
    ws2_dep = cc.find_library('ws2_32')
    iphlpapi_dep = cc.find_library('iphlpapi')
    rt_dep = disabler()
    slirp_for_lkl = disabler()
    lkl_extra_deps = [ws2_dep, iphlpapi_dep]
    lkl_compile_args = []
else
    rt_dep = cc.find_library('rt', required: false)
    slirp_for_lkl = dependency('slirp', required: false)
    # liblkl.a on Linux compiles in virtio_net_slirp.o, which references
    # libslirp symbols, which in turn reference glib (when libslirp is
    # linked statically). Pull glib into lkl_dep so every lkl_dep consumer
    # gets it automatically — otherwise an executable that doesn't itself
    # use slirp (e.g. anyfs-nfsd after the host_proxy migration) still
    # fails to link because virtio_net_slirp.o is in liblkl.a.
    if slirp_for_lkl.found()
        glib_for_lkl = dependency('glib-2.0', required: false)
    else
        glib_for_lkl = disabler()
    endif
    lkl_extra_deps = [rt_dep] \
        + (slirp_for_lkl.found() ? [slirp_for_lkl] : []) \
        + (glib_for_lkl.found() ? [glib_for_lkl] : [])
    lkl_compile_args = ['-DLKL_HOST_CONFIG_POSIX']
endif
# Arch headers: lkl/asm/syscalls.h. The static source headers live in
# the kernel source tree; the generated uapi/{asm/...} live in the dist.
lkl_arch_inc = include_directories(lkl_src / 'arch' / 'lkl' / 'include')
lkl_arch_gen_inc = include_directories(
    lkl_root_rel / '..' / '..' / 'arch' / 'lkl' / 'include' / 'generated' / 'uapi',
)
# On Windows, mingw32/ must come before the rest so our lkl_autoconf.h
# (without LKL_HOST_CONFIG_POSIX) takes precedence over the Linux one.
_lkl_inc_dirs = [lkl_inc, lkl_src_inc, lkl_arch_inc, lkl_arch_gen_inc]
if host_machine.system() == 'windows'
    _lkl_inc_dirs = [lkl_mingw_inc] + _lkl_inc_dirs
endif
lkl_dep = declare_dependency(
    include_directories: _lkl_inc_dirs,
    dependencies: [lkl_lib, thread_dep] + lkl_extra_deps,
    compile_args: lkl_compile_args,
)

# GLib/GIO (optional, from ~/glib static build)
glib_root = get_option('glib_root')
enable_gio = get_option('enable_gio')

if enable_gio
    glib_build = glib_root / 'build-static'
    glib_inc = include_directories(
        glib_root,
        glib_root / 'glib',
        glib_root / 'gio',
        glib_root / 'gobject',
        glib_root / 'gmodule',
        glib_build,
        glib_build / 'glib',
        glib_build / 'gio',  # for gio config headers
    )
    glib_lib = cc.find_library('glib-2.0', dirs: [glib_build / 'glib'], static: true)
    gio_lib = cc.find_library('gio-2.0', dirs: [glib_build / 'gio'], static: true)
    gobject_lib = cc.find_library('gobject-2.0', dirs: [glib_build / 'gobject'], static: true)
    gmodule_lib = cc.find_library('gmodule-2.0', dirs: [glib_build / 'gmodule'], static: true, required: false)

    # GIO's internal deps
    libffi_dep = dependency('libffi', required: false)
    libz_dep = dependency('zlib', required: false)
    libmount_dep = dependency('mount', required: false)
    resolv_lib = cc.find_library('resolv', required: false)
    selinux_dep = dependency('libselinux', required: false)
    pcre2_dep = dependency('libpcre2-8', required: false)
    sysprof_dep = dependency('sysprof-capture-4', required: false)
    if not sysprof_dep.found()
        sysprof_dep = cc.find_library('sysprof-capture-4', required: false)
    endif

    gio_dep = declare_dependency(
        include_directories: glib_inc,
        dependencies: [gio_lib, gobject_lib, glib_lib, gmodule_lib,
                       thread_dep, libffi_dep, libz_dep, libmount_dep,
                       resolv_lib, selinux_dep, pcre2_dep, sysprof_dep],
    )
    gio_args = ['-DANYFS_HAS_GIO=1']
else
    gio_dep = []
    gio_args = []
endif

# Public include directory
anyfs_inc = include_directories('include')

# --- QEMU Block Backend (optional) ---
enable_qemu = get_option('enable_qemu')
if enable_qemu
    qemu_root = get_option('qemu_root')
    qemu_build = get_option('qemu_build')
    qemu_shared = get_option('qemu_shared')

    qemu_inc = include_directories(
        qemu_root / 'include',
        qemu_build,
        qemu_build / 'qapi',
        qemu_build / 'trace',
    )
    # System headers from QEMU's bundled linux-headers. Only present in the
    # build dir when configure ran on Linux; mingw configure skips the
    # symlink. Probe with the FS module so meson doesn't error out.
    fs = import('fs')
    _qemu_sys_dirs = [qemu_root / 'linux-headers']
    if fs.is_dir(qemu_build / 'linux-headers')
        _qemu_sys_dirs += [qemu_build / 'linux-headers']
    endif
    qemu_sys_inc = include_directories(_qemu_sys_dirs, is_system: true)

    if qemu_shared
        # Dynamic linking: use pre-built libanyfs-qemublk.so / libanyfs-qemublk.dll
        anyfs_qemu_lib = cc.find_library('anyfs-qemublk', dirs: [qemu_build])
        glib_sys_dep = dependency('glib-2.0', required: true)
        if host_machine.system() == 'windows'
            qemu_dep = declare_dependency(
                include_directories: [qemu_inc, qemu_sys_inc],
                dependencies: [anyfs_qemu_lib, glib_sys_dep, thread_dep],
            )
        else
            pixman_dep = dependency('pixman-1', required: true)
            # libanyfs-qemublk.so leaves libaio / liburing symbols
            # undefined (no DT_NEEDED), so consumers must link them.
            aio_lib = cc.find_library('aio', required: true)
            uring_dep = dependency('liburing', required: false)
            qemu_dep = declare_dependency(
                include_directories: [qemu_inc, qemu_sys_inc],
                dependencies: [anyfs_qemu_lib, glib_sys_dep, pixman_dep,
                               thread_dep, rt_dep, aio_lib]
                              + (uring_dep.found() ? [uring_dep] : []),
            )
        endif
    else
        # Static linking: link individual QEMU archives
        qemu_libs = []
        foreach libname : ['qemuutil', 'io', 'qom', 'authz', 'crypto', 'event-loop-base']
            qemu_libs += cc.find_library(libname, dirs: [qemu_build], static: true)
        endforeach

        # libblock.a needs --whole-archive to pull in format driver constructors
        qemu_block_link_args = [
            '-Wl,--whole-archive',
            qemu_build / 'libblock.a',
            '-Wl,--no-whole-archive',
            '-lm',  # needed by some block drivers
            '-lbz2', # needed by dmg driver
        ]

        # External deps needed by QEMU
        zstd_dep = dependency('libzstd', required: true)
        uring_dep = dependency('liburing', required: false)
        aio_lib = cc.find_library('aio', required: true)
        pixman_dep = dependency('pixman-1', required: true)
        glib_sys_dep = dependency('glib-2.0', required: true)
        gthread_dep = dependency('gthread-2.0', required: true)
        z_dep = dependency('zlib', required: true)

        qemu_dep = declare_dependency(
            include_directories: [qemu_inc, qemu_sys_inc],
            dependencies: qemu_libs + [zstd_dep, aio_lib, pixman_dep,
                                        glib_sys_dep, gthread_dep, z_dep,
                                        thread_dep, rt_dep] +
                          (uring_dep.found() ? [uring_dep] : []),
            link_args: qemu_block_link_args,
        )
    endif
    qemu_args = ['-DANYFS_HAS_QEMU=1']
else
    qemu_dep = []
    qemu_args = []
    qemu_shared = false
endif

# --- Core library ---

anyfs_core_sources = files(
    'src/core/anyfs.c',
    'src/core/raw_blk_backend.c',
    'src/core/anyfs_disk.c',
    'src/core/anyfs_disk_dump.c',
    'src/core/anyfs_dm.c',
    'src/core/anyfs_sysfs.c',
    'src/core/kindprobe.c',
    'src/core/path_dsl.c',
    'src/core/share_spec.c',
)

# libblkid (optional, v2 enrichment of KIND_FS rows with FSTYPE/LABEL/UUID)
blkid_dep = dependency('blkid', required: false)
if blkid_dep.found()
    blkid_args = ['-DANYFS_HAS_BLKID=1']
else
    blkid_args = []
endif

if enable_gio
    anyfs_core_sources += files(
        'src/core/gio_blk_backend.c',
    )
endif

if enable_qemu
    anyfs_core_sources += files(
        'src/core/qemu_blk_backend.c',
    )
    if not qemu_shared
        anyfs_core_sources += files(
            'src/core/qemu_stubs.c',
        )
    endif
endif

# QEMU sources need gnu11 due to osdep.h macros using __COUNTER__
qemu_c_args = enable_qemu ? ['-std=gnu11'] : []

# On mingw, qemu_blk_backend.c #defines __MSYS__ to dodge a struct iovec
# collision between QEMU's osdep.h and LKL's lkl_host.h. That same #define
# makes lkl_host.h include <sys/uio.h>, which mingw doesn't ship — supply an
# empty shim alongside the core sources.
_anyfs_core_inc = [anyfs_inc, include_directories('src/core')]
if host_machine.system() == 'windows' and enable_qemu
    _anyfs_core_inc += [include_directories('include/mingw-shims')]
endif

anyfs_core = static_library('anyfs_core',
    anyfs_core_sources,
    include_directories: _anyfs_core_inc,
    dependencies: [lkl_dep, gio_dep] + (enable_qemu ? [qemu_dep] : [])
                  + (blkid_dep.found() ? [blkid_dep] : []),
    c_args: ['-DANYFS_CORE_BUILDING', '-fvisibility=hidden']
            + gio_args + qemu_args + blkid_args,
    install: true,
)

anyfs_dep = declare_dependency(
    include_directories: anyfs_inc,
    link_with: anyfs_core,
    dependencies: [lkl_dep, gio_dep] + (enable_qemu ? [qemu_dep] : []),
)

# --- host_proxy (shared between anyfs-ksmbd, anyfs-nfsd, lkl-zerobench) ---
# Pure userspace TCP <-> LKL TCP splice. No slirp, no virtio-net. Two
# threads (host syscalls + lkl_sys_*), per-conn SPSC rings.
host_proxy_lib = static_library('host_proxy',
    'src/host_proxy/host_proxy.c',
    include_directories: [include_directories('src/host_proxy')],
    dependencies: [lkl_dep, thread_dep],
)
host_proxy_dep = declare_dependency(
    include_directories: include_directories('src/host_proxy'),
    link_with: host_proxy_lib,
    dependencies: [lkl_dep, thread_dep],
)

# --- Tests ---

test_raw_mount_deps = [anyfs_dep]
if host_machine.system() == 'linux'
    test_raw_mount_deps += [dependency('glib-2.0', required: true)]
endif
test_raw_mount = executable('test_raw_mount',
    'tests/test_raw_mount.c',
    include_directories: anyfs_inc,
    dependencies: test_raw_mount_deps,
    c_args: gio_args + qemu_args,
)

bench_backends = executable('bench_backends',
    'tests/bench_backends.c',
    include_directories: anyfs_inc,
    dependencies: [anyfs_dep],
    c_args: gio_args + qemu_args,
)

if enable_qemu
    test_qemu_mount = executable('test_qemu_mount',
        'tests/test_qemu_mount.c',
        include_directories: [anyfs_inc, include_directories('src/core')],
        dependencies: [anyfs_dep],
        c_args: qemu_args,
    )
endif

# --- anyfs-lspart (standalone partition lister) ---
subdir('src/lspart')

# --- CLI Shell ---

readline_dep = dependency('readline', required: false)
if not readline_dep.found()
    readline_dep = cc.find_library('readline', required: false)
endif

if readline_dep.found()
    anyfs_shell = executable('anyfs-shell',
        'src/cli/shell.c',
        include_directories: [anyfs_inc, include_directories('src/core')],
        dependencies: [anyfs_dep, readline_dep],
        c_args: gio_args + qemu_args,
        install: true,
        install_rpath: '$ORIGIN/../lib',
    )
endif

# --- ksmbd SMB3 server (LKL + ksmbd-tools) ---
enable_ksmbd = get_option('enable_ksmbd')
if enable_ksmbd
    ksmbd_tools_root = get_option('ksmbd_tools_root')
    ksmbd_tools_inc_dirs = [
        ksmbd_tools_root / 'include',
        ksmbd_tools_root,
    ]
    if host_machine.system() == 'windows'
        ksmbd_tools_inc_dirs += ['src/ksmbd/win32-compat']
    endif
    ksmbd_tools_inc = include_directories(ksmbd_tools_inc_dirs)

    slirp_dep = dependency('slirp', required: true)
    glib_sys_dep2 = dependency('glib-2.0', required: true)

    # ksmbd-tools source files (no libnl dependency - we use ipc_lkl.c)
    ksmbd_tools_sources = files(
        ksmbd_tools_root / 'mountd/worker.c',
        ksmbd_tools_root / 'mountd/rpc.c',
        ksmbd_tools_root / 'mountd/rpc_srvsvc.c',
        ksmbd_tools_root / 'mountd/rpc_wkssvc.c',
        ksmbd_tools_root / 'mountd/rpc_samr.c',
        ksmbd_tools_root / 'mountd/rpc_lsarpc.c',
        ksmbd_tools_root / 'mountd/smbacl.c',
        ksmbd_tools_root / 'tools/management/user.c',
        ksmbd_tools_root / 'tools/management/share.c',
        ksmbd_tools_root / 'tools/management/session.c',
        ksmbd_tools_root / 'tools/management/tree_conn.c',
        ksmbd_tools_root / 'tools/config_parser.c',
    )

    # tools.c compiled separately with main renamed to avoid conflict
    ksmbd_tools_lib = static_library('ksmbd_tools_lib',
        ksmbd_tools_root / 'tools/tools.c',
        include_directories: [ksmbd_tools_inc],
        dependencies: [glib_sys_dep2],
        c_args: [
            '-DSYSCONFDIR="/etc"',
            '-DRUNSTATEDIR="/run"',
            '-DHAVE_CONFIG_H',
            '-I' + meson.current_build_dir(),
            '-Wno-pointer-sign',
            '-Dmain=ksmbd_tools_main_unused',
        ],
    )

    # config.h for ksmbd-tools (no KRB5)
    ksmbd_config = configure_file(
        output: 'config.h',
        configuration: configuration_data(),
    )
    ksmbd_config_inc = include_directories('.')

    lkl_ksmbd = executable('anyfs-ksmbd',
        'src/ksmbd/lkl_ksmbd.c',
        'src/ksmbd/ipc_lkl.c',
        'src/ksmbd/stubs_lkl.c',
        ksmbd_tools_sources,
        include_directories: [anyfs_inc, ksmbd_tools_inc],
        dependencies: [lkl_dep, slirp_dep, glib_sys_dep2, thread_dep, host_proxy_dep],
        link_with: [ksmbd_tools_lib, anyfs_core],
        c_args: [
            '-DSYSCONFDIR="/etc"',
            '-DRUNSTATEDIR="/run"',
            '-DHAVE_CONFIG_H',
            '-I' + meson.current_build_dir(),
            '-Wno-pointer-sign',
        ],
        install: true,
        install_rpath: '$ORIGIN/../lib',
    )
endif

# --- NFSv4 server (LKL nfsd, no external tools needed) ---
if enable_ksmbd  # reuses same LKL build with NFSD config
    # glib is needed because libslirp depends on it (g_log/g_malloc/etc).
    # When linking libslirp statically those symbols don't come in transitively.
    lkl_nfsd = executable('anyfs-nfsd',
        'src/nfsd/lkl_nfsd.c',
        include_directories: [anyfs_inc],
        dependencies: [lkl_dep, slirp_dep, glib_sys_dep2, thread_dep],
        link_with: [anyfs_core],
        install: true,
        install_rpath: '$ORIGIN/../lib',
    )
endif

# --- FUSE Frontend (anyfs-fuse) ---
enable_fuse = get_option('enable_fuse')
if enable_fuse
    fuse3_dep = dependency('fuse3', required: true)
    fuse_flags = []
    fuse_deps = [lkl_dep, fuse3_dep]
    if enable_qemu
        fuse_deps += [qemu_dep]
        fuse_flags += ['-DANYFS_HAS_QEMU=1']
    endif

    anyfs_fuse = executable('anyfs-fuse',
        'src/fuse/anyfs_fuse.c',
        include_directories: [anyfs_inc],
        dependencies: fuse_deps,
        link_with: [anyfs_core],
        c_args: fuse_flags + qemu_args + gio_args,
        install: true,
        install_rpath: '$ORIGIN/../lib',
    )
endif

# --- WinFSP FUSE Frontend (anyfs-winfsp.exe) ---
enable_winfsp = get_option('enable_winfsp')
if enable_winfsp
    winfsp_root = get_option('winfsp_root')
    if winfsp_root == ''
        error('winfsp_root must be set when enable_winfsp=true')
    endif

    winfsp_inc = include_directories(
        winfsp_root / 'inc',
    )

    _winfsp_lib = host_machine.cpu_family() == 'x86_64' ? 'winfsp-x64' : 'winfsp-x86'
    winfsp_lib = cc.find_library(_winfsp_lib,
        dirs: [winfsp_root / 'opt' / 'fsext' / 'lib'],
        required: true,
    )

    anyfs_winfsp = executable('anyfs-winfsp',
        'src/fuse/anyfs_fuse.c',
        include_directories: [anyfs_inc, winfsp_inc],
        dependencies: [lkl_dep, winfsp_lib, thread_dep, ws2_dep, iphlpapi_dep],
        link_with: [anyfs_core],
        c_args: ['-DFUSE_USE_VERSION=35'],
        win_subsystem: 'console',
        install: true,
        install_rpath: '',
    )
endif

# --- GTK3 GUI File Manager ---
gtk3_dep = dependency('gtk+-3.0', required: false)
if gtk3_dep.found()
    # GUI links anyfs_core directly (not via anyfs_dep) to avoid
    # static glib from enable_gio conflicting with GTK3's dynamic glib.
    gui_deps = [lkl_dep, gtk3_dep]
    gui_args = []
    if enable_qemu
        gui_deps += [qemu_dep]
        gui_args += ['-DANYFS_HAS_QEMU=1']
    endif
    gui_link_args = []
    gui_subsystem = 'console'
    if host_machine.system() == 'windows'
        gui_subsystem = 'windows'
    endif
    anyfs_gui = executable('anyfs-gui',
        'src/gui/anyfs_gui.c',
        include_directories: [anyfs_inc],
        dependencies: gui_deps,
        link_with: [anyfs_core],
        c_args: gui_args,
        link_args: gui_link_args,
        win_subsystem: gui_subsystem,
        install: true,
        install_rpath: '$ORIGIN/../lib',
    )
endif

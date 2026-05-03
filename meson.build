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

# LKL (pre-built)
lkl_root = get_option('lkl_root')
lkl_inc = include_directories(lkl_root / 'include')
lkl_lib = cc.find_library('lkl',
    dirs: [lkl_root],
    static: true,
)
# LKL needs pthread, rt, and slirp (compiled into liblkl.a)
thread_dep = dependency('threads')
rt_dep = cc.find_library('rt', required: false)
slirp_for_lkl = dependency('slirp', required: false)
lkl_dep = declare_dependency(
    include_directories: lkl_inc,
    dependencies: [lkl_lib, thread_dep, rt_dep] + (slirp_for_lkl.found() ? [slirp_for_lkl] : []),
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

    qemu_inc = include_directories(
        qemu_root / 'include',
        qemu_build,
        qemu_build / 'qapi',
        qemu_build / 'trace',
    )
    # System headers from QEMU's bundled linux-headers
    qemu_sys_inc = include_directories(
        qemu_root / 'linux-headers',
        qemu_build / 'linux-headers',
        is_system: true,
    )

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
    qemu_args = ['-DANYFS_HAS_QEMU=1']
else
    qemu_dep = []
    qemu_args = []
endif

# --- Core library ---

anyfs_core_sources = files(
    'src/core/anyfs.c',
    'src/core/raw_blk_backend.c',
)

if enable_gio
    anyfs_core_sources += files(
        'src/core/gio_blk_backend.c',
    )
endif

if enable_qemu
    anyfs_core_sources += files(
        'src/core/qemu_blk_backend.c',
        'src/core/qemu_stubs.c',
    )
endif

# QEMU sources need gnu11 due to osdep.h macros using __COUNTER__
qemu_c_args = enable_qemu ? ['-std=gnu11'] : []

anyfs_core = static_library('anyfs_core',
    anyfs_core_sources,
    include_directories: [anyfs_inc, include_directories('src/core')],
    dependencies: [lkl_dep, gio_dep] + (enable_qemu ? [qemu_dep] : []),
    c_args: ['-DANYFS_CORE_BUILDING', '-fvisibility=hidden'] + gio_args + qemu_args,
    install: true,
)

anyfs_dep = declare_dependency(
    include_directories: anyfs_inc,
    link_with: anyfs_core,
    dependencies: [lkl_dep, gio_dep] + (enable_qemu ? [qemu_dep] : []),
)

# --- Tests ---

test_raw_mount = executable('test_raw_mount',
    'tests/test_raw_mount.c',
    include_directories: anyfs_inc,
    dependencies: [anyfs_dep],
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
    )
endif

# --- ksmbd SMB3 server (LKL + ksmbd-tools) ---
enable_ksmbd = get_option('enable_ksmbd')
if enable_ksmbd
    ksmbd_tools_root = get_option('ksmbd_tools_root')
    ksmbd_tools_inc = include_directories(
        ksmbd_tools_root / 'include',
        ksmbd_tools_root,
    )

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

    lkl_ksmbd = executable('lkl_ksmbd',
        'src/ksmbd/lkl_ksmbd.c',
        'src/ksmbd/ipc_lkl.c',
        'src/ksmbd/stubs_lkl.c',
        ksmbd_tools_sources,
        include_directories: [anyfs_inc, ksmbd_tools_inc],
        dependencies: [lkl_dep, slirp_dep, glib_sys_dep2],
        link_with: [ksmbd_tools_lib, anyfs_core],
        c_args: [
            '-DSYSCONFDIR="/etc"',
            '-DRUNSTATEDIR="/run"',
            '-DHAVE_CONFIG_H',
            '-I' + meson.current_build_dir(),
            '-Wno-pointer-sign',
        ],
    )
endif

# --- NFSv4 server (LKL nfsd, no external tools needed) ---
if enable_ksmbd  # reuses same LKL build with NFSD config
    lkl_nfsd = executable('lkl_nfsd',
        'src/nfsd/lkl_nfsd.c',
        include_directories: [anyfs_inc],
        dependencies: [lkl_dep, slirp_dep, thread_dep],
        link_with: [anyfs_core],
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
    anyfs_gui = executable('anyfs-gui',
        'src/gui/anyfs_gui.c',
        include_directories: [anyfs_inc],
        dependencies: gui_deps,
        link_with: [anyfs_core],
        c_args: gui_args,
    )
endif

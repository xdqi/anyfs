project('anyfs-reader', 'c',
    version: '0.1.0',
    default_options: [
        'c_std=c11',
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
# LKL needs pthread and rt
thread_dep = dependency('threads')
rt_dep = cc.find_library('rt', required: false)
lkl_dep = declare_dependency(
    include_directories: lkl_inc,
    dependencies: [lkl_lib, thread_dep, rt_dep],
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

# --- Core library ---

anyfs_core_sources = files(
    'src/core/anyfs_core.c',
    'src/core/raw_blk_backend.c',
    'src/core/aio_blk_backend.c',
)

if enable_gio
    anyfs_core_sources += files(
        'src/core/gio_blk_backend.c',
        'src/core/gio_async_blk_backend.c',
    )
endif

anyfs_core = both_libraries('anyfs_core',
    anyfs_core_sources,
    include_directories: [anyfs_inc, include_directories('src/core')],
    dependencies: [lkl_dep, gio_dep],
    c_args: ['-DANYFS_CORE_BUILDING', '-fvisibility=hidden'] + gio_args,
    install: true,
)

anyfs_dep = declare_dependency(
    include_directories: anyfs_inc,
    link_with: anyfs_core.get_static_lib(),
    dependencies: [lkl_dep, gio_dep],
)

# --- Tests ---

test_raw_mount = executable('test_raw_mount',
    'tests/test_raw_mount.c',
    include_directories: anyfs_inc,
    dependencies: [anyfs_dep],
    c_args: gio_args,
)

bench_backends = executable('bench_backends',
    'tests/bench_backends.c',
    include_directories: anyfs_inc,
    dependencies: [anyfs_dep],
    c_args: gio_args,
)

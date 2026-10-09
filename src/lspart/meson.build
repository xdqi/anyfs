anyfs_lspart = executable('anyfs-lspart',
    'lspart_main.c',
    include_directories: [anyfs_inc, include_directories('../core')],
    dependencies: [anyfs_dep, anyfs_u8_cli_dep],
    c_args: gio_args + qemu_args,
    install: true,
    install_rpath: exe_install_rpath,
)

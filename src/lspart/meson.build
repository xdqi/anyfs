anyfs_lspart = executable('anyfs-lspart',
    'anyfs_lspart.c',
    include_directories: [anyfs_inc, include_directories('../core')],
    dependencies: [anyfs_dep],
    c_args: gio_args + qemu_args,
    install: true,
    install_rpath: '$ORIGIN/../lib',
)

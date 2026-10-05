{
  "variables": {
    "repo_root":      "<(module_root_dir)/../../..",
    "qemu_bld_linux": "<!(echo ${QEMU_BLD_LINUX:-${HOME}/qemu/build-anyfs-linux-amd64})",
    "qemu_src":       "<!(echo ${QEMU_SRC:-${HOME}/qemu})",
    "linux_src":      "<!(echo ${LINUX_SRC:-${HOME}/linux})",
    "linux_sysroot":  "<!(echo ${ANYFS_LINUX_SYSROOT:-${XDG_CACHE_HOME:-${HOME}/.cache}/anyfs-linux-sysroot/x86_64-linux-gnu.2.11})"
  },
  "targets": [
    {
      "target_name": "anyfs_native",
      "sources": [
        "src/binding.cc",
        "../../native/anyfs_ts.c"
      ],
      "include_dirs": [
        "<!@(node -p \"require('node-addon-api').include\")",
        "<(repo_root)/include",
        "<(repo_root)/src/core",
        "<(repo_root)/lkl-linux-amd64/tools/lkl/include",
        "<(repo_root)/lkl-linux-amd64/arch/lkl/include/generated/uapi",
        "<(linux_src)/tools/lkl/include",
        "<(linux_src)/arch/lkl/include"
      ],
      "defines": ["NAPI_DISABLE_CPP_EXCEPTIONS"],
      "cflags!":    ["-fno-exceptions"],
      "cflags_cc!": ["-fno-exceptions"],
      "cflags_c":   ["-D_FILE_OFFSET_BITS=64"],
      "conditions": [
        ["OS==\"linux\"", {
          # Built by scripts/build-linux-electron.sh with zig at glibc 2.25
          # (CC/CXX = scripts/lib/zig-c{c,++}, ANYFS_ZIG_TARGET). Every
          # dependency is a static archive from the linux sysroot, passed by
          # path so no host library can be picked up and QEMU's libcrypto.a
          # can't be confused with OpenSSL's.
          #
          # libanyfs_core.a was compiled with -DANYFS_HAS_QEMU, so it pulls in
          # the QEMU block layer. libblock.a needs --whole-archive: each
          # format driver (qcow2, vmdk, vdi, vpc, raw, ...) registers itself
          # from a block_init() constructor that nothing references, and
          # without it blk_new_open silently falls back to raw probing.
          "cflags":    ["-fvisibility=hidden"],
          "cflags_cc": ["-fvisibility-inlines-hidden"],
          "ldflags":   ["-Wl,--version-script=<(module_root_dir)/exports.map"],
          "libraries": [
            "-Wl,--start-group",
              "<(repo_root)/build-anyfs-linux-amd64/libanyfs_core.a",
              "<(repo_root)/lkl-linux-amd64/tools/lkl/liblkl.a",
              "-Wl,--whole-archive",
                "<(qemu_bld_linux)/libblock.a",
              "-Wl,--no-whole-archive",
              "<(qemu_bld_linux)/libio.a",
              "<(qemu_bld_linux)/libqom.a",
              "<(qemu_bld_linux)/libauthz.a",
              "<(qemu_bld_linux)/libcrypto.a",
              "<(qemu_bld_linux)/libevent-loop-base.a",
              "<(qemu_bld_linux)/libqemuutil.a",
              "<(linux_sysroot)/lib/libgio-2.0.a",
              "<(linux_sysroot)/lib/libgmodule-2.0.a",
              "<(linux_sysroot)/lib/libgobject-2.0.a",
              "<(linux_sysroot)/lib/libgthread-2.0.a",
              "<(linux_sysroot)/lib/libglib-2.0.a",
              "<(linux_sysroot)/lib/libpcre2-8.a",
              "<(linux_sysroot)/lib/libffi.a",
              "<(linux_sysroot)/lib/libblkid.a",
              "<(linux_sysroot)/lib/libcurl.a",
              "<(linux_sysroot)/lib/libssl.a",
              "<(linux_sysroot)/lib/libcrypto.a",
              "<(linux_sysroot)/lib/libzstd.a",
              "<(linux_sysroot)/lib/libbz2.a",
              "<(linux_sysroot)/lib/libz.a",
              "<(linux_sysroot)/lib/libaio.a",
              "<(linux_sysroot)/lib/liburing.a",
            "-Wl,--end-group",
            "-lpthread", "-lrt", "-ldl", "-lm"
          ]
        }]
      ],
      "ldflags": []
    }
  ]
}

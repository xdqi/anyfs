{
  "variables": {
    "repo_root":      "<(module_root_dir)/../../..",
    "qemu_bld_linux": "<!(echo ${QEMU_BLD_LINUX:-${HOME}/qemu/build-anyfs-linux-amd64})",
    "qemu_src":       "<!(echo ${QEMU_SRC:-${HOME}/qemu})",
    "linux_src":      "<!(echo ${LINUX_SRC:-${HOME}/linux})",
    "libslirp_src":   "<!(echo ${LIBSLIRP_SRC:-${HOME}/libslirp})"
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
      "cflags_c":   ["-D_FILE_OFFSET_BITS=64", "-DLKL_HOST_CONFIG_POSIX"],
      "conditions": [
        ["OS==\"linux\"", {
          # libanyfs_core.a (linux-amd64 build) was already compiled with
          # -DANYFS_HAS_QEMU, so it references qemu_backend_ops + libblock's
          # block-driver registrations. We just need to satisfy those
          # references at link time by pulling in the QEMU static archives
          # plus the glib/zstd/bz2/z stack that QEMU depends on.
          #
          # libblock.a needs --whole-archive — each block format driver
          # (qcow2, vmdk, vdi, vpc, raw, …) registers itself via a
          # block_init(...) constructor in its own .c. Without
          # --whole-archive, --gc-sections drops those constructors and
          # blk_new_open silently falls back to raw probing.
          "libraries": [
            "-Wl,--start-group",
              "<(repo_root)/build-anyfs-linux-amd64/libanyfs_core.a",
              "<(repo_root)/lkl-linux-amd64/tools/lkl/liblkl.a",
              "<(libslirp_src)/build-linux-static/libslirp.a",
              "-Wl,--whole-archive",
                "<(qemu_bld_linux)/libblock.a",
              "-Wl,--no-whole-archive",
              "<(qemu_bld_linux)/libio.a",
              "<(qemu_bld_linux)/libqom.a",
              "<(qemu_bld_linux)/libauthz.a",
              "<(qemu_bld_linux)/libcrypto.a",
              "<(qemu_bld_linux)/libevent-loop-base.a",
              "<(qemu_bld_linux)/libqemuutil.a",
            "-Wl,--end-group",
            "-lgio-2.0", "-lgmodule-2.0", "-lgobject-2.0",
            "-lgthread-2.0", "-lglib-2.0",
            "-lpcre2-8",
            "-lffi", "-lz", "-lzstd", "-lbz2",
            "-lblkid", "-lpthread", "-lrt", "-lresolv", "-ldl",
            "-laio", "-luring",
            "-lcurl"
          ]
        }]
      ],
      "ldflags": []
    }
  ]
}

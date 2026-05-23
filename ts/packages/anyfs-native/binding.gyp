{
  "variables": {
    "repo_root": "<(module_root_dir)/../../.."
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
        "<(repo_root)/lkl-linux-amd64/tools/lkl/include",
        "<(repo_root)/lkl-linux-amd64/arch/lkl/include/generated/uapi",
        "${LINUX_SRC}/tools/lkl/include",
        "${LINUX_SRC}/arch/lkl/include"
      ],
      "defines": ["NAPI_DISABLE_CPP_EXCEPTIONS"],
      "cflags!":    ["-fno-exceptions"],
      "cflags_cc!": ["-fno-exceptions"],
      "cflags_c":   ["-D_FILE_OFFSET_BITS=64", "-DLKL_HOST_CONFIG_POSIX"],
      "libraries": [
        "-Wl,--start-group",
        "<(repo_root)/build-anyfs-linux-amd64/libanyfs_core.a",
        "<(repo_root)/lkl-linux-amd64/tools/lkl/liblkl.a",
        "${LIBSLIRP_SRC}/build-linux-static/libslirp.a",
        "-Wl,--end-group",
        "-lglib-2.0",
        "-lblkid",
        "-lpthread",
        "-lrt"
      ],
      "ldflags": []
    }
  ]
}

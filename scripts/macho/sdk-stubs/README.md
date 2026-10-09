# macOS SDK stubs for zig cc

zig cc ships the macOS libc headers and `libSystem.tbd`, but no frameworks. Two
dependencies of the macOS build insist on a framework that the code anyfs uses does
not otherwise need:

| Framework | Wanted by | What it is used for |
|---|---|---|
| CoreFoundation | QEMU's meson (`dependency('appleframeworks', modules: 'CoreFoundation')`, required on darwin); curl | QEMU: host CD-ROM/block-device code in `block/file-posix.c`, compiled out without IOKit headers. curl: `CFRelease` |
| CoreServices | curl's configure (links it on every macOS target) | nothing |
| SystemConfiguration | curl's `lib/macos.c` | `SCDynamicStoreCopyProxies`, which macOS needs called once so that IPv4 literals get NAT64-synthesized IPv6 addresses |

`Frameworks/` holds a text stub (`.tbd`) per framework: the real install name, a
compatibility version no newer than the real one, and only the symbols the build
references. A link against the stub records the real framework as a load command, and
dyld loads the real one at run time. `SystemConfiguration.framework/Headers/` declares
the one call curl makes, with Apple's opaque CF pointer types.

Builds pass `-F<repo>/scripts/macho/sdk-stubs/Frameworks` (compile and link) and
link final images with `-Wl,-dead_strip_dylibs`, so an image that calls none of these
frameworks loads none of them. Add a symbol here only together with the code that
needs it.

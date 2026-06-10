#!/usr/bin/env python3
"""Add a prefix to non-public kernel symbols in a relocatable wasm .o,
so they stop colliding with libc at final-link time.

Why: LKL's `lkl.o` is the partial-link of every kernel TU. wasm-ld leaves all
non-static globals visible at link time — including the kernel's own
`vsnprintf` / `memcpy` / `strlen` / etc. When the final emcc link pulls in
`liblkl.a` with --whole-archive, those kernel symbols outrank musl libc's weak
copies, and any user-space caller of vsnprintf (e.g. tools/lkl/lib/utils.c's
`lkl_printf`) ends up running the kernel implementation. That implementation
treats wasm pointers < PAGE_SIZE as bad addresses and prints "(efault)".

The ELF/PE flow sidesteps this with `objcopy --prefix-symbols=_` plus
`-G_lkl_*` to namespace everything except the public ABI. llvm-objcopy
rejects those flags on wasm objects (despite advertising them in --help), so
we do the same job by hand: read the linking-section symbol table, prepend a
prefix to every defined non-LOCAL function/data/global whose name isn't part
of the public LKL ABI, and write the file back.

We only touch the symbol-table *name* field. Code-section calls and DATA
relocations refer to symbols by INDEX, so renames don't require relocation
edits. COMDAT groups also reference symbols by index. The custom `name`
section (debug info) is left alone — function/data names there are indexed
by function/data index, not linkage symbol name, so backtraces still resolve
the right code; only the *displayed* name lags the rename, which is fine.

Usage:
    wasm_prefix_kernel_symbols.py <in.o> [out.o] [--prefix=_]
"""
import sys, struct

WASM_SYMBOL_TYPE_FUNCTION = 0
WASM_SYMBOL_TYPE_DATA = 1
WASM_SYMBOL_TYPE_GLOBAL = 2
WASM_SYMBOL_TYPE_SECTION = 3
WASM_SYMBOL_TYPE_EVENT = 4
WASM_SYMBOL_TYPE_TABLE = 5

WASM_SYM_BINDING_WEAK = 0x01
WASM_SYM_BINDING_LOCAL = 0x02
WASM_SYM_VISIBILITY_HIDDEN = 0x04
WASM_SYM_UNDEFINED = 0x10
WASM_SYM_EXPORTED = 0x20
WASM_SYM_EXPLICIT_NAME = 0x40
WASM_SYM_NO_STRIP = 0x80
WASM_SYM_TLS = 0x100
WASM_SYM_ABSOLUTE = 0x200

WASM_SEGMENT_INFO = 5
WASM_INIT_FUNCS = 6
WASM_COMDAT_INFO = 7
WASM_SYMBOL_TABLE = 8


def read_uleb(buf, off):
    r, shift = 0, 0
    while True:
        b = buf[off]
        off += 1
        r |= (b & 0x7f) << shift
        if (b & 0x80) == 0:
            return r, off
        shift += 7


def write_uleb(v):
    out = bytearray()
    while True:
        b = v & 0x7f
        v >>= 7
        if v:
            out.append(b | 0x80)
        else:
            out.append(b)
            return bytes(out)


def read_string(buf, off):
    n, off = read_uleb(buf, off)
    s = bytes(buf[off:off + n]).decode('utf-8')
    return s, off + n


def parse_sections(buf):
    off = 8
    secs = []
    while off < len(buf):
        sid = buf[off]
        hdr = off
        off += 1
        sz, off = read_uleb(buf, off)
        body = off
        secs.append((sid, hdr, body, sz))
        off += sz
    return secs


def parse_linking_section(buf, body, sz):
    end = body + sz
    off = body
    name, off = read_string(buf, off)
    assert name == 'linking', f"expected linking, got {name!r}"
    version, off = read_uleb(buf, off)

    sym_table = None
    while off < end:
        subid = buf[off]
        sub_hdr = off
        off += 1
        sub_sz, off = read_uleb(buf, off)
        sub_body = off
        if subid == WASM_SYMBOL_TABLE:
            so = sub_body
            cnt, so = read_uleb(buf, so)
            symbols = []
            for _ in range(cnt):
                sym = {'entry_off': so}
                sym['type'] = buf[so]; so += 1
                sym['flags'], so = read_uleb(buf, so)
                t = sym['type']
                if t in (WASM_SYMBOL_TYPE_FUNCTION, WASM_SYMBOL_TYPE_GLOBAL,
                         WASM_SYMBOL_TYPE_EVENT, WASM_SYMBOL_TYPE_TABLE):
                    sym['index'], so = read_uleb(buf, so)
                    if (sym['flags'] & WASM_SYM_EXPLICIT_NAME) or \
                       not (sym['flags'] & WASM_SYM_UNDEFINED):
                        sym['name'], so = read_string(buf, so)
                    else:
                        sym['name'] = None
                elif t == WASM_SYMBOL_TYPE_DATA:
                    sym['name'], so = read_string(buf, so)
                    if not (sym['flags'] & WASM_SYM_UNDEFINED):
                        sym['segment'], so = read_uleb(buf, so)
                        sym['offset'], so = read_uleb(buf, so)
                        sym['size'], so = read_uleb(buf, so)
                    else:
                        sym['segment'] = None
                elif t == WASM_SYMBOL_TYPE_SECTION:
                    sym['section'], so = read_uleb(buf, so)
                    sym['name'] = None
                else:
                    raise ValueError(f'unknown symbol type {t}')
                sym['entry_end'] = so
                symbols.append(sym)
            sym_table = {
                'sub_header_off': sub_hdr,
                'sub_body_off': sub_body,
                'sub_body_size': sub_sz,
                'count': cnt,
                'symbols': symbols,
            }
        off += sub_sz
    return version, sym_table


def encode_sym(sym):
    out = bytearray()
    out.append(sym['type'])
    out += write_uleb(sym['flags'])
    t = sym['type']
    if t in (WASM_SYMBOL_TYPE_FUNCTION, WASM_SYMBOL_TYPE_GLOBAL,
             WASM_SYMBOL_TYPE_EVENT, WASM_SYMBOL_TYPE_TABLE):
        out += write_uleb(sym['index'])
        if sym['name'] is not None:
            n = sym['name'].encode('utf-8')
            out += write_uleb(len(n))
            out += n
    elif t == WASM_SYMBOL_TYPE_DATA:
        n = sym['name'].encode('utf-8')
        out += write_uleb(len(n))
        out += n
        if not (sym['flags'] & WASM_SYM_UNDEFINED):
            out += write_uleb(sym['segment'])
            out += write_uleb(sym['offset'])
            out += write_uleb(sym['size'])
    elif t == WASM_SYMBOL_TYPE_SECTION:
        out += write_uleb(sym['section'])
    return bytes(out)


# Names that DO clash with musl libc and whose kernel-internal copy must be
# pushed out of the way at final-link time. Anything not in this set is left
# alone — touching the rest of the kernel's symbol topology breaks tracing /
# initcall iteration / module init in subtle ways even though the wasm
# linking format claims index-based references should survive a rename.
LIBC_CONFLICTS = {
    # printf family — the actual reason this tool exists. The kernel's
    # vsnprintf treats wasm pointers below PAGE_SIZE as "(efault)".
    'vsnprintf', 'snprintf', 'vsprintf', 'sprintf',
    # mem*
    'memcpy', 'memset', 'memcmp', 'memmove', 'memchr',
    # str*
    'strlen', 'strnlen', 'strcmp', 'strncmp',
    'strcpy', 'strncpy', 'strcat', 'strncat',
    'strchr', 'strrchr', 'strstr', 'strsep',
    'strspn', 'strcspn', 'strpbrk',
    # misc
    'abort', 'bsearch',
    # lib/zstd/common/error_private.c — zstd's only ERR_-prefixed export
    # in the kernel. Listed by exact name (rather than as ERR_* prefix)
    # so we don't accidentally rename unrelated kernel helpers that
    # happen to start with ERR_ in the future.
    'ERR_getErrorString',
    # QEMU libqemuutil also defines these — kernel internals must yield
    # so QEMU's userspace RCU / buffer / crc32c run their own versions
    # when QEMU code calls them, while the kernel keeps its own
    # implementations under the prefixed name for internal callers.
    'synchronize_rcu', 'buffer_init', 'crc32c',
    # Sysroot libz (pulled in by QEMU/glib via -lz) defines this with the
    # exact same name as lib/zlib_inflate/inffast.c. Push the kernel copy
    # under the prefix so its internal zlib_inflate() still calls it, and
    # libz's userspace inflate_fast resolves cleanly for libz callers.
    # Only the kernel zlib that SQUASHFS_ZLIB / ZISOFS / BTRFS use needs
    # this symbol — they reach it through zlib_inflate() inside the
    # kernel, all of which gets renamed in lock-step.
    'inflate_fast',
}

# Like LIBC_CONFLICTS but matched as a string prefix. Used for symbol families
# that have too many members to enumerate individually. Each kernel-internal
# call to a matching name is resolved by INDEX inside lkl.o (the prefix tool
# only rewrites the name field), so the kernel's bundled implementation keeps
# working under the renamed export — and userspace callers resolve cleanly
# against the equivalently-named symbols in their own library.
LIBC_CONFLICT_PREFIXES = (
    # lib/zstd/ inside the kernel (used by SQUASHFS_ZSTD / EROFS_FS_ZIP_ZSTD /
    # ZSTD crypto) duplicates every ZSTD_* export of userspace libzstd.a.
    # QEMU's block/qcow2-threads.c references the userspace copy for qcow2
    # compression_type=zstd; without prefixing, wasm-ld fails on duplicate
    # symbol errors. ZSTD_ itself is ~271 funcs; HUF_/FSE_/HIST_ are
    # entropy-coding helpers that zstd bundles and also exports.
    'ZSTD_', 'HUF_', 'FSE_', 'HIST_',
)


def should_prefix(sym, prefix):
    # Only rename defined function/data/global symbols.
    if sym['type'] not in (WASM_SYMBOL_TYPE_FUNCTION,
                           WASM_SYMBOL_TYPE_DATA,
                           WASM_SYMBOL_TYPE_GLOBAL,
                           WASM_SYMBOL_TYPE_EVENT,
                           WASM_SYMBOL_TYPE_TABLE):
        return False
    if sym['flags'] & WASM_SYM_UNDEFINED:
        return False
    name = sym.get('name')
    if not name:
        return False
    if name in LIBC_CONFLICTS:
        return True
    for p in LIBC_CONFLICT_PREFIXES:
        if name.startswith(p):
            return True
    return False


def main():
    args = sys.argv[1:]
    prefix = '_'
    rest = []
    for a in args:
        if a.startswith('--prefix='):
            prefix = a[len('--prefix='):]
        else:
            rest.append(a)
    if len(rest) < 1:
        print("usage: wasm_prefix_kernel_symbols.py <in.o> [out.o] [--prefix=_]",
              file=sys.stderr)
        sys.exit(2)
    in_path = rest[0]
    out_path = rest[1] if len(rest) > 1 else in_path

    with open(in_path, 'rb') as f:
        buf = bytearray(f.read())

    secs = parse_sections(buf)
    linking_sec = None
    for sid, hdr, body, sz in secs:
        if sid == 0:
            n, off = read_uleb(buf, body)
            name = bytes(buf[off:off + n]).decode('utf-8')
            if name == 'linking':
                linking_sec = (hdr, body, sz)
                break
    if linking_sec is None:
        print("error: no linking section", file=sys.stderr)
        sys.exit(1)

    lhdr, lbody, lsz = linking_sec
    version, sym_table = parse_linking_section(buf, lbody, lsz)
    if sym_table is None:
        print("error: no symbol table in linking section", file=sys.stderr)
        sys.exit(1)

    print(f"linking version {version}, {sym_table['count']} symbols")

    renamed = 0
    kept_lkl = 0
    sample_renamed = []
    sample_kept = []
    for sym in sym_table['symbols']:
        if should_prefix(sym, prefix):
            old = sym['name']
            sym['name'] = prefix + old
            renamed += 1
            if len(sample_renamed) < 8:
                sample_renamed.append(old)
        elif (sym.get('name') or '').startswith('lkl_'):
            kept_lkl += 1
            if len(sample_kept) < 8:
                sample_kept.append(sym['name'])

    print(f"renamed {renamed} symbols with prefix '{prefix}'")
    print(f"  sample: {sample_renamed}")
    print(f"kept {kept_lkl} public lkl_* symbols")
    print(f"  sample: {sample_kept}")

    if renamed == 0:
        print("nothing to do")
        return

    # Rebuild WASM_SYMBOL_TABLE subsection payload
    new_st = bytearray()
    new_st += write_uleb(sym_table['count'])
    for sym in sym_table['symbols']:
        new_st += encode_sym(sym)

    st_id_off = sym_table['sub_header_off']
    st_size_field_off = st_id_off + 1
    st_body_off = sym_table['sub_body_off']
    st_body_end = st_body_off + sym_table['sub_body_size']
    new_st_size_uleb = write_uleb(len(new_st))

    # Rebuild linking section body
    new_linking_body = bytearray()
    new_linking_body += buf[lbody:st_size_field_off]
    new_linking_body += new_st_size_uleb
    new_linking_body += new_st
    new_linking_body += buf[st_body_end:lbody + lsz]
    new_linking_size_uleb = write_uleb(len(new_linking_body))

    # Rebuild file: [0, lhdr+1) | size | body | [end of old linking, end)
    new_buf = bytearray()
    new_buf += buf[:lhdr + 1]
    new_buf += new_linking_size_uleb
    new_buf += new_linking_body
    new_buf += buf[lbody + lsz:]

    with open(out_path, 'wb') as f:
        f.write(new_buf)
    print(f"wrote {out_path} ({len(buf)} -> {len(new_buf)} bytes)")


if __name__ == '__main__':
    main()

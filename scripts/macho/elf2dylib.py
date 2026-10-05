#!/usr/bin/env python3
"""Convert a linked ELF shared object into a Mach-O dylib.

The input must be what `ld.lld -shared -Bsymbolic -z now -z max-page-size=16384
-z separate-loadable-segments --no-undefined` makes of position-independent
code: four PT_LOAD segments (R, RX, RW with PT_GNU_RELRO, RW) on 16 KiB
boundaries, only R_*_RELATIVE dynamic relocations, and no imports. Anything
else is rejected.

This tool does not write Mach-O itself. It generates assembly that .incbin's
each segment into a 16 KiB-aligned section, turns every RELATIVE slot into
`.quad <segment label> + offset` (which ld64.lld emits as a rebase), defines
symbols with .set, and links that with ld64.lld. Each segment lands at its
ELF address + DELTA, so PC-relative code inside the image stays valid. The
result is then compared with the ELF byte for byte and discarded on any
mismatch.

Design: docs/superpowers/specs/2026-10-05-lkl-macos-elf2dylib-design.md
"""
import argparse
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile

DELTA = 0x4000  # one page for the Mach-O header and load commands
PAGE = 0x4000   # arm64 macOS page size, used for x86_64 too

PT_LOAD, PT_DYNAMIC, PT_GNU_RELRO = 1, 2, 0x6474E552
PF_X, PF_W, PF_R = 1, 2, 4
SHT_SYMTAB, SHT_DYNSYM = 2, 11
SHN_UNDEF, SHN_LORESERVE = 0, 0xFF00
STT_OBJECT, STT_FUNC = 1, 2
DT_NULL, DT_NEEDED, DT_HASH, DT_STRTAB, DT_SYMTAB = 0, 1, 4, 5, 6
DT_RELA, DT_RELASZ, DT_RELAENT, DT_STRSZ, DT_SYMENT = 7, 8, 9, 10, 11
DT_SONAME, DT_SYMBOLIC, DT_REL, DT_TEXTREL, DT_JMPREL = 14, 16, 17, 22, 23
DT_BIND_NOW, DT_FLAGS, DT_RELR = 24, 30, 36
DT_GNU_HASH, DT_VERSYM, DT_RELACOUNT = 0x6FFFFEF5, 0x6FFFFFF0, 0x6FFFFFF9
DT_FLAGS_1, DT_VERDEF, DT_VERDEFNUM = 0x6FFFFFFB, 0x6FFFFFFC, 0x6FFFFFFD
DF_TEXTREL = 0x4
# Every other tag (init arrays, packed relocations, ...) carries meaning that
# the conversion would drop, so it is rejected.
DT_ACCEPTED = {DT_NULL, DT_SONAME, DT_SYMBOLIC, DT_BIND_NOW, DT_FLAGS, DT_FLAGS_1,
               DT_RELA, DT_RELASZ, DT_RELAENT, DT_RELACOUNT, DT_SYMTAB, DT_SYMENT,
               DT_STRTAB, DT_STRSZ, DT_HASH, DT_GNU_HASH, DT_VERSYM, DT_VERDEF,
               DT_VERDEFNUM}
RELA_SIZE = 24

# --arch: (e_machine, R_*_RELATIVE type, clang triple prefix)
ARCHES = {"arm64": (183, 1027, "arm64"), "x86_64": (62, 8, "x86_64")}

# One Mach-O section per PT_LOAD, in the order R, RX, RELRO, RW.
SHAPE = [PF_R, PF_R | PF_X, PF_R | PF_W, PF_R | PF_W]
SECTIONS = [
    ("__TEXT", "__lkl_const", ""),
    ("__TEXT", "__lkl_text", ",regular,pure_instructions"),
    ("__DATA_CONST", "__lkl_relro", ""),
    ("__DATA", "__lkl_data", ""),
]
BSS = ("__DATA", "__lkl_bss")  # zero-fill tail of the last segment
INSN = re.compile(r"\s*[0-9a-f]+:")     # an objdump -d instruction line
ANNOTATION = re.compile(r"<.*>")         # "bl 0x40c0 <x18>" names a symbol
X18 = re.compile(r"\b[xw]18\b")
LIBSYSTEM = "/usr/lib/libSystem.B.dylib"


class Reject(Exception):
    """Input or output outside what this converter accepts."""


def tool(env, *names):
    if os.environ.get(env):
        return os.environ[env]
    for name in names:
        if shutil.which(name):
            return name
    raise Reject(f"none of {', '.join(names)} is on PATH (or set ${env})")


def run(argv):
    r = subprocess.run(argv, capture_output=True, text=True)
    if r.returncode:
        raise Reject(f"{os.path.basename(argv[0])} failed:\n{r.stderr.strip()}")
    return r.stdout


def run_lines(argv):
    """Yield argv's output line by line, without holding all of it in memory."""
    with tempfile.TemporaryFile() as err:
        with subprocess.Popen(argv, stdout=subprocess.PIPE, stderr=err, text=True,
                              errors="replace") as proc:
            yield from proc.stdout
        if proc.returncode:
            err.seek(0)
            raise Reject(f"{os.path.basename(argv[0])} failed:\n"
                         f"{err.read().decode(errors='replace').strip()}")


class Elf:
    def __init__(self, path):
        with open(path, "rb") as f:
            self.data = f.read()
        d = self.data
        if d[:4] != b"\x7fELF" or d[4] != 2 or d[5] != 1:
            raise Reject("not a 64-bit little-endian ELF file")
        (self.type, self.machine, _, _, phoff, shoff, _, _, phentsize, phnum,
         shentsize, shnum, _) = struct.unpack_from("<HHIQQQIHHHHHH", d, 16)
        # (p_type, p_flags, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align)
        self.phdrs = [struct.unpack_from("<IIQQQQQQ", d, phoff + i * phentsize)
                      for i in range(phnum)]
        # (sh_name, sh_type, sh_flags, sh_addr, sh_offset, sh_size, sh_link, ...)
        self.shdrs = [struct.unpack_from("<IIQQQQIIQQ", d, shoff + i * shentsize)
                      for i in range(shnum)]

    def symbols(self, sh_type):
        """(name, st_info, st_shndx, st_value) of every entry in the table."""
        out = []
        for sh in self.shdrs:
            if sh[1] != sh_type:
                continue
            strtab = self.shdrs[sh[6]][4]
            for off in range(sh[4], sh[4] + sh[5], 24):
                name, info, _, shndx, value, _ = struct.unpack_from("<IBBHQQ", self.data, off)
                end = self.data.index(b"\0", strtab + name)
                out.append((self.data[strtab + name:end].decode(), info, shndx, value))
        return out

    def dynamic(self):
        """{d_tag: [d_val, ...]} from PT_DYNAMIC."""
        tags = {}
        for p in self.phdrs:
            if p[0] == PT_DYNAMIC:
                for off in range(p[2], p[2] + p[5], 16):
                    tag, val = struct.unpack_from("<qQ", self.data, off)
                    if tag == DT_NULL:
                        break
                    tags.setdefault(tag, []).append(val)
        return tags


class Image:
    """The checked input: segments, RELATIVE slots and symbols."""

    def __init__(self, path, arch, exports, objdump):
        try:
            self.parse(path, arch, exports)
        except (OSError, struct.error, IndexError, KeyError, ValueError) as e:
            # ValueError includes UnicodeDecodeError and a string without its NUL.
            raise Reject(f"malformed ELF: {e}") from None
        if arch == "arm64":
            # Only instruction lines ("  4000:  mov ..."): the header line names
            # the file. "<sym>" annotations name symbols, not registers.
            hits = []
            for line in run_lines([objdump, "-d", "--no-show-raw-insn", path]):
                if INSN.match(line) and X18.search(ANNOTATION.sub("", line)):
                    hits.append(line.rstrip())
            if hits:
                n = len(hits)
                raise Reject(f"{n} instruction{' uses' if n == 1 else 's use'} x18/w18, "
                             "which Darwin reserves (build with -ffixed-x18), e.g.:\n"
                             + "\n".join(hits[:5]))

    def parse(self, path, arch, exports):
        machine, rel_type, _ = ARCHES[arch]
        elf = self.elf = Elf(path)
        if elf.type != 3 or elf.machine != machine:
            raise Reject(f"expected ET_DYN for {arch} (e_machine {machine}), "
                         f"got e_type {elf.type}, e_machine {elf.machine}")
        self.loads = [p for p in elf.phdrs if p[0] == PT_LOAD]
        flags = [p[1] for p in self.loads]
        if flags != SHAPE:
            raise Reject(f"PT_LOAD flags are {flags}, expected {SHAPE} (R, RX, RW, RW): "
                         "link with -z now -z separate-loadable-segments")
        for p in self.loads:
            if p[3] % PAGE or p[2] != p[3]:
                raise Reject(f"PT_LOAD at {p[3]:#x} is not on a 16 KiB boundary with "
                             "p_offset == p_vaddr: link with -z max-page-size=16384 "
                             "-z separate-loadable-segments")
            if p[2] + p[5] > len(elf.data):
                raise Reject(f"malformed ELF: PT_LOAD at {p[3]:#x} extends past the "
                             "end of the file")
        for p, q in zip(self.loads, self.loads[1:]):
            if q[3] < p[3] + p[6]:
                raise Reject("PT_LOADs overlap or are out of order")
        if self.loads[0][3] != 0:
            raise Reject("the first PT_LOAD must start at address 0")
        for p in self.loads[:2]:
            if p[6] != p[5]:
                raise Reject(f"read-only PT_LOAD at {p[3]:#x} has a zero-fill tail")
        relro = [p for p in elf.phdrs if p[0] == PT_GNU_RELRO]
        if (len(relro) != 1 or relro[0][3] != self.loads[2][3]
                or relro[0][6] != self.loads[2][6]):
            raise Reject("expected one PT_GNU_RELRO covering the third PT_LOAD")

        dyn = elf.dynamic()
        for tag, what in ((DT_NEEDED, "DT_NEEDED (it depends on a shared library)"),
                          (DT_TEXTREL, "DT_TEXTREL (relocations in read-only code)"),
                          (DT_JMPREL, "PLT relocations (it imports functions)"),
                          (DT_REL, "DT_REL relocations"),
                          (DT_RELR, "DT_RELR relocations")):
            if tag in dyn:
                raise Reject(f"input has {what}")
        for tag in sorted(dyn):
            if tag not in DT_ACCEPTED:
                raise Reject(f"input has unsupported dynamic tag {tag:#x}")
        if dyn.get(DT_FLAGS, [0])[0] & DF_TEXTREL:
            raise Reject("input has DF_TEXTREL (relocations in read-only code)")

        self.slots = {}  # r_offset -> r_addend
        if DT_RELA in dyn or DT_RELASZ in dyn:
            if DT_RELA not in dyn or DT_RELASZ not in dyn:
                raise Reject("input has only one of DT_RELA and DT_RELASZ")
            start, size = dyn[DT_RELA][0], dyn[DT_RELASZ][0]
            if dyn.get(DT_RELAENT) != [RELA_SIZE] or size % RELA_SIZE:
                raise Reject(f"DT_RELAENT is {dyn.get(DT_RELAENT)}, DT_RELASZ {size:#x}: "
                             f"expected a table of {RELA_SIZE}-byte entries")
            # The first PT_LOAD starts at file offset and address 0, so the table
            # can be read at its address only if it lies in that segment's file data.
            if start + size > self.loads[0][5]:
                raise Reject(f"RELA table {start:#x}..{start + size:#x} is outside the "
                             "first PT_LOAD's file data")
            for off in range(start, start + size, RELA_SIZE):
                where, info, addend = struct.unpack_from("<QQq", elf.data, off)
                if info != rel_type:
                    raise Reject(f"relocation at {where:#x} has r_info {info:#x}; "
                                 f"only R_*_RELATIVE ({rel_type}) is accepted")
                if not any(p[3] <= where and where + 8 <= p[3] + p[5] for p in self.loads[2:]):
                    raise Reject(f"relocation at {where:#x} is outside the writable "
                                 "segments' file data")
                self.label(addend)  # rejects addends outside every segment
                self.slots[where] = addend

        self.dynsyms = {}
        for name, _, shndx, value in elf.symbols(SHT_DYNSYM):
            if not name:
                continue
            if shndx == SHN_UNDEF:
                raise Reject(f"undefined dynamic symbol {name}: the image must import nothing")
            self.dynsyms[name] = value
        for name in exports:
            if name not in self.dynsyms:
                raise Reject(f"--export {name}: no such defined dynamic symbol")

        self.locals = []
        for name, info, shndx, value in elf.symbols(SHT_SYMTAB):
            if ((info & 0xF) in (STT_OBJECT, STT_FUNC) and SHN_UNDEF < shndx < SHN_LORESERVE
                    and name and not name.startswith("$")):
                self.label(value)
                self.locals.append((name, value))

    def label(self, addr):
        """Assembler expression for an image address, relative to a segment label."""
        for i, p in enumerate(self.loads):
            vaddr, filesz, memsz = p[3], p[5], p[6]
            if vaddr <= addr <= vaddr + memsz:
                if i == 3 and memsz > filesz and addr >= vaddr + filesz:
                    return f"Lbss + {addr - vaddr - filesz:#x}"
                return f"Lseg{i} + {addr - vaddr:#x}"
        raise Reject(f"address {addr:#x} lies outside every segment")


def incbin(lines, data, path):
    """.incbin a whole file holding data. A file per chunk: the assembler keeps
    every .incbin'd file mapped, so slices of one big file would map it once per
    chunk."""
    with open(path, "wb") as f:
        f.write(data)
    lines.append(f'\t.incbin "{path}"')


def write_asm(img, exports, path, workdir):
    lines = []
    data = memoryview(img.elf.data)
    for i, p in enumerate(img.loads):
        seg, sect, attrs = SECTIONS[i]
        vaddr, filesz, memsz = p[3], p[5], p[6]
        seg_data = data[p[2]:p[2] + filesz]
        lines += [f"\t.section {seg},{sect}{attrs}", "\t.p2align 14", f"Lseg{i}:"]
        pos = 0
        for where in sorted(w for w in img.slots if vaddr <= w < vaddr + filesz):
            off = where - vaddr
            if off < pos:
                raise Reject(f"relocations at {where - 8:#x}..{where:#x} overlap")
            if off > pos:
                incbin(lines, seg_data[pos:off], os.path.join(workdir, f"seg{i}.{pos:x}.bin"))
            lines.append(f"\t.quad {img.label(img.slots[where])}")
            pos = off + 8
        if filesz > pos:
            incbin(lines, seg_data[pos:], os.path.join(workdir, f"seg{i}.{pos:x}.bin"))
        if memsz > filesz:
            if i == 3:
                lines.append(f"\t.zerofill {BSS[0]},{BSS[1]},Lbss,{memsz - filesz},0")
            else:
                lines.append(f"\t.space {memsz - filesz}")
        gap = img.loads[i + 1][3] - (vaddr + memsz) if i < 3 else 0
        if gap:
            # Pad exactly to the next segment's address, so the next section starts
            # at its ELF address + DELTA. .p2align 14 alone does not get there when
            # segments are further apart (64 KiB on AArch64 by default), and an
            # unpadded section end also lets the next Mach-O segment start below its
            # section (ld64.lld aligns x86_64 segments to 4 KiB only), which
            # llvm-objdump's rebase decoder rejects.
            lines.append(f"\t.space {gap}")
    used = set(exports.values())
    for name, value in img.locals:
        sym, n = f"_{name}", 1
        while sym in used:
            n += 1
            sym = f"_{name}~{n}"
        used.add(sym)
        lines.append(f'\t.set "{sym}", {img.label(value)}')
    for elfname, sym in exports.items():
        lines += [f'\t.globl "{sym}"', f'\t.set "{sym}", {img.label(img.dynsyms[elfname])}']
    with open(path, "w") as f:
        f.write("\n".join(lines) + "\n")


def link(asm, out, arch, min_os, install_name, libsystem, clang, ld64):
    obj = asm[:-2] + ".o"
    run([clang, "-target", f"{ARCHES[arch][2]}-apple-macos{min_os}", "-c", asm, "-o", obj])
    run([ld64, "-arch", arch, "-platform_version", "macos", min_os, min_os, "-dylib",
         "-install_name", install_name, "-no_fixup_chains", "-adhoc_codesign",
         "-o", out, obj, libsystem])


def hexpair(t):
    return f"({t[0]:#x}, {t[1]:#x})" if t else "missing"


def check_output(img, exports, out, install_name, workdir, objdump, objcopy, nm):
    sections = {}
    for line in run([objdump, "--macho", "--section-headers", out]).splitlines():
        f = line.split()
        if len(f) >= 4 and f[0].isdigit():
            sections[f[1]] = (int(f[3], 16), int(f[2], 16))

    for i, p in enumerate(img.loads):
        seg, sect, _ = SECTIONS[i]
        vaddr, filesz = p[3], p[5]
        size = filesz if i == 3 else img.loads[i + 1][3] - vaddr  # write_asm pads to it
        if sections.get(sect) != (vaddr + DELTA, size):
            raise Reject(f"{seg},{sect} (address, size) is {hexpair(sections.get(sect))}, "
                         f"expected {hexpair((vaddr + DELTA, size))}")
        dump = os.path.join(workdir, f"out{i}.bin")
        run([objcopy, f"--dump-section={seg},{sect}={dump}", out, os.devnull])
        with open(dump, "rb") as f:
            got = f.read()
        want = bytearray(img.elf.data[p[2]:p[2] + filesz]) + bytes(size - filesz)
        for where, addend in img.slots.items():
            if vaddr <= where < vaddr + filesz:
                struct.pack_into("<Q", want, where - vaddr, addend + DELTA)
        if got != want:
            diff = next((k for k in range(min(len(got), len(want))) if got[k] != want[k]),
                        min(len(got), len(want)))
            raise Reject(f"{seg},{sect} differs from ELF segment {i} at offset {diff:#x}")

    p = img.loads[3]
    if p[6] > p[5]:
        want = (p[3] + p[5] + DELTA, p[6] - p[5])
        if sections.get(BSS[1]) != want:
            raise Reject(f"{BSS[0]},{BSS[1]} (address, size) is "
                         f"{hexpair(sections.get(BSS[1]))}, expected {hexpair(want)}")

    rebases = []
    for line in run([objdump, "--macho", "--rebase", out]).splitlines():
        f = line.split()
        if len(f) >= 4 and f[2].startswith("0x"):  # types: pointer, text abs32, ...
            if f[3:] != ["pointer"]:
                raise Reject(f"rebase of type {' '.join(f[3:])} at {f[2]}; "
                             "expected only pointer")
            rebases.append(int(f[2], 16))
    if sorted(rebases) != sorted(w + DELTA for w in img.slots):
        raise Reject(f"rebase table has {len(rebases)} entries, expected the "
                     f"{len(img.slots)} RELATIVE slots + {DELTA:#x}")

    got = {}
    for line in run([objdump, "--macho", "--exports-trie", out]).splitlines():
        f = line.split()
        if len(f) == 2 and f[0].startswith("0x"):
            got[f[1]] = int(f[0], 16)
    want = {sym: img.dynsyms[e] + DELTA for e, sym in exports.items()}
    if got != want:
        raise Reject(f"exports are {sorted(got)}, expected {sorted(want)} "
                     f"at their ELF addresses + {DELTA:#x}")

    deps = [line.split(" (")[0].strip()
            for line in run([objdump, "--macho", "--dylibs-used", out]).splitlines()[1:]]
    deps = [d for d in deps if d != install_name]
    if deps != [LIBSYSTEM]:
        raise Reject(f"dependent dylibs are {deps}, expected [{LIBSYSTEM}]")
    undefined = run([nm, "-u", out]).split()
    if undefined != ["dyld_stub_binder"]:
        raise Reject(f"undefined symbols are {undefined}, expected only dyld_stub_binder")


def same_file(a, b):
    if os.path.exists(a) and os.path.exists(b):
        return os.path.samefile(a, b)
    return os.path.realpath(a) == os.path.realpath(b)


def install(src, dst):
    """Copy src to dst atomically: dst is either complete or untouched."""
    tmp = dst + ".tmp"
    try:
        shutil.copy(src, tmp)  # copies the mode too
        os.replace(tmp, dst)
    finally:
        if os.path.lexists(tmp):
            os.unlink(tmp)


def main():
    ap = argparse.ArgumentParser(description="Convert a linked ELF shared object into a Mach-O dylib.")
    ap.add_argument("--arch", required=True, choices=sorted(ARCHES))
    ap.add_argument("--export", action="append", default=[], metavar="ELF=MACHO",
                    help="export ELF symbol ELF as Mach-O symbol MACHO, e.g. lkl_init=_lklk_init")
    ap.add_argument("--install-name", required=True)
    ap.add_argument("--min-os", default="11.0")
    ap.add_argument("--libsystem", required=True, help="libSystem.tbd to link against")
    ap.add_argument("-o", dest="output", required=True)
    ap.add_argument("input")
    a = ap.parse_args()
    try:
        if same_file(a.input, a.output):
            raise Reject(f"-o {a.output} is the input file")
        if os.path.exists(a.output):
            os.unlink(a.output)
        exports = {}
        for e in a.export:
            if "=" not in e:
                raise Reject(f"--export {e}: expected ELF=MACHO")
            elfname, sym = e.split("=", 1)
            if elfname in exports:
                raise Reject(f"--export {elfname} is given more than once")
            exports[elfname] = sym
        objdump = tool("OBJDUMP", "llvm-objdump-19", "llvm-objdump")
        objcopy = tool("OBJCOPY", "llvm-objcopy-19", "llvm-objcopy")
        nm = tool("NM", "llvm-nm-19", "llvm-nm")
        clang = tool("CLANG", "clang-19", "clang")
        ld64 = tool("LD64", "ld64.lld-19", "ld64.lld")
        img = Image(a.input, a.arch, exports, objdump)
        with tempfile.TemporaryDirectory() as tmp:
            asm = os.path.join(tmp, "image.S")
            out = os.path.join(tmp, "image.dylib")
            write_asm(img, exports, asm, tmp)
            link(asm, out, a.arch, a.min_os, a.install_name, a.libsystem, clang, ld64)
            check_output(img, exports, out, a.install_name, tmp, objdump, objcopy, nm)
            install(out, a.output)
    except Reject as e:
        print(f"elf2dylib: {a.input}: {e}", file=sys.stderr)
        return 1
    print(f"elf2dylib: {a.output}: {len(img.slots)} rebases, {len(exports)} exports, "
          f"{len(img.locals)} local symbols")
    return 0


if __name__ == "__main__":
    sys.exit(main())

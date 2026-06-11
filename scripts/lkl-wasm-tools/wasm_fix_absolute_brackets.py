#!/usr/bin/env python3
# Vendored from ~/lklftpd (canonical copy for anyfs-reader builds since 2026-06-10).
"""Convert WASM_SYM_ABSOLUTE data symbols in a relocatable .o to
segment-relative symbols, when the symbol's absolute value falls inside
(or on the boundary of) one of the file's data segments.

Why: Joel's wasm-ld emits script-defined symbols (e.g. __start_ftrace_events
inside SECTIONS{}) with WASM_SYM_ABSOLUTE and value = offset in the merged
data layout. In the final emcc link, absolute symbols' value is used as-is
for the VA, but emcc re-packs/DCE-prunes input segments — so the
absolute brackets no longer point at the corresponding segment.

Fix: detect absolute symbols whose value falls in segment N's virtual range,
rewrite them as segment-relative (segment=N, offset=value-N.start, clear
ABSOLUTE bit). The relocation now references a defined symbol bound to a
real segment, which (a) gives the symbol the correct final VA and (b) keeps
the segment alive through DCE.
"""
import sys, struct

# WASM constants
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


def read_sleb(buf, off):
    r, shift = 0, 0
    while True:
        b = buf[off]
        off += 1
        r |= (b & 0x7f) << shift
        shift += 7
        if (b & 0x80) == 0:
            if b & 0x40:
                r |= -(1 << shift)
            return r, off


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
    s = buf[off:off + n]
    return s.decode('utf-8'), off + n


def parse_sections(buf):
    """Return list of (id, header_off, body_off, body_size) for each section."""
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


def parse_data_segments(buf, body, sz):
    """Parse the DATA section. Return list of dicts {init_offset, size, body_off}.
    For active segments, init_offset is decoded from the constant expr.
    """
    off = body
    count, off = read_uleb(buf, off)
    segs = []
    for _ in range(count):
        flag, off = read_uleb(buf, off)
        if flag == 0:
            # active, memory 0, offset expr
            opc = buf[off]
            off += 1
            assert opc == 0x41, f"expected i32.const, got 0x{opc:02x}"
            init_off, off = read_sleb(buf, off)
            eopc = buf[off]
            off += 1
            assert eopc == 0x0b
            seg_sz, off = read_uleb(buf, off)
            segs.append({'init_offset': init_off, 'size': seg_sz, 'body_off': off, 'flag': flag})
            off += seg_sz
        elif flag == 1:
            # passive
            seg_sz, off = read_uleb(buf, off)
            segs.append({'init_offset': None, 'size': seg_sz, 'body_off': off, 'flag': flag})
            off += seg_sz
        elif flag == 2:
            # active with explicit memidx
            mem, off = read_uleb(buf, off)
            opc = buf[off]; off += 1
            init_off, off = read_sleb(buf, off)
            eopc = buf[off]; off += 1
            seg_sz, off = read_uleb(buf, off)
            segs.append({'init_offset': init_off, 'size': seg_sz, 'body_off': off, 'flag': flag})
            off += seg_sz
        else:
            raise ValueError(f'unknown data flag {flag}')
    return segs


def parse_linking_section(buf, body, sz, n_data_segs):
    """Parse the `linking` custom section. Return:
        version: int
        subsections: list of {id, header_off, body_off, body_size}
        seg_names: list of segment names by index (None if not present)
        sym_table: {symtab_body_off, count, symbols: [{type, flags, name, ...}], end_off}
    """
    end = body + sz
    off = body
    name, off = read_string(buf, off)
    assert name == 'linking'
    version, off = read_uleb(buf, off)

    seg_names = [None] * n_data_segs
    sym_table = None
    subsections = []
    while off < end:
        subid = buf[off]
        sub_hdr = off
        off += 1
        sub_sz, off = read_uleb(buf, off)
        sub_body = off
        subsections.append({'id': subid, 'header_off': sub_hdr, 'body_off': sub_body, 'body_size': sub_sz})
        if subid == WASM_SEGMENT_INFO:
            so = sub_body
            cnt, so = read_uleb(buf, so)
            for i in range(cnt):
                nm, so = read_string(buf, so)
                _align, so = read_uleb(buf, so)
                _flags, so = read_uleb(buf, so)
                if i < n_data_segs:
                    seg_names[i] = nm
        elif subid == WASM_SYMBOL_TABLE:
            so = sub_body
            cnt, so = read_uleb(buf, so)
            symbols = []
            for _ in range(cnt):
                sym = {}
                sym['entry_off'] = so
                sym['type'] = buf[so]
                so += 1
                sym['flags'], so = read_uleb(buf, so)
                if sym['type'] in (WASM_SYMBOL_TYPE_FUNCTION, WASM_SYMBOL_TYPE_GLOBAL,
                                   WASM_SYMBOL_TYPE_EVENT, WASM_SYMBOL_TYPE_TABLE):
                    sym['index'], so = read_uleb(buf, so)
                    if (sym['flags'] & WASM_SYM_EXPLICIT_NAME) or not (sym['flags'] & WASM_SYM_UNDEFINED):
                        sym['name'], so = read_string(buf, so)
                    else:
                        sym['name'] = None
                elif sym['type'] == WASM_SYMBOL_TYPE_DATA:
                    sym['name'], so = read_string(buf, so)
                    if not (sym['flags'] & WASM_SYM_UNDEFINED):
                        sym['segment'], so = read_uleb(buf, so)
                        sym['offset'], so = read_uleb(buf, so)
                        sym['size'], so = read_uleb(buf, so)
                    else:
                        sym['segment'] = None
                elif sym['type'] == WASM_SYMBOL_TYPE_SECTION:
                    sym['section'], so = read_uleb(buf, so)
                    sym['name'] = None
                else:
                    raise ValueError(f'unknown symbol type {sym["type"]}')
                sym['entry_end'] = so
                symbols.append(sym)
            sym_table = {
                'sub_body': sub_body,
                'sub_size': sub_sz,
                'count': cnt,
                'symbols': symbols,
                'end_off': so,
            }
        off += sub_sz
    return version, subsections, seg_names, sym_table


def encode_sym(sym):
    """Serialize a symbol entry back to bytes."""
    out = bytearray()
    out.append(sym['type'])
    out += write_uleb(sym['flags'])
    if sym['type'] in (WASM_SYMBOL_TYPE_FUNCTION, WASM_SYMBOL_TYPE_GLOBAL,
                       WASM_SYMBOL_TYPE_EVENT, WASM_SYMBOL_TYPE_TABLE):
        out += write_uleb(sym['index'])
        if sym['name'] is not None:
            n = sym['name'].encode('utf-8')
            out += write_uleb(len(n))
            out += n
    elif sym['type'] == WASM_SYMBOL_TYPE_DATA:
        n = sym['name'].encode('utf-8')
        out += write_uleb(len(n))
        out += n
        if not (sym['flags'] & WASM_SYM_UNDEFINED):
            out += write_uleb(sym['segment'])
            out += write_uleb(sym['offset'])
            out += write_uleb(sym['size'])
    elif sym['type'] == WASM_SYMBOL_TYPE_SECTION:
        out += write_uleb(sym['section'])
    return bytes(out)


def find_candidates(segs, value):
    """Return all (seg_idx, offset_within_seg) bindings that would produce this
    value in the original layout. A value can match both "end of prev seg" and
    "start of next seg" if they're adjacent — both are returned so the caller
    can disambiguate based on pairing.
    """
    out = []
    for i, s in enumerate(segs):
        if s['init_offset'] is None:
            continue
        if s['init_offset'] <= value <= s['init_offset'] + s['size']:
            out.append((i, value - s['init_offset']))
    return out


# Map __start_X / __stop_X style pairs by stripping the directional prefix.
def pair_key(name):
    """Return (key, role) where role is 'start' or 'stop' or None.
    Used to pair symbols that bracket the same range.
    """
    pairs = [
        ('__start_', '__stop_'),
        ('__start', '__stop'),  # __start___param / __stop___param
        ('__begin_', '__end_'),
    ]
    for s, e in pairs:
        if name.startswith(s):
            return name[len(s):], 'start'
        if name.startswith(e):
            return name[len(e):], 'stop'
    # __initcallN_start / __initcall_end style is harder; skip pairing
    # __setup_start / __setup_end
    if name.endswith('_start'):
        return name[:-len('_start')], 'start'
    if name.endswith('_end'):
        return name[:-len('_end')], 'stop'
    if name.endswith('_begin'):
        return name[:-len('_begin')], 'start'
    return None, None


def find_segment_for_value(segs, value):
    """Single-symbol fallback: prefer start-of-segment over end-of-previous."""
    # Prefer exact start-of-segment match (or strictly inside)
    for i, s in enumerate(segs):
        if s['init_offset'] is None:
            continue
        if s['init_offset'] <= value < s['init_offset'] + s['size']:
            return i, value - s['init_offset']
    # Fallback: exact end of last segment with no next
    for i, s in enumerate(segs):
        if s['init_offset'] is None:
            continue
        if value == s['init_offset'] + s['size']:
            return i, s['size']
    return None, None


def main():
    if len(sys.argv) < 2:
        print("usage: wasm_fix_absolute_brackets.py <in.o> [out.o]", file=sys.stderr)
        sys.exit(2)
    in_path = sys.argv[1]
    out_path = sys.argv[2] if len(sys.argv) > 2 else in_path

    with open(in_path, 'rb') as f:
        buf = bytearray(f.read())

    secs = parse_sections(buf)
    data_sec = None
    linking_sec = None
    for sid, hdr, body, sz in secs:
        if sid == 11:  # DATA
            data_sec = (hdr, body, sz)
        elif sid == 0:  # custom
            # need to peek name
            n, off = read_uleb(buf, body)
            name = bytes(buf[off:off + n]).decode('utf-8')
            if name == 'linking':
                linking_sec = (hdr, body, sz)

    assert data_sec is not None, "no DATA section"
    assert linking_sec is not None, "no linking section"

    _, dbody, dsz = data_sec
    segs = parse_data_segments(buf, dbody, dsz)
    print(f"data segments: {len(segs)}")

    _, lbody, lsz = linking_sec
    version, subsections, seg_names, sym_table = parse_linking_section(buf, lbody, lsz, len(segs))
    print(f"linking version {version}, {len(subsections)} subsections, {sym_table['count']} symbols")

    # print first few segs with their virt range and name
    for i, s in enumerate(segs[:5]):
        print(f"  seg[{i}] '{seg_names[i]}' init_off=0x{s['init_offset']:x} size=0x{s['size']:x}")

    # Collect all absolute DATA symbols
    abs_syms = []
    for sym in sym_table['symbols']:
        if sym['type'] != WASM_SYMBOL_TYPE_DATA:
            continue
        if sym['flags'] & WASM_SYM_UNDEFINED:
            continue
        if not (sym['flags'] & WASM_SYM_ABSOLUTE):
            continue
        abs_syms.append(sym)

    # Bucket by pair key
    by_pair = {}  # key -> {'start': sym, 'stop': sym}
    unpaired = []
    for sym in abs_syms:
        key, role = pair_key(sym['name'])
        if key is None or role is None:
            unpaired.append(sym)
            continue
        by_pair.setdefault(key, {})[role] = sym

    # Some "pairs" may only have one half; treat the orphan as unpaired
    for key, d in list(by_pair.items()):
        if 'start' not in d or 'stop' not in d:
            for sym in d.values():
                unpaired.append(sym)
            del by_pair[key]

    conversions = []
    skipped = []
    paired_resolved = 0

    # Resolve paired symbols
    for key, d in by_pair.items():
        s_sym, e_sym = d['start'], d['stop']
        s_val, e_val = s_sym['offset'], e_sym['offset']
        s_cands = find_candidates(segs, s_val)
        e_cands = find_candidates(segs, e_val)
        if not s_cands or not e_cands:
            skipped.append((s_sym, s_val, 'no seg'))
            skipped.append((e_sym, e_val, 'no seg'))
            continue
        # Prefer pairing where start and stop are in the same segment AND
        # offset_stop - offset_start == (e_val - s_val).
        size = e_val - s_val
        chosen = None
        for sc in s_cands:
            for ec in e_cands:
                if sc[0] == ec[0] and ec[1] - sc[1] == size:
                    chosen = (sc, ec)
                    break
            if chosen:
                break
        if chosen is None:
            # Multi-segment span: bind start to seg @ 0 of "next" seg
            # (start-of-segment candidate) and stop to "end of prev" (size).
            sc = next((c for c in s_cands if c[1] == 0), s_cands[0])
            ec = next((c for c in e_cands if c[1] == segs[c[0]]['size']), e_cands[-1])
            chosen = (sc, ec)
        conversions.append((s_sym, chosen[0][0], chosen[0][1]))
        conversions.append((e_sym, chosen[1][0], chosen[1][1]))
        paired_resolved += 1

    # Resolve unpaired with simple rule
    for sym in unpaired:
        val = sym['offset']
        seg_idx, new_off = find_segment_for_value(segs, val)
        if seg_idx is None:
            skipped.append((sym, val, 'no seg'))
            continue
        conversions.append((sym, seg_idx, new_off))

    print(f"\nResolved {paired_resolved} bracket pairs, {len(conversions) - 2 * paired_resolved} unpaired absolutes")
    for sym, val, why in skipped:
        print(f"  SKIP {sym['name']:40s} = 0x{val:x} ({why})")

    print(f"\nConverting {len(conversions)} absolute brackets to segment-relative:")
    for sym, seg_idx, new_off in conversions:
        print(f"  {sym['name']:40s} 0x{sym['offset']:x} -> seg#{seg_idx} ({seg_names[seg_idx]}) @ 0x{new_off:x}")

    if not conversions:
        print("nothing to do")
        return

    # Apply conversions in-memory
    for sym, seg_idx, new_off in conversions:
        sym['flags'] &= ~WASM_SYM_ABSOLUTE
        sym['segment'] = seg_idx
        sym['offset'] = new_off
        # size: keep as-is

    # Rebuild the symbol table subsection
    new_st = bytearray()
    new_st += write_uleb(sym_table['count'])
    for sym in sym_table['symbols']:
        new_st += encode_sym(sym)

    # Find the WASM_SYMBOL_TABLE subsection in linking
    st_sub = None
    for ss in subsections:
        if ss['id'] == WASM_SYMBOL_TABLE:
            st_sub = ss
            break
    assert st_sub is not None

    old_st_size = st_sub['body_size']
    new_st_size = len(new_st)
    print(f"\nsymbol table: {old_st_size} -> {new_st_size} bytes")

    # Reconstruct the linking section body:
    #   linking section body = name_string + version + subsections
    # We need to rebuild: copy [body, st_sub_body_start), then new size+payload,
    # then [st_sub_body_end, linking_end).
    # st_sub_body_start = st_sub['body_off']
    # st_sub_size_field_start = st_sub['header_off'] + 1 (one byte sub id)
    # Replace size uleb + payload.

    st_id_off = st_sub['header_off']
    st_size_field_off = st_id_off + 1  # right after id byte
    st_body_end = st_sub['body_off'] + st_sub['body_size']

    new_size_uleb = write_uleb(new_st_size)
    # Build new linking section *body* by replacing the subsection
    # The original linking section body spans [lbody, lbody+lsz)
    new_linking_body = bytearray()
    # part before subsection's size field (i.e., up to and including id byte)
    new_linking_body += buf[lbody:st_size_field_off]
    new_linking_body += new_size_uleb
    new_linking_body += new_st
    # part after subsection body
    new_linking_body += buf[st_body_end:lbody + lsz]

    new_linking_size = len(new_linking_body)
    new_linking_size_uleb = write_uleb(new_linking_size)

    # The linking section header was at linking_sec[0] (hdr offset)
    # Format: id byte (0) + uleb size + body
    lhdr, _, _ = linking_sec
    old_size_uleb_len = lbody - (lhdr + 1)

    # Build final buffer:
    # [0, lhdr+1): everything up to and including section id
    # new linking size uleb
    # new linking body
    # [lbody+lsz, end): everything after linking section

    new_buf = bytearray()
    new_buf += buf[:lhdr + 1]
    new_buf += new_linking_size_uleb
    new_buf += new_linking_body
    new_buf += buf[lbody + lsz:]

    with open(out_path, 'wb') as f:
        f.write(new_buf)
    print(f"\nwrote {out_path} ({len(buf)} -> {len(new_buf)} bytes)")


if __name__ == '__main__':
    main()

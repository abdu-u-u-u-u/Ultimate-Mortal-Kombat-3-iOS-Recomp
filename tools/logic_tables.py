#!/usr/bin/env python3
"""logic_tables.py -- extract the fight engine's data tables from the user's binary.

    python tools/logic_tables.py <UMK3 binary> <out.c> [--report <out.txt>] [--layout]

`decomp/gamecode/logic` is code. The data it runs on -- special-move lists,
animation scripts, reaction tables, AI branch tables, ~1,100 objects in all --
lives in the binary's __DATA,__data and __TEXT,__const and is NOT in this
repository. This tool reads it out of the user's own copy at build time and
writes a C file that defines every one of those objects, so the decompiled
engine links and runs. The generated file holds game data: it belongs in the
build directory and is never committed.

## Which objects

The binary is not stripped, and its STABs say which source file defined each
global and each function-local static (`N_SO` opens a file, `N_GSYM`/`N_STSYM`
name its data, `N_FUN` its functions). Every data object of every
`src/gamecode/logic/*.c` file is emitted.

Zero-filled ones (__common: `G`, `H`, `Plyr`, `mytc` ...) are emitted as
storage under `umk3_common_<name>`, unless the map lists them as `object`.
The decompiled code does not name most of them directly -- it goes through
slot pointers (`char *Plyr`,
`GAMESTATE *G`) that `runtime/fight_runtime.c` points at this storage -- but
tables do: `swtab` holds addresses inside `G`, and those must land in the
same bytes the slot reaches.

## How big each one is

The gap to the next symbol in the same section. Anonymous objects -- the
string literals behind `txt_tie`, for instance -- sit in that gap and come
along with the object before them, at the same relative offset, which is what
a pointer into them needs.

## Which words are pointers

The binary is not position-independent, so there is no rebase table to say.
A word is relocated when, and only when:

  * it is a Thumb function start (bit 0 set) of a function whose STAB places
    it in a gamecode/logic source file -- every function of which has a native
    definition; or
  * it falls inside one of the emitted logic data objects; or
  * it falls inside an object `logic_tables.map` maps to native storage.

and it is not listed as `int` in `logic_tables.map`.

Anything else is an integer and is copied as is. That rule is deliberately
narrow: the special-move lists hold switch masks such as 0x00200000 that land
inside front-end objects, and fixed-point constants such as 0x00050000 land in
the middle of functions. Neither is a pointer, and the rule does not take
them for one. Every relocation and every rejected near-miss is written to the
report so the classification can be audited.

## `int` entries: words checked to be numbers

A value can land inside a logic object by accident. `ochar_headrip_lineups`
is read as `int16_t` pairs by mkfatal.c, and its pair (0x0030, 0x000e) spells
0x000e0030, which is inside `seq_mileena_groundroll`. The map lists each such
word, by symbol and offset, with the read that settles it. A listed word is
never relocated, and the generator refuses a listed word that would not have
been relocated anyway, so a stale entry cannot hide.

## Layout

Every object is emitted in binary address order into one section, zero-filled
ones included (a zero array would otherwise go to .bss and leave the order).
Objects whose address or size is not a multiple of four are emitted as bytes,
at their exact size and alignment; one of those holding a relocation is an
error. So two objects that touch in the image touch on the host, and a read
past the end of one sees the next, as it does on the device:
`ochar_slam_damage` is 25 int16 indexed by character, and sits directly in
front of `ochar_slammed_anis`.

## Pointer width

The generated arrays hold `uintptr_t` and the file refuses to compile unless
that is four bytes: the engine's structs are 32-bit, and so are its tables.

## --layout: the same tables, without their bytes

A program that is handed to other people -- the browser build, served as a
static page -- must not carry the tables, because they are the game's. With
`--layout` the analysis above runs unchanged (the same objects, sizes,
alignment, order, section, relocation rule, map and errors) and the output
defines the same symbols with the same types and sizes, all of them zero, plus
a descriptor table (`umk3_logic_layout`, types in runtime/umk3_tables_rt.h):

  * per object: its C name and storage, its address in the binary, its size
    in bytes, whether it is `uintptr_t` words or raw bytes, and its slice of
    the relocation list;
  * per relocated word: its index in the object, the value the binary holds
    there (an iOS address: a function's address | 1, or one inside a data
    object), and the host value that replaces it, as the same expression the
    default output writes in place -- `(uintptr_t)sym + off`, or a function.

runtime/umk3_tables_rt.c reads the user's own binary at run time, checks its
LC_UUID, copies each object's bytes into its storage, checks every relocated
word against the value recorded here, and only then writes the host's. That
leaves memory as the default output would have, and a binary that is not
the one the layout was made from is refused rather than half-loaded.

The layout holds no game content: names, addresses, sizes, word indices, the
binary's own pointer values, and its uuid. It is meant for the browser build,
and differs from the default output in two ways only wasm needs: the functions
a table points at are declared as functions (`UMK3_LOGIC_FN`), because wasm-ld
refuses a function declared as `char[]`; and the `uintptr_t` arrays carry their
alignment, 4, written out (WORDS_ALIGN), because wasm32 clang otherwise aligns
an array of 16 bytes or more to 16 and opens gaps between objects that touch.
On i686 both are what the default output already gets. The filler refuses a
build in which two objects that touch in the image do not touch in memory --
the check `check_logic_tables.py --nm` makes, which has no nm to read under
wasm. --report writes the same report in both modes; it holds word values, so
it is an audit file for the machine that has the binary and is not part of
any build.
"""

import os
import re
import struct
import sys
import bisect

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import macho  # noqa: E402

N_GSYM, N_FUN, N_STSYM, N_SO = 0x20, 0x24, 0x26, 0x64
LOGIC_DIR = "/gamecode/logic/"


def load(path):
    data = macho.read_file(path)
    base = 0
    for cputype, sub, off, size, align in macho.fat_slices(data):
        if cputype == macho.CPU_TYPE_ARM and sub == 9:
            base = off
            break
    else:
        if macho.fat_slices(data):
            raise SystemExit("%s: no armv7 slice" % path)
    m = macho.MachO(data, base)
    if m.arch() != "armv7":
        raise SystemExit("%s: expected armv7, got %s" % (path, m.arch()))
    return data, base, m


def read_map(path):
    native, alias, objects = {}, {}, set()
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.split("#", 1)[0].split()
            if not line:
                continue
            if line[0] == "native" and len(line) == 3:
                native[line[1]] = line[2]
            elif line[0] == "alias" and len(line) == 3:
                alias[int(line[2], 16)] = line[1]
            elif line[0] == "object" and len(line) == 2:
                objects.add(line[1])
            elif line[0] in ("int", "number") and len(line) == 3:
                pass                    # read_ints / check_logic_tables.py
            else:
                raise SystemExit("%s: bad line %r" % (path, " ".join(line)))
    return native, alias, objects


def read_ints(path):
    """`int <binary symbol> <offset>` lines -> [(symbol, offset)]."""
    out = []
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.split("#", 1)[0].split()
            if line and line[0] == "int":
                out.append((line[1], int(line[2], 16)))
    return out


def c_name(binary_name):
    """`_funcs.7674` -> `funcs_7674`: the leading underscore is the Mach-O C
    prefix, and a function-local static's `.N` suffix is not a C identifier."""
    n = binary_name[1:] if binary_name.startswith("_") else binary_name
    return n.replace(".", "_")


LOGIC_SRC = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                         "decomp", "gamecode", "logic")
_DEFN = re.compile(r"^(?!static\b)(?!typedef\b)(?!extern\b)([A-Za-z_][\w \t*]*?[\s*])(\w+)\s*\(",
                   re.M)


def _abi_type(t):
    """A C type as wasm32 passes it: every pointer and integer up to 32 bits
    is one i32 (`long`), so only void, float, double and 64-bit integers keep
    their own spelling. Enough to give a declaration its definition's shape."""
    t = " ".join(re.sub(r"\b(const|volatile|struct|union|enum|register|inline)\b",
                        " ", t).split())
    if "*" in t or "(" in t or "[" in t:
        return "long"
    if re.search(r"\bdouble\b", t):
        return "double"
    if re.search(r"\bfloat\b", t):
        return "float"
    if re.search(r"\blong long\b|\bu?int64_t\b", t):
        return "long long"
    if t == "void":
        return "void"
    return "long"


def logic_prototypes(names, src_dir=LOGIC_SRC):
    """{name: prototype} for the functions in `names` that have a global
    definition in decomp/gamecode/logic, each spelled with the ABI-equivalent
    types _abi_type gives -- what --layout declares them as under wasm. A name
    with no definition there is left out (the caller falls back)."""
    wanted, protos = set(names), {}
    try:
        files = sorted(f for f in os.listdir(src_dir) if f.endswith(".c"))
    except OSError:
        return protos
    for fn in files:
        with open(os.path.join(src_dir, fn), encoding="utf-8", errors="replace") as f:
            text = f.read()
        for m in _DEFN.finditer(text):
            name = m.group(2)
            if name not in wanted or name in protos:
                continue
            i, depth = m.end(), 1               # just past the "("
            while i < len(text) and depth:
                depth += {"(": 1, ")": -1}.get(text[i], 0)
                i += 1
            rest = text[i:i + 200].lstrip()
            while rest.startswith("/*"):        # a comment before the body
                rest = rest[rest.find("*/") + 2:].lstrip()
            if not rest.startswith("{"):
                continue                        # a declaration, not the definition
            params, depth, cur = [], 0, ""
            for c in text[m.end():i - 1]:
                if c == "," and depth == 0:
                    params.append(cur)
                    cur = ""
                    continue
                depth += {"(": 1, ")": -1}.get(c, 0)
                cur += c
            params.append(cur)
            params = [p.strip() for p in params if p.strip()]
            if params == ["void"] or not params:
                plist = "void"
            else:
                plist = ", ".join("..." if p == "..." else _abi_type(p) for p in params)
            protos[name] = "%s %s(%s)" % (_abi_type(m.group(1)), name, plist)
    return protos


def analyse(binpath, mappath, report):
    """Everything both outputs are made from: the objects, their sizes and
    sections, and the word-by-word relocation decision. `report` only says
    whether to collect the report's lines. Raises SystemExit on the errors the
    module docstring describes, before anything is written."""
    native, alias, objects = read_map(mappath)

    data, base, m = load(binpath)
    secs = m.sections
    syms = m.symbols()

    # 1. Which names belong to gamecode/logic, per the STABs.
    cu = None
    logic_data, logic_funcs = set(), set()
    for name, t, sect, desc, val in syms:
        if t == N_SO and name:
            cu = name
        elif cu and LOGIC_DIR in cu:
            if t in (N_GSYM, N_STSYM):
                logic_data.add(name)
            elif t == N_FUN and name:
                logic_funcs.add(name.split(":", 1)[0])

    # 2. Real (non-STAB) symbols with their sections, sorted.
    real = []
    for name, t, sect, desc, val in syms:
        if t & macho.N_STAB or (t & macho.N_TYPE) != macho.N_SECT:
            continue
        real.append((val, name, secs[sect - 1], desc))
    real.sort(key=lambda r: r[0])

    def extent(i):
        a, _, s, _ = real[i]
        end = s.addr + s.size
        for j in range(i + 1, len(real)):
            if real[j][0] > a:
                if real[j][2] is s or real[j][0] < end:
                    end = min(end, real[j][0])
                break
        return end - a

    forced_int = {}
    for bname, off in read_ints(mappath):
        hit = [r[0] for r in real if r[1] == bname]
        if not hit:
            raise SystemExit("logic_tables.map: int %s: not in the binary" % bname)
        forced_int[hit[0] + off] = "%s+0x%x" % (bname, off)

    thumb_funcs = {}
    for a, name, s, desc in real:
        if s.segname == "__TEXT" and s.sectname == "__text" and desc & 0x8 \
                and name in logic_funcs:
            thumb_funcs[a] = c_name(name)
    # A name the binary defines more than once is a file-local function
    # (`q_yes` lives in moves.c, mkdrone.c and mkboss.c). The decompiled C keeps
    # only one of each as a global, so a table that pointed at another copy
    # could not be linked by name. None does today; this keeps it that way.
    names = list(thumb_funcs.values())
    ambiguous = {n for n in names if names.count(n) > 1}

    objs = []         # (addr, size, cname, binary name, section, zerofill)
    seen = set()
    for i, (a, name, s, desc) in enumerate(real):
        if name not in logic_data or a in seen:
            continue
        seen.add(a)
        zerofill = (s.flags & 0xFF) in (0x1, 0xC)
        cname = alias.get(a, c_name(name))
        if zerofill and name not in objects:
            cname = "umk3_common_" + cname
        objs.append((a, extent(i), cname, name, s, zerofill))

    emitted = [o for o in objs if not o[5]]
    common = [o for o in objs if o[5]]
    ranges = sorted((o[0], o[0] + o[1], o[2]) for o in objs)
    # objects outside gamecode/logic that the map hands to native storage
    for a, name, s, desc in real:
        if name in native and name not in logic_data:
            ranges.append((a, a + extent(real.index((a, name, s, desc))),
                           native[name]))
    ranges.sort()
    starts = [r[0] for r in ranges]
    native_targets = set(native.values())

    def target(v):
        if v & 1 and (v & ~1) in thumb_funcs:
            if thumb_funcs[v & ~1] in ambiguous:
                raise SystemExit("a table points at 0x%x, one of several "
                                 "functions named %s"
                                 % (v & ~1, thumb_funcs[v & ~1]))
            return ("fn", thumb_funcs[v & ~1], 0)
        i = bisect.bisect_right(starts, v) - 1
        if i >= 0 and ranges[i][0] <= v < ranges[i][1]:
            return ("data", ranges[i][2], v - ranges[i][0])
        return None

    def read(a, n):
        s = m.section_for_addr(a)
        off = base + s.offset + (a - s.addr)
        return data[off:off + n]

    # The report keeps every near miss: a word in an address range that the
    # rule nevertheless calls an integer.
    sym_starts = [r[0] for r in real]

    def near_miss(v):
        s = m.section_for_addr(v)
        if s is None or v < 0x1000:
            return None
        j = bisect.bisect_right(sym_starts, v) - 1
        return "%s,%s %s+0x%x" % (s.segname, s.sectname, real[j][1],
                                  v - real[j][0]) if j >= 0 else None

    rep = []
    fn_refs, data_defs = set(), []
    nrel = 0
    for a, size, cname, bname, s, zf in emitted:
        raw = read(a, size) + b"\0" * (-size % 4)
        words = struct.unpack("<%dI" % (len(raw) // 4), raw)
        items = []
        relocs = []   # (word index, host expression, the binary's value)
        orel = 0
        for k, v in enumerate(words):
            t = target(v)
            if a + k * 4 in forced_int:
                if t is None:
                    raise SystemExit("logic_tables.map: int %s is not a "
                                     "relocation candidate; drop the entry"
                                     % forced_int.pop(a + k * 4))
                forced_int.pop(a + k * 4)
                if report:
                    rep.append("int!  %-28s +0x%04x %08x  (map: a number, "
                               "not %s+0x%x)" % (cname, k * 4, v, t[1], t[2]))
                t = None
                items.append("0x%x" % v)
                continue
            if t is None:
                items.append("0x%x" % v)
                miss = near_miss(v) if v >= 0x1000 else None
                if miss and report:
                    rep.append("int   %-28s +0x%04x %08x  (%s)"
                               % (cname, k * 4, v, miss))
                continue
            kind, tname, off = t
            nrel += 1
            orel += 1
            if kind == "fn":
                fn_refs.add(tname)
                items.append("(uintptr_t)%s" % tname)
            elif off:
                items.append("(uintptr_t)%s + 0x%x" % (tname, off))
            else:
                items.append("(uintptr_t)%s" % tname)
            relocs.append((k, items[-1], v))
            if report:
                rep.append("%-5s %-28s +0x%04x %08x -> %s%s"
                           % (kind, cname, k * 4, v, tname,
                              "+0x%x" % off if off else ""))
        exact = a % 4 == 0 and size % 4 == 0
        if not exact and orel:
            raise SystemExit("%s at 0x%x is not word-aligned and holds %d "
                             "relocation(s)" % (bname, a, orel))
        data_defs.append((cname, bname, a, s, items, size,
                          None if exact else read(a, size), relocs))
    if forced_int:
        raise SystemExit("logic_tables.map: int %s is in no emitted object"
                         % ", ".join(sorted(forced_int.values())))

    return Tables(m.uuid, data_defs, common, fn_refs, native_targets, nrel, rep)


class Tables(object):
    """What analyse() found, for emit_c() and emit_layout().

    data_defs  per emitted object, in address order: (C name, binary name,
               address, section, items, size, raw, relocs). `items` is one C
               initializer per word; `raw` the object's bytes when it is not
               word-aligned, None when it is; `relocs` its relocated words as
               (word index, host expression, the binary's value).
    common     per zero-filled object: (address, size, C name, binary name,
               section, zerofill).
    fn_refs    the functions the tables point at; native_targets the native
               storage logic_tables.map names; nrel the relocated words; rep
               the report's lines (empty unless asked for).
    """

    def __init__(self, uuid, data_defs, common, fn_refs, native_targets, nrel,
                 rep):
        self.uuid = uuid
        self.data_defs = data_defs
        self.common = common
        self.fn_refs = fn_refs
        self.native_targets = native_targets
        self.nrel = nrel
        self.rep = rep


# The section both outputs put every object in.
SECTION_LINES = [
    "/* One section, binary order: see Layout in tools/logic_tables.py. */",
    "#if defined(__APPLE__)",
    "#define UMK3_LOGIC __attribute__((section(\"__DATA,__umk3logic\")))",
    "#elif defined(__wasm__)",
    "#define UMK3_LOGIC  /* no named sections: order is not guaranteed */",
    "#else",
    "#define UMK3_LOGIC __attribute__((section(\".data.umk3logic\")))",
    "#endif",
]


def data_externs(t):
    """The data declarations both outputs put ahead of the definitions, so a
    word can name an object defined further down."""
    out = []
    for n in sorted(t.native_targets):
        out.append("extern char %s[];" % n)
    for cname, bname, a, s, items, size, raw, relocs in t.data_defs:
        out.append("extern %s %s[];" % ("unsigned char" if raw else "uintptr_t",
                                        cname))
    for a, size, cname, bname, s, zf in t.common:
        out.append("extern %s %s[];" % ("uintptr_t" if a % 4 == 0 and size % 4 == 0
                                        else "unsigned char", cname))
    return out


def raw_align(a):
    """The alignment a byte-array object is given: its address's, at most 4."""
    align = a & -a if a & 3 else 4
    return min(align, 4)


def common_lines(t, words_attr=""):
    """The zero-filled objects, the same in both outputs but for `words_attr`,
    which --layout puts on the uintptr_t ones (see WORDS_ALIGN)."""
    out = []
    out.append("/* Runtime state: zero, owned through the slot pointers. Zero-filled")
    out.append(" * on the device too, but kept in the section so it stays in order. */")
    for a, size, cname, bname, s, zf in t.common:
        if a % 4 == 0 and size % 4 == 0:
            out.append("uintptr_t %s[%d] UMK3_LOGIC%s = {0};  /* %s 0x%08x, %d bytes */"
                       % (cname, size // 4, words_attr, bname, a, size))
        else:
            out.append("unsigned char %s[%d] UMK3_LOGIC = {0};  /* %s 0x%08x, %d bytes */"
                       % (cname, size, bname, a, size))
    return out


# --layout's uintptr_t arrays say their alignment, 4, which is what they have
# anyway on i686. wasm32 clang gives a global array of 16 bytes or more an
# alignment of 16 unless one is written, and that opens a gap between two
# objects that touch in the image -- the read past the end of one would no
# longer see the next (Layout, above). With an alignment written, only it
# counts. The default output does not need it: it cannot link under wasm.
WORDS_ALIGN = " __attribute__((aligned(4)))"


def emit_c(t):
    """The default output: every object with its bytes, relocated words as
    host expressions. A list of lines."""
    out = []
    out.append("/* GENERATED by tools/logic_tables.py -- do not edit, do not commit.")
    out.append(" *")
    out.append(" * Every data object of src/gamecode/logic, read out of the user's own")
    out.append(" * binary (uuid %s) at build time." % t.uuid)
    out.append(" * %d objects, %d relocated words. */" % (len(t.data_defs), t.nrel))
    out.append("#include <stdint.h>")
    out.append("typedef char umk3_logic_tables_need_32bit_pointers"
               "[sizeof(void *) == 4 ? 1 : -1];")
    out.append("")
    out.extend(SECTION_LINES)
    out.append("")
    for f in sorted(t.fn_refs):
        out.append("extern char %s[];" % f)
    out.extend(data_externs(t))
    out.append("")
    for cname, bname, a, s, items, size, raw, relocs in t.data_defs:
        out.append("/* %s  0x%08x  %s,%s */" % (bname, a, s.segname, s.sectname))
        if raw is not None:
            out.append("unsigned char %s[%d] UMK3_LOGIC __attribute__((aligned(%d))) = {"
                       % (cname, size, raw_align(a)))
            for k in range(0, len(raw), 16):
                out.append("    " + ", ".join(str(b) for b in raw[k:k + 16]) + ",")
        else:
            out.append("uintptr_t %s[%d] UMK3_LOGIC = {" % (cname, len(items)))
            for k in range(0, len(items), 6):
                out.append("    " + ", ".join(items[k:k + 6]) + ",")
        out.append("};")
    out.append("")
    out.extend(common_lines(t))
    return out


def emit_layout(t):
    """The --layout output: the symbols emit_c() defines, with the same types,
    sizes, alignment (written out: WORDS_ALIGN), order and section, all zero;
    and `umk3_logic_layout`, which says where each object's bytes are in the
    binary and which words to relocate, for runtime/umk3_tables_rt.c. Never a
    byte of the objects themselves: only names, addresses, sizes and word
    indices, and for each relocated word the binary's value -- an iOS address
    -- which the filler checks before it writes the host's. A list of
    lines."""
    out = []
    out.append("/* GENERATED by tools/logic_tables.py --layout -- do not edit, do not commit.")
    out.append(" *")
    out.append(" * Where every data object of src/gamecode/logic lies in the user's own")
    out.append(" * binary (uuid %s), and none of their bytes:" % t.uuid)
    out.append(" * the storage below is zero until runtime/umk3_tables_rt.c copies it out")
    out.append(" * of that binary at run time and relocates the words umk3_logic_relocs")
    out.append(" * lists. See --layout in tools/logic_tables.py.")
    out.append(" * %d objects, %d relocated words. */" % (len(t.data_defs), t.nrel))
    out.append("#include <stdint.h>")
    out.append("#include \"umk3_tables_rt.h\"")
    out.append("typedef char umk3_logic_tables_need_32bit_pointers"
               "[sizeof(void *) == 4 ? 1 : -1];")
    out.append("")
    out.extend(SECTION_LINES)
    out.append("")
    out.append("/* A function a table points at. wasm-ld keeps functions and data apart")
    out.append(" * and refuses one declared as `char[]`; and a declaration whose shape")
    out.append(" * differs from the definition's makes wasm-ld hand this file a stub that")
    out.append(" * traps (\"function signature mismatch\"), so the table would point at the")
    out.append(" * stub. Each is declared with the shape of its definition in")
    out.append(" * decomp/gamecode/logic, as wasm32 passes it (logic_prototypes()). */")
    out.append("#if defined(__wasm__)")
    protos = logic_prototypes(t.fn_refs)
    for f in sorted(t.fn_refs):
        out.append("%s;" % protos.get(f, "void %s(void)" % f))
    out.append("#define UMK3_LOGIC_FN(f) extern __typeof__(f) f")
    out.append("#else")
    out.append("#define UMK3_LOGIC_FN(f) extern char f[]")
    out.append("#endif")
    out.append("")
    for f in sorted(t.fn_refs):
        out.append("UMK3_LOGIC_FN(%s);" % f)
    out.extend(data_externs(t))
    out.append("")
    for cname, bname, a, s, items, size, raw, relocs in t.data_defs:
        out.append("/* %s  0x%08x  %s,%s */" % (bname, a, s.segname, s.sectname))
        if raw is not None:
            out.append("unsigned char %s[%d] UMK3_LOGIC __attribute__((aligned(%d))) = {0};"
                       % (cname, size, raw_align(a)))
        else:
            out.append("uintptr_t %s[%d] UMK3_LOGIC%s = {0};"
                       % (cname, len(items), WORDS_ALIGN))
    out.append("")
    out.extend(common_lines(t, WORDS_ALIGN))
    out.append("")
    out.append("/* Each relocated word: its index in the object, the binary's value there")
    out.append(" * (checked before anything is written), and the host's. */")
    if t.nrel:
        out.append("static const UMK3_RT_RELOC umk3_logic_relocs[%d] = {" % t.nrel)
        for cname, bname, a, s, items, size, raw, relocs in t.data_defs:
            if relocs:
                out.append("    /* %s */" % cname)
            for k, expr, v in relocs:
                out.append("    { 0x%x, 0x%08x, %s }," % (k, v, expr))
        out.append("};")
        out.append("")
    out.append("/* Each object: name, storage, address in the binary, size in bytes,")
    out.append(" * 1 for uintptr_t words or 0 for bytes, first relocation, count. */")
    if t.data_defs:
        out.append("static const UMK3_RT_OBJECT umk3_logic_objects[%d] = {"
                   % len(t.data_defs))
        first = 0
        for cname, bname, a, s, items, size, raw, relocs in t.data_defs:
            out.append("    { \"%s\", %s, 0x%08x, %d, %d, %d, %d },"
                       % (cname, cname, a, size, 0 if raw is not None else 1,
                          first, len(relocs)))
            first += len(relocs)
        out.append("};")
        out.append("")
    out.append("const UMK3_RT_LAYOUT umk3_logic_layout = {")
    out.append("    \"%s\"," % (t.uuid or ""))
    out.append("    %s, %d," % ("umk3_logic_objects" if t.data_defs else "0",
                                len(t.data_defs)))
    out.append("    %s, %d," % ("umk3_logic_relocs" if t.nrel else "0", t.nrel))
    out.append("};")
    return out


def main(argv):
    layout = "--layout" in argv
    if layout:
        argv = [x for x in argv if x != "--layout"]
    if len(argv) < 3:
        raise SystemExit(__doc__)
    binpath, outpath = argv[1], argv[2]
    report = argv[argv.index("--report") + 1] if "--report" in argv else None
    mappath = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                           "logic_tables.map")
    t = analyse(binpath, mappath, report)
    out = emit_layout(t) if layout else emit_c(t)
    with open(outpath, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(out) + "\n")
    if report:
        with open(report, "w", encoding="utf-8", newline="\n") as f:
            f.write("\n".join(t.rep) + "\n")
    print("logic_tables%s: %d objects, %d relocations, %d functions referenced -> %s"
          % (" --layout" if layout else "", len(t.data_defs), t.nrel,
             len(t.fn_refs), outpath))


if __name__ == "__main__":
    main(sys.argv)

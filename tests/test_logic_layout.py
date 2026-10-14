#!/usr/bin/env python3
"""test_logic_layout.py -- tools/logic_tables.py --layout, without the game's binary.

    python tests/test_logic_layout.py

Builds a synthetic armv7 Mach-O with the shapes the generators look for --
STABs placing data and functions in src/gamecode/logic, word and byte objects
in __TEXT,__const and __DATA,__data, zero-fill __common objects, the
`_ochar_headrip_lineups+0x8` word logic_tables.map lists as `int`,
`_CurrentScene` / pool / `_Level_Info` / `_LevelMusic`, and the playback
scripts at 0x000de242 -- none of it the game's bytes. Then:

  * the default output is byte for byte GOLDEN_DEFAULT below, which is what
    tools/logic_tables.py wrote for this file before --layout existed;
  * --layout defines the same symbols with the same types, sizes, alignment
    (written out for the uintptr_t arrays), order and section; writes the
    same report; stops with the same errors;
    and carries none of the objects' content -- only names, addresses, sizes,
    word indices and the binary's pointer values;
  * its descriptor table says what this file's objects and pointers are;
  * when a 32-bit compiler is at hand (the pinned llvm-mingw in toolchain/,
    or $UMK3_CC32): the default tables -- logic_tables.c, level_info.c and
    seq_data.c as generated -- and the layout plus runtime/umk3_tables_rt.c
    filled from the file at run time are each linked into the same harness,
    which prints every object word by word (pointers as symbol+offset),
    whether objects that touch in the image touch in memory, the relocated
    Level_Info and every script byte. The two prints must match. With
    Emscripten ($UMK3_EMCC, or toolchain/emsdk) the layout build is also run
    as wasm under node and must print the same.
"""

import glob
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOLS = os.path.join(ROOT, "tools")
EXE = ".exe" if os.name == "nt" else ""

UUID = bytes.fromhex("90d6f56a18e2303f8f2053b02a42742e")

N_GSYM, N_FUN, N_STSYM, N_SO = 0x20, 0x24, 0x26, 0x64
N_SECT_EXT, N_SECT_LOCAL, N_UNDF_EXT = 0x0f, 0x0e, 0x01
THUMB = 0x0008                          # N_ARM_THUMB_DEF in n_desc


# ------------------------------------------------------ the synthetic binary

def build_macho(sections, symbols, uuid=UUID):
    """A thin 32-bit armv7 Mach-O. `sections`: (segment, section, address,
    contents or an int size for zero-fill, flags), grouped into one
    LC_SEGMENT per segment in order. `symbols`: (name, n_type, n_sect,
    n_desc, n_value)."""
    segs = []
    for sec in sections:
        if not segs or segs[-1][0] != sec[0]:
            segs.append((sec[0], []))
        segs[-1][1].append(sec)
    cmds_size = sum(56 + 68 * len(s[1]) for s in segs) + 24 + 24
    off = (28 + cmds_size + 0xff) & ~0xff
    body = b""
    placed = []
    for seg, secs in segs:
        for segname, sectname, addr, contents, flags in secs:
            if isinstance(contents, int):
                placed.append((segname, sectname, addr, contents, 0, flags))
            else:
                placed.append((segname, sectname, addr, len(contents),
                               off + len(body), flags))
                body += contents
                body += b"\0" * (-len(body) % 4)
    symoff = off + len(body)
    strtab = b"\0"
    nlist = b""
    for name, n_type, n_sect, n_desc, n_value in symbols:
        strx = len(strtab) if name else 0
        if name:
            strtab += name.encode() + b"\0"
        nlist += struct.pack("<IBBhI", strx, n_type, n_sect, n_desc, n_value)
    stroff = symoff + len(nlist)

    cmds = b""
    k = 0
    for seg, secs in segs:
        lo = min(s[2] for s in secs)
        hi = max(s[2] + (s[3] if isinstance(s[3], int) else len(s[3]))
                 for s in secs)
        cmd = struct.pack("<II16sIIIIIIII", 0x1, 56 + 68 * len(secs),
                          seg.encode(), lo, hi - lo, 0, 0, 7, 7, len(secs), 0)
        for _ in secs:
            segname, sectname, addr, size, foff, flags = placed[k]
            k += 1
            cmd += struct.pack("<16s16sIIIIIIIII", sectname.encode(),
                               segname.encode(), addr, size, foff, 2, 0, 0,
                               flags, 0, 0)
        cmds += cmd
    cmds += struct.pack("<II16s", 0x1b, 24, uuid)
    cmds += struct.pack("<IIIIII", 0x2, 24, symoff, len(symbols), stroff,
                        len(strtab))
    header = struct.pack("<7I", 0xfeedface, 12, 9, 2, len(segs) + 2,
                         len(cmds), 0x00200085)
    image = header + cmds
    image += b"\0" * (off - len(image))
    return image + body + nlist + strtab


def fat(thin):
    """`thin` as the armv7 slice of a fat file, behind an x86_64 one."""
    head = struct.pack(">II", 0xcafebabe, 2)
    head += struct.pack(">5I", 0x01000007, 3, 0x40, 0x20, 4)
    head += struct.pack(">5I", 12, 9, 0x1000, len(thin), 12)
    head += b"\0" * (0x40 - len(head)) + b"\xcc" * 0x20
    return head + b"\0" * (0x1000 - len(head)) + thin


def words(*ws):
    return struct.pack("<%dI" % len(ws), *ws)


A_TEXT, A_CONST, A_DATA, A_COMMON = 0x00002000, 0x000de200, 0x00100000, 0x00100100
SEQ_LO, SEQ_HI = 0x000de242, 0x000de2ff        # the scripts: 189 bytes, odd
POOL = b"pit.scene\0church.scene\0sky.pvr\0"

# Content: the words and bytes the layout must never carry. Every non-zero
# number is >= 0x10000, so none can pass for a wasm function index.
CONTENT_WORDS = [0x00050000, 0xc0ffee33, 0xc0ffee44, 0x00200000, 0xc0ffee11,
                 0xc0ffee22, 0x000e0030, 0xc0ffee55, 0xc0ffee66]
ODD5, ODD3, RAW6, RAW2 = b"\xa5\x5a\xc3\x3c\x96", b"\x69\x99\x0f", b"QRSTUV", b"\xab\xcd"

# Level_Info's words; the five in [0x00100054, 0x00100080) point into POOL.
INFO = [0x3f800000, 0x00100054, 0x0010005e, 0x41200000,
        0x0010006b, 0x00100080, 0x00100053, 0x0010007f,
        0, 0, 0, 0, 0, 0, 0, 0x00100060]


def synthetic(ochar_word=0x00100004, raw6=RAW6):
    """The test binary. `ochar_word` is the word the map's `int` entry names;
    `raw6` the contents of a 6-byte object (the error tests vary both)."""
    const = words(0x00050000, 0xc0ffee33)                     # seq_tbl
    const += ODD5 + ODD3                                      # odd5, odd3
    const += words(A_TEXT + 0x11, A_DATA + 0x18, 0xc0ffee44) + b"\0" * 36
    const += bytes((i * 37 + 11) & 0xff for i in range(0xde2ff - 0xde240))
    data = words(A_TEXT + 1, 0x00200000, A_DATA + 0x14, A_COMMON + 8)   # tbl_a
    data += words(0xc0ffee11, A_DATA, A_TEXT + 0x11)                    # tbl_b
    data += words(0xc0ffee22, 0x000e0030, ochar_word)         # ochar_headrip_lineups
    data += raw6 + RAW2                                       # raw6, raw2
    data += words(0x0010004c, A_TEXT, A_COMMON + 0x2c, A_COMMON + 0x20)  # tbl_c
    data += words(0, 0)                                       # zero_tbl
    data += words(0xc0ffee55, 0xc0ffee66)                     # fe_data
    data += words(0)                                          # CurrentScene
    data += POOL + b"\0" * (0x2c - len(POOL))
    data += words(*INFO)                                      # Level_Info
    data += b"Pit.mp3\0" + b"\0" * 0x38                       # LevelMusic
    assert len(const) == 0xff and len(data) == 0x100
    sections = [
        ("__TEXT", "__text", A_TEXT, b"\0" * 0x40, 0x80000400),
        ("__TEXT", "__const", A_CONST, const, 0),
        ("__DATA", "__data", A_DATA, data, 0),
        ("__DATA", "__common", A_COMMON, 0x40, 0x1),
    ]
    logic_data = [
        ("_seq_tbl", N_STSYM, 2), ("_odd5", N_STSYM, 2), ("_odd3", N_STSYM, 2),
        ("_const_ptrs", N_STSYM, 2), ("_tbl_a", N_GSYM, 0),
        ("_tbl_b", N_GSYM, 0), ("_ochar_headrip_lineups", N_GSYM, 0),
        ("_raw6", N_STSYM, 3), ("_raw2", N_STSYM, 3), ("_tbl_c", N_GSYM, 0),
        ("_zero_tbl", N_GSYM, 0), ("_G", N_GSYM, 0), ("_TList", N_GSYM, 0),
        ("_odd_common", N_GSYM, 0), ("_odd2", N_GSYM, 0),
        ("_last_common", N_GSYM, 0),
    ]
    syms = [("/b/", N_SO, 0, 0, 0), ("/b/src/gamecode/logic/mk3.c", N_SO, 0, 0, 0),
            ("_fn_alpha", N_FUN, 1, 0, A_TEXT), ("", N_FUN, 0, 0, 0x10),
            ("_fn_beta", N_FUN, 1, 0, A_TEXT + 0x10), ("", N_FUN, 0, 0, 0x10)]
    syms += [(n, t, s, 0, 0) for n, t, s in logic_data]
    syms += [("", N_SO, 1, 0, 0), ("/b/src/gamecode/GameCode.c", N_SO, 0, 0, 0),
             ("_fe_func", N_FUN, 1, 0, A_TEXT + 0x20), ("", N_FUN, 0, 0, 0x10)]
    syms += [(n, N_GSYM, 0, 0, 0) for n in
             ("_fe_data", "_CurrentScene", "_Level_Info", "_LevelMusic",
              "_seq_area")]
    syms += [("", N_SO, 1, 0, 0)]
    real = [
        ("_fn_alpha", N_SECT_EXT, 1, THUMB, A_TEXT),
        ("_fn_beta", N_SECT_EXT, 1, THUMB, A_TEXT + 0x10),
        ("_fe_func", N_SECT_EXT, 1, THUMB, A_TEXT + 0x20),
        ("_seq_tbl", N_SECT_LOCAL, 2, 0, 0x000de200),
        ("_odd5", N_SECT_LOCAL, 2, 0, 0x000de208),
        ("_odd3", N_SECT_LOCAL, 2, 0, 0x000de20d),
        ("_const_ptrs", N_SECT_LOCAL, 2, 0, 0x000de210),
        ("_seq_area", N_SECT_LOCAL, 2, 0, 0x000de240),
        ("_tbl_a", N_SECT_EXT, 3, 0, 0x00100000),
        ("_tbl_b", N_SECT_EXT, 3, 0, 0x00100010),
        ("_ochar_headrip_lineups", N_SECT_EXT, 3, 0, 0x0010001c),
        ("_raw6", N_SECT_LOCAL, 3, 0, 0x00100028),
        ("_raw2", N_SECT_LOCAL, 3, 0, 0x0010002e),
        ("_tbl_c", N_SECT_EXT, 3, 0, 0x00100030),
        ("_zero_tbl", N_SECT_EXT, 3, 0, 0x00100040),
        ("_fe_data", N_SECT_EXT, 3, 0, 0x00100048),
        ("_CurrentScene", N_SECT_EXT, 3, 0, 0x00100050),
        ("_Level_Info", N_SECT_EXT, 3, 0, 0x00100080),
        ("_LevelMusic", N_SECT_EXT, 3, 0, 0x001000c0),
        ("_G", N_SECT_EXT, 4, 0, A_COMMON),
        ("_TList", N_SECT_EXT, 4, 0, A_COMMON + 0x20),
        ("_odd_common", N_SECT_EXT, 4, 0, A_COMMON + 0x24),
        ("_odd2", N_SECT_EXT, 4, 0, A_COMMON + 0x26),
        ("_last_common", N_SECT_EXT, 4, 0, A_COMMON + 0x28),
        # second symbols of a name, which level_info.py's setdefault ignores:
        # one inside Level_Info, one inside the pool
        ("_LevelMusic", N_SECT_LOCAL, 3, 0, 0x001000a0),
        ("_fe_data", N_SECT_LOCAL, 3, 0, 0x00100060),
        ("_printf", N_UNDF_EXT, 0, 0, 0),
    ]
    return build_macho(sections, syms + real)


# Objects in address order: (C name, address, size, words?), then the common.
OBJECTS = [("seq_tbl", 0x000de200, 8, 1), ("odd5", 0x000de208, 5, 0),
           ("odd3", 0x000de20d, 3, 0), ("const_ptrs", 0x000de210, 0x30, 1),
           ("tbl_a", 0x00100000, 0x10, 1), ("tbl_b", 0x00100010, 0x0c, 1),
           ("ochar_headrip_lineups", 0x0010001c, 0x0c, 1),
           ("raw6", 0x00100028, 6, 0), ("raw2", 0x0010002e, 2, 0),
           ("tbl_c", 0x00100030, 0x10, 1), ("zero_tbl", 0x00100040, 8, 1)]
COMMON = [("umk3_common_G", A_COMMON, 0x20), ("TList", A_COMMON + 0x20, 4),
          ("umk3_common_odd_common", A_COMMON + 0x24, 2),
          ("umk3_common_odd2", A_COMMON + 0x26, 2),
          ("umk3_common_last_common", A_COMMON + 0x28, 0x18)]
# (object, word, the binary's value, host expression)
RELOCS = [("const_ptrs", 0, 0x00002011, "(uintptr_t)fn_beta"),
          ("const_ptrs", 1, 0x00100018, "(uintptr_t)tbl_b + 0x8"),
          ("tbl_a", 0, 0x00002001, "(uintptr_t)fn_alpha"),
          ("tbl_a", 2, 0x00100014, "(uintptr_t)tbl_b + 0x4"),
          ("tbl_a", 3, 0x00100108, "(uintptr_t)umk3_common_G + 0x8"),
          ("tbl_b", 1, 0x00100000, "(uintptr_t)tbl_a"),
          ("tbl_b", 2, 0x00002011, "(uintptr_t)fn_beta"),
          ("tbl_c", 2, 0x0010012c, "(uintptr_t)umk3_common_last_common + 0x4"),
          ("tbl_c", 3, 0x00100120, "(uintptr_t)TList")]


# What tools/logic_tables.py wrote for synthetic() before --layout existed
# (git 8b16441), byte for byte.
GOLDEN_DEFAULT = r"""/* GENERATED by tools/logic_tables.py -- do not edit, do not commit.
 *
 * Every data object of src/gamecode/logic, read out of the user's own
 * binary (uuid 90d6f56a18e2303f8f2053b02a42742e) at build time.
 * 11 objects, 9 relocated words. */
#include <stdint.h>
typedef char umk3_logic_tables_need_32bit_pointers[sizeof(void *) == 4 ? 1 : -1];

/* One section, binary order: see Layout in tools/logic_tables.py. */
#if defined(__APPLE__)
#define UMK3_LOGIC __attribute__((section("__DATA,__umk3logic")))
#elif defined(__wasm__)
#define UMK3_LOGIC  /* no named sections: order is not guaranteed */
#else
#define UMK3_LOGIC __attribute__((section(".data.umk3logic")))
#endif

extern char fn_alpha[];
extern char fn_beta[];
extern uintptr_t seq_tbl[];
extern unsigned char odd5[];
extern unsigned char odd3[];
extern uintptr_t const_ptrs[];
extern uintptr_t tbl_a[];
extern uintptr_t tbl_b[];
extern uintptr_t ochar_headrip_lineups[];
extern unsigned char raw6[];
extern unsigned char raw2[];
extern uintptr_t tbl_c[];
extern uintptr_t zero_tbl[];
extern uintptr_t umk3_common_G[];
extern uintptr_t TList[];
extern unsigned char umk3_common_odd_common[];
extern unsigned char umk3_common_odd2[];
extern uintptr_t umk3_common_last_common[];

/* _seq_tbl  0x000de200  __TEXT,__const */
uintptr_t seq_tbl[2] UMK3_LOGIC = {
    0x50000, 0xc0ffee33,
};
/* _odd5  0x000de208  __TEXT,__const */
unsigned char odd5[5] UMK3_LOGIC __attribute__((aligned(4))) = {
    165, 90, 195, 60, 150,
};
/* _odd3  0x000de20d  __TEXT,__const */
unsigned char odd3[3] UMK3_LOGIC __attribute__((aligned(1))) = {
    105, 153, 15,
};
/* _const_ptrs  0x000de210  __TEXT,__const */
uintptr_t const_ptrs[12] UMK3_LOGIC = {
    (uintptr_t)fn_beta, (uintptr_t)tbl_b + 0x8, 0xc0ffee44, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
};
/* _tbl_a  0x00100000  __DATA,__data */
uintptr_t tbl_a[4] UMK3_LOGIC = {
    (uintptr_t)fn_alpha, 0x200000, (uintptr_t)tbl_b + 0x4, (uintptr_t)umk3_common_G + 0x8,
};
/* _tbl_b  0x00100010  __DATA,__data */
uintptr_t tbl_b[3] UMK3_LOGIC = {
    0xc0ffee11, (uintptr_t)tbl_a, (uintptr_t)fn_beta,
};
/* _ochar_headrip_lineups  0x0010001c  __DATA,__data */
uintptr_t ochar_headrip_lineups[3] UMK3_LOGIC = {
    0xc0ffee22, 0xe0030, 0x100004,
};
/* _raw6  0x00100028  __DATA,__data */
unsigned char raw6[6] UMK3_LOGIC __attribute__((aligned(4))) = {
    81, 82, 83, 84, 85, 86,
};
/* _raw2  0x0010002e  __DATA,__data */
unsigned char raw2[2] UMK3_LOGIC __attribute__((aligned(2))) = {
    171, 205,
};
/* _tbl_c  0x00100030  __DATA,__data */
uintptr_t tbl_c[4] UMK3_LOGIC = {
    0x10004c, 0x2000, (uintptr_t)umk3_common_last_common + 0x4, (uintptr_t)TList,
};
/* _zero_tbl  0x00100040  __DATA,__data */
uintptr_t zero_tbl[2] UMK3_LOGIC = {
    0x0, 0x0,
};

/* Runtime state: zero, owned through the slot pointers. Zero-filled
 * on the device too, but kept in the section so it stays in order. */
uintptr_t umk3_common_G[8] UMK3_LOGIC = {0};  /* _G 0x00100100, 32 bytes */
uintptr_t TList[1] UMK3_LOGIC = {0};  /* _TList 0x00100120, 4 bytes */
unsigned char umk3_common_odd_common[2] UMK3_LOGIC = {0};  /* _odd_common 0x00100124, 2 bytes */
unsigned char umk3_common_odd2[2] UMK3_LOGIC = {0};  /* _odd2 0x00100126, 2 bytes */
uintptr_t umk3_common_last_common[6] UMK3_LOGIC = {0};  /* _last_common 0x00100128, 24 bytes */
"""


# ------------------------------------------------------------------- running

def read(path):
    with open(path, encoding="utf-8") as f:
        return f.read()


def run_tool(script, *args):
    return subprocess.run([sys.executable, os.path.join(TOOLS, script)] + list(args),
                          capture_output=True, text=True, timeout=300)


def first_existing(*paths):
    for p in paths:
        if p and os.path.exists(p):
            return p
    return None


def find_cc32():
    return first_existing(
        os.environ.get("UMK3_CC32"),
        os.path.join(ROOT, "toolchain", "llvm-mingw-20260616-ucrt-x86_64", "bin",
                     "i686-w64-mingw32-clang" + EXE),
        shutil.which("i686-w64-mingw32-clang"))


def find_emcc():
    """(emcc, node, environment) or None."""
    env = dict(os.environ)
    emcc = os.environ.get("UMK3_EMCC")
    emsdk = os.path.join(ROOT, "toolchain", "emsdk")
    if not emcc:
        emcc = first_existing(
            os.path.join(emsdk, "upstream", "emscripten", "emcc" + EXE),
            os.path.join(emsdk, "upstream", "emscripten", "emcc"))
        if emcc and os.path.exists(os.path.join(emsdk, ".emscripten")):
            env["EMSDK"] = emsdk
            env["EM_CONFIG"] = os.path.join(emsdk, ".emscripten")
    emcc = emcc or shutil.which("emcc")
    nodes = sorted(glob.glob(os.path.join(emsdk, "node", "*", "node" + EXE)) +
                   glob.glob(os.path.join(emsdk, "node", "*", "bin", "node")))
    node = os.environ.get("UMK3_NODE") or (nodes[-1] if nodes else None) \
        or shutil.which("node")
    if not emcc or not node:
        return None
    env["PATH"] = os.pathsep.join([os.path.dirname(node), os.path.dirname(emcc),
                                   env.get("PATH", "")])
    return emcc, node, env


def c_bytes(b):
    return ", ".join(str(x) for x in b)


def harness(layout):
    """The C program both builds link: it prints the state the tables leave."""
    names = [o[0] for o in OBJECTS] + [c[0] for c in COMMON]
    info = words(*INFO)
    lines = ["#include <stdint.h>", "#include <stdio.h>", "#include <string.h>"]
    if layout:
        lines.append('#include "umk3_tables_rt.h"')
    else:
        lines.append("void umk3_relocate_level_info(void);")
        lines.append("const unsigned char *umk3_seq_addr(unsigned long ios_address);")
    lines.append("void fn_alpha(void) {}")
    lines.append("void fn_beta(void) {}")
    lines += ["extern unsigned char %s[];" % n for n in names]
    lines.append("char Level_Info[%d] = { %s };"
                 % (len(info), ", ".join(str(x - 256 if x > 127 else x) for x in info)))
    lines.append("static const unsigned char level_init[%d] = { %s };"
                 % (len(info), c_bytes(info)))
    lines.append("struct obj { const char *name; const unsigned char *p; "
                 "unsigned size, words, vmaddr; };")
    lines.append("static const struct obj objs[] = {")
    for n, a, size, w in OBJECTS:
        lines.append('    { "%s", %s, %d, %d, 0x%x },' % (n, n, size, w, a))
    for n, a, size in COMMON:
        lines.append('    { "%s", %s, %d, %d, 0x%x },' % (n, n, size, size % 4 == 0, a))
    lines.append("};")
    lines.append(HARNESS_MAIN)
    return "\n".join(lines) + "\n"


HARNESS_MAIN = r"""
#define NOBJS (sizeof objs / sizeof objs[0])

static void sym(uintptr_t v)
{
    unsigned i;

    if (v == (uintptr_t)fn_alpha) { printf(" fn_alpha"); return; }
    if (v == (uintptr_t)fn_beta)  { printf(" fn_beta");  return; }
    for (i = 0; i < NOBJS; i++)
        if (v >= (uintptr_t)objs[i].p && v < (uintptr_t)objs[i].p + objs[i].size) {
            printf(" %s+0x%x", objs[i].name, (unsigned)(v - (uintptr_t)objs[i].p));
            return;
        }
    printf(" 0x%lx", (unsigned long)v);
}

int main(int argc, char **argv)
{
    const unsigned char *base;
    uintptr_t minp = UINTPTR_MAX;
    unsigned i, k;
    unsigned long a;

#ifdef UMK3_TABLES_RT_H
    char err[300];

    if (argc < 2 || umk3_tables_fill_file(argv[1], err, sizeof err) != 0) {
        printf("fill: %s\n", argc < 2 ? "no file" : err);
        return 1;
    }
#else
    (void)argc;
    (void)argv;
#endif
    for (i = 0; i < NOBJS; i++) {
        printf("obj %s %s %u:", objs[i].name, objs[i].words ? "W" : "B", objs[i].size);
        for (k = 0; k < objs[i].size; k += objs[i].words ? 4 : 1) {
            if (objs[i].words) {
                uintptr_t v;
                memcpy(&v, objs[i].p + k, sizeof v);
                sym(v);
            } else {
                printf(" %02x", objs[i].p[k]);
            }
        }
        printf("\n");
    }
    /* objects that touch in the image must touch here */
    for (i = 1; i < NOBJS; i++)
        if (objs[i - 1].vmaddr + objs[i - 1].size == objs[i].vmaddr)
            printf("touch %s %s %s\n", objs[i - 1].name, objs[i].name,
                   objs[i].p - objs[i - 1].p == (long)objs[i - 1].size ? "ok" : "BROKEN");

    umk3_relocate_level_info();
    for (k = 0; k < sizeof Level_Info; k += 4)
        if (memcmp(Level_Info + k, level_init + k, 4)) {
            uint32_t v;
            memcpy(&v, Level_Info + k, 4);
            if (v < minp)
                minp = v;
        }
    for (k = 0; k < sizeof Level_Info; k += 4) {
        uint32_t v;
        memcpy(&v, Level_Info + k, 4);
        if (memcmp(Level_Info + k, level_init + k, 4))
            printf("level +0x%02x -> pool+%u \"%s\"\n", k, (unsigned)(v - minp),
                   (const char *)(uintptr_t)v);
        else
            printf("level +0x%02x 0x%08x\n", k, (unsigned)v);
    }

    base = umk3_seq_addr(0xde242ul);
    printf("seq base aligned %d\n", base ? (int)((uintptr_t)base % 2 == 0) : -1);
    for (a = 0xde240ul; a < 0xde302ul; a++) {
        const unsigned char *p = umk3_seq_addr(a);
        if (!p)
            printf("seq %lx -\n", a);
        else if (p != base + (a - 0xde242ul))
            printf("seq %lx MOVED\n", a);
        else
            printf("seq %lx %02x\n", a, *p);
    }
    printf("seq pad %02x\n", base ? base[0xde2ff - 0xde242] : 0xff);
    return 0;
}
"""


class LayoutTests(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp(prefix="umk3_layout_")
        cls.bin = os.path.join(cls.tmp, "synth.bin")
        with open(cls.bin, "wb") as f:
            f.write(fat(synthetic()))
        cls.outs = {}
        for mode in ("default", "layout"):
            c = os.path.join(cls.tmp, "logic_%s.c" % mode)
            rep = os.path.join(cls.tmp, "report_%s.txt" % mode)
            args = [cls.bin, c, "--report", rep] + (["--layout"] if mode == "layout" else [])
            r = run_tool("logic_tables.py", *args)
            assert r.returncode == 0, r.stderr
            cls.outs[mode] = (read(c), read(rep))
        for script in ("level_info.py", "seq_data.py"):
            r = run_tool(script, cls.bin, os.path.join(cls.tmp, script[:-3] + ".c"))
            assert r.returncode == 0, r.stderr

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    # -- the generator's text

    def test_default_output_unchanged(self):
        self.assertEqual(self.outs["default"][0], GOLDEN_DEFAULT)

    def test_report_same_in_both_modes(self):
        self.assertEqual(self.outs["default"][1], self.outs["layout"][1])
        self.assertIn("int!  ochar_headrip_lineups", self.outs["layout"][1])

    def test_same_definitions(self):
        pat = re.compile(r"^(uintptr_t|unsigned char) (\w+)\[(\d+)\] UMK3_LOGIC"
                         r"(?: __attribute__\(\(aligned\((\d+)\)\)\))? = \{", re.M)
        default = pat.findall(self.outs["default"][0])
        layout = pat.findall(self.outs["layout"][0])
        # the layout writes out the 4 a uintptr_t array is aligned to anyway
        self.assertEqual([d[:3] + ("4",) if d[0] == "uintptr_t" else d
                          for d in default], layout)
        self.assertEqual([d[1] for d in default],
                         [o[0] for o in OBJECTS] + [c[0] for c in COMMON])
        ext = re.compile(r"^extern (?:uintptr_t|unsigned char|char) \w+\[\];$", re.M)
        self.assertEqual(ext.findall(self.outs["default"][0])[2:],   # [2:]: the functions
                         ext.findall(self.outs["layout"][0]))
        self.assertEqual(re.findall(r"^UMK3_LOGIC_FN\((\w+)\);$", self.outs["layout"][0], re.M),
                         ["fn_alpha", "fn_beta"])
        self.assertIn(SECTION_BLOCK, self.outs["layout"][0])

    def test_descriptor(self):
        text = self.outs["layout"][0]
        objs = re.findall(r'^    \{ "(\w+)", (\w+), 0x([0-9a-f]{8}), (\d+), (\d), (\d+), (\d+) \},$',
                          text, re.M)
        relocs = re.findall(r"^    \{ 0x([0-9a-f]+), 0x([0-9a-f]{8}), (.*) \},$", text, re.M)
        first = 0
        want_objs = []
        for name, a, size, w in OBJECTS:
            n = sum(1 for r in RELOCS if r[0] == name)
            want_objs.append((name, name, "%08x" % a, str(size), str(w), str(first), str(n)))
            first += n
        self.assertEqual(objs, want_objs)
        self.assertEqual(relocs, [("%x" % k, "%08x" % v, e) for _, k, v, e in RELOCS])
        self.assertIn("    \"%s\",\n    umk3_logic_objects, %d,\n    umk3_logic_relocs, %d,"
                      % (UUID.hex(), len(OBJECTS), len(RELOCS)), text)

    def test_layout_holds_no_content(self):
        text = self.outs["layout"][0]
        numbers = {int(x, 0) for x in re.findall(r"\b(0x[0-9a-f]+|\d+)\b", text)}
        for w in CONTENT_WORDS + [0x3f800000, 0x41200000]:
            self.assertNotIn(w, numbers)
        for blob in (ODD5, ODD3, RAW6, RAW2):
            self.assertNotIn(c_bytes(blob), text)
        self.assertNotIn("pit.scene", text)
        # and the default output does carry them, so the check can see them
        self.assertIn("0xc0ffee11", self.outs["default"][0])
        self.assertIn(c_bytes(RAW6), self.outs["default"][0])

    def test_same_errors(self):
        cases = [(dict(ochar_word=0x12345678), "is not a relocation candidate"),
                 (dict(raw6=words(A_DATA)[:4] + b"VV"), "is not word-aligned")]
        for kw, want in cases:
            path = os.path.join(self.tmp, "bad.bin")
            with open(path, "wb") as f:
                f.write(synthetic(**kw))
            msgs = []
            for extra in ([], ["--layout"]):
                out = os.path.join(self.tmp, "bad.c")
                if os.path.exists(out):
                    os.remove(out)
                r = run_tool("logic_tables.py", path, out, *extra)
                self.assertNotEqual(r.returncode, 0)
                self.assertFalse(os.path.exists(out))
                msgs.append(r.stderr.strip().splitlines()[-1])
            self.assertEqual(msgs[0], msgs[1])
            self.assertIn(want, msgs[0])

    # -- the state a program is left in

    def build_and_run(self, cc, sources, defines, exe, run, env=None, extra=()):
        cmd = [cc, "-std=gnu11", "-Wall", "-Wextra", "-O2",
               "-I", os.path.join(ROOT, "runtime")] + list(extra) + defines + \
              sources + ["-o", exe]
        r = subprocess.run(cmd, capture_output=True, text=True, env=env,
                           cwd=self.tmp, timeout=600)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        # a fill that loops is a failure, not a test that never ends
        r = subprocess.run(run, capture_output=True, text=True, env=env,
                           cwd=self.tmp, timeout=60)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        return r.stdout, r.stderr

    def write(self, name, text):
        path = os.path.join(self.tmp, name)
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
        return path

    def default_print(self, cc):
        h = self.write("harness_default.c", harness(False))
        exe = os.path.join(self.tmp, "default" + EXE)
        out, _ = self.build_and_run(
            cc, [h, os.path.join(self.tmp, "logic_default.c"),
                 os.path.join(self.tmp, "level_info.c"),
                 os.path.join(self.tmp, "seq_data.c")], [], exe, [exe])
        return out

    def test_fill_matches_compiled_tables(self):
        cc = find_cc32()
        if not cc:
            self.skipTest("no 32-bit compiler (set UMK3_CC32)")
        default = self.default_print(cc)
        h = self.write("harness_layout.c", harness(True))
        exe = os.path.join(self.tmp, "layout" + EXE)
        layout, warn = self.build_and_run(
            cc, [h, os.path.join(self.tmp, "logic_layout.c"),
                 os.path.join(ROOT, "runtime", "umk3_tables_rt.c")], [], exe,
            [exe, self.bin])
        self.assertEqual(layout, default)
        self.assertIn("obj tbl_a W 16: fn_alpha 0x200000 tbl_b+0x4 umk3_common_G+0x8",
                      layout)
        self.assertIn('level +0x3c -> pool+12 "urch.scene"', layout)
        self.assertNotIn("BROKEN", layout)
        self.assertNotIn("MOVED", layout)
        self.assertIn("seq pad 00", layout)

    def test_fill_matches_compiled_tables_wasm(self):
        cc, tools = find_cc32(), find_emcc()
        if not cc or not tools:
            self.skipTest("needs a 32-bit compiler and Emscripten "
                          "(UMK3_CC32, UMK3_EMCC, UMK3_NODE)")
        emcc, node, env = tools
        default = self.default_print(cc)
        h = self.write("harness_wasm.c", harness(True))
        js = os.path.join(self.tmp, "layout.js")
        layout, _ = self.build_and_run(
            emcc, [h, os.path.join(self.tmp, "logic_layout.c"),
                   os.path.join(ROOT, "runtime", "umk3_tables_rt.c")], [], js,
            [node, js, "synth.bin"], env=env, extra=["-sNODERAWFS=1"])
        self.assertEqual(layout, default)


SECTION_BLOCK = """#if defined(__APPLE__)
#define UMK3_LOGIC __attribute__((section("__DATA,__umk3logic")))
#elif defined(__wasm__)
#define UMK3_LOGIC  /* no named sections: order is not guaranteed */
#else
#define UMK3_LOGIC __attribute__((section(".data.umk3logic")))
#endif"""


if __name__ == "__main__":
    unittest.main()

/*
 * test_tables_rt.c -- runtime/umk3_tables_rt.c against synthetic Mach-O files.
 *
 * The filler reads the player's binary in the browser build, so it has to be
 * right without one: this test builds its own, in memory -- a 32-bit armv7
 * Mach-O with the shape the real one has where the filler looks (sections in
 * two LC_SEGMENTs, a zero-fill __common, LC_UUID, LC_SYMTAB with stabs and
 * duplicate names, `_CurrentScene` / pool / `_Level_Info` / `_LevelMusic` in
 * __DATA,__data, the playback scripts from 0x000de242 to the end of
 * __TEXT,__const), thin and inside a fat file. None of its bytes are the
 * game's. Then:
 *
 *   - a thin and a fat file fill the storage: words copied as numbers,
 *     relocated words replaced, an unaligned byte object copied exactly,
 *     Level_Info's pool pointers rewritten (and nothing else of it), and
 *     umk3_seq_addr mapping the scripts byte for byte, odd last byte padded;
 *   - level_info.py's first-of-a-name rule holds: a second `_LevelMusic`
 *     inside Level_Info and a second `_CurrentScene` inside the pool are
 *     ignored, as `syms.setdefault` ignores them;
 *   - ~40 malformed files and layouts are refused with a message, and a
 *     refused fill writes nothing;
 *   - every truncation of both files is refused, and 20,000 seeded random
 *     corruptions run without a crash (build it with -fsanitize=address under
 *     Emscripten to have the reads checked, as below).
 *
 * Build and run, from the repository root (32-bit compilers only, like the
 * filler):
 *
 *   toolchain/llvm-mingw-20260616-ucrt-x86_64/bin/i686-w64-mingw32-clang \
 *       -std=gnu11 -Wall -Wextra -O2 -I runtime \
 *       -o build/test_tables_rt.exe tests/test_tables_rt.c runtime/umk3_tables_rt.c
 *   build/test_tables_rt.exe
 *
 *   emcc -std=gnu11 -Wall -Wextra -O1 -fsanitize=address -sALLOW_MEMORY_GROWTH=1 \
 *       -I runtime -o build/test_tables_rt.js \
 *       tests/test_tables_rt.c runtime/umk3_tables_rt.c
 *   node build/test_tables_rt.js
 *
 * (Memory growth because AddressSanitizer's quarantine outgrows the default
 * 16 MB over the 20,000 corruptions; the browser build links with it too.)
 *
 * Exit status 0 and "RESULT: all passed" when every check holds.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "umk3_tables_rt.h"

/* ---------------------------------------------------------------- checks */

static int g_checks, g_failed;

#define CHECK(cond) check((cond), #cond, __LINE__)

static void check(int ok, const char *what, int line)
{
    g_checks++;
    if (!ok) {
        g_failed++;
        printf("  FAIL line %d: %s\n", line, what);
    }
}

/* ------------------------------------------------- the synthetic binary */

/* Load commands: the header, two LC_SEGMENTs of two sections, LC_UUID and
 * LC_SYMTAB, at these offsets. */
#define O_SEGTEXT      0x01c
#define O_SECT_TEXT    (O_SEGTEXT + 56)
#define O_SECT_CONST   (O_SECT_TEXT + 68)
#define O_SEGDATA      (O_SECT_CONST + 68)
#define O_SECT_DATA    (O_SEGDATA + 56)
#define O_SECT_COMMON  (O_SECT_DATA + 68)
#define O_UUID         (O_SECT_COMMON + 68)
#define O_SYMTAB       (O_UUID + 24)
#define O_CMDS_END     (O_SYMTAB + 24)

/* Where the sections are, in memory and in the file. */
#define A_TEXT     0x00002000u
#define SZ_TEXT    0x20u
#define F_TEXT     0x200u
#define A_CONST    0x000de200u    /* holds SEQ_LO, 0x000de242 */
#define SZ_CONST   0x7fu          /* so the scripts are 61 bytes: odd */
#define F_CONST    0x220u
#define A_DATA     0x00100000u
#define SZ_DATA    0xd0u
#define F_DATA     0x2a0u
#define A_COMMON   0x00100100u
#define SZ_COMMON  0x40u
#define F_SYMS     0x370u
#define NSYMS      13
#define F_STR      (F_SYMS + NSYMS * 12)

/* In __DATA,__data. */
#define A_W        0x00100000u    /* 4 words, two of them pointers */
#define A_X        0x00100020u    /* 2 words */
#define A_SCENE    0x00100040u    /* _CurrentScene */
#define A_POOL     0x00100044u    /* the anonymous strings */
#define A_INFO     0x00100080u    /* _Level_Info, 16 words */
#define A_MUSIC    0x001000c0u    /* _LevelMusic */
/* In __TEXT,__const. */
#define A_R        0x000de201u    /* 5 bytes at an odd address */
#define SEQ_LO     0x000de242u
#define SEQ_HI     (A_CONST + SZ_CONST)

#define IMG_CAP    0x2000

static const unsigned char UUID_BYTES[16] = {
    0x90, 0xd6, 0xf5, 0x6a, 0x18, 0xe2, 0x30, 0x3f,
    0x8f, 0x20, 0x53, 0xb0, 0x2a, 0x42, 0x74, 0x2e,
};

static const char POOL[] = "pit.lime\0church.lime\0sky.pvr";   /* 29 bytes */

/* Level_Info's words. Those in [A_POOL, A_INFO) are the pointers. */
static const uint32_t INFO_WORDS[16] = {
    0x3f800000u, A_POOL + 0x00, A_POOL + 0x09, 0x00000007u,
    A_POOL + 0x15, A_INFO,        A_POOL - 1,    A_INFO - 1,
    0x41200000u, 0,               0,             0,
    0,           0,               0,             A_POOL + 0x0c,
};

static void put32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}

static void put32be(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)(v >> 24);
    p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);
    p[3] = (unsigned char)v;
}

static void put_name(unsigned char *p, const char *s)
{
    memset(p, 0, 16);
    memcpy(p, s, strlen(s));
}

static void put_section(unsigned char *q, const char *seg, const char *sect,
                        uint32_t addr, uint32_t size, uint32_t off,
                        uint32_t flags)
{
    put_name(q, sect);
    put_name(q + 16, seg);
    put32(q + 32, addr);
    put32(q + 36, size);
    put32(q + 40, off);
    put32(q + 44, 2);             /* align */
    put32(q + 56, flags);
}

static unsigned char *data_at(unsigned char *b, uint32_t addr)
{
    return b + F_DATA + (addr - A_DATA);
}

/* nlist i, and the string table offset its name went to. */
static size_t g_strx[NSYMS];

static size_t build_thin(unsigned char *b)
{
    static const struct { const char *name; uint8_t type, sect; uint16_t desc;
                          uint32_t value; } syms[NSYMS] = {
        { "/src/gamecode/logic/mk3.c", 0x64, 0, 0, 0 },  /* N_SO: a stab */
        { "_Level_Info",   0x0f, 3, 0, A_INFO },
        { "_CurrentScene", 0x0f, 3, 0, A_SCENE },
        { "_LevelMusic",   0x0f, 3, 0, A_MUSIC },
        { "_W",            0x0f, 3, 0, A_W },
        { "_X",            0x0f, 3, 0, A_X },
        { "_fn",           0x0f, 1, 8, A_TEXT },
        /* the second of a name: level_info.py's setdefault ignores both */
        { "_LevelMusic",   0x0e, 3, 0, A_INFO + 0x20 },
        { "_CurrentScene", 0x0e, 3, 0, A_POOL + 0x0c },
        /* a stab and an undefined symbol: not N_SECT, ignored */
        { "_Level_Info",   0x26, 3, 0, A_INFO + 0x10 },
        { "_printf",       0x01, 0, 0, 0 },
        { "_R",            0x0e, 2, 0, A_R },
        { "_G",            0x0f, 4, 0, A_COMMON },
    };
    size_t   str = 1, i;
    unsigned k;

    memset(b, 0, IMG_CAP);

    put32(b + 0, 0xfeedfaceu);
    put32(b + 4, 12);                 /* CPU_TYPE_ARM */
    put32(b + 8, 9);                  /* armv7 */
    put32(b + 12, 2);                 /* MH_EXECUTE */
    put32(b + 16, 4);                 /* ncmds */
    put32(b + 20, O_CMDS_END - O_SEGTEXT);
    put32(b + 24, 0x00200085u);

    put32(b + O_SEGTEXT, 0x01);
    put32(b + O_SEGTEXT + 4, 56 + 2 * 68);
    put_name(b + O_SEGTEXT + 8, "__TEXT");
    put32(b + O_SEGTEXT + 24, 0);
    put32(b + O_SEGTEXT + 28, 0x00100000u);
    put32(b + O_SEGTEXT + 48, 2);
    put_section(b + O_SECT_TEXT, "__TEXT", "__text", A_TEXT, SZ_TEXT, F_TEXT,
                0x80000400u);
    put_section(b + O_SECT_CONST, "__TEXT", "__const", A_CONST, SZ_CONST,
                F_CONST, 0);

    put32(b + O_SEGDATA, 0x01);
    put32(b + O_SEGDATA + 4, 56 + 2 * 68);
    put_name(b + O_SEGDATA + 8, "__DATA");
    put32(b + O_SEGDATA + 24, A_DATA);
    put32(b + O_SEGDATA + 28, 0x200);
    put32(b + O_SEGDATA + 48, 2);
    put_section(b + O_SECT_DATA, "__DATA", "__data", A_DATA, SZ_DATA, F_DATA, 0);
    put_section(b + O_SECT_COMMON, "__DATA", "__common", A_COMMON, SZ_COMMON,
                0, 0x01);         /* S_ZEROFILL */

    put32(b + O_UUID, 0x1b);
    put32(b + O_UUID + 4, 24);
    memcpy(b + O_UUID + 8, UUID_BYTES, 16);

    /* __text: anything; __TEXT,__const: R, then the scripts */
    memset(b + F_TEXT, 0x00, SZ_TEXT);
    memcpy(b + F_CONST + (A_R - A_CONST), "RAW!?", 5);
    for (k = 0; k < SEQ_HI - SEQ_LO; k++)
        b[F_CONST + (SEQ_LO - A_CONST) + k] = (unsigned char)(k * 37 + 11);

    /* __data */
    put32(data_at(b, A_W + 0), 0xa1a2a3a4u);
    put32(data_at(b, A_W + 4), A_X);              /* -> X */
    put32(data_at(b, A_W + 8), 0xb1b2b3b4u);
    put32(data_at(b, A_W + 12), A_TEXT | 1);      /* -> fn, Thumb */
    put32(data_at(b, A_X + 0), 0xc1c2c3c4u);
    put32(data_at(b, A_X + 4), 0xd1d2d3d4u);
    memcpy(data_at(b, A_POOL), POOL, sizeof POOL);
    for (k = 0; k < 16; k++)
        put32(data_at(b, A_INFO + 4 * k), INFO_WORDS[k]);
    memcpy(data_at(b, A_MUSIC), "Pit.mp3", 8);

    /* LC_SYMTAB */
    for (i = 0; i < NSYMS; i++) {
        unsigned char *e = b + F_SYMS + i * 12;
        size_t n = strlen(syms[i].name) + 1;

        g_strx[i] = str;
        memcpy(b + F_STR + str, syms[i].name, n);
        put32(e, (uint32_t)str);
        e[4] = syms[i].type;
        e[5] = syms[i].sect;
        e[6] = (unsigned char)syms[i].desc;
        e[7] = (unsigned char)(syms[i].desc >> 8);
        put32(e + 8, syms[i].value);
        str += n;
    }
    put32(b + O_SYMTAB, 0x02);
    put32(b + O_SYMTAB + 4, 24);
    put32(b + O_SYMTAB + 8, F_SYMS);
    put32(b + O_SYMTAB + 12, NSYMS);
    put32(b + O_SYMTAB + 16, F_STR);
    put32(b + O_SYMTAB + 20, (uint32_t)str);
    return F_STR + str;
}

/* A fat file: an x86_64 slice and an armv7s slice of junk ahead of the armv7
 * one, which tools/logic_tables.py's load() -- the first cputype 12 subtype 9
 * -- has to find past them. */
#define FAT_SLICE 0x1000u

static size_t build_fat(unsigned char *f, const unsigned char *thin, size_t n)
{
    memset(f, 0, FAT_SLICE + n);
    put32be(f, 0xcafebabeu);
    put32be(f + 4, 3);
    put32be(f + 8, 0x01000007u);  put32be(f + 12, 3);
    put32be(f + 16, 0x40);        put32be(f + 20, 0x20);   put32be(f + 24, 4);
    put32be(f + 28, 12);          put32be(f + 32, 11);     /* armv7s */
    put32be(f + 36, 0x60);        put32be(f + 40, 0x20);   put32be(f + 44, 4);
    put32be(f + 48, 12);          put32be(f + 52, 9);      /* armv7 */
    put32be(f + 56, FAT_SLICE);   put32be(f + 60, (uint32_t)n);
    put32be(f + 64, 12);
    memset(f + 0x40, 0xcc, 0x40);
    memcpy(f + FAT_SLICE, thin, n);
    return FAT_SLICE + n;
}

/* --------------------------------------------- the layout under test */

static void test_fn(void) {}

static uintptr_t     W_store[4];
static uintptr_t     X_store[2];
static unsigned char R_guard[7];          /* R is R_guard[1..5] */

static const UMK3_RT_RELOC t_relocs[] = {
    { 1, A_X,          (uintptr_t)X_store },
    { 3, A_TEXT | 1,   (uintptr_t)test_fn },
};

static const UMK3_RT_OBJECT t_objects[] = {
    { "R", R_guard + 1, A_R, 5,  0, 0, 0 },
    { "W", W_store,     A_W, 16, 1, 0, 2 },
    { "X", X_store,     A_X, 8,  1, 2, 0 },
};

const UMK3_RT_LAYOUT umk3_logic_layout = {
    UMK3_RT_UUID, t_objects, 3, t_relocs, 2
};

/* The game's Level_Info: runtime/gamecode_globals.c's size. */
char Level_Info[0xff4];

static void reset_storage(void)
{
    memset(W_store, 0, sizeof W_store);
    memset(X_store, 0, sizeof X_store);
    memset(R_guard, 0xee, sizeof R_guard);
    memset(Level_Info, 0x5a, sizeof Level_Info);
}

static int storage_untouched(void)
{
    static const uintptr_t zero[4];
    unsigned i;

    if (memcmp(W_store, zero, sizeof W_store) || memcmp(X_store, zero, sizeof X_store))
        return 0;
    for (i = 0; i < sizeof R_guard; i++)
        if (R_guard[i] != 0xee)
            return 0;
    return 1;
}

static const char *string_at(int off)
{
    uint32_t v;

    memcpy(&v, Level_Info + off, 4);
    return (const char *)(uintptr_t)v;
}

/* What a fill of the synthetic file must leave. */
static void check_state(const unsigned char *img)
{
    static const unsigned char raw[7] = { 0xee, 'R', 'A', 'W', '!', '?', 0xee };
    static const int reloc_at[] = { 4, 8, 16, 28, 60 };
    const unsigned char *base;
    const char *p4;
    unsigned i, k;

    CHECK(W_store[0] == 0xa1a2a3a4u);
    CHECK(W_store[1] == (uintptr_t)X_store);
    CHECK(W_store[2] == 0xb1b2b3b4u);
    CHECK(W_store[3] == (uintptr_t)test_fn);
    CHECK(X_store[0] == 0xc1c2c3c4u);
    CHECK(X_store[1] == 0xd1d2d3d4u);
    CHECK(memcmp(R_guard, raw, sizeof raw) == 0);

    /* Level_Info: the pointers into the pool, and not a byte else. */
    memset(Level_Info, 0x5a, sizeof Level_Info);
    umk3_relocate_level_info();
    p4 = string_at(4);
    CHECK(strcmp(p4, "pit.lime") == 0);
    CHECK(strcmp(string_at(8), "church.lime") == 0);
    CHECK(strcmp(string_at(16), "sky.pvr") == 0);
    CHECK(string_at(8) - p4 == 0x09);
    CHECK(string_at(16) - p4 == 0x15);
    CHECK(string_at(28) - p4 == 0x3b);      /* the pool's last byte */
    CHECK(*string_at(28) == '\0');
    CHECK(string_at(60) - p4 == 0x0c);      /* past _LevelMusic's twin */
    CHECK(strcmp(string_at(60), "rch.lime") == 0);
    for (i = 0; i < sizeof Level_Info; i++) {
        int relocated = 0;

        for (k = 0; k < sizeof reloc_at / sizeof reloc_at[0]; k++)
            if (i >= (unsigned)reloc_at[k] && i < (unsigned)reloc_at[k] + 4)
                relocated = 1;
        if (!relocated && (unsigned char)Level_Info[i] != 0x5a) {
            CHECK(!"Level_Info byte written that is no pool pointer");
            break;
        }
    }

    /* The scripts. */
    base = umk3_seq_addr(SEQ_LO);
    CHECK(base != NULL);
    CHECK(umk3_seq_addr(SEQ_LO - 1) == NULL);
    CHECK(umk3_seq_addr(SEQ_HI) == NULL);
    CHECK(umk3_seq_addr(SEQ_HI - 1) != NULL);
    CHECK(umk3_seq_addr(0) == NULL);
    CHECK(umk3_seq_addr(0xfffffffful) == NULL);
    if (base) {
        CHECK((uintptr_t)base % 2 == 0);
        for (k = SEQ_LO; k < SEQ_HI; k++) {
            const unsigned char *p = umk3_seq_addr(k);

            if (p != base + (k - SEQ_LO) || *p != img[F_CONST + (k - A_CONST)]) {
                CHECK(!"script byte differs from the file");
                break;
            }
        }
        CHECK(base[SEQ_HI - SEQ_LO] == 0);  /* the odd byte's zero pad */
    }
}

/* ------------------------------------------------------------ rejections */

static unsigned char g_img[IMG_CAP], g_mut[IMG_CAP + FAT_SLICE];
static size_t        g_len;

static void expect_fail(const char *label, const unsigned char *f, size_t n,
                        const UMK3_RT_LAYOUT *L, const char *want)
{
    char err[300];
    int  rc;

    memset(err, 'x', sizeof err);
    rc = umk3_tables_fill_layout(L ? L : &umk3_logic_layout, f, n, err,
                                 sizeof err);
    printf("  %-34s -> %s\n", label, err);
    CHECK(rc == -1);
    CHECK(memchr(err, 0, sizeof err) != NULL);
    CHECK(strstr(err, want) != NULL);
    CHECK(storage_untouched());
}

/* g_mut = g_img with one 32-bit field changed */
static const unsigned char *patched(size_t off, uint32_t v)
{
    memcpy(g_mut, g_img, g_len);
    put32(g_mut + off, v);
    return g_mut;
}

static const unsigned char *renamed(size_t off, const char *name)
{
    memcpy(g_mut, g_img, g_len);
    put_name(g_mut + off, name);
    return g_mut;
}

static void reject_files(void)
{
    unsigned char *m;

    expect_fail("3 bytes", g_img, 3, NULL, "too short");
    expect_fail("bad magic", patched(0, 0x12345678u), g_len, NULL,
                "not a Mach-O");
    expect_fail("64-bit magic", patched(0, 0xfeedfacfu), g_len, NULL, "64-bit");
    expect_fail("x86 cputype", patched(4, 7), g_len, NULL, "expected armv7");
    expect_fail("armv7s subtype", patched(8, 11), g_len, NULL, "expected armv7");
    expect_fail("sizeofcmds too big", patched(20, 0x00ffffffu), g_len, NULL,
                "load commands");
    expect_fail("ncmds past sizeofcmds", patched(16, 100), g_len, NULL,
                "past sizeofcmds");
    expect_fail("cmdsize 4", patched(O_SEGTEXT + 4, 4), g_len, NULL, "size 4");
    expect_fail("cmdsize past the end", patched(O_SEGTEXT + 4, 0x10000), g_len,
                NULL, "size 65536");
    expect_fail("nsects past cmdsize", patched(O_SEGTEXT + 48, 1000), g_len,
                NULL, "do not fit");
    expect_fail("LC_SYMTAB short", patched(O_SYMTAB + 4, 16), g_len, NULL,
                "LC_SYMTAB: 16 bytes");

    /* identity */
    m = (unsigned char *)patched(0, 0xfeedfaceu);
    m[O_UUID + 8 + 15] ^= 1;
    expect_fail("uuid differs", m, g_len, NULL, "binary uuid");
    expect_fail("no LC_UUID", patched(O_UUID, 0x99), g_len, NULL, "no LC_UUID");
    /* the uuid command read as LC_ENCRYPTION_INFO: its cryptid word is uuid
     * bytes 8..11, non-zero -- an App Store copy never decrypted */
    expect_fail("encrypted", patched(O_UUID, 0x21), g_len, NULL, "still encrypted");

    /* logic objects */
    expect_fail("relocated word differs", patched(F_DATA + 4, A_X + 4), g_len,
                NULL, "W+0x4: the binary holds 0x00100024");
    expect_fail("thumb pointer differs", patched(F_DATA + 12, A_TEXT), g_len,
                NULL, "W+0xc: the binary holds 0x00002000");
    expect_fail("__data offset outside", patched(O_SECT_DATA + 40, 0x7fff0000u),
                g_len, NULL, "outside the");
    expect_fail("__const truncated", patched(O_SECT_CONST + 36, 0), g_len,
                NULL, "R: 0x000de201 is in no section");

    /* LC_SYMTAB and level_info */
    expect_fail("no LC_SYMTAB", patched(O_SYMTAB, 0x98), g_len, NULL,
                "no LC_SYMTAB");
    expect_fail("nsyms too many", patched(O_SYMTAB + 12, 0x01000000u), g_len,
                NULL, "symbols at");
    expect_fail("string table outside", patched(O_SYMTAB + 16, 0x7fff0000u),
                g_len, NULL, "string table");
    expect_fail("strx outside", patched(F_SYMS + 1 * 12, 0x00100000u), g_len,
                NULL, "symbol 1: its name");
    m = (unsigned char *)patched(0, 0xfeedfaceu);
    memcpy(m + F_STR + g_strx[1], "_Level_Infx", 11);
    expect_fail("no _Level_Info", m, g_len, NULL, "no _Level_Info");
    m = (unsigned char *)patched(0, 0xfeedfaceu);
    memcpy(m + F_STR + g_strx[2], "_CurrentScenX", 13);   /* and its twin */
    memcpy(m + F_STR + g_strx[8], "_CurrentScenX", 13);
    expect_fail("no _CurrentScene", m, g_len, NULL, "no _CurrentScene");
    expect_fail("a first symbol in the pool", patched(F_SYMS + 5 * 12 + 8,
                A_POOL + 4), g_len, NULL, "a symbol inside the string pool (_X");
    expect_fail("_CurrentScene after Level_Info", patched(F_SYMS + 2 * 12 + 8,
                A_INFO), g_len, NULL, "does not end before");
    expect_fail("Level_Info not in __data", renamed(O_SECT_DATA, "__datb"),
                g_len, NULL, "not in __DATA,__data");
    m = (unsigned char *)patched(0, 0xfeedfaceu);
    put32(m + F_DATA + (A_INFO - A_DATA) + 4, 0);
    put32(m + F_DATA + (A_INFO - A_DATA) + 8, 0);
    put32(m + F_DATA + (A_INFO - A_DATA) + 16, 0);
    put32(m + F_DATA + (A_INFO - A_DATA) + 28, 0);
    put32(m + F_DATA + (A_INFO - A_DATA) + 60, 0);
    expect_fail("no word into the pool", m, g_len, NULL,
                "no Level_Info word points into the pool");
    /* With _LevelMusic and _G moved below it, only _LevelMusic's twin is
     * above Level_Info -- and the twin does not count. */
    m = (unsigned char *)patched(F_SYMS + 3 * 12 + 8, 0x10);
    put32(m + F_SYMS + 12 * 12 + 8, 0x10);
    expect_fail("nothing after Level_Info", m, g_len, NULL,
                "no symbol after _Level_Info");
    m = (unsigned char *)patched(F_SYMS + 3 * 12 + 8, A_INFO + 0x1000);
    put32(m + F_SYMS + 12 * 12 + 8, 0x10);
    expect_fail("Level_Info too big", m, g_len, NULL, "more than the 0xff4");
    expect_fail("Level_Info past __data", patched(F_SYMS + 3 * 12 + 8,
                A_COMMON + 0x10), g_len, NULL,
                "_Level_Info: 0x001000d0 is in no section");

    /* seq_data */
    expect_fail("scripts not in __const", renamed(O_SECT_CONST, "__cstring"),
                g_len, NULL, "0x000de242 is not in __TEXT,__const");
}

static void reject_fat(const unsigned char *fat, size_t fatlen)
{
    memcpy(g_mut, fat, fatlen);
    put32be(g_mut + 52, 6);               /* armv7 -> armv6: none left */
    expect_fail("fat without armv7", g_mut, fatlen, NULL, "no armv7 slice");
    memcpy(g_mut, fat, fatlen);
    put32be(g_mut + 60, (uint32_t)fatlen);
    expect_fail("fat slice past the end", g_mut, fatlen, NULL, "runs past");
    memcpy(g_mut, fat, fatlen);
    put32be(g_mut + 56, 0xfffffff0u);
    expect_fail("fat slice offset huge", g_mut, fatlen, NULL, "runs past");
    memcpy(g_mut, fat, fatlen);
    put32be(g_mut + 4, 0x10000000u);
    expect_fail("fat count huge", g_mut, fatlen, NULL, "slices do not fit");
    expect_fail("fat header cut", fat, 6, NULL, "fat header: truncated");
}

static void reject_layouts(void)
{
    UMK3_RT_OBJECT  o[3];
    UMK3_RT_RELOC   r[2];
    UMK3_RT_LAYOUT  L;

#define FRESH() (memcpy(o, t_objects, sizeof o), memcpy(r, t_relocs, sizeof r), \
                 L.uuid = UMK3_RT_UUID, L.objects = o, L.nobjects = 3,          \
                 L.relocs = r, L.nrelocs = 2)

    FRESH();
    L.uuid = "00000000000000000000000000000000";
    expect_fail("layout of another binary", g_img, g_len, &L,
                "the layout was made from uuid 0000");
    FRESH();
    o[2].vmaddr = 0x00500000u;
    expect_fail("object in no section", g_img, g_len, &L, "X: 0x00500000 is in no");
    FRESH();
    o[2].vmaddr = A_DATA + SZ_DATA - 4;
    expect_fail("object past its section", g_img, g_len, &L, "runs past the end");
    FRESH();
    o[2].vmaddr = A_COMMON;
    expect_fail("object in zero-fill", g_img, g_len, &L, "zero-fill");
    FRESH();
    r[0].word = 4;
    expect_fail("reloc past the object", g_img, g_len, &L, "W word 4 is past");
    FRESH();
    o[0].nrelocs = 1;
    expect_fail("bytes with a reloc", g_img, g_len, &L, "R is bytes");
    FRESH();
    o[2].first_reloc = 2;
    o[2].nrelocs = 1;
    expect_fail("relocs outside the list", g_img, g_len, &L, "outside the 2");
    FRESH();
    o[2].size = 6;
    expect_fail("words of 6 bytes", g_img, g_len, &L, "not whole words");
    FRESH();
    o[2].words = 2;
    expect_fail("words flag 2", g_img, g_len, &L, "words is 2");
    FRESH();
    o[1].dst = NULL;
    expect_fail("no storage", g_img, g_len, &L, "W has no storage");
    FRESH();
    L.relocs = NULL;
    expect_fail("no reloc table", g_img, g_len, &L, "a table is missing");
#undef FRESH
    {
        char err[100];

        CHECK(umk3_tables_fill_layout(NULL, g_img, g_len, err, sizeof err) == -1);
        CHECK(strcmp(err, "no layout") == 0);
        CHECK(storage_untouched());
    }

    /* Objects that touch in the image (W, and 8 bytes straight after it)
     * must touch in memory: refused 4 bytes apart, filled when adjacent. */
    {
        static uintptr_t pair[8];
        UMK3_RT_OBJECT   two[2];
        UMK3_RT_LAYOUT   L2;
        char             err[300];

        two[0] = t_objects[1];
        two[0].dst = pair;
        two[1] = t_objects[2];
        two[1].name = "V";
        two[1].vmaddr = A_W + 16;
        two[1].dst = pair + 5;
        L2.uuid = UMK3_RT_UUID;
        L2.objects = two;
        L2.nobjects = 2;
        L2.relocs = t_relocs;
        L2.nrelocs = 2;
        expect_fail("touching objects kept apart", g_img, g_len, &L2,
                    "W and V touch in the binary but lie 20 bytes apart");
        CHECK(pair[0] == 0);
        two[1].dst = pair + 4;
        CHECK(umk3_tables_fill_layout(&L2, g_img, g_len, err, sizeof err) == 0);
        CHECK(pair[0] == 0xa1a2a3a4u && pair[1] == (uintptr_t)X_store);
        CHECK(pair[2] == 0xb1b2b3b4u && pair[3] == (uintptr_t)test_fn);
        CHECK(pair[4] == 0 && pair[5] == 0 && pair[6] == 0);
        CHECK(storage_untouched());
    }
}

/* -------------------------------------------------------------- the rest */

static uint32_t g_seed = 0x2545f491u;

static uint32_t rnd(void)
{
    g_seed = g_seed * 1664525u + 1013904223u;
    return g_seed >> 8;
}

int main(void)
{
    static unsigned char fat[IMG_CAP + FAT_SLICE];
    char   err[300];
    size_t fatlen, n;
    int    i, rc, ok = 0;

    g_len = build_thin(g_img);
    fatlen = build_fat(fat, g_img, g_len);
    printf("synthetic armv7 Mach-O: %lu bytes thin, %lu fat\n",
           (unsigned long)g_len, (unsigned long)fatlen);

    /* Before any fill: nothing to relocate, no scripts. */
    reset_storage();
    umk3_relocate_level_info();
    CHECK(Level_Info[0] == 0x5a && Level_Info[sizeof Level_Info - 1] == 0x5a);
    CHECK(umk3_seq_addr(SEQ_LO) == NULL);

    printf("refused, writing nothing:\n");
    reset_storage();
    reject_files();
    reject_fat(fat, fatlen);
    reject_layouts();
    CHECK(umk3_tables_fill(g_img, g_len, NULL, 0) == 0);   /* err may be NULL */
    reset_storage();

    printf("thin file\n");
    rc = umk3_tables_fill(g_img, g_len, err, sizeof err);
    CHECK(rc == 0);
    CHECK(err[0] == '\0');
    check_state(g_img);

    printf("a refused refill leaves the tables as they were\n");
    W_store[0] = 0xdeadbeefu;
    rc = umk3_tables_fill(patched(F_DATA + 4, 0), g_len, err, sizeof err);
    CHECK(rc == -1);
    CHECK(W_store[0] == 0xdeadbeefu && W_store[1] == (uintptr_t)X_store);
    CHECK(strcmp(string_at(4), "pit.lime") == 0);   /* the pool is still there */

    printf("fat file\n");
    reset_storage();
    rc = umk3_tables_fill(fat, fatlen, err, sizeof err);
    CHECK(rc == 0);
    check_state(g_img);

    printf("file on disk\n");
    {
        FILE *f = fopen("test_tables_rt.bin", "wb");

        if (f) {
            fwrite(fat, 1, fatlen, f);
            fclose(f);
            reset_storage();
            CHECK(umk3_tables_fill_file("test_tables_rt.bin", err, sizeof err) == 0);
            check_state(g_img);
            remove("test_tables_rt.bin");
        }
        CHECK(umk3_tables_fill_file("no such file", err, sizeof err) == -1);
        CHECK(strstr(err, "cannot open") != NULL);
    }

    printf("every truncation refused\n");
    for (n = 0; n < g_len; n++)
        if (umk3_tables_fill(g_img, n, err, sizeof err) != -1) {
            CHECK(!"a truncated thin file was accepted");
            break;
        }
    for (n = 0; n < fatlen; n++)
        if (umk3_tables_fill(fat, n, err, sizeof err) != -1) {
            CHECK(!"a truncated fat file was accepted");
            break;
        }

    printf("20000 random corruptions\n");
    for (i = 0; i < 20000; i++) {
        int flips = 1 + (int)(rnd() % 4), k;

        memcpy(g_mut, g_img, g_len);
        for (k = 0; k < flips; k++) {
            /* half of them in the header and load commands */
            size_t at = (rnd() & 1) ? rnd() % O_CMDS_END : rnd() % g_len;

            g_mut[at] = (unsigned char)rnd();
        }
        if (umk3_tables_fill(g_mut, g_len, err, sizeof err) == 0)
            ok++;
    }
    printf("  %d of them still filled\n", ok);

    /* And the real file still fills afterwards. */
    reset_storage();
    CHECK(umk3_tables_fill(g_img, g_len, err, sizeof err) == 0);
    check_state(g_img);

    printf("%d checks, %d failed\n", g_checks, g_failed);
    printf("RESULT: %s\n", g_failed ? "FAIL" : "all passed");
    return g_failed ? 1 : 0;
}

/*
 * umk3_tables_rt.c -- the fight engine's tables, filled at run time from the
 * user's own binary. What and why: runtime/umk3_tables_rt.h.
 *
 * Three things come out of the binary here, each the run-time twin of a
 * build-time generator, and each follows that generator's rules rather than
 * inventing its own:
 *
 *   1. The ~1,100 logic objects (tools/logic_tables.py). The analysis -- which
 *      objects, how big, which words are pointers -- stays in Python and
 *      arrives as `umk3_logic_layout`; this copies the bytes and relocates the
 *      words it lists, after checking each against the value the layout
 *      recorded.
 *
 *   2. Level_Info's arena names (tools/level_info.py). `_Level_Info`
 *      (0x0014e8d4) holds iOS addresses of strings in the anonymous pool
 *      between `_CurrentScene` (0x0014e29c, 4 bytes) and itself. The pool and
 *      the list of words that point into it are found here exactly as the
 *      script finds them: the two symbols by name in LC_SYMTAB, Level_Info's
 *      size as the gap to the next symbol, a word relocated iff its value lies
 *      in the pool. That costs one pass over the symbol table and needs no
 *      second layout file -- the script's whole input is four numbers the
 *      symbol table already gives, and it names no host symbol a layout would
 *      have to link.
 *
 *   3. The playback scripts (tools/seq_data.py): 0x000de242 to the end of
 *      __TEXT,__const, no symbol, no pointers.
 *
 * ## Reading the file
 *
 * Everything is bounds-checked: a field that would send a read outside the
 * file, or the slice, is an error with a message, never a read. Addresses are
 * mapped to file offsets the way tools/macho.py maps them -- the first
 * LC_SEGMENT section, in load-command order, whose [addr, addr + size)
 * contains the address -- with two checks the Python can do without: the
 * whole range has to lie in that section, and a zero-fill section has no file
 * bytes to give.
 *
 * ## All or nothing
 *
 * The fill runs in two passes. The first parses, maps every range and checks
 * every relocated word, writing nothing; only when all of it holds does the
 * second copy. A binary that fails half-way therefore leaves the program as
 * it was, not half-loaded.
 *
 * The first pass also checks the build it runs in: two objects that touch in
 * the image have to touch in memory (see logic_check). A build that broke
 * that would load and then misbehave far from here, so it does not load.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "umk3_tables_rt.h"

/* Mach-O, as tools/macho.py reads it. */
#define FAT_MAGIC          0xcafebabeu   /* big-endian, as are its fat_arch records */
#define MH_MAGIC           0xfeedfaceu
#define MH_MAGIC_64        0xfeedfacfu
#define CPU_TYPE_ARM       12u
#define CPU_SUBTYPE_ARM_V7 9u
#define LC_SEGMENT         0x01u
#define LC_SYMTAB          0x02u
#define LC_UUID            0x1bu
#define LC_ENCRYPTION_INFO 0x21u
#define N_STAB             0xe0u
#define N_TYPE             0x0eu
#define N_SECT             0x0eu

#define MACH_HEADER_SIZE   28u           /* struct mach_header, 32-bit */
#define SEGMENT_CMD_SIZE   56u           /* struct segment_command */
#define SECTION_SIZE       68u           /* struct section */
#define NLIST_SIZE         12u           /* struct nlist, 32-bit */
#define FAT_ARCH_SIZE      20u

/* nlist's n_sect is one byte, so no symbol can name a section past 255. */
#define MAX_SECTS          255

/* `_Level_Info`, 0x0014e8d4: 16 records of 0xf4 bytes and a tail, 0xff4 bytes
 * up to `_LevelMusic` at 0x0014f8c8. runtime/gamecode_globals.c defines it as
 * `char Level_Info[4084]`. */
extern char Level_Info[];
#define LEVEL_INFO_SIZE    0xff4u

typedef struct SECT {
    unsigned char sectname[16], segname[16];
    uint32_t      addr, size, offset, flags;
} SECT;

typedef struct IMAGE {
    const unsigned char *p;     /* the armv7 Mach-O: the file, or its fat slice */
    size_t               len;
    SECT                 sect[MAX_SECTS];
    unsigned             nsect;
    int                  have_uuid, have_symtab;
    unsigned char        uuid[16];
    uint32_t             symoff, nsyms, stroff, strsize;
} IMAGE;

/* What level_info.c held: the pool, and per relocated word its offset in
 * Level_Info and the offset in the pool it points at. */
typedef struct LEVEL {
    char     *pool;
    uint32_t *off, *pool_off;
    uint32_t  n;
} LEVEL;

/* What seq_data.c held: the scripts as 16-bit words, [SEQ_LO, hi). */
typedef struct SEQ {
    uint16_t *words;
    uint32_t  hi;
} SEQ;

static LEVEL g_level;
static SEQ   g_seq;

/* ------------------------------------------------------------------ util */

#if defined(__GNUC__)
__attribute__((format(printf, 3, 4)))
#endif
static int fail(char *err, size_t errlen, const char *fmt, ...)
{
    va_list ap;

    if (err && errlen) {
        va_start(ap, fmt);
        vsnprintf(err, errlen, fmt, ap);
        va_end(ap);
        err[errlen - 1] = '\0';
    }
    return -1;
}

static uint32_t le32(const unsigned char *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint32_t be32(const unsigned char *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
           (uint32_t)p[2] << 8 | (uint32_t)p[3];
}

/* A Mach-O name is 16 bytes, NUL-padded and not always NUL-terminated.
 * macho.py strips trailing NULs and compares what is left, so a name matches
 * when its bytes are `want` followed by nothing but NULs. */
static int name_is(const unsigned char field[16], const char *want)
{
    size_t n = strlen(want), i;

    if (n > 16 || memcmp(field, want, n) != 0)
        return 0;
    for (i = n; i < 16; i++)
        if (field[i])
            return 0;
    return 1;
}

/* S_ZEROFILL, S_GB_ZEROFILL, S_THREAD_LOCAL_ZEROFILL: no bytes in the file. */
static int zerofill(const SECT *s)
{
    uint32_t type = s->flags & 0xffu;

    return type == 0x01u || type == 0x0cu || type == 0x12u;
}

/* ----------------------------------------------------------------- parse */

static int parse_segment(IMAGE *im, const unsigned char *c, uint32_t cmdsize,
                         char *err, size_t errlen)
{
    uint32_t nsects, j;

    if (cmdsize < SEGMENT_CMD_SIZE)
        return fail(err, errlen, "LC_SEGMENT: %u bytes, shorter than its header",
                    (unsigned)cmdsize);
    nsects = le32(c + 48);
    if (nsects > (cmdsize - SEGMENT_CMD_SIZE) / SECTION_SIZE)
        return fail(err, errlen, "LC_SEGMENT %.16s: %u sections do not fit in "
                    "its %u bytes", (const char *)c + 8, (unsigned)nsects,
                    (unsigned)cmdsize);
    for (j = 0; j < nsects; j++) {
        const unsigned char *q = c + SEGMENT_CMD_SIZE + (size_t)j * SECTION_SIZE;
        SECT *s;

        if (im->nsect == MAX_SECTS)
            return fail(err, errlen, "more than %d sections", MAX_SECTS);
        s = &im->sect[im->nsect++];
        memcpy(s->sectname, q, 16);
        memcpy(s->segname, q + 16, 16);   /* the section's own, as macho.py */
        s->addr   = le32(q + 32);
        s->size   = le32(q + 36);
        s->offset = le32(q + 40);
        s->flags  = le32(q + 56);
    }
    return 0;
}

/* The armv7 image in `file`: thin, or the first armv7 slice of a fat file
 * (tools/logic_tables.py's load()). Records its sections, LC_UUID and
 * LC_SYMTAB; checks nothing they point at yet. */
static int image_open(IMAGE *im, const unsigned char *file, size_t len,
                      char *err, size_t errlen)
{
    size_t   base = 0, size = len, off, end;
    uint32_t magic, ncmds, sizeofcmds, i;

    memset(im, 0, sizeof *im);
    if (!file)
        return fail(err, errlen, "no file");
    if (len < 4)
        return fail(err, errlen, "%lu bytes: too short for a Mach-O file",
                    (unsigned long)len);

    if (be32(file) == FAT_MAGIC) {
        uint32_t nfat, k, slice_off = 0, slice_size = 0;
        int      found = 0;

        if (len < 8)
            return fail(err, errlen, "fat header: truncated");
        nfat = be32(file + 4);
        if (nfat > (len - 8) / FAT_ARCH_SIZE)
            return fail(err, errlen, "fat header: %u slices do not fit in a "
                        "%lu-byte file", (unsigned)nfat, (unsigned long)len);
        for (k = 0; k < nfat && !found; k++) {
            const unsigned char *a = file + 8 + (size_t)k * FAT_ARCH_SIZE;

            if (be32(a) == CPU_TYPE_ARM && be32(a + 4) == CPU_SUBTYPE_ARM_V7) {
                slice_off  = be32(a + 8);
                slice_size = be32(a + 12);
                found = 1;
            }
        }
        if (!found)
            return fail(err, errlen, "fat file: no armv7 slice");
        if (slice_off > len || slice_size > len - slice_off)
            return fail(err, errlen, "fat file: the armv7 slice (0x%x bytes at "
                        "0x%x) runs past the end of the %lu-byte file",
                        (unsigned)slice_size, (unsigned)slice_off,
                        (unsigned long)len);
        base = slice_off;
        size = slice_size;
    }

    im->p   = file + base;
    im->len = size;
    if (size < MACH_HEADER_SIZE)
        return fail(err, errlen, "Mach-O header: truncated (%lu bytes)",
                    (unsigned long)size);
    magic = le32(im->p);
    if (magic == MH_MAGIC_64)
        return fail(err, errlen, "a 64-bit Mach-O: the game's tables are in "
                    "the armv7 slice");
    if (magic != MH_MAGIC)
        return fail(err, errlen, "not a Mach-O file (magic 0x%08x)",
                    (unsigned)magic);
    if (le32(im->p + 4) != CPU_TYPE_ARM || le32(im->p + 8) != CPU_SUBTYPE_ARM_V7)
        return fail(err, errlen, "expected armv7 (cputype 12, subtype 9), got "
                    "cputype %u subtype %u", (unsigned)le32(im->p + 4),
                    (unsigned)le32(im->p + 8));

    ncmds      = le32(im->p + 16);
    sizeofcmds = le32(im->p + 20);
    if (sizeofcmds > size - MACH_HEADER_SIZE)
        return fail(err, errlen, "load commands: %u bytes do not fit in the "
                    "%lu-byte image", (unsigned)sizeofcmds, (unsigned long)size);
    off = MACH_HEADER_SIZE;
    end = MACH_HEADER_SIZE + (size_t)sizeofcmds;
    for (i = 0; i < ncmds; i++) {
        const unsigned char *c = im->p + off;
        uint32_t cmd, cmdsize;

        if (end - off < 8)
            return fail(err, errlen, "load command %u of %u lies past "
                        "sizeofcmds", (unsigned)i, (unsigned)ncmds);
        cmd     = le32(c);
        cmdsize = le32(c + 4);
        if (cmdsize < 8 || cmdsize > end - off)
            return fail(err, errlen, "load command %u (0x%x): size %u",
                        (unsigned)i, (unsigned)cmd, (unsigned)cmdsize);
        if (cmd == LC_SEGMENT) {
            if (parse_segment(im, c, cmdsize, err, errlen))
                return -1;
        } else if (cmd == LC_SYMTAB) {
            if (cmdsize < 24)
                return fail(err, errlen, "LC_SYMTAB: %u bytes", (unsigned)cmdsize);
            im->have_symtab = 1;
            im->symoff  = le32(c + 8);
            im->nsyms   = le32(c + 12);
            im->stroff  = le32(c + 16);
            im->strsize = le32(c + 20);
        } else if (cmd == LC_UUID) {
            if (cmdsize < 24)
                return fail(err, errlen, "LC_UUID: %u bytes", (unsigned)cmdsize);
            im->have_uuid = 1;            /* the last one wins, as in macho.py */
            memcpy(im->uuid, c + 8, 16);
        } else if (cmd == LC_ENCRYPTION_INFO) {
            /* An App Store copy never decrypted (cryptid != 0): its __TEXT,
             * the playback scripts' section among it, is unreadable. The
             * native build refuses it too (launcher/check_binary.py). */
            if (cmdsize < 20)
                return fail(err, errlen, "LC_ENCRYPTION_INFO: %u bytes",
                            (unsigned)cmdsize);
            if (le32(c + 16) != 0)
                return fail(err, errlen, "the app's executable is still "
                            "encrypted (cryptid %u): the port needs a copy "
                            "whose binary was decrypted",
                            (unsigned)le32(c + 16));
        }
        off += cmdsize;
    }
    return 0;
}

static int check_uuid(const IMAGE *im, char *err, size_t errlen)
{
    static const char digits[] = "0123456789abcdef";
    char hex[33];
    int  i;

    if (!im->have_uuid)
        return fail(err, errlen, "the binary has no LC_UUID; the port needs "
                    "UMK3 1.2.59 for iPhone (uuid %s)", UMK3_RT_UUID);
    for (i = 0; i < 16; i++) {
        hex[2 * i]     = digits[im->uuid[i] >> 4];
        hex[2 * i + 1] = digits[im->uuid[i] & 15];
    }
    hex[32] = '\0';
    if (strcmp(hex, UMK3_RT_UUID) != 0)
        return fail(err, errlen, "binary uuid %s: not the one the port was "
                    "made from (UMK3 1.2.59 for iPhone, uuid %s)", hex,
                    UMK3_RT_UUID);
    return 0;
}

/* --------------------------------------------------------------- mapping */

static const SECT *section_for(const IMAGE *im, uint32_t addr)
{
    unsigned i;

    for (i = 0; i < im->nsect; i++) {
        const SECT *s = &im->sect[i];

        if (s->addr <= addr && (uint64_t)addr < (uint64_t)s->addr + s->size)
            return s;
    }
    return NULL;
}

/* The file bytes of [addr, addr + n): inside one section that has bytes, and
 * inside the image. NULL, with a message naming `what`, otherwise. */
static const unsigned char *image_bytes(const IMAGE *im, uint32_t addr,
                                        uint32_t n, const char *what,
                                        char *err, size_t errlen)
{
    const SECT *s = section_for(im, addr);
    uint64_t    off;

    if (!s) {
        fail(err, errlen, "%s: 0x%08x is in no section of the binary", what,
             (unsigned)addr);
        return NULL;
    }
    if ((uint64_t)addr + n > (uint64_t)s->addr + s->size) {
        fail(err, errlen, "%s: 0x%08x + 0x%x runs past the end of %.16s,%.16s",
             what, (unsigned)addr, (unsigned)n, (const char *)s->segname,
             (const char *)s->sectname);
        return NULL;
    }
    if (zerofill(s)) {
        fail(err, errlen, "%s: 0x%08x is in %.16s,%.16s, which is zero-fill "
             "and has no bytes in the file", what, (unsigned)addr,
             (const char *)s->segname, (const char *)s->sectname);
        return NULL;
    }
    off = (uint64_t)s->offset + (addr - s->addr);
    if (off > im->len || n > im->len - off) {
        fail(err, errlen, "%s: 0x%08x + 0x%x is at file offset 0x%lx, outside "
             "the %lu-byte image", what, (unsigned)addr, (unsigned)n,
             (unsigned long)off, (unsigned long)im->len);
        return NULL;
    }
    return im->p + off;
}

/* --------------------------------------------- 1. tools/logic_tables.py */

/* Pass one: every object maps, every relocated word holds what the layout
 * recorded. Writes nothing. */
static int logic_check(const UMK3_RT_LAYOUT *L, const IMAGE *im,
                       char *err, size_t errlen)
{
    uint32_t i, j;

    if (!L)
        return fail(err, errlen, "no layout");
    if (!L->uuid || strcmp(L->uuid, UMK3_RT_UUID) != 0)
        return fail(err, errlen, "the layout was made from uuid %s, not %s: "
                    "regenerate it with tools/logic_tables.py --layout",
                    L->uuid ? L->uuid : "(none)", UMK3_RT_UUID);
    if ((L->nobjects && !L->objects) || (L->nrelocs && !L->relocs))
        return fail(err, errlen, "layout: a table is missing");
    for (i = 0; i < L->nobjects; i++) {
        const UMK3_RT_OBJECT *o = &L->objects[i];
        const char           *name = o->name ? o->name : "(unnamed)";
        const unsigned char  *src;

        if (!o->dst)
            return fail(err, errlen, "layout: %s has no storage", name);
        if (o->words > 1)
            return fail(err, errlen, "layout: %s: words is %u, not 0 or 1",
                        name, (unsigned)o->words);
        if (o->words && o->size % 4)
            return fail(err, errlen, "layout: %s: %u bytes are not whole words",
                        name, (unsigned)o->size);
        if (!o->words && o->nrelocs)
            return fail(err, errlen, "layout: %s is bytes and has %u "
                        "relocation(s)", name, (unsigned)o->nrelocs);
        if (o->first_reloc > L->nrelocs ||
            o->nrelocs > L->nrelocs - o->first_reloc)
            return fail(err, errlen, "layout: %s's relocations %u..%u are "
                        "outside the %u listed", name, (unsigned)o->first_reloc,
                        (unsigned)(o->first_reloc + o->nrelocs),
                        (unsigned)L->nrelocs);
        /* Two objects that touch in the image must touch in memory, so that
         * a read past the end of one sees the next, as on the device
         * (`ochar_slam_damage` is read past into `ochar_slammed_anis`; Layout
         * in tools/logic_tables.py). That is the build's doing, not the
         * file's, and tools/check_logic_tables.py --nm checks it for a native
         * build; under wasm there is no nm, so it is checked here. */
        if (i > 0) {
            const UMK3_RT_OBJECT *p = &L->objects[i - 1];

            if ((uint64_t)p->vmaddr + p->size == o->vmaddr &&
                (uintptr_t)o->dst - (uintptr_t)p->dst != p->size)
                return fail(err, errlen, "%s and %s touch in the binary but "
                            "lie %ld bytes apart in this build, not %u",
                            p->name ? p->name : "(unnamed)", name,
                            (long)((uintptr_t)o->dst - (uintptr_t)p->dst),
                            (unsigned)p->size);
        }
        src = image_bytes(im, o->vmaddr, o->size, name, err, errlen);
        if (!src)
            return -1;
        for (j = 0; j < o->nrelocs; j++) {
            const UMK3_RT_RELOC *r = &L->relocs[o->first_reloc + j];
            uint32_t v;

            if (r->word >= o->size / 4)
                return fail(err, errlen, "layout: %s word %u is past its %u "
                            "bytes", name, (unsigned)r->word, (unsigned)o->size);
            v = le32(src + 4 * (size_t)r->word);
            if (v != r->original)
                return fail(err, errlen, "%s+0x%x: the binary holds 0x%08x, the "
                            "layout the pointer 0x%08x; it was not made from "
                            "this binary", name, (unsigned)(4 * r->word),
                            (unsigned)v, (unsigned)r->original);
        }
    }
    return 0;
}

/* Pass two. A word object gets each word as a number, the way the generated
 * file writes `0x...` -- so the host's byte order does not matter -- and then
 * its relocations; a byte object gets its bytes. */
static void logic_write(const UMK3_RT_LAYOUT *L, const IMAGE *im)
{
    uint32_t i, j, k;

    for (i = 0; i < L->nobjects; i++) {
        const UMK3_RT_OBJECT *o = &L->objects[i];
        const unsigned char  *src = image_bytes(im, o->vmaddr, o->size, "",
                                                NULL, 0);

        if (!src)                         /* cannot happen after logic_check */
            continue;
        if (o->words) {
            uintptr_t *d = (uintptr_t *)o->dst;

            for (k = 0; k < o->size / 4; k++)
                d[k] = le32(src + 4 * (size_t)k);
            for (j = 0; j < o->nrelocs; j++) {
                const UMK3_RT_RELOC *r = &L->relocs[o->first_reloc + j];

                d[r->word] = r->target;
            }
        } else {
            memcpy(o->dst, src, o->size);
        }
    }
}

/* ----------------------------------------------- 2. tools/level_info.py */

static const unsigned char *nlist_at(const IMAGE *im, uint32_t i)
{
    return im->p + im->symoff + (size_t)i * NLIST_SIZE;
}

/* level_info.py's filter: N_SECT and not a stab. */
static int is_sect_symbol(const unsigned char *e)
{
    return (e[4] & N_STAB) == 0 && (e[4] & N_TYPE) == N_SECT;
}

/* The symbol's name, or NULL when n_strx or the string runs out of the
 * string table. n_strx 0 is the empty name, as in macho.py. */
static const char *symbol_name(const IMAGE *im, const unsigned char *e)
{
    uint32_t    strx = le32(e);
    const char *s;

    if (strx == 0)
        return "";
    if (strx >= im->strsize)
        return NULL;
    s = (const char *)im->p + im->stroff + strx;
    return memchr(s, 0, im->strsize - strx) ? s : NULL;
}

/* level_info.py keeps the FIRST value of each name (`syms.setdefault`), so a
 * later symbol of the same name takes no part in any of its rules. Is symbol
 * i the first N_SECT symbol of its name? */
static int first_of_name(const IMAGE *im, uint32_t i)
{
    const char *name = symbol_name(im, nlist_at(im, i));
    uint32_t    j;

    for (j = 0; j < i; j++) {
        const unsigned char *e = nlist_at(im, j);

        if (is_sect_symbol(e) && strcmp(symbol_name(im, e), name) == 0)
            return 0;
    }
    return 1;
}

static int find_symbol(const IMAGE *im, const char *name, uint32_t *value)
{
    uint32_t i;

    for (i = 0; i < im->nsyms; i++) {
        const unsigned char *e = nlist_at(im, i);

        if (is_sect_symbol(e) && strcmp(symbol_name(im, e), name) == 0) {
            *value = le32(e + 8);
            return 1;
        }
    }
    return 0;
}

static void level_free(LEVEL *lv)
{
    free(lv->pool);
    free(lv->off);
    free(lv->pool_off);
    memset(lv, 0, sizeof *lv);
}

static int level_compute(const IMAGE *im, LEVEL *lv, char *err, size_t errlen)
{
    uint32_t info, after, best = 0, info_size, pool_lo, pool_hi, off, i, n;
    uint64_t lower;
    const unsigned char *pool, *w;
    const SECT *s;

    memset(lv, 0, sizeof *lv);
    if (!im->have_symtab)
        return fail(err, errlen, "the binary has no LC_SYMTAB to find "
                    "_Level_Info in");
    if (im->symoff > im->len || im->nsyms > (im->len - im->symoff) / NLIST_SIZE)
        return fail(err, errlen, "LC_SYMTAB: %u symbols at 0x%x run past the "
                    "end of the image", (unsigned)im->nsyms,
                    (unsigned)im->symoff);
    if (im->stroff > im->len || im->strsize > im->len - im->stroff)
        return fail(err, errlen, "LC_SYMTAB: the string table (%u bytes at "
                    "0x%x) runs past the end of the image",
                    (unsigned)im->strsize, (unsigned)im->stroff);
    /* Every name compared below is checked once, here. */
    for (i = 0; i < im->nsyms; i++) {
        const unsigned char *e = nlist_at(im, i);

        if (is_sect_symbol(e) && !symbol_name(im, e))
            return fail(err, errlen, "symbol %u: its name is outside the "
                        "string table", (unsigned)i);
    }

    if (!find_symbol(im, "_Level_Info", &info))
        return fail(err, errlen, "the binary has no _Level_Info symbol");
    if (!find_symbol(im, "_CurrentScene", &after))
        return fail(err, errlen, "the binary has no _CurrentScene symbol");

    /* Level_Info's size: the gap to the lowest symbol above it (0xff4 in
     * 1.2.59: `_LevelMusic` is at 0x0014f8c8). A value counts only if some
     * symbol is the first of its name there. */
    lower = info;
    for (;;) {
        int found = 0, first = 0;

        for (i = 0; i < im->nsyms; i++) {
            const unsigned char *e = nlist_at(im, i);
            uint32_t v = le32(e + 8);

            if (is_sect_symbol(e) && v > lower && (!found || v < best)) {
                best = v;
                found = 1;
            }
        }
        if (!found)
            return fail(err, errlen, "no symbol after _Level_Info (0x%08x): "
                        "its size is unknown", (unsigned)info);
        for (i = 0; i < im->nsyms && !first; i++) {
            const unsigned char *e = nlist_at(im, i);

            first = is_sect_symbol(e) && le32(e + 8) == best &&
                    first_of_name(im, i);
        }
        if (first)
            break;
        lower = best;
    }
    info_size = best - info;
    /* The words found below are written into the host's Level_Info, which
     * runtime/gamecode_globals.c defines at the binary's 0xff4 bytes. The
     * script needs no such check -- its output is compiled against the same
     * binary -- but a file read at run time could measure it larger and have
     * them written past the end. */
    if (info_size > LEVEL_INFO_SIZE)
        return fail(err, errlen, "_Level_Info measures 0x%x bytes, more than "
                    "the 0x%x the port gives it", (unsigned)info_size,
                    (unsigned)LEVEL_INFO_SIZE);

    /* The pool: after _CurrentScene's four bytes, up to Level_Info. */
    if ((uint64_t)after + 4 > info)
        return fail(err, errlen, "_CurrentScene (0x%08x) does not end before "
                    "_Level_Info (0x%08x): the layout changed",
                    (unsigned)after, (unsigned)info);
    pool_lo = after + 4;
    pool_hi = info;
    for (i = 0; i < im->nsyms; i++) {
        const unsigned char *e = nlist_at(im, i);
        uint32_t v = le32(e + 8);

        if (is_sect_symbol(e) && pool_lo <= v && v < pool_hi &&
            first_of_name(im, i))
            return fail(err, errlen, "a symbol inside the string pool (%s at "
                        "0x%08x): the layout changed",
                        symbol_name(im, e), (unsigned)v);
    }

    s = section_for(im, info);
    if (!s || !name_is(s->segname, "__DATA") || !name_is(s->sectname, "__data"))
        return fail(err, errlen, "_Level_Info is not in __DATA,__data");
    pool = image_bytes(im, pool_lo, pool_hi - pool_lo, "the arena-name pool",
                       err, errlen);
    if (!pool)
        return -1;

    /* A word is relocated when, and only when, its value lies in the pool.
     * One pass to count, one to record. */
    for (n = 0, off = 0; off + 3 < info_size; off += 4) {
        w = image_bytes(im, info + off, 4, "_Level_Info", err, errlen);
        if (!w)
            return -1;
        if (pool_lo <= le32(w) && le32(w) < pool_hi)
            n++;
    }
    if (n == 0)
        return fail(err, errlen, "no Level_Info word points into the pool");

    lv->pool     = (char *)malloc(pool_hi - pool_lo ? pool_hi - pool_lo : 1);
    lv->off      = (uint32_t *)malloc((size_t)n * sizeof *lv->off);
    lv->pool_off = (uint32_t *)malloc((size_t)n * sizeof *lv->pool_off);
    if (!lv->pool || !lv->off || !lv->pool_off) {
        level_free(lv);
        return fail(err, errlen, "out of memory for Level_Info's pool");
    }
    memcpy(lv->pool, pool, pool_hi - pool_lo);
    for (off = 0; off + 3 < info_size && lv->n < n; off += 4) {
        w = image_bytes(im, info + off, 4, "_Level_Info", NULL, 0);
        if (w && pool_lo <= le32(w) && le32(w) < pool_hi) {
            lv->off[lv->n]      = off;
            lv->pool_off[lv->n] = le32(w) - pool_lo;
            lv->n++;
        }
    }
    return 0;
}

/* level_info.c's function: point each recorded word at the same byte of the
 * host's pool. Runs after a successful fill (game_main.c calls it between
 * fight_runtime_init and GameCodeInit); before one, there is nothing to do. */
void umk3_relocate_level_info(void)
{
    uint32_t i;

    for (i = 0; i < g_level.n; i++) {
        uint32_t v = (uint32_t)(uintptr_t)(g_level.pool + g_level.pool_off[i]);

        memcpy(Level_Info + g_level.off[i], &v, sizeof v);
    }
}

/* ------------------------------------------------- 3. tools/seq_data.py */

static int seq_compute(const IMAGE *im, SEQ *sq, char *err, size_t errlen)
{
    const SECT          *s = section_for(im, (uint32_t)UMK3_RT_SEQ_LO);
    const unsigned char *blob;
    uint64_t             hi;
    uint32_t             n, k, nwords;

    memset(sq, 0, sizeof *sq);
    if (!s || !name_is(s->segname, "__TEXT") || !name_is(s->sectname, "__const"))
        return fail(err, errlen, "0x%08lx is not in __TEXT,__const: the layout "
                    "changed", UMK3_RT_SEQ_LO);
    hi = (uint64_t)s->addr + s->size;
    if (hi > 0xffffffffu)
        return fail(err, errlen, "__TEXT,__const runs past the 32-bit address "
                    "space");
    n = (uint32_t)(hi - UMK3_RT_SEQ_LO);
    blob = image_bytes(im, (uint32_t)UMK3_RT_SEQ_LO, n, "the playback scripts",
                       err, errlen);
    if (!blob)
        return -1;

    /* seq_data.c's `unsigned short seq_words[(n + 1) / 2]`, each word the
     * little-endian pair, an odd last byte padded with zero. */
    nwords = n / 2 + n % 2;
    sq->words = (uint16_t *)malloc(nwords ? (size_t)nwords * 2 : 2);
    if (!sq->words)
        return fail(err, errlen, "out of memory for the playback scripts");
    for (k = 0; k < nwords; k++)
        sq->words[k] = (uint16_t)(blob[2 * (size_t)k] |
                       (2 * k + 1 < n ? blob[2 * (size_t)k + 1] << 8 : 0));
    sq->hi = (uint32_t)hi;
    return 0;
}

/* seq_data.c's function: the host address of the script byte at the binary's
 * `ios_address`, NULL outside [0x000de242, end of __TEXT,__const). */
const unsigned char *umk3_seq_addr(unsigned long ios_address)
{
    if (!g_seq.words || ios_address < UMK3_RT_SEQ_LO || ios_address >= g_seq.hi)
        return NULL;
    return (const unsigned char *)g_seq.words + (ios_address - UMK3_RT_SEQ_LO);
}

/* ------------------------------------------------------------------ fill */

int umk3_tables_fill_layout(const UMK3_RT_LAYOUT *layout,
                            const unsigned char *file, size_t len,
                            char *err, size_t errlen)
{
    IMAGE *im;
    LEVEL  lv;
    SEQ    sq;
    int    rc;

    if (err && errlen)
        err[0] = '\0';
    /* The tables hold 32-bit addresses, and the layout file does not compile
     * on any other host. This one does, so that tools/check.sh can check it
     * with whatever compiler it has, and refuses here instead. */
    if (sizeof(void *) != 4)
        return fail(err, errlen, "32-bit hosts only: the tables hold 32-bit "
                    "addresses");
    memset(&lv, 0, sizeof lv);
    memset(&sq, 0, sizeof sq);
    im = (IMAGE *)malloc(sizeof *im);
    if (!im)
        return fail(err, errlen, "out of memory");

    rc = image_open(im, file, len, err, errlen);
    if (!rc)
        rc = check_uuid(im, err, errlen);
    if (!rc)
        rc = logic_check(layout, im, err, errlen);
    if (!rc)
        rc = level_compute(im, &lv, err, errlen);
    if (!rc)
        rc = seq_compute(im, &sq, err, errlen);

    if (!rc) {
        /* Everything holds: from here on, nothing can fail. A second fill
         * replaces the pool, so umk3_relocate_level_info has to run again. */
        level_free(&g_level);
        g_level = lv;
        free(g_seq.words);
        g_seq = sq;
        logic_write(layout, im);
    } else {
        level_free(&lv);
        free(sq.words);
    }
    free(im);
    return rc;
}

int umk3_tables_fill(const unsigned char *file, size_t len,
                     char *err, size_t errlen)
{
    return umk3_tables_fill_layout(&umk3_logic_layout, file, len, err, errlen);
}

int umk3_tables_fill_file(const char *path, char *err, size_t errlen)
{
    FILE          *f;
    long           n;
    unsigned char *buf;
    int            rc;

    f = path ? fopen(path, "rb") : NULL;
    if (!f)
        return fail(err, errlen, "%s: cannot open", path ? path : "(no path)");
    if (fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) < 0 ||
        fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return fail(err, errlen, "%s: cannot read its size", path);
    }
    buf = (unsigned char *)malloc(n ? (size_t)n : 1);
    if (!buf) {
        fclose(f);
        return fail(err, errlen, "%s: out of memory for %ld bytes", path, n);
    }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        free(buf);
        return fail(err, errlen, "%s: short read", path);
    }
    fclose(f);
    rc = umk3_tables_fill(buf, (size_t)n, err, errlen);
    free(buf);
    return rc;
}

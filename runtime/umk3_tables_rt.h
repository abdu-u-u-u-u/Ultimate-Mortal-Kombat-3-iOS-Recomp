/*
 * umk3_tables_rt.h -- the fight engine's tables, filled at run time from the
 * user's own binary instead of being compiled in.
 *
 * A native build reads the user's binary at BUILD time: tools/logic_tables.py,
 * tools/level_info.py and tools/seq_data.py write C files that hold the
 * game's bytes, and those are compiled into the executable. That is fine for a
 * program built on the player's own machine from the player's own .ipa, and
 * wrong for one served to other people as a static web page: the .wasm would
 * carry the game's data.
 *
 * The browser build therefore compiles two things instead:
 *
 *   - `tools/logic_tables.py --layout`'s output: every logic object defined as
 *     zero storage of the same type, size, alignment and order, plus
 *     `umk3_logic_layout`, which says where each object's bytes are in the
 *     binary and which of its words are pointers. No game bytes.
 *
 *   - runtime/umk3_tables_rt.c: given the bytes of the player's
 *     Payload/UMK3.app/UMK3 (read in the browser from the player's .ipa), it
 *     fills that storage, and does at run time what level_info.c and
 *     seq_data.c did at build time, with the same function names.
 *
 * Call umk3_tables_fill (or umk3_tables_fill_file) once, before
 * fight_runtime_init() and GameCodeInit(); runtime/game_main.c's existing
 * umk3_relocate_level_info() call then works unchanged. On success memory
 * holds what the compiled-in tables would have held, and the file's bytes
 * are no longer needed (everything is copied). On failure nothing has been
 * written, and `err` says why.
 *
 * 32-bit hosts only (i686, wasm32), like the generated files: the engine's
 * tables hold 32-bit addresses. The layout file refuses to compile anywhere
 * else; the filler compiles anywhere (tools/check.sh compiles every runtime
 * file with the host's compiler) and refuses at run time.
 */
#ifndef UMK3_TABLES_RT_H
#define UMK3_TABLES_RT_H

#include <stddef.h>
#include <stdint.h>

/* LC_UUID of the binary every address in decomp/ was read from:
 * Payload/UMK3.app/UMK3, armv7 slice, UMK3 1.2.59 for iPhone. The same uuid
 * launcher/build_game.ps1 checks before it builds anything. */
#define UMK3_RT_UUID "90d6f56a18e2303f8f2053b02a42742e"

/* tools/seq_data.py's SEQ_LO: the lowest address in seq_lookup's table
 * (0x000adb80), in __TEXT,__const. The scripts run from here to the end of
 * that section. */
#define UMK3_RT_SEQ_LO 0x000de242ul

/* One logic object, as tools/logic_tables.py --layout describes it. */
typedef struct UMK3_RT_OBJECT {
    const char *name;         /* its C name, for messages */
    void       *dst;          /* the zero storage the layout file defines */
    uint32_t    vmaddr;       /* where its bytes are in the binary */
    uint32_t    size;         /* how many bytes */
    uint32_t    words;        /* 1: uintptr_t[size / 4]; 0: unsigned char[size] */
    uint32_t    first_reloc;  /* its relocations: relocs[first_reloc] ... */
    uint32_t    nrelocs;      /* ... and how many (0 for a byte object) */
} UMK3_RT_OBJECT;

/* One relocated word. `original` is what the binary holds at that word -- an
 * iOS address, a Thumb function's | 1 or one inside a data object -- and is
 * checked before `target`, the host's address of the same thing, replaces
 * it. */
typedef struct UMK3_RT_RELOC {
    uint32_t  word;           /* index of the 32-bit word in the object */
    uint32_t  original;
    uintptr_t target;
} UMK3_RT_RELOC;

typedef struct UMK3_RT_LAYOUT {
    const char           *uuid;      /* of the binary the layout was made from */
    const UMK3_RT_OBJECT *objects;   /* binary address order */
    uint32_t              nobjects;
    const UMK3_RT_RELOC  *relocs;
    uint32_t              nrelocs;
} UMK3_RT_LAYOUT;

/* Defined by the output of tools/logic_tables.py --layout. */
extern const UMK3_RT_LAYOUT umk3_logic_layout;

/* Fill the tables from `file` (`len` bytes): a thin armv7 Mach-O, or a fat
 * one with an armv7 slice. 0 on success; -1 on failure, with a message in
 * `err` (always NUL-terminated when errlen > 0; err may be NULL). */
int umk3_tables_fill(const unsigned char *file, size_t len,
                     char *err, size_t errlen);

/* The same, reading the file at `path` (in the browser: the virtual file the
 * page wrote the .ipa's Payload/UMK3.app/UMK3 to). */
int umk3_tables_fill_file(const char *path, char *err, size_t errlen);

/* umk3_tables_fill against a given layout; the tests use it. */
int umk3_tables_fill_layout(const UMK3_RT_LAYOUT *layout,
                            const unsigned char *file, size_t len,
                            char *err, size_t errlen);

/* level_info.c's and seq_data.c's functions, with their meaning. Before a
 * successful fill the first does nothing and the second returns NULL. */
void umk3_relocate_level_info(void);
const unsigned char *umk3_seq_addr(unsigned long ios_address);

#endif /* UMK3_TABLES_RT_H */

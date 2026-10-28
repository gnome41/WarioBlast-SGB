/* Static recompiler: Game Boy ROM -> C source.  Used ahead of time by the
 * developer build (tools/recomp/wb_recomp.c) and at runtime by the
 * player, which compiles the result with TinyCC on the user's machine. */
#ifndef RECOMP_H
#define RECOMP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---- opcode table (generated from tools/recomp/sm83.py) ---- */

typedef enum {
    K_OP, K_JP, K_JR, K_CALL, K_RET, K_RETI, K_RST, K_JPHL, K_HALT, K_STOP,
    K_EI, K_DI, K_ILLEGAL, K_PREFIX
} Sm83Kind;

typedef struct {
    uint8_t length, kind, cycles, cycles_taken, target, sets_a;
    const char *mnem;   /* disassembly, with {n8} {n16} {e8} {e8:+} placeholders */
    const char *code;   /* C template for K_OP, same placeholders */
    const char *cond;   /* C condition for conditional flow, or NULL */
} Sm83Op;

extern const Sm83Op sm83_main[256], sm83_cb[256];

/* ---- game notes (generated from overrides.toml / coverage.txt) ---- */

typedef struct { uint8_t bank; uint16_t addr; } RecompRef;
typedef struct { uint16_t addr, max; } RecompFarTable;
/* routine taking a code address in a register pair (0 bc, 1 de, 2 hl) and
 * optionally its bank in an 8-bit register (ld r,n8 numbering, -1 none) */
typedef struct { uint16_t routine; uint8_t addr_reg; int8_t bank_reg; } RecompFarCall;

typedef struct {
    uint32_t rom_crc32;
    const uint16_t *jump_table_routines; size_t n_jump_table_routines;
    const uint16_t *bank_switch_routines; size_t n_bank_switch_routines;
    const RecompRef *noreturn; size_t n_noreturn;
    const RecompRef *entries; size_t n_entries;
    const RecompFarTable *far_tables; size_t n_far_tables;
    const RecompFarCall *far_calls; size_t n_far_calls;
    const RecompRef *coverage; size_t n_coverage;   /* sorted, unique */
} RecompConfig;

extern const RecompConfig game_notes;

/* sound driver interface (RAM addresses), for audio-replacement mods */
typedef struct {
    uint16_t music_request;      /* song id written by the game */
    uint16_t sfx_request;        /* effect id written by the game */
    uint16_t sfx_channel_mask;   /* bit n set while an effect uses channel n+1 */
    uint16_t pause_flag;
    uint8_t pause_mask;          /* pause_flag & pause_mask != 0 while paused */
    uint8_t music_stop;          /* request value that stops the music */
    uint8_t ack_mask;            /* requests with these bits are the driver's own
                                    writes (request taken / song over): ignored */
} GameAudioNotes;

extern const GameAudioNotes game_audio;

/* widescreen view: while the window sits at (active_wx, active_wy) a match
 * is on screen, and the background is an arena of arena_w x arena_h
 * pixels drawn through the 256x256 tilemap with wrap-around (0 = unused) */
typedef struct {
    uint8_t active_wx, active_wy;
    uint16_t arena_w, arena_h;
} GameWideNotes;

extern const GameWideNotes game_wide;

/* ---- results ---- */

typedef struct {
    uint8_t bank;
    uint16_t entry;
    size_t src_off, src_len;   /* this function's text within RecompResult.src */
} RecompFunc;

typedef struct {
    uint8_t bank;
    uint16_t addr;
    uint32_t fn;               /* index into funcs */
} RecompEntry;

typedef struct {
    char *src;                 /* all functions, ordered by (bank, entry) */
    size_t src_len;
    RecompFunc *funcs;
    uint32_t nfuncs;
    RecompEntry *entries;      /* every instruction, function entries first */
    size_t nentries;
    size_t ninsns, ntables, nseeds;
} RecompResult;

bool recomp_run(const uint8_t *rom, size_t size, const RecompConfig *cfg, RecompResult *out);
void recomp_free(RecompResult *r);
void recomp_fn_name(uint8_t bank, uint16_t entry, char out[16]);

#endif

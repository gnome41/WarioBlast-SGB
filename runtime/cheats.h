/* Cheat codes: GameShark (RAM writes every frame) and Game Genie (ROM
 * patches).  Recompiled code has the ROM's instructions compiled in, so
 * functions containing a Game Genie-patched byte run on the interpreter,
 * which reads the patched ROM. */
#ifndef CHEATS_H
#define CHEATS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CHEATS_MAX 64

typedef struct {
    bool on;
    char name[64];
    char codes[192];    /* one or more codes separated by spaces, commas or '+' */
} Cheat;

/* Checks a code list; on error writes a message (upper case, for the
 * launcher) to err and returns false. */
bool cheats_parse_check(const char *codes, char *err, size_t err_size);

/* The cheats file (cheats.txt in the settings folder): "on|off name = codes".
 * A missing file gives the built-in cheats for this game. */
int cheats_file_load(const char *path, Cheat *out, int max);
bool cheats_file_save(const char *path, const Cheat *list, int n);
int cheats_builtin(Cheat *out, int max);

/* Activates codes (after the ROM is loaded and recompiled); returns false
 * and reports to stderr if one is invalid (the others still apply). */
bool cheats_add(const char *codes);
bool cheats_active(void);         /* codes loaded and not toggled off */
void cheats_toggle(void);         /* F8 */
void cheats_frame(void);          /* VBlank: GameShark writes */

/* Stage select: stages 0..stage_count()-1 in play order (1-1 ... 8-4);
 * new games start at the chosen one (0 = normal).  cheats_start_at
 * patches the ROM, so call it after recompiling and before cheats_add. */
int stage_count(void);
void stage_label(int i, char *buf, size_t size);   /* "5-3 ENEMY 3", "5-4 BOSS" */
int stage_find(const char *ws);                    /* "W-S" -> stage, or -1 */
int stage_next_world(int stage, int dir);          /* first stage of the next / previous world */
bool cheats_start_at(int stage);

/* Dispatcher: ROM bytes changed by Game Genie codes, so it can leave the
 * recompiled functions containing them to the interpreter.  Returns the
 * number of patched locations; bank 0 for addresses below 0x4000. */
size_t cheats_rom_patches(const uint8_t **bank, const uint16_t **addr);
extern bool dispatch_dirty;       /* rebuild the dispatch table (cheats toggled) */

#endif

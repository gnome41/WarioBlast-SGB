#ifndef LAUNCHER_H
#define LAUNCHER_H

#include <stdbool.h>
#include <stddef.h>

/* Shows the launcher.  On "Play" fills opt and the chosen ROM / BIOS
 * paths (bios empty = none) and returns true; returns false on quit. */
bool launcher_run(char *rom, size_t rom_size, char *bios, size_t bios_size);

/* mod packs chosen in the launcher (priority order) and the dump toggle */
extern char launcher_mods[16][128];
extern int launcher_mod_count;
extern bool launcher_dump;
extern const char *launcher_mods_dir;   /* --mods-dir, or NULL for the default */

/* code lists of the cheats switched on in the launcher */
extern char launcher_cheats[64][192];
extern int launcher_cheat_count;

#endif

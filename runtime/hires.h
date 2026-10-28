/* Per-pixel capture for HD tile replacement (filled by the PPU while mods
 * or dumping are active; render-only, never affects emulation). */
#ifndef HIRES_H
#define HIRES_H

#include <stdbool.h>
#include <stdint.h>
#include "gb.h"
#include "mods.h"

#define NO_INST 0xFFFF

enum {
    SPF_XFLIP = 1, SPF_YFLIP = 2, SPF_BEHIND = 4, SPF_DRAWN = 8, SPF_OBP1 = 16,
};

typedef struct {
    uint16_t bg;          /* tile instance of the background/window pixel */
    uint16_t sp;          /* tile instance of the sprite covering it, or NO_INST */
    uint8_t bg_sub;       /* pixel within the tile: x | y << 3 */
    uint8_t bg_idx;       /* colour index 0-3 */
    uint8_t bg_shade;     /* after BGP */
    uint8_t sp_sub, sp_idx, sp_shade, sp_flags;
} PixRef;

typedef struct {
    uint8_t data[16];     /* the tile's 2bpp data = its key */
    uint8_t pal;          /* BGP / OBP value it was drawn with */
    uint8_t kind;         /* 0 background/window, 1 sprite */
    const ModTile *tile;  /* replacement, or NULL */
} TileInst;

extern PixRef hires_refs[144][160];
extern PixRef hires_refs_wide[144][WIDE_W];
extern TileInst hires_inst[];
extern int hires_inst_count;
extern bool hires_captured;   /* any line captured this frame */

/* Instance for a tile drawn this frame (deduplicated). */
uint16_t hires_instance(const uint8_t *tile16, uint8_t pal, uint8_t kind);

#endif

/* Scanline PPU: mode/LY/STAT timing and per-line rendering to DMG shades. */
#include <stdlib.h>
#include <string.h>
#include "gb.h"
#include "hires.h"

uint8_t ppu_shades[144][160];
uint8_t ppu_wide[144][WIDE_W];
uint32_t map_gen[0x800];
uint32_t screen_gen = 1;
bool wide_has_content;

/* In the wide view, a tilemap entry outside the original 160 px is only
 * shown if it was written for the current screen. */
static int no_mask = -1;   /* WB_WIDE_NOMASK: debugging aid */

static inline bool map_valid(unsigned map_addr, int x)
{
    return (x >= 0 && x < 160) || no_mask || map_gen[map_addr - 0x9800] == screen_gen;
}

#define LCDC (gb.io[IO_LCDC])
#define STAT (gb.io[IO_STAT])
#define LY (gb.io[IO_LY])

static void set_mode(int m)
{
    gb.ppu_mode = m;
    STAT = (STAT & ~3) | m;
}

static void update_stat_line(void)
{
    bool coinc = LY == gb.io[IO_LYC];
    STAT = (STAT & ~4) | (coinc ? 4 : 0);
    bool line = (coinc && (STAT & 0x40)) ||
                (gb.ppu_mode == 0 && (STAT & 0x08)) ||
                (gb.ppu_mode == 1 && (STAT & 0x10)) ||
                (gb.ppu_mode == 2 && (STAT & 0x20));
    if (line && !gb.stat_line)
        gb.io[IO_IF] |= INT_STAT;
    gb.stat_line = line;
}

static inline uint8_t tile_pixel(unsigned tile_addr, int x, int y)
{
    const uint8_t *t = &gb.vram[tile_addr - 0x8000 + y * 2];
    int b = 7 - x;
    return ((t[0] >> b) & 1) | (((t[1] >> b) & 1) << 1);
}

static unsigned bg_tile_addr(uint8_t idx)
{
    if (LCDC & 0x10)
        return 0x8000 + idx * 16;
    return 0x9000 + (int8_t)idx * 16;
}

/* Renders screen columns x0 .. x0+w-1 of line ly into out[0..w-1].  The
 * normal view is x0 = 0, w = 160; the widescreen view extends the same
 * line to either side (x0 < 0, w > 160) using identical register state. */
static void render_span(int ly, int x0, int w, int wy, bool window_on, uint8_t *out, PixRef *refs)
{
    uint8_t bgidx[WIDE_W];
    uint8_t bgp = gb.io[IO_BGP];
    unsigned last_ta = 0;
    uint16_t inst = NO_INST;

    memset(bgidx, 0, sizeof bgidx);
    if (refs)
        for (int i = 0; i < w; i++)
            refs[i] = (PixRef){ .bg = NO_INST, .sp = NO_INST };
    if (LCDC & 0x01) {
        unsigned map = (LCDC & 0x08) ? 0x9C00 : 0x9800;
        int y = (ly + gb.io[IO_SCY]) & 0xFF;
        for (int i = 0; i < w; i++) {
            int raw = x0 + i + gb.io[IO_SCX];
            int sx = raw & 0xFF;
            unsigned ma = map + (y / 8) * 32 + sx / 8;
            if (!map_valid(ma, x0 + i))
                continue;
            /* outside the original view, never wrap around the 256-px map:
             * that would show the far end of the level */
            if ((raw < 0 || raw > 255) && (x0 + i < 0 || x0 + i >= 160) && !no_mask)
                continue;
            unsigned ta = bg_tile_addr(gb.vram[ma - 0x8000]);
            bgidx[i] = tile_pixel(ta, sx & 7, y & 7);
            if (bgidx[i] && (x0 + i < 0 || x0 + i >= 160))
                wide_has_content = true;
            if (refs) {
                if (ta != last_ta || inst == NO_INST) {
                    inst = hires_instance(&gb.vram[ta - 0x8000], bgp, 0);
                    last_ta = ta;
                }
                refs[i].bg = inst;
                refs[i].bg_sub = (uint8_t)((sx & 7) | ((y & 7) << 3));
            }
        }
        if (window_on) {
            int wx = gb.io[IO_WX] - 7;
            unsigned wmap = (LCDC & 0x40) ? 0x9C00 : 0x9800;
            for (int x = wx < x0 ? x0 : wx; x < x0 + w; x++) {
                int px = x - wx;
                unsigned ma = wmap + (wy / 8) * 32 + (px / 8 & 31);
                if (!map_valid(ma, x)) {
                    bgidx[x - x0] = 0;
                    if (refs)
                        refs[x - x0].bg = NO_INST;
                    continue;
                }
                unsigned ta = bg_tile_addr(gb.vram[ma - 0x8000]);
                bgidx[x - x0] = tile_pixel(ta, px & 7, wy & 7);
                if (refs) {
                    if (ta != last_ta || inst == NO_INST) {
                        inst = hires_instance(&gb.vram[ta - 0x8000], bgp, 0);
                        last_ta = ta;
                    }
                    refs[x - x0].bg = inst;
                    refs[x - x0].bg_sub = (uint8_t)((px & 7) | ((wy & 7) << 3));
                }
            }
        }
    }
    for (int i = 0; i < w; i++)
        out[i] = (bgp >> (bgidx[i] * 2)) & 3;
    if (refs)
        for (int i = 0; i < w; i++) {
            refs[i].bg_idx = bgidx[i];
            refs[i].bg_shade = out[i];
        }

    if (!(LCDC & 0x02))
        return;
    int h = (LCDC & 0x04) ? 16 : 8;
    int sel[10], n = 0;
    for (int i = 0; i < 40 && n < 10; i++) {
        int sy = gb.oam[i * 4] - 16;
        if (ly >= sy && ly < sy + h)
            sel[n++] = i;
    }
    /* DMG priority: smaller X wins, then lower OAM index.  Draw lowest
     * priority first so higher priority sprites overwrite. */
    for (int a = 0; a < n; a++)
        for (int b = a + 1; b < n; b++) {
            int xa = gb.oam[sel[a] * 4 + 1], xb = gb.oam[sel[b] * 4 + 1];
            if (xb > xa || (xb == xa && sel[b] > sel[a])) {
                int t = sel[a]; sel[a] = sel[b]; sel[b] = t;
            }
        }
    for (int k = 0; k < n; k++) {
        const uint8_t *s = &gb.oam[sel[k] * 4];
        int sy = s[0] - 16, sx = s[1] - 8;
        uint8_t tile = s[2], attr = s[3];
        int row = ly - sy;
        if (attr & 0x40)
            row = h - 1 - row;
        if (h == 16)
            tile &= 0xFE;
        unsigned addr = 0x8000 + tile * 16 + (row >= 8 ? 16 : 0);
        uint8_t pal = (attr & 0x10) ? gb.io[IO_OBP1] : gb.io[IO_OBP0];
        uint16_t sinst = refs ? hires_instance(&gb.vram[addr - 0x8000], pal, 1) : NO_INST;
        for (int px = 0; px < 8; px++) {
            int x = sx + px;
            if (x < x0 || x >= x0 + w)
                continue;
            int tx = (attr & 0x20) ? 7 - px : px;
            uint8_t c = tile_pixel(addr, tx, row & 7);
            bool drawn = c && !((attr & 0x80) && bgidx[x - x0]);
            if (refs) {
                /* the sprite covering this pixel: an opaque, drawn one wins;
                 * otherwise the highest-priority sprite whose box covers it
                 * (so HD art may extend beyond the original silhouette) */
                PixRef *r = &refs[x - x0];
                if (drawn || !(r->sp != NO_INST && (r->sp_flags & SPF_DRAWN))) {
                    r->sp = sinst;
                    r->sp_sub = (uint8_t)(tx | ((row & 7) << 3));
                    r->sp_idx = c;
                    r->sp_shade = (pal >> (c * 2)) & 3;
                    r->sp_flags = ((attr & 0x20) ? SPF_XFLIP : 0) | ((attr & 0x40) ? SPF_YFLIP : 0) |
                                  ((attr & 0x80) ? SPF_BEHIND : 0) | (drawn ? SPF_DRAWN : 0) |
                                  ((attr & 0x10) ? SPF_OBP1 : 0);
                }
            }
            if (!drawn)
                continue;
            out[x - x0] = (pal >> (c * 2)) & 3;
            if (x < 0 || x >= 160)
                wide_has_content = true;
        }
    }
}

static void render_line(int ly)
{
    if (no_mask < 0)
        no_mask = getenv("WB_WIDE_NOMASK") != NULL;
    int wx = gb.io[IO_WX] - 7;
    bool window_on = (LCDC & 0x01) && (LCDC & 0x20) && ly >= gb.io[IO_WY] &&
                     wx < 160 && gb.io[IO_WX] <= 166;
    int wy = gb.window_line;
    bool cap = hires_capture_active();
    render_span(ly, 0, 160, wy, window_on, ppu_shades[ly], cap ? hires_refs[ly] : NULL);
    if (opt.view == VIEW_WIDE)
        render_span(ly, -WIDE_EXTRA, WIDE_W, wy, window_on, ppu_wide[ly], cap ? hires_refs_wide[ly] : NULL);
    if (window_on)
        gb.window_line++;
}

void ppu_lcdc_write(uint8_t v)
{
    uint8_t old = LCDC;
    LCDC = v;
    if ((old & 0x80) && !(v & 0x80)) {
        screen_gen++;
        LY = 0;
        gb.ppu_dot = 0;
        set_mode(0);
        gb.lcd_off_cycles = 0;
        memset(ppu_shades, 0, sizeof ppu_shades);
    } else if (!(old & 0x80) && (v & 0x80)) {
        LY = 0;
        gb.ppu_dot = 0;
        gb.window_line = 0;
        set_mode(2);
        update_stat_line();
    }
}

void ppu_tick(int n)
{
    if (!(LCDC & 0x80)) {
        gb.lcd_off_cycles += n;
        if (gb.lcd_off_cycles >= CYCLES_PER_FRAME) {
            gb.lcd_off_cycles -= CYCLES_PER_FRAME;
            platform_frame();
            hires_frame_done();
        }
        return;
    }
    gb.ppu_dot += n;
    for (;;) {
        if (LY < 144) {
            if (gb.ppu_mode == 2 && gb.ppu_dot >= 80) {
                set_mode(3);
            } else if (gb.ppu_mode == 3 && gb.ppu_dot >= 252) {
                render_line(LY);
                set_mode(0);
            } else if (gb.ppu_mode == 0 && gb.ppu_dot >= 456) {
                gb.ppu_dot -= 456;
                LY++;
                if (LY == 144) {
                    set_mode(1);
                    gb.io[IO_IF] |= INT_VBLANK;
                    update_stat_line();
                    platform_frame();
                    hires_frame_done();
                    continue;
                }
                set_mode(2);
            } else {
                break;
            }
        } else {
            if (gb.ppu_dot < 456)
                break;
            gb.ppu_dot -= 456;
            LY++;
            if (LY > 153) {
                LY = 0;
                gb.window_line = 0;
                set_mode(2);
            }
        }
        update_stat_line();
    }
    update_stat_line();
}

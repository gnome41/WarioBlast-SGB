/* Super Game Boy: command packets sent through P1, palettes, attribute
 * maps, VRAM data transfers and the SNES border.  SNES-side sound commands
 * (SOUND, SOU_TRN) are accepted and ignored. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gb.h"
#include "mods.h"

enum {
    PAL01 = 0x00, PAL23, PAL03, PAL12, ATTR_BLK, ATTR_LIN, ATTR_DIV, ATTR_CHR,
    SOUND, SOU_TRN, PAL_SET, PAL_TRN, ATRC_EN, TEST_EN, ICON_EN, DATA_SND,
    DATA_TRN, MLT_REQ, JUMP, CHR_TRN, PCT_TRN, ATTR_TRN, ATTR_SET, MASK_EN, OBJ_TRN,
};

static struct {
    /* packet receiver */
    uint8_t prev_p1;
    bool receiving;
    int bit;
    uint8_t packet[16];
    uint8_t cmd[7 * 16];
    int got, total;   /* packets received / expected for the current command */

    /* multiplayer */
    int players, player;

    uint16_t pal[4][4];
    uint16_t sys_pal[512][4];
    uint8_t attr[18][20];
    uint8_t attr_files[45][90];
    int mask;

    uint8_t border_tiles[256 * 32];
    uint16_t border_map[32 * 32];
    uint16_t border_pal[4][16];
    bool has_border;

    int pending_trn;    /* command waiting for the next frame's VRAM capture */
    uint8_t trn_arg;
    uint32_t frozen[144][160];
    uint32_t frozen_wide[144][WIDE_W];
} s;

static const uint16_t default_pal[4] = { 0x67BF, 0x265B, 0x10B5, 0x2866 };

void sgb_reset(void)
{
    memset(&s, 0, sizeof s);
    s.prev_p1 = 0x30;
    s.players = 1;
    for (int p = 0; p < 4; p++)
        memcpy(s.pal[p], default_pal, sizeof default_pal);
    s.pending_trn = -1;
}

bool sgb_active(void) { return opt.sgb; }

int sgb_player_count(void) { return s.players; }

uint8_t sgb_p1_read_ids(void)
{
    return 0x0F - (s.players > 1 ? s.player : 0);
}

static uint16_t rd16(const uint8_t *p) { return p[0] | (p[1] << 8); }

static void set_shared_color0(uint16_t c)
{
    for (int p = 0; p < 4; p++)
        s.pal[p][0] = c;
}

static void cmd_pal(int a, int b)
{
    const uint8_t *d = s.cmd + 1;
    set_shared_color0(rd16(d));
    for (int i = 0; i < 3; i++) {
        s.pal[a][i + 1] = rd16(d + 2 + i * 2);
        s.pal[b][i + 1] = rd16(d + 8 + i * 2);
    }
}

static void cmd_attr_blk(void)
{
    int n = s.cmd[1];
    for (int k = 0; k < n && k < 18; k++) {
        const uint8_t *d = s.cmd + 2 + k * 6;
        uint8_t ctrl = d[0] & 7, pals = d[1];
        int x1 = d[2] & 31, y1 = d[3] & 31, x2 = d[4] & 31, y2 = d[5] & 31;
        int pin = pals & 3, pbor = (pals >> 2) & 3, pout = (pals >> 4) & 3;
        bool in = ctrl & 1, bor = ctrl & 2, out = ctrl & 4;
        if (in && !bor && !out) { bor = true; pbor = pin; }
        if (out && !bor && !in) { bor = true; pbor = pout; }
        for (int y = 0; y < 18; y++)
            for (int x = 0; x < 20; x++) {
                bool inside = x > x1 && x < x2 && y > y1 && y < y2;
                bool edge = !inside && x >= x1 && x <= x2 && y >= y1 && y <= y2;
                if (inside && in) s.attr[y][x] = pin;
                else if (edge && bor) s.attr[y][x] = pbor;
                else if (!inside && !edge && out) s.attr[y][x] = pout;
            }
    }
}

static void cmd_attr_lin(void)
{
    int n = s.cmd[1];
    for (int k = 0; k < n && k < 110; k++) {
        uint8_t d = s.cmd[2 + k];
        int line = d & 31, pal = (d >> 5) & 3;
        if (d & 0x80) {
            if (line < 18)
                for (int x = 0; x < 20; x++) s.attr[line][x] = pal;
        } else if (line < 20) {
            for (int y = 0; y < 18; y++) s.attr[y][line] = pal;
        }
    }
}

static void cmd_attr_div(void)
{
    uint8_t d = s.cmd[1];
    int c = s.cmd[2] & 31;
    int after = d & 3, before = (d >> 2) & 3, on = (d >> 4) & 3;
    for (int y = 0; y < 18; y++)
        for (int x = 0; x < 20; x++) {
            int v = (d & 0x40) ? y : x;
            s.attr[y][x] = v < c ? before : v == c ? on : after;
        }
}

static void cmd_attr_chr(void)
{
    int x = s.cmd[1] % 20, y = s.cmd[2] % 18;
    int n = rd16(s.cmd + 3);
    bool vertical = s.cmd[5] & 1;
    for (int i = 0; i < n && i < 360 && 6 + i / 4 < (int)sizeof s.cmd; i++) {
        uint8_t b = s.cmd[6 + i / 4];
        s.attr[y][x] = (b >> (6 - 2 * (i & 3))) & 3;
        if (vertical) {
            if (++y == 18) { y = 0; x = (x + 1) % 20; }
        } else {
            if (++x == 20) { x = 0; y = (y + 1) % 18; }
        }
    }
}

static void apply_attr_file(int f)
{
    if (f >= 45)
        return;
    for (int i = 0; i < 360; i++)
        s.attr[i / 20][i % 20] = (s.attr_files[f][i / 4] >> (6 - 2 * (i & 3))) & 3;
}

static void cmd_pal_set(void)
{
    for (int p = 0; p < 4; p++) {
        int n = rd16(s.cmd + 1 + p * 2) & 511;
        memcpy(s.pal[p], s.sys_pal[n], sizeof s.pal[p]);
    }
    set_shared_color0(s.pal[0][0]);
    uint8_t f = s.cmd[9];
    if (f & 0x80)
        apply_attr_file(f & 0x3F);
    if (f & 0x40)
        s.mask = 0;
}

static void execute(void)
{
    int c = s.cmd[0] >> 3;
    static int log = -1;
    if (log < 0)
        log = getenv("WB_SGB_LOG") != NULL;
    if (log)
    {
        fprintf(stderr, "sgb frame %llu: cmd %02X len %d args", (unsigned long long)gb.frame_count, c, s.cmd[0] & 7);
        for (int i = 1; i < 16; i++)
            fprintf(stderr, " %02X", s.cmd[i]);
        fprintf(stderr, "\n");
    }
    switch (c) {
    case PAL01: cmd_pal(0, 1); break;
    case PAL23: cmd_pal(2, 3); break;
    case PAL03: cmd_pal(0, 3); break;
    case PAL12: cmd_pal(1, 2); break;
    case ATTR_BLK: cmd_attr_blk(); break;
    case ATTR_LIN: cmd_attr_lin(); break;
    case ATTR_DIV: cmd_attr_div(); break;
    case ATTR_CHR: cmd_attr_chr(); break;
    case PAL_SET: cmd_pal_set(); break;
    case SOUND:
        sgb_sound_command(s.cmd);
        modaudio_on_sgb_sound(s.cmd);
        break;
    case MLT_REQ:
        s.players = (s.cmd[1] & 3) == 3 ? 4 : (s.cmd[1] & 3) + 1;
        if (s.players == 3)
            s.players = 2;
        s.player = 0;
        break;
    case ATTR_SET:
        apply_attr_file(s.cmd[1] & 0x3F);
        if (s.cmd[1] & 0x40)
            s.mask = 0;
        break;
    case MASK_EN:
        s.mask = s.cmd[1] & 3;
        break;
    case PAL_TRN: case CHR_TRN: case PCT_TRN: case ATTR_TRN: case SOU_TRN:
        s.pending_trn = c;
        s.trn_arg = s.cmd[1];
        break;
    default:
        break;  /* sound, SNES memory and misc commands are not emulated */
    }
}

static void packet_done(void)
{
    if (s.got == 0) {
        s.total = s.packet[0] & 7;
        if (s.total == 0)
            return;
        memset(s.cmd, 0, sizeof s.cmd);
    }
    memcpy(s.cmd + s.got * 16, s.packet, 16);
    if (++s.got == s.total) {
        s.got = 0;
        execute();
    }
}

void sgb_p1_write(uint8_t v)
{
    uint8_t sel = v & 0x30, prev = s.prev_p1;
    s.prev_p1 = sel;

    if (s.players > 1 && (sel & 0x20) && !(prev & 0x20) && !s.receiving)
        s.player = (s.player + 1) % s.players;

    if (sel == 0x00) {          /* reset pulse: start of a packet */
        s.receiving = true;
        s.bit = 0;
        memset(s.packet, 0, sizeof s.packet);
        return;
    }
    if (!s.receiving || prev != 0x30 || sel == 0x30)
        return;
    int b = sel == 0x10 ? 1 : 0;   /* P15 low = 1, P14 low = 0 */
    if (s.bit < 128) {
        if (b)
            s.packet[s.bit / 8] |= 1 << (s.bit & 7);
        s.bit++;
        return;
    }
    s.receiving = false;           /* stop bit */
    packet_done();
}

/* Reconstruct the 4 KiB the SNES would capture from the displayed screen:
 * 256 tiles laid out 20 per row in the BG map. */
static void capture_vram(uint8_t *out)
{
    uint8_t lcdc = gb.io[IO_LCDC];
    unsigned map = (lcdc & 0x08) ? 0x1C00 : 0x1800;
    for (int i = 0; i < 256; i++) {
        uint8_t t = gb.vram[map + (i / 20) * 32 + i % 20];
        unsigned addr = (lcdc & 0x10) ? t * 16 : 0x1000 + (int8_t)t * 16;
        memcpy(out + i * 16, &gb.vram[addr], 16);
    }
}

void sgb_frame(void)
{
    sgb_sound_frame();
    if (s.pending_trn < 0)
        return;
    uint8_t buf[4096];
    capture_vram(buf);
    switch (s.pending_trn) {
    case PAL_TRN:
        for (int i = 0; i < 512 * 4; i++)
            s.sys_pal[i / 4][i % 4] = rd16(buf + i * 2);
        break;
    case CHR_TRN:
        if (!(s.trn_arg & 2))
            memcpy(s.border_tiles + (s.trn_arg & 1) * 4096, buf, 4096);
        break;
    case PCT_TRN:
        for (int i = 0; i < 32 * 32; i++)
            s.border_map[i] = rd16(buf + i * 2);
        for (int i = 0; i < 64; i++)
            s.border_pal[i / 16][i % 16] = rd16(buf + 0x800 + i * 2);
        s.has_border = true;
        break;
    case ATTR_TRN:
        memcpy(s.attr_files, buf, sizeof s.attr_files);
        break;
    case SOU_TRN:
        sgb_sound_transfer(buf, sizeof buf);
        break;
    }
    s.pending_trn = -1;
}

static uint32_t rgb(uint16_t c)
{
    uint32_t r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
    r = (r << 3) | (r >> 2);
    g = (g << 3) | (g >> 2);
    b = (b << 3) | (b >> 2);
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

void sgb_render(uint32_t *out)
{
    uint32_t backdrop = rgb(s.pal[0][0]);
    for (int i = 0; i < 256 * 224; i++)
        out[i] = backdrop;

    for (int y = 0; y < 144; y++)
        for (int x = 0; x < 160; x++) {
            uint32_t c;
            if (s.mask == 1)
                c = s.frozen[y][x];
            else if (s.mask == 2)
                c = 0xFF000000u;
            else if (s.mask == 3)
                c = backdrop;
            else {
                uint8_t sh = ppu_shades[y][x];
                c = rgb(sh ? s.pal[s.attr[y / 8][x / 8]][sh] : s.pal[0][0]);
            }
            if (s.mask != 1)
                s.frozen[y][x] = c;
            out[(y + 40) * 256 + x + 48] = c;
        }

    if (!s.has_border)
        return;
    for (int ty = 0; ty < 28; ty++)
        for (int tx = 0; tx < 32; tx++) {
            uint16_t e = s.border_map[ty * 32 + tx];
            const uint8_t *t = s.border_tiles + (e & 0xFF) * 32;
            const uint16_t *pal = s.border_pal[((e >> 10) & 7) & 3];
            for (int py = 0; py < 8; py++) {
                int row = (e & 0x8000) ? 7 - py : py;
                for (int px = 0; px < 8; px++) {
                    int b = (e & 0x4000) ? px : 7 - px;
                    int ci = ((t[row * 2] >> b) & 1) | (((t[row * 2 + 1] >> b) & 1) << 1) |
                             (((t[16 + row * 2] >> b) & 1) << 2) | (((t[16 + row * 2 + 1] >> b) & 1) << 3);
                    if (ci)
                        out[(ty * 8 + py) * 256 + tx * 8 + px] = rgb(pal[ci]);
                }
            }
        }
}

/* Widescreen view: the game image only (no border), with the attribute
 * map's edge cells extended over the extra columns. */
void sgb_render_wide(uint32_t *out)
{
    uint32_t backdrop = rgb(s.pal[0][0]);
    for (int y = 0; y < 144; y++)
        for (int x = 0; x < WIDE_W; x++) {
            uint32_t c;
            if (s.mask == 1) {
                c = s.frozen_wide[y][x];
            } else if (s.mask == 2) {
                c = 0xFF000000u;
            } else if (s.mask == 3) {
                c = backdrop;
            } else {
                int cx = x / 8 - WIDE_EXTRA / 8;
                cx = cx < 0 ? 0 : cx > 19 ? 19 : cx;
                uint8_t sh = ppu_wide[y][x];
                c = rgb(sh ? s.pal[s.attr[y / 8][cx]][sh] : s.pal[0][0]);
                s.frozen_wide[y][x] = c;
            }
            out[y * WIDE_W + x] = c;
        }
}

/* ---- accessors for the HD compositor (hires.c) ---- */

int sgb_mask_mode(void) { return s.mask; }
uint32_t sgb_backdrop(void) { return rgb(s.pal[0][0]); }

/* Colour of a GB pixel with the given shade in attribute cell (cx, cy). */
uint32_t sgb_pixel_color(int cx, int cy, int shade)
{
    cx = cx < 0 ? 0 : cx > 19 ? 19 : cx;
    cy = cy < 0 ? 0 : cy > 17 ? 17 : cy;
    return rgb(shade ? s.pal[s.attr[cy][cx]][shade] : s.pal[0][0]);
}

bool sgb_has_border(void) { return s.has_border; }
uint32_t sgb_border_color(int pal, int ci) { return rgb(s.border_pal[pal & 3][ci & 15]); }
uint16_t sgb_border_entry(int tx, int ty) { return s.border_map[ty * 32 + tx]; }
const uint8_t *sgb_border_tile(int tile) { return s.border_tiles + (tile & 0xFF) * 32; }

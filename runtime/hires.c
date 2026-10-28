/* HD compositor for mod packs.
 *
 * While a frame renders, the PPU records for every pixel which tile (and
 * which pixel of it) the background/window and sprite layers came from
 * (hires.h).  At the end of the frame this builds the current view at the
 * pack's scale S: the normal output is upscaled, then the game area is
 * redrawn block by block, using replacement art where a pack has it, and
 * the SGB border likewise.
 *
 * Colour handling:
 *  - color=rgba art is used as is, but shifted by the difference between
 *    the current palette and the pack's reference palette, so fades to
 *    white/black still work;
 *  - color=indexed art is read as colour indices (white = 0 ... black = 3)
 *    and goes through the real palettes, so fades and SGB colours are exact.
 * Render-only: nothing here affects emulation. */
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "hires.h"

PixRef hires_refs[144][160];
PixRef hires_refs_wide[144][WIDE_W];
bool hires_captured;

#define MAX_INST 4096
#define INST_HASH 8192
TileInst hires_inst[MAX_INST];
int hires_inst_count;
static uint16_t inst_slot[INST_HASH];
static uint32_t inst_gen[INST_HASH], gen = 1;

static const uint32_t dmg_colors[4] = { 0xFFE0F8D0, 0xFF88C070, 0xFF346856, 0xFF081820 };

bool hires_capture_active(void)
{
    return (mods_have_graphics() && mods_enabled()) || moddump_active();
}

uint16_t hires_instance(const uint8_t *tile16, uint8_t pal, uint8_t kind)
{
    hires_captured = true;
    uint32_t h = 2166136261u ^ pal ^ ((uint32_t)kind << 8);
    for (int i = 0; i < 16; i++)
        h = (h ^ tile16[i]) * 16777619u;
    for (uint32_t i = h & (INST_HASH - 1);; i = (i + 1) & (INST_HASH - 1)) {
        if (inst_gen[i] != gen) {
            if (hires_inst_count >= MAX_INST)
                return NO_INST;
            TileInst *t = &hires_inst[hires_inst_count];
            memcpy(t->data, tile16, 16);
            t->pal = pal;
            t->kind = kind;
            t->tile = mods_tile(tile16, pal);
            inst_gen[i] = gen;
            inst_slot[i] = (uint16_t)hires_inst_count;
            return (uint16_t)hires_inst_count++;
        }
        TileInst *t = &hires_inst[inst_slot[i]];
        if (t->pal == pal && t->kind == kind && !memcmp(t->data, tile16, 16))
            return inst_slot[i];
    }
}

void hires_frame_done(void)
{
    gen++;
    hires_inst_count = 0;
    hires_captured = false;
}

/* ---- colour helpers ---------------------------------------------------- */

static inline uint32_t native_color(int xs, int y, int shade)
{
    if (opt.sgb)
        return sgb_pixel_color(xs < 0 ? 0 : xs / 8, y / 8, shade);
    return dmg_colors[shade & 3];
}

static inline int clamp255(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

static inline uint32_t shift_color(uint32_t art_rgb, int dr, int dg, int db)
{
    int r = clamp255((int)((art_rgb >> 16) & 0xFF) + dr);
    int g = clamp255((int)((art_rgb >> 8) & 0xFF) + dg);
    int b = clamp255((int)(art_rgb & 0xFF) + db);
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

/* Colour index from an indexed-art pixel: white 0 ... black 3. */
static inline int art_index(const uint8_t *p)
{
    int lum = (p[0] * 3 + p[1] * 6 + p[2]) / 10;
    int idx = (255 - lum + 42) / 85;
    return idx < 0 ? 0 : idx > 3 ? 3 : idx;
}

static inline const uint8_t *art_px(const ModTile *t, int sub, int i, int j, int S, int flags)
{
    int St = t->scale;
    int si = (flags & SPF_XFLIP) ? S - 1 - i : i;
    int sj = (flags & SPF_YFLIP) ? S - 1 - j : j;
    int ax = (sub & 7) * St + si * St / S;
    int ay = (sub >> 3) * St + sj * St / S;
    return &t->img.rgba[(ay * t->img.w + ax) * 4];
}

/* ---- game area ---------------------------------------------------------- */

static inline uint32_t delta_rgb(uint32_t cur, uint32_t ref, int *dr, int *dg, int *db)
{
    *dr = (int)((cur >> 16) & 0xFF) - (int)((ref >> 16) & 0xFF);
    *dg = (int)((cur >> 8) & 0xFF) - (int)((ref >> 8) & 0xFF);
    *db = (int)(cur & 0xFF) - (int)(ref & 0xFF);
    return *dr | *dg | *db;
}

/* Draws cols x 144 captured pixels (first column at screen x xs0) at
 * (ox, oy) native coordinates of an output with the given stride. */
static void draw_game(const PixRef *refs, int ref_stride, int cols, int xs0,
                      uint32_t *out, int stride, int ox, int oy, int S)
{
    for (int y = 0; y < 144; y++) {
        const PixRef *row = refs + (size_t)y * ref_stride;
        for (int c = 0; c < cols; c++) {
            const PixRef *r = &row[c];
            int xs = xs0 + c;
            uint32_t *blk = out + (size_t)((oy + y) * S) * stride + (size_t)(ox + c) * S;

            /* background / window */
            const TileInst *bi = r->bg != NO_INST ? &hires_inst[r->bg] : NULL;
            const ModTile *bt = bi ? bi->tile : NULL;
            uint32_t flat = native_color(xs, y, r->bg_shade);
            if (!bt) {
                for (int j = 0; j < S; j++)
                    for (int i = 0; i < S; i++)
                        blk[j * stride + i] = flat;
            } else {
                uint32_t idx0 = native_color(xs, y, bi->pal & 3);
                int dr = 0, dg = 0, db = 0;
                bool shift = !bt->indexed &&
                             delta_rgb(flat, native_color(xs, y, (bt->ref_pal[0] >> (r->bg_idx * 2)) & 3), &dr, &dg, &db);
                if (!bt->indexed && !shift && bt->scale == S) {
                    /* fast path: art at output scale, no fade shift */
                    const uint32_t *src = bt->img.argb + (size_t)((r->bg_sub >> 3) * S) * bt->img.w + (r->bg_sub & 7) * S;
                    for (int j = 0; j < S; j++, src += bt->img.w)
                        for (int i = 0; i < S; i++)
                            blk[j * stride + i] = (src[i] >> 24) >= 128 ? src[i] | 0xFF000000u : idx0;
                } else {
                    for (int j = 0; j < S; j++)
                        for (int i = 0; i < S; i++) {
                            const uint8_t *p = art_px(bt, r->bg_sub, i, j, S, 0);
                            uint32_t col;
                            if (p[3] < 128)
                                col = idx0;
                            else if (bt->indexed)
                                col = native_color(xs, y, (bi->pal >> (art_index(p) * 2)) & 3);
                            else
                                col = shift_color(((uint32_t)p[0] << 16) | (p[1] << 8) | p[2], dr, dg, db);
                            blk[j * stride + i] = col;
                        }
                }
            }

            /* sprite */
            if (r->sp == NO_INST)
                continue;
            const TileInst *si = &hires_inst[r->sp];
            const ModTile *st = si->tile;
            if (!st) {
                if (r->sp_flags & SPF_DRAWN) {
                    uint32_t col = native_color(xs, y, r->sp_shade);
                    for (int j = 0; j < S; j++)
                        for (int i = 0; i < S; i++)
                            blk[j * stride + i] = col;
                }
                continue;
            }
            if ((r->sp_flags & SPF_BEHIND) && r->bg_idx)
                continue;
            int dr = 0, dg = 0, db = 0;
            bool shift = false;
            if (!st->indexed && r->sp_idx) {
                int ref_pal = st->ref_pal[(r->sp_flags & SPF_OBP1) ? 2 : 1];
                shift = delta_rgb(native_color(xs, y, r->sp_shade),
                                  native_color(xs, y, (ref_pal >> (r->sp_idx * 2)) & 3), &dr, &dg, &db);
            }
            if (!st->indexed && !shift && st->scale == S) {
                bool xf = r->sp_flags & SPF_XFLIP, yf = r->sp_flags & SPF_YFLIP;
                const uint32_t *base = st->img.argb + (size_t)((r->sp_sub >> 3) * S) * st->img.w + (r->sp_sub & 7) * S;
                for (int j = 0; j < S; j++) {
                    const uint32_t *src = base + (size_t)(yf ? S - 1 - j : j) * st->img.w;
                    for (int i = 0; i < S; i++) {
                        uint32_t px = src[xf ? S - 1 - i : i];
                        if ((px >> 24) >= 128)
                            blk[j * stride + i] = px | 0xFF000000u;
                    }
                }
                continue;
            }
            for (int j = 0; j < S; j++)
                for (int i = 0; i < S; i++) {
                    const uint8_t *p = art_px(st, r->sp_sub, i, j, S, r->sp_flags);
                    if (p[3] < 128)
                        continue;
                    if (st->indexed) {
                        int idx = art_index(p);
                        if (!idx)
                            continue;   /* colour 0 is transparent for sprites */
                        blk[j * stride + i] = native_color(xs, y, (si->pal >> (idx * 2)) & 3);
                    } else {
                        blk[j * stride + i] = shift_color(((uint32_t)p[0] << 16) | (p[1] << 8) | p[2], dr, dg, db);
                    }
                }
        }
    }
}

/* ---- SGB border (cached) --------------------------------------------------- */

static inline uint32_t blend(uint32_t dst, uint32_t src)
{
    unsigned a = src >> 24;
    if (a == 255)
        return src;
    unsigned r = (((src >> 16) & 0xFF) * a + ((dst >> 16) & 0xFF) * (255 - a)) / 255;
    unsigned g = (((src >> 8) & 0xFF) * a + ((dst >> 8) & 0xFF) * (255 - a)) / 255;
    unsigned b = ((src & 0xFF) * a + (dst & 0xFF) * (255 - a)) / 255;
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

static bool in_game(int nx, int ny) { return nx >= 48 && nx < 208 && ny >= 40 && ny < 184; }

/* The 256x224 SGB layer at scale S without the game: the original border
 * upscaled, with the pack's border image / tiles on top.  Rebuilt only when
 * the native border pixels, the pack border or the scale change. */
static uint32_t *border_cache;
static uint32_t *border_over;    /* border pixels over the game area (alpha), or NULL */
static uint64_t border_key;

static void build_border(const uint32_t *native, int nstride, int bx, int by, int S)
{
    size_t n = (size_t)256 * S * 224 * S;
    free(border_cache);
    free(border_over);
    border_cache = malloc(n * sizeof *border_cache);
    border_over = NULL;
    if (!border_cache)
        return;
    int W = 256 * S;
    for (int y = 0; y < 224 * S; y++)
        for (int x = 0; x < W; x++)
            border_cache[(size_t)y * W + x] = native[(by + y / S) * nstride + bx + x / S];
    uint32_t backdrop = sgb_backdrop();
    int bs;
    const ModImage *img = mods_border_image(&bs);
    if (img) {
        border_over = calloc(n, sizeof *border_over);
        for (int y = 0; y < 224 * S; y++)
            for (int x = 0; x < W; x++) {
                uint32_t src = img->argb[(size_t)(y * bs / S) * img->w + x * bs / S];
                uint32_t *d = &border_cache[(size_t)y * W + x];
                if (in_game(x / S, y / S)) {
                    if (border_over)
                        border_over[(size_t)y * W + x] = src;   /* blended over the game per frame */
                } else {
                    *d = (src >> 24) ? blend(backdrop, src) : backdrop;
                }
            }
        return;
    }
    if (!sgb_has_border())
        return;
    for (int ty = 0; ty < 28; ty++)
        for (int tx = 0; tx < 32; tx++) {
            uint16_t e = sgb_border_entry(tx, ty);
            const ModTile *t = mods_border_tile(sgb_border_tile(e & 0xFF));
            if (!t)
                continue;
            for (int j = 0; j < 8 * S; j++)
                for (int i = 0; i < 8 * S; i++) {
                    int nx = tx * 8 + i / S, ny = ty * 8 + j / S;
                    if (in_game(nx, ny))
                        continue;
                    int si = (e & 0x4000) ? 8 * S - 1 - i : i, sj = (e & 0x8000) ? 8 * S - 1 - j : j;
                    uint32_t src = t->img.argb[(size_t)(sj * t->scale / S) * t->img.w + si * t->scale / S];
                    uint32_t *d = &border_cache[(size_t)(ty * 8 * S + j) * W + tx * 8 * S + i];
                    *d = blend(backdrop, src);
                }
        }
}

static void draw_border(const uint32_t *native, int nstride, uint32_t *out, int stride, int bx, int by, int S)
{
    int bs;
    const void *src = mods_border_image(&bs);
    uint64_t key = 1469598103934665603ull ^ (uint64_t)(uintptr_t)src ^ ((uint64_t)S << 48) ^ ((uint64_t)bx << 32);
    for (int y = 0; y < 224; y++)
        for (int x = 0; x < 256; x++)
            if (!in_game(x, y))
                key = (key ^ native[(by + y) * nstride + bx + x]) * 1099511628211ull;
    if (key != border_key || !border_cache) {
        build_border(native, nstride, bx, by, S);
        border_key = key;
    }
    if (!border_cache)
        return;
    int W = 256 * S;
    for (int y = 0; y < 224 * S; y++) {
        uint32_t *d = out + (size_t)(by * S + y) * stride + bx * S;
        const uint32_t *c = border_cache + (size_t)y * W;
        int ny = y / S;
        if (ny < 40 || ny >= 184) {
            memcpy(d, c, (size_t)W * sizeof *d);
            continue;
        }
        memcpy(d, c, (size_t)48 * S * sizeof *d);                       /* left of the game */
        memcpy(d + 208 * S, c + 208 * S, (size_t)48 * S * sizeof *d);  /* right of it */
        if (border_over)
            for (int x = 48 * S; x < 208 * S; x++)
                if (border_over[(size_t)y * W + x] >> 24)
                    d[x] = blend(d[x], border_over[(size_t)y * W + x]);
    }
}

/* ---- view composition ---------------------------------------------------- */

static void upscale(const uint32_t *native, int nw, int nh, uint32_t *out, int S)
{
    int stride = nw * S;
    for (int y = 0; y < nh; y++) {
        uint32_t *d = out + (size_t)y * S * stride;
        const uint32_t *srow = native + (size_t)y * nw;
        for (int x = 0; x < nw; x++)
            for (int i = 0; i < S; i++)
                d[x * S + i] = srow[x];
        for (int j = 1; j < S; j++)
            memcpy(d + (size_t)j * stride, d, (size_t)stride * sizeof *d);
    }
}

bool hires_compose(int view, const uint32_t *native, int nw, int nh, int max_scale,
                   uint32_t **out, int *w, int *h)
{
    int S = mods_scale();
    if (max_scale > 0 && S > max_scale)
        S = max_scale;
    if (S <= 1 || !mods_enabled() || !mods_have_graphics())
        return false;
    static uint32_t *buf;
    static size_t cap;
    size_t need = (size_t)nw * S * nh * S;
    if (need > cap) {
        free(buf);
        buf = malloc(need * sizeof *buf);
        cap = buf ? need : 0;
        if (!buf)
            return false;
    }
    int stride = nw * S;
    upscale(native, nw, nh, buf, S);

    bool game = hires_captured && !(opt.sgb && sgb_mask_mode());
    if (view == VIEW_WIDE) {
        if (game && wide_has_content)
            draw_game(&hires_refs_wide[0][0], WIDE_W, WIDE_W, -WIDE_EXTRA, buf, stride, 0, 0, S);
        else if (game)
            draw_game(&hires_refs[0][0], 160, 160, 0, buf, stride, WIDE_EXTRA, 0, S);
    } else {
        int bx = 0, by = 0;           /* where the 256x224 SGB layer (or 160x144 DMG screen) starts */
        if (view == VIEW_FRAME)
            bx = opt.sgb ? (400 - 256) / 2 : (256 - 160) / 2;
        int gx = bx + (opt.sgb ? 48 : 0), gy = by + (opt.sgb ? 40 : 0);
        if (game)
            draw_game(&hires_refs[0][0], 160, 160, 0, buf, stride, gx, gy, S);
        if (opt.sgb)
            draw_border(native, nw, buf, stride, bx, by, S);
    }
    *out = buf;
    *w = nw * S;
    *h = nh * S;
    return true;
}

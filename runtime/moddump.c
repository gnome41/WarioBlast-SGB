/* Dump tool for modders: writes every tile the game draws (once), the SGB
 * border, a log of music/effect requests and optionally recordings of the
 * original music, plus a pack.ini template.  The output contains the
 * game's own graphics/sound - it is for local use when making a pack. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hires.h"
#include "files.h"
#include "third_party/stb_image_write.h"

static bool active, record_audio;
static char root[700];
static FILE *log_file;

/* tiles already written: key = 16 bytes (+ pal/kind for the shaded copy) */
typedef struct { uint8_t k[18]; bool used; } Seen;
static Seen *seen;
static size_t seen_cap, seen_n;
static unsigned pal_count[3][256];
static uint64_t border_hash;
static int tiles_written, border_tiles_written;

/* music recording */
static FILE *wav;
static uint32_t wav_frames;
static int wav_id = -1;

static bool seen_insert(const uint8_t *k, int n)
{
    if (seen_n * 2 >= seen_cap) {
        size_t old = seen_cap;
        Seen *o = seen;
        seen_cap = seen_cap ? seen_cap * 2 : 4096;
        seen = calloc(seen_cap, sizeof *seen);
        seen_n = 0;
        for (size_t i = 0; i < old; i++)
            if (o[i].used)
                seen_insert(o[i].k, 18);
        free(o);
    }
    uint8_t key[18] = { 0 };
    memcpy(key, k, n);
    uint32_t h = 2166136261u;
    for (int i = 0; i < 18; i++)
        h = (h ^ key[i]) * 16777619u;
    for (size_t i = h & (seen_cap - 1);; i = (i + 1) & (seen_cap - 1)) {
        if (!seen[i].used) {
            memcpy(seen[i].k, key, 18);
            seen[i].used = true;
            seen_n++;
            return true;
        }
        if (!memcmp(seen[i].k, key, 18))
            return false;
    }
}

typedef struct { uint8_t *p; size_t n, cap; } Mem;

static void mem_cb(void *ctx, void *data, int size)
{
    Mem *m = ctx;
    if (m->n + size > m->cap) {
        m->cap = (m->n + size) * 2;
        m->p = realloc(m->p, m->cap);
    }
    memcpy(m->p + m->n, data, size);
    m->n += size;
}

static void write_png(const char *path, const uint8_t *rgba, int w, int h)
{
    Mem m = { 0 };
    if (stbi_write_png_to_func(mem_cb, &m, w, h, 4, rgba, w * 4))
        file_write(path, m.p, m.n);
    free(m.p);
}

static void hex(char *out, const uint8_t *b, int n)
{
    for (int i = 0; i < n; i++)
        sprintf(out + i * 2, "%02x", b[i]);
}

bool moddump_start(const char *dir, bool audio)
{
    snprintf(root, sizeof root, "%s", dir);
    char p[800];
    const char *sub[] = { "tiles", "tiles_seen", "border", "audio" };
    for (int i = 0; i < 4; i++) {
        snprintf(p, sizeof p, "%s/%s", root, sub[i]);
        make_dirs(p);
    }
    snprintf(p, sizeof p, "%s/audio_log.txt", root);
    log_file = fopen(p, "w");
    if (!log_file) {
        fprintf(stderr, "dump: cannot write to %s\n", root);
        return false;
    }
    fprintf(log_file, "# frame  kind  id (hex) - name replacements audio/<kind>_<id>.ogg\n");
    active = true;
    record_audio = audio;
    modaudio_watch = true;
    fprintf(stderr, "dumping tiles, border and audio requests to %s\n", root);
    return true;
}

bool moddump_active(void) { return active; }

static void dump_tiles(void)
{
    static const uint8_t grey[4] = { 255, 170, 85, 0 };
    char name[64], path[900];
    for (int i = 0; i < hires_inst_count; i++) {
        const TileInst *t = &hires_inst[i];
        uint8_t rgba[8 * 8 * 4], key[18];
        /* the editable tile: colour indices as grey (white = 0); colour 0 is
         * transparent, which suits both uses - sprites show what is behind,
         * backgrounds show the palette's colour 0 */
        if (seen_insert(t->data, 16)) {
            for (int y = 0; y < 8; y++)
                for (int x = 0; x < 8; x++) {
                    int c = ((t->data[y * 2] >> (7 - x)) & 1) | (((t->data[y * 2 + 1] >> (7 - x)) & 1) << 1);
                    uint8_t *p = &rgba[(y * 8 + x) * 4];
                    p[0] = p[1] = p[2] = grey[c];
                    p[3] = c ? 255 : 0;
                }
            hex(name, t->data, 16);
            snprintf(path, sizeof path, "%s/tiles/%s.png", root, name);
            write_png(path, rgba, 8, 8);
            tiles_written++;
        }
        /* as seen: with the palette it was drawn with */
        memcpy(key, t->data, 16);
        key[16] = t->pal;
        key[17] = (uint8_t)(0x80 | t->kind);
        if (seen_insert(key, 18)) {
            for (int y = 0; y < 8; y++)
                for (int x = 0; x < 8; x++) {
                    int c = ((t->data[y * 2] >> (7 - x)) & 1) | (((t->data[y * 2 + 1] >> (7 - x)) & 1) << 1);
                    uint8_t *p = &rgba[(y * 8 + x) * 4];
                    p[0] = p[1] = p[2] = grey[(t->pal >> (c * 2)) & 3];
                    p[3] = (t->kind == 1 && c == 0) ? 0 : 255;
                }
            hex(name, t->data, 16);
            snprintf(path, sizeof path, "%s/tiles_seen/%s_%02x%s.png", root, name, t->pal,
                     t->kind ? "_sprite" : "");
            write_png(path, rgba, 8, 8);
        }
    }
}

/* border tiles already written (32-byte keys, by 64-bit hash) */
static bool border_tile_new(const uint8_t *t)
{
    static uint64_t done[1024];
    static int n;
    uint64_t h = 1469598103934665603ull;
    for (int k = 0; k < 32; k++)
        h = (h ^ t[k]) * 1099511628211ull;
    for (int i = 0; i < n; i++)
        if (done[i] == h)
            return false;
    if (n < 1024)
        done[n++] = h;
    return true;
}

static void dump_border(void)
{
    if (!opt.sgb || !sgb_has_border())
        return;
    uint64_t h = 1469598103934665603ull;
    for (int i = 0; i < 32 * 28; i++) {
        uint16_t e = sgb_border_entry(i % 32, i / 32);
        const uint8_t *t = sgb_border_tile(e & 0xFF);
        h = (h ^ e) * 1099511628211ull;
        for (int k = 0; k < 32; k++)
            h = (h ^ t[k]) * 1099511628211ull;
    }
    if (h == border_hash)
        return;
    border_hash = h;
    static uint8_t img[256 * 224 * 4];
    char name[80], path[900];
    for (int ty = 0; ty < 28; ty++)
        for (int tx = 0; tx < 32; tx++) {
            uint16_t e = sgb_border_entry(tx, ty);
            const uint8_t *t = sgb_border_tile(e & 0xFF);
            uint8_t tile[8 * 8 * 4];
            for (int py = 0; py < 8; py++)
                for (int px = 0; px < 8; px++) {
                    int b = 7 - px;
                    int ci = ((t[py * 2] >> b) & 1) | (((t[py * 2 + 1] >> b) & 1) << 1) |
                             (((t[16 + py * 2] >> b) & 1) << 2) | (((t[16 + py * 2 + 1] >> b) & 1) << 3);
                    uint32_t c = sgb_border_color(((e >> 10) & 7) & 3, ci);
                    uint8_t *p = &tile[(py * 8 + px) * 4];
                    p[0] = (c >> 16) & 0xFF;
                    p[1] = (c >> 8) & 0xFF;
                    p[2] = c & 0xFF;
                    p[3] = ci ? 255 : 0;
                }
            /* the per-tile file is unflipped; the whole image shows the layout */
            hex(name, t, 32);
            snprintf(path, sizeof path, "%s/border/%s.png", root, name);
            if (border_tile_new(t)) {
                write_png(path, tile, 8, 8);
                border_tiles_written++;
            }
            for (int py = 0; py < 8; py++)
                for (int px = 0; px < 8; px++) {
                    int sx = (e & 0x4000) ? 7 - px : px, sy = (e & 0x8000) ? 7 - py : py;
                    memcpy(&img[((ty * 8 + py) * 256 + tx * 8 + px) * 4], &tile[(sy * 8 + sx) * 4], 4);
                }
        }
    snprintf(path, sizeof path, "%s/border/border.png", root);
    write_png(path, img, 256, 224);
}

void moddump_frame(void)
{
    if (!active)
        return;
    if (gb.io[IO_LCDC] & 0x80) {
        pal_count[0][gb.io[IO_BGP]]++;
        pal_count[1][gb.io[IO_OBP0]]++;
        pal_count[2][gb.io[IO_OBP1]]++;
    }
    dump_tiles();
    dump_border();
    if (log_file)
        fflush(log_file);
}

/* ---- audio ------------------------------------------------------------- */

static void wav_close(void)
{
    if (!wav)
        return;
    uint32_t data = wav_frames * 4;
    uint8_t h[44] = "RIFF....WAVEfmt \x10\0\0\0\1\0\2\0....\0\0\0\0\4\0\x10\0data....";
    uint32_t v[] = { 36 + data, 48000, 48000 * 4, data };
    memcpy(h + 4, &v[0], 4);
    memcpy(h + 24, &v[1], 4);
    memcpy(h + 28, &v[2], 4);
    memcpy(h + 40, &v[3], 4);
    fseek(wav, 0, SEEK_SET);
    fwrite(h, 1, 44, wav);
    fclose(wav);
    wav = NULL;
}

void moddump_audio_event(const char *kind, int id)
{
    if (log_file)
        fprintf(log_file, "%llu %s %02x\n", (unsigned long long)gb.frame_count, kind, id);
    if (!record_audio || strcmp(kind, "music"))
        return;
    wav_close();
    wav_id = -1;
    if (id == 0x80)
        return;
    char path[900];
    snprintf(path, sizeof path, "%s/audio/music_%02x_original.wav", root, id);
    if (file_exists(path))
        return;   /* first recording of each song is kept */
    wav = fopen(path, "wb");
    if (wav) {
        static const uint8_t zero[44];
        fwrite(zero, 1, sizeof zero, wav);   /* header written on close */
        wav_frames = 0;
        wav_id = id;
    }
}

void moddump_audio_samples(const int16_t *buf, size_t frames)
{
    if (wav && wav_id >= 0) {
        fwrite(buf, 4, frames, wav);
        wav_frames += (uint32_t)frames;
    }
}

void moddump_finish(void)
{
    if (!active)
        return;
    wav_close();
    int best[3] = { 0xE4, 0xE4, 0xE4 };
    for (int k = 0; k < 3; k++) {
        unsigned most = 0;
        for (int p = 0; p < 256; p++)
            if (pal_count[k][p] > most) {
                most = pal_count[k][p];
                best[k] = p;
            }
    }
    char path[900], text[1024];
    snprintf(path, sizeof path, "%s/pack.ini", root);
    int n = snprintf(text, sizeof text,
                     "# Template written by the dump. Start a pack by copying this file and\n"
                     "# the tiles you redraw into mods/<YourPack>/ (see MODDING.md).\n"
                     "name = My pack\nauthor =\nversion = 1.0\ndescription =\n"
                     "scale = 4\ncolor = rgba\n"
                     "# palettes the game uses most (the colours your rgba art is drawn for)\n"
                     "ref_bgp = %02X\nref_obp0 = %02X\nref_obp1 = %02X\n", best[0], best[1], best[2]);
    file_write(path, text, (size_t)n);
    if (log_file)
        fclose(log_file);
    log_file = NULL;
    fprintf(stderr, "dump: %d tiles, %d border tiles -> %s\n", tiles_written, border_tiles_written, root);
}

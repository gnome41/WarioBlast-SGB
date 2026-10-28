/* Mod pack loading and lookup.  Packs are folders under mods/; each may
 * contain pack.ini, tiles/, border/ and audio/ (see MODDING.md).  Several
 * packs can be active: they are loaded lowest priority first, so entries
 * of higher-priority packs replace earlier ones. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL.h>
#include "mods.h"
#include "files.h"
#include "third_party/stb_image.h"

/* ---- small helpers ---------------------------------------------------- */

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Parses exactly n bytes of hex from s; returns chars consumed or 0. */
static int parse_hex(const char *s, uint8_t *out, int n)
{
    for (int i = 0; i < n; i++) {
        int a = hexval(s[2 * i]), b = hexval(s[2 * i + 1]);
        if (a < 0 || b < 0)
            return 0;
        out[i] = (uint8_t)(a * 16 + b);
    }
    return 2 * n;
}

static bool ends_with(const char *s, const char *suffix)
{
    size_t a = strlen(s), b = strlen(suffix);
    return a >= b && !SDL_strcasecmp(s + a - b, suffix);
}

typedef void (*IniCallback)(const char *key, const char *value, void *user);

/* key = value lines; '#' / ';' comments; [sections] ignored. */
static bool ini_read(const char *path, IniCallback cb, void *user)
{
    size_t n;
    char *d = (char *)file_read(path, &n, 1 << 20);
    if (!d)
        return false;
    char *text = realloc(d, n + 1);
    if (!text) {
        free(d);
        return false;
    }
    text[n] = 0;
    for (char *line = text, *next; line && *line; line = next) {
        next = strpbrk(line, "\r\n");
        if (next)
            *next++ = 0;
        char *eq = strchr(line, '=');
        if (!eq || line[0] == '#' || line[0] == ';' || line[0] == '[')
            continue;
        *eq = 0;
        char *k = line, *v = eq + 1;
        while (*k == ' ' || *k == '\t') k++;
        while (*v == ' ' || *v == '\t') v++;
        for (char *e = k + strlen(k); e > k && (e[-1] == ' ' || e[-1] == '\t'); ) *--e = 0;
        for (char *e = v + strlen(v); e > v && (e[-1] == ' ' || e[-1] == '\t'); ) *--e = 0;
        cb(k, v, user);
    }
    free(text);
    return true;
}

static bool load_image(const char *path, ModImage *img)
{
    size_t n;
    uint8_t *d = file_read(path, &n, 64 << 20);
    if (!d)
        return false;
    int comp;
    img->rgba = stbi_load_from_memory(d, (int)n, &img->w, &img->h, &comp, 4);
    free(d);
    if (!img->rgba)
        return false;
    img->argb = malloc((size_t)img->w * img->h * sizeof *img->argb);
    if (!img->argb) {
        stbi_image_free(img->rgba);
        return false;
    }
    for (size_t i = 0; i < (size_t)img->w * img->h; i++) {
        const uint8_t *p = &img->rgba[i * 4];
        img->argb[i] = ((uint32_t)p[3] << 24) | ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
    }
    return true;
}

/* ---- pack info ---------------------------------------------------------- */

static void info_ini(const char *k, const char *v, void *user)
{
    ModInfo *m = user;
    if (!SDL_strcasecmp(k, "name")) snprintf(m->name, sizeof m->name, "%s", v);
    else if (!SDL_strcasecmp(k, "author")) snprintf(m->author, sizeof m->author, "%s", v);
    else if (!SDL_strcasecmp(k, "version")) snprintf(m->version, sizeof m->version, "%s", v);
    else if (!SDL_strcasecmp(k, "description")) snprintf(m->description, sizeof m->description, "%s", v);
    else if (!SDL_strcasecmp(k, "scale")) m->scale = atoi(v);
    else if (!SDL_strcasecmp(k, "color")) m->indexed = !SDL_strcasecmp(v, "indexed");
}

typedef struct { int *count; const char *ext1, *ext2; } CountCtx;

static void count_files(const char *name, bool is_dir, void *user)
{
    CountCtx *c = user;
    if (!is_dir && (ends_with(name, c->ext1) || (c->ext2 && ends_with(name, c->ext2))))
        (*c->count)++;
}

static void read_info(const char *mods_dir, const char *dir, ModInfo *m)
{
    char path[700];
    memset(m, 0, sizeof *m);
    snprintf(m->dir, sizeof m->dir, "%s", dir);
    snprintf(m->name, sizeof m->name, "%s", dir);
    snprintf(path, sizeof path, "%s/%s/pack.ini", mods_dir, dir);
    ini_read(path, info_ini, m);
    snprintf(path, sizeof path, "%s/%s/tiles", mods_dir, dir);
    dir_list(path, count_files, &(CountCtx){ &m->tiles, ".png", NULL });
    snprintf(path, sizeof path, "%s/%s/border", mods_dir, dir);
    int border_files = 0;
    dir_list(path, count_files, &(CountCtx){ &border_files, ".png", NULL });
    snprintf(path, sizeof path, "%s/%s/border/border.png", mods_dir, dir);
    m->border = file_exists(path);
    m->border_tiles = border_files - (m->border ? 1 : 0);
    snprintf(path, sizeof path, "%s/%s/audio", mods_dir, dir);
    dir_list(path, count_files, &(CountCtx){ &m->audio, ".ogg", ".wav" });
}

typedef struct { const char *mods_dir; ModInfo *out; int max, n; } ScanCtx;

static void scan_one(const char *name, bool is_dir, void *user)
{
    ScanCtx *c = user;
    if (!is_dir || name[0] == '_' || c->n >= c->max)   /* _dump etc. are not packs */
        return;
    read_info(c->mods_dir, name, &c->out[c->n++]);
}

static int cmp_info(const void *a, const void *b)
{
    return SDL_strcasecmp(((const ModInfo *)a)->dir, ((const ModInfo *)b)->dir);
}

int mods_scan(const char *mods_dir, ModInfo *out, int max)
{
    ScanCtx c = { mods_dir, out, max, 0 };
    dir_list(mods_dir, scan_one, &c);
    qsort(out, c.n, sizeof *out, cmp_info);
    return c.n;
}

const char *mods_default_dir(void)
{
    static char path[700];
    if (!path[0]) {
        char *base = SDL_GetBasePath();
        snprintf(path, sizeof path, "%smods", base ? base : "");
        SDL_free(base);
    }
    return path;
}

/* ---- loaded state ------------------------------------------------------- */

#define ANY_PAL 0x100

typedef struct { uint8_t key[32]; int keylen; int pal; ModTile tile; bool used; } Slot;

static Slot *table;            /* open addressing, power of two */
static size_t table_cap, table_used;
static ModImage border_img;
static int border_scale;
static ModAudioRef *audio;
static char (*audio_names)[40];
static int n_audio, cap_audio;
static int out_scale = 1;
static bool loaded_gfx, loaded_audio, toggled_off;

static uint64_t hash_key(const uint8_t *k, int n, int pal)
{
    uint64_t h = 1469598103934665603ull ^ (uint64_t)n;
    for (int i = 0; i < n; i++)
        h = (h ^ k[i]) * 1099511628211ull;
    return (h ^ (uint64_t)pal) * 1099511628211ull;
}

static Slot *find_slot(const uint8_t *k, int n, int pal, bool insert)
{
    if (!table_cap || (insert && table_used * 2 >= table_cap)) {
        if (!insert)
            return NULL;
        size_t old_cap = table_cap;
        Slot *old = table;
        table_cap = table_cap ? table_cap * 2 : 1024;
        table = calloc(table_cap, sizeof *table);
        table_used = 0;
        for (size_t i = 0; i < old_cap; i++)
            if (old[i].used) {
                Slot *s = find_slot(old[i].key, old[i].keylen, old[i].pal, true);
                *s = old[i];
                table_used++;
            }
        free(old);
    }
    size_t i = (size_t)hash_key(k, n, pal) & (table_cap - 1);
    for (;;) {
        Slot *s = &table[i];
        if (!s->used)
            return insert ? s : NULL;
        if (s->keylen == n && s->pal == pal && !memcmp(s->key, k, n))
            return s;
        i = (i + 1) & (table_cap - 1);
    }
}

static void put_tile(const uint8_t *k, int n, int pal, const ModTile *t)
{
    Slot *s = find_slot(k, n, pal, true);
    if (s->used) {                           /* replaced by a higher-priority pack */
        stbi_image_free(s->tile.img.rgba);
        free(s->tile.img.argb);
    }
    else
        table_used++;
    memcpy(s->key, k, n);
    s->keylen = n;
    s->pal = pal;
    s->tile = *t;
    s->used = true;
    if (t->scale > out_scale)
        out_scale = t->scale;
}

typedef struct {
    const char *dir;
    const ModInfo *info;
    uint8_t ref_pal[3];
    bool border;     /* loading border/ instead of tiles/ */
} LoadCtx;

static void load_tile_file(const char *name, bool is_dir, void *user)
{
    LoadCtx *c = user;
    int keylen = c->border ? 32 : 16;
    uint8_t key[32];
    if (is_dir || !ends_with(name, ".png") || !parse_hex(name, key, keylen))
        return;
    const char *rest = name + keylen * 2;
    int pal = ANY_PAL;
    uint8_t p;
    if (!c->border && rest[0] == '_' && parse_hex(rest + 1, &p, 1) && !SDL_strcasecmp(rest + 3, ".png"))
        pal = p;
    else if (SDL_strcasecmp(rest, ".png"))
        return;
    char path[900];
    snprintf(path, sizeof path, "%s/%s", c->dir, name);
    ModTile t = { 0 };
    if (!load_image(path, &t.img))
        return;
    if (t.img.w != t.img.h || t.img.w % 8 || t.img.w < 8 || t.img.w > 8 * 16) {
        fprintf(stderr, "mods: %s: must be square, 8 x scale pixels (scale 1-16)\n", path);
        stbi_image_free(t.img.rgba);
        free(t.img.argb);
        return;
    }
    t.scale = t.img.w / 8;
    t.indexed = c->info->indexed;
    memcpy(t.ref_pal, c->ref_pal, 3);
    put_tile(key, keylen, pal, &t);
    loaded_gfx = true;
}

static void ref_ini(const char *k, const char *v, void *user)
{
    uint8_t *ref = user;
    uint8_t b;
    if (!parse_hex(v, &b, 1))
        return;
    if (!SDL_strcasecmp(k, "ref_bgp")) ref[0] = b;
    else if (!SDL_strcasecmp(k, "ref_obp0")) ref[1] = b;
    else if (!SDL_strcasecmp(k, "ref_obp1")) ref[2] = b;
}

typedef struct { const char *dir; } AudioCtx;

static void loop_ini(const char *k, const char *v, void *user)
{
    ModAudioRef *a = user;
    if (!SDL_strcasecmp(k, "loop")) a->loop = atoi(v) != 0;
    else if (!SDL_strcasecmp(k, "loop_start")) a->loop_start = atol(v);
    else if (!SDL_strcasecmp(k, "loop_end")) a->loop_end = atol(v);
}

static void load_audio_file(const char *name, bool is_dir, void *user)
{
    AudioCtx *c = user;
    if (is_dir || !(ends_with(name, ".ogg") || ends_with(name, ".wav")))
        return;
    char base[40];
    size_t len = strlen(name) - 4;
    if (len >= sizeof base)
        return;
    memcpy(base, name, len);
    base[len] = 0;
    for (char *p = base; *p; p++)
        *p = (char)SDL_tolower(*p);
    int idx = -1;
    for (int i = 0; i < n_audio; i++)
        if (!strcmp(audio_names[i], base))
            idx = i;   /* a higher-priority pack replaces the entry */
    if (idx < 0) {
        if (n_audio == cap_audio) {
            cap_audio = cap_audio ? cap_audio * 2 : 32;
            audio = realloc(audio, cap_audio * sizeof *audio);
            audio_names = realloc(audio_names, cap_audio * sizeof *audio_names);
        }
        idx = n_audio++;
    }
    snprintf(audio_names[idx], sizeof audio_names[idx], "%s", base);
    ModAudioRef *a = &audio[idx];
    memset(a, 0, sizeof *a);
    snprintf(a->path, sizeof a->path, "%s/%s", c->dir, name);
    a->loop = !strncmp(base, "music_", 6);   /* music loops by default */
    char ini[760];
    snprintf(ini, sizeof ini, "%s/%s.ini", c->dir, base);
    ini_read(ini, loop_ini, a);
    loaded_audio = true;
}

int mods_load(const char *mods_dir, const char *const *names, int count)
{
    int loaded = 0;
    for (int i = count - 1; i >= 0; i--) {   /* lowest priority first */
        ModInfo info;
        read_info(mods_dir, names[i], &info);
        char dir[700], path[760];
        snprintf(dir, sizeof dir, "%s/%s", mods_dir, names[i]);
        snprintf(path, sizeof path, "%s/pack.ini", dir);
        uint8_t ref[3] = { 0xE4, 0xE4, 0xE4 };
        ini_read(path, ref_ini, ref);

        LoadCtx lc = { path, &info, { ref[0], ref[1], ref[2] }, false };
        snprintf(path, sizeof path, "%s/tiles", dir);
        dir_list(path, load_tile_file, &lc);
        lc.border = true;
        snprintf(path, sizeof path, "%s/border", dir);
        dir_list(path, load_tile_file, &lc);

        snprintf(path, sizeof path, "%s/border/border.png", dir);
        ModImage img;
        if (file_exists(path) && load_image(path, &img)) {
            if (img.w % 256 || img.h != img.w / 256 * 224) {
                fprintf(stderr, "mods: %s: must be 256 x 224 times the scale\n", path);
                stbi_image_free(img.rgba);
                free(img.argb);
            } else {
                stbi_image_free(border_img.rgba);
                free(border_img.argb);
                border_img = img;
                border_scale = img.w / 256;
                if (border_scale > out_scale)
                    out_scale = border_scale;
                loaded_gfx = true;
            }
        }
        AudioCtx ac = { path };
        snprintf(path, sizeof path, "%s/audio", dir);
        dir_list(path, load_audio_file, &ac);
        fprintf(stderr, "mod pack \"%s\" by %s: %d tiles, %s border, %d border tiles, %d audio files\n",
                info.name, info.author[0] ? info.author : "unknown", info.tiles,
                info.border ? "HD" : "no", info.border_tiles, info.audio);
        loaded++;
    }
    return loaded;
}

bool mods_have_graphics(void) { return loaded_gfx; }
bool mods_have_audio(void) { return loaded_audio; }
bool mods_enabled(void) { return (loaded_gfx || loaded_audio) && !toggled_off; }
void mods_toggle(void) { toggled_off = !toggled_off; }
int mods_scale(void) { return loaded_gfx && !toggled_off ? out_scale : 1; }

const ModTile *mods_tile(const uint8_t key[16], uint8_t pal)
{
    if (!loaded_gfx || toggled_off)
        return NULL;
    Slot *s = find_slot(key, 16, pal, false);
    if (!s)
        s = find_slot(key, 16, ANY_PAL, false);
    return s ? &s->tile : NULL;
}

const ModTile *mods_border_tile(const uint8_t key[32])
{
    if (!loaded_gfx || toggled_off)
        return NULL;
    Slot *s = find_slot(key, 32, ANY_PAL, false);
    return s ? &s->tile : NULL;
}

const ModImage *mods_border_image(int *scale)
{
    if (!border_img.rgba || toggled_off)
        return NULL;
    *scale = border_scale;
    return &border_img;
}

const ModAudioRef *mods_audio(const char *kind, int id)
{
    if (!loaded_audio || toggled_off)
        return NULL;
    char name[40];
    snprintf(name, sizeof name, "%s_%02x", kind, id);
    for (int i = 0; i < n_audio; i++)
        if (!strcmp(audio_names[i], name))
            return &audio[i];
    return NULL;
}

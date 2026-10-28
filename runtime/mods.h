/* Mod packs: HD tile replacements, HD Super Game Boy border and
 * music/sound replacements.  See MODDING.md for the pack format. */
#ifndef MODS_H
#define MODS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MODS_MAX_PACKS 16

typedef struct {
    int w, h;
    uint8_t *rgba;      /* w * h * 4 */
    uint32_t *argb;     /* same pixels as 0xAARRGGBB, for fast copying */
} ModImage;

typedef struct {
    ModImage img;
    int scale;          /* img.w / 8 */
    bool indexed;       /* grey levels are colour indices (pack color=indexed) */
    uint8_t ref_pal[3]; /* reference BGP, OBP0, OBP1 of the pack (color=rgba fades) */
} ModTile;

typedef struct {
    char dir[128];      /* folder name */
    char name[96], author[96], version[32], description[200];
    int scale;
    bool indexed;
    int tiles, border_tiles, audio;
    bool border;
} ModInfo;

/* listing (launcher) */
int mods_scan(const char *mods_dir, ModInfo *out, int max);
const char *mods_default_dir(void);

/* loading: names in priority order, first = highest priority */
int mods_load(const char *mods_dir, const char *const *names, int count);
bool mods_have_graphics(void);
bool mods_have_audio(void);
bool mods_enabled(void);         /* active and not toggled off with F9 */
void mods_toggle(void);
int mods_scale(void);            /* output scale: largest image scale among loaded packs */

const ModTile *mods_tile(const uint8_t key[16], uint8_t pal);
const ModTile *mods_border_tile(const uint8_t key[32]);
const ModImage *mods_border_image(int *scale);

/* audio: kind = "music", "sfx", "sgb_score", "sgb_a", "sgb_b"; NULL if none */
typedef struct {
    char path[700];
    bool loop;
    long loop_start, loop_end;   /* in samples of the source file; loop_end 0 = end */
} ModAudioRef;
const ModAudioRef *mods_audio(const char *kind, int id);

/* modaudio.c */
void modaudio_on_write(uint16_t addr, uint8_t value);   /* write watch on the request variables */
void modaudio_on_sgb_sound(const uint8_t *packet);
bool modaudio_sgb_muted(void);
void modaudio_frame(void);
void modaudio_mix(int16_t *buf, size_t frames, int rate);
extern bool modaudio_watch;      /* mem_write checks request addresses only when set */

/* hires.c: capture + compositor */
bool hires_capture_active(void);
void hires_frame_done(void);
/* max_scale caps the output scale (0 = the packs' full scale) */
bool hires_compose(int view, const uint32_t *native, int nw, int nh, int max_scale,
                   uint32_t **out, int *w, int *h);

/* moddump.c */
bool moddump_start(const char *dir, bool audio);
bool moddump_active(void);
void moddump_frame(void);
void moddump_finish(void);
void moddump_audio_event(const char *kind, int id);
void moddump_audio_samples(const int16_t *buf, size_t frames);

#endif

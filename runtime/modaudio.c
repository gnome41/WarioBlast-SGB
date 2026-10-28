/* Music / sound replacement for mod packs.
 *
 * The game asks its sound driver for a song by writing the request
 * variable (game_audio.music_request: a song id, or music_stop) and for an
 * effect by writing sfx_request; a write watch in mem_write reports those
 * here.  When a pack has a replacement it is played, and the original is
 * silenced at the output stage only:
 *  - replacement music mutes every Game Boy channel that no effect is
 *    using (game_audio.sfx_channel_mask), so the game's own effects still
 *    play;
 *  - a replacement effect mutes the channels the original effect occupies.
 * Super Game Boy SNES-side sounds are replaced via the SOUND command.
 * Emulation is unaffected. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gb.h"
#include "mods.h"
#include "files.h"
#include "recomp/recomp.h"
#define STB_VORBIS_HEADER_ONLY
#define STB_VORBIS_NO_STDIO
#include "third_party/stb_vorbis.c"

#define OUT_RATE 48000
#define MAX_SFX 8

typedef struct {
    char path[700];
    int16_t *pcm;           /* stereo, OUT_RATE */
    size_t frames;
    bool loop;
    size_t loop_start, loop_end;
} Track;

typedef struct {
    Track *t;
    size_t pos;
} Voice;

bool modaudio_watch;
static Track **cache;          /* tracks are allocated individually: voices keep pointers */
static int n_cache, cap_cache;
static Voice music, sfx[MAX_SFX], sgbv;
static bool paused;

/* ---- decoding ----------------------------------------------------------- */

static int16_t *resample(const int16_t *in, size_t frames, int ch, int rate, size_t *out_frames)
{
    if (rate <= 0 || ch <= 0)
        return NULL;
    size_t n = (size_t)((double)frames * OUT_RATE / rate);
    int16_t *out = malloc((n ? n : 1) * 2 * sizeof *out);
    if (!out)
        return NULL;
    for (size_t i = 0; i < n; i++) {
        double src = (double)i * rate / OUT_RATE;
        size_t a = (size_t)src;
        double f = src - a;
        size_t b = a + 1 < frames ? a + 1 : a;
        for (int c = 0; c < 2; c++) {
            int sc = c < ch ? c : 0;
            out[i * 2 + c] = (int16_t)(in[a * ch + sc] * (1 - f) + in[b * ch + sc] * f);
        }
    }
    *out_frames = n;
    return out;
}

static uint32_t le32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

/* PCM 8/16-bit or 32-bit float WAV -> interleaved int16 */
static int16_t *decode_wav(const uint8_t *d, size_t n, int *ch, int *rate, size_t *frames)
{
    if (n < 12 || memcmp(d, "RIFF", 4) || memcmp(d + 8, "WAVE", 4))
        return NULL;
    int fmt = 0, bits = 0;
    *ch = 0;
    for (size_t p = 12; p + 8 <= n;) {
        uint32_t len = le32(d + p + 4);
        const uint8_t *c = d + p + 8;
        if (p + 8 + len > n)
            break;
        if (!memcmp(d + p, "fmt ", 4) && len >= 16) {
            fmt = le16(c);
            *ch = le16(c + 2);
            *rate = (int)le32(c + 4);
            bits = le16(c + 14);
            if (fmt == 0xFFFE && len >= 26)
                fmt = le16(c + 24);   /* WAVE_FORMAT_EXTENSIBLE: sub-format */
        } else if (!memcmp(d + p, "data", 4) && *ch) {
            int bps = bits / 8;
            if (!((fmt == 1 && (bits == 8 || bits == 16)) || (fmt == 3 && bits == 32)))
                return NULL;
            *frames = len / (bps * *ch);
            int16_t *out = malloc((*frames ? *frames : 1) * *ch * sizeof *out);
            if (!out)
                return NULL;
            for (size_t i = 0; i < *frames * *ch; i++) {
                const uint8_t *s = c + i * bps;
                if (bits == 8)
                    out[i] = (int16_t)((s[0] - 128) << 8);
                else if (bits == 16)
                    out[i] = (int16_t)le16(s);
                else {
                    float f;
                    memcpy(&f, s, 4);
                    f = f < -1 ? -1 : f > 1 ? 1 : f;
                    out[i] = (int16_t)(f * 32767);
                }
            }
            return out;
        }
        p += 8 + len + (len & 1);
    }
    return NULL;
}

static Track *load_track(const ModAudioRef *ref)
{
    for (int i = 0; i < n_cache; i++)
        if (!strcmp(cache[i]->path, ref->path))
            return cache[i]->pcm ? cache[i] : NULL;   /* NULL: failed before */
    if (n_cache == cap_cache) {
        int cap = cap_cache ? cap_cache * 2 : 16;
        Track **nc = realloc(cache, cap * sizeof *cache);
        if (!nc)
            return NULL;
        cache = nc;
        cap_cache = cap;
    }
    Track *t = calloc(1, sizeof *t);
    if (!t)
        return NULL;
    cache[n_cache++] = t;
    snprintf(t->path, sizeof t->path, "%s", ref->path);
    size_t n;
    uint8_t *d = file_read(ref->path, &n, 256u << 20);
    if (!d) {
        fprintf(stderr, "mods: cannot read %s\n", ref->path);
        return NULL;
    }
    int ch = 0, rate = 0;
    size_t frames = 0;
    int16_t *pcm = NULL;
    size_t len = strlen(ref->path);
    if (len > 4 && !strcmp(ref->path + len - 4, ".ogg")) {
        short *out = NULL;
        int f = stb_vorbis_decode_memory(d, (int)n, &ch, &rate, &out);
        if (f > 0) {
            pcm = out;
            frames = (size_t)f;
        }
    } else {
        pcm = decode_wav(d, n, &ch, &rate, &frames);
    }
    free(d);
    if (!pcm) {
        fprintf(stderr, "mods: cannot decode %s (OGG Vorbis or PCM/float WAV expected)\n", ref->path);
        return NULL;
    }
    t->pcm = resample(pcm, frames, ch, rate, &t->frames);
    free(pcm);
    if (!t->pcm)
        return NULL;
    double k = (double)OUT_RATE / rate;
    t->loop = ref->loop;
    t->loop_start = (size_t)(ref->loop_start * k);
    t->loop_end = ref->loop_end > 0 ? (size_t)(ref->loop_end * k) : t->frames;
    if (t->loop_end > t->frames)
        t->loop_end = t->frames;
    if (t->loop_start >= t->loop_end)
        t->loop_start = 0;
    return t;
}

static bool start(Voice *v, const char *kind, int id)
{
    const ModAudioRef *ref = mods_audio(kind, id);
    Track *t = ref ? load_track(ref) : NULL;
    v->t = t;
    v->pos = 0;
    return t != NULL;
}

/* ---- hooks ------------------------------------------------------------- */

void modaudio_on_write(uint16_t addr, uint8_t value)
{
    static int dbg = -1;
    if (dbg < 0)
        dbg = getenv("WB_DEBUG") != NULL;
    if (dbg)
        fprintf(stderr, "modaudio: frame %llu write %04X=%02X\n", (unsigned long long)gb.frame_count, addr, value);
    if (value & game_audio.ack_mask)
        return;   /* the driver marking a request as taken */
    if (addr == game_audio.music_request) {
        if (value == game_audio.music_stop) {
            music.t = NULL;
            return;
        }
        if (moddump_active())
            moddump_audio_event("music", value);
        if (!start(&music, "music", value))
            music.t = NULL;   /* no replacement: the original plays */
    } else if (addr == game_audio.sfx_request) {
        if (!value)
            return;
        if (moddump_active())
            moddump_audio_event("sfx", value);
        for (int i = 0; i < MAX_SFX; i++)
            if (!sfx[i].t) {
                start(&sfx[i], "sfx", value);
                break;
            }
    }
}

void modaudio_on_sgb_sound(const uint8_t *packet)
{
    /* SOUND: byte 1 effect A, 2 effect B, 4 music score */
    struct { const char *kind; int id; } req[3] = {
        { "sgb_score", packet[4] }, { "sgb_a", packet[1] }, { "sgb_b", packet[2] },
    };
    for (int i = 0; i < 3; i++) {
        int id = req[i].id;
        if (!id || id == 0x80)
            continue;
        if (moddump_active())
            moddump_audio_event(req[i].kind, id);
        if (mods_audio(req[i].kind, id))
            start(&sgbv, req[i].kind, id);
    }
}

bool modaudio_sgb_muted(void) { return sgbv.t != NULL; }

void modaudio_frame(void)
{
    uint16_t pf = game_audio.pause_flag;
    paused = pf >= 0xC000 && pf < 0xE000 && (gb.wram[pf - 0xC000] & game_audio.pause_mask);
    if (!mods_enabled()) {
        apu_set_mute_mask(0);
        return;
    }
    uint8_t mute = 0;
    bool sfx_playing = false;
    for (int i = 0; i < MAX_SFX; i++)
        sfx_playing |= sfx[i].t != NULL;
    uint16_t ma = game_audio.sfx_channel_mask;
    uint8_t effects = ma >= 0xC000 && ma < 0xE000 ? gb.wram[ma - 0xC000] : 0;
    for (int c = 0; c < 4; c++) {
        bool effect_on_channel = effects >> c & 1;
        if (music.t && !effect_on_channel)
            mute |= 1 << c;
        if (sfx_playing && effect_on_channel)
            mute |= 1 << c;
    }
    apu_set_mute_mask(mute);
    static int dbg = -1;
    if (dbg < 0)
        dbg = getenv("WB_DEBUG") != NULL;
    if (dbg)
        fprintf(stderr, "modaudio: frame %llu music=%s paused=%d mute=%X\n", (unsigned long long)gb.frame_count,
                music.t ? "replaced" : "original", paused, mute);
}

static void mix_voice(Voice *v, int32_t *acc, size_t frames)
{
    for (size_t i = 0; i < frames && v->t; i++) {
        Track *t = v->t;
        if (v->pos >= (t->loop ? t->loop_end : t->frames)) {
            if (!t->loop) {
                v->t = NULL;
                break;
            }
            v->pos = t->loop_start;
        }
        acc[i * 2] += t->pcm[v->pos * 2];
        acc[i * 2 + 1] += t->pcm[v->pos * 2 + 1];
        v->pos++;
    }
}

void modaudio_mix(int16_t *buf, size_t frames, int rate)
{
    if (rate != OUT_RATE || !mods_enabled())
        return;
    static int32_t acc[8192 * 2];
    if (frames > 8192)
        frames = 8192;
    for (size_t i = 0; i < frames * 2; i++)
        acc[i] = buf[i];
    if (!paused)
        mix_voice(&music, acc, frames);
    for (int i = 0; i < MAX_SFX; i++)
        mix_voice(&sfx[i], acc, frames);
    mix_voice(&sgbv, acc, frames);
    for (size_t i = 0; i < frames * 2; i++)
        buf[i] = (int16_t)(acc[i] < -32768 ? -32768 : acc[i] > 32767 ? 32767 : acc[i]);
}

/* S-DSP (SNES sound mixer), modelled one output sample at a time.
 *
 * Written from the publicly documented hardware behaviour (Martin Korth's
 * "fullsnes" and anomie's S-DSP notes): 8 voices playing BRR-compressed
 * samples with 4-point interpolation, ADSR/GAIN envelopes, pitch
 * modulation, a noise generator, and an echo unit with an 8-tap FIR. */
#include <math.h>
#include <stdbool.h>
#include <string.h>
#include "snes_apu.h"

/* global registers */
enum {
    MVOLL = 0x0C, MVOLR = 0x1C, EVOLL = 0x2C, EVOLR = 0x3C, KON = 0x4C, KOFF = 0x5C,
    FLG = 0x6C, ENDX = 0x7C, EFB = 0x0D, PMON = 0x2D, NON = 0x3D, EON = 0x4D,
    DIR = 0x5D, ESA = 0x6D, EDL = 0x7D,
};
/* per-voice registers, at voice * 0x10 */
enum { VOLL, VOLR, PITCHL, PITCHH, SRCN, ADSR1, ADSR2, GAIN, ENVX, OUTX };

enum { STAGE_RELEASE, STAGE_ATTACK, STAGE_DECAY, STAGE_SUSTAIN };

typedef struct {
    bool playing;
    uint16_t block;          /* address of the BRR block being played */
    /* decoded samples (16-bit, always even): [0..2] are the last three of
     * the previous block, [3..18] the current block */
    int16_t smp[19];
    int idx;                 /* position in the current block, 0..15 */
    int phase;               /* 12-bit fraction between samples */
    int level;               /* envelope, 0..0x7FF */
    int level_raw;           /* last computed (unclamped) envelope value */
    int stage;
    int start_delay;         /* silent samples after key-on */
} Voice;

static uint8_t reg[128];
static Voice vc[8];
static uint8_t keyon_latch;
static int tick;             /* shared envelope/noise rate counter */
static uint16_t lfsr;
static int16_t fir_buf[8][2];
static int fir_pos;
static unsigned echo_pos, echo_len;
static int16_t gauss[512];

#define FIFO 8192
static int16_t fifo[FIFO * 2];
static size_t fifo_head, fifo_tail;

static inline uint8_t *vreg(int v, int r) { return &reg[v * 0x10 + r]; }

static int sat16(int x) { return x > 32767 ? 32767 : x < -32768 ? -32768 : x; }

/* ---- rate timing ----------------------------------------------------
 * Envelope steps and the noise clock are paced by one counter shared by
 * all voices.  It counts down from 0x77FF once per sample; rate r fires
 * when (counter + phase) is a multiple of the period.  Rate 0 never
 * fires.  The period / phase values are the hardware's. */
static const struct { uint16_t period, phase; } rates[32] = {
    { 0x7801, 1 }, { 2048, 0 }, { 1536, 1040 }, { 1280, 536 }, { 1024, 0 }, { 768, 1040 },
    { 640, 536 }, { 512, 0 }, { 384, 1040 }, { 320, 536 }, { 256, 0 }, { 192, 1040 },
    { 160, 536 }, { 128, 0 }, { 96, 1040 }, { 80, 536 }, { 64, 0 }, { 48, 1040 },
    { 40, 536 }, { 32, 0 }, { 24, 1040 }, { 20, 536 }, { 16, 0 }, { 12, 1040 },
    { 10, 536 }, { 8, 0 }, { 6, 1040 }, { 5, 536 }, { 4, 0 }, { 3, 1040 }, { 2, 0 }, { 1, 0 },
};

static bool fires(int rate)
{
    return ((unsigned)tick + rates[rate].phase) % rates[rate].period == 0;
}

/* ---- interpolation --------------------------------------------------
 * The hardware weights four consecutive samples with a fixed 512-entry
 * Gaussian table.  Here the table is generated: a normalised Gaussian
 * fitted to the hardware's (centre weight ~0.64, neighbours ~0.18). */
static void make_gauss(void)
{
    const double sigma = 0.63;
    double w[512];
    for (int i = 0; i < 512; i++) {
        double d = 2.0 - i / 256.0;
        w[i] = exp(-d * d / (2 * sigma * sigma));
    }
    for (int f = 0; f < 256; f++) {
        int k[4] = { 255 - f, 511 - f, 256 + f, f };
        double scale = 2048.0 / (w[k[0]] + w[k[1]] + w[k[2]] + w[k[3]]);
        for (int j = 0; j < 4; j++)
            gauss[k[j]] = (int16_t)lrint(w[k[j]] * scale);
    }
}

/* Weighted sum of the four samples ending at the current position.  The
 * first three products are summed with 16-bit wrap-around before the
 * fourth is added and the result clipped, as on the hardware. */
static int interpolate(const Voice *v)
{
    int f = v->phase >> 4;
    const int16_t *s = &v->smp[v->idx];   /* s[0] oldest .. s[3] newest */
    int acc = ((gauss[255 - f] * s[0]) >> 11) + ((gauss[511 - f] * s[1]) >> 11) +
              ((gauss[256 + f] * s[2]) >> 11);
    acc = (int16_t)acc + ((gauss[f] * s[3]) >> 11);
    return sat16(acc) & ~1;
}

/* ---- BRR ------------------------------------------------------------
 * 9-byte blocks: a header (range in bits 4-7, filter in bits 2-3, loop
 * flag bit 1, end flag bit 0) and 16 signed 4-bit samples.  Each sample
 * is scaled by the range, then a filter adds a prediction from the two
 * previous outputs.  Arithmetic is done at 15-bit precision; the result
 * is clipped to 16 bits and then doubled with wrap-around. */
static uint16_t sample_dir(int srcn, bool loop)
{
    uint16_t a = (uint16_t)((reg[DIR] << 8) + srcn * 4 + (loop ? 2 : 0));
    return spc_ram[a] | (spc_ram[(uint16_t)(a + 1)] << 8);
}

static void brr_decode(Voice *v)
{
    uint8_t hdr = spc_ram[v->block];
    int range = hdr >> 4, filter = (hdr >> 2) & 3;
    memmove(v->smp, v->smp + 16, 3 * sizeof v->smp[0]);
    int p1 = v->smp[2] / 2, p2 = v->smp[1] / 2;     /* previous two, 15-bit */
    for (int n = 0; n < 16; n++) {
        uint8_t b = spc_ram[(uint16_t)(v->block + 1 + n / 2)];
        int x = ((n & 1 ? b : b >> 4) & 0x0F);
        x = x >= 8 ? x - 16 : x;
        if (range <= 12)
            x = (x * (1 << range)) >> 1;
        else
            x = x < 0 ? -2048 : 0;
        switch (filter) {
        case 1: x += p1 + ((-p1) >> 4); break;
        case 2: x += 2 * p1 + ((-3 * p1) >> 5) - p2 + (p2 >> 4); break;
        case 3: x += 2 * p1 + ((-13 * p1) >> 6) - p2 + ((3 * p2) >> 4); break;
        }
        int16_t out = (int16_t)(sat16(x) * 2);
        v->smp[3 + n] = out;
        p2 = p1;
        p1 = out / 2;
    }
}

/* Moves to the next BRR block once the current one has been consumed. */
static void brr_advance(Voice *v, int n)
{
    uint8_t hdr = spc_ram[v->block];
    if (hdr & 1) {                        /* end of sample */
        reg[ENDX] |= 1 << n;
        if (hdr & 2) {
            v->block = sample_dir(*vreg(n, SRCN), true);
        } else {
            v->stage = STAGE_RELEASE;
            v->level = 0;
            v->block += 9;
        }
    } else {
        v->block += 9;
    }
    brr_decode(v);
    v->idx = 0;
}

/* ---- envelope -------------------------------------------------------
 * ADSR: attack adds 32 per step (1024 at the fastest rate); decay and
 * sustain decrease exponentially (L -= 1 + (L-1)/256); the decay stage
 * ends at the sustain level ((L >> 8) == SL).  Release subtracts 8 every
 * sample regardless of rate.  GAIN: direct level, or linear /
 * exponential decrease, linear / bent increase (bent: +8 above 0x600). */
static int exp_down(int l) { return l - 1 - ((l - 1) >> 8); }

static void envelope(Voice *v, int n)
{
    if (v->stage == STAGE_RELEASE) {
        v->level = v->level > 8 ? v->level - 8 : 0;
        return;
    }
    uint8_t a1 = *vreg(n, ADSR1), a2 = *vreg(n, ADSR2), g = *vreg(n, GAIN);
    int next = v->level, rate, sustain_ref;
    if (a1 & 0x80) {
        sustain_ref = a2 >> 5;
        if (v->stage == STAGE_ATTACK) {
            rate = (a1 & 0x0F) * 2 + 1;
            next += rate == 31 ? 1024 : 32;
        } else {
            rate = v->stage == STAGE_DECAY ? ((a1 >> 4) & 7) * 2 + 16 : a2 & 0x1F;
            next = exp_down(next);
        }
    } else {
        /* in GAIN mode the decay->sustain check compares with GAIN's top bits */
        sustain_ref = g >> 5;
        if (!(g & 0x80)) {
            next = (g & 0x7F) * 16;
            rate = 31;
        } else {
            rate = g & 0x1F;
            switch ((g >> 5) & 3) {
            case 0: next -= 32; break;
            case 1: next = exp_down(next); break;
            case 2: next += 32; break;
            case 3: next += (unsigned)v->level_raw < 0x600 ? 32 : 8; break;
            }
        }
    }
    if (v->stage == STAGE_DECAY && (next >> 8) == sustain_ref)
        v->stage = STAGE_SUSTAIN;
    v->level_raw = next;
    if (next < 0 || next > 0x7FF) {
        next = next < 0 ? 0 : 0x7FF;
        if (v->stage == STAGE_ATTACK)
            v->stage = STAGE_DECAY;
    }
    if (fires(rate))
        v->level = next;
}

/* ---- voices ---------------------------------------------------------- */

static void start_voice(int n)
{
    Voice *v = &vc[n];
    memset(v, 0, sizeof *v);
    v->playing = true;
    v->block = sample_dir(*vreg(n, SRCN), false);
    v->stage = STAGE_ATTACK;
    v->start_delay = 5;
    reg[ENDX] &= ~(1 << n);
    brr_decode(v);
}

/* One voice for one sample; returns its output (before volume). */
static int run_voice(int n, int prev_out, uint8_t flg)
{
    Voice *v = &vc[n];
    if (!v->playing)
        return 0;
    if ((reg[KOFF] & (1 << n)) || (flg & 0x80)) {
        v->stage = STAGE_RELEASE;
        if (flg & 0x80)
            v->level = 0;
    }
    int step = (*vreg(n, PITCHL) | (*vreg(n, PITCHH) << 8)) & 0x3FFF;
    if (n > 0 && (reg[PMON] & (1 << n)))
        step += ((prev_out >> 5) * step) >> 10;   /* modulated by the previous voice */
    step = step < 0 ? 0 : step > 0x3FFF ? 0x3FFF : step;

    if (v->start_delay > 0) {
        v->start_delay--;
        return 0;
    }
    int s = (reg[NON] & (1 << n)) ? (int16_t)(lfsr * 2) : interpolate(v);
    int out = ((s * v->level) >> 11) & ~1;
    envelope(v, n);
    for (v->phase += step; v->phase >= 0x1000; v->phase -= 0x1000)
        if (++v->idx == 16)
            brr_advance(v, n);
    if (v->stage == STAGE_RELEASE && v->level == 0)
        v->playing = false;
    return out;
}

/* ---- echo -----------------------------------------------------------
 * The echo buffer in SPC RAM (ESA * 256, EDL * 2 KiB) delays the echo
 * mix.  Each sample the delayed value is read, filtered by an 8-tap FIR
 * (taps FIR0..FIR7, oldest to newest; the first seven products wrap at
 * 16 bits before the last is added), fed back with EFB and written back
 * unless FLG bit 5 disables writes. */
static void echo(int in_l, int in_r, uint8_t flg, int out[2])
{
    uint16_t at = (uint16_t)((reg[ESA] << 8) + echo_pos);
    fir_pos = (fir_pos + 1) & 7;
    for (int ch = 0; ch < 2; ch++) {
        uint16_t a = (uint16_t)(at + ch * 2);
        fir_buf[fir_pos][ch] = (int16_t)(spc_ram[a] | (spc_ram[(uint16_t)(a + 1)] << 8)) >> 1;
    }
    for (int ch = 0; ch < 2; ch++) {
        int acc = 0;
        for (int t = 0; t < 7; t++)
            acc += (fir_buf[(fir_pos + 1 + t) & 7][ch] * (int8_t)reg[t * 0x10 + 0x0F]) >> 6;
        acc = (int16_t)acc + ((fir_buf[fir_pos][ch] * (int8_t)reg[0x7F]) >> 6);
        out[ch] = sat16(acc) & ~1;
    }
    if (!(flg & 0x20)) {
        int fb[2] = { in_l, in_r };
        for (int ch = 0; ch < 2; ch++) {
            int w = sat16(fb[ch] + ((out[ch] * (int8_t)reg[EFB]) >> 7)) & ~1;
            uint16_t a = (uint16_t)(at + ch * 2);
            spc_ram[a] = w & 0xFF;
            spc_ram[(uint16_t)(a + 1)] = (w >> 8) & 0xFF;
        }
    }
    if (echo_pos == 0)
        echo_len = (reg[EDL] & 0x0F) ? (reg[EDL] & 0x0F) * 0x800 : 4;
    echo_pos += 4;
    if (echo_pos >= echo_len)
        echo_pos = 0;
}

/* ---- one output sample ----------------------------------------------- */

void dsp_run_sample(void)
{
    tick = tick > 0 ? tick - 1 : 0x77FF;

    for (int n = 0; keyon_latch && n < 8; n++)
        if (keyon_latch & (1 << n))
            start_voice(n);
    keyon_latch = 0;

    uint8_t flg = reg[FLG];
    if (fires(flg & 0x1F))   /* noise: 15-bit LFSR, feedback = bit0 ^ bit1 */
        lfsr = (lfsr >> 1) | (((lfsr << 14) ^ (lfsr << 13)) & 0x4000);

    int mix[2] = { 0, 0 }, emix[2] = { 0, 0 }, prev = 0;
    for (int n = 0; n < 8; n++) {
        int out = run_voice(n, prev, flg);
        prev = out;
        *vreg(n, ENVX) = vc[n].level >> 4;
        *vreg(n, OUTX) = out >> 8;
        for (int ch = 0; ch < 2; ch++) {
            int x = (out * (int8_t)*vreg(n, VOLL + ch)) >> 7;
            mix[ch] = sat16(mix[ch] + x);
            if (reg[EON] & (1 << n))
                emix[ch] = sat16(emix[ch] + x);
        }
    }

    int wet[2];
    echo(emix[0], emix[1], flg, wet);
    int out[2];
    for (int ch = 0; ch < 2; ch++)
        out[ch] = (flg & 0x40) ? 0
                : sat16(((mix[ch] * (int8_t)reg[MVOLL + ch * 0x10]) >> 7) +
                        ((wet[ch] * (int8_t)reg[EVOLL + ch * 0x10]) >> 7));

    size_t next = (fifo_head + 1) % FIFO;
    if (next == fifo_tail)
        fifo_tail = (fifo_tail + 1) % FIFO;   /* consumer stalled: drop the oldest */
    fifo[fifo_head * 2] = (int16_t)out[0];
    fifo[fifo_head * 2 + 1] = (int16_t)out[1];
    fifo_head = next;
}

size_t dsp_take_samples(int16_t *dst, size_t max_frames)
{
    size_t n = 0;
    for (; fifo_tail != fifo_head && n < max_frames; n++) {
        dst[n * 2] = fifo[fifo_tail * 2];
        dst[n * 2 + 1] = fifo[fifo_tail * 2 + 1];
        fifo_tail = (fifo_tail + 1) % FIFO;
    }
    return n;
}

void dsp_reset(void)
{
    memset(reg, 0, sizeof reg);
    memset(vc, 0, sizeof vc);
    memset(fir_buf, 0, sizeof fir_buf);
    reg[FLG] = 0xE0;          /* power-on: soft reset, muted, echo writes off */
    keyon_latch = 0;
    tick = 0;
    lfsr = 0x4000;
    fir_pos = 0;
    echo_pos = 0;
    echo_len = 4;
    fifo_head = fifo_tail = 0;
    make_gauss();
}

uint8_t dsp_read(uint8_t addr) { return reg[addr & 0x7F]; }

void dsp_write(uint8_t addr, uint8_t v)
{
    addr &= 0x7F;
    if (addr == ENDX) {       /* any write clears it */
        reg[ENDX] = 0;
        return;
    }
    reg[addr] = v;
    if (addr == KON)
        keyon_latch |= v;
}

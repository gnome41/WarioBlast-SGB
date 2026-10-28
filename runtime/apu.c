/* DMG APU: two square channels (one with sweep), wave, noise. */
#include <string.h>
#include "gb.h"

typedef struct {
    bool on, dac;
    int length;
    int timer;
    int pos;          /* duty step / wave position */
    int vol, env_timer;
    /* channel 1 sweep */
    int shadow, sweep_timer;
    bool sweep_on;
    uint16_t lfsr;
} Chan;

static Chan ch[4];
static uint8_t regs[0x30];   /* 0xFF10-0xFF3F */
static bool power;
static int seq_timer, seq_step;
static int sample_rate;
static double sample_acc, cycles_per_sample;
static float hp_l, hp_r;

#define RING 16384
static int16_t ring[RING * 2];
static size_t ring_w, ring_r;

#define NR(x) regs[(x) - 0x10]

static int freq(int c)
{
    int base = 0x13 + 5 * c;
    return NR(base) | ((NR(base + 1) & 7) << 8);
}

static int square_period(int c) { return (2048 - freq(c)) * 4; }
static int wave_period(void) { return (2048 - freq(2)) * 2; }

static int noise_period(void)
{
    static const int div[8] = { 8, 16, 32, 48, 64, 80, 96, 112 };
    uint8_t r = NR(0x22);
    return div[r & 7] << (r >> 4);
}

static int sweep_calc(void)
{
    uint8_t r = NR(0x10);
    int d = ch[0].shadow >> (r & 7);
    int f = (r & 8) ? ch[0].shadow - d : ch[0].shadow + d;
    if (f > 2047)
        ch[0].on = false;
    return f;
}

static void trigger(int c)
{
    Chan *k = &ch[c];
    k->on = k->dac;
    if (k->length == 0)
        k->length = c == 2 ? 256 : 64;
    if (c < 2)
        k->timer = square_period(c);
    else if (c == 2) {
        k->timer = wave_period();
        k->pos = 0;
    } else {
        k->timer = noise_period();
        k->lfsr = 0x7FFF;
    }
    if (c != 2) {
        uint8_t env = NR(0x12 + 5 * c);
        k->vol = env >> 4;
        k->env_timer = env & 7 ? env & 7 : 8;
    }
    if (c == 0) {
        uint8_t r = NR(0x10);
        k->shadow = freq(0);
        int p = (r >> 4) & 7;
        k->sweep_timer = p ? p : 8;
        k->sweep_on = p || (r & 7);
        if (r & 7)
            sweep_calc();
    }
}

static void clock_length(void)
{
    for (int c = 0; c < 4; c++) {
        uint8_t nrx4 = NR(0x14 + 5 * c);
        if ((nrx4 & 0x40) && ch[c].length > 0 && --ch[c].length == 0)
            ch[c].on = false;
    }
}

static void clock_env(void)
{
    for (int c = 0; c < 4; c++) {
        if (c == 2)
            continue;
        uint8_t env = NR(0x12 + 5 * c);
        if (!(env & 7))
            continue;
        if (--ch[c].env_timer <= 0) {
            ch[c].env_timer = env & 7;
            if ((env & 8) && ch[c].vol < 15)
                ch[c].vol++;
            else if (!(env & 8) && ch[c].vol > 0)
                ch[c].vol--;
        }
    }
}

static void clock_sweep(void)
{
    Chan *k = &ch[0];
    uint8_t r = NR(0x10);
    if (--k->sweep_timer > 0)
        return;
    int p = (r >> 4) & 7;
    k->sweep_timer = p ? p : 8;
    if (!k->sweep_on || !p)
        return;
    int f = sweep_calc();
    if (f <= 2047 && (r & 7)) {
        k->shadow = f;
        NR(0x13) = f & 0xFF;
        NR(0x14) = (NR(0x14) & ~7) | ((f >> 8) & 7);
        sweep_calc();
    }
}

static int chan_out(int c)
{
    static const uint8_t duty[4] = { 0x01, 0x81, 0x87, 0x7E };
    Chan *k = &ch[c];
    if (!k->on)
        return 0;
    if (c < 2) {
        uint8_t d = duty[NR(0x11 + 5 * c) >> 6];
        return ((d >> (7 - k->pos)) & 1) ? k->vol : 0;
    }
    if (c == 2) {
        int shift = (NR(0x1C) >> 5) & 3;
        if (!shift)
            return 0;
        uint8_t b = NR(0x30 + k->pos / 2);
        int s = (k->pos & 1) ? (b & 0x0F) : (b >> 4);
        return s >> (shift - 1);
    }
    return (k->lfsr & 1) ? 0 : k->vol;
}

static uint8_t mute_mask;

void apu_set_mute_mask(uint8_t mask) { mute_mask = mask; }

static void emit_sample(void)
{
    float l = 0, r = 0;
    uint8_t pan = NR(0x25);
    for (int c = 0; c < 4; c++) {
        if (!ch[c].dac || (mute_mask & (1 << c)))
            continue;
        float v = (chan_out(c) - 7.5f) / 7.5f;
        if (pan & (0x10 << c)) l += v;
        if (pan & (0x01 << c)) r += v;
    }
    uint8_t vol = NR(0x24);
    l *= (((vol >> 4) & 7) + 1) / 8.0f;
    r *= ((vol & 7) + 1) / 8.0f;
    /* DC-blocking high-pass like the hardware's output capacitor */
    float ol = l - hp_l, orr = r - hp_r;
    hp_l = l - ol * 0.996f;
    hp_r = r - orr * 0.996f;
    float scale = 32767.0f / 4.0f * 0.6f;
    size_t next = (ring_w + 1) % RING;
    if (next == ring_r)
        return;  /* consumer is behind: drop */
    ring[ring_w * 2] = (int16_t)(ol * scale);
    ring[ring_w * 2 + 1] = (int16_t)(orr * scale);
    ring_w = next;
}

void apu_init(int rate)
{
    sample_rate = rate;
    cycles_per_sample = (double)CPU_HZ / rate;
    memset(ch, 0, sizeof ch);
    ch[3].lfsr = 0x7FFF;
}

int apu_sample_rate(void) { return sample_rate; }

size_t apu_take_samples(int16_t *out, size_t max_frames)
{
    size_t n = 0;
    while (ring_r != ring_w && n < max_frames) {
        out[n * 2] = ring[ring_r * 2];
        out[n * 2 + 1] = ring[ring_r * 2 + 1];
        ring_r = (ring_r + 1) % RING;
        n++;
    }
    return n;
}

void apu_tick(int n)
{
    if (power) {
        seq_timer -= n;
        while (seq_timer <= 0) {
            seq_timer += 8192;
            if (!(seq_step & 1))
                clock_length();
            if (seq_step == 2 || seq_step == 6)
                clock_sweep();
            if (seq_step == 7)
                clock_env();
            seq_step = (seq_step + 1) & 7;
        }
        for (int c = 0; c < 4; c++) {
            Chan *k = &ch[c];
            k->timer -= n;
            while (k->timer <= 0) {
                if (c < 2) {
                    k->timer += square_period(c);
                    k->pos = (k->pos + 1) & 7;
                } else if (c == 2) {
                    k->timer += wave_period();
                    k->pos = (k->pos + 1) & 31;
                } else {
                    k->timer += noise_period();
                    int x = (k->lfsr ^ (k->lfsr >> 1)) & 1;
                    k->lfsr = (k->lfsr >> 1) | (x << 14);
                    if (NR(0x22) & 8)
                        k->lfsr = (k->lfsr & ~0x40) | (x << 6);
                }
            }
        }
    }
    if (!sample_rate)
        return;
    sample_acc += n;
    while (sample_acc >= cycles_per_sample) {
        sample_acc -= cycles_per_sample;
        emit_sample();
    }
}

uint8_t apu_read(uint16_t addr)
{
    static const uint8_t mask[0x20] = {
        0x80, 0x3F, 0x00, 0xFF, 0xBF, 0xFF, 0x3F, 0x00, 0xFF, 0xBF,
        0x7F, 0xFF, 0x9F, 0xFF, 0xBF, 0xFF, 0xFF, 0x00, 0x00, 0xBF,
        0x00, 0x00, 0x70, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    };
    int r = addr - 0xFF10;
    if (r >= 0x20)
        return regs[r];
    if (addr == 0xFF26) {
        uint8_t v = (power ? 0x80 : 0) | 0x70;
        for (int c = 0; c < 4; c++)
            if (ch[c].on)
                v |= 1 << c;
        return v;
    }
    return regs[r] | mask[r];
}

void apu_write(uint16_t addr, uint8_t v)
{
    int r = addr - 0xFF10;
    if (r >= 0x20) {
        regs[r] = v;
        return;
    }
    if (addr == 0xFF26) {
        bool p = v & 0x80;
        if (power && !p) {
            memset(regs, 0, 0x20);
            for (int c = 0; c < 4; c++)
                ch[c].on = ch[c].dac = false;
        } else if (!power && p) {
            seq_step = 0;
            seq_timer = 8192;
        }
        power = p;
        return;
    }
    if (!power)
        return;
    regs[r] = v;
    switch (addr) {
    case 0xFF11: case 0xFF16: case 0xFF20:
        ch[(r - 1) / 5].length = 64 - (v & 0x3F);
        break;
    case 0xFF1B:
        ch[2].length = 256 - v;
        break;
    case 0xFF12: case 0xFF17: case 0xFF21: {
        int c = (r - 2) / 5;
        ch[c].dac = (v & 0xF8) != 0;
        if (!ch[c].dac)
            ch[c].on = false;
        break;
    }
    case 0xFF1A:
        ch[2].dac = (v & 0x80) != 0;
        if (!ch[2].dac)
            ch[2].on = false;
        break;
    case 0xFF14: case 0xFF19: case 0xFF1E: case 0xFF23:
        if (v & 0x80)
            trigger((r - 4) / 5);
        break;
    }
}

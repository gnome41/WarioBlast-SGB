/* SPC700 CPU with its timers, IO ports and DSP interface. */
#include <stdbool.h>
#include <string.h>
#include "snes_apu.h"

uint8_t spc_ram[0x10000];

enum { FN = 0x80, FV = 0x40, FP = 0x20, FB = 0x10, FH = 0x08, FI = 0x04, FZ = 0x02, FC = 0x01 };

static uint8_t A, X, Y, SP, PSW;
static uint16_t PC;
static bool halted;

static uint8_t port_in[4];   /* written by the SNES CPU */
static uint8_t port_out[4];  /* written by the SPC700 */
static uint8_t control, dsp_addr;
static struct {
    uint8_t target, out;
    int stage;
    bool enabled;
} timer[3];
static int t01_acc, t2_acc, dsp_acc;

static const uint8_t cycle_table[256] = {
    2,8,4,5,3,4,3,6,2,6,5,4,5,4,6,8,  2,8,4,5,4,5,5,6,5,5,6,5,2,2,4,6,
    2,8,4,5,3,4,3,6,2,6,5,4,5,4,5,4,  2,8,4,5,4,5,5,6,5,5,6,5,2,2,3,8,
    2,8,4,5,3,4,3,6,2,6,4,4,5,4,6,6,  2,8,4,5,4,5,5,6,5,5,4,5,2,2,4,3,
    2,8,4,5,3,4,3,6,2,6,4,4,5,4,5,5,  2,8,4,5,4,5,5,6,5,5,5,5,2,2,3,6,
    2,8,4,5,3,4,3,6,2,6,5,4,5,2,4,5,  2,8,4,5,4,5,5,6,5,5,5,5,2,2,12,5,
    3,8,4,5,3,4,3,6,2,6,4,4,5,2,4,4,  2,8,4,5,4,5,5,6,5,5,5,5,2,2,3,4,
    3,8,4,5,4,5,4,7,2,5,6,4,5,2,4,9,  2,8,4,5,5,6,6,7,4,5,5,5,2,2,6,3,
    2,8,4,5,3,4,3,6,2,4,5,3,4,3,4,3,  2,8,4,5,4,5,5,6,3,4,5,4,2,2,4,3,
};

static int extra_cycles;

/* ---------------------------------------------------------------- memory */

static void timer_tick(int i)
{
    if (!timer[i].enabled)
        return;
    if (++timer[i].stage >= (timer[i].target ? timer[i].target : 256)) {
        timer[i].stage = 0;
        timer[i].out = (timer[i].out + 1) & 15;
    }
}

static uint8_t rd(uint16_t a)
{
    if (a >= 0xF0 && a <= 0xFF) {
        switch (a) {
        case 0xF2: return dsp_addr;
        case 0xF3: return dsp_read(dsp_addr & 0x7F);
        case 0xF4: case 0xF5: case 0xF6: case 0xF7: return port_in[a - 0xF4];
        case 0xFD: case 0xFE: case 0xFF: {
            uint8_t v = timer[a - 0xFD].out;
            timer[a - 0xFD].out = 0;
            return v;
        }
        case 0xF8: case 0xF9: return spc_ram[a];
        default: return 0;
        }
    }
    return spc_ram[a];
}

static void wr(uint16_t a, uint8_t v)
{
    if (a >= 0xF0 && a <= 0xFF) {
        switch (a) {
        case 0xF1:
            for (int i = 0; i < 3; i++) {
                bool en = v & (1 << i);
                if (en && !timer[i].enabled) {
                    timer[i].stage = 0;
                    timer[i].out = 0;
                }
                timer[i].enabled = en;
            }
            if (v & 0x10) port_in[0] = port_in[1] = 0;
            if (v & 0x20) port_in[2] = port_in[3] = 0;
            control = v;
            break;
        case 0xF2: dsp_addr = v; break;
        case 0xF3: if (dsp_addr < 0x80) dsp_write(dsp_addr, v); break;
        case 0xF4: case 0xF5: case 0xF6: case 0xF7: port_out[a - 0xF4] = v; break;
        case 0xFA: case 0xFB: case 0xFC: timer[a - 0xFA].target = v; break;
        }
    }
    spc_ram[a] = v;
}

static inline uint16_t dp(uint8_t a) { return ((PSW & FP) ? 0x100 : 0) | a; }
static inline uint8_t fetch(void) { return rd(PC++); }
static inline uint16_t fetch16(void) { uint16_t lo = fetch(); return lo | (fetch() << 8); }
static inline uint16_t rd16dp(uint8_t a) { return rd(dp(a)) | (rd(dp((uint8_t)(a + 1))) << 8); }
static inline void push(uint8_t v) { spc_ram[0x100 | SP--] = v; }
static inline uint8_t pop(void) { return spc_ram[0x100 | ++SP]; }
static inline void push16(uint16_t v) { push(v >> 8); push(v & 0xFF); }
static inline uint16_t pop16(void) { uint16_t lo = pop(); return lo | (pop() << 8); }

/* -------------------------------------------------------------- ALU */

static inline uint8_t nz(uint8_t v)
{
    PSW = (PSW & ~(FN | FZ)) | (v & 0x80) | (v ? 0 : FZ);
    return v;
}

static inline void nz16(uint16_t v)
{
    PSW = (PSW & ~(FN | FZ)) | ((v >> 8) & 0x80) | (v ? 0 : FZ);
}

static uint8_t adc(uint8_t a, uint8_t b)
{
    int r = a + b + (PSW & FC);
    PSW &= ~(FV | FH | FC);
    if (~(a ^ b) & (a ^ r) & 0x80) PSW |= FV;
    if ((a ^ b ^ r) & 0x10) PSW |= FH;
    if (r > 0xFF) PSW |= FC;
    return nz((uint8_t)r);
}

static uint8_t sbc(uint8_t a, uint8_t b) { return adc(a, ~b); }

static void cmp(uint8_t a, uint8_t b)
{
    int r = a - b;
    PSW = (PSW & ~FC) | (r >= 0 ? FC : 0);
    nz((uint8_t)r);
}

static uint8_t alu(int op, uint8_t a, uint8_t b)
{
    switch (op) {
    case 0: return nz(a | b);
    case 1: return nz(a & b);
    case 2: return nz(a ^ b);
    case 3: cmp(a, b); return a;
    case 4: return adc(a, b);
    default: return sbc(a, b);
    }
}

static uint8_t shift(int op, uint8_t v)
{
    int c = PSW & FC;
    switch (op) {
    case 0: c = v >> 7; v <<= 1; break;              /* ASL */
    case 1: { int n = v >> 7; v = (v << 1) | c; c = n; break; }   /* ROL */
    case 2: c = v & 1; v >>= 1; break;               /* LSR */
    default: { int n = v & 1; v = (v >> 1) | (c << 7); c = n; break; } /* ROR */
    }
    PSW = (PSW & ~FC) | (c ? FC : 0);
    return nz(v);
}

static uint16_t addw(uint16_t a, uint16_t b, int c)
{
    int r = a + b + c;
    PSW &= ~(FV | FH | FC);
    if (~(a ^ b) & (a ^ r) & 0x8000) PSW |= FV;
    if ((a ^ b ^ r) & 0x1000) PSW |= FH;
    if (r > 0xFFFF) PSW |= FC;
    nz16((uint16_t)r);
    return (uint16_t)r;
}

static void branch(bool cond)
{
    int8_t rel = (int8_t)fetch();
    if (cond) {
        PC += rel;
        extra_cycles += 2;
    }
}

/* -------------------------------------------------------------- execute */

static void step(void)
{
    uint8_t op = fetch();
    extra_cycles = 0;
    int lo = op & 0x0F, hi = op >> 4;

    /* ALU block: OR AND EOR CMP ADC SBC, columns 4-9, rows 0-B */
    if (hi < 0x0C && lo >= 4 && lo <= 9) {
        int f = hi >> 1;
        bool odd = hi & 1;
        uint16_t addr = 0;
        uint8_t b;
        switch (lo | (odd ? 0x10 : 0)) {
        case 0x04: addr = dp(fetch()); break;
        case 0x05: addr = fetch16(); break;
        case 0x06: addr = dp(X); break;
        case 0x07: { uint8_t d = fetch() + X; addr = rd16dp(d); break; }
        case 0x08: A = alu(f, A, fetch()); return;
        case 0x09: {                                 /* dd,ds */
            uint8_t s = rd(dp(fetch()));
            uint16_t d = dp(fetch());
            uint8_t r = alu(f, rd(d), s);
            if (f != 3) wr(d, r);
            return;
        }
        case 0x14: addr = dp(fetch() + X); break;
        case 0x15: addr = fetch16() + X; break;
        case 0x16: addr = fetch16() + Y; break;
        case 0x17: addr = rd16dp(fetch()) + Y; break;
        case 0x18: {                                 /* d,#i */
            uint8_t i = fetch();
            uint16_t d = dp(fetch());
            uint8_t r = alu(f, rd(d), i);
            if (f != 3) wr(d, r);
            return;
        }
        case 0x19: {                                 /* (X),(Y) */
            uint8_t s = rd(dp(Y));
            uint8_t r = alu(f, rd(dp(X)), s);
            if (f != 3) wr(dp(X), r);
            return;
        }
        }
        b = rd(addr);
        A = alu(f, A, b);
        return;
    }

    /* SET1 / CLR1 d.n */
    if (lo == 2) {
        uint16_t a = dp(fetch());
        int bit = hi >> 1;
        uint8_t v = rd(a);
        wr(a, (hi & 1) ? (v & ~(1 << bit)) : (v | (1 << bit)));
        return;
    }
    /* BBS / BBC d.n,r */
    if (lo == 3) {
        uint8_t v = rd(dp(fetch()));
        int bit = (v >> (hi >> 1)) & 1;
        branch((hi & 1) ? !bit : bit);
        return;
    }
    /* TCALL n */
    if (lo == 1) {
        push16(PC);
        PC = spc_ram[0xFFDE - 2 * hi] | (spc_ram[0xFFDF - 2 * hi] << 8);
        return;
    }

    switch (op) {
    /* column 0 */
    case 0x00: break;
    case 0x10: branch(!(PSW & FN)); break;
    case 0x20: PSW &= ~FP; break;
    case 0x30: branch(PSW & FN); break;
    case 0x40: PSW |= FP; break;
    case 0x50: branch(!(PSW & FV)); break;
    case 0x60: PSW &= ~FC; break;
    case 0x70: branch(PSW & FV); break;
    case 0x80: PSW |= FC; break;
    case 0x90: branch(!(PSW & FC)); break;
    case 0xA0: PSW |= FI; break;
    case 0xB0: branch(PSW & FC); break;
    case 0xC0: PSW &= ~FI; break;
    case 0xD0: branch(!(PSW & FZ)); break;
    case 0xE0: PSW &= ~(FV | FH); break;
    case 0xF0: branch(PSW & FZ); break;

    /* MOV with A, rows C-F columns 4-7 */
    case 0xC4: wr(dp(fetch()), A); break;
    case 0xD4: wr(dp(fetch() + X), A); break;
    case 0xE4: A = nz(rd(dp(fetch()))); break;
    case 0xF4: A = nz(rd(dp(fetch() + X))); break;
    case 0xC5: wr(fetch16(), A); break;
    case 0xD5: wr(fetch16() + X, A); break;
    case 0xE5: A = nz(rd(fetch16())); break;
    case 0xF5: A = nz(rd(fetch16() + X)); break;
    case 0xC6: wr(dp(X), A); break;
    case 0xD6: wr(fetch16() + Y, A); break;
    case 0xE6: A = nz(rd(dp(X))); break;
    case 0xF6: A = nz(rd(fetch16() + Y)); break;
    case 0xC7: { uint8_t d = fetch() + X; wr(rd16dp(d), A); break; }
    case 0xD7: wr(rd16dp(fetch()) + Y, A); break;
    case 0xE7: { uint8_t d = fetch() + X; A = nz(rd(rd16dp(d))); break; }
    case 0xF7: A = nz(rd(rd16dp(fetch()) + Y)); break;

    /* column 8 rows C-F */
    case 0xC8: cmp(X, fetch()); break;
    case 0xD8: wr(dp(fetch()), X); break;
    case 0xE8: A = nz(fetch()); break;
    case 0xF8: X = nz(rd(dp(fetch()))); break;

    /* column 9 rows C-F */
    case 0xC9: wr(fetch16(), X); break;
    case 0xD9: wr(dp(fetch() + Y), X); break;
    case 0xE9: X = nz(rd(fetch16())); break;
    case 0xF9: X = nz(rd(dp(fetch() + Y))); break;

    /* column A */
    case 0x0A: case 0x2A: case 0x4A: case 0x6A: case 0x8A: case 0xAA: case 0xCA: case 0xEA: {
        uint16_t w = fetch16();
        uint16_t a = w & 0x1FFF;
        int bit = w >> 13;
        uint8_t v = rd(a);
        int b = (v >> bit) & 1, c = PSW & FC;
        switch (op) {
        case 0x0A: c |= b; break;
        case 0x2A: c |= !b; break;
        case 0x4A: c &= b; break;
        case 0x6A: c &= !b; break;
        case 0x8A: c ^= b; break;
        case 0xAA: c = b; break;
        case 0xCA: wr(a, c ? (v | (1 << bit)) : (v & ~(1 << bit))); break;
        case 0xEA: wr(a, v ^ (1 << bit)); break;
        }
        PSW = (PSW & ~FC) | (c ? FC : 0);
        break;
    }
    case 0x1A: case 0x3A: {                          /* DECW / INCW */
        uint8_t d = fetch();
        uint16_t w = rd16dp(d) + (op == 0x3A ? 1 : -1);
        wr(dp(d), w & 0xFF);
        wr(dp((uint8_t)(d + 1)), w >> 8);
        nz16(w);
        break;
    }
    case 0x5A: {                                     /* CMPW YA,d */
        uint16_t w = rd16dp(fetch()), ya = (Y << 8) | A;
        int r = ya - w;
        PSW = (PSW & ~FC) | (r >= 0 ? FC : 0);
        nz16((uint16_t)r);
        break;
    }
    case 0x7A: case 0x9A: {                          /* ADDW / SUBW YA,d */
        uint16_t w = rd16dp(fetch()), ya = (Y << 8) | A;
        uint16_t r = op == 0x7A ? addw(ya, w, 0) : addw(ya, ~w, 1);
        A = r & 0xFF;
        Y = r >> 8;
        break;
    }
    case 0xBA: {                                     /* MOVW YA,d */
        uint8_t d = fetch();
        A = rd(dp(d));
        Y = rd(dp((uint8_t)(d + 1)));
        nz16((Y << 8) | A);
        break;
    }
    case 0xDA: {                                     /* MOVW d,YA */
        uint8_t d = fetch();
        wr(dp(d), A);
        wr(dp((uint8_t)(d + 1)), Y);
        break;
    }
    case 0xFA: {                                     /* MOV dd,ds */
        uint8_t s = rd(dp(fetch()));
        wr(dp(fetch()), s);
        break;
    }

    /* column B: shifts / inc / dec on d, d+X; MOV with Y */
    case 0x0B: case 0x2B: case 0x4B: case 0x6B: { uint16_t a = dp(fetch()); wr(a, shift(hi >> 1, rd(a))); break; }
    case 0x1B: case 0x3B: case 0x5B: case 0x7B: { uint16_t a = dp(fetch() + X); wr(a, shift(hi >> 1, rd(a))); break; }
    case 0x8B: { uint16_t a = dp(fetch()); wr(a, nz(rd(a) - 1)); break; }
    case 0x9B: { uint16_t a = dp(fetch() + X); wr(a, nz(rd(a) - 1)); break; }
    case 0xAB: { uint16_t a = dp(fetch()); wr(a, nz(rd(a) + 1)); break; }
    case 0xBB: { uint16_t a = dp(fetch() + X); wr(a, nz(rd(a) + 1)); break; }
    case 0xCB: wr(dp(fetch()), Y); break;
    case 0xDB: wr(dp(fetch() + X), Y); break;
    case 0xEB: Y = nz(rd(dp(fetch()))); break;
    case 0xFB: Y = nz(rd(dp(fetch() + X))); break;

    /* column C */
    case 0x0C: case 0x2C: case 0x4C: case 0x6C: { uint16_t a = fetch16(); wr(a, shift(hi >> 1, rd(a))); break; }
    case 0x1C: case 0x3C: case 0x5C: case 0x7C: A = shift(hi >> 1, A); break;
    case 0x8C: { uint16_t a = fetch16(); wr(a, nz(rd(a) - 1)); break; }
    case 0x9C: A = nz(A - 1); break;
    case 0xAC: { uint16_t a = fetch16(); wr(a, nz(rd(a) + 1)); break; }
    case 0xBC: A = nz(A + 1); break;
    case 0xCC: wr(fetch16(), Y); break;
    case 0xDC: Y = nz(Y - 1); break;
    case 0xEC: Y = nz(rd(fetch16())); break;
    case 0xFC: Y = nz(Y + 1); break;

    /* column D */
    case 0x0D: push(PSW); break;
    case 0x1D: X = nz(X - 1); break;
    case 0x2D: push(A); break;
    case 0x3D: X = nz(X + 1); break;
    case 0x4D: push(X); break;
    case 0x5D: X = nz(A); break;
    case 0x6D: push(Y); break;
    case 0x7D: A = nz(X); break;
    case 0x8D: Y = nz(fetch()); break;
    case 0x9D: X = nz(SP); break;
    case 0xAD: cmp(Y, fetch()); break;
    case 0xBD: SP = X; break;
    case 0xCD: X = nz(fetch()); break;
    case 0xDD: A = nz(Y); break;
    case 0xED: PSW ^= FC; break;
    case 0xFD: Y = nz(A); break;

    /* column E */
    case 0x0E: case 0x4E: {                          /* TSET1 / TCLR1 !a */
        uint16_t a = fetch16();
        uint8_t v = rd(a);
        nz((uint8_t)(A - v));
        wr(a, op == 0x0E ? (v | A) : (v & ~A));
        break;
    }
    case 0x1E: cmp(X, rd(fetch16())); break;
    case 0x2E: { uint8_t v = rd(dp(fetch())); branch(A != v); break; }
    case 0x3E: cmp(X, rd(dp(fetch()))); break;
    case 0x5E: cmp(Y, rd(fetch16())); break;
    case 0x6E: {                                     /* DBNZ d,r */
        uint16_t a = dp(fetch());
        uint8_t v = rd(a) - 1;
        wr(a, v);
        branch(v != 0);
        break;
    }
    case 0x7E: cmp(Y, rd(dp(fetch()))); break;
    case 0x8E: PSW = pop(); break;
    case 0x9E: {                                     /* DIV YA,X */
        /* Documented hardware behaviour (fullsnes): V = quotient overflows
         * 8 bits (Y >= X), H = (Y & 15) >= (X & 15).  While the quotient
         * fits in 9 bits (Y < 2X) A/Y are the ordinary quotient/remainder;
         * beyond that the divider's internal steps give
         * A = 255 - (YA - 512X) / (256 - X), Y = X + (YA - 512X) % (256 - X). */
        unsigned dividend = (Y << 8) | A, divisor = X;
        unsigned q, r;
        PSW &= ~(FV | FH);
        PSW |= (Y >= X ? FV : 0) | ((Y & 15) >= (X & 15) ? FH : 0);
        if (Y < 2 * divisor) {
            q = dividend / divisor;
            r = dividend % divisor;
        } else {
            unsigned excess = dividend - divisor * 512, span = 256 - divisor;
            q = 255 - excess / span;
            r = divisor + excess % span;
        }
        A = (uint8_t)q;
        Y = (uint8_t)r;
        nz(A);
        break;
    }
    case 0xAE: A = pop(); break;
    case 0xBE: {                                     /* DAS */
        if (!(PSW & FC) || A > 0x99) { A -= 0x60; PSW &= ~FC; }
        if (!(PSW & FH) || (A & 15) > 9) A -= 6;
        nz(A);
        break;
    }
    case 0xCE: X = pop(); break;
    case 0xDE: { uint8_t v = rd(dp(fetch() + X)); branch(A != v); break; }
    case 0xEE: Y = pop(); break;
    case 0xFE: Y--; branch(Y != 0); break;

    /* column F */
    case 0x0F:                                       /* BRK */
        push16(PC);
        push(PSW);
        PSW = (PSW | FB) & ~FI;
        PC = spc_ram[0xFFDE] | (spc_ram[0xFFDF] << 8);
        break;
    case 0x1F: { uint16_t a = fetch16() + X; PC = rd(a) | (rd(a + 1) << 8); break; }
    case 0x2F: branch(true); break;
    case 0x3F: { uint16_t a = fetch16(); push16(PC); PC = a; break; }
    case 0x4F: { uint8_t u = fetch(); push16(PC); PC = 0xFF00 | u; break; }
    case 0x5F: PC = fetch16(); break;
    case 0x6F: PC = pop16(); break;
    case 0x7F: PSW = pop(); PC = pop16(); break;
    case 0x8F: { uint8_t i = fetch(); wr(dp(fetch()), i); break; }
    case 0x9F: A = nz((A >> 4) | (A << 4)); break;
    case 0xAF: wr(dp(X), A); X++; break;
    case 0xBF: A = nz(rd(dp(X))); X++; break;
    case 0xCF: { unsigned r = Y * A; A = r & 0xFF; Y = r >> 8; nz(Y); break; }
    case 0xDF:                                       /* DAA */
        if ((PSW & FC) || A > 0x99) { A += 0x60; PSW |= FC; }
        if ((PSW & FH) || (A & 15) > 9) A += 6;
        nz(A);
        break;
    case 0xEF: case 0xFF:                            /* SLEEP / STOP */
        halted = true;
        PC--;
        break;
    }
}

void spc_run(int cycles)
{
    while (cycles > 0) {
        int c;
        if (halted) {
            c = 2;
        } else {
            uint8_t op = spc_ram[PC];
            step();
            c = cycle_table[op] + extra_cycles;
        }
        cycles -= c;
        t01_acc += c;
        while (t01_acc >= 128) {
            t01_acc -= 128;
            timer_tick(0);
            timer_tick(1);
        }
        t2_acc += c;
        while (t2_acc >= 16) {
            t2_acc -= 16;
            timer_tick(2);
        }
        dsp_acc += c;
        while (dsp_acc >= 32) {
            dsp_acc -= 32;
            dsp_run_sample();
        }
    }
}

void spc_reset(uint16_t pc)
{
    /* the IPL ROM clears $01-$EF and jumps with A = X = 0, SP = $EF */
    memset(spc_ram + 1, 0, 0xEF);
    A = X = Y = 0;
    SP = 0xEF;
    PSW = FZ;
    PC = pc;
    halted = false;
    memset(port_in, 0, sizeof port_in);
    memset(port_out, 0, sizeof port_out);
    for (int i = 0; i < 3; i++) {
        timer[i].enabled = false;
        timer[i].stage = timer[i].out = 0;
    }
    control = 0;
}

void spc_set_pc(uint16_t pc)
{
    PC = pc;
    halted = false;
}

void spc_write_port(int port, uint8_t v) { port_in[port & 3] = v; }
uint8_t spc_read_port(int port) { return port_out[port & 3]; }

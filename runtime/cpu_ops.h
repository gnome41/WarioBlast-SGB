/* Instruction semantics helpers.  Used by the generated interpreter and by
 * the statically recompiled code, so both execute identical logic. */
#ifndef CPU_OPS_H
#define CPU_OPS_H

#include "gb.h"

#define FZ 0x80
#define FN 0x40
#define FH 0x20
#define FC 0x10

#define RD(a) mem_read((uint16_t)(a))
#define WR(a, v) mem_write((uint16_t)(a), (uint8_t)(v))
#define TICK(n) gb_tick(n)

#define AF ((uint16_t)((R.a << 8) | R.f))
#define BC ((uint16_t)((R.b << 8) | R.c))
#define DE ((uint16_t)((R.d << 8) | R.e))
#define HL ((uint16_t)((R.h << 8) | R.l))
#define SET_BC(v) do { uint16_t v_ = (uint16_t)(v); R.b = v_ >> 8; R.c = v_ & 0xFF; } while (0)
#define SET_DE(v) do { uint16_t v_ = (uint16_t)(v); R.d = v_ >> 8; R.e = v_ & 0xFF; } while (0)
#define SET_HL(v) do { uint16_t v_ = (uint16_t)(v); R.h = v_ >> 8; R.l = v_ & 0xFF; } while (0)

static inline void push16(uint16_t v)
{
    R.sp -= 2;
    WR(R.sp + 1, v >> 8);
    WR(R.sp, v & 0xFF);
}

static inline uint16_t pop16(void)
{
    uint16_t v = RD(R.sp) | (RD(R.sp + 1) << 8);
    R.sp += 2;
    return v;
}

/* Called before every instruction at address pc.  Returns true when an
 * interrupt was dispatched (pc pushed, R.pc = vector). */
static inline bool irq_check(uint16_t pc)
{
    if (trace_enabled)
        trace_insn(pc);
    if (R.ei_delay) {
        R.ei_delay = 0;
        R.ime = 1;
        return false;
    }
    uint8_t pend = gb.ie & gb.io[IO_IF] & 0x1F;
    if (!R.ime || !pend)
        return false;
    int bit = 0;
    while (!(pend & (1 << bit)))
        bit++;
    gb.io[IO_IF] &= ~(1 << bit);
    R.ime = 0;
    push16(pc);
    R.pc = 0x40 + 8 * bit;
    TICK(20);
    return true;
}

static inline uint8_t inc8(uint8_t v)
{
    uint8_t r = v + 1;
    R.f = (R.f & FC) | (r ? 0 : FZ) | ((v & 0x0F) == 0x0F ? FH : 0);
    return r;
}

static inline uint8_t dec8(uint8_t v)
{
    uint8_t r = v - 1;
    R.f = (R.f & FC) | FN | (r ? 0 : FZ) | ((v & 0x0F) == 0 ? FH : 0);
    return r;
}

static inline void add_hl(uint16_t v)
{
    unsigned hl = HL, r = hl + v;
    R.f = (R.f & FZ) | (((hl & 0xFFF) + (v & 0xFFF)) > 0xFFF ? FH : 0) | (r > 0xFFFF ? FC : 0);
    SET_HL(r);
}

static inline uint16_t sp_plus_e(int e)
{
    uint16_t sp = R.sp;
    uint8_t u = (uint8_t)e;
    R.f = (((sp & 0x0F) + (u & 0x0F)) > 0x0F ? FH : 0) | (((sp & 0xFF) + u) > 0xFF ? FC : 0);
    return (uint16_t)(sp + e);
}

static inline void alu_add(uint8_t v)
{
    unsigned r = R.a + v;
    R.f = ((r & 0xFF) ? 0 : FZ) | (((R.a & 0xF) + (v & 0xF)) > 0xF ? FH : 0) | (r > 0xFF ? FC : 0);
    R.a = r;
}

static inline void alu_adc(uint8_t v)
{
    unsigned c = (R.f & FC) ? 1 : 0;
    unsigned r = R.a + v + c;
    R.f = ((r & 0xFF) ? 0 : FZ) | (((R.a & 0xF) + (v & 0xF) + c) > 0xF ? FH : 0) | (r > 0xFF ? FC : 0);
    R.a = r;
}

static inline uint8_t sub_flags(uint8_t v, unsigned c)
{
    int r = R.a - v - c;
    R.f = FN | ((r & 0xFF) ? 0 : FZ) | (((R.a & 0xF) - (v & 0xF) - (int)c) < 0 ? FH : 0) | (r < 0 ? FC : 0);
    return (uint8_t)r;
}

static inline void alu_sub(uint8_t v) { R.a = sub_flags(v, 0); }
static inline void alu_sbc(uint8_t v) { R.a = sub_flags(v, (R.f & FC) ? 1 : 0); }
static inline void alu_cp(uint8_t v) { sub_flags(v, 0); }
static inline void alu_and(uint8_t v) { R.a &= v; R.f = (R.a ? 0 : FZ) | FH; }
static inline void alu_xor(uint8_t v) { R.a ^= v; R.f = R.a ? 0 : FZ; }
static inline void alu_or(uint8_t v) { R.a |= v; R.f = R.a ? 0 : FZ; }

static inline uint8_t zflag(uint8_t r, int carry)
{
    R.f = (r ? 0 : FZ) | (carry ? FC : 0);
    return r;
}

static inline uint8_t rlc(uint8_t v) { return zflag((uint8_t)((v << 1) | (v >> 7)), v & 0x80); }
static inline uint8_t rrc(uint8_t v) { return zflag((uint8_t)((v >> 1) | (v << 7)), v & 1); }
static inline uint8_t rl(uint8_t v) { return zflag((uint8_t)((v << 1) | ((R.f & FC) ? 1 : 0)), v & 0x80); }
static inline uint8_t rr(uint8_t v) { return zflag((uint8_t)((v >> 1) | ((R.f & FC) ? 0x80 : 0)), v & 1); }
static inline uint8_t sla(uint8_t v) { return zflag((uint8_t)(v << 1), v & 0x80); }
static inline uint8_t sra(uint8_t v) { return zflag((uint8_t)((v >> 1) | (v & 0x80)), v & 1); }
static inline uint8_t srl(uint8_t v) { return zflag((uint8_t)(v >> 1), v & 1); }
static inline uint8_t swap(uint8_t v) { return zflag((uint8_t)((v << 4) | (v >> 4)), 0); }

static inline void bit_n(int n, uint8_t v)
{
    R.f = (R.f & FC) | FH | ((v >> n) & 1 ? 0 : FZ);
}

static inline void daa(void)
{
    int a = R.a;
    uint8_t f = R.f;
    if (!(f & FN)) {
        if ((f & FC) || a > 0x99) { a += 0x60; f |= FC; }
        if ((f & FH) || (a & 0x0F) > 0x09) a += 0x06;
    } else {
        if (f & FC) a -= 0x60;
        if (f & FH) a -= 0x06;
    }
    a &= 0xFF;
    f &= ~(FZ | FH);
    if (!a) f |= FZ;
    R.a = (uint8_t)a;
    R.f = f;
}

/* Recompiled-code helpers.  Every instruction starts with IRQ(pc) (bank 0)
 * or IRQB(pc, bank) (switchable bank: bail out to the dispatcher if the
 * mapped bank no longer matches the bank this code was translated from). */
#define IRQ(at) do { if (irq_check(at)) return; } while (0)
#define IRQB(at, bank) do { if (gb.rom_bank != (bank)) { R.pc = (at); return; } \
                            if (irq_check(at)) return; } while (0)
#define EXIT(target) do { R.pc = (target); return; } while (0)

#endif

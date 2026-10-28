/* Static recompiler: bank-aware control-flow discovery and C generation.
 *
 * Phase 1 finds code by recursive descent from known entry points (reset,
 * RST/interrupt vectors, configured seeds and coverage addresses),
 * following jumps, calls and the game's jump-table idioms, and tracking
 * the ROM bank selected by "ld a,N / rst 10h" style bank switches.
 * Phase 2 groups the code into functions: each entry point owns
 * everything reachable from it by fallthrough and jumps, stopping at other
 * entry points.  Phase 3 emits one C function per entry point.
 *
 * Mistakes in the analysis never affect correctness, only speed: each
 * function is a faithful translation of the bytes at its (bank, address),
 * the runtime selects code by the bank actually mapped, and anything not
 * translated runs on the interpreter.
 *
 * The traversal order (a LIFO work list, first visit wins) matters: it
 * decides which bank state reaches shared code first. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "recomp.h"

typedef struct {
    uint16_t addr, op;     /* op: 0-255 main table, 0x100 + n for CB n */
    uint8_t n8, raw0, len;
    uint16_t n16;
} Insn;

typedef struct { uint8_t bank; uint16_t addr; int16_t a_val, mapped; } Work;

typedef struct {
    const uint8_t *rom;
    unsigned banks;
    const RecompConfig *cfg;
    Insn *insns;            /* indexed by key() */
    uint8_t *have;          /* decoded at key() */
    uint8_t *entry;         /* is an entry point */
    Work *work;
    size_t nwork, capwork;
    size_t ninsns, ntables;
} An;

/* ---- helpers --------------------------------------------------------- */

static const Sm83Op *info(const Insn *i) { return i->op >= 0x100 ? &sm83_cb[i->op - 0x100] : &sm83_main[i->op]; }
static int region(int addr, int bank) { return addr < 0x4000 ? 0 : bank; }

static int fetch(const An *a, int bank, long addr)
{
    if (addr < 0x4000)
        return a->rom[addr];
    if (addr < 0x8000)
        return a->rom[(size_t)bank * 0x4000 + (addr - 0x4000)];
    return 0xFF;
}

static int word(const An *a, int bank, long addr) { return fetch(a, bank, addr) | (fetch(a, bank, addr + 1) << 8); }

static bool valid(const An *a, int bank, int addr)
{
    if (addr >= 0x8000)
        return false;
    if (addr >= 0x4000 && !(bank >= 1 && (unsigned)bank < a->banks))
        return false;
    return true;
}

/* Index for (region bank, address); -1 if no such key can exist. */
static long key(const An *a, int bank, int addr)
{
    if (addr < 0 || addr >= 0x8000 || (addr < 0x4000) != (bank == 0) || bank < 0 || (unsigned)bank >= a->banks)
        return -1;
    return addr < 0x4000 ? addr : (long)bank * 0x4000 + (addr - 0x4000);
}

static bool has_insn(const An *a, int bank, int addr) { long k = key(a, bank, addr); return k >= 0 && a->have[k]; }
static bool is_entry(const An *a, int bank, int addr) { long k = key(a, bank, addr); return k >= 0 && a->entry[k]; }

static Insn decode(const An *a, int bank, int addr)
{
    Insn i = { 0 };
    i.addr = (uint16_t)addr;
    int b0 = fetch(a, bank, addr);
    i.raw0 = (uint8_t)b0;
    if (b0 == 0xCB) {
        i.op = 0x100 + fetch(a, bank, (addr + 1) & 0xFFFF);
        i.len = 2;
        return i;
    }
    i.op = (uint16_t)b0;
    i.len = sm83_main[b0].length;
    if (i.len >= 2)
        i.n8 = (uint8_t)fetch(a, bank, (addr + 1) & 0xFFFF);
    if (i.len == 3)
        i.n16 = (uint16_t)(i.n8 | (fetch(a, bank, (addr + 2) & 0xFFFF) << 8));
    return i;
}

static int e8(const Insn *i) { return i->n8 >= 128 ? i->n8 - 256 : i->n8; }
static int next_addr(const Insn *i) { return (i->addr + i->len) & 0xFFFF; }

static int jump_target(const Insn *i)
{
    switch (info(i)->kind) {
    case K_JR: return (next_addr(i) + e8(i)) & 0xFFFF;
    case K_JP: case K_CALL: return i->n16;
    case K_RST: return info(i)->target;
    }
    return -1;
}

static bool in_u16(const uint16_t *v, size_t n, int x)
{
    for (size_t i = 0; i < n; i++)
        if (v[i] == x)
            return true;
    return false;
}

static bool in_refs(const RecompRef *v, size_t n, int bank, int addr)
{
    for (size_t i = 0; i < n; i++)
        if (v[i].bank == bank && v[i].addr == addr)
            return true;
    return false;
}

static bool flow_end(int kind) { return kind == K_RET || kind == K_RETI || kind == K_JPHL || kind == K_ILLEGAL; }

/* ---- phase 1 ----------------------------------------------------------- */

static void push_work(An *a, int bank, int addr, int a_val, int mapped)
{
    if (a->nwork == a->capwork) {
        a->capwork = a->capwork ? a->capwork * 2 : 1024;
        a->work = realloc(a->work, a->capwork * sizeof *a->work);
    }
    a->work[a->nwork++] = (Work){ (uint8_t)bank, (uint16_t)addr, (int16_t)a_val, (int16_t)mapped };
}

static void add_entry(An *a, int bank, int addr, int a_val, int mapped)
{
    bank = region(addr, bank);
    if (!valid(a, bank, addr))
        return;
    a->entry[key(a, bank, addr)] = 1;
    push_work(a, bank, addr, a_val, mapped);
}

/* Bank to assume for a call/jump target, or -1 if unknown. */
static int callee_bank(int cur_region_bank, int known_bank, int target)
{
    if (target < 0x4000)
        return 0;
    if (cur_region_bank)
        return known_bank >= 0 ? known_bank : cur_region_bank;
    return known_bank;
}

/* Inline word table following a jump-table call. */
static void jump_table(An *a, int bank, int table, int mapped)
{
    RecompRef targets[128];
    int n = 0;
    long lowest = 0x10000, p = table;
    for (int k = 0; k < 128; k++) {
        if (p >= lowest || region((int)p + 1, bank) != bank)
            break;
        int w = word(a, bank, p);
        int tb = callee_bank(bank, mapped, w);
        if (w >= 0x8000 || w < 0x0100 || tb < 0 || !valid(a, region(w, tb), w))
            break;
        Insn first = decode(a, region(w, tb), w);
        if (info(&first)->kind == K_ILLEGAL || first.raw0 == 0xFF)
            break;
        targets[n++] = (RecompRef){ (uint8_t)region(w, tb), (uint16_t)w };
        if (region(w, tb) == bank && w > table && w < lowest)
            lowest = w;
        p += 2;
    }
    for (int k = 0; k < n; k++)
        add_entry(a, targets[k].bank, targets[k].addr, -1, -1);
    a->ntables++;
}

/* ld hl,TABLE / add hl,rr / ld a,(hl+) / ld h,(hl) / ld l,a / jp hl */
static void inline_table(An *a, int bank, const Insn *recent, int nrecent, int mapped)
{
    if (nrecent < 4 || recent[nrecent - 4].op != 0x2A || recent[nrecent - 3].op != 0x66 ||
        recent[nrecent - 2].op != 0x6F || recent[nrecent - 1].op != 0xE9)
        return;
    for (int k = nrecent - 5; k >= 0; k--) {
        if (recent[k].op == 0x09 || recent[k].op == 0x19)
            continue;
        if (recent[k].op == 0x21)
            jump_table(a, bank, recent[k].n16, mapped);
        return;
    }
}

/* Configured tables of 3-byte (bank, little-endian address) entries. */
static void far_tables(An *a)
{
    for (size_t t = 0; t < a->cfg->n_far_tables; t++) {
        long base = a->cfg->far_tables[t].addr;
        for (int n = 0; n < a->cfg->far_tables[t].max; n++) {
            long e = base + 3L * n;
            int b = fetch(a, 0, e), w = word(a, 0, e + 1);
            if (!(b >= 1 && (unsigned)b < a->banks && w >= 0x4000 && w < 0x8000))
                break;
            add_entry(a, b, w, -1, -1);
        }
    }
}

/* Constant loaded into an 8-bit register (ld r,n8 numbering) or register
 * pair (0 bc, 1 de, 2 hl; reg = 8 + pair) by the latest instructions before
 * the last one, or -1.  Only a hint: stops at the first other instruction
 * that names the register. */
static int recent_const(const Insn *recent, int nrecent, int reg)
{
    for (int k = nrecent - 2; k >= 0; k--) {
        const Insn *i = &recent[k];
        int kind = info(i)->kind;
        if (kind == K_CALL || kind == K_RST || kind == K_JP || kind == K_JR || kind == K_JPHL)
            return -1;
        if (reg < 8) {
            if (i->op == 0x06 + 8 * reg)
                return i->n8;
            int pair = reg / 2;
            if (pair < 3 && i->op == 0x01 + 16 * pair)
                return reg & 1 ? i->n16 & 0xFF : i->n16 >> 8;
            if (i->op < 0x100 && i->op >= 0x40 && i->op < 0x80 && ((i->op >> 3) & 7) == reg)
                return -1;
        } else {
            int pair = reg - 8;
            if (i->op == 0x01 + 16 * pair)
                return i->n16;
            if (i->op == 0x06 + 16 * pair || i->op == 0x0E + 16 * pair)
                return -1;
        }
    }
    return -1;
}

static void far_call(An *a, int bank, int mapped, const Insn *recent, int nrecent, int t)
{
    for (size_t i = 0; i < a->cfg->n_far_calls; i++) {
        const RecompFarCall *fc = &a->cfg->far_calls[i];
        if (fc->routine != t)
            continue;
        int addr = recent_const(recent, nrecent, 8 + fc->addr_reg);
        if (addr < 0x0100 || addr >= 0x8000)
            return;
        int tb = fc->bank_reg >= 0 ? recent_const(recent, nrecent, fc->bank_reg) : -1;
        if (tb >= 0)
            tb = (tb & 0x1F) ? (tb & 0x1F) : 1;
        else
            tb = callee_bank(bank, mapped, addr);
        if (tb >= 0)
            add_entry(a, tb, addr, -1, -1);
        return;
    }
}

static void trace(An *a, int bank, int addr, int a_val, int mapped)
{
    if (bank)
        mapped = bank;
    Insn recent[10];
    int nrecent = 0;
    for (;;) {
        if (!valid(a, bank, addr) || has_insn(a, bank, addr))
            return;
        Insn insn = decode(a, bank, addr);
        long k = key(a, bank, addr);
        a->insns[k] = insn;
        a->have[k] = 1;
        a->ninsns++;
        const Sm83Op *op = info(&insn);
        if (nrecent == 10) {
            memmove(recent, recent + 1, 9 * sizeof *recent);
            nrecent = 9;
        }
        recent[nrecent++] = insn;

        /* constant tracking for bank switches */
        if (insn.op == 0x3E)
            a_val = insn.n8;
        else if (insn.op == 0xAF)
            a_val = 0;
        else if (insn.op == 0xEA && insn.n16 >= 0x2000 && insn.n16 < 0x4000 && a_val >= 0)
            mapped = (a_val & 0x1F) ? (a_val & 0x1F) : 1;
        else if (op->sets_a)
            a_val = -1;

        int nxt = next_addr(&insn);
        /* ld rr,ADDR / push rr: a pushed return address */
        if ((insn.op == 0xC5 || insn.op == 0xD5 || insn.op == 0xE5) && nrecent >= 2 &&
            recent[nrecent - 2].op == insn.op - 0xC4) {
            int t = recent[nrecent - 2].n16;
            int tb = callee_bank(bank, mapped, t);
            if (tb >= 0 && t >= 0x0100 && t < 0x8000)
                add_entry(a, tb, t, -1, -1);
        }
        if (op->kind == K_JPHL)
            inline_table(a, bank, recent, nrecent, mapped);
        if (flow_end(op->kind) && !op->cond)
            return;
        if (op->kind == K_JP || op->kind == K_JR) {
            int t = jump_target(&insn);
            int tb = callee_bank(bank, mapped, t);
            if (tb >= 0 && region(t, tb) == bank)
                push_work(a, bank, t, a_val, mapped);
            else if (tb >= 0)
                add_entry(a, tb, t, a_val, mapped);
            if (!op->cond)
                return;
        } else if (op->kind == K_CALL || op->kind == K_RST) {
            int t = jump_target(&insn);
            far_call(a, bank, mapped, recent, nrecent, t);
            if (in_u16(a->cfg->bank_switch_routines, a->cfg->n_bank_switch_routines, t) && a_val >= 0)
                mapped = (a_val & 0x1F) ? (a_val & 0x1F) : 1;
            int tb = callee_bank(bank, mapped, t);
            if (tb >= 0)
                add_entry(a, tb, t, a_val, mapped);
            if (in_u16(a->cfg->jump_table_routines, a->cfg->n_jump_table_routines, t) && !op->cond) {
                jump_table(a, bank, nxt, mapped);
                return;
            }
            if (tb >= 0 && in_refs(a->cfg->noreturn, a->cfg->n_noreturn, region(t, tb), t))
                return;
            a_val = -1;   /* callee may clobber A */
        }
        if (region(nxt, bank) != bank)
            return;       /* fell off the fixed bank into the switchable one */
        addr = nxt;
    }
}

static void run(An *a, const RecompRef *seeds, size_t nseeds)
{
    for (size_t i = 0; i < nseeds; i++)
        add_entry(a, seeds[i].bank, seeds[i].addr, -1, -1);
    if (nseeds)
        far_tables(a);
    while (a->nwork) {
        Work w = a->work[--a->nwork];
        trace(a, w.bank, w.addr, w.a_val, w.mapped);
    }
}

/* ---- text output ------------------------------------------------------- */

typedef struct { char *p; size_t len, cap; } Buf;

static void put(Buf *b, const char *fmt, ...)
{
    va_list ap;
    for (;;) {
        va_start(ap, fmt);
        int n = vsnprintf(b->p ? b->p + b->len : NULL, b->p ? b->cap - b->len : 0, fmt, ap);
        va_end(ap);
        if (n < 0)
            return;
        if (b->p && b->len + (size_t)n < b->cap) {
            b->len += (size_t)n;
            return;
        }
        b->cap = (b->cap + (size_t)n + 1) * 2;
        b->p = realloc(b->p, b->cap);
    }
}

/* Appends template with each "{name}" replaced. */
static void put_template(Buf *b, const char *t, const char *n8, const char *n16, const char *e8s, const char *e8p)
{
    while (*t) {
        const char *r = NULL;
        size_t skip = 0;
        if (!strncmp(t, "{n8}", 4)) r = n8, skip = 4;
        else if (!strncmp(t, "{n16}", 5)) r = n16, skip = 5;
        else if (!strncmp(t, "{e8:+}", 6)) r = e8p, skip = 6;
        else if (!strncmp(t, "{e8}", 4)) r = e8s, skip = 4;
        if (r) {
            put(b, "%s", r);
            t += skip;
        } else {
            put(b, "%c", *t++);
        }
    }
}

static void insn_text(Buf *b, const Insn *i)
{
    char n8[8], n16[8], e8s[8], e8p[8];
    if (info(i)->kind == K_JR) {
        snprintf(n16, sizeof n16, "$%04X", jump_target(i));
        put_template(b, info(i)->mnem, "", "", n16, n16);
        return;
    }
    snprintf(n8, sizeof n8, "$%02X", i->n8);
    snprintf(n16, sizeof n16, "$%04X", i->n16);
    snprintf(e8s, sizeof e8s, "%d", e8(i));
    snprintf(e8p, sizeof e8p, "%+d", e8(i));
    put_template(b, info(i)->mnem, n8, n16, e8s, e8p);
}

void recomp_fn_name(uint8_t bank, uint16_t entry, char out[16])
{
    snprintf(out, 16, "f_%02X_%04X", bank, entry);
}

typedef struct { const uint16_t *addrs; size_t n; } Body;

static bool in_body(const Body *body, int addr)
{
    size_t lo = 0, hi = body->n;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (body->addrs[mid] < addr) lo = mid + 1;
        else hi = mid;
    }
    return lo < body->n && body->addrs[lo] == addr;
}

static void go(Buf *b, const Body *body, int t)
{
    if (in_body(body, t))
        put(b, "goto L_%04X;", t);
    else
        put(b, "EXIT(0x%04x);", t);
}

/* One instruction; returns true if control falls through to the next. */
static bool emit_insn(Buf *b, const Insn *i, int bank, const Body *body)
{
    const Sm83Op *op = info(i);
    int a = i->addr, nxt = next_addr(i);
    put(b, "L_%04X: /* ", a);
    insn_text(b, i);
    put(b, " */\n");
    if (a >= 0x4000)
        put(b, "    IRQB(0x%04x, %d);\n", a, bank);
    else
        put(b, "    IRQ(0x%04x);\n", a);
    switch (op->kind) {
    case K_OP: {
        char n8[8], n16[8], e8s[8];
        snprintf(n8, sizeof n8, "0x%02x", i->n8);
        snprintf(n16, sizeof n16, "0x%04x", i->n16);
        snprintf(e8s, sizeof e8s, "%d", e8(i));
        if (op->code[0]) {
            put(b, "    ");
            put_template(b, op->code, n8, n16, e8s, e8s);
            put(b, " TICK(%d);\n", op->cycles);
        } else {
            put(b, "    TICK(%d);\n", op->cycles);
        }
        return true;
    }
    case K_JP: case K_JR: {
        int t = jump_target(i);
        if (op->cond) {
            put(b, "    if (%s) { TICK(%d); ", op->cond, op->cycles_taken);
            go(b, body, t);
            put(b, " }\n    TICK(%d);\n", op->cycles);
            return true;
        }
        put(b, "    TICK(%d); ", op->cycles);
        go(b, body, t);
        put(b, "\n");
        return false;
    }
    case K_CALL: {
        int t = jump_target(i);
        if (op->cond) {
            put(b, "    if (%s) { push16(0x%04x); TICK(%d); EXIT(0x%04x); }\n    TICK(%d);\n",
                op->cond, nxt, op->cycles_taken, t, op->cycles);
            return true;
        }
        put(b, "    push16(0x%04x); TICK(%d); EXIT(0x%04x);\n", nxt, op->cycles, t);
        return false;
    }
    case K_RST:
        put(b, "    push16(0x%04x); TICK(%d); EXIT(0x%04x);\n", nxt, op->cycles, op->target);
        return false;
    case K_RET:
        if (op->cond) {
            put(b, "    if (%s) { uint16_t t = pop16(); TICK(%d); EXIT(t); }\n    TICK(%d);\n",
                op->cond, op->cycles_taken, op->cycles);
            return true;
        }
        put(b, "    { uint16_t t = pop16(); TICK(%d); EXIT(t); }\n", op->cycles);
        return false;
    case K_RETI:
        put(b, "    { uint16_t t = pop16(); R.ime = 1; TICK(%d); EXIT(t); }\n", op->cycles);
        return false;
    case K_JPHL:
        put(b, "    TICK(%d); EXIT(HL);\n", op->cycles);
        return false;
    case K_HALT:
        put(b, "    TICK(%d); gb_halt();\n", op->cycles);
        return true;
    case K_STOP:
        put(b, "    TICK(%d); gb_stop();\n", op->cycles);
        return true;
    case K_DI:
        put(b, "    R.ime = 0; R.ei_delay = 0; TICK(%d);\n", op->cycles);
        return true;
    case K_EI:
        put(b, "    if (!R.ime) { R.ei_delay = 1; } TICK(%d);\n", op->cycles);
        return true;
    default:   /* K_ILLEGAL */
        put(b, "    R.pc = 0x%04x; gb_illegal(0x%04x); return;\n", a, a);
        return false;
    }
}

static void emit_function(Buf *b, const An *a, int bank, int entry, const uint16_t *addrs, size_t n)
{
    Body body = { addrs, n };
    char name[16];
    recomp_fn_name((uint8_t)bank, (uint16_t)entry, name);
    put(b, "void %s(void)\n{\n    switch (R.pc) {\n", name);
    for (size_t k = 0; k < n; k++)
        put(b, "    case 0x%04x: goto L_%04X;\n", addrs[k], addrs[k]);
    put(b, "    default: return;\n    }\n");
    for (size_t k = 0; k < n; k++) {
        const Insn *i = &a->insns[key(a, bank, addrs[k])];
        if (emit_insn(b, i, bank, &body)) {
            int nxt = next_addr(i);
            if (k + 1 >= n || addrs[k + 1] != nxt) {
                put(b, "    ");
                if (in_body(&body, nxt))
                    put(b, "goto L_%04X;", nxt);
                else
                    put(b, "EXIT(0x%04x);", nxt);
                put(b, "\n");
            }
        }
    }
    put(b, "}\n");
}

/* ---- phase 2 + 3 ------------------------------------------------------- */

static bool no_return_after(const An *a, int bank, const Insn *i)
{
    const Sm83Op *op = info(i);
    int t = jump_target(i);
    if (op->cond)
        return false;
    if (in_u16(a->cfg->jump_table_routines, a->cfg->n_jump_table_routines, t))
        return true;
    return in_refs(a->cfg->noreturn, a->cfg->n_noreturn, region(t, bank), t);
}

static int cmp_u16(const void *x, const void *y) { return *(const uint16_t *)x - *(const uint16_t *)y; }

typedef struct { RecompEntry e; bool is_entry; size_t order; } SortEntry;

static int cmp_entries(const void *x, const void *y)
{
    const SortEntry *p = x, *q = y;
    if (p->is_entry != q->is_entry) return p->is_entry ? -1 : 1;
    if (p->e.bank != q->e.bank) return p->e.bank - q->e.bank;
    if (p->e.addr != q->e.addr) return p->e.addr - q->e.addr;
    return p->order < q->order ? -1 : p->order > q->order;
}

bool recomp_run(const uint8_t *rom, size_t size, const RecompConfig *cfg, RecompResult *out)
{
    memset(out, 0, sizeof *out);
    if (size < 0x8000 || size % 0x4000)
        return false;
    An a = { 0 };
    a.rom = rom;
    a.banks = (unsigned)(size / 0x4000);
    a.cfg = cfg;
    size_t keys = (size_t)a.banks * 0x4000;
    a.insns = calloc(keys, sizeof *a.insns);
    a.have = calloc(keys, 1);
    a.entry = calloc(keys, 1);
    uint32_t *seen = calloc(0x10000, sizeof *seen);
    uint16_t *addrs = malloc(0x8000 * sizeof *addrs);
    uint16_t *stack = malloc(0x20000 * sizeof *stack);
    if (!a.insns || !a.have || !a.entry || !seen || !addrs || !stack)
        goto fail;

    /* phase 1: vectors and configured entry points, then coverage */
    RecompRef seeds[16 + 64];
    size_t ns = 0;
    seeds[ns++] = (RecompRef){ 0, 0x0100 };
    for (int v = 0; v < 0x68; v += 8)
        seeds[ns++] = (RecompRef){ 0, (uint16_t)v };
    for (size_t i = 0; i < cfg->n_entries && ns < sizeof seeds / sizeof *seeds; i++)
        seeds[ns++] = cfg->entries[i];
    run(&a, seeds, ns);
    for (size_t i = 0; i < cfg->n_coverage; i++) {
        const RecompRef *r = &cfg->coverage[i];
        if (!has_insn(&a, r->bank, r->addr)) {
            add_entry(&a, r->bank, r->addr, -1, -1);
            run(&a, NULL, 0);
            out->nseeds++;
        }
    }

    /* phases 2 and 3 */
    Buf src = { 0 };
    size_t capf = 0, cape = 0;
    SortEntry *ents = NULL;
    size_t nents = 0;
    uint32_t stamp = 0;
    for (size_t k = 0; k < keys; k++) {
        if (!a.entry[k] || !a.have[k])
            continue;
        int bank = k < 0x4000 ? 0 : (int)(k / 0x4000);
        int entry = k < 0x4000 ? (int)k : (int)(0x4000 + k % 0x4000);
        stamp++;
        size_t n = 0, sp = 0;
        stack[sp++] = (uint16_t)entry;
        while (sp) {
            int p = stack[--sp];
            if (seen[p] == stamp || !has_insn(&a, bank, p))
                continue;
            if (p != entry && is_entry(&a, bank, p))
                continue;   /* another function: leave via EXIT */
            seen[p] = stamp;
            addrs[n++] = (uint16_t)p;
            const Insn *i = &a.insns[key(&a, bank, p)];
            const Sm83Op *op = info(i);
            if (flow_end(op->kind) && !op->cond)
                continue;
            if ((op->kind == K_CALL || op->kind == K_RST) && no_return_after(&a, bank, i))
                continue;
            if (op->kind == K_JP || op->kind == K_JR) {
                int t = jump_target(i);
                if (region(t, bank) == bank)
                    stack[sp++] = (uint16_t)t;
                if (!op->cond)
                    continue;
            }
            stack[sp++] = (uint16_t)next_addr(i);
        }
        qsort(addrs, n, sizeof *addrs, cmp_u16);

        if (out->nfuncs == capf) {
            capf = capf ? capf * 2 : 512;
            out->funcs = realloc(out->funcs, capf * sizeof *out->funcs);
        }
        RecompFunc *f = &out->funcs[out->nfuncs];
        f->bank = (uint8_t)bank;
        f->entry = (uint16_t)entry;
        f->src_off = src.len;
        emit_function(&src, &a, bank, entry, addrs, n);
        f->src_len = src.len - f->src_off;
        for (size_t j = 0; j < n; j++) {
            if (nents == cape) {
                cape = cape ? cape * 2 : 8192;
                ents = realloc(ents, cape * sizeof *ents);
            }
            ents[nents] = (SortEntry){ { (uint8_t)bank, addrs[j], out->nfuncs }, addrs[j] == entry, nents };
            nents++;
        }
        out->nfuncs++;
    }
    qsort(ents, nents, sizeof *ents, cmp_entries);
    out->entries = malloc((nents ? nents : 1) * sizeof *out->entries);
    for (size_t j = 0; j < nents; j++)
        out->entries[j] = ents[j].e;
    out->nentries = nents;
    out->src = src.p ? src.p : calloc(1, 1);
    out->src_len = src.len;
    out->ninsns = a.ninsns;
    out->ntables = a.ntables;
    free(ents);
    free(a.insns); free(a.have); free(a.entry); free(a.work);
    free(seen); free(addrs); free(stack);
    return true;

fail:
    free(a.insns); free(a.have); free(a.entry); free(a.work);
    free(seen); free(addrs); free(stack);
    return false;
}

void recomp_free(RecompResult *r)
{
    free(r->src);
    free(r->funcs);
    free(r->entries);
    memset(r, 0, sizeof *r);
}

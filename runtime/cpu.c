/* CPU reset, fallback interpreter and the dispatcher.
 *
 * Execution model: R.pc always names the next instruction whenever control
 * is in the dispatcher.  Recompiled functions are looked up by
 * (mapped bank, pc); every instruction they contain is a valid resume point,
 * so the dispatcher can re-enter them anywhere (after a RET, an interrupt,
 * a bank change).  Anything without recompiled code - code in RAM, or ROM
 * code the static analysis did not find - runs on the interpreter, one
 * instruction at a time. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cpu_ops.h"
#include "cheats.h"

void cpu_reset(void)
{
    if (opt.sgb) {
        R.a = 0x01; R.f = 0x00; R.b = 0x00; R.c = 0x14;
        R.d = 0x00; R.e = 0x00; R.h = 0xC0; R.l = 0x60;
    } else {
        R.a = 0x01; R.f = 0xB0; R.b = 0x00; R.c = 0x13;
        R.d = 0x00; R.e = 0xD8; R.h = 0x01; R.l = 0x4D;
    }
    R.sp = 0xFFFE;
    R.pc = 0x0100;
    R.ime = 0;
    R.ei_delay = 0;
}

void gb_halt(void)
{
    if (!R.ime && (gb.ie & gb.io[IO_IF] & 0x1F))
        return;  /* HALT bug case: not emulated, just continue */
    while (!(gb.ie & gb.io[IO_IF] & 0x1F))
        TICK(4);
}

void gb_stop(void)
{
    gb.div_counter = 0;
}

void gb_illegal(uint16_t pc)
{
    fprintf(stderr, "illegal opcode %02X at %02X:%04X - CPU locked up\n",
            mem_read(pc), pc >= 0x4000 && pc < 0x8000 ? gb.rom_bank : 0, pc);
    platform_shutdown();
    exit(1);
}

/* ---- fallback coverage log ---- */

static uint8_t *hit_map;

void fallback_log_hit(unsigned bank, uint16_t addr)
{
    if (!hit_map) {
        hit_map = calloc(gb.rom_banks, 0x4000);
        if (!hit_map)
            return;
    }
    size_t i = addr < 0x4000 ? addr : bank * 0x4000 + (addr - 0x4000);
    hit_map[i] = 1;
}

void fallback_log_write(void)
{
    if (!hit_map || !opt.fallback_log)
        return;
    FILE *f = fopen(opt.fallback_log, "w");
    if (!f)
        return;
    size_t n = 0;
    for (size_t i = 0; i < (size_t)gb.rom_banks * 0x4000; i++)
        if (hit_map[i]) {
            unsigned bank = i / 0x4000;
            unsigned addr = bank ? 0x4000 + (i % 0x4000) : i;
            fprintf(f, "%02X:%04X\n", bank, addr);
            n++;
        }
    fclose(f);
    fprintf(stderr, "fallback log: %zu interpreted ROM addresses -> %s\n", n, opt.fallback_log);
}

/* ---- instruction trace (WB_TRACE=file, WB_TRACE_FROM=frame) ---- */

bool trace_enabled;
static FILE *trace_file;

void trace_frame(void)
{
    static long from = -1;
    if (from < 0) {
        const char *f = getenv("WB_TRACE");
        const char *s = getenv("WB_TRACE_FROM");
        from = s ? atol(s) : 0;
        if (f)
            trace_file = fopen(f, "w");
        if (!trace_file)
            from = 0x7FFFFFFF;
    }
    trace_enabled = trace_file && (long)gb.frame_count >= from;

    /* WB_POKE="frame:addr=val[,addr=val...];..." - debugging aid that
     * patches memory at the start of a frame */
    static const char *poke = (const char *)-1;
    if (poke == (const char *)-1)
        poke = getenv("WB_POKE");
    for (const char *p = poke; p && *p;) {
        char *end;
        unsigned long f = strtoul(p, &end, 10);
        if (*end != ':')
            break;
        p = end + 1;
        while (*p && *p != ';') {
            unsigned long a = strtoul(p, &end, 16), v = 0;
            if (*end == '=')
                v = strtoul(end + 1, &end, 16);
            if (f == gb.frame_count)
                mem_write((uint16_t)a, (uint8_t)v);
            p = *end == ',' ? end + 1 : end;
            if (end == p && *p != ';' && *p) break;
        }
        if (*p == ';')
            p++;
    }
}

void trace_insn(uint16_t pc)
{
    fprintf(trace_file, "%llu %02X:%04X a%02X f%02X b%02X c%02X d%02X e%02X h%02X l%02X sp%04X ly%02X\n",
            (unsigned long long)gb.cycles, pc >= 0x4000 && pc < 0x8000 ? gb.rom_bank : 0, pc,
            R.a, R.f, R.b, R.c, R.d, R.e, R.h, R.l, R.sp, gb.io[IO_LY]);
}

void trace_read(uint16_t addr, uint8_t v)
{
    fprintf(trace_file, "  R %04X=%02X\n", addr, v);
}

void trace_write(uint16_t addr, uint8_t v)
{
    fprintf(trace_file, "  W %04X=%02X\n", addr, v);
}

/* ---- interpreter ---- */

uint64_t interp_count;

void interp_step(void)
{
    uint16_t pc = R.pc;
    interp_count++;
    if (irq_check(pc))
        return;
    if (opt.fallback_log && pc < 0x8000)
        fallback_log_hit(pc < 0x4000 ? 0 : gb.rom_bank, pc);
    uint8_t op = RD(pc);
#include "interp_ops.inc"
}

/* ---- dispatcher ---- */

static GenFn *fn_table;
static const GenEntry *code_entries;
static size_t code_count;

/* Registers recompiled code; entries for the same address: first wins. */
void dispatch_set_code(const GenEntry *entries, size_t count)
{
    code_entries = entries;
    code_count = count;
}

/* Leaves the recompiled functions that contain a byte changed by a Game
 * Genie code to the interpreter: their instructions are compiled in.  An
 * instruction covering the byte starts at most 2 bytes before it; dropping
 * an extra function is harmless (the interpreter is exact, only slower). */
static void drop_patched_code(void)
{
    const uint8_t *bank;
    const uint16_t *addr;
    size_t n = cheats_rom_patches(&bank, &addr), dropped = 0;
    for (size_t i = 0; i < n; i++) {
        size_t base = addr[i] < 0x4000 ? 0 : (size_t)bank[i] * 0x4000;
        unsigned lo = addr[i] < 0x4000 ? 0 : 0x4000;
        for (unsigned back = 0; back <= 2 && addr[i] >= lo + back; back++) {
            GenFn fn = fn_table[base + (addr[i] - back - lo)];
            if (!fn)
                continue;
            for (size_t j = base; j < base + 0x4000; j++)
                if (fn_table[j] == fn)
                    fn_table[j] = NULL;
            dropped++;
        }
    }
    if (dropped)
        fprintf(stderr, "cheats: %zu recompiled functions run on the interpreter\n", dropped);
}

static void build_table(void)
{
    if (!fn_table)
        fn_table = malloc((size_t)gb.rom_banks * 0x4000 * sizeof *fn_table);
    if (!fn_table) {
        fprintf(stderr, "out of memory\n");
        exit(1);
    }
    memset(fn_table, 0, (size_t)gb.rom_banks * 0x4000 * sizeof *fn_table);
    for (size_t i = 0; i < code_count; i++) {
        const GenEntry *e = &code_entries[i];
        if (e->bank >= gb.rom_banks)
            continue;
        size_t idx = e->addr < 0x4000 ? e->addr : e->bank * 0x4000 + (e->addr - 0x4000);
        if (!fn_table[idx])
            fn_table[idx] = e->fn;
    }
    drop_patched_code();
    dispatch_dirty = false;
}

void run_forever(void)
{
    if (!opt.interp_only && code_count)
        build_table();
    for (;;) {
        if (dispatch_dirty && fn_table)
            build_table();
        uint16_t pc = R.pc;
        GenFn fn = NULL;
        if (fn_table && pc < 0x8000)
            fn = fn_table[pc < 0x4000 ? pc : gb.rom_bank * 0x4000 + (pc - 0x4000)];
        if (fn)
            fn();
        else
            interp_step();
    }
}

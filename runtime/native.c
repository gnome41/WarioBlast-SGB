/* Player build: recompiles the user's ROM at startup and compiles the
 * result in memory with TinyCC (libtcc, LGPL, loaded as a shared
 * library).  Nothing derived from the ROM is shipped or written to disk.
 *
 * The compiled code makes no external references: the prelude reaches the
 * runtime through pointer variables (wb_gb, wb_mem_read, ...) that are
 * filled in here after relocation. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL.h>
#include "libtcc.h"
#include "gb.h"
#include "recomp/recomp.h"

extern const char tcc_prelude[];

static TCCState *tcc;          /* owns the compiled code: kept for the process lifetime */
static GenEntry *entries;

static void tcc_error(void *opaque, const char *msg)
{
    (void)opaque;
    fprintf(stderr, "recompiler (TinyCC): %s\n", msg);
}

static bool bind(const char *name, const void *value, size_t size)
{
    void *p = tcc_get_symbol(tcc, name);
    if (!p) {
        fprintf(stderr, "recompiler: missing binding %s\n", name);
        return false;
    }
    memcpy(p, value, size);
    return true;
}

static bool layout_matches(void)
{
    size_t (*probe)(int) = (size_t (*)(int))tcc_get_symbol(tcc, "wb_layout");
    const size_t host[] = {
        sizeof(Gb), offsetof(Gb, cpu), offsetof(Gb, rom_bank), offsetof(Gb, io),
        offsetof(Gb, ie), offsetof(Gb, cycles), sizeof(Cpu),
    };
    if (!probe)
        return false;
    for (int i = 0; i < (int)(sizeof host / sizeof *host); i++)
        if (probe(i) != host[i]) {
            fprintf(stderr, "recompiler: struct layout mismatch (%d: %zu vs %zu)\n", i, probe(i), host[i]);
            return false;
        }
    return true;
}

bool native_load(void)
{
    Uint64 t0 = SDL_GetPerformanceCounter();
    RecompResult r;
    if (!recomp_run(gb.rom, gb.rom_size, &game_notes, &r))
        return false;
    Uint64 t1 = SDL_GetPerformanceCounter();

    size_t plen = strlen(tcc_prelude);
    char *src = malloc(plen + r.src_len + 1);
    if (!src) {
        recomp_free(&r);
        return false;
    }
    memcpy(src, tcc_prelude, plen);
    memcpy(src + plen, r.src, r.src_len);
    src[plen + r.src_len] = 0;

    tcc = tcc_new();
    bool ok = tcc != NULL;
    if (ok) {
        tcc_set_error_func(tcc, NULL, tcc_error);
        tcc_set_options(tcc, "-nostdlib -nostdinc");
        tcc_set_output_type(tcc, TCC_OUTPUT_MEMORY);
        ok = tcc_compile_string(tcc, src) == 0 && tcc_relocate(tcc, TCC_RELOCATE_AUTO) >= 0;
    }
    free(src);

    if (ok) {
        Gb *gbp = &gb;
        bool *tracep = &trace_enabled;
        uint8_t (*rd)(uint16_t) = mem_read;
        void (*wr)(uint16_t, uint8_t) = mem_write;
        void (*tick)(int) = gb_tick;
        void (*halt)(void) = gb_halt, (*stop)(void) = gb_stop;
        void (*ill)(uint16_t) = gb_illegal, (*tr)(uint16_t) = trace_insn;
        ok = bind("wb_gb", &gbp, sizeof gbp) && bind("wb_trace_enabled", &tracep, sizeof tracep) &&
             bind("wb_mem_read", &rd, sizeof rd) && bind("wb_mem_write", &wr, sizeof wr) &&
             bind("wb_gb_tick", &tick, sizeof tick) && bind("wb_gb_halt", &halt, sizeof halt) &&
             bind("wb_gb_stop", &stop, sizeof stop) && bind("wb_gb_illegal", &ill, sizeof ill) &&
             bind("wb_trace_insn", &tr, sizeof tr) && layout_matches();
    }

    GenFn *fns = ok ? calloc(r.nfuncs ? r.nfuncs : 1, sizeof *fns) : NULL;
    for (uint32_t i = 0; ok && i < r.nfuncs; i++) {
        char name[16];
        recomp_fn_name(r.funcs[i].bank, r.funcs[i].entry, name);
        fns[i] = (GenFn)tcc_get_symbol(tcc, name);
        ok = fns[i] != NULL;
    }
    if (ok) {
        entries = malloc((r.nentries ? r.nentries : 1) * sizeof *entries);
        ok = entries != NULL;
        for (size_t i = 0; ok && i < r.nentries; i++)
            entries[i] = (GenEntry){ r.entries[i].bank, r.entries[i].addr, fns[r.entries[i].fn] };
    }
    if (ok) {
        dispatch_set_code(entries, r.nentries);
        double f = (double)SDL_GetPerformanceFrequency();
        fprintf(stderr, "recompiled %zu instructions into %u functions in %.2f s (analysis %.2f s, compile %.2f s)\n",
                r.ninsns, r.nfuncs, (SDL_GetPerformanceCounter() - t0) / f, (t1 - t0) / f,
                (SDL_GetPerformanceCounter() - t1) / f);
    } else if (tcc) {
        tcc_delete(tcc);
        tcc = NULL;
    }
    free(fns);
    recomp_free(&r);
    return ok;
}

/* Cheat codes (see cheats.h).
 *
 * GameShark  ttVVLLHH       type tt (01; 00 and 8x/9x are accepted as 01),
 *                           value VV, address HHLL; written every VBlank.
 * Game Genie VVA-AAA[-CCC]  replaces the ROM byte at an address in
 *                           0000-7FFF; with the compare byte only where
 *                           the original byte matches it (on real hardware
 *                           that selects the ROM bank).  Applied by patching
 *                           the loaded ROM image, which is the same as
 *                           patching every read and costs nothing. */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gb.h"
#include "cheats.h"
#include "files.h"

typedef struct { uint16_t addr; uint8_t value; } RamWrite;
typedef struct { uint32_t offset; uint8_t value, original; } RomPatch;

static RamWrite ram_writes[CHEATS_MAX * 4];
static size_t n_ram;
static RomPatch rom_patches[CHEATS_MAX * 4 * 32];
static size_t n_rom;
static size_t n_fixed;   /* the first n_fixed patches (start stage) ignore F8 */
static uint8_t patch_bank[sizeof rom_patches / sizeof *rom_patches];
static uint16_t patch_addr[sizeof rom_patches / sizeof *rom_patches];
static bool enabled = true;
bool dispatch_dirty;

static int hexval(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10
         : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

/* One code: kind 'S' GameShark, 'G' Game Genie (cmp < 0: no compare). */
typedef struct { char kind; uint16_t addr; uint8_t value; int cmp; } Code;

static bool parse_one(const char *tok, Code *c, char *err, size_t err_size)
{
    char h[16];
    int n = 0, dashes = 0;
    for (const char *p = tok; *p; p++) {
        if (*p == '-') { dashes++; continue; }
        if (hexval(*p) < 0 || n >= 12) {
            snprintf(err, err_size, "NOT A CODE: %.20s", tok);
            return false;
        }
        h[n++] = *p;
    }
    int d[12];
    for (int i = 0; i < n; i++)
        d[i] = hexval(h[i]);
    if (n == 8 && !dashes) {
        int type = d[0] << 4 | d[1];
        if (type > 0x01 && (type & 0xE0) != 0x80) {
            snprintf(err, err_size, "UNSUPPORTED GAMESHARK TYPE %02X: %.20s", type, tok);
            return false;
        }
        c->kind = 'S';
        c->value = (uint8_t)(d[2] << 4 | d[3]);
        c->addr = (uint16_t)((d[6] << 12) | (d[7] << 8) | (d[4] << 4) | d[5]);
        c->cmp = -1;
        if (c->addr < 0x8000) {
            snprintf(err, err_size, "GAMESHARK ADDRESS %04X IS ROM (USE GAME GENIE): %.20s", c->addr, tok);
            return false;
        }
        return true;
    }
    if (n == 6 || n == 9) {
        c->kind = 'G';
        c->value = (uint8_t)(d[0] << 4 | d[1]);
        c->addr = (uint16_t)((((d[5] ^ 0xF) << 12) | (d[2] << 8) | (d[3] << 4) | d[4]));
        c->cmp = -1;
        if (n == 9) {
            int v = d[6] << 4 | d[8];
            c->cmp = (((v >> 2) | (v << 6)) & 0xFF) ^ 0xBA;
        }
        if (c->addr >= 0x8000) {
            snprintf(err, err_size, "GAME GENIE ADDRESS %04X IS NOT ROM: %.20s", c->addr, tok);
            return false;
        }
        return true;
    }
    snprintf(err, err_size, "NOT A GAMESHARK (8 DIGITS) OR GAME GENIE (ABC-DEF[-GHI]) CODE: %.20s", tok);
    return false;
}

/* Calls fn for each code in the list; stops at the first invalid one. */
static bool each_code(const char *codes, bool (*fn)(const Code *), char *err, size_t err_size)
{
    char buf[256];
    snprintf(buf, sizeof buf, "%s", codes);
    int count = 0;
    for (char *t = strtok(buf, " \t,+;"); t; t = strtok(NULL, " \t,+;")) {
        Code c;
        if (!parse_one(t, &c, err, err_size))
            return false;
        if (fn && !fn(&c)) {
            snprintf(err, err_size, "TOO MANY CODES");
            return false;
        }
        count++;
    }
    if (!count) {
        snprintf(err, err_size, "NO CODE ENTERED");
        return false;
    }
    return true;
}

bool cheats_parse_check(const char *codes, char *err, size_t err_size)
{
    return each_code(codes, NULL, err, err_size);
}

/* ---- active codes ---- */

static void rom_apply(bool on)
{
    if (on) {
        for (size_t i = n_fixed; i < n_rom; i++)
            gb.rom[rom_patches[i].offset] = rom_patches[i].value;
    } else {
        for (size_t i = n_rom; i-- > n_fixed;)
            gb.rom[rom_patches[i].offset] = rom_patches[i].original;
    }
    dispatch_dirty = true;
}

static bool add_code(const Code *c)
{
    if (c->kind == 'S') {
        if (n_ram == sizeof ram_writes / sizeof *ram_writes)
            return false;
        ram_writes[n_ram++] = (RamWrite){ c->addr, c->value };
        return true;
    }
    /* Game Genie: every bank the address can show, where the compare matches */
    unsigned first = c->addr < 0x4000 ? 0 : 1, last = c->addr < 0x4000 ? 0 : gb.rom_banks - 1;
    for (unsigned b = first; b <= last; b++) {
        uint32_t off = c->addr < 0x4000 ? c->addr : b * 0x4000u + (c->addr - 0x4000u);
        if (off >= gb.rom_size || (c->cmp >= 0 && gb.rom[off] != c->cmp))
            continue;
        if (n_rom == sizeof rom_patches / sizeof *rom_patches)
            return false;
        rom_patches[n_rom] = (RomPatch){ off, c->value, gb.rom[off] };
        patch_bank[n_rom] = (uint8_t)(c->addr < 0x4000 ? 0 : b);
        patch_addr[n_rom] = c->addr;
        gb.rom[off] = c->value;     /* later codes compare against earlier patches, as on hardware */
        n_rom++;
    }
    return true;
}

bool cheats_add(const char *codes)
{
    char err[160];
    /* add_code compares with and patches gb.rom directly: start from the patched ROM */
    for (size_t i = 0; i < n_rom; i++)
        gb.rom[rom_patches[i].offset] = rom_patches[i].value;
    bool ok = each_code(codes, add_code, err, sizeof err);
    if (!ok)
        fprintf(stderr, "cheat \"%s\": %s\n", codes, err);
    rom_apply(enabled);
    return ok;
}

bool cheats_active(void) { return enabled && (n_ram || n_rom > n_fixed); }

void cheats_toggle(void)
{
    if (!n_ram && n_rom == n_fixed)
        return;
    enabled = !enabled;
    rom_apply(enabled);
    fprintf(stderr, "cheats %s\n", enabled ? "on" : "off");
}

/* ---- start stage ---- */

/* Normal mode is 32 rounds, 8 worlds of 3 enemies and a boss; the round
 * index is C3F0 (0-31).  A new game passes through the title's
 * initialisation at 0A:4013, which clears it:
 *     xor a / ld [$C3F0],a / ld [$C3F1],a
 * ($C3F1 was already cleared just before).  The start stage rewrites those
 * seven bytes as
 *     ld a,ROUND / ld [$C3F0],a / xor a / nop
 * Passwords set C3F0 afterwards, so continuing is unchanged. */
#define WORLDS 8
#define STAGES_PER_WORLD 4
#define START_BANK 0x0A
#define START_ADDR 0x4013

int stage_count(void) { return WORLDS * STAGES_PER_WORLD; }

void stage_label(int i, char *buf, size_t size)
{
    int w = i / STAGES_PER_WORLD + 1, s = i % STAGES_PER_WORLD + 1;
    if (s == STAGES_PER_WORLD)
        snprintf(buf, size, "%d-%d BOSS", w, s);
    else
        snprintf(buf, size, "%d-%d ENEMY %d", w, s, s);
}

int stage_find(const char *ws)
{
    int w, s;
    if (sscanf(ws, "%d-%d", &w, &s) != 2 || w < 1 || w > WORLDS || s < 1 || s > STAGES_PER_WORLD)
        return -1;
    return (w - 1) * STAGES_PER_WORLD + s - 1;
}

int stage_next_world(int stage, int dir)
{
    int w = stage / STAGES_PER_WORLD;
    if (dir < 0 && stage % STAGES_PER_WORLD == 0)
        w--;                  /* from a world's first stage back to the previous world */
    else if (dir > 0)
        w++;
    if (w < 0)
        return 0;
    if (w >= WORLDS)
        return stage_count() - 1;
    return w * STAGES_PER_WORLD;
}

bool cheats_start_at(int stage)
{
    static const uint8_t orig[] = { 0xAF, 0xEA, 0xF0, 0xC3, 0xEA, 0xF1, 0xC3 };
    uint8_t patch[] = { 0x3E, (uint8_t)stage, 0xEA, 0xF0, 0xC3, 0xAF, 0x00 };
    uint32_t off = START_BANK * 0x4000u + (START_ADDR - 0x4000u);
    if (stage <= 0 || stage >= stage_count() || n_rom != n_fixed || off + sizeof orig > gb.rom_size ||
        memcmp(gb.rom + off, orig, sizeof orig))
        return false;
    for (size_t i = 0; i < sizeof patch; i++) {
        rom_patches[n_rom] = (RomPatch){ off + (uint32_t)i, patch[i], gb.rom[off + i] };
        patch_bank[n_rom] = START_BANK;
        patch_addr[n_rom] = (uint16_t)(START_ADDR + i);
        gb.rom[off + i] = patch[i];
        n_rom++;
    }
    n_fixed = n_rom;
    dispatch_dirty = true;
    return true;
}

void cheats_frame(void)
{
    if (!enabled)
        return;
    for (size_t i = 0; i < n_ram; i++)
        mem_write(ram_writes[i].addr, ram_writes[i].value);
}

size_t cheats_rom_patches(const uint8_t **bank, const uint16_t **addr)
{
    *bank = patch_bank;
    *addr = patch_addr;
    return enabled ? n_rom : n_fixed;
}

/* ---- cheats file ---- */

/* A lost bout is recorded at 01:48DC (ld a,$01 / ld [$C590],a; 2 = won,
 * 0 = draw); with 0 instead the bout counts as a draw and is replayed,
 * so a match is never lost.  The stage timer (BCD at C585-C587) has $01
 * subtracted every 120 frames by 1FD6; the operand is at 1FEC. */
static const Cheat builtin[] = {
    { false, "NEVER LOSE A BOUT", "008-DDB-EAE" },
    { false, "FREEZE STAGE TIMER", "00F-ECE-EAE" },
};

int cheats_builtin(Cheat *out, int max)
{
    int n = 0;
    for (size_t i = 0; i < sizeof builtin / sizeof *builtin && n < max; i++)
        if (builtin[i].codes[0])
            out[n++] = builtin[i];
    return n;
}

static char *trim(char *s)
{
    while (isspace((unsigned char)*s))
        s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1]))
        *--e = 0;
    return s;
}

int cheats_file_load(const char *path, Cheat *out, int max)
{
    size_t size;
    char *d = path ? (char *)file_read(path, &size, 1 << 20) : NULL;
    if (!d)
        return cheats_builtin(out, max);
    char *text = realloc(d, size + 1);
    if (!text) {
        free(d);
        return 0;
    }
    text[size] = 0;
    int n = 0;
    for (char *line = strtok(text, "\r\n"); line && n < max; line = strtok(NULL, "\r\n")) {
        line = trim(line);
        if (!*line || *line == '#')
            continue;
        Cheat c = { 0 };
        if (!strncmp(line, "on ", 3))
            c.on = true, line += 3;
        else if (!strncmp(line, "off ", 4))
            line += 4;
        char *eq = strrchr(line, '=');
        if (eq) {
            *eq = 0;
            snprintf(c.name, sizeof c.name, "%s", trim(line));
            line = eq + 1;
        }
        snprintf(c.codes, sizeof c.codes, "%s", trim(line));
        if (!c.name[0])
            snprintf(c.name, sizeof c.name, "%.63s", c.codes);
        if (c.codes[0])
            out[n++] = c;
    }
    free(text);
    return n;
}

bool cheats_file_save(const char *path, const Cheat *list, int n)
{
    size_t cap = 512 + (size_t)n * 280, len = 0;
    char *text = malloc(cap);
    if (!text || !path) {
        free(text);
        return false;
    }
    len += (size_t)snprintf(text + len, cap - len,
        "# WarioBlast Recomp cheats, one per line:  on|off  NAME = CODES\n"
        "# CODES: GameShark (8 digits, 01VVLLHH: value VV to RAM HHLL) or Game Genie (ABC-DEF or\n"
        "# ABC-DEF-GHI); several codes separated by spaces or '+'.\n");
    for (int i = 0; i < n; i++)
        len += (size_t)snprintf(text + len, cap - len, "%s %s = %s\n", list[i].on ? "on" : "off",
                                list[i].name, list[i].codes);
    bool ok = file_write(path, text, len);
    free(text);
    return ok;
}

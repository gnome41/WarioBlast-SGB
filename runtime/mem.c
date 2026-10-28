/* Memory map, MBC1 cartridge, IO registers, timer. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gb.h"
#include "files.h"
#include "mods.h"
#include "recomp/recomp.h"

Gb gb;

static void mbc1_update(void)
{
    unsigned lo = gb.mbc_lo & 0x1F;
    if (lo == 0)
        lo = 1;
    gb.rom_bank = (lo | ((unsigned)gb.mbc_hi << 5)) % gb.rom_banks;
    gb.ram_bank = gb.mbc_mode ? gb.mbc_hi : 0;
    if (gb.sram_size <= 0x2000)
        gb.ram_bank = 0;
}

bool cart_load(const char *path)
{
    size_t size;
    uint8_t *rom = file_read(path, &size, 8 * 1024 * 1024);
    if (!rom)
        return false;
    if (size < 0x8000) {
        free(rom);
        return false;
    }
    free(gb.rom);
    gb.rom = rom;
    gb.rom_size = size;
    gb.rom_banks = size / 0x4000;
    static const size_t ram_sizes[] = { 0, 0x800, 0x2000, 0x8000 };
    uint8_t rs = gb.rom[0x149];
    gb.sram_size = rs < 4 ? ram_sizes[rs] : 0x8000;
    return true;
}

void cart_load_sram(const char *path)
{
    size_t n;
    uint8_t *d = file_read(path, &n, sizeof gb.sram);
    if (!d)
        return;
    memcpy(gb.sram, d, n < gb.sram_size ? n : gb.sram_size);
    free(d);
}

void cart_save_sram(const char *path)
{
    if (!gb.sram_size)
        return;
    if (!file_write(path, gb.sram, gb.sram_size))
        fprintf(stderr, "warning: cannot write %s\n", path);
    gb.sram_dirty = false;
}

void mem_reset(void)
{
    memset(gb.vram, 0, sizeof gb.vram);
    memset(gb.wram, 0, sizeof gb.wram);
    memset(gb.oam, 0, sizeof gb.oam);
    memset(gb.hram, 0, sizeof gb.hram);
    memset(gb.io, 0, sizeof gb.io);
    gb.mbc_lo = 1;
    gb.mbc_hi = 0;
    gb.mbc_mode = 0;
    mbc1_update();
    gb.io[IO_P1] = 0x30;
    gb.io[IO_LCDC] = 0x91;
    gb.io[IO_STAT] = 0x85;
    gb.io[IO_BGP] = 0xFC;
    gb.io[IO_OBP0] = 0xFF;
    gb.io[IO_OBP1] = 0xFF;
    gb.io[IO_IF] = 0x01;
    gb.div_counter = 0xABCC;
    static const uint8_t apu_init_regs[][2] = {
        {0x26, 0xF1}, {0x10, 0x80}, {0x11, 0xBF}, {0x12, 0xF3}, {0x14, 0xBF},
        {0x16, 0x3F}, {0x19, 0xBF}, {0x1A, 0x7F}, {0x1B, 0xFF}, {0x1C, 0x9F},
        {0x1E, 0xBF}, {0x20, 0xFF}, {0x23, 0xBF}, {0x24, 0x77}, {0x25, 0xF3},
    };
    for (size_t i = 0; i < sizeof apu_init_regs / sizeof apu_init_regs[0]; i++)
        apu_write(0xFF00 | apu_init_regs[i][0], apu_init_regs[i][1]);
}

static uint8_t pad_merge(const uint8_t *b)
{
    uint8_t m = b[0] | b[1] | b[2] | b[3];
    if ((m & 0x03) == 0x03) m &= ~0x03;
    if ((m & 0x0C) == 0x0C) m &= ~0x0C;
    return m;
}

static uint8_t joypad_read(void)
{
    uint8_t sel = gb.io[IO_P1] & 0x30;
    /* one player: every controller drives it; SGB multiplayer: one each */
    uint8_t pressed = pad_merge(gb.buttons);
    uint8_t lo = 0x0F;
    if (sel == 0x30)
        return 0xC0 | sel | (opt.sgb ? sgb_p1_read_ids() : 0x0F);
    if (opt.sgb && sgb_player_count() > 1)
        pressed = gb.buttons[0x0F - sgb_p1_read_ids()];
    if (!(sel & 0x10))
        lo &= ~(pressed & 0x0F);
    if (!(sel & 0x20))
        lo &= ~(pressed >> 4);
    return 0xC0 | sel | lo;
}

static uint8_t io_read(uint8_t r)
{
    switch (r) {
    case IO_P1: return joypad_read();
    case IO_SC: return gb.io[r] | 0x7E;
    case IO_DIV: return gb.div_counter >> 8;
    case IO_TAC: return gb.io[r] | 0xF8;
    case IO_IF: return gb.io[r] | 0xE0;
    case IO_STAT: return gb.io[r] | 0x80;
    }
    if (r >= 0x10 && r < 0x40)
        return apu_read(0xFF00 | r);
    if (r >= 0x4C)
        return 0xFF;
    return gb.io[r];
}

static void io_write(uint8_t r, uint8_t v)
{
    switch (r) {
    case IO_P1:
        gb.io[r] = v & 0x30;
        if (opt.sgb)
            sgb_p1_write(v);
        return;
    case IO_SC:
        gb.io[r] = v;
        if ((v & 0x81) == 0x81) {  /* no link partner: transfer completes reading 0xFF */
            gb.io[IO_SB] = 0xFF;
            gb.io[IO_SC] &= 0x7F;
            gb.io[IO_IF] |= INT_SERIAL;
        }
        return;
    case IO_DIV: gb.div_counter = 0; return;
    case IO_IF: gb.io[r] = v & 0x1F; return;
    case IO_LCDC: ppu_lcdc_write(v); return;
    case IO_STAT: gb.io[r] = (gb.io[r] & 0x07) | (v & 0x78); return;
    case IO_LY: return;
    case IO_DMA: {
        uint16_t src = v << 8;
        for (int i = 0; i < 0xA0; i++)
            gb.oam[i] = mem_read(src + i);
        gb.io[r] = v;
        return;
    }
    }
    if (r >= 0x10 && r < 0x40) {
        apu_write(0xFF00 | r, v);
        return;
    }
    gb.io[r] = v;
}

uint8_t mem_read(uint16_t a)
{
    if (trace_enabled && ((a >= 0xC000 && a < 0xE000) || (a >= 0xFF80 && a < 0xFFFF)))
        trace_read(a, a >= 0xFF80 ? gb.hram[a - 0xFF80] : gb.wram[a - 0xC000]);
    if (a < 0x4000)
        return gb.rom[a];
    if (a < 0x8000)
        return gb.rom[gb.rom_bank * 0x4000 + (a - 0x4000)];
    if (a < 0xA000)
        return gb.vram[a - 0x8000];
    if (a < 0xC000) {
        if (!gb.sram_enabled || !gb.sram_size)
            return 0xFF;
        return gb.sram[(gb.ram_bank * 0x2000 + (a - 0xA000)) % gb.sram_size];
    }
    if (a < 0xE000)
        return gb.wram[a - 0xC000];
    if (a < 0xFE00)
        return gb.wram[a - 0xE000];
    if (a < 0xFEA0)
        return gb.oam[a - 0xFE00];
    if (a < 0xFF00)
        return 0x00;
    if (a < 0xFF80)
        return io_read(a & 0x7F);
    if (a < 0xFFFF)
        return gb.hram[a - 0xFF80];
    return gb.ie;
}

void mem_write(uint16_t a, uint8_t v)
{
    if (trace_enabled)
        trace_write(a, v);
    if (a < 0x8000) {
        switch (a >> 13) {
        case 0: {
            bool en = (v & 0x0F) == 0x0A;
            if (gb.sram_enabled && !en && gb.sram_dirty)
                gb.sram_dirty = false, gb.sram_flush_request = true;
            gb.sram_enabled = en;
            break;
        }
        case 1: gb.mbc_lo = v & 0x1F; break;
        case 2: gb.mbc_hi = v & 0x03; break;
        case 3: gb.mbc_mode = v & 1; break;
        }
        mbc1_update();
        return;
    }
    if (a < 0xA000) {
        gb.vram[a - 0x8000] = v;
        if (a >= 0x9800)
            map_gen[a - 0x9800] = screen_gen;
        return;
    }
    if (a < 0xC000) {
        if (gb.sram_enabled && gb.sram_size) {
            gb.sram[(gb.ram_bank * 0x2000 + (a - 0xA000)) % gb.sram_size] = v;
            gb.sram_dirty = true;
        }
        return;
    }
    if (a < 0xE000) {
        gb.wram[a - 0xC000] = v;
        if (modaudio_watch && (a == game_audio.music_request || a == game_audio.sfx_request))
            modaudio_on_write(a, v);   /* output-only: replacement audio */
        return;
    }
    if (a < 0xFE00) {
        gb.wram[a - 0xE000] = v;
        return;
    }
    if (a < 0xFEA0) {
        gb.oam[a - 0xFE00] = v;
        return;
    }
    if (a < 0xFF00)
        return;
    if (a < 0xFF80) {
        io_write(a & 0x7F, v);
        return;
    }
    if (a < 0xFFFF) {
        gb.hram[a - 0xFF80] = v;
        return;
    }
    gb.ie = v;
}

void timer_tick(int n)
{
    static const int tac_bit[4] = { 9, 3, 5, 7 };
    uint8_t tac = gb.io[IO_TAC];
    for (int i = 0; i < n; i += 4) {
        uint16_t old = gb.div_counter;
        gb.div_counter += 4;
        if (!(tac & 4))
            continue;
        int bit = tac_bit[tac & 3];
        if (((old >> bit) & 1) && !((gb.div_counter >> bit) & 1)) {
            if (++gb.io[IO_TIMA] == 0) {
                gb.io[IO_TIMA] = gb.io[IO_TMA];
                gb.io[IO_IF] |= INT_TIMER;
            }
        }
    }
}

void gb_tick(int n)
{
    gb.cycles += n;
    timer_tick(n);
    ppu_tick(n);
    apu_tick(n);
}

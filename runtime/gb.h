/* Core machine state shared by the runtime, the interpreter and the
 * statically recompiled code. */
#ifndef GB_H
#define GB_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define CPU_HZ 4194304
#define CYCLES_PER_FRAME 70224

typedef struct {
    uint8_t a, f, b, c, d, e, h, l;
    uint16_t sp, pc;
    uint8_t ime;       /* interrupt master enable */
    uint8_t ei_delay;  /* EI takes effect after the following instruction */
} Cpu;

typedef struct {
    Cpu cpu;

    /* cartridge */
    uint8_t *rom;
    size_t rom_size;
    unsigned rom_banks;
    uint8_t sram[0x8000];
    size_t sram_size;
    bool sram_enabled, sram_dirty, sram_flush_request;
    uint8_t mbc_lo, mbc_hi, mbc_mode;
    unsigned rom_bank;   /* bank currently mapped at 0x4000-0x7FFF */
    unsigned ram_bank;

    uint8_t vram[0x2000];
    uint8_t wram[0x2000];
    uint8_t oam[0xA0];
    uint8_t hram[0x80];
    uint8_t io[0x80];
    uint8_t ie;

    /* timer */
    uint16_t div_counter;
    int tima_reload;

    /* ppu */
    int ppu_dot;        /* dot within the current line */
    int ppu_mode;
    int window_line;
    bool stat_line;     /* STAT interrupt line, for rising-edge detection */
    int lcd_off_cycles; /* frame pacing while the LCD is disabled */

    /* joypad: bit set = pressed. bits 0-3: right left up down, 4-7: A B select start */
    uint8_t buttons[4];

    uint64_t cycles;
    uint64_t frame_count;
} Gb;

extern Gb gb;
#define R (gb.cpu)

/* IO register indices */
enum {
    IO_P1 = 0x00, IO_SB = 0x01, IO_SC = 0x02, IO_DIV = 0x04, IO_TIMA = 0x05,
    IO_TMA = 0x06, IO_TAC = 0x07, IO_IF = 0x0F, IO_LCDC = 0x40, IO_STAT = 0x41,
    IO_SCY = 0x42, IO_SCX = 0x43, IO_LY = 0x44, IO_LYC = 0x45, IO_DMA = 0x46,
    IO_BGP = 0x47, IO_OBP0 = 0x48, IO_OBP1 = 0x49, IO_WY = 0x4A, IO_WX = 0x4B,
};

enum { INT_VBLANK = 1, INT_STAT = 2, INT_TIMER = 4, INT_SERIAL = 8, INT_JOYPAD = 16 };

/* display views: render-only, never affect emulation */
enum { VIEW_ORIGINAL, VIEW_FRAME, VIEW_WIDE, VIEW_COUNT };
#define WIDE_EXTRA 48                 /* extra columns each side in the wide view */
#define WIDE_W (160 + 2 * WIDE_EXTRA) /* 256: 16:9 at 144 lines */

/* options */
typedef struct {
    bool sgb;          /* emulate Super Game Boy */
    bool interp_only;  /* ignore recompiled code */
    bool headless;
    bool unthrottled;
    bool no_save;      /* neither load nor write battery RAM (for tests) */
    bool swap_ab;      /* controller: Nintendo button positions */
    bool fullscreen;
    bool no_bios_search; /* launcher already chose (or declined) a BIOS */
    int scale;
    int view;          /* VIEW_* */
    long max_frames;   /* 0 = unlimited */
    const char *fallback_log;
    const char *input_script;  /* deterministic input: "random:SEED" */
    const char *hash_log;      /* per-frame framebuffer hash output */
    const char *screenshot;    /* PPM written at exit */
    const char *wav;           /* audio capture (headless) */
    const char *sgb_bios;      /* Super Game Boy SNES ROM, for SGB sound */
} Options;
extern Options opt;

/* mem.c */
uint8_t mem_read(uint16_t addr);
void mem_write(uint16_t addr, uint8_t v);
bool cart_load(const char *path);
void cart_load_sram(const char *path);
void cart_save_sram(const char *path);
void mem_reset(void);

/* timing: advance every peripheral by n T-cycles */
void gb_tick(int n);
void timer_tick(int n);

/* ppu.c */
void ppu_tick(int n);
void ppu_lcdc_write(uint8_t v);
extern uint8_t ppu_shades[144][160];   /* final DMG shade 0-3 per pixel */
extern uint8_t ppu_wide[144][WIDE_W];  /* widescreen view (VIEW_WIDE only) */
/* Render-only bookkeeping for the wide view: the "screen generation"
 * (bumped when the LCD is switched off, i.e. a new screen is being set up)
 * in which each tilemap entry was last written.  Entries not written for
 * the current screen are leftovers and are not shown outside the 160-px view. */
extern uint32_t map_gen[0x800];
extern uint32_t screen_gen;
extern bool wide_has_content;   /* this frame drew something valid outside the 160 px */

/* apu.c */
void apu_init(int sample_rate);
void apu_tick(int n);
uint8_t apu_read(uint16_t addr);
void apu_write(uint16_t addr, uint8_t v);
size_t apu_take_samples(int16_t *out, size_t max_frames);
int apu_sample_rate(void);
void apu_set_mute_mask(uint8_t mask);   /* output only: channels 1-4 = bits 0-3 */

/* sgb.c */
void sgb_reset(void);
void sgb_p1_write(uint8_t v);
uint8_t sgb_p1_read_ids(void);
int sgb_player_count(void);    /* 1, 2 or 4 (MLT_REQ) */
void sgb_frame(void);
void sgb_render(uint32_t *out256x224);
void sgb_render_wide(uint32_t *out256x144);
int sgb_mask_mode(void);
uint32_t sgb_backdrop(void);
uint32_t sgb_pixel_color(int cx, int cy, int shade);
bool sgb_has_border(void);
uint32_t sgb_border_color(int pal, int ci);
uint16_t sgb_border_entry(int tx, int ty);
const uint8_t *sgb_border_tile(int tile);
bool sgb_active(void);

/* sgb_sound.c: SNES-side sound (needs the SGB BIOS) */
bool sgb_sound_init(const char *bios_path);
bool sgb_bios_valid(const char *bios_path);
bool sgb_sound_enabled(void);
void sgb_sound_command(const uint8_t *packet);
void sgb_sound_transfer(const uint8_t *data, size_t n);
void sgb_sound_frame(void);
void sgb_sound_mix(int16_t *buf, size_t n, int rate);

/* platform */
void platform_frame(void);   /* called at the start of each VBlank */
void platform_shutdown(void);

/* cpu / dispatch */
void cpu_reset(void);
void interp_step(void);
void run_forever(void);
void gb_halt(void);
void gb_stop(void);
void gb_illegal(uint16_t pc);
void fallback_log_hit(unsigned bank, uint16_t addr);
void fallback_log_write(void);
extern bool trace_enabled;
void trace_insn(uint16_t pc);
void trace_write(uint16_t addr, uint8_t v);
void trace_read(uint16_t addr, uint8_t v);
void trace_frame(void);

/* recompiled code: linked in (developer build) or compiled at runtime
 * from the user's ROM (player build, runtime/native.c) */
typedef void (*GenFn)(void);
typedef struct { uint8_t bank; uint16_t addr; GenFn fn; } GenEntry;
void dispatch_set_code(const GenEntry *entries, size_t count);
uint32_t expected_rom_crc32(void);

#endif

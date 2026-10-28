/* SDL2 platform layer: window, input, audio, frame pacing, plus a
 * headless mode for automated testing. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL.h>
#include "gb.h"
#include "pad.h"
#include "mods.h"
#include "cheats.h"

#define SAMPLE_RATE 48000

static SDL_Window *window;
static SDL_Renderer *renderer;
static SDL_Texture *texture;
static SDL_AudioDeviceID audio;
static uint8_t kb_buttons;      /* keyboard state; controllers are polled */
static bool tab_held, cli_unthrottled;
static int tex_w, tex_h;
#define CANVAS_MAX (400 * 224)
static uint32_t pixels[CANVAS_MAX];
static uint32_t base[256 * 224];
static uint64_t next_frame_time;
static FILE *hash_file;
static uint64_t rng_state;
static bool scripted;
static const char *sram_path;
static FILE *wav_file;
static uint32_t wav_frames;

static void wav_header(void)
{
    uint32_t data = wav_frames * 4;
    uint8_t h[44] = "RIFF....WAVEfmt \x10\0\0\0\1\0\2\0........\4\0\x10\0data....";
    uint32_t v[] = { 36 + data, SAMPLE_RATE, SAMPLE_RATE * 4, data };
    memcpy(h + 4, &v[0], 4);
    memcpy(h + 24, &v[1], 4);
    memcpy(h + 28, &v[2], 4);
    memcpy(h + 40, &v[3], 4);
    fseek(wav_file, 0, SEEK_SET);
    fwrite(h, 1, 44, wav_file);
}

static const uint32_t dmg_colors[4] = { 0xFFE0F8D0, 0xFF88C070, 0xFF346856, 0xFF081820 };

void platform_set_sram_path(const char *p) { sram_path = p; }

/* Report a fatal error on stderr and, for windowed runs, in a message box
 * (Windows GUI builds have no console). */
void platform_error(const char *msg)
{
    fprintf(stderr, "%s\n", msg);
    if (!opt.headless)
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "WarioBlast Recomp", msg, NULL);
}

static uint64_t rng(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}

/* Deterministic pseudo-random play: holds a random button combination for
 * a few frames at a time, pressing Start/A often enough to get through
 * menus. */
static void scripted_input(void)
{
    if (gb.frame_count % 6)
        return;
    uint64_t r = rng();
    uint8_t b = 0;
    static const uint8_t dirs[] = { 0x00, 0x01, 0x02, 0x04, 0x08, 0x05, 0x06, 0x09, 0x0A };
    b |= dirs[r % 9];
    if ((r >> 8) % 3 == 0) b |= 0x10;   /* A */
    if ((r >> 12) % 5 == 0) b |= 0x20;  /* B */
    if ((r >> 16) % 23 == 0) b |= 0x80; /* Start */
    if ((r >> 24) % 97 == 0) b |= 0x40; /* Select */
    gb.buttons[0] = b;
}

static uint64_t frame_hash(void)
{
    uint64_t h = 1469598103934665603ull;
#define MIX(p, n) for (size_t i_ = 0; i_ < (n); i_++) { h ^= ((const uint8_t *)(p))[i_]; h *= 1099511628211ull; }
    MIX(ppu_shades, sizeof ppu_shades);
    MIX(gb.wram, sizeof gb.wram);
    MIX(gb.hram, sizeof gb.hram);
    uint8_t regs[10] = { R.a, R.f, R.b, R.c, R.d, R.e, R.h, R.l, (uint8_t)R.sp, (uint8_t)(R.sp >> 8) };
    MIX(regs, sizeof regs);
    MIX(&gb.cycles, sizeof gb.cycles);
#undef MIX
    return h;
}

static void set_key(uint8_t mask, bool down)
{
    if (down)
        kb_buttons |= mask;
    else
        kb_buttons &= ~mask;
}

static uint8_t key_mask(SDL_Keycode k)
{
    switch (k) {
    case SDLK_RIGHT: return 0x01;
    case SDLK_LEFT: return 0x02;
    case SDLK_UP: return 0x04;
    case SDLK_DOWN: return 0x08;
    case SDLK_x: case SDLK_k: return 0x10;
    case SDLK_z: case SDLK_j: return 0x20;
    case SDLK_RSHIFT: case SDLK_BACKSPACE: return 0x40;
    case SDLK_RETURN: return 0x80;
    }
    return 0;
}

/* ---- views ---- */

static void view_size(int view, int *w, int *h)
{
    switch (view) {
    case VIEW_FRAME: *w = opt.sgb ? 400 : 256; *h = opt.sgb ? 224 : 144; break;
    case VIEW_WIDE: *w = WIDE_W; *h = 144; break;
    default: *w = opt.sgb ? 256 : 160; *h = opt.sgb ? 224 : 144; break;
    }
}

static void render_base(int *bw, int *bh)
{
    if (opt.sgb) {
        sgb_render(base);
        *bw = 256;
        *bh = 224;
    } else {
        for (int y = 0; y < 144; y++)
            for (int x = 0; x < 160; x++)
                base[y * 160 + x] = dmg_colors[ppu_shades[y][x]];
        *bw = 160;
        *bh = 144;
    }
}

/* Fills out (w x h) with a blurred, darkened stretch of src (sw x sh),
 * except columns off .. off+sw-1, which get src itself, unscaled. */
static void ambient_fill(uint32_t *out, int w, int h, const uint32_t *src, int sw, int sh, int off)
{
    enum { CELL = 8 };
    int gw = sw / CELL, gh = sh / CELL;
    static float grid[32 * 28][3];
    for (int gy = 0; gy < gh; gy++)
        for (int gx = 0; gx < gw; gx++) {
            unsigned r = 0, g = 0, b = 0;
            for (int y = 0; y < CELL; y++)
                for (int x = 0; x < CELL; x++) {
                    uint32_t c = src[(gy * CELL + y) * sw + gx * CELL + x];
                    r += (c >> 16) & 0xFF;
                    g += (c >> 8) & 0xFF;
                    b += c & 0xFF;
                }
            float *dst = grid[gy * gw + gx];
            dst[0] = r / (float)(CELL * CELL);
            dst[1] = g / (float)(CELL * CELL);
            dst[2] = b / (float)(CELL * CELL);
        }
    for (int y = 0; y < h; y++) {
        float v = (y + 0.5f) * gh / h - 0.5f;
        int y0 = v < 0 ? 0 : (int)v, y1 = y0 + 1 < gh ? y0 + 1 : gh - 1;
        float fy = v - y0 < 0 ? 0 : v - y0;
        for (int x = 0; x < w; x++) {
            if (x >= off && x < off + sw) {
                out[y * w + x] = src[y * sw + x - off];
                continue;
            }
            float u = (x + 0.5f) * gw / w - 0.5f;
            int x0 = u < 0 ? 0 : (int)u, x1 = x0 + 1 < gw ? x0 + 1 : gw - 1;
            float fx = u - x0 < 0 ? 0 : u - x0;
            uint32_t c = 0xFF000000u;
            for (int k = 0; k < 3; k++) {
                float top = grid[y0 * gw + x0][k] * (1 - fx) + grid[y0 * gw + x1][k] * fx;
                float bot = grid[y1 * gw + x0][k] * (1 - fx) + grid[y1 * gw + x1][k] * fx;
                int val = (int)((top * (1 - fy) + bot * fy) * 0.45f);
                c |= (uint32_t)val << (16 - 8 * k);
            }
            out[y * w + x] = c;
        }
    }
}

/* 16:9 frame: the unscaled image centred, the sides filled with a blurred,
 * darkened stretch of the same image. */
static void compose_frame(uint32_t *out, int w, int h)
{
    int bw, bh;
    render_base(&bw, &bh);
    ambient_fill(out, w, h, base, bw, bh, (w - bw) / 2);
}

/* Wide view.  When nothing valid was drawn outside the original 160 px
 * this frame (menus, cut-scenes, LCD off), the sides fall back to the
 * ambient fill around the unscaled centre instead of flat colour. */
static void compose_wide(uint32_t *out)
{
    if (opt.sgb)
        sgb_render_wide(out);
    else
        for (int y = 0; y < 144; y++)
            for (int x = 0; x < WIDE_W; x++)
                out[y * WIDE_W + x] = dmg_colors[ppu_wide[y][x]];
    if (!wide_has_content) {
        for (int y = 0; y < 144; y++)
            memcpy(base + y * 160, out + y * WIDE_W + WIDE_EXTRA, 160 * 4);
        ambient_fill(out, WIDE_W, 144, base, 160, 144, WIDE_EXTRA);
    }
}

static void compose_view(uint32_t *out, int *w, int *h)
{
    view_size(opt.view, w, h);
    if (opt.view == VIEW_FRAME) {
        compose_frame(out, *w, *h);
    } else if (opt.view == VIEW_WIDE) {
        compose_wide(out);
    } else {
        int bw, bh;
        render_base(&bw, &bh);
        memcpy(out, base, (size_t)bw * bh * 4);
    }
}

static void ensure_texture(int w, int h);

/* (Re)creates the texture for the current view and resizes the window to
 * an integer multiple of it, unless fullscreen. */
static void apply_view(void)
{
    view_size(opt.view, &tex_w, &tex_h);
    ensure_texture(tex_w, tex_h);
    SDL_RenderSetLogicalSize(renderer, tex_w, tex_h);
    if (!(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN_DESKTOP)) {
        int scale = opt.scale > 0 ? opt.scale : (672 + tex_h / 2) / tex_h;
        SDL_SetWindowSize(window, tex_w * scale, tex_h * scale);
    }
}

static void cycle_view(void)
{
    opt.view = (opt.view + 1) % VIEW_COUNT;
    apply_view();
}

static void toggle_fullscreen(void)
{
    bool fs = SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN_DESKTOP;
    SDL_SetWindowFullscreen(window, fs ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
    if (fs)
        apply_view();   /* restore the window size for the current view */
}

/* The frame for the current view: native size, or the mods' HD scale. */
static const uint32_t *compose_output(int *w, int *h, int max_scale)
{
    compose_view(pixels, w, h);
    uint32_t *hd;
    int hw, hh;
    if (hires_compose(opt.view, pixels, *w, *h, max_scale, &hd, &hw, &hh)) {
        *w = hw;
        *h = hh;
        return hd;
    }
    return pixels;
}

static void write_screenshot(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return;
    int w, h;
    const uint32_t *px = compose_output(&w, &h, 0);   /* full pack scale */
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int i = 0; i < w * h; i++) {
        uint8_t rgb[3] = { px[i] >> 16, px[i] >> 8, px[i] };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

/* Matches the texture to the output size (HD packs give a larger one than
 * the logical view size; it is then scaled smoothly). */
static int tex_px_w, tex_px_h;

static void ensure_texture(int w, int h)
{
    if (texture && w == tex_px_w && h == tex_px_h)
        return;
    if (texture)
        SDL_DestroyTexture(texture);
    texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, w, h);
    SDL_SetTextureScaleMode(texture, w > tex_w ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);
    tex_px_w = w;
    tex_px_h = h;
}

void platform_shutdown(void)
{
    if (opt.screenshot)
        write_screenshot(opt.screenshot);
    moddump_finish();
    if (getenv("WB_DUMP_VRAM")) {   /* debugging aid: VRAM + IO registers */
        FILE *f = fopen(getenv("WB_DUMP_VRAM"), "wb");
        if (f) {
            fwrite(gb.vram, 1, sizeof gb.vram, f);
            fwrite(gb.io, 1, sizeof gb.io, f);
            fclose(f);
        }
    }
    extern uint64_t interp_count;
    if (getenv("WB_DEBUG"))
        fprintf(stderr, "interpreted instructions: %llu\n", (unsigned long long)interp_count);
    if (getenv("WB_DEBUG"))
        fprintf(stderr, "frames=%llu cycles=%llu pc=%04X bank=%u sp=%04X lcdc=%02X ie=%02X if=%02X ime=%d\n",
                (unsigned long long)gb.frame_count, (unsigned long long)gb.cycles, R.pc,
                gb.rom_bank, R.sp, gb.io[IO_LCDC], gb.ie, gb.io[IO_IF], R.ime);
    if (sram_path)
        cart_save_sram(sram_path);
    fallback_log_write();
    if (hash_file)
        fclose(hash_file);
    if (wav_file) {
        wav_header();
        fclose(wav_file);
    }
    if (window)
        SDL_Quit();
}

static void quit(void)
{
    platform_shutdown();
    exit(0);
}

bool platform_init(void)
{
    if (opt.input_script) {
        if (strncmp(opt.input_script, "random:", 7) != 0) {
            fprintf(stderr, "unknown input script '%s' (expected random:SEED)\n", opt.input_script);
            return false;
        }
        rng_state = strtoull(opt.input_script + 7, NULL, 0) * 2654435761u + 1;
        scripted = true;
    }
    if (opt.hash_log) {
        hash_file = fopen(opt.hash_log, "wb");
        if (!hash_file) {
            fprintf(stderr, "cannot write %s\n", opt.hash_log);
            return false;
        }
    }
    if (opt.headless) {
        if (opt.wav) {
            wav_file = fopen(opt.wav, "wb");
            if (!wav_file) {
                fprintf(stderr, "cannot write %s\n", opt.wav);
                return false;
            }
            wav_header();
        }
        apu_init(opt.wav ? SAMPLE_RATE : 0);
        return true;
    }

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return false;
    }
    view_size(opt.view, &tex_w, &tex_h);
    int scale = opt.scale > 0 ? opt.scale : (672 + tex_h / 2) / tex_h;
    window = SDL_CreateWindow("WarioBlast Recomp", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                              tex_w * scale, tex_h * scale, SDL_WINDOW_RESIZABLE);
    if (!window) {
        fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        return false;
    }
    set_window_icon(window);
    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    if (!renderer)
        renderer = SDL_CreateRenderer(window, -1, 0);
    if (!renderer) {
        fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError());
        return false;
    }
    SDL_RenderSetIntegerScale(renderer, SDL_TRUE);
    if (getenv("WB_DEBUG")) {
        SDL_RendererInfo ri;
        if (SDL_GetRendererInfo(renderer, &ri) == 0)
            fprintf(stderr, "renderer: %s%s\n", ri.name, (ri.flags & SDL_RENDERER_ACCELERATED) ? " (accelerated)" : " (software)");
    }
    apply_view();
    if (opt.fullscreen)
        SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN_DESKTOP);

    cli_unthrottled = opt.unthrottled;
    if (!opt.unthrottled) {
        SDL_AudioSpec want = { 0 }, have;
        want.freq = SAMPLE_RATE;
        want.format = AUDIO_S16SYS;
        want.channels = 2;
        want.samples = 1024;
        audio = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
        if (audio) {
            apu_init(SAMPLE_RATE);
            SDL_PauseAudioDevice(audio, 0);
        } else {
            fprintf(stderr, "warning: no audio: %s\n", SDL_GetError());
            apu_init(0);
        }
    } else {
        apu_init(0);
    }
    char *base_path = SDL_GetBasePath();
    pad_init(base_path);
    SDL_free(base_path);
    next_frame_time = SDL_GetPerformanceCounter();
    return true;
}

static void poll_events(void)
{
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_QUIT:
            quit();
            break;
        case SDL_KEYDOWN:
        case SDL_KEYUP:
            if (e.key.keysym.sym == SDLK_ESCAPE) {
                quit();
            } else if (e.key.keysym.sym == SDLK_F11 && e.type == SDL_KEYDOWN && !e.key.repeat) {
                toggle_fullscreen();
            } else if (e.key.keysym.sym == SDLK_F9 && e.type == SDL_KEYDOWN && !e.key.repeat) {
                mods_toggle();   /* compare with / without mods */
            } else if (e.key.keysym.sym == SDLK_F8 && e.type == SDL_KEYDOWN && !e.key.repeat) {
                cheats_toggle();
            } else if (e.key.keysym.sym == SDLK_F10 && e.type == SDL_KEYDOWN && !e.key.repeat) {
                cycle_view();
            } else if (e.key.keysym.sym == SDLK_TAB) {
                tab_held = e.type == SDL_KEYDOWN;  /* hold Tab to fast-forward */
            } else {
                set_key(key_mask(e.key.keysym.sym), e.type == SDL_KEYDOWN);
            }
            break;
        case SDL_CONTROLLERDEVICEADDED:
        case SDL_CONTROLLERDEVICEREMOVED:
            pad_event(&e);
            break;
        case SDL_WINDOWEVENT:
            if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST)
                kb_buttons = 0;   /* key-ups are lost while unfocused */
            break;
        }
    }
}

void platform_frame(void)
{
    gb.frame_count++;
    trace_frame();
    if (opt.sgb)
        sgb_frame();
    if (gb.sram_flush_request && sram_path) {
        gb.sram_flush_request = false;
        cart_save_sram(sram_path);
    }
    if (hash_file) {
        fprintf(hash_file, "%llu %016llx\n", (unsigned long long)gb.frame_count,
                (unsigned long long)frame_hash());
        if (getenv("WB_HASHPARTS")) {
            uint64_t h = 0;
            for (size_t i = 0; i < sizeof ppu_shades; i++) h = h * 31 + ((uint8_t *)ppu_shades)[i];
            fprintf(hash_file, "  shades %016llx", (unsigned long long)h);
            h = 0;
            for (size_t i = 0; i < sizeof gb.wram; i++) h = h * 31 + gb.wram[i];
            fprintf(hash_file, " wram %016llx", (unsigned long long)h);
            h = 0;
            for (size_t i = 0; i < sizeof gb.hram; i++) h = h * 31 + gb.hram[i];
            fprintf(hash_file, " hram %016llx cyc %llu sp %04X a %02X\n", (unsigned long long)h,
                    (unsigned long long)gb.cycles, R.sp, R.a);
        }
    }
    if (scripted)
        scripted_input();
    /* WB_RAMLOG="C000-C0FF" - debugging aid: logs a memory range each frame */
    static const char *ramlog = (const char *)-1;
    if (ramlog == (const char *)-1)
        ramlog = getenv("WB_RAMLOG");
    if (ramlog) {
        unsigned lo = 0, hi = 0;
        if (sscanf(ramlog, "%x-%x", &lo, &hi) == 2 && hi >= lo && hi <= 0xFFFF) {
            fprintf(stderr, "ram %llu", (unsigned long long)gb.frame_count);
            for (unsigned a = lo; a <= hi; a++)
                fprintf(stderr, " %02X", mem_read((uint16_t)a));
            fprintf(stderr, "\n");
        }
    }
    /* WB_HOLD="from-to:mask[@player][;...]" - debugging aid forcing the
     * buttons (bit order as gb.buttons) of player 1-4 for a range of frames */
    static const char *hold = (const char *)-1;
    if (hold == (const char *)-1)
        hold = getenv("WB_HOLD");
    for (const char *p = hold; p && *p;) {
        char *end;
        unsigned long from = strtoul(p, &end, 10), to = from, mask;
        if (*end == '-')
            to = strtoul(end + 1, &end, 10);
        if (*end != ':')
            break;
        mask = strtoul(end + 1, &end, 16);
        unsigned long player = 1;
        if (*end == '@')
            player = strtoul(end + 1, &end, 10);
        if (gb.frame_count >= from && gb.frame_count <= to && player >= 1 && player <= 4)
            gb.buttons[player - 1] = (uint8_t)mask;
        p = *end == ';' ? end + 1 : end;
    }
    cheats_frame();
    moddump_frame();
    modaudio_frame();
    if (wav_file) {
        int16_t buf[4096 * 2];
        size_t n = apu_take_samples(buf, 4096);
        sgb_sound_mix(buf, n, SAMPLE_RATE);
        moddump_audio_samples(buf, n);
        modaudio_mix(buf, n, SAMPLE_RATE);
        fwrite(buf, 4, n, wav_file);
        wav_frames += n;
    }
    if (opt.max_frames && (long)gb.frame_count >= opt.max_frames)
        quit();
    if (opt.headless) {
        wide_has_content = false;
        return;
    }

    /* HD packs: compose no larger than the window shows */
    int dw = 0, dh = 0;
    SDL_GetRendererOutputSize(renderer, &dw, &dh);
    int w, h, need = tex_h > 0 ? (dh + tex_h - 1) / tex_h : 0;
    uint64_t p0 = SDL_GetPerformanceCounter();
    const uint32_t *px = compose_output(&w, &h, need > 0 ? need : 1);
    wide_has_content = false;
    uint64_t p1 = SDL_GetPerformanceCounter();
    ensure_texture(w, h);
    SDL_UpdateTexture(texture, NULL, px, w * 4);
    uint64_t p2 = SDL_GetPerformanceCounter();
    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, texture, NULL, NULL);
    SDL_RenderPresent(renderer);
    uint64_t p3 = SDL_GetPerformanceCounter();
    static int prof = -1;
    static double t_comp, t_up, t_pres;
    static int nprof;
    if (prof < 0)
        prof = getenv("WB_PROFILE") != NULL;
    if (prof) {
        double f = (double)SDL_GetPerformanceFrequency() / 1000.0;
        t_comp += (p1 - p0) / f;
        t_up += (p2 - p1) / f;
        t_pres += (p3 - p2) / f;
        if (++nprof == 120) {
            fprintf(stderr, "profile: %dx%d  compose %.2f ms  upload %.2f ms  present %.2f ms\n",
                    w, h, t_comp / nprof, t_up / nprof, t_pres / nprof);
            t_comp = t_up = t_pres = 0;
            nprof = 0;
        }
    }
    poll_events();
    PadInput pad = pad_poll();
    if (!scripted) {
        gb.buttons[0] = pad_cancel_opposite(kb_buttons | pad.player[0]);
        for (int i = 1; i < 4; i++)
            gb.buttons[i] = pad.player[i];
    }
    opt.unthrottled = cli_unthrottled || tab_held || pad.fast_forward;
    if (pad.cycle_view)
        cycle_view();
    if (pad.toggle_fullscreen)
        toggle_fullscreen();

    int16_t buf[4096 * 2];
    size_t n = apu_take_samples(buf, 4096);
    sgb_sound_mix(buf, n, SAMPLE_RATE);
    moddump_audio_samples(buf, n);
    modaudio_mix(buf, n, SAMPLE_RATE);
    if (audio) {
        /* keep latency bounded: drop audio if more than ~100 ms is queued */
        if (SDL_GetQueuedAudioSize(audio) < SAMPLE_RATE / 10 * 4 && !opt.unthrottled)
            SDL_QueueAudio(audio, buf, n * 4);
    }

    if (opt.unthrottled)
        return;
    uint64_t freq = SDL_GetPerformanceFrequency();
    uint64_t frame = freq * CYCLES_PER_FRAME / CPU_HZ;
    next_frame_time += frame;
    uint64_t now = SDL_GetPerformanceCounter();
    if (now > next_frame_time + freq / 10) {
        next_frame_time = now;   /* far behind (e.g. window dragged): resync */
        return;
    }
    while (now < next_frame_time) {
        uint64_t ms = (next_frame_time - now) * 1000 / freq;
        if (ms > 1)
            SDL_Delay((uint32_t)(ms - 1));
        now = SDL_GetPerformanceCounter();
    }
}

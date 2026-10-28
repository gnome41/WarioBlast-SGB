/* Super Game Boy SNES-side sound.
 *
 * The SGB BIOS (a SNES cartridge ROM, supplied by the user) contains the
 * SPC700 sound driver and its built-in instrument samples.  The 65816 side
 * of the BIOS is replaced by a re-implementation of the few routines that
 * touch the APU:
 *   - boot: the driver and default score are uploaded through the IPL, then
 *     the driver starts at $0400;
 *   - SOUND (BIOS $C557): packet bytes 1-4 go to the variables at
 *     $0341/$0342/$0343/$0340, i.e. APU ports 1, 2, 3 and 0;
 *   - once per frame (BIOS $BABD) the variables are written to the ports,
 *     but only after the driver has echoed the previous values back (up to
 *     128 frames of retries); ports 0-2 are then cleared;
 *   - SOU_TRN (BIOS $C576): the driver is sent back to the IPL and the
 *     transferred block list is uploaded and started.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gb.h"
#include "files.h"
#include "snes_apu.h"
#include "mods.h"

static bool enabled;
static uint8_t vars[4];   /* BIOS $0340-$0343: next values for ports 0-3 */
static uint8_t last[4];   /* BIOS $0344-$0347: values last sent */
static int retries;       /* BIOS $034C */
static double spc_cycle_acc;

/* resampler state (32 kHz -> output rate) */
static double rs_pos = 1.0;
static int16_t rs_prev[2], rs_cur[2];

bool sgb_sound_enabled(void) { return enabled; }

/* Writes an IPL-format block list (len16, addr16, data...; len 0 = end,
 * followed by the start address) into SPC RAM.  Returns the start
 * address, or -1 if the list is malformed. */
static int upload(const uint8_t *p, size_t n)
{
    size_t i = 0;
    while (i + 4 <= n) {
        unsigned len = p[i] | (p[i + 1] << 8), addr = p[i + 2] | (p[i + 3] << 8);
        i += 4;
        if (len == 0)
            return addr;
        if (i + len > n)
            return -1;
        for (unsigned k = 0; k < len; k++)
            spc_ram[(uint16_t)(addr + k)] = p[i + k];
        i += len;
    }
    return -1;
}

/* Finds the first block list in the BIOS that satisfies pred. */
static long find_list(const uint8_t *rom, size_t n, bool (*pred)(unsigned, unsigned, unsigned, unsigned))
{
    for (size_t start = 0; start + 4 < n; start++) {
        size_t p = start;
        unsigned blocks = 0, total = 0, big_driver = 0, score = 0;
        while (p + 4 <= n && blocks < 16) {
            unsigned len = rom[p] | (rom[p + 1] << 8), addr = rom[p + 2] | (rom[p + 3] << 8);
            if (len == 0) {
                if (addr == 0x0400 && blocks && pred(blocks, total, big_driver, score))
                    return (long)start;
                break;
            }
            if (addr < 0x0200 || addr + len > 0x10000)
                break;
            if (addr == 0x0400 && len >= 0x1000)
                big_driver = 1;
            if (addr == 0x2B00)
                score = 1;
            blocks++;
            total += len;
            p += 4 + len;
        }
    }
    return -1;
}

static bool is_driver(unsigned blocks, unsigned total, unsigned drv, unsigned score)
{
    (void)blocks; (void)score;
    return drv && total >= 0x8000;
}

static bool is_score(unsigned blocks, unsigned total, unsigned drv, unsigned score)
{
    (void)total; (void)drv;
    return blocks == 1 && score;
}

/* Loads the BIOS, strips a copier header; returns NULL if unreadable. */
static uint8_t *load_bios(const char *path, size_t *n)
{
    uint8_t *rom = file_read(path, n, 0x100000);
    if (rom && *n % 0x8000 == 0x200)
        memmove(rom, rom + 0x200, *n -= 0x200);
    return rom;
}

bool sgb_bios_valid(const char *path)
{
    size_t n;
    uint8_t *rom = load_bios(path, &n);
    bool ok = rom && find_list(rom, n, is_driver) >= 0;
    free(rom);
    return ok;
}

bool sgb_sound_init(const char *path)
{
    size_t n;
    uint8_t *rom = load_bios(path, &n);
    if (!rom)
        return false;
    long drv = find_list(rom, n, is_driver);
    long score = find_list(rom, n, is_score);
    if (drv < 0) {
        fprintf(stderr, "%s: no SGB sound driver found - SNES sound disabled\n", path);
        free(rom);
        return false;
    }
    memset(spc_ram, 0, sizeof spc_ram);
    dsp_reset();
    spc_reset(0x0400);
    upload(rom + drv, n - drv);
    if (score >= 0)
        upload(rom + score, n - score);
    free(rom);
    memset(vars, 0, sizeof vars);
    memset(last, 0, sizeof last);
    retries = 0;
    enabled = true;
    return true;
}

void sgb_sound_command(const uint8_t *cmd)
{
    vars[1] = cmd[1];
    vars[2] = cmd[2];
    vars[3] = cmd[3];
    vars[0] = cmd[4];
}

void sgb_sound_transfer(const uint8_t *data, size_t n)
{
    if (!enabled)
        return;
    spc_reset(0x0400);           /* driver -> IPL: clears $01-$EF */
    int start = upload(data, n);
    if (start >= 0)
        spc_set_pc((uint16_t)start);
}

void sgb_sound_frame(void)
{
    if (!enabled || !apu_sample_rate())
        return;              /* nobody is listening: skip the SPC entirely */
    /* WB_SGB_SOUND="frame:a,b,attr,score;..." - debugging aid that
     * injects SOUND commands (hex bytes) */
    for (const char *p = getenv("WB_SGB_SOUND"); p && *p;) {
        char *end;
        unsigned long f = strtoul(p, &end, 10);
        unsigned v[4] = { 0 };
        if (*end != ':' || sscanf(end + 1, "%x,%x,%x,%x", &v[0], &v[1], &v[2], &v[3]) != 4)
            break;
        if (f == gb.frame_count) {
            uint8_t cmd[5] = { 0x41, (uint8_t)v[0], (uint8_t)v[1], (uint8_t)v[2], (uint8_t)v[3] };
            sgb_sound_command(cmd);
        }
        p = strchr(end, ';');
        p = p ? p + 1 : NULL;
    }
    /* BIOS $BABD */
    bool echoed = spc_read_port(0) == last[0] && spc_read_port(1) == last[1] &&
                  spc_read_port(2) == last[2] && spc_read_port(3) == last[3];
    if (retries < 0x80 && !echoed) {
        for (int i = 0; i < 4; i++)
            spc_write_port(i, last[i]);
        retries++;
    } else {
        for (int i = 0; i < 4; i++) {
            spc_write_port(i, vars[i]);
            last[i] = vars[i];
        }
        retries = 0;
        vars[0] = vars[1] = vars[2] = 0;
    }

    static int log = -1;
    if (log < 0)
        log = getenv("WB_SPC_LOG") != NULL;
    if (log) {
        static uint8_t prev_in[4], prev_out[4];
        uint8_t in[4], out[4];
        for (int i = 0; i < 4; i++) {
            in[i] = last[i];
            out[i] = spc_read_port(i);
        }
        if (memcmp(in, prev_in, 4) || memcmp(out, prev_out, 4))
            fprintf(stderr, "spc frame %llu: in %02X %02X %02X %02X  out %02X %02X %02X %02X%s\n",
                    (unsigned long long)gb.frame_count, in[0], in[1], in[2], in[3],
                    out[0], out[1], out[2], out[3], retries ? " (retrying)" : "");
        memcpy(prev_in, in, 4);
        memcpy(prev_out, out, 4);
    }

    spc_cycle_acc += (double)CYCLES_PER_FRAME * SPC_CLOCK / CPU_HZ;
    int c = (int)spc_cycle_acc;
    spc_cycle_acc -= c;
    spc_run(c);
}

/* Adds the SNES audio, resampled to `rate`, onto n stereo GB samples. */
void sgb_sound_mix(int16_t *buf, size_t n, int rate)
{
    if (!enabled || !rate)
        return;
    bool muted = modaudio_sgb_muted();   /* a mod replaces the current SNES sound */
    double step = (double)DSP_RATE / rate;
    for (size_t i = 0; i < n; i++) {
        rs_pos += step;
        while (rs_pos >= 1.0) {
            rs_pos -= 1.0;
            rs_prev[0] = rs_cur[0];
            rs_prev[1] = rs_cur[1];
            int16_t s[2];
            if (dsp_take_samples(s, 1)) {
                rs_cur[0] = s[0];
                rs_cur[1] = s[1];
            }
        }
        for (int ch = 0; ch < 2; ch++) {
            double v = rs_prev[ch] + (rs_cur[ch] - rs_prev[ch]) * rs_pos;
            int m = buf[i * 2 + ch] + (muted ? 0 : (int)(v * 0.5));
            buf[i * 2 + ch] = m < -32768 ? -32768 : m > 32767 ? 32767 : m;
        }
    }
}

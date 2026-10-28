/* SNES audio processing unit (SPC700 + S-DSP), used for the Super Game
 * Boy's SNES-side sound. */
#ifndef SNES_APU_H
#define SNES_APU_H

#include <stdint.h>
#include <stddef.h>

#define SPC_CLOCK 1024000   /* SPC700 cycles per second */
#define DSP_RATE 32000      /* one DSP sample every 32 SPC cycles */

/* spc.c */
extern uint8_t spc_ram[0x10000];
void spc_reset(uint16_t pc);           /* registers as left by the IPL ROM before jumping */
void spc_set_pc(uint16_t pc);
void spc_run(int cycles);              /* runs the CPU, timers and DSP */
void spc_write_port(int port, uint8_t v);   /* SNES -> SPC ($2140-$2143) */
uint8_t spc_read_port(int port);            /* SPC -> SNES */

/* dsp.c */
void dsp_reset(void);
uint8_t dsp_read(uint8_t addr);
void dsp_write(uint8_t addr, uint8_t v);
void dsp_run_sample(void);                  /* produces one stereo sample into the output FIFO */
size_t dsp_take_samples(int16_t *out, size_t max_frames);

#endif

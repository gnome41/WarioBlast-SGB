#ifndef PAD_H
#define PAD_H

#include <stdbool.h>
#include <stdint.h>
#include <SDL.h>

typedef struct {
    uint8_t buttons;        /* Game Boy buttons, bit order as gb.buttons (all controllers) */
    uint8_t player[4];      /* the same per Super Game Boy player: the n-th connected
                               controller is player n+1 (5th and later: player 4) */
    bool fast_forward;      /* held */
    bool cycle_view;        /* pressed this frame */
    bool toggle_fullscreen; /* pressed this frame */
} PadInput;

void pad_init(const char *exe_dir);
void pad_event(const SDL_Event *e);
PadInput pad_poll(void);
uint8_t pad_cancel_opposite(uint8_t buttons);
int pad_count(void);
int pad_selftest(void);

void set_window_icon(SDL_Window *window);   /* icon.c */

#endif

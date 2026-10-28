/* Window icon (generated pixel art, see tools/make_icon.py). */
#include <SDL.h>
#include "icon_data.h"

void set_window_icon(SDL_Window *window)
{
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormatFrom((void *)icon_rgba, ICON_SIZE, ICON_SIZE, 32,
                                                        ICON_SIZE * 4, SDL_PIXELFORMAT_RGBA32);
    if (!s)
        return;
    SDL_SetWindowIcon(window, s);
    SDL_FreeSurface(s);
}

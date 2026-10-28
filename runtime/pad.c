/* Game controllers: any number, hot-plugged, polled once per frame.
 *
 *   D-pad / left stick  -> Game Boy D-pad
 *   A, Y                -> Game Boy A      (B, X with --swap-ab)
 *   B, X                -> Game Boy B      (A, Y with --swap-ab)
 *   Start / Back        -> Start / Select
 *   RB or RT (hold)     -> fast-forward
 *   LB                  -> cycle display view
 *   Guide               -> toggle fullscreen
 *
 * In Super Game Boy multiplayer games (MLT_REQ) the first controller is
 * player 1 (together with the keyboard), the second player 2, and so on.
 *
 * Extra mappings are read from gamecontrollerdb.txt (SDL format) in the
 * current directory or next to the executable, if present. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL.h>
#include "gb.h"
#include "pad.h"

#define MAX_PADS 8
#define STICK_DEADZONE 12000
#define TRIGGER_THRESHOLD 16000

static SDL_GameController *pads[MAX_PADS];
static bool prev_lb, prev_guide;

static void open_pad(int device_index)
{
    if (!SDL_IsGameController(device_index))
        return;
    SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(device_index);
    for (int i = 0; i < MAX_PADS; i++)
        if (pads[i] && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pads[i])) == id)
            return;   /* already open (SDL reports devices present at start twice) */
    for (int i = 0; i < MAX_PADS; i++) {
        if (pads[i])
            continue;
        pads[i] = SDL_GameControllerOpen(device_index);
        if (pads[i])
            fprintf(stderr, "controller connected: %s\n", SDL_GameControllerName(pads[i]));
        return;
    }
}

static void close_pad(SDL_JoystickID id)
{
    for (int i = 0; i < MAX_PADS; i++)
        if (pads[i] && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pads[i])) == id) {
            fprintf(stderr, "controller disconnected: %s\n", SDL_GameControllerName(pads[i]));
            SDL_GameControllerClose(pads[i]);
            pads[i] = NULL;
        }
}

void pad_init(const char *exe_dir)
{
    const char *db = "gamecontrollerdb.txt";
    int n = SDL_GameControllerAddMappingsFromFile(db);
    if (n < 0 && exe_dir) {
        char path[1024];
        snprintf(path, sizeof path, "%s%s", exe_dir, db);
        n = SDL_GameControllerAddMappingsFromFile(path);
    }
    if (n > 0)
        fprintf(stderr, "loaded %d controller mappings from %s\n", n, db);
    for (int i = 0; i < SDL_NumJoysticks(); i++)
        open_pad(i);
}

void pad_event(const SDL_Event *e)
{
    if (e->type == SDL_CONTROLLERDEVICEADDED)
        open_pad(e->cdevice.which);
    else if (e->type == SDL_CONTROLLERDEVICEREMOVED)
        close_pad(e->cdevice.which);
}

int pad_count(void)
{
    int n = 0;
    for (int i = 0; i < MAX_PADS; i++)
        n += pads[i] != NULL;
    return n;
}

uint8_t pad_cancel_opposite(uint8_t b)
{
    if ((b & 0x03) == 0x03) b &= ~0x03;
    if ((b & 0x0C) == 0x0C) b &= ~0x0C;
    return b;
}

PadInput pad_poll(void)
{
    PadInput in = { 0 };
    bool lb = false, guide = false;
    uint8_t a = opt.swap_ab ? 0x20 : 0x10, b = opt.swap_ab ? 0x10 : 0x20;
    int np = 0;
    for (int i = 0; i < MAX_PADS; i++) {
        SDL_GameController *p = pads[i];
        if (!p)
            continue;
        uint8_t pb = 0;
#define BTN(x) SDL_GameControllerGetButton(p, SDL_CONTROLLER_BUTTON_##x)
        if (BTN(DPAD_RIGHT)) pb |= 0x01;
        if (BTN(DPAD_LEFT)) pb |= 0x02;
        if (BTN(DPAD_UP)) pb |= 0x04;
        if (BTN(DPAD_DOWN)) pb |= 0x08;
        if (BTN(A) || BTN(Y)) pb |= a;
        if (BTN(B) || BTN(X)) pb |= b;
        if (BTN(BACK)) pb |= 0x40;
        if (BTN(START)) pb |= 0x80;
        if (BTN(RIGHTSHOULDER)) in.fast_forward = true;
        lb |= BTN(LEFTSHOULDER);
        guide |= BTN(GUIDE);
#undef BTN
        int x = SDL_GameControllerGetAxis(p, SDL_CONTROLLER_AXIS_LEFTX);
        int y = SDL_GameControllerGetAxis(p, SDL_CONTROLLER_AXIS_LEFTY);
        if (x > STICK_DEADZONE) pb |= 0x01;
        if (x < -STICK_DEADZONE) pb |= 0x02;
        if (y < -STICK_DEADZONE) pb |= 0x04;
        if (y > STICK_DEADZONE) pb |= 0x08;
        if (SDL_GameControllerGetAxis(p, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > TRIGGER_THRESHOLD)
            in.fast_forward = true;
        in.buttons |= pb;
        in.player[np < 3 ? np : 3] |= pb;
        np++;
    }
    /* opposite directions cancel, as on the real D-pad */
    in.buttons = pad_cancel_opposite(in.buttons);
    for (int i = 0; i < 4; i++)
        in.player[i] = pad_cancel_opposite(in.player[i]);
    in.cycle_view = lb && !prev_lb;
    in.toggle_fullscreen = guide && !prev_guide;
    prev_lb = lb;
    prev_guide = guide;
    return in;
}

/* WB_PAD_SELFTEST=1: attaches SDL virtual controllers and checks
 * hot-plug, every mapping, the stick and the hotkeys.  Returns the exit
 * code (0 = pass). */
int pad_selftest(void)
{
    if (SDL_Init(SDL_INIT_GAMECONTROLLER) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    pad_init(NULL);
    int fails = 0;
#define CHECK(cond, what) do { bool ok_ = (cond); printf("%s %s\n", ok_ ? "PASS" : "FAIL", what); fails += !ok_; } while (0)

    SDL_VirtualJoystickDesc desc;
    SDL_zero(desc);
    desc.version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
    desc.type = SDL_JOYSTICK_TYPE_GAMECONTROLLER;
    desc.naxes = SDL_CONTROLLER_AXIS_MAX;
    desc.nbuttons = SDL_CONTROLLER_BUTTON_MAX;
    desc.name = "WarioBlast virtual pad";
    int dev[2];
    SDL_Joystick *js[2];
    for (int k = 0; k < 2; k++) {
        dev[k] = SDL_JoystickAttachVirtualEx(&desc);
        js[k] = dev[k] >= 0 ? SDL_JoystickOpen(dev[k]) : NULL;
    }
    if (!js[0] || !js[1]) {
        printf("FAIL cannot attach virtual controllers: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Event e;
    while (SDL_PollEvent(&e))
        pad_event(&e);
    CHECK(pad_count() == 2, "two controllers hot-plugged");

    struct { int button; uint8_t expect; const char *name; } map[] = {
        { SDL_CONTROLLER_BUTTON_DPAD_RIGHT, 0x01, "dpad right" },
        { SDL_CONTROLLER_BUTTON_DPAD_LEFT, 0x02, "dpad left" },
        { SDL_CONTROLLER_BUTTON_DPAD_UP, 0x04, "dpad up" },
        { SDL_CONTROLLER_BUTTON_DPAD_DOWN, 0x08, "dpad down" },
        { SDL_CONTROLLER_BUTTON_A, 0x10, "A -> GB A" },
        { SDL_CONTROLLER_BUTTON_Y, 0x10, "Y -> GB A" },
        { SDL_CONTROLLER_BUTTON_B, 0x20, "B -> GB B" },
        { SDL_CONTROLLER_BUTTON_X, 0x20, "X -> GB B" },
        { SDL_CONTROLLER_BUTTON_BACK, 0x40, "Back -> Select" },
        { SDL_CONTROLLER_BUTTON_START, 0x80, "Start -> Start" },
    };
    for (size_t i = 0; i < sizeof map / sizeof *map; i++) {
        SDL_JoystickSetVirtualButton(js[1], map[i].button, 1);
        SDL_GameControllerUpdate();
        PadInput in = pad_poll();
        SDL_JoystickSetVirtualButton(js[1], map[i].button, 0);
        SDL_GameControllerUpdate();
        char what[64];
        snprintf(what, sizeof what, "%s (second controller)", map[i].name);
        CHECK(in.buttons == map[i].expect, what);
    }
    opt.swap_ab = true;
    SDL_JoystickSetVirtualButton(js[0], SDL_CONTROLLER_BUTTON_A, 1);
    SDL_GameControllerUpdate();
    CHECK(pad_poll().buttons == 0x20, "--swap-ab: A -> GB B");
    SDL_JoystickSetVirtualButton(js[0], SDL_CONTROLLER_BUTTON_A, 0);
    opt.swap_ab = false;

    SDL_JoystickSetVirtualAxis(js[0], SDL_CONTROLLER_AXIS_LEFTX, 30000);
    SDL_JoystickSetVirtualAxis(js[0], SDL_CONTROLLER_AXIS_LEFTY, -30000);
    SDL_GameControllerUpdate();
    CHECK(pad_poll().buttons == 0x05, "left stick up-right -> dpad up+right");
    SDL_JoystickSetVirtualAxis(js[0], SDL_CONTROLLER_AXIS_LEFTX, 5000);
    SDL_JoystickSetVirtualAxis(js[0], SDL_CONTROLLER_AXIS_LEFTY, -5000);
    SDL_GameControllerUpdate();
    CHECK(pad_poll().buttons == 0, "small stick movement ignored (deadzone)");
    SDL_JoystickSetVirtualAxis(js[0], SDL_CONTROLLER_AXIS_LEFTX, 0);
    SDL_JoystickSetVirtualAxis(js[0], SDL_CONTROLLER_AXIS_LEFTY, 0);

    SDL_JoystickSetVirtualButton(js[0], SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, 1);
    SDL_GameControllerUpdate();
    CHECK(pad_poll().fast_forward, "RB -> fast-forward");
    SDL_JoystickSetVirtualButton(js[0], SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, 0);
    SDL_JoystickSetVirtualAxis(js[0], SDL_CONTROLLER_AXIS_TRIGGERRIGHT, 32767);
    SDL_GameControllerUpdate();
    CHECK(pad_poll().fast_forward, "RT -> fast-forward");
    SDL_JoystickSetVirtualAxis(js[0], SDL_CONTROLLER_AXIS_TRIGGERRIGHT, 0);

    SDL_JoystickSetVirtualButton(js[0], SDL_CONTROLLER_BUTTON_LEFTSHOULDER, 1);
    SDL_GameControllerUpdate();
    bool first = pad_poll().cycle_view, again = pad_poll().cycle_view;
    CHECK(first && !again, "LB -> cycle view once per press");
    SDL_JoystickSetVirtualButton(js[0], SDL_CONTROLLER_BUTTON_LEFTSHOULDER, 0);
    SDL_JoystickSetVirtualButton(js[0], SDL_CONTROLLER_BUTTON_GUIDE, 1);
    SDL_GameControllerUpdate();
    CHECK(pad_poll().toggle_fullscreen, "Guide -> toggle fullscreen");
    SDL_JoystickSetVirtualButton(js[0], SDL_CONTROLLER_BUTTON_GUIDE, 0);

    /* unplug while a button is held: nothing may stay pressed */
    SDL_JoystickSetVirtualButton(js[1], SDL_CONTROLLER_BUTTON_START, 1);
    SDL_GameControllerUpdate();
    SDL_JoystickClose(js[1]);
    SDL_JoystickDetachVirtual(dev[1]);
    while (SDL_PollEvent(&e))
        pad_event(&e);
    CHECK(pad_count() == 1, "unplugged controller closed");
    SDL_GameControllerUpdate();
    CHECK(pad_poll().buttons == 0, "no stuck buttons after unplug");
#undef CHECK
    printf("%s: %d failure(s)\n", fails ? "FAILED" : "ALL PASSED", fails);
    SDL_Quit();
    return fails ? 1 : 0;
}

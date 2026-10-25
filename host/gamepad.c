/* MHFU-Android -- controllers through SDL3. See gamepad.h.
 *
 * SDL is initialised, polled and read on one thread of its own, as it wants;
 * the game thread only reads the last state from three words. */
#include "gamepad.h"
#include "input.h"      /* PSP_CTRL_* */

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>

static SDL_AtomicInt g_buttons;
static SDL_AtomicInt g_stick;        /* x | y << 8, or -1 while the stick is centred */

/* Past this, the right stick presses the D-pad. */
#define RIGHT_STICK_PRESS 16000
/* Within this the left stick is centred, and the keyboard's stick is used. */
#define LEFT_STICK_DEAD   6000

static int held(SDL_Gamepad *pad, SDL_GamepadButton b) { return SDL_GetGamepadButton(pad, b) ? 1 : 0; }

static uint32_t read_buttons(SDL_Gamepad *pad) {
    uint32_t b = 0;
    if (held(pad, SDL_GAMEPAD_BUTTON_SOUTH))          b |= PSP_CTRL_CROSS;
    if (held(pad, SDL_GAMEPAD_BUTTON_EAST))           b |= PSP_CTRL_CIRCLE;
    if (held(pad, SDL_GAMEPAD_BUTTON_WEST))           b |= PSP_CTRL_SQUARE;
    if (held(pad, SDL_GAMEPAD_BUTTON_NORTH))          b |= PSP_CTRL_TRIANGLE;
    if (held(pad, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER))  b |= PSP_CTRL_LTRIGGER;
    if (held(pad, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER)) b |= PSP_CTRL_RTRIGGER;
    if (held(pad, SDL_GAMEPAD_BUTTON_START))          b |= PSP_CTRL_START;
    if (held(pad, SDL_GAMEPAD_BUTTON_BACK))           b |= PSP_CTRL_SELECT;
    if (held(pad, SDL_GAMEPAD_BUTTON_DPAD_UP))        b |= PSP_CTRL_UP;
    if (held(pad, SDL_GAMEPAD_BUTTON_DPAD_DOWN))      b |= PSP_CTRL_DOWN;
    if (held(pad, SDL_GAMEPAD_BUTTON_DPAD_LEFT))      b |= PSP_CTRL_LEFT;
    if (held(pad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT))     b |= PSP_CTRL_RIGHT;
    if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16000)  b |= PSP_CTRL_LTRIGGER;
    if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16000) b |= PSP_CTRL_RTRIGGER;

    const int rx = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTX);
    const int ry = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTY);
    if (rx < -RIGHT_STICK_PRESS) b |= PSP_CTRL_LEFT;
    if (rx >  RIGHT_STICK_PRESS) b |= PSP_CTRL_RIGHT;
    if (ry < -RIGHT_STICK_PRESS) b |= PSP_CTRL_UP;
    if (ry >  RIGHT_STICK_PRESS) b |= PSP_CTRL_DOWN;
    return b;
}

static int read_stick(SDL_Gamepad *pad) {
    const int x = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX);
    const int y = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY);
    if (x > -LEFT_STICK_DEAD && x < LEFT_STICK_DEAD && y > -LEFT_STICK_DEAD && y < LEFT_STICK_DEAD) return -1;
    return ((x + 32768) >> 8) | ((y + 32768) >> 8) << 8;
}

/* The controller in use, chosen among those connected. */
static SDL_Gamepad *g_pad;
static int g_external;               /* the SDL window's loop feeds the events */

void mhfu_gamepad_event(const SDL_Event *e) {
    /* MHFU_GAMEPAD_GUID (the launcher's Controller page): only that device;
     * otherwise the first one connected. */
    static const char *want;
    static int got_want;
    if (!got_want) { want = getenv("MHFU_GAMEPAD_GUID"); got_want = 1; }
    if (getenv("MHFU_NO_GAMEPAD")) return;
    if (e->type == SDL_EVENT_GAMEPAD_ADDED && !g_pad) {
        if (want && *want) {
            char guid[64];
            SDL_GUIDToString(SDL_GetGamepadGUIDForID(e->gdevice.which), guid, sizeof guid);
            if (SDL_strcasecmp(guid, want) != 0) return;
        }
        g_pad = SDL_OpenGamepad(e->gdevice.which);
        if (g_pad) { printf("gamepad: %s connected\n", SDL_GetGamepadName(g_pad)); fflush(stdout); }
    } else if (e->type == SDL_EVENT_GAMEPAD_REMOVED && g_pad && e->gdevice.which == SDL_GetGamepadID(g_pad)) {
        printf("gamepad: %s removed\n", SDL_GetGamepadName(g_pad));
        fflush(stdout);
        SDL_CloseGamepad(g_pad);
        g_pad = NULL;
    }
}

void mhfu_gamepad_update(void) {
    SDL_SetAtomicInt(&g_buttons, g_pad ? (int)read_buttons(g_pad) : 0);
    SDL_SetAtomicInt(&g_stick, g_pad ? read_stick(g_pad) : -1);
}

void mhfu_gamepad_external(void) { g_external = 1; }
int mhfu_gamepad_connected(void) { return g_pad != NULL; }

static int SDLCALL pad_thread(void *arg) {
    (void)arg;
    /* The window is not SDL's, so to SDL the program is always in the
     * background: without this it would ignore the controller. */
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (!SDL_Init(SDL_INIT_GAMEPAD)) {
        fprintf(stderr, "gamepad: SDL: %s\n", SDL_GetError());
        return 0;
    }
    for (;;) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) mhfu_gamepad_event(&e);
        mhfu_gamepad_update();
        SDL_Delay(4);
    }
    return 0;
}

int mhfu_gamepad_start(void) {
    SDL_SetAtomicInt(&g_stick, -1);
    /* MHFU_NO_GAMEPAD=1: scripted runs, which a controller lying on the desk
     * should not steer. */
    if (getenv("MHFU_NO_GAMEPAD") || g_external) return 0;
    SDL_Thread *t = SDL_CreateThread(pad_thread, "gamepad", NULL);
    if (!t) return -1;
    SDL_DetachThread(t);
    return 0;
}

uint32_t mhfu_gamepad_read(uint8_t *ax, uint8_t *ay) {
    const int s = SDL_GetAtomicInt(&g_stick);
    if (s >= 0) {
        *ax = (uint8_t)(s & 0xFF);
        *ay = (uint8_t)(s >> 8);
    }
    return (uint32_t)SDL_GetAtomicInt(&g_buttons);
}

/* MHFU-Android -- controllers through SDL3. See gamepad.h.
 *
 * SDL is initialised, polled and read on one thread of its own, as it wants;
 * the game thread only reads the last state from three words. */
#include "gamepad.h"
#include "input.h"      /* PSP_CTRL_* */

#include <SDL3/SDL.h>
#include <stdio.h>

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

static int SDLCALL pad_thread(void *arg) {
    (void)arg;
    /* The window is not SDL's, so to SDL the program is always in the
     * background: without this it would ignore the controller. */
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (!SDL_Init(SDL_INIT_GAMEPAD)) {
        fprintf(stderr, "gamepad: SDL: %s\n", SDL_GetError());
        return 0;
    }
    SDL_Gamepad *pad = NULL;
    for (;;) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_GAMEPAD_ADDED && !pad) {
                pad = SDL_OpenGamepad(e.gdevice.which);
                if (pad) { printf("gamepad: %s connected\n", SDL_GetGamepadName(pad)); fflush(stdout); }
            } else if (e.type == SDL_EVENT_GAMEPAD_REMOVED && pad && e.gdevice.which == SDL_GetGamepadID(pad)) {
                printf("gamepad: %s removed\n", SDL_GetGamepadName(pad));
                fflush(stdout);
                SDL_CloseGamepad(pad);
                pad = NULL;
            }
        }
        SDL_SetAtomicInt(&g_buttons, pad ? (int)read_buttons(pad) : 0);
        SDL_SetAtomicInt(&g_stick, pad ? read_stick(pad) : -1);
        SDL_Delay(4);
    }
    return 0;
}

int mhfu_gamepad_start(void) {
    SDL_SetAtomicInt(&g_stick, -1);
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

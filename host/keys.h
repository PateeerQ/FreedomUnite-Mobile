/* MHFU-Android -- keyboard bindings for the window (input.ini).
 *
 * One line per PSP control, `action = key[, key...]`, `#` starts a comment:
 *
 *     cross = k, space
 *     stick_up = w
 *
 * Actions: cross circle square triangle l r start select up down left right
 * stick_up stick_down stick_left stick_right. Key names: a-z, 0-9, space,
 * enter, backspace, tab, escape, capslock, lshift rshift lctrl rctrl lalt
 * ralt, up down left right, insert delete home end pageup pagedown, f1-f12,
 * comma period slash semicolon quote minus equals lbracket rbracket backslash
 * grave, kp0-kp9 kp_plus kp_minus kp_multiply kp_divide kp_period. The
 * launcher (launcher/keys.cpp) writes the same names. An action the file does
 * not mention keeps its default; `action =` with nothing unbinds it. F3 (FPS)
 * and F11 (fullscreen) belong to the window. */
#ifndef MHFU_KEYS_H
#define MHFU_KEYS_H

#include <stdint.h>

/* Defaults, then `path` on top (NULL or missing: defaults only). */
void mhfu_keys_load(const char *path);

/* The Windows virtual-key code input.ini's `name` stands for (the SDL window
 * maps its keys to these too); -1 for an unknown name. */
int mhfu_keys_vk(const char *name);

/* For a Windows virtual-key code (left/right modifiers already told apart):
 * the PSP buttons it holds, and the stick directions (bit 0 left, 1 right,
 * 2 up, 3 down). */
uint32_t mhfu_keys_buttons(unsigned vk);
int      mhfu_keys_stick(unsigned vk);

#endif

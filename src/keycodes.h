/* SPDX-License-Identifier: MIT
 * Portable Linux evdev keycode definitions and name <-> code conversion.
 *
 * Uses <linux/input-event-codes.h> on Linux, and provides matching Linux ABI
 * constants when running host unit tests on non-Linux systems.
 */
#ifndef GLOBETROT_KEYCODES_H
#define GLOBETROT_KEYCODES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __linux__
#include <linux/input-event-codes.h>
#else
#define EV_SYN                  0x00
#define EV_KEY                  0x01
#define EV_REL                  0x02
#define EV_ABS                  0x03
#define EV_MSC                  0x04
#define EV_LED                  0x11
#define EV_REP                  0x14

#define SYN_REPORT              0
#define REL_HWHEEL              0x06
#define REL_WHEEL               0x08
#define REL_WHEEL_HI_RES        0x0b
#define REL_HWHEEL_HI_RES       0x0c

#define KEY_RESERVED            0
#define KEY_ESC                 1
#define KEY_1                   2
#define KEY_2                   3
#define KEY_3                   4
#define KEY_4                   5
#define KEY_5                   6
#define KEY_6                   7
#define KEY_7                   8
#define KEY_8                   9
#define KEY_9                   10
#define KEY_0                   11
#define KEY_MINUS               12
#define KEY_EQUAL               13
#define KEY_BACKSPACE           14
#define KEY_TAB                 15
#define KEY_Q                   16
#define KEY_W                   17
#define KEY_E                   18
#define KEY_R                   19
#define KEY_T                   20
#define KEY_Y                   21
#define KEY_U                   22
#define KEY_I                   23
#define KEY_O                   24
#define KEY_P                   25
#define KEY_LEFTBRACE           26
#define KEY_RIGHTBRACE          27
#define KEY_ENTER               28
#define KEY_LEFTCTRL            29
#define KEY_A                   30
#define KEY_S                   31
#define KEY_D                   32
#define KEY_F                   33
#define KEY_G                   34
#define KEY_H                   35
#define KEY_J                   36
#define KEY_K                   37
#define KEY_L                   38
#define KEY_SEMICOLON           39
#define KEY_APOSTROPHE          40
#define KEY_GRAVE               41
#define KEY_LEFTSHIFT           42
#define KEY_BACKSLASH           43
#define KEY_Z                   44
#define KEY_X                   45
#define KEY_C                   46
#define KEY_V                   47
#define KEY_B                   48
#define KEY_N                   49
#define KEY_M                   50
#define KEY_COMMA               51
#define KEY_DOT                 52
#define KEY_SLASH               53
#define KEY_RIGHTSHIFT          54
#define KEY_KPASTERISK          55
#define KEY_LEFTALT             56
#define KEY_SPACE               57
#define KEY_CAPSLOCK            58
#define KEY_F1                  59
#define KEY_F2                  60
#define KEY_F3                  61
#define KEY_F4                  62
#define KEY_F5                  63
#define KEY_F6                  64
#define KEY_F7                  65
#define KEY_F8                  66
#define KEY_F9                  67
#define KEY_F10                 68
#define KEY_NUMLOCK             69
#define KEY_SCROLLLOCK          70
#define KEY_F11                 87
#define KEY_F12                 88
#define KEY_KPENTER             96
#define KEY_RIGHTCTRL           97
#define KEY_SYSRQ               99
#define KEY_RIGHTALT            100
#define KEY_HOME                102
#define KEY_UP                  103
#define KEY_PAGEUP              104
#define KEY_LEFT                105
#define KEY_RIGHT               106
#define KEY_END                 107
#define KEY_DOWN                108
#define KEY_PAGEDOWN            109
#define KEY_INSERT              110
#define KEY_DELETE              111
#define KEY_MUTE                113
#define KEY_VOLUMEDOWN          114
#define KEY_VOLUMEUP            115
#define KEY_POWER               116
#define KEY_SCALE               120
#define KEY_LEFTMETA            125
#define KEY_RIGHTMETA           126
#define KEY_COMPOSE             127
#define KEY_PROG1               148
#define KEY_PROG2               149
#define KEY_NEXTSONG            163
#define KEY_PLAYPAUSE           164
#define KEY_PREVIOUSSONG        165
#define KEY_ISO                 170
#define KEY_F13                 183
#define KEY_F14                 184
#define KEY_F15                 185
#define KEY_F16                 186
#define KEY_F17                 187
#define KEY_F18                 188
#define KEY_F19                 189
#define KEY_F20                 190
#define KEY_F21                 191
#define KEY_F22                 192
#define KEY_F23                 193
#define KEY_F24                 194
#define KEY_ALL_APPLICATIONS    204
#define KEY_DASHBOARD           KEY_ALL_APPLICATIONS
#define KEY_SEARCH              217
#define KEY_BRIGHTNESSDOWN      224
#define KEY_BRIGHTNESSUP        225
#define KEY_KBDILLUMTOGGLE      228
#define KEY_KBDILLUMDOWN        229
#define KEY_KBDILLUMUP          230
#define BTN_LEFT                0x110
#define BTN_RIGHT               0x111
#define BTN_MIDDLE              0x112
#define BTN_SIDE                0x113
#define BTN_EXTRA               0x114
#define KEY_FN                  0x1d0
#define KEY_VOICECOMMAND        0x246
#define KEY_EMOJI_PICKER        0x249
#define KEY_DO_NOT_DISTURB      0x24f
#define KEY_MACRO1              0x290
#define KEY_MAX                 0x2ff
#endif /* __linux__ */

#ifndef KEY_ALL_APPLICATIONS
#define KEY_ALL_APPLICATIONS    204
#endif
#ifndef KEY_DASHBOARD
#define KEY_DASHBOARD           KEY_ALL_APPLICATIONS
#endif
#ifndef KEY_VOICECOMMAND
#define KEY_VOICECOMMAND        0x246
#endif
#ifndef KEY_EMOJI_PICKER
#define KEY_EMOJI_PICKER        0x249
#endif
#ifndef KEY_DO_NOT_DISTURB
#define KEY_DO_NOT_DISTURB      0x24f
#endif
#ifndef KEY_MACRO1
#define KEY_MACRO1              0x290
#endif
#ifndef KEY_MICMUTE
#define KEY_MICMUTE             248
#endif
#ifndef KEY_EJECTCD
#define KEY_EJECTCD             161
#endif
#ifndef KEY_INSERT
#define KEY_INSERT              110
#endif
#ifndef REL_WHEEL_HI_RES
#define REL_WHEEL_HI_RES        0x0b
#endif
#ifndef REL_HWHEEL_HI_RES
#define REL_HWHEEL_HI_RES       0x0c
#endif

#define GT_MAX_KEYCODE      768
#define GT_MAX_CHORD_KEYS   4

/* Order-independent modifier bitmask (L/R normalized). */
typedef enum {
    MOD_NONE  = 0,
    MOD_SHIFT = 1u << 0,
    MOD_CTRL  = 1u << 1,
    MOD_ALT   = 1u << 2,
    MOD_META  = 1u << 3,
} gt_mod_mask_t;

typedef struct {
    uint16_t keys[GT_MAX_CHORD_KEYS];
    uint8_t  count;
} gt_key_chord_t;

bool gt_keycode_from_name(const char *name, uint16_t *out_code);
const char *gt_keycode_to_name(uint16_t code);
bool gt_chord_parse(const char *spec, gt_key_chord_t *out_chord);
bool gt_combo_modifier_parse(const char *spec, gt_key_chord_t *out_chord);

uint8_t gt_keycode_mod_bit(uint16_t code);
bool gt_keycode_is_pointer_btn(uint16_t code);
bool gt_rel_is_scroll_axis(uint16_t axis);

/* Returns true if code belongs to the kernel Fn-layer output set
 * (F1..F12, media/brightness/system keys, Home/End/PgUp/PgDn/Delete/Insert).
 * hid-apple translates Fn+Backspace/Return/Up/Down/Left/Right into
 * Delete/Insert/PageUp/PageDown/Home/End before userspace sees them. */
bool gt_keycode_is_fn_layer_key(uint16_t code);

#endif /* GLOBETROT_KEYCODES_H */

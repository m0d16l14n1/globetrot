/* SPDX-License-Identifier: MIT */
#include "keycodes.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *name;
    uint16_t    code;
} key_entry_t;

static const key_entry_t k_key_table[] = {
    {"esc", KEY_ESC}, {"escape", KEY_ESC},
    {"1", KEY_1}, {"2", KEY_2}, {"3", KEY_3}, {"4", KEY_4}, {"5", KEY_5},
    {"6", KEY_6}, {"7", KEY_7}, {"8", KEY_8}, {"9", KEY_9}, {"0", KEY_0},
    {"minus", KEY_MINUS}, {"equal", KEY_EQUAL},
    {"backspace", KEY_BACKSPACE}, {"tab", KEY_TAB},
    {"q", KEY_Q}, {"w", KEY_W}, {"e", KEY_E}, {"r", KEY_R}, {"t", KEY_T},
    {"y", KEY_Y}, {"u", KEY_U}, {"i", KEY_I}, {"o", KEY_O}, {"p", KEY_P},
    {"leftbrace", KEY_LEFTBRACE}, {"rightbrace", KEY_RIGHTBRACE},
    {"enter", KEY_ENTER}, {"return", KEY_ENTER},
    {"leftctrl", KEY_LEFTCTRL}, {"lctrl", KEY_LEFTCTRL}, {"ctrl", KEY_LEFTCTRL},
    {"a", KEY_A}, {"s", KEY_S}, {"d", KEY_D}, {"f", KEY_F}, {"g", KEY_G},
    {"h", KEY_H}, {"j", KEY_J}, {"k", KEY_K}, {"l", KEY_L},
    {"semicolon", KEY_SEMICOLON}, {"apostrophe", KEY_APOSTROPHE},
    {"grave", KEY_GRAVE},
    {"leftshift", KEY_LEFTSHIFT}, {"lshift", KEY_LEFTSHIFT}, {"shift", KEY_LEFTSHIFT},
    {"backslash", KEY_BACKSLASH},
    {"z", KEY_Z}, {"x", KEY_X}, {"c", KEY_C}, {"v", KEY_V}, {"b", KEY_B},
    {"n", KEY_N}, {"m", KEY_M},
    {"comma", KEY_COMMA}, {"dot", KEY_DOT}, {"period", KEY_DOT}, {"slash", KEY_SLASH},
    {"rightshift", KEY_RIGHTSHIFT}, {"rshift", KEY_RIGHTSHIFT},
    {"leftalt", KEY_LEFTALT}, {"lalt", KEY_LEFTALT}, {"alt", KEY_LEFTALT},
    {"space", KEY_SPACE}, {"capslock", KEY_CAPSLOCK},
    {"f1", KEY_F1}, {"f2", KEY_F2}, {"f3", KEY_F3}, {"f4", KEY_F4},
    {"f5", KEY_F5}, {"f6", KEY_F6}, {"f7", KEY_F7}, {"f8", KEY_F8},
    {"f9", KEY_F9}, {"f10", KEY_F10}, {"f11", KEY_F11}, {"f12", KEY_F12},
    {"f13", KEY_F13}, {"f14", KEY_F14}, {"f15", KEY_F15}, {"f16", KEY_F16},
    {"f17", KEY_F17}, {"f18", KEY_F18}, {"f19", KEY_F19}, {"f20", KEY_F20},
    {"f21", KEY_F21}, {"f22", KEY_F22}, {"f23", KEY_F23}, {"f24", KEY_F24},
    {"kpenter", KEY_KPENTER},
    {"rightctrl", KEY_RIGHTCTRL}, {"rctrl", KEY_RIGHTCTRL},
    {"rightalt", KEY_RIGHTALT}, {"ralt", KEY_RIGHTALT}, {"altgr", KEY_RIGHTALT},
    {"home", KEY_HOME}, {"up", KEY_UP}, {"pageup", KEY_PAGEUP},
    {"left", KEY_LEFT}, {"right", KEY_RIGHT},
    {"end", KEY_END}, {"down", KEY_DOWN}, {"pagedown", KEY_PAGEDOWN},
    {"insert", KEY_INSERT}, {"delete", KEY_DELETE},
    {"mute", KEY_MUTE}, {"volumedown", KEY_VOLUMEDOWN}, {"volumeup", KEY_VOLUMEUP},
    {"scale", KEY_SCALE},
    {"leftmeta", KEY_LEFTMETA}, {"lmeta", KEY_LEFTMETA}, {"super", KEY_LEFTMETA},
    {"meta", KEY_LEFTMETA},
    {"rightmeta", KEY_RIGHTMETA}, {"rmeta", KEY_RIGHTMETA}, {"rsuper", KEY_RIGHTMETA},
    {"compose", KEY_COMPOSE}, {"menu", KEY_COMPOSE},
    {"prog1", KEY_PROG1}, {"prog2", KEY_PROG2},
    {"nextsong", KEY_NEXTSONG}, {"playpause", KEY_PLAYPAUSE}, {"previoussong", KEY_PREVIOUSSONG},
    {"iso", KEY_ISO},
    {"dashboard", KEY_DASHBOARD}, {"search", KEY_SEARCH},
    {"brightnessdown", KEY_BRIGHTNESSDOWN}, {"brightnessup", KEY_BRIGHTNESSUP},
    {"kbdillumtoggle", KEY_KBDILLUMTOGGLE},
    {"kbdillumdown", KEY_KBDILLUMDOWN}, {"kbdillumup", KEY_KBDILLUMUP},
    {"fn", KEY_FN}, {"globe", KEY_FN},
    {"voicecommand", KEY_VOICECOMMAND},
    {"emoji_picker", KEY_EMOJI_PICKER}, {"emoji", KEY_EMOJI_PICKER},
    {"do_not_disturb", KEY_DO_NOT_DISTURB},
    {"macro1", KEY_MACRO1},
    {"micmute", KEY_MICMUTE}, {"ejectcd", KEY_EJECTCD},
};

static bool streq_ci(const char *a, const char *b) {
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
            return false;
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

bool gt_keycode_from_name(const char *name, uint16_t *out_code) {
    if (!name || !*name || !out_code)
        return false;

    while (*name && isspace((unsigned char)*name))
        name++;

    char buf[64];
    size_t len = 0;
    while (name[len] && !isspace((unsigned char)name[len]) && len < sizeof(buf) - 1) {
        buf[len] = (char)tolower((unsigned char)name[len]);
        len++;
    }
    buf[len] = '\0';
    if (len == 0)
        return false;

    const char *p = buf;
    if (strncmp(p, "key_", 4) == 0)
        p += 4;

    if (isdigit((unsigned char)p[0])) {
        char *endp = NULL;
        unsigned long val = strtoul(p, &endp, 0);
        if (endp && *endp == '\0' && val > 0 && val < GT_MAX_KEYCODE) {
            *out_code = (uint16_t)val;
            return true;
        }
    }

    for (size_t i = 0; i < sizeof(k_key_table) / sizeof(k_key_table[0]); i++) {
        if (streq_ci(p, k_key_table[i].name)) {
            *out_code = k_key_table[i].code;
            return true;
        }
    }
    return false;
}

const char *gt_keycode_to_name(uint16_t code) {
    for (size_t i = 0; i < sizeof(k_key_table) / sizeof(k_key_table[0]); i++) {
        if (k_key_table[i].code == code)
            return k_key_table[i].name;
    }
    return "unknown";
}

bool gt_chord_parse(const char *spec, gt_key_chord_t *out_chord) {
    if (!spec || !out_chord)
        return false;
    memset(out_chord, 0, sizeof(*out_chord));

    const char *p = spec;
    while (*p && isspace((unsigned char)*p))
        p++;
    if (*p == '\0' || streq_ci(p, "none"))
        return true;

    while (*p) {
        if (out_chord->count >= GT_MAX_CHORD_KEYS)
            return false;
        const char *plus = strchr(p, '+');
        char tok[64];
        size_t tlen = plus ? (size_t)(plus - p) : strlen(p);
        if (tlen == 0 || tlen >= sizeof(tok))
            return false;
        memcpy(tok, p, tlen);
        tok[tlen] = '\0';

        while (tlen > 0 && isspace((unsigned char)tok[tlen - 1]))
            tok[--tlen] = '\0';

        uint16_t code = 0;
        if (!gt_keycode_from_name(tok, &code))
            return false;
        out_chord->keys[out_chord->count++] = code;

        if (!plus)
            break;
        p = plus + 1;
    }
    return out_chord->count > 0;
}

bool gt_combo_modifier_parse(const char *spec, gt_key_chord_t *out_chord) {
    if (!spec || !out_chord)
        return false;
    while (*spec && isspace((unsigned char)*spec))
        spec++;
    if (*spec == '\0' || streq_ci(spec, "none") || streq_ci(spec, "off")) {
        memset(out_chord, 0, sizeof(*out_chord));
        return true;
    }
    /* Stock xkeyboard-config maps F21..F23 to XF86TouchpadToggle/On/Off, which
     * desktops act on, so the Hyper preset uses F18 (XF86Launch9, unbound by
     * default) and expects xkb/globetrot-hyper.txt to turn it into Hyper. */
    if (streq_ci(spec, "hyper") || streq_ci(spec, "mod3"))
        return gt_chord_parse("f18", out_chord);
    if (streq_ci(spec, "hyper_chord"))
        return gt_chord_parse("leftctrl+leftalt+leftmeta+leftshift", out_chord);
    if (streq_ci(spec, "super_alt") || streq_ci(spec, "meta_alt"))
        return gt_chord_parse("leftmeta+leftalt", out_chord);
    if (streq_ci(spec, "ctrl_alt_super"))
        return gt_chord_parse("leftctrl+leftalt+leftmeta", out_chord);
    if (streq_ci(spec, "rightalt") || streq_ci(spec, "altgr"))
        return gt_chord_parse("rightalt", out_chord);
    return gt_chord_parse(spec, out_chord);
}

uint8_t gt_keycode_mod_bit(uint16_t code) {
    switch (code) {
    case KEY_LEFTSHIFT:
    case KEY_RIGHTSHIFT:
        return MOD_SHIFT;
    case KEY_LEFTCTRL:
    case KEY_RIGHTCTRL:
        return MOD_CTRL;
    case KEY_LEFTALT:
    case KEY_RIGHTALT:
        return MOD_ALT;
    case KEY_LEFTMETA:
    case KEY_RIGHTMETA:
        return MOD_META;
    default:
        return MOD_NONE;
    }
}

bool gt_keycode_is_pointer_btn(uint16_t code) {
    return code >= BTN_LEFT && code <= BTN_EXTRA;
}

bool gt_rel_is_scroll_axis(uint16_t axis) {
    return axis == REL_WHEEL || axis == REL_HWHEEL ||
           axis == REL_WHEEL_HI_RES || axis == REL_HWHEEL_HI_RES;
}

bool gt_keycode_is_fn_layer_key(uint16_t code) {
    switch (code) {
    case KEY_DELETE:
    case KEY_HOME:
    case KEY_END:
    case KEY_PAGEUP:
    case KEY_PAGEDOWN:
    case KEY_INSERT:
    case KEY_KPENTER:
    case KEY_F1: case KEY_F2: case KEY_F3: case KEY_F4:
    case KEY_F5: case KEY_F6: case KEY_F7: case KEY_F8:
    case KEY_F9: case KEY_F10: case KEY_F11: case KEY_F12:
    case KEY_BRIGHTNESSDOWN:
    case KEY_BRIGHTNESSUP:
    case KEY_SCALE:
    case KEY_DASHBOARD:
    case KEY_SEARCH:
    case KEY_VOICECOMMAND:
    case KEY_DO_NOT_DISTURB:
    case KEY_KBDILLUMDOWN:
    case KEY_KBDILLUMUP:
    case KEY_KBDILLUMTOGGLE:
    case KEY_PREVIOUSSONG:
    case KEY_PLAYPAUSE:
    case KEY_NEXTSONG:
    case KEY_MUTE:
    case KEY_VOLUMEDOWN:
    case KEY_VOLUMEUP:
    case KEY_MICMUTE:
    case KEY_EJECTCD:
        return true;
    default:
        return false;
    }
}

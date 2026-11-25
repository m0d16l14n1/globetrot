/* SPDX-License-Identifier: MIT */
#include "globe_config.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void set_err(char *errbuf, size_t errbuf_len, const char *fmt, int line, const char *detail) {
    if (!errbuf || errbuf_len == 0)
        return;
    if (line > 0)
        snprintf(errbuf, errbuf_len, "line %d: %s (%s)", line, fmt, detail ? detail : "");
    else
        snprintf(errbuf, errbuf_len, "%s (%s)", fmt, detail ? detail : "");
}

static bool streq_ci(const char *a, const char *b) {
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
            return false;
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static char *trim_inplace(char *s) {
    while (*s && isspace((unsigned char)*s))
        s++;
    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1]))
        s[--len] = '\0';
    return s;
}

static bool parse_bool(const char *val, bool *out) {
    if (streq_ci(val, "1") || streq_ci(val, "true") || streq_ci(val, "yes") || streq_ci(val, "on")) {
        *out = true;
        return true;
    }
    if (streq_ci(val, "0") || streq_ci(val, "false") || streq_ci(val, "no") || streq_ci(val, "off")) {
        *out = false;
        return true;
    }
    return false;
}

static bool parse_u32(const char *val, uint32_t *out) {
    if (!val || !*val || !isdigit((unsigned char)val[0]))
        return false;
    char *endp = NULL;
    unsigned long v = strtoul(val, &endp, 10);
    if (!endp || *endp != '\0' || v > 0xffffffffUL)
        return false;
    *out = (uint32_t)v;
    return true;
}

bool globe_tap_mode_from_name(const char *name, globe_tap_mode_t *out_mode) {
    if (!name || !out_mode)
        return false;
    if (streq_ci(name, "disabled") || streq_ci(name, "off") || streq_ci(name, "none")) {
        *out_mode = GLOBE_TAP_DISABLED;
        return true;
    }
    if (streq_ci(name, "layout") || streq_ci(name, "switch_layout") || streq_ci(name, "language")) {
        *out_mode = GLOBE_TAP_LAYOUT;
        return true;
    }
    if (streq_ci(name, "key") || streq_ci(name, "keycode")) {
        *out_mode = GLOBE_TAP_KEY;
        return true;
    }
    if (streq_ci(name, "both") || streq_ci(name, "layout+key")) {
        *out_mode = GLOBE_TAP_BOTH;
        return true;
    }
    if (streq_ci(name, "emoji") || streq_ci(name, "emoji_picker")) {
        *out_mode = GLOBE_TAP_EMOJI;
        return true;
    }
    if (streq_ci(name, "command") || streq_ci(name, "exec")) {
        *out_mode = GLOBE_TAP_COMMAND;
        return true;
    }
    return false;
}

const char *globe_tap_mode_to_name(globe_tap_mode_t mode) {
    switch (mode) {
    case GLOBE_TAP_DISABLED: return "disabled";
    case GLOBE_TAP_LAYOUT:   return "layout";
    case GLOBE_TAP_KEY:      return "key";
    case GLOBE_TAP_BOTH:     return "both";
    case GLOBE_TAP_EMOJI:    return "emoji";
    case GLOBE_TAP_COMMAND:  return "command";
    default:                 return "unknown";
    }
}

static bool parse_binding_lhs(const char *lhs, uint16_t *out_key, uint8_t *out_mods) {
    char buf[128];
    if (strlen(lhs) >= sizeof(buf))
        return false;
    strcpy(buf, lhs);

    uint8_t mods = MOD_NONE;
    uint16_t trigger = 0;
    int non_mod_count = 0;

    char *p = buf;
    while (*p) {
        char *plus = strchr(p, '+');
        if (plus) *plus = '\0';
        char *tok = trim_inplace(p);
        if (*tok == '\0')
            return false;

        if (streq_ci(tok, "globe") || streq_ci(tok, "fn")) {
            /* Implicit prefix */
        } else if (streq_ci(tok, "shift") || streq_ci(tok, "lshift") || streq_ci(tok, "rshift")) {
            mods |= MOD_SHIFT;
        } else if (streq_ci(tok, "ctrl") || streq_ci(tok, "control") || streq_ci(tok, "lctrl") || streq_ci(tok, "rctrl")) {
            mods |= MOD_CTRL;
        } else if (streq_ci(tok, "alt") || streq_ci(tok, "lalt") || streq_ci(tok, "ralt")) {
            mods |= MOD_ALT;
        } else if (streq_ci(tok, "super") || streq_ci(tok, "meta") || streq_ci(tok, "lmeta") || streq_ci(tok, "rmeta")) {
            mods |= MOD_META;
        } else {
            uint16_t code = 0;
            if (!gt_keycode_from_name(tok, &code))
                return false;
            if (non_mod_count > 0)
                return false;
            trigger = code;
            non_mod_count++;
        }

        if (!plus)
            break;
        p = plus + 1;
    }

    if (non_mod_count != 1 || trigger == 0)
        return false;
    *out_key = trigger;
    *out_mods = mods;
    return true;
}

static bool parse_binding_rhs(const char *rhs, globe_binding_t *b) {
    if (strncmp(rhs, "key:", 4) == 0) {
        const char *spec = rhs + 4;
        if (!gt_chord_parse(spec, &b->out_chord) || b->out_chord.count == 0)
            return false;
        b->kind = GLOBE_BIND_KEY;
        b->action[0] = '\0';
        return true;
    }
    if (strncmp(rhs, "action:", 7) == 0 || strncmp(rhs, "exec:", 5) == 0) {
        const char *body = strchr(rhs, ':') + 1;
        while (*body && isspace((unsigned char)*body))
            body++;
        if (*body == '\0' || strlen(rhs) >= sizeof(b->action))
            return false;
        b->kind = GLOBE_BIND_IPC;
        memset(&b->out_chord, 0, sizeof(b->out_chord));
        snprintf(b->action, sizeof(b->action), "%s", rhs);
        return true;
    }
    if (streq_ci(rhs, "emoji") || streq_ci(rhs, "switch_layout") ||
        streq_ci(rhs, "prev_layout")) {
        b->kind = GLOBE_BIND_IPC;
        memset(&b->out_chord, 0, sizeof(b->out_chord));
        snprintf(b->action, sizeof(b->action), "action:%s", rhs);
        return true;
    }
    return false;
}

bool globe_config_parse_string(const char *text, globe_config_t *cfg, bool overlay,
                               char *errbuf, size_t errbuf_len) {
    if (!text || !cfg) {
        set_err(errbuf, errbuf_len, "null argument", 0, "");
        return false;
    }
    if (!overlay)
        globe_config_defaults(cfg);

    enum { SEC_GLOBE = 0, SEC_BINDINGS = 1, SEC_OTHER = 2 } section = SEC_GLOBE;
    int lineno = 0;
    const char *cur = text;

    while (*cur) {
        lineno++;
        const char *nl = strchr(cur, '\n');
        size_t len = nl ? (size_t)(nl - cur) : strlen(cur);
        char line[512];
        if (len >= sizeof(line)) {
            set_err(errbuf, errbuf_len, "line too long", lineno, "");
            return false;
        }
        memcpy(line, cur, len);
        line[len] = '\0';
        cur = nl ? (nl + 1) : (cur + len);

        char *s = trim_inplace(line);
        if (*s == '\0' || *s == '#' || *s == ';')
            continue;

        if (*s == '[') {
            char *close = strchr(s, ']');
            if (!close) {
                set_err(errbuf, errbuf_len, "unterminated section header", lineno, s);
                return false;
            }
            *close = '\0';
            char *sec = trim_inplace(s + 1);
            if (streq_ci(sec, "globe") || streq_ci(sec, "general"))
                section = SEC_GLOBE;
            else if (streq_ci(sec, "bindings") || streq_ci(sec, "chords"))
                section = SEC_BINDINGS;
            else
                section = SEC_OTHER;
            continue;
        }

        if (section == SEC_OTHER)
            continue;

        char *eq = strchr(s, '=');
        if (!eq) {
            set_err(errbuf, errbuf_len, "expected key = value", lineno, s);
            return false;
        }
        *eq = '\0';
        char *key = trim_inplace(s);
        char *val = trim_inplace(eq + 1);

        if (strncmp(val, "exec:", 5) != 0 && !streq_ci(key, "tap_command") && !streq_ci(key, "double_tap_command")) {
            char *hash = strchr(val, '#');
            if (hash) {
                *hash = '\0';
                val = trim_inplace(val);
            }
        }

        if (section == SEC_GLOBE) {
            if (streq_ci(key, "globe_keycode")) {
                if (!gt_keycode_from_name(val, &cfg->globe_keycode)) {
                    set_err(errbuf, errbuf_len, "invalid globe_keycode", lineno, val);
                    return false;
                }
            } else if (streq_ci(key, "tap_mode")) {
                if (!globe_tap_mode_from_name(val, &cfg->tap_mode)) {
                    set_err(errbuf, errbuf_len, "invalid tap_mode", lineno, val);
                    return false;
                }
            } else if (streq_ci(key, "tap_timeout_ms")) {
                uint32_t ms = 0;
                if (!parse_u32(val, &ms) || ms < GT_MIN_TAP_TIMEOUT_MS || ms > GT_MAX_TAP_TIMEOUT_MS) {
                    set_err(errbuf, errbuf_len, "tap_timeout_ms must be 50..2000", lineno, val);
                    return false;
                }
                cfg->tap_timeout_ms = ms;
            } else if (streq_ci(key, "tap_key")) {
                if (!gt_chord_parse(val, &cfg->tap_chord) || cfg->tap_chord.count == 0) {
                    set_err(errbuf, errbuf_len, "invalid tap_key", lineno, val);
                    return false;
                }
            } else if (streq_ci(key, "tap_command")) {
                if (strlen(val) >= sizeof(cfg->tap_command)) {
                    set_err(errbuf, errbuf_len, "tap_command longer than 127 bytes", lineno, key);
                    return false;
                }
                snprintf(cfg->tap_command, sizeof(cfg->tap_command), "%s", val);
            } else if (streq_ci(key, "double_tap_mode")) {
                if (!globe_tap_mode_from_name(val, &cfg->double_tap_mode)) {
                    set_err(errbuf, errbuf_len, "invalid double_tap_mode", lineno, val);
                    return false;
                }
            } else if (streq_ci(key, "double_tap_window_ms")) {
                uint32_t ms = 0;
                if (!parse_u32(val, &ms) || ms < 100 || ms > 1000) {
                    set_err(errbuf, errbuf_len, "double_tap_window_ms must be 100..1000", lineno, val);
                    return false;
                }
                cfg->double_tap_window_ms = ms;
            } else if (streq_ci(key, "double_tap_key")) {
                if (!gt_chord_parse(val, &cfg->double_tap_chord)) {
                    set_err(errbuf, errbuf_len, "invalid double_tap_key", lineno, val);
                    return false;
                }
            } else if (streq_ci(key, "double_tap_command")) {
                if (strlen(val) >= sizeof(cfg->double_tap_command)) {
                    set_err(errbuf, errbuf_len, "double_tap_command longer than 127 bytes", lineno, key);
                    return false;
                }
                snprintf(cfg->double_tap_command, sizeof(cfg->double_tap_command), "%s", val);
            } else if (streq_ci(key, "combo_modifier")) {
                if (!gt_combo_modifier_parse(val, &cfg->combo_modifier)) {
                    set_err(errbuf, errbuf_len, "invalid combo_modifier", lineno, val);
                    return false;
                }
            } else if (streq_ci(key, "pass_fn_raw")) {
                if (!parse_bool(val, &cfg->pass_fn_raw)) {
                    set_err(errbuf, errbuf_len, "invalid boolean for pass_fn_raw", lineno, val);
                    return false;
                }
            } else if (streq_ci(key, "rollover_safe")) {
                if (!parse_bool(val, &cfg->rollover_safe)) {
                    set_err(errbuf, errbuf_len, "invalid boolean for rollover_safe", lineno, val);
                    return false;
                }
            } else if (streq_ci(key, "cancel_on_pointer")) {
                if (!parse_bool(val, &cfg->cancel_on_pointer)) {
                    set_err(errbuf, errbuf_len, "invalid boolean for cancel_on_pointer", lineno, val);
                    return false;
                }
            } else {
                set_err(errbuf, errbuf_len, "unknown option in [globe]", lineno, key);
                return false;
            }
        } else if (section == SEC_BINDINGS) {
            uint16_t trig = 0;
            uint8_t mods = 0;
            if (!parse_binding_lhs(key, &trig, &mods)) {
                set_err(errbuf, errbuf_len, "invalid binding trigger", lineno, key);
                return false;
            }
            globe_binding_t b;
            memset(&b, 0, sizeof(b));
            b.trigger_key = trig;
            b.req_mods = mods;
            if (!parse_binding_rhs(val, &b)) {
                set_err(errbuf, errbuf_len, "invalid binding target (use key:..., action:..., or exec:...)", lineno, val);
                return false;
            }
            bool replaced = false;
            for (size_t i = 0; i < cfg->binding_count; i++) {
                if (cfg->bindings[i].trigger_key == trig && cfg->bindings[i].req_mods == mods) {
                    cfg->bindings[i] = b;
                    replaced = true;
                    break;
                }
            }
            if (!replaced) {
                if (cfg->binding_count >= GT_MAX_BINDINGS) {
                    set_err(errbuf, errbuf_len, "too many bindings (max 64)", lineno, key);
                    return false;
                }
                cfg->bindings[cfg->binding_count++] = b;
            }
        }
    }
    return true;
}

bool globe_config_load_file(const char *path, globe_config_t *cfg, bool missing_ok,
                            char *errbuf, size_t errbuf_len) {
    if (!path || !cfg)
        return false;
    FILE *f = fopen(path, "rb");
    if (!f) {
        if (errno == ENOENT && missing_ok) {
            globe_config_defaults(cfg);
            return true;
        }
        set_err(errbuf, errbuf_len, "cannot open config file", 0, strerror(errno));
        return false;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        set_err(errbuf, errbuf_len, "fseek failed", 0, path);
        return false;
    }
    long sz = ftell(f);
    if (sz < 0 || sz > 262144) {
        fclose(f);
        set_err(errbuf, errbuf_len, "config file size out of range", 0, path);
        return false;
    }
    rewind(f);

    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        set_err(errbuf, errbuf_len, "out of memory", 0, path);
        return false;
    }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[rd] = '\0';

    bool ok = globe_config_parse_string(buf, cfg, false, errbuf, errbuf_len);
    free(buf);
    return ok;
}

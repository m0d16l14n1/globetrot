/* SPDX-License-Identifier: MIT */
#include "globe_sm.h"

#include <stdio.h>
#include <string.h>

static const uint16_t k_all_std_mods[8] = {
    KEY_LEFTSHIFT, KEY_RIGHTSHIFT,
    KEY_LEFTCTRL,  KEY_RIGHTCTRL,
    KEY_LEFTALT,   KEY_RIGHTALT,
    KEY_LEFTMETA,  KEY_RIGHTMETA,
};

static void batch_clear(globe_out_batch_t *out) {
    if (out)
        out->count = 0;
}

static void batch_push(globe_out_batch_t *out, const globe_out_event_t *ev) {
    if (!out || out->count >= GT_MAX_OUT_EVENTS)
        return;
    out->events[out->count++] = *ev;
}

static void emit_key_raw(globe_out_batch_t *out, uint16_t code, int32_t value) {
    globe_out_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = GLOBE_OUT_KEY;
    ev.code = code;
    ev.value = value;
    batch_push(out, &ev);
}

static void emit_syn(globe_out_batch_t *out) {
    if (!out || out->count == 0)
        return;
    /* Avoid redundant consecutive SYN_REPORT frames. */
    if (out->events[out->count - 1].type == GLOBE_OUT_SYN)
        return;
    globe_out_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = GLOBE_OUT_SYN;
    batch_push(out, &ev);
}

static bool batch_has_unflushed_key(const globe_out_batch_t *out) {
    if (!out || out->count == 0)
        return false;
    for (size_t i = out->count; i > 0; i--) {
        globe_out_type_t t = out->events[i - 1].type;
        if (t == GLOBE_OUT_SYN)
            return false;
        if (t == GLOBE_OUT_KEY)
            return true;
    }
    return false;
}

static void finalize_syn(globe_out_batch_t *out) {
    if (batch_has_unflushed_key(out))
        emit_syn(out);
}

static void virt_press(globe_sm_t *sm, uint16_t code, globe_out_batch_t *out) {
    if (code >= GT_MAX_KEYCODE)
        return;
    if (sm->virt_refcount[code] == 0)
        emit_key_raw(out, code, 1);
    if (sm->virt_refcount[code] < 255)
        sm->virt_refcount[code]++;
}

static void virt_release(globe_sm_t *sm, uint16_t code, globe_out_batch_t *out) {
    if (code >= GT_MAX_KEYCODE)
        return;
    if (sm->virt_refcount[code] == 0)
        return;
    sm->virt_refcount[code]--;
    if (sm->virt_refcount[code] == 0)
        emit_key_raw(out, code, 0);
}

static void virt_repeat(globe_sm_t *sm, uint16_t code, globe_out_batch_t *out) {
    if (code >= GT_MAX_KEYCODE)
        return;
    if (sm->virt_refcount[code] > 0)
        emit_key_raw(out, code, 2);
}

static bool chord_contains(const gt_key_chord_t *ch, uint16_t code) {
    for (uint8_t i = 0; i < ch->count; i++) {
        if (ch->keys[i] == code)
            return true;
    }
    return false;
}

static void emit_chord_press(globe_sm_t *sm, const gt_key_chord_t *ch, globe_out_batch_t *out) {
    for (uint8_t i = 0; i < ch->count; i++)
        virt_press(sm, ch->keys[i], out);
}

static void emit_chord_release(globe_sm_t *sm, const gt_key_chord_t *ch, globe_out_batch_t *out) {
    for (int i = (int)ch->count - 1; i >= 0; i--)
        virt_release(sm, ch->keys[i], out);
}

/* Emit a synthesized chord tap with an explicit SYN_REPORT between DOWN and UP
 * so libinput and Wayland compositors always observe distinct press/release frames. */
static void emit_chord_tap(globe_sm_t *sm, const gt_key_chord_t *ch, globe_out_batch_t *out) {
    if (ch->count == 0)
        return;
    emit_chord_press(sm, ch, out);
    emit_syn(out);
    emit_chord_release(sm, ch, out);
    emit_syn(out);
}

static void emit_ipc(globe_out_batch_t *out, globe_out_type_t type, globe_tap_mode_t mode,
                     uint32_t elapsed_ms, uint16_t trigger_key, const char *action) {
    globe_out_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = type;
    ev.tap_mode = mode;
    ev.elapsed_ms = elapsed_ms;
    ev.trigger_key = trigger_key;
    if (action && *action)
        snprintf(ev.action, sizeof(ev.action), "%s", action);
    batch_push(out, &ev);
}

static void dispatch_tap_action(globe_sm_t *sm, globe_tap_mode_t mode,
                                const gt_key_chord_t *chord,
                                const char *command,
                                globe_out_type_t ipc_type,
                                uint32_t elapsed_ms,
                                globe_out_batch_t *out) {
    switch (mode) {
    case GLOBE_TAP_DISABLED:
        break;
    case GLOBE_TAP_KEY:
        emit_chord_tap(sm, chord, out);
        emit_ipc(out, ipc_type, mode, elapsed_ms, sm->cfg.globe_keycode, "");
        break;
    case GLOBE_TAP_BOTH:
        emit_chord_tap(sm, chord, out);
        emit_ipc(out, ipc_type, mode, elapsed_ms, sm->cfg.globe_keycode, "switch_layout");
        break;
    case GLOBE_TAP_LAYOUT:
        emit_ipc(out, ipc_type, mode, elapsed_ms, sm->cfg.globe_keycode, "switch_layout");
        break;
    case GLOBE_TAP_EMOJI:
        emit_ipc(out, ipc_type, mode, elapsed_ms, sm->cfg.globe_keycode, "emoji");
        break;
    case GLOBE_TAP_COMMAND:
        emit_ipc(out, ipc_type, mode, elapsed_ms, sm->cfg.globe_keycode, command);
        break;
    }
}

static void cancel_pending_tap(globe_sm_t *sm, uint16_t cause_code, globe_out_batch_t *out) {
    if (!sm->pending_tap)
        return;
    sm->pending_tap = false;
    emit_ipc(out, GLOBE_OUT_IPC_CANCELLED, sm->cfg.tap_mode, 0, cause_code, "");
}

static void activate_combo_modifier(globe_sm_t *sm, globe_out_batch_t *out) {
    if (sm->combo_mod_active || sm->cfg.combo_modifier.count == 0)
        return;
    emit_chord_press(sm, &sm->cfg.combo_modifier, out);
    sm->combo_mod_active = true;
}

static void deactivate_combo_modifier(globe_sm_t *sm, globe_out_batch_t *out) {
    if (!sm->combo_mod_active)
        return;
    emit_chord_release(sm, &sm->cfg.combo_modifier, out);
    sm->combo_mod_active = false;
}

static int find_matching_binding(const globe_sm_t *sm, uint16_t code, uint8_t cur_mods) {
    for (size_t i = 0; i < sm->cfg.binding_count; i++) {
        const globe_binding_t *b = &sm->cfg.bindings[i];
        if (b->kind != GLOBE_BIND_NONE && b->trigger_key == code && b->req_mods == cur_mods)
            return (int)i;
    }
    return -1;
}

/* Temporarily release physically held trigger modifiers required by a binding
 * that are NOT part of out_chord, so e.g. globe+ctrl+left -> key:leftmeta+left
 * emits pure Super+Left rather than Ctrl+Super+Left. */
static void mask_trigger_mods(globe_sm_t *sm, uint16_t trigger_code, uint8_t req_mods,
                              const gt_key_chord_t *out_chord, globe_out_batch_t *out) {
    masked_mods_t *mm = &sm->masked_mods[trigger_code];
    mm->count = 0;
    if (req_mods == MOD_NONE)
        return;

    bool released_any = false;
    for (size_t i = 0; i < sizeof(k_all_std_mods) / sizeof(k_all_std_mods[0]); i++) {
        uint16_t mcode = k_all_std_mods[i];
        if ((gt_keycode_mod_bit(mcode) & req_mods) == 0)
            continue;
        if (!sm->phys_down[mcode] || sm->virt_refcount[mcode] == 0)
            continue;
        if (chord_contains(out_chord, mcode))
            continue;
        virt_release(sm, mcode, out);
        if (mm->count < GT_MAX_MASKED_MODS) {
            mm->keys[mm->count] = mcode;
            mm->seq[mm->count] = sm->phys_seq[mcode];
            mm->count++;
        }
        released_any = true;
    }
    if (released_any)
        emit_syn(out);
}

static void unmask_trigger_mods(globe_sm_t *sm, uint16_t trigger_code, globe_out_batch_t *out) {
    masked_mods_t *mm = &sm->masked_mods[trigger_code];
    if (mm->count == 0)
        return;
    for (uint8_t i = 0; i < mm->count; i++) {
        uint16_t mcode = mm->keys[i];
        /* Restore only if the same physical press is still held. */
        if (sm->phys_down[mcode] && sm->phys_seq[mcode] == mm->seq[i])
            virt_press(sm, mcode, out);
    }
    mm->count = 0;
}

void globe_config_defaults(globe_config_t *cfg) {
    if (!cfg)
        return;
    memset(cfg, 0, sizeof(*cfg));
    cfg->globe_keycode = KEY_FN;
    cfg->tap_mode = GLOBE_TAP_LAYOUT;
    cfg->tap_timeout_ms = GT_DEFAULT_TAP_TIMEOUT_MS;
    cfg->tap_chord.keys[0] = GT_DEFAULT_TAP_KEYCODE;
    cfg->tap_chord.count = 1;

    cfg->double_tap_mode = GLOBE_TAP_DISABLED;
    cfg->double_tap_window_ms = GT_DEFAULT_DOUBLE_TAP_MS;
    cfg->double_tap_chord.keys[0] = GT_DEFAULT_DOUBLE_TAP_KEYCODE;
    cfg->double_tap_chord.count = 1;

    cfg->combo_modifier.count = 0;
    cfg->pass_fn_raw = false;
    cfg->rollover_safe = true;
    cfg->cancel_on_pointer = true;
}

void globe_sm_init(globe_sm_t *sm, const globe_config_t *cfg) {
    memset(sm, 0, sizeof(*sm));
    if (cfg)
        sm->cfg = *cfg;
    else
        globe_config_defaults(&sm->cfg);

    if (sm->cfg.globe_keycode == 0 || sm->cfg.globe_keycode >= GT_MAX_KEYCODE)
        sm->cfg.globe_keycode = KEY_FN;
    if (sm->cfg.tap_timeout_ms < GT_MIN_TAP_TIMEOUT_MS ||
        sm->cfg.tap_timeout_ms > GT_MAX_TAP_TIMEOUT_MS)
        sm->cfg.tap_timeout_ms = GT_DEFAULT_TAP_TIMEOUT_MS;
    if (sm->cfg.double_tap_window_ms < 100 || sm->cfg.double_tap_window_ms > 1000)
        sm->cfg.double_tap_window_ms = GT_DEFAULT_DOUBLE_TAP_MS;
    for (size_t i = 0; i < GT_MAX_KEYCODE; i++)
        sm->remap_binding_idx[i] = -1;
}

uint8_t globe_sm_active_mods(const globe_sm_t *sm) {
    uint8_t m = MOD_NONE;
    if (sm->phys_down[KEY_LEFTSHIFT] || sm->phys_down[KEY_RIGHTSHIFT]) m |= MOD_SHIFT;
    if (sm->phys_down[KEY_LEFTCTRL]  || sm->phys_down[KEY_RIGHTCTRL])  m |= MOD_CTRL;
    if (sm->phys_down[KEY_LEFTALT]   || sm->phys_down[KEY_RIGHTALT])   m |= MOD_ALT;
    if (sm->phys_down[KEY_LEFTMETA]  || sm->phys_down[KEY_RIGHTMETA])  m |= MOD_META;
    return m;
}

static void flush_deferred_single_tap(globe_sm_t *sm, globe_out_batch_t *out) {
    if (!sm->awaiting_double_tap)
        return;
    sm->awaiting_double_tap = false;
    dispatch_tap_action(sm, sm->cfg.tap_mode, &sm->cfg.tap_chord,
                        sm->cfg.tap_command, GLOBE_OUT_IPC_TAP,
                        sm->first_tap_elapsed_ms, out);
}

void globe_sm_handle_key(globe_sm_t *sm, uint16_t code, int32_t value,
                         uint64_t ts_ns, globe_out_batch_t *out) {
    batch_clear(out);
    if (code >= GT_MAX_KEYCODE)
        return;

    /* 1. Globe / Fn trigger key */
    if (code == sm->cfg.globe_keycode) {
        if (value == 2)
            return;

        if (value == 1) {
            if (sm->awaiting_double_tap) {
                uint64_t dt_ns = (ts_ns >= sm->first_tap_up_ns) ? (ts_ns - sm->first_tap_up_ns) : 0;
                uint64_t win_ns = (uint64_t)sm->cfg.double_tap_window_ms * 1000000ULL;
                if (dt_ns > win_ns)
                    flush_deferred_single_tap(sm, out);
            }

            if (!sm->globe_down) {
                sm->globe_down = true;
                sm->globe_down_ns = ts_ns;
                sm->phys_down[code] = 1;

                memcpy(sm->pressed_before_globe, sm->phys_down, sizeof(sm->pressed_before_globe));
                sm->pressed_before_globe[code] = 0;

                /* Solo tap only arms when no standard modifier is already held. */
                sm->pending_tap = (globe_sm_active_mods(sm) == MOD_NONE);

                if (sm->cfg.pass_fn_raw)
                    virt_press(sm, code, out);
            }
            finalize_syn(out);
            return;
        }

        if (!sm->globe_down)
            return;
        sm->globe_down = false;
        sm->phys_down[code] = 0;
        memset(sm->pressed_before_globe, 0, sizeof(sm->pressed_before_globe));

        deactivate_combo_modifier(sm, out);
        if (sm->cfg.pass_fn_raw)
            virt_release(sm, code, out);

        if (sm->pending_tap) {
            sm->pending_tap = false;
            uint64_t elapsed_ns = (ts_ns >= sm->globe_down_ns) ? (ts_ns - sm->globe_down_ns) : 0;
            uint64_t timeout_ns = (uint64_t)sm->cfg.tap_timeout_ms * 1000000ULL;
            uint32_t elapsed_ms = (uint32_t)(elapsed_ns / 1000000ULL);

            if (globe_sm_active_mods(sm) != MOD_NONE) {
                emit_ipc(out, GLOBE_OUT_IPC_CANCELLED, sm->cfg.tap_mode, elapsed_ms, code, "");
                finalize_syn(out);
                return;
            }

            if (elapsed_ns <= timeout_ns) {
                if (sm->cfg.double_tap_mode != GLOBE_TAP_DISABLED) {
                    if (sm->awaiting_double_tap) {
                        uint64_t dt_ns = (ts_ns >= sm->first_tap_up_ns) ? (ts_ns - sm->first_tap_up_ns) : 0;
                        uint64_t win_ns = (uint64_t)sm->cfg.double_tap_window_ms * 1000000ULL;
                        if (dt_ns <= win_ns) {
                            sm->awaiting_double_tap = false;
                            dispatch_tap_action(sm, sm->cfg.double_tap_mode,
                                                &sm->cfg.double_tap_chord,
                                                sm->cfg.double_tap_command,
                                                GLOBE_OUT_IPC_DOUBLE_TAP,
                                                elapsed_ms, out);
                            finalize_syn(out);
                            return;
                        }
                        flush_deferred_single_tap(sm, out);
                    }
                    sm->awaiting_double_tap = true;
                    sm->first_tap_up_ns = ts_ns;
                    sm->first_tap_elapsed_ms = elapsed_ms;
                } else {
                    dispatch_tap_action(sm, sm->cfg.tap_mode, &sm->cfg.tap_chord,
                                        sm->cfg.tap_command, GLOBE_OUT_IPC_TAP,
                                        elapsed_ms, out);
                }
            } else {
                emit_ipc(out, GLOBE_OUT_IPC_DEBOUNCED, sm->cfg.tap_mode, elapsed_ms, code, "");
            }
        }
        finalize_syn(out);
        return;
    }

    /* 2. Any new non-Globe press flushes a deferred single tap immediately. */
    if (value == 1 && sm->awaiting_double_tap)
        flush_deferred_single_tap(sm, out);

    /* 3. Non-Globe key PRESS */
    if (value == 1) {
        if (sm->phys_down[code])
            return;
        sm->phys_down[code] = 1;
        sm->phys_seq[code]++;

        if (!sm->globe_down) {
            sm->prov[code] = KEY_PROV_PASSTHROUGH;
            sm->remap_binding_idx[code] = -1;
            virt_press(sm, code, out);
            finalize_syn(out);
            return;
        }

        cancel_pending_tap(sm, code, out);

        if (gt_keycode_mod_bit(code) != MOD_NONE) {
            sm->prov[code] = KEY_PROV_PASSTHROUGH;
            sm->remap_binding_idx[code] = -1;
            virt_press(sm, code, out);
            finalize_syn(out);
            return;
        }

        uint8_t cur_mods = globe_sm_active_mods(sm);
        int bidx = find_matching_binding(sm, code, cur_mods);
        if (bidx >= 0) {
            const globe_binding_t *b = &sm->cfg.bindings[bidx];
            if (b->kind == GLOBE_BIND_KEY) {
                if (sm->combo_mod_active) {
                    deactivate_combo_modifier(sm, out);
                    emit_syn(out);
                }
                sm->prov[code] = KEY_PROV_REMAP_CHORD;
                sm->remap_binding_idx[code] = (int16_t)bidx;
                mask_trigger_mods(sm, code, b->req_mods, &b->out_chord, out);
                emit_chord_press(sm, &b->out_chord, out);
            } else if (b->kind == GLOBE_BIND_IPC) {
                sm->prov[code] = KEY_PROV_SUPPRESSED;
                sm->remap_binding_idx[code] = (int16_t)bidx;
                emit_ipc(out, GLOBE_OUT_IPC_CHORD, GLOBE_TAP_COMMAND, 0, code, b->action);
            }
            finalize_syn(out);
            return;
        }

        /* Preserve kernel Fn-layer outputs pure & unmodified. */
        if (gt_keycode_is_fn_layer_key(code)) {
            if (sm->combo_mod_active) {
                deactivate_combo_modifier(sm, out);
                emit_syn(out);
            }
            sm->prov[code] = KEY_PROV_PASSTHROUGH;
            sm->remap_binding_idx[code] = -1;
            virt_press(sm, code, out);
            finalize_syn(out);
            return;
        }

        if (sm->cfg.combo_modifier.count > 0) {
            activate_combo_modifier(sm, out);
            sm->prov[code] = KEY_PROV_COMBO_MOD;
            sm->remap_binding_idx[code] = -1;
            virt_press(sm, code, out);
            finalize_syn(out);
            return;
        }

        sm->prov[code] = KEY_PROV_PASSTHROUGH;
        sm->remap_binding_idx[code] = -1;
        virt_press(sm, code, out);
        finalize_syn(out);
        return;
    }

    /* 4. Non-Globe key REPEAT */
    if (value == 2) {
        switch (sm->prov[code]) {
        case KEY_PROV_PASSTHROUGH:
        case KEY_PROV_COMBO_MOD:
            virt_repeat(sm, code, out);
            break;
        case KEY_PROV_REMAP_CHORD: {
            int bidx = sm->remap_binding_idx[code];
            if (bidx >= 0 && (size_t)bidx < sm->cfg.binding_count) {
                const globe_binding_t *b = &sm->cfg.bindings[bidx];
                if (b->out_chord.count > 0)
                    virt_repeat(sm, b->out_chord.keys[b->out_chord.count - 1], out);
            }
            break;
        }
        case KEY_PROV_SUPPRESSED:
        case KEY_PROV_NONE:
            break;
        }
        finalize_syn(out);
        return;
    }

    /* 5. Non-Globe key RELEASE */
    sm->phys_down[code] = 0;

    if (sm->globe_down && sm->pending_tap) {
        bool was_prior = (sm->pressed_before_globe[code] != 0);
        sm->pressed_before_globe[code] = 0;
        if (!sm->cfg.rollover_safe || !was_prior)
            cancel_pending_tap(sm, code, out);
    }

    uint8_t p = sm->prov[code];
    int16_t bidx = sm->remap_binding_idx[code];
    sm->prov[code] = KEY_PROV_NONE;
    sm->remap_binding_idx[code] = -1;

    switch (p) {
    case KEY_PROV_PASSTHROUGH:
    case KEY_PROV_COMBO_MOD:
        virt_release(sm, code, out);
        break;
    case KEY_PROV_REMAP_CHORD:
        if (bidx >= 0 && (size_t)bidx < sm->cfg.binding_count) {
            emit_chord_release(sm, &sm->cfg.bindings[bidx].out_chord, out);
            if (sm->masked_mods[code].count > 0) {
                emit_syn(out);
                unmask_trigger_mods(sm, code, out);
            }
        }
        break;
    case KEY_PROV_SUPPRESSED:
    case KEY_PROV_NONE:
        break;
    }
    finalize_syn(out);
}

void globe_sm_handle_pointer_btn(globe_sm_t *sm, uint16_t btn_code, int32_t value,
                                 uint64_t ts_ns, globe_out_batch_t *out) {
    (void)ts_ns;
    batch_clear(out);
    if (!sm->cfg.cancel_on_pointer)
        return;
    if (value == 1 && gt_keycode_is_pointer_btn(btn_code)) {
        if (sm->globe_down && sm->pending_tap)
            cancel_pending_tap(sm, btn_code, out);
        if (sm->awaiting_double_tap)
            flush_deferred_single_tap(sm, out);
    }
    finalize_syn(out);
}

void globe_sm_handle_pointer_scroll(globe_sm_t *sm, uint16_t axis, int32_t delta,
                                    uint64_t ts_ns, globe_out_batch_t *out) {
    (void)ts_ns;
    batch_clear(out);
    if (!sm->cfg.cancel_on_pointer || delta == 0)
        return;
    if (gt_rel_is_scroll_axis(axis)) {
        if (sm->globe_down && sm->pending_tap)
            cancel_pending_tap(sm, axis, out);
    }
}

int32_t globe_sm_tick(globe_sm_t *sm, uint64_t now_ns, globe_out_batch_t *out) {
    batch_clear(out);
    if (!sm->awaiting_double_tap)
        return -1;
    uint64_t win_ns = (uint64_t)sm->cfg.double_tap_window_ms * 1000000ULL;
    uint64_t deadline_ns = sm->first_tap_up_ns + win_ns;
    if (now_ns >= deadline_ns) {
        flush_deferred_single_tap(sm, out);
        finalize_syn(out);
        return -1;
    }
    uint64_t rem_ms = (deadline_ns - now_ns + 999999ULL) / 1000000ULL;
    return (rem_ms > 10000ULL) ? 10000 : (int32_t)rem_ms;
}

bool globe_sm_has_virtual_keys(const globe_sm_t *sm) {
    for (size_t code = 0; code < GT_MAX_KEYCODE; code++) {
        if (sm->virt_refcount[code] > 0)
            return true;
    }
    return false;
}

void globe_sm_release_all(globe_sm_t *sm, globe_out_batch_t *out) {
    batch_clear(out);
    for (uint16_t code = 0; code < GT_MAX_KEYCODE; code++) {
        /* Keep one slot for the closing SYN_REPORT; a key that does not fit
         * keeps its refcount and is released by the next call. */
        if (sm->virt_refcount[code] > 0 && (!out || out->count + 1 < GT_MAX_OUT_EVENTS)) {
            sm->virt_refcount[code] = 0;
            emit_key_raw(out, code, 0);
        }
        sm->phys_down[code] = 0;
        sm->pressed_before_globe[code] = 0;
        sm->prov[code] = KEY_PROV_NONE;
        sm->remap_binding_idx[code] = -1;
        sm->masked_mods[code].count = 0;
    }
    sm->globe_down = false;
    sm->pending_tap = false;
    sm->combo_mod_active = false;
    sm->awaiting_double_tap = false;
    finalize_syn(out);
}

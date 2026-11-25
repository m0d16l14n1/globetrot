/* SPDX-License-Identifier: MIT */
#include "../src/globe_sm.h"
#include "../src/keycodes.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define MS(x) ((uint64_t)(x) * 1000000ULL)

static int g_tests_run = 0;

#define RUN_TEST(fn) do { \
    fn(); \
    g_tests_run++; \
    printf("  [PASS] %s\n", #fn); \
} while (0)

static bool has_ipc(const globe_out_batch_t *b, globe_out_type_t type) {
    for (size_t i = 0; i < b->count; i++) {
        if (b->events[i].type == type)
            return true;
    }
    return false;
}

static bool has_key(const globe_out_batch_t *b, uint16_t code, int32_t val) {
    for (size_t i = 0; i < b->count; i++) {
        if (b->events[i].type == GLOBE_OUT_KEY && b->events[i].code == code && b->events[i].value == val)
            return true;
    }
    return false;
}

static int find_key_idx(const globe_out_batch_t *b, uint16_t code, int32_t val) {
    for (size_t i = 0; i < b->count; i++) {
        if (b->events[i].type == GLOBE_OUT_KEY && b->events[i].code == code && b->events[i].value == val)
            return (int)i;
    }
    return -1;
}

static bool has_syn_between(const globe_out_batch_t *b, int a, int c) {
    if (a < 0 || c <= a)
        return false;
    for (int i = a + 1; i < c; i++) {
        if (b->events[i].type == GLOBE_OUT_SYN)
            return true;
    }
    return false;
}

static size_t count_keys(const globe_out_batch_t *b) {
    size_t c = 0;
    for (size_t i = 0; i < b->count; i++) {
        if (b->events[i].type == GLOBE_OUT_KEY)
            c++;
    }
    return c;
}

static void assert_no_stuck_keys(const globe_sm_t *sm) {
    for (size_t i = 0; i < GT_MAX_KEYCODE; i++) {
        assert(sm->virt_refcount[i] == 0);
    }
}

/* 1. Solo tap within default 400ms ceiling dispatches layout switch cleanly. */
static void test_solo_tap_layout_within_default_ceiling(void) {
    globe_sm_t sm;
    globe_out_batch_t out;
    globe_sm_init(&sm, NULL);

    globe_sm_handle_key(&sm, KEY_FN, 1, MS(1000), &out);
    assert(out.count == 0); /* pass_fn_raw=false: zero stray keys */

    /* Hardware auto-repeat on Fn must not cancel or reset timestamp. */
    globe_sm_handle_key(&sm, KEY_FN, 2, MS(1180), &out);
    assert(out.count == 0);

    /* Release at exact 400ms boundary -> dispatched! */
    globe_sm_handle_key(&sm, KEY_FN, 0, MS(1400), &out);
    assert(has_ipc(&out, GLOBE_OUT_IPC_TAP));
    assert(count_keys(&out) == 0);
    assert(out.events[0].elapsed_ms == 400);
    assert(strcmp(out.events[0].action, "switch_layout") == 0);
    assert_no_stuck_keys(&sm);
}

/* 2. Holding Globe past tap_timeout_ms (401ms) and releasing alone is debounced. */
static void test_solo_hold_debounced_over_timeout(void) {
    globe_sm_t sm;
    globe_out_batch_t out;
    globe_sm_init(&sm, NULL);

    globe_sm_handle_key(&sm, KEY_FN, 1, MS(1000), &out);
    globe_sm_handle_key(&sm, KEY_FN, 0, MS(1401), &out);
    assert(!has_ipc(&out, GLOBE_OUT_IPC_TAP));
    assert(has_ipc(&out, GLOBE_OUT_IPC_DEBOUNCED));
    assert(count_keys(&out) == 0);
    assert_no_stuck_keys(&sm);
}

/* 3. tap_mode = key emits KEY_F24 DOWN -> SYN_REPORT -> KEY_F24 UP -> SYN_REPORT. */
static void test_solo_tap_key_mode_syn_framing(void) {
    globe_config_t cfg;
    globe_config_defaults(&cfg);
    cfg.tap_mode = GLOBE_TAP_KEY;
    assert(gt_chord_parse("f24", &cfg.tap_chord));

    globe_sm_t sm;
    globe_out_batch_t out;
    globe_sm_init(&sm, &cfg);

    globe_sm_handle_key(&sm, KEY_FN, 1, MS(100), &out);
    assert(out.count == 0);
    globe_sm_handle_key(&sm, KEY_FN, 0, MS(180), &out);

    int dn = find_key_idx(&out, KEY_F24, 1);
    int up = find_key_idx(&out, KEY_F24, 0);
    assert(dn >= 0 && up > dn);
    assert(has_syn_between(&out, dn, up));
    assert(has_ipc(&out, GLOBE_OUT_IPC_TAP));
    assert_no_stuck_keys(&sm);
}

/* 4. tap_mode = key with multi-key chord emits press -> SYN -> reverse release -> SYN. */
static void test_solo_tap_chord_mode_syn_framing(void) {
    globe_config_t cfg;
    globe_config_defaults(&cfg);
    cfg.tap_mode = GLOBE_TAP_KEY;
    assert(gt_chord_parse("leftmeta+space", &cfg.tap_chord));

    globe_sm_t sm;
    globe_out_batch_t out;
    globe_sm_init(&sm, &cfg);

    globe_sm_handle_key(&sm, KEY_FN, 1, MS(100), &out);
    globe_sm_handle_key(&sm, KEY_FN, 0, MS(170), &out);
    assert(out.count >= 6);
    assert(out.events[0].type == GLOBE_OUT_KEY && out.events[0].code == KEY_LEFTMETA && out.events[0].value == 1);
    assert(out.events[1].type == GLOBE_OUT_KEY && out.events[1].code == KEY_SPACE    && out.events[1].value == 1);
    assert(out.events[2].type == GLOBE_OUT_SYN);
    assert(out.events[3].type == GLOBE_OUT_KEY && out.events[3].code == KEY_SPACE    && out.events[3].value == 0);
    assert(out.events[4].type == GLOBE_OUT_KEY && out.events[4].code == KEY_LEFTMETA && out.events[4].value == 0);
    assert(out.events[5].type == GLOBE_OUT_SYN);
    assert_no_stuck_keys(&sm);
}

/* 5. Native kernel Fn-layer outputs (Delete, Home, F1, BrightnessDown) stay pure
 *    even when combo_modifier = super_alt is enabled, AND cancel solo Globe tap. */
static void test_fn_layer_outputs_stay_pure(void) {
    globe_config_t cfg;
    globe_config_defaults(&cfg);
    assert(gt_combo_modifier_parse("super_alt", &cfg.combo_modifier));

    globe_sm_t sm;
    globe_out_batch_t out;
    globe_sm_init(&sm, &cfg);

    uint16_t hw_keys[] = { KEY_DELETE, KEY_HOME, KEY_END, KEY_PAGEUP, KEY_PAGEDOWN,
                           KEY_INSERT, KEY_KPENTER, KEY_F1, KEY_BRIGHTNESSDOWN,
                           KEY_VOLUMEUP, KEY_MICMUTE };
    for (size_t i = 0; i < sizeof(hw_keys) / sizeof(hw_keys[0]); i++) {
        uint16_t k = hw_keys[i];
        globe_sm_handle_key(&sm, KEY_FN, 1, MS(1000 + i * 500), &out);
        globe_sm_handle_key(&sm, k, 1, MS(1050 + i * 500), &out);
        assert(has_ipc(&out, GLOBE_OUT_IPC_CANCELLED));
        assert(has_key(&out, k, 1));
        assert(!has_key(&out, KEY_LEFTMETA, 1));
        assert(!has_key(&out, KEY_LEFTALT, 1));

        globe_sm_handle_key(&sm, k, 0, MS(1090 + i * 500), &out);
        assert(has_key(&out, k, 0));

        globe_sm_handle_key(&sm, KEY_FN, 0, MS(1110 + i * 500), &out);
        assert(!has_ipc(&out, GLOBE_OUT_IPC_TAP));
        assert_no_stuck_keys(&sm);
    }
}

/* 6. Universal combo_modifier wraps unbound Globe+C cleanly AND handles Fn released BEFORE C. */
static void test_combo_modifier_and_early_fn_release(void) {
    globe_config_t cfg;
    globe_config_defaults(&cfg);
    assert(gt_combo_modifier_parse("super_alt", &cfg.combo_modifier));

    globe_sm_t sm;
    globe_out_batch_t out;
    globe_sm_init(&sm, &cfg);

    globe_sm_handle_key(&sm, KEY_FN, 1, MS(100), &out);
    assert(count_keys(&out) == 0);

    globe_sm_handle_key(&sm, KEY_C, 1, MS(140), &out);
    assert(has_ipc(&out, GLOBE_OUT_IPC_CANCELLED));
    assert(has_key(&out, KEY_LEFTMETA, 1));
    assert(has_key(&out, KEY_LEFTALT, 1));
    assert(has_key(&out, KEY_C, 1));

    /* Release Fn BEFORE releasing C -> combo modifier releases cleanly, C stays held. */
    globe_sm_handle_key(&sm, KEY_FN, 0, MS(180), &out);
    assert(!has_ipc(&out, GLOBE_OUT_IPC_TAP));
    assert(has_key(&out, KEY_LEFTALT, 0));
    assert(has_key(&out, KEY_LEFTMETA, 0));
    assert(!has_key(&out, KEY_C, 0));

    /* Now release C -> C releases cleanly, zero stuck keys. */
    globe_sm_handle_key(&sm, KEY_C, 0, MS(220), &out);
    assert(has_key(&out, KEY_C, 0));
    assert_no_stuck_keys(&sm);
}

/* 6b. Combo modifier stays held across consecutive unbound keys (switcher UX),
 *     but deactivates cleanly with SYN_REPORT before a pure Fn-layer or bound key. */
static void test_combo_modifier_deactivates_for_fn_layer_and_bindings(void) {
    globe_config_t cfg;
    globe_config_defaults(&cfg);
    assert(gt_combo_modifier_parse("super_alt", &cfg.combo_modifier));
    cfg.bindings[0].trigger_key = KEY_F;
    cfg.bindings[0].req_mods = MOD_NONE;
    cfg.bindings[0].kind = GLOBE_BIND_KEY;
    assert(gt_chord_parse("f11", &cfg.bindings[0].out_chord));
    cfg.binding_count = 1;

    globe_sm_t sm;
    globe_out_batch_t out;
    globe_sm_init(&sm, &cfg);

    globe_sm_handle_key(&sm, KEY_FN, 1, MS(100), &out);
    /* Tap Tab twice while holding Fn -> Super+Alt stays held between taps! */
    globe_sm_handle_key(&sm, KEY_TAB, 1, MS(120), &out);
    assert(has_key(&out, KEY_LEFTMETA, 1) && has_key(&out, KEY_LEFTALT, 1) && has_key(&out, KEY_TAB, 1));
    globe_sm_handle_key(&sm, KEY_TAB, 0, MS(140), &out);
    assert(!has_key(&out, KEY_LEFTMETA, 0) && !has_key(&out, KEY_LEFTALT, 0));
    globe_sm_handle_key(&sm, KEY_TAB, 1, MS(160), &out);
    assert(!has_key(&out, KEY_LEFTMETA, 1) && has_key(&out, KEY_TAB, 1));
    globe_sm_handle_key(&sm, KEY_TAB, 0, MS(180), &out);

    /* Now press pure Fn-layer KEY_DELETE while still holding Fn -> Super+Alt MUST release first! */
    globe_sm_handle_key(&sm, KEY_DELETE, 1, MS(200), &out);
    int meta_up = find_key_idx(&out, KEY_LEFTMETA, 0);
    int del_dn = find_key_idx(&out, KEY_DELETE, 1);
    assert(meta_up >= 0 && del_dn > meta_up && has_syn_between(&out, meta_up, del_dn));
    globe_sm_handle_key(&sm, KEY_DELETE, 0, MS(220), &out);

    /* Activate combo_modifier again with C, then press custom binding globe+f -> F11 stays pure! */
    globe_sm_handle_key(&sm, KEY_C, 1, MS(240), &out);
    assert(has_key(&out, KEY_LEFTMETA, 1));
    globe_sm_handle_key(&sm, KEY_C, 0, MS(260), &out);
    globe_sm_handle_key(&sm, KEY_F, 1, MS(280), &out);
    int meta_up2 = find_key_idx(&out, KEY_LEFTMETA, 0);
    int f11_dn = find_key_idx(&out, KEY_F11, 1);
    assert(meta_up2 >= 0 && f11_dn > meta_up2 && has_syn_between(&out, meta_up2, f11_dn));
    globe_sm_handle_key(&sm, KEY_F, 0, MS(300), &out);

    globe_sm_handle_key(&sm, KEY_FN, 0, MS(320), &out);
    assert_no_stuck_keys(&sm);
}

/* 7. Overlapping physical Super + combo_modifier (super_alt) preserves refcount cleanly. */
static void test_overlapping_mod_with_combo_modifier(void) {
    globe_config_t cfg;
    globe_config_defaults(&cfg);
    assert(gt_combo_modifier_parse("super_alt", &cfg.combo_modifier));

    globe_sm_t sm;
    globe_out_batch_t out;
    globe_sm_init(&sm, &cfg);

    globe_sm_handle_key(&sm, KEY_LEFTMETA, 1, MS(100), &out);
    assert(has_key(&out, KEY_LEFTMETA, 1));
    globe_sm_handle_key(&sm, KEY_FN, 1, MS(120), &out);
    globe_sm_handle_key(&sm, KEY_C, 1, MS(140), &out);
    /* KEY_LEFTMETA already held (refcount 1 -> 2), so no duplicate DOWN; ALT + C emit DOWN. */
    assert(!has_key(&out, KEY_LEFTMETA, 1));
    assert(has_key(&out, KEY_LEFTALT, 1));
    assert(has_key(&out, KEY_C, 1));

    /* Release Fn -> ALT releases, LEFTMETA stays held (refcount 2 -> 1). */
    globe_sm_handle_key(&sm, KEY_FN, 0, MS(180), &out);
    assert(has_key(&out, KEY_LEFTALT, 0));
    assert(!has_key(&out, KEY_LEFTMETA, 0));

    globe_sm_handle_key(&sm, KEY_C, 0, MS(200), &out);
    assert(has_key(&out, KEY_C, 0));
    globe_sm_handle_key(&sm, KEY_LEFTMETA, 0, MS(220), &out);
    assert(has_key(&out, KEY_LEFTMETA, 0));
    assert_no_stuck_keys(&sm);
}

/* 8. Custom bindings: IPC action, key remap, and trigger-modifier masking & restoration. */
static void test_custom_bindings_and_mod_masking(void) {
    globe_config_t cfg;
    globe_config_defaults(&cfg);
    /* globe+e -> action:emoji */
    cfg.bindings[0].trigger_key = KEY_E;
    cfg.bindings[0].req_mods = MOD_NONE;
    cfg.bindings[0].kind = GLOBE_BIND_IPC;
    strcpy(cfg.bindings[0].action, "action:emoji");
    /* globe+f -> key:f11 */
    cfg.bindings[1].trigger_key = KEY_F;
    cfg.bindings[1].req_mods = MOD_NONE;
    cfg.bindings[1].kind = GLOBE_BIND_KEY;
    assert(gt_chord_parse("f11", &cfg.bindings[1].out_chord));
    /* globe+ctrl+left -> key:leftmeta+left */
    cfg.bindings[2].trigger_key = KEY_LEFT;
    cfg.bindings[2].req_mods = MOD_CTRL;
    cfg.bindings[2].kind = GLOBE_BIND_KEY;
    assert(gt_chord_parse("leftmeta+left", &cfg.bindings[2].out_chord));
    cfg.binding_count = 3;

    globe_sm_t sm;
    globe_out_batch_t out;
    globe_sm_init(&sm, &cfg);

    /* Globe + E (IPC action): raw E suppressed on down, repeat, and up. */
    globe_sm_handle_key(&sm, KEY_FN, 1, MS(100), &out);
    globe_sm_handle_key(&sm, KEY_E, 1, MS(130), &out);
    assert(count_keys(&out) == 0);
    assert(has_ipc(&out, GLOBE_OUT_IPC_CHORD));
    globe_sm_handle_key(&sm, KEY_E, 2, MS(200), &out);
    assert(out.count == 0);
    globe_sm_handle_key(&sm, KEY_FN, 0, MS(220), &out);
    assert(!has_ipc(&out, GLOBE_OUT_IPC_TAP));
    globe_sm_handle_key(&sm, KEY_E, 0, MS(250), &out);
    assert(count_keys(&out) == 0);
    assert_no_stuck_keys(&sm);

    /* Globe + F (key remap): emits F11 down, repeat, and F11 up even if Fn releases first. */
    globe_sm_handle_key(&sm, KEY_FN, 1, MS(500), &out);
    globe_sm_handle_key(&sm, KEY_F, 1, MS(520), &out);
    assert(has_key(&out, KEY_F11, 1));
    assert(!has_key(&out, KEY_F, 1));
    globe_sm_handle_key(&sm, KEY_F, 2, MS(560), &out);
    assert(has_key(&out, KEY_F11, 2));
    globe_sm_handle_key(&sm, KEY_FN, 0, MS(580), &out);
    assert(!has_ipc(&out, GLOBE_OUT_IPC_TAP));
    globe_sm_handle_key(&sm, KEY_F, 0, MS(600), &out);
    assert(has_key(&out, KEY_F11, 0));
    assert_no_stuck_keys(&sm);

    /* Globe + Ctrl + Left -> masks LEFTCTRL, emits LEFTMETA+LEFT, restores LEFTCTRL on release! */
    globe_sm_handle_key(&sm, KEY_FN, 1, MS(900), &out);
    globe_sm_handle_key(&sm, KEY_LEFTCTRL, 1, MS(920), &out);
    assert(has_key(&out, KEY_LEFTCTRL, 1));
    globe_sm_handle_key(&sm, KEY_LEFT, 1, MS(940), &out);
    int ctrl_up = find_key_idx(&out, KEY_LEFTCTRL, 0);
    int meta_dn = find_key_idx(&out, KEY_LEFTMETA, 1);
    int left_dn = find_key_idx(&out, KEY_LEFT, 1);
    assert(ctrl_up >= 0 && meta_dn > ctrl_up && left_dn > meta_dn);
    assert(has_syn_between(&out, ctrl_up, meta_dn));

    globe_sm_handle_key(&sm, KEY_LEFT, 0, MS(970), &out);
    int left_up = find_key_idx(&out, KEY_LEFT, 0);
    int meta_up = find_key_idx(&out, KEY_LEFTMETA, 0);
    int ctrl_restore = find_key_idx(&out, KEY_LEFTCTRL, 1);
    assert(left_up >= 0 && meta_up > left_up && ctrl_restore > meta_up);
    assert(has_syn_between(&out, meta_up, ctrl_restore));

    globe_sm_handle_key(&sm, KEY_LEFTCTRL, 0, MS(980), &out);
    assert(has_key(&out, KEY_LEFTCTRL, 0));
    globe_sm_handle_key(&sm, KEY_FN, 0, MS(990), &out);
    assert(!has_ipc(&out, GLOBE_OUT_IPC_TAP));
    assert_no_stuck_keys(&sm);

    /* Edge case: release & re-press LEFTCTRL while LEFT is held -> no refcount leak! */
    globe_sm_handle_key(&sm, KEY_FN, 1, MS(1200), &out);
    globe_sm_handle_key(&sm, KEY_LEFTCTRL, 1, MS(1210), &out);
    globe_sm_handle_key(&sm, KEY_LEFT, 1, MS(1220), &out);
    assert(has_key(&out, KEY_LEFTCTRL, 0));
    globe_sm_handle_key(&sm, KEY_LEFTCTRL, 0, MS(1230), &out);
    globe_sm_handle_key(&sm, KEY_LEFTCTRL, 1, MS(1240), &out);
    assert(has_key(&out, KEY_LEFTCTRL, 1));
    globe_sm_handle_key(&sm, KEY_LEFT, 0, MS(1250), &out);
    /* Must NOT double-press LEFTCTRL because it's a new physical press (phys_seq changed). */
    assert(!has_key(&out, KEY_LEFTCTRL, 1));
    globe_sm_handle_key(&sm, KEY_LEFTCTRL, 0, MS(1260), &out);
    assert(has_key(&out, KEY_LEFTCTRL, 0));
    globe_sm_handle_key(&sm, KEY_FN, 0, MS(1270), &out);
    assert_no_stuck_keys(&sm);
}

/* 9. Modifier held BEFORE pressing Globe cancels solo Globe tap. */
static void test_modifier_before_globe_cancels_tap(void) {
    globe_sm_t sm;
    globe_out_batch_t out;
    globe_sm_init(&sm, NULL);

    globe_sm_handle_key(&sm, KEY_LEFTSHIFT, 1, MS(100), &out);
    globe_sm_handle_key(&sm, KEY_FN, 1, MS(120), &out);
    globe_sm_handle_key(&sm, KEY_FN, 0, MS(160), &out);
    assert(!has_ipc(&out, GLOBE_OUT_IPC_TAP));
    globe_sm_handle_key(&sm, KEY_LEFTSHIFT, 0, MS(180), &out);
    assert_no_stuck_keys(&sm);
}

/* 10. Pointer click and pointer scroll while holding Globe both cancel solo tap. */
static void test_pointer_click_and_scroll_cancel_tap(void) {
    globe_sm_t sm;
    globe_out_batch_t out;
    globe_sm_init(&sm, NULL);

    /* Click cancels */
    globe_sm_handle_key(&sm, KEY_FN, 1, MS(100), &out);
    globe_sm_handle_pointer_btn(&sm, BTN_LEFT, 1, MS(130), &out);
    assert(has_ipc(&out, GLOBE_OUT_IPC_CANCELLED));
    globe_sm_handle_pointer_btn(&sm, BTN_LEFT, 0, MS(150), &out);
    globe_sm_handle_key(&sm, KEY_FN, 0, MS(180), &out);
    assert(!has_ipc(&out, GLOBE_OUT_IPC_TAP));

    /* Scroll cancels */
    globe_sm_handle_key(&sm, KEY_FN, 1, MS(400), &out);
    globe_sm_handle_pointer_scroll(&sm, REL_WHEEL_HI_RES, -120, MS(430), &out);
    assert(has_ipc(&out, GLOBE_OUT_IPC_CANCELLED));
    globe_sm_handle_key(&sm, KEY_FN, 0, MS(460), &out);
    assert(!has_ipc(&out, GLOBE_OUT_IPC_TAP));
    assert_no_stuck_keys(&sm);
}

/* 11. Bilingual typing rollover: lifting a key pressed BEFORE Fn does not break Globe tap
 *     when rollover_safe=true, and DOES cancel when rollover_safe=false. */
static void test_bilingual_typing_rollover(void) {
    globe_config_t cfg;
    globe_config_defaults(&cfg);
    cfg.rollover_safe = true;

    globe_sm_t sm;
    globe_out_batch_t out;
    globe_sm_init(&sm, &cfg);

    globe_sm_handle_key(&sm, KEY_A, 1, MS(100), &out);
    globe_sm_handle_key(&sm, KEY_FN, 1, MS(120), &out);
    globe_sm_handle_key(&sm, KEY_A, 0, MS(135), &out);
    assert(has_key(&out, KEY_A, 0));
    globe_sm_handle_key(&sm, KEY_FN, 0, MS(190), &out);
    assert(has_ipc(&out, GLOBE_OUT_IPC_TAP));
    assert_no_stuck_keys(&sm);

    /* Strict rollover_safe=false cancels on any key release during Globe hold. */
    cfg.rollover_safe = false;
    globe_sm_init(&sm, &cfg);
    globe_sm_handle_key(&sm, KEY_A, 1, MS(500), &out);
    globe_sm_handle_key(&sm, KEY_FN, 1, MS(520), &out);
    globe_sm_handle_key(&sm, KEY_A, 0, MS(535), &out);
    globe_sm_handle_key(&sm, KEY_FN, 0, MS(590), &out);
    assert(!has_ipc(&out, GLOBE_OUT_IPC_TAP));
    assert_no_stuck_keys(&sm);
}

/* 12. Optional double-tap mode: single tap commits after window, double tap fires secondary action. */
static void test_double_tap_mode(void) {
    globe_config_t cfg;
    globe_config_defaults(&cfg);
    cfg.tap_mode = GLOBE_TAP_LAYOUT;
    cfg.double_tap_mode = GLOBE_TAP_EMOJI;
    cfg.double_tap_window_ms = 300;

    globe_sm_t sm;
    globe_out_batch_t out;
    globe_sm_init(&sm, &cfg);

    /* Single tap -> deferred until 300ms tick */
    globe_sm_handle_key(&sm, KEY_FN, 1, MS(1000), &out);
    globe_sm_handle_key(&sm, KEY_FN, 0, MS(1080), &out);
    assert(!has_ipc(&out, GLOBE_OUT_IPC_TAP));
    int32_t rem = globe_sm_tick(&sm, MS(1200), &out);
    assert(rem == 180 && out.count == 0);
    rem = globe_sm_tick(&sm, MS(1380), &out);
    assert(rem == -1 && has_ipc(&out, GLOBE_OUT_IPC_TAP));

    /* Double tap within 300ms -> fires DOUBLE_TAP, never single TAP. */
    globe_sm_handle_key(&sm, KEY_FN, 1, MS(2000), &out);
    globe_sm_handle_key(&sm, KEY_FN, 0, MS(2060), &out);
    assert(!has_ipc(&out, GLOBE_OUT_IPC_TAP));
    globe_sm_handle_key(&sm, KEY_FN, 1, MS(2150), &out);
    globe_sm_handle_key(&sm, KEY_FN, 0, MS(2220), &out);
    assert(has_ipc(&out, GLOBE_OUT_IPC_DOUBLE_TAP));
    assert(!has_ipc(&out, GLOBE_OUT_IPC_TAP));
    assert(globe_sm_tick(&sm, MS(3000), &out) == -1);
    assert_no_stuck_keys(&sm);
}

/* 13. Panic release cleanly clears all held keys and combo modifiers. */
static void test_panic_release_all(void) {
    globe_config_t cfg;
    globe_config_defaults(&cfg);
    assert(gt_combo_modifier_parse("super_alt", &cfg.combo_modifier));

    globe_sm_t sm;
    globe_out_batch_t out;
    globe_sm_init(&sm, &cfg);

    globe_sm_handle_key(&sm, KEY_FN, 1, MS(100), &out);
    globe_sm_handle_key(&sm, KEY_C, 1, MS(120), &out);
    globe_sm_release_all(&sm, &out);
    assert(has_key(&out, KEY_LEFTMETA, 0));
    assert(has_key(&out, KEY_LEFTALT, 0));
    assert(has_key(&out, KEY_C, 0));
    assert_no_stuck_keys(&sm);
}

/* 14. More held keys than one output batch can carry: nothing is dropped, the
 *     remainder is released by repeated calls, and every batch ends in SYN. */
static void test_release_all_overflow_is_chunked(void) {
    globe_sm_t sm;
    globe_out_batch_t out;
    globe_sm_init(&sm, NULL);

    size_t held = 0;
    for (uint16_t k = KEY_ESC; k < KEY_ESC + 100; k++) {
        if (gt_keycode_mod_bit(k) != MOD_NONE)
            continue;
        globe_sm_handle_key(&sm, k, 1, MS(10 + k), &out);
        held++;
    }
    assert(held > GT_MAX_OUT_EVENTS);

    size_t released = 0;
    int rounds = 0;
    do {
        globe_sm_release_all(&sm, &out);
        assert(out.count <= GT_MAX_OUT_EVENTS);
        assert(out.count > 0 && out.events[out.count - 1].type == GLOBE_OUT_SYN);
        released += count_keys(&out);
        rounds++;
    } while (globe_sm_has_virtual_keys(&sm) && rounds < 10);

    assert(rounds == 2);
    assert(released == held);
    assert_no_stuck_keys(&sm);
}

int main(void) {
    printf("Running globetrot state machine tests...\n");
    RUN_TEST(test_solo_tap_layout_within_default_ceiling);
    RUN_TEST(test_solo_hold_debounced_over_timeout);
    RUN_TEST(test_solo_tap_key_mode_syn_framing);
    RUN_TEST(test_solo_tap_chord_mode_syn_framing);
    RUN_TEST(test_fn_layer_outputs_stay_pure);
    RUN_TEST(test_combo_modifier_and_early_fn_release);
    RUN_TEST(test_combo_modifier_deactivates_for_fn_layer_and_bindings);
    RUN_TEST(test_overlapping_mod_with_combo_modifier);
    RUN_TEST(test_custom_bindings_and_mod_masking);
    RUN_TEST(test_modifier_before_globe_cancels_tap);
    RUN_TEST(test_pointer_click_and_scroll_cancel_tap);
    RUN_TEST(test_bilingual_typing_rollover);
    RUN_TEST(test_double_tap_mode);
    RUN_TEST(test_panic_release_all);
    RUN_TEST(test_release_all_overflow_is_chunked);
    printf("All %d state machine tests passed!\n", g_tests_run);
    return 0;
}

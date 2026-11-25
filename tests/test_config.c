/* SPDX-License-Identifier: MIT */
#include "../src/globe_config.h"
#include "../src/keycodes.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_defaults(void) {
    globe_config_t cfg;
    globe_config_defaults(&cfg);
    assert(cfg.globe_keycode == KEY_FN);
    assert(cfg.tap_mode == GLOBE_TAP_LAYOUT);
    assert(cfg.tap_timeout_ms == 400);
    assert(cfg.tap_chord.count == 1 && cfg.tap_chord.keys[0] == KEY_F24);
    assert(cfg.double_tap_chord.count == 1 && cfg.double_tap_chord.keys[0] == KEY_F19);
    assert(cfg.rollover_safe == true);
    assert(cfg.cancel_on_pointer == true);
}

static void test_full_config_parse(void) {
    const char *sample =
        "# Sample config\n"
        "[globe]\n"
        "tap_mode = both\n"
        "tap_timeout_ms = 300\n"
        "tap_key = leftmeta+space\n"
        "double_tap_mode = emoji\n"
        "double_tap_window_ms = 320\n"
        "combo_modifier = super_alt\n"
        "rollover_safe = false\n"
        "cancel_on_pointer = true\n"
        "\n"
        "[bindings]\n"
        "globe+e = action:emoji\n"
        "globe+f = key:f11\n"
        "globe+ctrl+left = key:leftmeta+left\n"
        "globe+q = exec:kwrite --new\n"
        "\n"
        "[session]\n"
        "backend = kwin\n"
        "mru_commit_ms = 1000\n";

    globe_config_t cfg;
    char err[256] = {0};
    bool ok = globe_config_parse_string(sample, &cfg, false, err, sizeof(err));
    if (!ok) {
        fprintf(stderr, "Unexpected parse error: %s\n", err);
    }
    assert(ok);
    assert(cfg.tap_mode == GLOBE_TAP_BOTH);
    assert(cfg.tap_timeout_ms == 300);
    assert(cfg.tap_chord.count == 2);
    assert(cfg.tap_chord.keys[0] == KEY_LEFTMETA);
    assert(cfg.tap_chord.keys[1] == KEY_SPACE);
    assert(cfg.double_tap_mode == GLOBE_TAP_EMOJI);
    assert(cfg.double_tap_window_ms == 320);
    assert(cfg.combo_modifier.count == 2);
    assert(cfg.combo_modifier.keys[0] == KEY_LEFTMETA);
    assert(cfg.combo_modifier.keys[1] == KEY_LEFTALT);
    assert(cfg.rollover_safe == false);
    assert(cfg.cancel_on_pointer == true);
    assert(cfg.binding_count == 4);

    assert(cfg.bindings[0].trigger_key == KEY_E && cfg.bindings[0].kind == GLOBE_BIND_IPC);
    assert(strcmp(cfg.bindings[0].action, "action:emoji") == 0);
    assert(cfg.bindings[1].trigger_key == KEY_F && cfg.bindings[1].kind == GLOBE_BIND_KEY);
    assert(cfg.bindings[1].out_chord.keys[0] == KEY_F11);
    assert(cfg.bindings[2].trigger_key == KEY_LEFT && cfg.bindings[2].req_mods == MOD_CTRL);
    assert(cfg.bindings[3].trigger_key == KEY_Q && strcmp(cfg.bindings[3].action, "exec:kwrite --new") == 0);
}

static void test_invalid_configs_rejected(void) {
    globe_config_t cfg;
    char err[256] = {0};

    assert(!globe_config_parse_string("[globe]\ntap_timeout_ms = 10\n", &cfg, false, err, sizeof(err)));
    assert(strstr(err, "50..2000") != NULL);

    assert(!globe_config_parse_string("[globe]\ntap_timeout_ms = 9999\n", &cfg, false, err, sizeof(err)));
    assert(!globe_config_parse_string("[globe]\ntap_mode = bogus\n", &cfg, false, err, sizeof(err)));
    assert(!globe_config_parse_string("[globe]\nunknown_key = 1\n", &cfg, false, err, sizeof(err)));
    assert(!globe_config_parse_string("[bindings]\nglobe+e = invalid_target\n", &cfg, false, err, sizeof(err)));
    assert(!globe_config_parse_string("[bindings]\nglobe+e = dictation\n", &cfg, false, err, sizeof(err)));
    assert(!globe_config_parse_string("[bindings]\nglobe+a+b = key:f11\n", &cfg, false, err, sizeof(err)));
    assert(!globe_config_parse_string("[bindings]\nglobe+q = exec:\n", &cfg, false, err, sizeof(err)));
    assert(!globe_config_parse_string("[bindings]\nglobe+q = exec:   \n", &cfg, false, err, sizeof(err)));

    /* Over-long commands are rejected, never silently truncated. */
    char longcfg[400];
    char cmd[200];
    memset(cmd, 'x', sizeof(cmd) - 1);
    cmd[sizeof(cmd) - 1] = '\0';
    snprintf(longcfg, sizeof(longcfg), "[globe]\ntap_command = %s\n", cmd);
    assert(!globe_config_parse_string(longcfg, &cfg, false, err, sizeof(err)));
    assert(strstr(err, "127") != NULL);
    snprintf(longcfg, sizeof(longcfg), "[globe]\ndouble_tap_command = %s\n", cmd);
    assert(!globe_config_parse_string(longcfg, &cfg, false, err, sizeof(err)));
}

static void test_shipped_example_config_parses(void) {
    globe_config_t cfg;
    char err[256] = {0};
    bool ok = globe_config_load_file("config/globetrot.conf", &cfg, false, err, sizeof(err));
    if (!ok)
        fprintf(stderr, "example config: %s\n", err);
    assert(ok);
    assert(cfg.tap_mode == GLOBE_TAP_LAYOUT);
    assert(cfg.combo_modifier.count == 0);
    assert(cfg.binding_count == 0);
}

static void test_keycode_helpers(void) {
    uint16_t code = 0;
    assert(gt_keycode_from_name("KEY_FN", &code) && code == KEY_FN);
    assert(gt_keycode_from_name("globe", &code) && code == KEY_FN);
    assert(gt_keycode_from_name("F24", &code) && code == KEY_F24);
    assert(gt_keycode_from_name("194", &code) && code == KEY_F24);
    assert(gt_keycode_from_name("0x1d0", &code) && code == KEY_FN);
    assert(!gt_keycode_from_name("nonexistent_key", &code));

    assert(gt_keycode_is_fn_layer_key(KEY_DELETE));
    /* hid-apple: Fn+Return -> Insert, Fn+F5 (media mode) -> MicMute. */
    assert(gt_keycode_is_fn_layer_key(KEY_INSERT));
    assert(gt_keycode_is_fn_layer_key(KEY_MICMUTE));
    assert(gt_keycode_is_fn_layer_key(KEY_F12));
    assert(!gt_keycode_is_fn_layer_key(KEY_C));
    assert(gt_rel_is_scroll_axis(REL_WHEEL));
    assert(gt_rel_is_scroll_axis(REL_WHEEL_HI_RES));
    assert(!gt_rel_is_scroll_axis(0));

    gt_key_chord_t ch;
    assert(gt_combo_modifier_parse("hyper", &ch) && ch.count == 1 && ch.keys[0] == KEY_F18);
    /* F21..F23 are touchpad toggle/on/off keysyms in stock xkb: no preset may emit them. */
    assert(gt_combo_modifier_parse("mod3", &ch) && ch.count == 1 && ch.keys[0] == KEY_F18);
    assert(gt_combo_modifier_parse("hyper_chord", &ch) && ch.count == 4);
    assert(gt_combo_modifier_parse("none", &ch) && ch.count == 0);
}

int main(void) {
    printf("Running globetrot config & keycode tests...\n");
    test_defaults();
    printf("  [PASS] test_defaults\n");
    test_full_config_parse();
    printf("  [PASS] test_full_config_parse\n");
    test_invalid_configs_rejected();
    printf("  [PASS] test_invalid_configs_rejected\n");
    test_keycode_helpers();
    printf("  [PASS] test_keycode_helpers\n");
    test_shipped_example_config_parses();
    printf("  [PASS] test_shipped_example_config_parses\n");
    printf("All config tests passed!\n");
    return 0;
}

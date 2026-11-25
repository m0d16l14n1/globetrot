/* SPDX-License-Identifier: MIT
 * Deterministic Fn / Globe dual-role key state machine for Linux evdev/uinput.
 *
 * Design guarantees:
 *   - Solo tap vs hold debounce ceiling (default 400 ms, range 50..2000 ms)
 *   - Immediate solo-tap cancellation on concurrent keys, held modifiers,
 *     pointer clicks, or pointer scroll events
 *   - Optional bilingual typing rollover tolerance (ignores release of keys
 *     pressed before Globe went down)
 *   - Pure kernel Fn-layer passthrough (F1..F12, media, Home/End/PgUp/PgDn/Del)
 *   - Modifier-masked custom Globe+[mods+]<key> chord remappings (no leaked trigger mods)
 *   - Universal combo_modifier emission for native desktop shortcut binding
 *   - Explicit GLOBE_OUT_SYN framing so synthesized taps never collapse in libinput
 *   - Stuck-key immunity via per-key press provenance and synthetic refcounting
 */
#ifndef GLOBETROT_SM_H
#define GLOBETROT_SM_H

#include "keycodes.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GT_MAX_BINDINGS         64
#define GT_MAX_ACTION_LEN       128
#define GT_MAX_OUT_EVENTS       64
#define GT_MAX_MASKED_MODS      8

#define GT_DEFAULT_TAP_TIMEOUT_MS   400u
#define GT_MIN_TAP_TIMEOUT_MS       50u
#define GT_MAX_TAP_TIMEOUT_MS       2000u
#define GT_DEFAULT_DOUBLE_TAP_MS    350u
#define GT_DEFAULT_TAP_KEYCODE      KEY_F24
/* F19 keeps its plain keysym in stock xkb; F21..F23 are touchpad on/off keys. */
#define GT_DEFAULT_DOUBLE_TAP_KEYCODE KEY_F19

typedef enum {
    GLOBE_TAP_DISABLED = 0,
    GLOBE_TAP_LAYOUT   = 1,
    GLOBE_TAP_KEY      = 2,
    GLOBE_TAP_BOTH     = 3,
    GLOBE_TAP_EMOJI    = 4,
    GLOBE_TAP_COMMAND  = 5,
} globe_tap_mode_t;

typedef enum {
    GLOBE_BIND_NONE = 0,
    GLOBE_BIND_KEY  = 1,
    GLOBE_BIND_IPC  = 2,
} globe_bind_kind_t;

typedef struct {
    uint16_t           trigger_key;
    uint8_t            req_mods;
    globe_bind_kind_t  kind;
    gt_key_chord_t out_chord;
    char               action[GT_MAX_ACTION_LEN];
} globe_binding_t;

typedef struct {
    uint16_t           globe_keycode;
    globe_tap_mode_t   tap_mode;
    uint32_t           tap_timeout_ms;
    gt_key_chord_t tap_chord;
    char               tap_command[GT_MAX_ACTION_LEN];

    globe_tap_mode_t   double_tap_mode;
    uint32_t           double_tap_window_ms;
    gt_key_chord_t double_tap_chord;
    char               double_tap_command[GT_MAX_ACTION_LEN];

    gt_key_chord_t combo_modifier;
    bool               pass_fn_raw;
    bool               rollover_safe;
    bool               cancel_on_pointer;

    globe_binding_t    bindings[GT_MAX_BINDINGS];
    size_t             binding_count;
} globe_config_t;

typedef enum {
    GLOBE_OUT_KEY            = 1, /* EV_KEY (code, value: 0/1/2) */
    GLOBE_OUT_SYN            = 2, /* EV_SYN SYN_REPORT boundary */
    GLOBE_OUT_IPC_TAP        = 3, /* Solo Globe tap dispatched */
    GLOBE_OUT_IPC_DOUBLE_TAP = 4, /* Double-tap Globe dispatched */
    GLOBE_OUT_IPC_DEBOUNCED  = 5, /* Solo Globe held past tap_timeout_ms */
    GLOBE_OUT_IPC_CANCELLED  = 6, /* Solo Globe cancelled by key/pointer */
    GLOBE_OUT_IPC_CHORD      = 7, /* Custom Globe+<key> IPC action */
} globe_out_type_t;

typedef struct {
    globe_out_type_t type;
    uint16_t         code;
    int32_t          value;
    uint32_t         elapsed_ms;
    globe_tap_mode_t tap_mode;
    uint16_t         trigger_key;
    char             action[GT_MAX_ACTION_LEN];
} globe_out_event_t;

typedef struct {
    globe_out_event_t events[GT_MAX_OUT_EVENTS];
    size_t            count;
} globe_out_batch_t;

typedef enum {
    KEY_PROV_NONE        = 0,
    KEY_PROV_PASSTHROUGH = 1,
    KEY_PROV_COMBO_MOD   = 2,
    KEY_PROV_REMAP_CHORD = 3,
    KEY_PROV_SUPPRESSED  = 4,
} key_provenance_t;

typedef struct {
    uint16_t keys[GT_MAX_MASKED_MODS];
    uint8_t  seq[GT_MAX_MASKED_MODS];
    uint8_t  count;
} masked_mods_t;

typedef struct {
    globe_config_t cfg;

    bool           globe_down;
    bool           pending_tap;
    uint64_t       globe_down_ns;
    bool           combo_mod_active;

    bool           awaiting_double_tap;
    uint64_t       first_tap_up_ns;
    uint32_t       first_tap_elapsed_ms;

    uint8_t        phys_down[GT_MAX_KEYCODE];
    uint8_t        phys_seq[GT_MAX_KEYCODE];
    uint8_t        pressed_before_globe[GT_MAX_KEYCODE];
    uint8_t        prov[GT_MAX_KEYCODE];
    int16_t        remap_binding_idx[GT_MAX_KEYCODE];
    masked_mods_t  masked_mods[GT_MAX_KEYCODE];

    uint8_t        virt_refcount[GT_MAX_KEYCODE];
} globe_sm_t;

void globe_config_defaults(globe_config_t *cfg);
void globe_sm_init(globe_sm_t *sm, const globe_config_t *cfg);

void globe_sm_handle_key(globe_sm_t *sm, uint16_t code, int32_t value,
                         uint64_t ts_ns, globe_out_batch_t *out);

void globe_sm_handle_pointer_btn(globe_sm_t *sm, uint16_t btn_code, int32_t value,
                                 uint64_t ts_ns, globe_out_batch_t *out);

void globe_sm_handle_pointer_scroll(globe_sm_t *sm, uint16_t axis, int32_t delta,
                                    uint64_t ts_ns, globe_out_batch_t *out);

int32_t globe_sm_tick(globe_sm_t *sm, uint64_t now_ns, globe_out_batch_t *out);

/* Resets all key state and releases held virtual keys. One batch holds at most
 * GT_MAX_OUT_EVENTS events; keys that did not fit stay counted, so callers
 * repeat the call while globe_sm_has_virtual_keys() is true. */
void globe_sm_release_all(globe_sm_t *sm, globe_out_batch_t *out);
bool globe_sm_has_virtual_keys(const globe_sm_t *sm);
uint8_t globe_sm_active_mods(const globe_sm_t *sm);

#endif /* GLOBETROT_SM_H */

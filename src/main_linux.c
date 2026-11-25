/* SPDX-License-Identifier: MIT
 * globetrotd: Linux evdev/uinput daemon for dual-role Fn / Globe key handling.
 *
 * - Discovers keyboards exposing both standard alphanumeric keys (KEY_A) and
 *   the configured globe_keycode (default KEY_FN = 464), or explicit --device.
 * - Uses EVIOCSCLOCKID(CLOCK_MONOTONIC) for jitter-free kernel event timestamps.
 * - Grabs matched keyboards via EVIOCGRAB and exposes a clean virtual uinput
 *   keyboard with LED forwarding (Caps Lock LED) and strict SYN_REPORT framing.
 * - Passively observes pointer/touchpad devices (read-only, no grab) so clicks
 *   and scroll gestures while holding Fn cancel pending solo taps.
 * - Watches /dev/input via inotify for hotplug and suspend/resume recovery.
 * - Broadcasts JSONL events over /run/globetrot/events.sock and writes live
 *   status to /run/globetrot/status.json.
 */
#define _GNU_SOURCE

#include "globe_config.h"
#include "globe_sm.h"
#include "keycodes.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define MAX_EVENTS          32
#define MAX_KBD_DEVS        8
#define MAX_PTR_DEVS        16
#define MAX_IPC_CLIENTS     16
#define VIRTUAL_KBD_NAME    "Globetrot Virtual Keyboard"

#define BITS_PER_LONG       (sizeof(unsigned long) * 8)
#define NLONGS(x)           (((x) + BITS_PER_LONG - 1) / BITS_PER_LONG)
#define TEST_BIT(bit, arr)  (((arr)[(bit) / BITS_PER_LONG] >> ((bit) % BITS_PER_LONG)) & 1UL)

typedef struct {
    int  fd;
    char path[300];
    char name[128];
} dev_slot_t;

typedef struct {
    int         epoll_fd;
    int         uinput_fd;
    int         inotify_fd;
    int         ipc_listen_fd;
    int         ipc_clients[MAX_IPC_CLIENTS];

    dev_slot_t  kbds[MAX_KBD_DEVS];
    size_t      kbd_count;

    dev_slot_t  ptrs[MAX_PTR_DEVS];
    size_t      ptr_count;

    globe_sm_t  sm;
    const char *config_path;
    const char *socket_path;
    const char *status_path;
    const char *explicit_device;
    bool        verbose;
} daemon_t;

static volatile sig_atomic_t g_stop = 0;
static volatile sig_atomic_t g_reload = 0;

static void handle_signal(int sig) {
    if (sig == SIGHUP)
        g_reload = 1;
    else
        g_stop = 1;
}

static uint64_t monotonic_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static uint64_t input_ev_ts_ns(const struct input_event *ev) {
    return (uint64_t)ev->input_event_sec * 1000000000ULL +
           (uint64_t)ev->input_event_usec * 1000ULL;
}

static void log_info(const char *fmt, ...) {
    va_list ap;
    fprintf(stderr, "globetrotd: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
}

static void epoll_add_fd(int epfd, int fd, uint32_t events) {
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = events;
    ev.data.fd = fd;
    (void)epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);
}

static void epoll_del_fd(int epfd, int fd) {
    (void)epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
}

/* Escape for a JSON string value. UTF-8 bytes pass through unchanged (JSON
 * text is UTF-8); control characters become \u00XX. Output that does not fit
 * dst is cut off; callers size dst for the longest configured action. */
static void json_escape(const char *src, char *dst, size_t dst_len) {
    static const char hex[] = "0123456789abcdef";
    size_t o = 0;
    if (dst_len == 0)
        return;
    for (size_t i = 0; src && src[i]; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '"' || c == '\\') {
            if (o + 2 >= dst_len)
                break;
            dst[o++] = '\\';
            dst[o++] = (char)c;
        } else if (c < 0x20 || c == 0x7f) {
            if (o + 6 >= dst_len)
                break;
            dst[o++] = '\\';
            dst[o++] = 'u';
            dst[o++] = '0';
            dst[o++] = '0';
            dst[o++] = hex[(c >> 4) & 0xf];
            dst[o++] = hex[c & 0xf];
        } else {
            if (o + 1 >= dst_len)
                break;
            dst[o++] = (char)c;
        }
    }
    dst[o] = '\0';
}

static size_t count_ipc_clients(const daemon_t *d) {
    size_t c = 0;
    for (int i = 0; i < MAX_IPC_CLIENTS; i++) {
        if (d->ipc_clients[i] >= 0)
            c++;
    }
    return c;
}

static void write_status_file(const daemon_t *d) {
    if (!d->status_path || !*d->status_path)
        return;
    char tmp[320];
    snprintf(tmp, sizeof(tmp), "%s.tmp", d->status_path);
    FILE *f = fopen(tmp, "w");
    if (!f)
        return;

    fprintf(f, "{\n");
    fprintf(f, "  \"tap_mode\": \"%s\",\n", globe_tap_mode_to_name(d->sm.cfg.tap_mode));
    fprintf(f, "  \"tap_timeout_ms\": %u,\n", d->sm.cfg.tap_timeout_ms);
    fprintf(f, "  \"bindings\": %zu,\n", d->sm.cfg.binding_count);
    fprintf(f, "  \"ipc_clients\": %zu,\n", count_ipc_clients(d));
    fprintf(f, "  \"keyboards\": [");
    for (size_t i = 0; i < d->kbd_count; i++) {
        char ep[320], en[256];
        json_escape(d->kbds[i].path, ep, sizeof(ep));
        json_escape(d->kbds[i].name, en, sizeof(en));
        fprintf(f, "%s{\"path\":\"%s\",\"name\":\"%s\"}", i ? "," : "", ep, en);
    }
    fprintf(f, "],\n");
    fprintf(f, "  \"pointers\": [");
    for (size_t i = 0; i < d->ptr_count; i++) {
        char ep[320], en[256];
        json_escape(d->ptrs[i].path, ep, sizeof(ep));
        json_escape(d->ptrs[i].name, en, sizeof(en));
        fprintf(f, "%s{\"path\":\"%s\",\"name\":\"%s\"}", i ? "," : "", ep, en);
    }
    fprintf(f, "]\n}\n");
    fclose(f);
    (void)chmod(tmp, 0644);
    (void)rename(tmp, d->status_path);
}

static void close_ipc_client(daemon_t *d, int slot) {
    int cfd = d->ipc_clients[slot];
    if (cfd < 0)
        return;
    epoll_del_fd(d->epoll_fd, cfd);
    close(cfd);
    d->ipc_clients[slot] = -1;
}

static void ipc_broadcast(daemon_t *d, const char *line) {
    size_t len = strlen(line);
    bool dropped = false;
    for (int i = 0; i < MAX_IPC_CLIENTS; i++) {
        int cfd = d->ipc_clients[i];
        if (cfd < 0)
            continue;
        ssize_t n = send(cfd, line, len, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            close_ipc_client(d, i);
            dropped = true;
        }
    }
    if (dropped)
        write_status_file(d);
}

static void handle_ipc_client_event(daemon_t *d, int slot) {
    int cfd = d->ipc_clients[slot];
    if (cfd < 0)
        return;
    char drain[128];
    while (1) {
        ssize_t n = recv(cfd, drain, sizeof(drain), MSG_DONTWAIT);
        if (n > 0)
            continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return;
        /* EOF (n == 0) or socket error -> client disconnected. */
        close_ipc_client(d, slot);
        write_status_file(d);
        return;
    }
}

static void uinput_write_ev(int ufd, uint16_t type, uint16_t code, int32_t value) {
    struct input_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = type;
    ev.code = code;
    ev.value = value;
    ssize_t n;
    do {
        n = write(ufd, &ev, sizeof(ev));
    } while (n < 0 && errno == EINTR);
}

static void dispatch_batch(daemon_t *d, const globe_out_batch_t *batch, uint64_t ts_ns) {
    for (size_t i = 0; i < batch->count; i++) {
        const globe_out_event_t *e = &batch->events[i];
        if (e->type == GLOBE_OUT_KEY) {
            uinput_write_ev(d->uinput_fd, EV_KEY, e->code, e->value);
            if (d->verbose)
                log_info("uinput key %s (%u) val=%d", gt_keycode_to_name(e->code), e->code, e->value);
            continue;
        }
        if (e->type == GLOBE_OUT_SYN) {
            uinput_write_ev(d->uinput_fd, EV_SYN, SYN_REPORT, 0);
            continue;
        }

        char esc_action[256];
        json_escape(e->action, esc_action, sizeof(esc_action));
        char msg[512];
        const char *type_str = "unknown";
        switch (e->type) {
        case GLOBE_OUT_IPC_TAP:        type_str = "globe_tap"; break;
        case GLOBE_OUT_IPC_DOUBLE_TAP: type_str = "globe_double_tap"; break;
        case GLOBE_OUT_IPC_DEBOUNCED:  type_str = "globe_debounced"; break;
        case GLOBE_OUT_IPC_CANCELLED:  type_str = "globe_cancelled"; break;
        case GLOBE_OUT_IPC_CHORD:      type_str = "globe_chord"; break;
        default: break;
        }
        /* Do not leak unrelated regular keycodes over IPC unless verbose. */
        const char *key_field = (e->type == GLOBE_OUT_IPC_CANCELLED && !d->verbose)
                                ? ""
                                : gt_keycode_to_name(e->trigger_key);
        snprintf(msg, sizeof(msg),
                 "{\"type\":\"%s\",\"mode\":\"%s\",\"elapsed_ms\":%u,\"key\":\"%s\",\"action\":\"%s\",\"ts_ns\":%llu}\n",
                 type_str, globe_tap_mode_to_name(e->tap_mode), e->elapsed_ms,
                 key_field, esc_action, (unsigned long long)ts_ns);
        ipc_broadcast(d, msg);
        if (d->verbose)
            log_info("ipc: %.*s", (int)(strlen(msg) > 0 ? strlen(msg) - 1 : 0), msg);
    }
}

/* Release every held virtual key, in as many batches as it takes. */
static void release_all_keys(daemon_t *d) {
    for (int round = 0; round < 32; round++) {
        globe_out_batch_t batch;
        globe_sm_release_all(&d->sm, &batch);
        dispatch_batch(d, &batch, monotonic_ns());
        if (!globe_sm_has_virtual_keys(&d->sm))
            break;
    }
}

static int create_uinput_keyboard(void) {
    int fd = open("/dev/uinput", O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        log_info("ERROR: open(/dev/uinput) failed: %s (try: modprobe uinput)", strerror(errno));
        return -1;
    }

    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    ioctl(fd, UI_SET_EVBIT, EV_SYN);
    ioctl(fd, UI_SET_EVBIT, EV_LED);

    for (int led = 0; led <= LED_MAX; led++)
        ioctl(fd, UI_SET_LEDBIT, led);

    /* Expose keyboard/consumer/function keys while excluding all BTN_* ranges
     * (BTN_MISC..BTN_THUMBR 0x100..0x15f, BTN_DPAD 0x220..0x223,
     * BTN_TRIGGER_HAPPY 0x2c0..0x2e7) so udev/libinput classifies the virtual
     * device strictly as ID_INPUT_KEYBOARD with no mouse/joystick flags. */
    int max_k = (GT_MAX_KEYCODE <= KEY_MAX) ? GT_MAX_KEYCODE : (KEY_MAX + 1);
    for (int k = 1; k < max_k; k++) {
        if ((k >= 0x100 && k <= 0x15f) ||
            (k >= 0x220 && k <= 0x223) ||
            (k >= 0x2c0 && k <= 0x2e7))
            continue;
        ioctl(fd, UI_SET_KEYBIT, k);
    }

    struct uinput_setup usetup;
    memset(&usetup, 0, sizeof(usetup));
    usetup.id.bustype = BUS_VIRTUAL;
    usetup.id.vendor  = 0x1209;
    usetup.id.product = 0x0001;
    usetup.id.version = 1;
    snprintf(usetup.name, sizeof(usetup.name), "%s", VIRTUAL_KBD_NAME);

    if (ioctl(fd, UI_DEV_SETUP, &usetup) < 0 || ioctl(fd, UI_DEV_CREATE) < 0) {
        log_info("ERROR: UI_DEV_CREATE failed: %s", strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

static bool dev_already_tracked(const dev_slot_t *arr, size_t count, const char *path) {
    for (size_t i = 0; i < count; i++) {
        if (strcmp(arr[i].path, path) == 0)
            return true;
    }
    return false;
}

/* Capability-based keyboard matching: any keyboard with KEY_A and globe_keycode
 * (default KEY_FN) is matched automatically without vendor-name heuristics. */
static bool is_matching_keyboard(int fd, const char *name, uint16_t globe_keycode, bool is_explicit) {
    if (strcmp(name, VIRTUAL_KBD_NAME) == 0)
        return false;

    unsigned long ev_bits[NLONGS(EV_MAX + 1)] = {0};
    unsigned long key_bits[NLONGS(KEY_MAX + 1)] = {0};
    if (ioctl(fd, EVIOCGBIT(0, sizeof(ev_bits)), ev_bits) < 0)
        return false;
    if (!TEST_BIT(EV_KEY, ev_bits))
        return false;
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key_bits)), key_bits) < 0)
        return false;

    if (is_explicit)
        return true;

    return TEST_BIT(KEY_A, key_bits) && TEST_BIT(globe_keycode, key_bits);
}

static bool is_pointer_device(int fd, const char *name) {
    if (strcmp(name, VIRTUAL_KBD_NAME) == 0)
        return false;
    unsigned long ev_bits[NLONGS(EV_MAX + 1)] = {0};
    unsigned long key_bits[NLONGS(KEY_MAX + 1)] = {0};
    if (ioctl(fd, EVIOCGBIT(0, sizeof(ev_bits)), ev_bits) < 0)
        return false;
    if (!TEST_BIT(EV_KEY, ev_bits))
        return false;
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key_bits)), key_bits) < 0)
        return false;
    return TEST_BIT(BTN_LEFT, key_bits) != 0;
}

static void wait_until_keys_idle(int fd) {
    unsigned long key_state[NLONGS(KEY_MAX + 1)];
    for (int attempt = 0; attempt < 20; attempt++) {
        memset(key_state, 0, sizeof(key_state));
        if (ioctl(fd, EVIOCGKEY(sizeof(key_state)), key_state) < 0)
            return;
        bool any = false;
        for (size_t i = 0; i < NLONGS(KEY_MAX + 1); i++) {
            if (key_state[i] != 0) {
                any = true;
                break;
            }
        }
        if (!any)
            return;
        usleep(20000);
    }
}

static void enable_monotonic_clock(int fd) {
    int clkid = CLOCK_MONOTONIC;
    (void)ioctl(fd, EVIOCSCLOCKID, &clkid);
}

static void try_attach_device(daemon_t *d, const char *path) {
    if (dev_already_tracked(d->kbds, d->kbd_count, path) ||
        dev_already_tracked(d->ptrs, d->ptr_count, path))
        return;

    bool is_explicit = (d->explicit_device && strcmp(d->explicit_device, path) == 0);
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
        return;

    char name[128] = "unknown";
    (void)ioctl(fd, EVIOCGNAME(sizeof(name)), name);

    if ((!d->explicit_device || is_explicit) && d->kbd_count < MAX_KBD_DEVS &&
        is_matching_keyboard(fd, name, d->sm.cfg.globe_keycode, is_explicit)) {
        /* Upgrade keyboard fd to O_RDWR so Caps Lock EV_LED writes succeed. */
        int rwfd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (rwfd >= 0) {
            close(fd);
            fd = rwfd;
        }
        wait_until_keys_idle(fd);
        if (ioctl(fd, EVIOCGRAB, (void *)1) < 0) {
            log_info("WARN: EVIOCGRAB failed on %s (%s): %s", path, name, strerror(errno));
            close(fd);
            return;
        }
        enable_monotonic_clock(fd);
        dev_slot_t *slot = &d->kbds[d->kbd_count++];
        slot->fd = fd;
        snprintf(slot->path, sizeof(slot->path), "%s", path);
        snprintf(slot->name, sizeof(slot->name), "%s", name);
        epoll_add_fd(d->epoll_fd, fd, EPOLLIN);
        log_info("grabbed keyboard %s [%s]", path, name);
        write_status_file(d);
        return;
    }

    if (d->sm.cfg.cancel_on_pointer && d->ptr_count < MAX_PTR_DEVS && is_pointer_device(fd, name)) {
        enable_monotonic_clock(fd);
        dev_slot_t *slot = &d->ptrs[d->ptr_count++];
        slot->fd = fd;
        snprintf(slot->path, sizeof(slot->path), "%s", path);
        snprintf(slot->name, sizeof(slot->name), "%s", name);
        epoll_add_fd(d->epoll_fd, fd, EPOLLIN);
        if (d->verbose)
            log_info("observing pointer %s [%s] (non-grabbing)", path, name);
        write_status_file(d);
        return;
    }

    close(fd);
}

static void scan_input_devices(daemon_t *d) {
    if (d->explicit_device)
        try_attach_device(d, d->explicit_device);

    DIR *dir = opendir("/dev/input");
    if (!dir)
        return;
    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        if (strncmp(de->d_name, "event", 5) != 0)
            continue;
        char path[300];
        snprintf(path, sizeof(path), "/dev/input/%s", de->d_name);
        try_attach_device(d, path);
    }
    closedir(dir);
}

static void detach_slot(daemon_t *d, dev_slot_t *arr, size_t *count, size_t idx, bool is_kbd) {
    int fd = arr[idx].fd;
    log_info("detached %s %s [%s]", is_kbd ? "keyboard" : "pointer", arr[idx].path, arr[idx].name);
    epoll_del_fd(d->epoll_fd, fd);
    if (is_kbd) {
        (void)ioctl(fd, EVIOCGRAB, (void *)0);
        release_all_keys(d);
    }
    close(fd);
    for (size_t j = idx; j + 1 < *count; j++)
        arr[j] = arr[j + 1];
    (*count)--;
    write_status_file(d);
}

static int setup_ipc_socket(const char *sock_path) {
    char dirbuf[256];
    snprintf(dirbuf, sizeof(dirbuf), "%s", sock_path);
    char *slash = strrchr(dirbuf, '/');
    if (slash && slash != dirbuf) {
        *slash = '\0';
        (void)mkdir(dirbuf, 0755);
    }
    (void)unlink(sock_path);

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", sock_path);

    mode_t prev_umask = umask(0000);
    int bind_rc = bind(fd, (struct sockaddr *)&addr, sizeof(addr));
    umask(prev_umask);
    if (bind_rc < 0 || listen(fd, 8) < 0) {
        log_info("WARN: IPC socket %s failed: %s", sock_path, strerror(errno));
        close(fd);
        return -1;
    }
    (void)chmod(sock_path, 0666);
    return fd;
}

static void forward_uinput_leds(daemon_t *d) {
    struct input_event ev;
    while (read(d->uinput_fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
        if (ev.type == EV_LED) {
            struct input_event syn;
            memset(&syn, 0, sizeof(syn));
            syn.type = EV_SYN;
            syn.code = SYN_REPORT;
            for (size_t i = 0; i < d->kbd_count; i++) {
                /* Best effort: a keyboard without that LED rejects the write. */
                if (write(d->kbds[i].fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev) &&
                    write(d->kbds[i].fd, &syn, sizeof(syn)) != (ssize_t)sizeof(syn))
                    log_info("WARN: LED SYN write failed on %s", d->kbds[i].path);
            }
        }
    }
}

static void handle_kbd_fd(daemon_t *d, size_t idx, uint32_t ev_mask) {
    struct input_event ev;
    int fd = d->kbds[idx].fd;
    while (1) {
        ssize_t n = read(fd, &ev, sizeof(ev));
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            detach_slot(d, d->kbds, &d->kbd_count, idx, true);
            return;
        }
        if (n == 0) {
            detach_slot(d, d->kbds, &d->kbd_count, idx, true);
            return;
        }
        if (n != (ssize_t)sizeof(ev))
            break;

        if (ev.type == EV_SYN && ev.code == SYN_DROPPED) {
            log_info("WARN: SYN_DROPPED on %s — releasing virtual keys to prevent stuck state",
                     d->kbds[idx].path);
            release_all_keys(d);
            continue;
        }

        if (ev.type == EV_KEY) {
            uint64_t ts = input_ev_ts_ns(&ev);
            if (ts == 0)
                ts = monotonic_ns();
            globe_out_batch_t batch;
            globe_sm_handle_key(&d->sm, ev.code, ev.value, ts, &batch);
            dispatch_batch(d, &batch, ts);
        }
    }
    if (ev_mask & (EPOLLHUP | EPOLLERR))
        detach_slot(d, d->kbds, &d->kbd_count, idx, true);
}

static void handle_ptr_fd(daemon_t *d, size_t idx, uint32_t ev_mask) {
    struct input_event ev;
    int fd = d->ptrs[idx].fd;
    while (1) {
        ssize_t n = read(fd, &ev, sizeof(ev));
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            detach_slot(d, d->ptrs, &d->ptr_count, idx, false);
            return;
        }
        if (n == 0) {
            detach_slot(d, d->ptrs, &d->ptr_count, idx, false);
            return;
        }
        if (n != (ssize_t)sizeof(ev))
            break;
        if (ev.type == EV_KEY && gt_keycode_is_pointer_btn(ev.code)) {
            uint64_t ts = input_ev_ts_ns(&ev);
            if (ts == 0)
                ts = monotonic_ns();
            globe_out_batch_t batch;
            globe_sm_handle_pointer_btn(&d->sm, ev.code, ev.value, ts, &batch);
            dispatch_batch(d, &batch, ts);
        } else if (ev.type == EV_REL && gt_rel_is_scroll_axis(ev.code)) {
            uint64_t ts = input_ev_ts_ns(&ev);
            if (ts == 0)
                ts = monotonic_ns();
            globe_out_batch_t batch;
            globe_sm_handle_pointer_scroll(&d->sm, ev.code, ev.value, ts, &batch);
            dispatch_batch(d, &batch, ts);
        }
    }
    if (ev_mask & (EPOLLHUP | EPOLLERR))
        detach_slot(d, d->ptrs, &d->ptr_count, idx, false);
}

static void handle_inotify(daemon_t *d) {
    char buf[1024] __attribute__((aligned(__alignof__(struct inotify_event))));
    while (read(d->inotify_fd, buf, sizeof(buf)) > 0) {
        /* Drain pending events before rescanning /dev/input. */
    }
    scan_input_devices(d);
}

static void reload_config(daemon_t *d) {
    globe_config_t next_cfg;
    char err[256] = {0};
    if (!globe_config_load_file(d->config_path, &next_cfg, true, err, sizeof(err))) {
        log_info("WARN: config reload failed (%s): %s — keeping active config", d->config_path, err);
        return;
    }
    uint16_t prev_globe = d->sm.cfg.globe_keycode;
    bool prev_ptr = d->sm.cfg.cancel_on_pointer;

    release_all_keys(d);
    globe_sm_init(&d->sm, &next_cfg);

    if (prev_globe != next_cfg.globe_keycode) {
        while (d->kbd_count > 0)
            detach_slot(d, d->kbds, &d->kbd_count, 0, true);
    }
    if (prev_ptr && !next_cfg.cancel_on_pointer) {
        while (d->ptr_count > 0)
            detach_slot(d, d->ptrs, &d->ptr_count, 0, false);
    }
    if (prev_globe != next_cfg.globe_keycode || (!prev_ptr && next_cfg.cancel_on_pointer))
        scan_input_devices(d);

    write_status_file(d);
    log_info("reloaded config %s (tap_mode=%s, tap_timeout_ms=%u, bindings=%zu)",
             d->config_path, globe_tap_mode_to_name(next_cfg.tap_mode),
             next_cfg.tap_timeout_ms, next_cfg.binding_count);
}

static void usage(const char *prog) {
    fprintf(stderr,
            "Usage: %s [OPTIONS]\n"
            "  -c, --config PATH       Config file (default: %s)\n"
            "  -s, --socket PATH       UNIX IPC socket (default: %s)\n"
            "      --status-file PATH  Runtime status JSON path (default: %s)\n"
            "  -d, --device PATH       Explicit /dev/input/eventN keyboard device\n"
            "      --check-config      Validate config file and exit\n"
            "  -v, --verbose           Log events and IPC messages to stderr\n"
            "  -h, --help              Show this help\n",
            prog, GT_DEFAULT_CONFIG_PATH, GT_DEFAULT_SOCKET_PATH, GT_DEFAULT_STATUS_PATH);
}

int main(int argc, char **argv) {
    daemon_t d;
    memset(&d, 0, sizeof(d));
    d.epoll_fd = -1;
    d.uinput_fd = -1;
    d.inotify_fd = -1;
    d.ipc_listen_fd = -1;
    for (int i = 0; i < MAX_IPC_CLIENTS; i++)
        d.ipc_clients[i] = -1;
    d.config_path = GT_DEFAULT_CONFIG_PATH;
    d.socket_path = GT_DEFAULT_SOCKET_PATH;
    d.status_path = GT_DEFAULT_STATUS_PATH;
    bool check_only = false;

    for (int i = 1; i < argc; i++) {
        if ((strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--config") == 0) && i + 1 < argc) {
            d.config_path = argv[++i];
        } else if ((strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "--socket") == 0) && i + 1 < argc) {
            d.socket_path = argv[++i];
        } else if (strcmp(argv[i], "--status-file") == 0 && i + 1 < argc) {
            d.status_path = argv[++i];
        } else if ((strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--device") == 0) && i + 1 < argc) {
            d.explicit_device = argv[++i];
        } else if (strcmp(argv[i], "--check-config") == 0) {
            check_only = true;
        } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            d.verbose = true;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    globe_config_t cfg;
    char err[256] = {0};
    if (!globe_config_load_file(d.config_path, &cfg, !check_only, err, sizeof(err))) {
        log_info("ERROR: invalid config %s: %s", d.config_path, err);
        return 1;
    }
    if (check_only) {
        printf("Config OK: %s (tap_mode=%s, tap_timeout_ms=%u, bindings=%zu)\n",
               d.config_path, globe_tap_mode_to_name(cfg.tap_mode),
               cfg.tap_timeout_ms, cfg.binding_count);
        return 0;
    }

    globe_sm_init(&d.sm, &cfg);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    d.uinput_fd = create_uinput_keyboard();
    if (d.uinput_fd < 0)
        return 1;

    d.epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (d.epoll_fd < 0) {
        log_info("ERROR: epoll_create1 failed: %s", strerror(errno));
        close(d.uinput_fd);
        return 1;
    }
    epoll_add_fd(d.epoll_fd, d.uinput_fd, EPOLLIN);

    d.ipc_listen_fd = setup_ipc_socket(d.socket_path);
    if (d.ipc_listen_fd >= 0)
        epoll_add_fd(d.epoll_fd, d.ipc_listen_fd, EPOLLIN);

    d.inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (d.inotify_fd >= 0) {
        (void)inotify_add_watch(d.inotify_fd, "/dev/input", IN_CREATE | IN_ATTRIB);
        epoll_add_fd(d.epoll_fd, d.inotify_fd, EPOLLIN);
    }

    scan_input_devices(&d);
    write_status_file(&d);
    log_info("started (tap_mode=%s, tap_timeout_ms=%u, kbds=%zu, ptrs=%zu, sock=%s)",
             globe_tap_mode_to_name(d.sm.cfg.tap_mode), d.sm.cfg.tap_timeout_ms,
             d.kbd_count, d.ptr_count, d.socket_path);

    struct epoll_event events[MAX_EVENTS];
    while (!g_stop) {
        if (g_reload) {
            g_reload = 0;
            reload_config(&d);
        }

        globe_out_batch_t tick_batch;
        uint64_t now = monotonic_ns();
        int32_t timeout_ms = globe_sm_tick(&d.sm, now, &tick_batch);
        if (tick_batch.count > 0)
            dispatch_batch(&d, &tick_batch, now);

        int n = epoll_wait(d.epoll_fd, events, MAX_EVENTS, timeout_ms);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            break;
        }

        for (int i = 0; i < n; i++) {
            int fd = events[i].data.fd;
            if (fd == d.uinput_fd) {
                forward_uinput_leds(&d);
            } else if (fd == d.inotify_fd) {
                handle_inotify(&d);
            } else if (fd == d.ipc_listen_fd) {
                bool added = false;
                while (1) {
                    int cfd = accept4(d.ipc_listen_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
                    if (cfd < 0)
                        break;
                    bool placed = false;
                    for (int c = 0; c < MAX_IPC_CLIENTS; c++) {
                        if (d.ipc_clients[c] < 0) {
                            d.ipc_clients[c] = cfd;
                            epoll_add_fd(d.epoll_fd, cfd, EPOLLIN | EPOLLRDHUP | EPOLLHUP);
                            placed = true;
                            added = true;
                            break;
                        }
                    }
                    if (!placed)
                        close(cfd);
                }
                if (added)
                    write_status_file(&d);
            } else {
                bool handled = false;
                for (size_t k = 0; k < d.kbd_count; k++) {
                    if (d.kbds[k].fd == fd) {
                        handle_kbd_fd(&d, k, events[i].events);
                        handled = true;
                        break;
                    }
                }
                if (!handled) {
                    for (size_t p = 0; p < d.ptr_count; p++) {
                        if (d.ptrs[p].fd == fd) {
                            handle_ptr_fd(&d, p, events[i].events);
                            handled = true;
                            break;
                        }
                    }
                }
                if (!handled) {
                    for (int c = 0; c < MAX_IPC_CLIENTS; c++) {
                        if (d.ipc_clients[c] == fd) {
                            handle_ipc_client_event(&d, c);
                            break;
                        }
                    }
                }
            }
        }
    }

    release_all_keys(&d);

    while (d.kbd_count > 0)
        detach_slot(&d, d.kbds, &d.kbd_count, 0, true);
    while (d.ptr_count > 0)
        detach_slot(&d, d.ptrs, &d.ptr_count, 0, false);

    for (int i = 0; i < MAX_IPC_CLIENTS; i++) {
        if (d.ipc_clients[i] >= 0)
            close(d.ipc_clients[i]);
    }
    if (d.ipc_listen_fd >= 0) {
        close(d.ipc_listen_fd);
        (void)unlink(d.socket_path);
    }
    if (d.status_path)
        (void)unlink(d.status_path);
    if (d.inotify_fd >= 0)
        close(d.inotify_fd);
    if (d.epoll_fd >= 0)
        close(d.epoll_fd);
    if (d.uinput_fd >= 0) {
        (void)ioctl(d.uinput_fd, UI_DEV_DESTROY);
        close(d.uinput_fd);
    }
    log_info("stopped cleanly");
    return 0;
}

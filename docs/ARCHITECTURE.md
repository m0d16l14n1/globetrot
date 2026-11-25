# Architecture

globetrot has two processes: a root daemon that owns the keyboard, and a user agent that talks to
the desktop.

```
keyboard (/dev/input/eventN, reports KEY_A and KEY_FN)
  │  EVIOCGRAB, CLOCK_MONOTONIC timestamps
  ▼
globetrotd                        system service, C, root
  ├── state machine (src/globe_sm.c)
  │     tap      Globe down and up within tap_timeout_ms (400 ms), nothing else pressed
  │              -> layout switch / key / emoji / command
  │     hold     longer than tap_timeout_ms -> ignored
  │     combo    any other key, a modifier held before Globe, a mouse button or wheel
  │              -> no tap; the other key goes through unchanged
  │     Fn layer Delete, Insert, Home/End/PgUp/PgDn, F-keys and media keys pass through
  │              without added modifiers
  │     bindings Globe+[mods+]key -> key chord, built-in action or command
  │     rollover a key pressed before Globe and released while it is down does not
  │              cancel the tap (fast typing across a layout switch)
  ├── uinput "Globetrot Virtual Keyboard" (Caps Lock LED forwarded back)
  ├── pointer devices watched read-only for clicks and wheel events
  ├── /run/globetrot/status.json   current state
  └── /run/globetrot/events.sock   one JSON line per Globe event
        │
        ▼
globetrot-session                 user service, Python standard library
  ├── recently-used layout order (two layouts toggle; with more, taps within
  │   1 s walk further back)
  └── backends: KWin (D-Bus), Hyprland, niri, Sway, GNOME, fcitx5
```

## Why two processes

Reading input devices and creating a uinput device need root. Switching the layout on Wayland, and
starting apps such as the emoji picker, has to happen inside the user's session, which knows the
session bus and the compositor socket. Keeping the key path in a small C program also keeps typing
latency out of the Python side.

With `tap_mode = key`, the daemon works on its own: it sends a key (F24 by default) on tap, and the
desktop's own shortcut settings take it from there.

## Safety

- The daemon releases every virtual key on shutdown, on device removal, on `SYN_DROPPED`, and on a
  config reload.
- If it exits or crashes, the kernel drops the grab, so the keyboard works directly again.
- Packages never enable or start the service on install. The installer runs a trial with a timer
  that stops the daemon unless the user confirms the keyboard works.
- The service runs with `ProtectSystem=strict`, `ProtectHome`, `DevicePolicy=closed` and access to
  input devices and uinput only.

## Known limits

- One virtual keyboard carries the keys of every grabbed keyboard. It is tagged internal, so typing
  on a grabbed external Apple keyboard also pauses the internal touchpad. In clamshell mode, libinput
  may also take that typing as a sign the lid switch is unreliable.
- Two-finger scrolling and tap-to-click on a touchpad are produced by libinput, above the evdev
  layer the daemon sees, so they don't cancel a pending tap. Physical clicks and mouse wheels do.
- The event socket is readable by all local users. It carries Globe events only, never ordinary
  keystrokes, unless the daemon runs with `--verbose`.
- Programs that read the physical keyboard device directly stop seeing its keys while it is grabbed.
  They see the virtual keyboard instead once they rescan devices.

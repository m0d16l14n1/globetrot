# globetrot

The Globe key on Linux, the way it works on a Mac.

On Apple keyboards the bottom-left key (fn, with a globe on recent models) switches the input
language when you tap it, and works as Fn when you hold it with other keys. globetrot does the same
on Linux:

- **Tap Globe alone**: switch to the previous keyboard layout. With three or more layouts,
  quick repeated taps walk further back through the recently used ones.
- **Hold Globe with another key**: the Fn layer still works as before: Globe+Backspace is Delete,
  Globe+Return is Insert, Globe+arrows are Home/End/PgUp/PgDn, and the top row switches between
  F1–F12 and media keys. No layout switch happens.
- **Hold Globe for longer and let go**: nothing happens, so a hesitant press never switches layout.
- **Optional extras**: Globe+key shortcuts, an emoji picker, a command of your own, or a key (F24 by
  default) that you can bind in any shortcut settings.

It works with KDE Plasma, Hyprland (including Omarchy), niri, Sway, GNOME and fcitx5, and any
keyboard whose driver reports the Fn key (`KEY_FN`). That covers Apple laptops on Asahi-based
kernels and Apple Magic Keyboards through `hid-apple`.

## Install

From a terminal in your desktop session, as your normal user:

```sh
curl -fsSL https://raw.githubusercontent.com/m0d16l14n1/globetrot/main/install.sh | bash
```

Add `-s -- --layouts us,ru` (with your own layouts) if you have only one layout set up so far:

```sh
curl -fsSL https://raw.githubusercontent.com/m0d16l14n1/globetrot/main/install.sh | bash -s -- --layouts us,ru
```

The script:

1. Checks that a keyboard reports a Fn key.
2. Builds a native package for your system (pacman, rpm or deb) and installs it. Build tools are
   installed if needed. The build takes well under a minute.
3. Starts the daemon **on trial**. You type `ok` to keep it. If the keyboard misbehaved, you just
   wait 30 seconds: the daemon stops by itself and nothing is enabled.
4. Enables the daemon and the session agent, and checks a layout switch works.

To remove it:

```sh
curl -fsSL https://raw.githubusercontent.com/m0d16l14n1/globetrot/main/install.sh | bash -s -- --uninstall
```

### Per distribution

**Arch Linux, Arch Linux ARM, Omarchy**: the installer builds with makepkg from
[`packaging/arch/PKGBUILD`](packaging/arch/PKGBUILD). To do it by hand:

```sh
git clone https://github.com/m0d16l14n1/globetrot && cd globetrot/packaging/arch && makepkg -si
```

On Omarchy, Hyprland runs under uwsm, so the session agent starts with the desktop. Keyboard
layouts are set with `kb_layout` in `~/.config/hypr/input.conf` (for example `kb_layout = us,ru`).

**Fedora (including Fedora Asahi Remix)**: the installer builds with rpmbuild from
[`packaging/fedora/globetrot.spec`](packaging/fedora/globetrot.spec).

**Debian, Ubuntu**: the installer builds with dpkg-buildpackage from [`debian/`](debian). To do it by hand:

```sh
git clone https://github.com/m0d16l14n1/globetrot && cd globetrot && dpkg-buildpackage -b -us -uc
sudo apt install ../globetrot_*.deb
```

None of these packages start anything on install; the services are enabled explicitly.

**NixOS**: use the flake:

```nix
{
  inputs.globetrot.url = "github:m0d16l14n1/globetrot";

  outputs = { nixpkgs, globetrot, ... }: {
    nixosConfigurations.myhost = nixpkgs.lib.nixosSystem {
      modules = [
        globetrot.nixosModules.default
        { services.globetrot.enable = true; }
        # ...
      ];
    };
  };
}
```

`services.globetrot.settings` holds the text of `/etc/globetrot.conf`. The module loads `uinput`,
installs the libinput quirk and starts the session agent with graphical sessions.

### Manual install

```sh
make && make test
sudo make install                       # PREFIX=/usr by default
sudo systemctl start globetrotd         # try it: type something
sudo systemctl enable globetrotd
systemctl --user enable --now globetrot-session
```

## Usage

```sh
globetrot status                        # settings, grabbed keyboards, desktop, layouts
globetrot monitor                       # live tap / hold / cancel events
globetrot test-switch                   # switch layout once, from the command line

sudo globetrot set --tap-timeout-ms 300                 # tap vs hold threshold (default 400 ms)
sudo globetrot set --tap-mode both --tap-key f24        # also send F24 on tap, for your own shortcuts
sudo globetrot bind globe+e action:emoji                # Globe+E opens the emoji picker
sudo globetrot bind globe+f key:f11                     # Globe+F sends F11
sudo globetrot bind globe+ctrl+home key:leftmeta+left   # Globe+Ctrl+Left (arrives as Home)
sudo globetrot set --combo-modifier super_alt           # Globe+<key> arrives as Super+Alt+<key>
```

All settings live in [`/etc/globetrot.conf`](config/globetrot.conf), which documents each one. `set`
and `bind` keep its comments, validate the result and reload the services. `~/.config/globetrot.conf`
can override the `[session]` section per user.

Two things to know when binding keys:

- The kernel turns Globe+Backspace/Return/arrows into Delete/Insert/Home/End/PgUp/PgDn before
  globetrot sees them, so bind those names.
- Don't use F21–F23 for anything. Standard keyboard maps turn them into touchpad toggle, on and off.

## How it works

`globetrotd` is a small C daemon. It takes exclusive hold of each keyboard that reports `KEY_A` and
`KEY_FN`, and re-sends its keys through a virtual keyboard (uinput). It holds back only the Globe key
itself, and decides from timing and the other keys whether a press was a tap. A tap goes out as an
event on a local socket.

`globetrot-session` runs in your desktop session and switches the layout through the desktop's own
interface: KWin over D-Bus, `hyprctl`, `niri msg`, `swaymsg`, `gsettings` or `fcitx5-remote`.

A libinput quirk marks the virtual keyboard as internal, so the touchpad still pauses while you type.
If the daemon stops for any reason, the kernel releases the keyboard and it works directly again.

More detail: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## Status

Version 0.4.0. In use on an Apple silicon MacBook with Arch Linux ARM and KDE Plasma.

- **Covered by the test suite and container runs:** packaging builds, install and removal on Arch
  Linux ARM, Fedora and Debian; the Nix build; and an end-to-end test of the daemon against a uinput
  keyboard.
- **Not yet tried on real machines:** Hyprland, niri, GNOME and Sway as desktops, Fedora with SELinux
  enforcing, and NixOS hosts. Reports are welcome.

## Development

```sh
make test WERROR=1                       # C tests under ASan/UBSan + Python tests
sudo python3 tests/e2e_uinput.py build/globetrotd   # end-to-end, in a VM or container
make dist                                # source tarball
```

## License

MIT

#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Globetrot installer.
#
#   curl -fsSL https://raw.githubusercontent.com/m0d16l14n1/globetrot/main/install.sh | bash
#   curl -fsSL https://raw.githubusercontent.com/m0d16l14n1/globetrot/main/install.sh | bash -s -- --layouts us,ru
#
# It builds a native package for your distribution (Arch Linux / Arch Linux ARM / Omarchy,
# Fedora, Debian / Ubuntu), installs it, then starts the daemon on trial: it stops by itself
# after 30 seconds unless you confirm the keyboard still works. Only then is it enabled.
# On NixOS it prints the flake configuration instead.
#
# Run it as your normal user from a terminal in your desktop session; it uses sudo itself.
#
# Options:
#   --layouts LIST    also configure keyboard layouts, e.g. us,ru (KDE; hints for others)
#   --version X.Y.Z   install that release (default: latest known to this script)
#   --ref REF         install a branch or commit instead of a release (e.g. main)
#   --source DIR      build from a local checkout
#   --no-trial        enable without the 30 s trial (for remote/scripted installs)
#   --package-only    build and install the package, change nothing else
#   --uninstall       disable and remove globetrot
set -euo pipefail

REPO="m0d16l14n1/globetrot"
DEFAULT_VERSION="0.4.0"
TRIAL_SECONDS=30

if [ -t 1 ]; then
  c_ok=$'\e[32m'; c_warn=$'\e[33m'; c_err=$'\e[31m'; c_dim=$'\e[2m'; c_off=$'\e[0m'
else
  c_ok=; c_warn=; c_err=; c_dim=; c_off=
fi
info() { printf '%s==>%s %s\n' "$c_ok" "$c_off" "$*"; }
warn() { printf '%sWARN:%s %s\n' "$c_warn" "$c_off" "$*" >&2; }
die()  { printf '%sERROR:%s %s\n' "$c_err" "$c_off" "$*" >&2; exit 1; }

WORK=""
cleanup() { if [ -n "$WORK" ] && [ -d "$WORK" ]; then rm -rf -- "$WORK"; fi; }

# ------------------------------------------------------------------ distribution
detect_family() {
  local id="" like=""
  if [ -r /etc/os-release ]; then
    # shellcheck disable=SC1091
    id="$(. /etc/os-release && echo "${ID:-}")"
    # shellcheck disable=SC1091
    like="$(. /etc/os-release && echo "${ID_LIKE:-}")"
  fi
  case " $id $like " in
    *" nixos "*) echo nixos ;;
    *" arch "*|*" archarm "*) echo arch ;;
    *" fedora "*|*" rhel "*) echo fedora ;;
    *" debian "*|*" ubuntu "*) echo debian ;;
    *)
      if command -v pacman >/dev/null; then echo arch
      elif command -v dnf >/dev/null; then echo fedora
      elif command -v apt-get >/dev/null; then echo debian
      else echo unknown; fi ;;
  esac
}

pkg_installed() {  # pkg_installed FAMILY NAME
  case "$1" in
    arch)   pacman -Q "$2" >/dev/null 2>&1 ;;
    fedora) rpm -q "$2" >/dev/null 2>&1 ;;
    debian) dpkg-query -W -f='${Status}' "$2" 2>/dev/null | grep -q 'install ok installed' ;;
    *) return 1 ;;
  esac
}

pkg_remove() {  # pkg_remove FAMILY NAME
  case "$1" in
    arch)   sudo pacman -R --noconfirm "$2" ;;
    fedora) sudo dnf remove -y "$2" ;;
    debian) sudo apt-get remove -y "$2" ;;
  esac
}

# ------------------------------------------------------------------ keyboard check
# True if some input device reports both KEY_A (30) and KEY_FN (464).
has_fn_keyboard() {
  local line bits n w30 w464 found=1 lb
  lb="$(getconf LONG_BIT 2>/dev/null || echo 64)"
  [ "$lb" = 64 ] || return 0   # bitmap layout differs; let the daemon decide
  while IFS= read -r line; do
    case "$line" in
      N:\ Name=*) name="${line#N: Name=}" ;;
      B:\ KEY=*)
        read -r -a bits <<<"${line#B: KEY=}"
        n=${#bits[@]}
        [ "$n" -ge 8 ] || continue
        w30=$(( 0x${bits[n-1]} ))
        w464=$(( 0x${bits[n-8]} ))
        if (( (w30 >> 30) & 1 )) && (( (w464 >> 16) & 1 )); then
          printf '     %s\n' "${name:-?}"
          found=0
        fi ;;
    esac
  done </proc/bus/input/devices
  return "$found"
}

# ------------------------------------------------------------------ source
fetch_source() {  # sets SRC_DIR and VERSION
  local version="$1" ref="$2" local_src="$3"
  if [ -n "$local_src" ]; then
    [ -f "$local_src/src/main_linux.c" ] || die "$local_src is not a globetrot checkout"
    SRC_DIR="$(cd "$local_src" && pwd)"
  else
    command -v curl >/dev/null || die "curl is required"
    local url
    if [ -n "$ref" ]; then
      url="https://github.com/$REPO/archive/$ref.tar.gz"
    else
      url="https://github.com/$REPO/archive/refs/tags/v$version.tar.gz"
    fi
    info "Downloading $url"
    mkdir -p "$WORK/download"
    curl -fsSL "$url" | tar -xz -C "$WORK/download" || die "download failed"
    SRC_DIR="$(find "$WORK/download" -mindepth 1 -maxdepth 1 -type d | head -1)"
  fi
  VERSION="$(sed -n 's/^__version__ = "\(.*\)"/\1/p' "$SRC_DIR/python/globetrot/__init__.py")"
  [ -n "$VERSION" ] || die "cannot read the version from $SRC_DIR"
  # Packaging tools expect globetrot-VERSION.tar.gz with a globetrot-VERSION/ top directory.
  mkdir -p "$WORK/src"
  tar -C "$SRC_DIR" --exclude=.git --exclude=build --exclude=__pycache__ \
    --transform "s,^\\.,globetrot-$VERSION," -czf "$WORK/src/globetrot-$VERSION.tar.gz" .
}

# ------------------------------------------------------------------ build + install
build_arch() {
  if ! pacman -Qq base-devel python >/dev/null 2>&1; then
    info "Installing build tools (base-devel)"
    sudo pacman -S --needed --noconfirm base-devel python
  fi
  local d="$WORK/arch"
  mkdir -p "$d"
  cp "$SRC_DIR/packaging/arch/PKGBUILD" "$SRC_DIR/packaging/arch/globetrot.install" "$d/"
  cp "$WORK/src/globetrot-$VERSION.tar.gz" "$d/"
  sed -i "s/^pkgver=.*/pkgver=$VERSION/" "$d/PKGBUILD"
  (cd "$d" && makepkg -f --skipchecksums --noconfirm)
  PKG_FILE="$(find "$d" -maxdepth 1 -name "globetrot-$VERSION-*.pkg.tar.*" ! -name '*-debug-*' | head -1)"
}

build_fedora() {
  local need=(rpm-build gcc make python3 systemd-rpm-macros libasan libubsan)
  if ! rpm -q "${need[@]}" >/dev/null 2>&1; then
    info "Installing build tools"
    sudo dnf install -y "${need[@]}"
  fi
  local top="$WORK/rpmbuild"
  mkdir -p "$top/SOURCES" "$top/SPECS"
  cp "$WORK/src/globetrot-$VERSION.tar.gz" "$top/SOURCES/"
  sed "s/^Version:.*/Version:        $VERSION/" "$SRC_DIR/packaging/fedora/globetrot.spec" >"$top/SPECS/globetrot.spec"
  rpmbuild --define "_topdir $top" -bb "$top/SPECS/globetrot.spec"
  PKG_FILE="$(find "$top/RPMS" -name "globetrot-$VERSION-*.rpm" ! -name '*debug*' | head -1)"
}

build_debian() {
  local need=(build-essential debhelper python3)
  if ! dpkg-query -W "${need[@]}" >/dev/null 2>&1; then
    info "Installing build tools"
    sudo apt-get update -qq
    sudo apt-get install -y "${need[@]}"
  fi
  local d="$WORK/deb"
  mkdir -p "$d"
  tar -xzf "$WORK/src/globetrot-$VERSION.tar.gz" -C "$d"
  (cd "$d/globetrot-$VERSION" && dpkg-buildpackage -b -us -uc)
  PKG_FILE="$(find "$d" -maxdepth 1 -name "globetrot_*.deb" | head -1)"
}

install_pkg() {
  case "$FAMILY" in
    arch)   sudo pacman -U --noconfirm "$PKG_FILE" ;;
    fedora) sudo dnf install -y "$PKG_FILE" ;;
    debian) sudo apt-get install -y --reinstall "$PKG_FILE" ;;
  esac
}

# The earlier package name. Its settings carry over into /etc/globetrot.conf.
OLD_PKG="aurora-globe"
OLD_CONF="/etc/aurora/globe.conf"

retire_old_package() {
  pkg_installed "$FAMILY" "$OLD_PKG" || return 0
  info "Replacing $OLD_PKG with globetrot (settings are kept)"
  if [ -f "$OLD_CONF" ]; then
    cp "$OLD_CONF" "$WORK/old.conf"
  fi
  systemctl --user disable --now aurora-globe-session.service >/dev/null 2>&1 || true
  sudo systemctl disable --now aurora-globed.service >/dev/null 2>&1 || true
  pkg_remove "$FAMILY" "$OLD_PKG"
}

migrate_old_settings() {
  [ -f "$WORK/old.conf" ] || return 0
  sudo cp /etc/globetrot.conf "/etc/globetrot.conf.before-migration"
  # Apply every value of the old file onto the new commented default.
  sudo python3 - "$WORK/old.conf" /etc/globetrot.conf <<'PY'
import pathlib, sys
for cand in ("/usr/lib/globetrot", "/run/current-system/sw/lib/globetrot"):
    sys.path.insert(0, cand)
from globetrot.ini import IniDoc
old = IniDoc(pathlib.Path(sys.argv[1]).read_text(encoding="utf-8"))
new_path = pathlib.Path(sys.argv[2])
new = IniDoc(new_path.read_text(encoding="utf-8"))
for section in ("globe", "bindings", "session"):
    for key, value in old.items(section):
        new.set(section, key, value)
new_path.write_text(new.text(), encoding="utf-8")
PY
  /usr/bin/globetrotd --check-config >/dev/null || {
    sudo cp /etc/globetrot.conf.before-migration /etc/globetrot.conf
    warn "old settings did not validate; using the defaults"
  }
  info "Carried over settings from $OLD_CONF"
}

# ------------------------------------------------------------------ trial / enable
trial() {
  local secs="$1"
  sudo systemctl stop globetrot-deadman.timer >/dev/null 2>&1 || true
  sudo systemctl reset-failed globetrot-deadman.service globetrot-deadman.timer >/dev/null 2>&1 || true
  # Root-side dead-man: stops the daemon after $secs unless confirmed, even if the
  # keyboard stops working and no password can be typed.
  sudo systemd-run --quiet --unit=globetrot-deadman --on-active="${secs}s" \
    --timer-property=AccuracySec=1s systemctl stop globetrotd.service
  sudo systemctl start globetrotd.service
  sleep 1
  if ! systemctl is-active --quiet globetrotd.service; then
    sudo systemctl stop globetrot-deadman.timer >/dev/null 2>&1 || true
    journalctl -u globetrotd.service -n 20 --no-pager 2>/dev/null || true
    die "globetrotd failed to start (log above)"
  fi
  echo
  info "globetrotd is running and has taken over the keyboard."
  echo "   Type ${c_ok}ok${c_off} and press Enter within ${secs} s to keep it."
  echo "   ${c_dim}If keys do not work, just wait: it stops by itself and the keyboard comes back.${c_off}"
  local ans=""
  if read -r -t "$secs" ans </dev/tty && [ "$ans" = "ok" ]; then
    sudo systemctl stop globetrot-deadman.timer >/dev/null 2>&1 || true
    info "Typing works through globetrotd."
    return 0
  fi
  sleep 2
  if systemctl is-active --quiet globetrotd.service; then
    sudo systemctl stop globetrotd.service
  fi
  return 1
}

session_env() {
  local vars=() v
  for v in WAYLAND_DISPLAY DISPLAY XDG_CURRENT_DESKTOP HYPRLAND_INSTANCE_SIGNATURE NIRI_SOCKET SWAYSOCK; do
    if [ -n "${!v:-}" ]; then vars+=("$v"); fi
  done
  if [ "${#vars[@]}" -gt 0 ]; then
    systemctl --user import-environment "${vars[@]}" 2>/dev/null || true
  fi
  if ! systemctl --user is-active --quiet graphical-session.target 2>/dev/null; then
    warn "graphical-session.target is not active, so globetrot-session will not start by itself at login."
    if [ -n "${HYPRLAND_INSTANCE_SIGNATURE:-}" ]; then
      warn "Hyprland without uwsm: add  exec-once = systemctl --user start globetrot-session"
    elif [ -n "${NIRI_SOCKET:-}" ]; then
      warn "niri: add  spawn-at-startup \"systemctl\" \"--user\" \"start\" \"globetrot-session\""
    fi
  fi
}

enable_services() {
  sudo systemctl enable --now globetrotd.service
  session_env
  systemctl --user daemon-reload
  systemctl --user enable globetrot-session.service
  systemctl --user restart globetrot-session.service
  info "Enabled globetrotd (system) and globetrot-session (your user)"
}

# ------------------------------------------------------------------ layouts
set_layouts() {
  local list="$1"
  case "$list" in *[!a-z0-9,_-]*|'') die "layouts must look like: us,ru (xkb layout names)" ;; esac
  if [[ "${XDG_CURRENT_DESKTOP:-}" == *KDE* ]] && command -v kwriteconfig6 >/dev/null; then
    if command -v kreadconfig6 >/dev/null; then
      echo "   previous KDE layouts: $(kreadconfig6 --file kxkbrc --group Layout --key LayoutList)"
    fi
    kwriteconfig6 --file kxkbrc --group Layout --key Use true
    kwriteconfig6 --file kxkbrc --group Layout --key LayoutList "$list"
    kwriteconfig6 --file kxkbrc --group Layout --key VariantList "$(printf '%s' "$list" | tr -cd ',')"
    dbus-send --session --type=signal /Layouts org.kde.keyboard.reloadConfig
    sleep 1
    info "KDE layouts set to $list"
  elif [ -n "${HYPRLAND_INSTANCE_SIGNATURE:-}" ]; then
    if hyprctl keyword input:kb_layout "$list" >/dev/null 2>&1; then
      info "Hyprland layouts set to $list for this session"
    else
      warn "hyprctl keyword input:kb_layout failed"
    fi
    echo "   To keep it, set kb_layout = $list in your Hyprland input config"
    echo "   (Omarchy: ~/.config/hypr/input.conf). Files mentioning kb_layout:"
    grep -rln 'kb_layout' "$HOME/.config/hypr" 2>/dev/null | sed 's/^/     /' || true
  elif [ -n "${NIRI_SOCKET:-}" ]; then
    echo "   niri reads layouts from its config (reloaded on save):"
    echo "     input { keyboard { xkb { layout \"$list\"; } } }"
  else
    warn "add the layouts $list in your desktop's keyboard settings"
  fi
}

layout_count() {
  globetrot status 2>/dev/null | sed -n 's/^Layouts (\([0-9]*\)).*/\1/p' | head -1
}

# ------------------------------------------------------------------ commands
print_nixos() {
  cat <<EOF
NixOS: add globetrot to your flake instead of running this script.

  inputs.globetrot.url = "github:$REPO";

  # in nixosSystem { modules = [ ... ]; }
  globetrot.nixosModules.default
  { services.globetrot.enable = true; }

Then: sudo nixos-rebuild switch
Settings go in services.globetrot.settings (the text of /etc/globetrot.conf).
EOF
}

uninstall() {
  systemctl --user disable --now globetrot-session.service >/dev/null 2>&1 || true
  sudo systemctl disable --now globetrotd.service >/dev/null 2>&1 || true
  sudo systemctl stop globetrot-deadman.timer >/dev/null 2>&1 || true
  if pkg_installed "$FAMILY" globetrot; then
    pkg_remove "$FAMILY" globetrot
  fi
  info "globetrot removed. The keyboard is back to normal."
}

main() {
  local version="$DEFAULT_VERSION" ref="" local_src="" layouts="" no_trial=0 package_only=0 do_uninstall=0
  while [ $# -gt 0 ]; do
    case "$1" in
      --layouts) layouts="${2:-}"; shift 2 ;;
      --version) version="${2:-}"; shift 2 ;;
      --ref) ref="${2:-}"; shift 2 ;;
      --source) local_src="${2:-}"; shift 2 ;;
      --no-trial) no_trial=1; shift ;;
      --package-only) package_only=1; shift ;;
      --uninstall) do_uninstall=1; shift ;;
      -h|--help) sed -n '3,27p' "${BASH_SOURCE[0]:-/dev/null}" 2>/dev/null | sed 's/^# \{0,1\}//'; return 0 ;;
      *) die "unknown option: $1 (see --help)" ;;
    esac
  done

  FAMILY="$(detect_family)"
  if [ "$FAMILY" = nixos ]; then print_nixos; return 0; fi
  [ "$(id -u)" -ne 0 ] || die "run as your normal user, not root; the script uses sudo when needed"
  command -v sudo >/dev/null || die "sudo is required"
  [ "$FAMILY" != unknown ] || die "unsupported distribution (supported: Arch/ALARM/Omarchy, Fedora, Debian/Ubuntu, NixOS)"

  if [ "$do_uninstall" -eq 1 ]; then uninstall; return 0; fi

  info "Keyboards with a Globe/Fn key:"
  if ! has_fn_keyboard; then
    [ "$package_only" -eq 1 ] || die "no keyboard reports a Fn key (KEY_FN); globetrot would have nothing to do"
    warn "no keyboard reports KEY_FN (continuing: --package-only)"
  fi

  WORK="$(mktemp -d "${TMPDIR:-/tmp}/globetrot.XXXXXX")"
  trap cleanup EXIT
  fetch_source "$version" "$ref" "$local_src"
  info "Building globetrot $VERSION for $FAMILY"
  PKG_FILE=""
  "build_$FAMILY"
  [ -n "$PKG_FILE" ] && [ -f "$PKG_FILE" ] || die "package build produced no package"

  retire_old_package
  info "Installing $(basename "$PKG_FILE")"
  install_pkg
  migrate_old_settings
  if [ "$package_only" -eq 1 ]; then
    info "Package installed; nothing enabled (--package-only)"
    return 0
  fi

  sudo systemctl stop globetrotd.service >/dev/null 2>&1 || true
  if [ "$no_trial" -eq 0 ]; then
    trial "$TRIAL_SECONDS" || die "not confirmed: globetrotd is stopped and not enabled. Log: journalctl -u globetrotd -b"
  fi
  enable_services

  if [ -n "$layouts" ]; then set_layouts "$layouts"; fi
  local n
  n="$(layout_count)"
  if [ -z "$n" ] || [ "$n" -lt 2 ]; then
    warn "fewer than two keyboard layouts are configured: a Globe tap has nothing to switch to."
    warn "add them in your desktop settings, or rerun with --layouts us,ru"
  elif ! { globetrot test-switch >/dev/null && globetrot test-switch >/dev/null; }; then
    warn "test layout switch failed: see 'globetrot status'"
  else
    info "Layout switching works (switched and back)"
  fi
  echo
  globetrot status || true
  echo
  info "Done. Tap Globe/Fn alone to switch layout; hold it with other keys for the Fn layer."
}

main "$@"

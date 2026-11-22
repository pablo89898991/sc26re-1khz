#!/usr/bin/env bash
# SC2 Overlay: one-command installer for Ubuntu / Debian.
#
#   ./install-linux.sh              build the app and install it
#   ./install-linux.sh -y           same, without asking questions
#   ./install-linux.sh --no-keys    do not add the GNOME hotkeys
#   ./install-linux.sh --uninstall  remove the app, the udev rules, autostart and the hotkeys
#
# Run it as your normal user (not with sudo). It asks for your password when it needs it.
set -euo pipefail

PKG="sc26re-overlay"
APP_DIR="/opt/SC2-Overlay"
UDEV_RULES="/etc/udev/rules.d/70-sc26re.rules"
AUTOSTART="${XDG_CONFIG_HOME:-$HOME/.config}/autostart/sc2-overlay.desktop"
SCHEMA="org.gnome.settings-daemon.plugins.media-keys"
KEY_BASE="/org/gnome/settings-daemon/plugins/media-keys/custom-keybindings"

ASSUME_YES=0; WANT_KEYS=1; UNINSTALL=0
for a in "$@"; do
  case "$a" in
    -y|--yes) ASSUME_YES=1 ;;
    --no-keys) WANT_KEYS=0 ;;
    --uninstall) UNINSTALL=1 ;;
    -h|--help) sed -n '2,9p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "Unknown option: $a (try --help)"; exit 2 ;;
  esac
done

say()  { printf '\n\033[1;34m==>\033[0m \033[1m%s\033[0m\n' "$*"; }
warn() { printf '\033[1;33mnote:\033[0m %s\n' "$*"; }
die()  { printf '\033[1;31merror:\033[0m %s\n' "$*" >&2; exit 1; }
ask() { # ask "question" -> yes unless the answer starts with n; -y always says yes
  [ "$ASSUME_YES" = 1 ] && return 0
  local r; read -r -p "$1 [Y/n] " r || true
  case "$r" in [nN]*) return 1 ;; *) return 0 ;; esac
}

[ "$(id -u)" -ne 0 ] || die "Run this as your normal user, not as root or with sudo."
command -v apt-get >/dev/null 2>&1 || die "This installer needs apt (Ubuntu or Debian). Build by hand with desktop/LINUX.md."
[ "$(uname -m)" = "x86_64" ] || die "This installer builds for x86_64 (64-bit Intel/AMD) only. Found: $(uname -m)."

# ---- GNOME hotkeys: Wayland does not hand global keys to apps, so GNOME runs the commands itself ----
gnome_keys_available() {
  command -v gsettings >/dev/null 2>&1 && command -v python3 >/dev/null 2>&1 || return 1
  local schemas; schemas="$(gsettings list-schemas 2>/dev/null || true)"   # not piped to grep: pipefail + early exit would fail
  [[ "$schemas" == *"$SCHEMA"* ]]
}

edit_key_list() { # edit_key_list add|remove id...   rewrites GNOME's custom-keybindings list
  local mode="$1" cur new; shift
  cur="$(gsettings get "$SCHEMA" custom-keybindings)"
  new="$(python3 - "$mode" "$cur" "$KEY_BASE" "$@" <<'PY'
import ast, sys
mode, cur, base, *ids = sys.argv[1:]
if cur.startswith("@as"):
    cur = cur[3:].strip()
items = ast.literal_eval(cur)
ours = [f"{base}/{i}/" for i in ids]
if mode == "add":
    items += [p for p in ours if p not in items]
else:
    items = [p for p in items if p not in ours]
print(items)
PY
)"
  gsettings set "$SCHEMA" custom-keybindings "$new"
}

set_key() { # set_key id name command binding
  local path="$KEY_BASE/$1/"
  gsettings set "$SCHEMA.custom-keybinding:$path" name "$2"
  gsettings set "$SCHEMA.custom-keybinding:$path" command "$3"
  gsettings set "$SCHEMA.custom-keybinding:$path" binding "$4"
}

add_keys() {
  # keep any binding the user already made for these commands
  local have="" p ids=""
  for p in $(gsettings get "$SCHEMA" custom-keybindings | grep -o "'[^']*'" | tr -d "'" || true); do
    have="$have $(gsettings get "$SCHEMA.custom-keybinding:$p" command 2>/dev/null || true)"
  done
  case "$have" in *"sc2-overlay --arrange"*) echo "An arrange hotkey already exists; leaving it." ;;
    *) ids="$ids sc2-overlay-arrange" ;; esac
  case "$have" in *"sc2-overlay --toggle"*) echo "A show/hide hotkey already exists; leaving it." ;;
    *) ids="$ids sc2-overlay-toggle" ;; esac
  [ -n "$ids" ] || return 0
  # shellcheck disable=SC2086
  edit_key_list add $ids
  case "$ids" in *arrange*) set_key sc2-overlay-arrange "SC2 Overlay: arrange" "sc2-overlay --arrange" "<Control><Shift>F10" ;; esac
  case "$ids" in *toggle*)  set_key sc2-overlay-toggle  "SC2 Overlay: show/hide" "sc2-overlay --toggle" "<Control><Shift>F9" ;; esac
}

remove_keys() {
  gnome_keys_available || return 0
  for id in sc2-overlay-arrange sc2-overlay-toggle; do
    gsettings reset-recursively "$SCHEMA.custom-keybinding:$KEY_BASE/$id/" 2>/dev/null || true
  done
  edit_key_list remove sc2-overlay-arrange sc2-overlay-toggle || true
}

# ---- uninstall ----
if [ "$UNINSTALL" = 1 ]; then
  say "Removing SC2 Overlay"
  pkill -f "$APP_DIR/sc2-overlay" 2>/dev/null || true
  if dpkg -s "$PKG" >/dev/null 2>&1; then sudo apt-get remove -y "$PKG"; else warn "the package is not installed"; fi
  if [ -f "$UDEV_RULES" ]; then sudo rm -f "$UDEV_RULES"; sudo udevadm control --reload-rules || true; fi
  rm -f "$AUTOSTART"
  remove_keys
  echo "Done. Your settings in ~/.config/SC2-Overlay were left alone; delete that folder to remove them too."
  exit 0
fi

# ---- find the source folder (this script sits next to desktop/ and web/) ----
ROOT="$(cd "$(dirname "$0")" && pwd)"
[ -f "$ROOT/desktop/package.json" ] && [ -d "$ROOT/web" ] || die "Run this from the project folder: it must sit next to the desktop/ and web/ folders."

# ---- 1. tools ----
say "1/4  Checking tools"
need=""
command -v curl >/dev/null 2>&1 || need="$need curl"
[ -f /etc/ssl/certs/ca-certificates.crt ] || need="$need ca-certificates"
if [ -n "$need" ]; then
  echo "Installing:$need"
  sudo apt-get update -qq
  # shellcheck disable=SC2086
  sudo apt-get install -y $need
fi

node_ok() { command -v node >/dev/null 2>&1 && [ "$(node -p 'process.versions.node.split(".")[0]')" -ge 18 ]; }
export NVM_DIR="${NVM_DIR:-$HOME/.nvm}"
if ! node_ok && [ -s "$NVM_DIR/nvm.sh" ]; then
  set +u; . "$NVM_DIR/nvm.sh"; set -u
fi
if ! node_ok; then
  echo "Node.js 18 or newer was not found; installing Node 20 with nvm (in your home folder)."
  curl -fsSL https://raw.githubusercontent.com/nvm-sh/nvm/v0.40.1/install.sh | bash
  set +u; . "$NVM_DIR/nvm.sh"; nvm install 20; set -u
fi
node_ok || die "Could not get Node.js 18+."
echo "Node $(node --version), npm $(npm --version)"

# ---- 2. build ----
say "2/4  Building (the first run downloads Electron, a few minutes)"
"$ROOT/desktop/build-linux.sh"
DEB="$(ls -1t "$ROOT"/desktop/dist/*.deb 2>/dev/null | head -n1 || true)"
[ -n "$DEB" ] || die "The build did not produce a .deb."

# ---- 3. install ----
say "3/4  Installing"
pkill -f "$APP_DIR/sc2-overlay" 2>/dev/null || true
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
cp "$DEB" "$TMP/sc2-overlay.deb"; chmod 755 "$TMP"; chmod 644 "$TMP/sc2-overlay.deb"
sudo apt-get install -y --reinstall "$TMP/sc2-overlay.deb"   # --reinstall: same version number as an older build

mode="$(stat -c '%a %U' "$APP_DIR/chrome-sandbox" 2>/dev/null || echo missing)"
[ "$mode" = "4755 root" ] || warn "sandbox helper is '$mode', expected '4755 root'; run:  sudo chown root:root '$APP_DIR/chrome-sandbox' && sudo chmod 4755 '$APP_DIR/chrome-sandbox'"

# ---- 4. hotkeys ----
say "4/4  Hotkeys"
if [ "$WANT_KEYS" = 1 ] && gnome_keys_available; then
  if ask "Add the GNOME hotkeys Ctrl+Shift+F10 (arrange) and Ctrl+Shift+F9 (show/hide)?"; then
    add_keys && echo "Added. (Change them in Settings > Keyboard > Custom Shortcuts.)"
  else
    echo "Skipped."
  fi
else
  echo "Skipped (no GNOME settings found, or --no-keys). Bind these to keys yourself:"
  echo "    sc2-overlay --arrange     sc2-overlay --toggle"
fi

say "Installed"
cat <<EOF
Start it:        sc2-overlay           (or find "SC2 Overlay" in your app menu)
Settings:        tray icon > Remapper and overlay settings...
Arrange mode:    Ctrl+Shift+F10, then drag and scroll to size; leave with the Done button, a right-click or F10 again
Show/hide:       Ctrl+Shift+F9
Unplug and replug the controller once so the device rules apply.
Remove it all:   ./install-linux.sh --uninstall
EOF

if { [ -n "${DISPLAY:-}" ] || [ -n "${WAYLAND_DISPLAY:-}" ]; } && ask "Start it now?"; then
  nohup sc2-overlay >/dev/null 2>&1 &
  disown || true
fi

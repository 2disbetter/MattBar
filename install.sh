#!/usr/bin/env bash
# Install MattBar: build, systemd unit, Hyprland user-config tweaks, and
# (optional) Quickshell takeover. Idempotent — safe to re-run.
#
# Usage:
#   ./install.sh              # bar + OSD + Omarchy shortcut rebinds
#   ./install.sh --takeover   # also shut down Quickshell (full shell)
#   ./install.sh --null-bar   # hide Omarchy's bar; keep the rest of QS
#   ./install.sh --no-build   # skip cmake (binary already installed)
#
# Rebinds Super+Space (launcher) and the other omarchy-menu / omarchy-shell
# shortcuts to mattbarctl so they work with Quickshell down. Personal
# overrides in bindings.lua still win (loaded after).
#
# Does not clone omarchy.battery or change power-profile files.

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
PREFIX=/usr/local
DO_BUILD=1
DO_TAKEOVER=0
DO_NULL_BAR=0
DO_BLUR=0

usage() {
  sed -n '2,14p' "$0" | sed 's/^# \?//'
  exit "${1:-0}"
}

while (($#)); do
  case "$1" in
  --takeover) DO_TAKEOVER=1 ;;
  --null-bar) DO_NULL_BAR=1 ;;
  --no-build) DO_BUILD=0 ;;
  --blur) DO_BLUR=1 ;;
  --prefix)
    PREFIX=${2:?}
    shift
    ;;
  -h | --help) usage 0 ;;
  *)
    echo "unknown option: $1" >&2
    usage 1
    ;;
  esac
  shift
done

log() { printf 'mattbar-install: %s\n' "$*"; }
die() { printf 'mattbar-install: %s\n' "$*" >&2; exit 1; }

need() { command -v "$1" >/dev/null 2>&1 || die "missing command: $1"; }

backup_once() {
  local f=$1
  [[ -f $f ]] || return 0
  [[ -f $f.bak.mattbar ]] && return 0
  cp -a "$f" "$f.bak.mattbar"
  log "backed up $f -> $f.bak.mattbar"
}

# Append $2 to $1 unless the file already contains $3 (grep -F).
append_unless() {
  local file=$1 body=$2 marker=$3
  mkdir -p "$(dirname "$file")"
  if [[ -f $file ]] && grep -Fq "$marker" "$file"; then
    log "already present in $file ($marker)"
    return 0
  fi
  backup_once "$file"
  [[ -f $file && -s $file && $(tail -c1 "$file" | wc -l) -eq 0 ]] && echo >>"$file"
  printf '\n%s\n' "$body" >>"$file"
  log "updated $file"
}

set_conf_key() {
  local file=$1 key=$2 val=$3
  mkdir -p "$(dirname "$file")"
  if [[ -f $file ]] && grep -q "^${key}[[:space:]]*=" "$file"; then
    backup_once "$file"
    sed -i "s|^${key}[[:space:]]*=.*|${key} = ${val}|" "$file"
  else
    [[ -f $file ]] && backup_once "$file"
    printf '%s = %s\n' "$key" "$val" >>"$file"
  fi
}

# Insert `require("hypr.mattbar-shell-keys")` immediately after Omarchy
# defaults so user binds in hypr.bindings still override.
inject_shell_keys_require() {
  local file=$1
  [[ -f $file ]] || return 0
  if grep -Fq 'hypr.mattbar-shell-keys' "$file"; then
    log "require already in $file"
    return 0
  fi
  backup_once "$file"
  if command -v python3 >/dev/null 2>&1; then
    python3 - "$file" <<'PY'
from pathlib import Path
import sys
p = Path(sys.argv[1])
t = p.read_text()
needle = 'require("default.hypr.omarchy")'
block = (
    needle
    + "\n\n-- MattBar: Omarchy shortcuts that called omarchy-menu / omarchy-shell.\n"
    + 'require("hypr.mattbar-shell-keys")'
)
if needle in t:
    p.write_text(t.replace(needle, block, 1))
else:
    p.write_text(t.rstrip() + "\n\n" + block + "\n")
PY
  else
    sed -i '/require("default.hypr.omarchy")/a\
\
-- MattBar: Omarchy shortcuts that called omarchy-menu / omarchy-shell.\
require("hypr.mattbar-shell-keys")
' "$file"
  fi
  log "require hypr.mattbar-shell-keys in $file"
}

comment_hyprlang_source() {
  local conf=$1
  [[ -f $conf ]] || return 0
  grep -q '^[[:space:]]*source = .*mattbar-media-keys' "$conf" || return 0
  backup_once "$conf"
  sed -i 's|^[[:space:]]*source = \(.*mattbar-media-keys.*\)|# source = \1  (moved to bindings.lua)|' "$conf"
  log "commented 3.x media-keys source in $conf"
}

# ---------------------------------------------------------------------------
log "source tree: $ROOT"

need cmake
need sed
[[ -f $ROOT/CMakeLists.txt ]] || die "run this from the MattBar source tree"

if ((DO_BUILD)); then
  need pkg-config
  log "building"
  cmake -B "$ROOT/build" -S "$ROOT" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$PREFIX"
  cmake --build "$ROOT/build" -j"$(nproc)"
  log "installing to $PREFIX (needs root for ${PREFIX}/bin)"
  if [[ -w $PREFIX/bin ]] || [[ $PREFIX == "$HOME"* ]]; then
    cmake --install "$ROOT/build"
  else
    sudo cmake --install "$ROOT/build"
  fi
else
  [[ -x $PREFIX/bin/mattbar ]] || die "$PREFIX/bin/mattbar not found; omit --no-build"
fi

# ---------------------------------------------------------------------------
log "systemd user unit"
mkdir -p "$HOME/.config/systemd/user"
cp "$ROOT/contrib/mattbar.service" "$HOME/.config/systemd/user/mattbar.service"
if [[ $PREFIX != /usr/local ]]; then
  sed -i "s|/usr/local/bin/mattbar|${PREFIX}/bin/mattbar|" \
    "$HOME/.config/systemd/user/mattbar.service"
fi
systemctl --user daemon-reload
systemctl --user enable mattbar.service
systemctl --user restart mattbar.service || systemctl --user start mattbar.service

# ---------------------------------------------------------------------------
SHIM_DIR=$HOME/.config/mattbar/shims
mkdir -p "$SHIM_DIR"
cp "$ROOT/contrib/omarchy-4x-media-keys/shims/omarchy-osd" "$SHIM_DIR/omarchy-osd"
chmod +x "$SHIM_DIR/omarchy-osd"
log "OSD shim -> $SHIM_DIR/omarchy-osd"

CONF=$HOME/.config/mattbar/mattbar.conf
set_conf_key "$CONF" enable_osd true
set_conf_key "$CONF" enable_notifications true
set_conf_key "$CONF" notifications_takeover true
if ((DO_TAKEOVER)); then
  set_conf_key "$CONF" quickshell_shutdown true
  log "takeover on (quickshell_shutdown = true)"
fi

# ---------------------------------------------------------------------------
HYPR=$HOME/.config/hypr
LUA_MAIN=$HYPR/hyprland.lua
LUA_BIND=$HYPR/bindings.lua
LUA_LOOK=$HYPR/looknfeel.lua

if [[ -f $LUA_MAIN ]]; then
  append_unless "$LUA_MAIN" \
'-- MattBar outside-click dismiss surface: keep it instant (the TUI card
-- is a separate small popup).
hl.layer_rule({ match = { namespace = "mattbar-dismiss" }, no_anim = true, animation = "none" })' \
    'namespace = "mattbar-dismiss"'

  if ((DO_BLUR)); then
    append_unless "$LUA_LOOK" \
'hl.layer_rule({ match = { namespace = "mattbar" }, blur = true })' \
      'namespace = "mattbar"'
  fi

  cp "$ROOT/contrib/omarchy-4x-shell-keys.lua" "$HYPR/mattbar-shell-keys.lua"
  log "shell shortcut binds -> $HYPR/mattbar-shell-keys.lua"
  inject_shell_keys_require "$LUA_MAIN"

  append_unless "$LUA_BIND" \
    "$(cat "$ROOT/contrib/omarchy-4x-media-keys/mattbar-media-keys.lua")" \
    'MattBar media keys for Omarchy 4'

  comment_hyprlang_source "$HYPR/hyprland.conf"
elif [[ -f $HYPR/hyprland.conf ]]; then
  log "Omarchy 3.x / hyprlang config"
  KEYS=$HYPR/mattbar-media-keys.conf
  cp "$ROOT/contrib/omarchy-3x-media-keys/mattbar-media-keys.conf" "$KEYS"
  append_unless "$HYPR/hyprland.conf" \
    "source = $KEYS" \
    'mattbar-media-keys.conf'
else
  log "no ~/.config/hypr found; skipping compositor tweaks"
fi

# ---------------------------------------------------------------------------
if ((DO_NULL_BAR)) && ((DO_TAKEOVER == 0)); then
  if command -v jq >/dev/null 2>&1 && [[ -n ${OMARCHY_PATH:-} || -d /usr/share/omarchy ]]; then
    OMARCHY_PATH=${OMARCHY_PATH:-/usr/share/omarchy}
    mkdir -p "$HOME/.config/omarchy/plugins"
    rm -rf "$HOME/.config/omarchy/plugins/mattbar.null-bar"
    cp -a "$ROOT/contrib/omarchy-null-bar" "$HOME/.config/omarchy/plugins/mattbar.null-bar"
    SHELL_JSON=$HOME/.config/omarchy/shell.json
    if [[ ! -f $SHELL_JSON ]]; then
      cp "$OMARCHY_PATH/config/omarchy/shell.json" "$SHELL_JSON"
    fi
    backup_once "$SHELL_JSON"
    tmp=$(mktemp)
    jq '.bar.id = "mattbar.null-bar"' "$SHELL_JSON" >"$tmp"
    mv "$tmp" "$SHELL_JSON"
    log "null-bar plugin enabled in $SHELL_JSON"
  else
    log "skipping --null-bar (jq or Omarchy not available)"
  fi
fi

# ---------------------------------------------------------------------------
if command -v hyprctl >/dev/null 2>&1 && [[ -n ${HYPRLAND_INSTANCE_SIGNATURE:-} || -n ${WAYLAND_DISPLAY:-} ]]; then
  log "reloading Hyprland"
  hyprctl reload >/dev/null || true
  if command -v hyprctl >/dev/null; then
    err=$(hyprctl configerrors 2>/dev/null || true)
    if [[ -n $err && $err != "ok" && $err != *"no errors"* ]]; then
      printf '%s\n' "$err" >&2
      log "hyprctl configerrors reported the above; fix those before relying on binds"
    fi
  fi
fi

# Restart so conf keys (takeover, OSD) apply now.
systemctl --user restart mattbar.service

log "done"
systemctl --user --no-pager --full status mattbar.service | head -n 12 || true
if ((DO_TAKEOVER)); then
  log "Quickshell takeover is on. Toggle off in Settings if you need QS back."
fi

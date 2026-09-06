#!/usr/bin/env bash
# =============================================================================
# MattBar installer (binary release + source tree)
# =============================================================================
# For non-developers: download the prebuilt `mattbar` binary and this script,
# put them in the same folder, then run:
#
#   chmod +x install.sh mattbar
#   ./install.sh
#
# You will be prompted for your password only when the binary is copied into
# /usr/local/bin. Do NOT run the whole script as root — user config and the
# systemd *user* unit must land in your own home directory.
#
# If you already used sudo by mistake, the script recovers via $SUDO_USER.
#
# Options:
#   ./install.sh                 # install / update (default)
#   ./install.sh --takeover      # full Quickshell replacement
#   ./install.sh --null-bar      # hide Omarchy bar only; keep Quickshell
#   ./install.sh --blur          # Hyprland blur on the bar layer
#   ./install.sh --prefix DIR    # binary install prefix (default /usr/local)
#   ./install.sh --uninstall     # same as ./uninstall.sh
#   ./install.sh --help
# =============================================================================

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
PREFIX=/usr/local
DO_TAKEOVER=0
DO_NULL_BAR=0
DO_BLUR=0

usage() {
  sed -n '2,26p' "$0" | sed 's/^# \?//'
  exit "${1:-0}"
}

while (($#)); do
  case "$1" in
  --takeover)  DO_TAKEOVER=1 ;;
  --null-bar)  DO_NULL_BAR=1 ;;
  --blur)      DO_BLUR=1 ;;
  --uninstall)
    shift
    extra=()
    has_prefix=0
    for a in "$@"; do
      [[ $a == --prefix ]] && has_prefix=1
    done
    if ((has_prefix == 0)); then
      extra=(--prefix "$PREFIX")
    fi
    exec bash "$ROOT/uninstall.sh" "${extra[@]}" "$@"
    ;;
  --prefix)
    PREFIX=${2:?--prefix needs a path}
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

log()  { printf 'mattbar-install: %s\n' "$*"; }
warn() { printf 'mattbar-install: WARNING: %s\n' "$*" >&2; }
die()  { printf 'mattbar-install: %s\n' "$*" >&2; exit 1; }

# ---------------------------------------------------------------------------
# Real user (never install user units into /root)
# ---------------------------------------------------------------------------
if [[ ${EUID:-$(id -u)} -eq 0 ]]; then
  if [[ -n ${SUDO_USER:-} && $SUDO_USER != root ]]; then
    REAL_USER=$SUDO_USER
    REAL_HOME=$(getent passwd "$SUDO_USER" | cut -d: -f6)
    log "running as root via sudo — applying user config for $REAL_USER ($REAL_HOME)"
  else
    die "do not run this script as root.
  Put mattbar and install.sh in the same folder, then run:

    ./install.sh

  Your password is only needed when copying the binary to $PREFIX/bin."
  fi
else
  REAL_USER=$(id -un)
  REAL_HOME=$HOME
fi

export HOME=$REAL_HOME
export XDG_CONFIG_HOME="${XDG_CONFIG_HOME:-$REAL_HOME/.config}"
export XDG_STATE_HOME="${XDG_STATE_HOME:-$REAL_HOME/.local/state}"
export XDG_DATA_HOME="${XDG_DATA_HOME:-$REAL_HOME/.local/share}"

run_as_user() {
  if [[ ${EUID:-$(id -u)} -eq 0 ]]; then
    sudo -u "$REAL_USER" -- "$@"
  else
    "$@"
  fi
}

# ---------------------------------------------------------------------------
backup_once() {
  local f=$1
  [[ -f $f ]] || return 0
  [[ -f $f.bak.mattbar ]] && return 0
  cp -a "$f" "$f.bak.mattbar"
  log "backed up $f -> $f.bak.mattbar"
}

append_unless() {
  local file=$1 body=$2 marker=$3
  mkdir -p "$(dirname "$file")"
  if [[ -f $file ]] && grep -Fq "$marker" "$file"; then
    log "already present in $file"
    return 0
  fi
  backup_once "$file"
  if [[ -f $file && -s $file ]] && [[ $(tail -c1 "$file" | wc -l) -eq 0 ]]; then
    echo >>"$file"
  fi
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

# ---------------------------------------------------------------------------
find_binary() {
  local candidates=(
    "$ROOT/mattbar"
    "$ROOT/bin/mattbar"
    "$PREFIX/bin/mattbar"
    "$(command -v mattbar 2>/dev/null || true)"
  )
  local c
  for c in "${candidates[@]}"; do
    [[ -n $c && -x $c && -f $c ]] || continue
    if [[ $c == "$ROOT/mattbar" || $c == "$ROOT/bin/mattbar" ]]; then
      echo "$c"
      return 0
    fi
  done
  for c in "${candidates[@]}"; do
    [[ -n $c && -x $c && -f $c ]] || continue
    echo "$c"
    return 0
  done
  return 1
}

# ---------------------------------------------------------------------------
# Embedded assets when contrib/ is not shipped next to the binary
# ---------------------------------------------------------------------------
write_service() {
  local dest=$1
  local binpath=$2
  cat >"$dest" <<EOF
[Unit]
Description=MattBar status bar
PartOf=graphical-session.target
After=graphical-session.target
StartLimitIntervalSec=60
StartLimitBurst=60

[Service]
Type=notify
NotifyAccess=main
ExecStart=${binpath}
Environment=PATH=${PREFIX}/bin:/usr/local/bin:/usr/bin
KillMode=process
TimeoutStopSec=8
Restart=always
RestartSec=500ms
WatchdogSec=30
Slice=session.slice

[Install]
WantedBy=graphical-session.target
EOF
}

write_osd_shim() {
  local dest=$1
  cat >"$dest" <<'EOF'
#!/bin/sh
# No-op omarchy-osd. Prepended to PATH only for media keybinds so Omarchy
# scripts still do the work while MattBar draws the only OSD.
exit 0
EOF
  chmod +x "$dest"
}

write_media_keys_lua() {
  local dest=$1
  cat >"$dest" <<'EOF'
-- MattBar media keys for Omarchy 4 ("Quattro").
local no_osd = 'env PATH="' .. os.getenv("HOME")
    .. '/.config/mattbar/shims:$PATH" '

hl.unbind("XF86AudioRaiseVolume")
hl.unbind("XF86AudioLowerVolume")
hl.unbind("XF86AudioMute")
hl.unbind("ALT + XF86AudioRaiseVolume")
hl.unbind("ALT + XF86AudioLowerVolume")
o.bind("XF86AudioRaiseVolume", "Volume up",
  no_osd .. "omarchy-audio-output-volume raise",
  { locked = true, repeating = true })
o.bind("XF86AudioLowerVolume", "Volume down",
  no_osd .. "omarchy-audio-output-volume lower",
  { locked = true, repeating = true })
o.bind("XF86AudioMute", "Mute",
  no_osd .. "omarchy-audio-output-volume mute-toggle",
  { locked = true })
o.bind("ALT + XF86AudioRaiseVolume", "Volume up precise",
  no_osd .. "omarchy-audio-output-volume +1",
  { locked = true, repeating = true })
o.bind("ALT + XF86AudioLowerVolume", "Volume down precise",
  no_osd .. "omarchy-audio-output-volume -1",
  { locked = true, repeating = true })

hl.unbind("XF86AudioMicMute")
o.bind("XF86AudioMicMute", "Mute microphone",
  no_osd .. "omarchy-audio-input-mute",
  { locked = true })

hl.unbind("XF86MonBrightnessUp")
hl.unbind("XF86MonBrightnessDown")
o.bind("XF86MonBrightnessUp", "Brightness up",
  "omarchy-brightness-display --no-osd +5%",
  { locked = true, repeating = true })
o.bind("XF86MonBrightnessDown", "Brightness down",
  "omarchy-brightness-display --no-osd 5%-",
  { locked = true, repeating = true })
EOF
}

write_shell_keys_lua() {
  local dest=$1
  cat >"$dest" <<'EOF'
-- MattBar replacements for Omarchy shortcuts (omarchy-menu / omarchy-shell).
local mb = "mattbarctl"

-- --- notifications --------------------------------------------------------
hl.unbind("SUPER + comma")
hl.unbind("SUPER + SHIFT + comma")
hl.unbind("SUPER + CTRL + comma")
hl.unbind("SUPER + ALT + comma")
hl.unbind("SUPER + SHIFT + ALT + comma")
o.bind("SUPER + comma", "Dismiss last notification",
  mb .. " notifications dismissOne")
o.bind("SUPER + SHIFT + comma", "Dismiss all notifications",
  mb .. " notifications dismissAll")
o.bind("SUPER + CTRL + comma", "Toggle do-not-disturb",
  mb .. " notifications toggleDnd")
o.bind("SUPER + ALT + comma", "Invoke last notification",
  mb .. " notifications invokeLast")
o.bind("SUPER + SHIFT + ALT + comma", "Open notification history",
  mb .. " notifications showHistory")

-- --- launcher / menus -----------------------------------------------------
hl.unbind("SUPER + SPACE")
o.bind("SUPER + SPACE", "Omarchy menu", mb .. " shell toggle omarchy.menu")
hl.unbind("SUPER + ALT + SPACE")
o.bind("SUPER + ALT + SPACE", "Apps menu",
  mb .. " shell toggle omarchy.menu '{\"menu\":\"apps\"}'")
hl.unbind("SUPER + ESCAPE")
o.bind("SUPER + ESCAPE", "System menu",
  mb .. " shell toggle omarchy.menu '{\"menu\":\"system\"}'")
EOF
}

write_media_keys_conf() {
  local dest=$1
  cat >"$dest" <<'EOF'
# MattBar media keys for Omarchy 3.x / plain Hyprland
unbind = , XF86AudioRaiseVolume
unbind = , XF86AudioLowerVolume
unbind = , XF86AudioMute
bindel = , XF86AudioRaiseVolume, exec, wpctl set-volume -l 1.5 @DEFAULT_AUDIO_SINK@ 5%+
bindel = , XF86AudioLowerVolume, exec, wpctl set-volume @DEFAULT_AUDIO_SINK@ 5%-
bindl  = , XF86AudioMute, exec, wpctl set-mute @DEFAULT_AUDIO_SINK@ toggle
unbind = , XF86AudioMicMute
bindl  = , XF86AudioMicMute, exec, wpctl set-mute @DEFAULT_AUDIO_SOURCE@ toggle
unbind = , XF86MonBrightnessUp
unbind = , XF86MonBrightnessDown
bindel = , XF86MonBrightnessUp, exec, brightnessctl set +5%
bindel = , XF86MonBrightnessDown, exec, brightnessctl set 5%-
EOF
}

# Style → Menu Bar → MattBar settings. User overlay only — never
# /usr/share/omarchy/. JSONC, so we splice before the last `}`.
install_menu_entry() {
  local file=$REAL_HOME/.config/omarchy/extensions/omarchy-menu.jsonc
  mkdir -p "$(dirname "$file")"
  if [[ -f $file ]] && grep -Fq '"style.bar.mattbar"' "$file"; then
    log "already present in $file"
    return 0
  fi
  if [[ ! -f $file ]]; then
    cat >"$file" <<'EOF'
{
  "style.bar.mattbar": {"icon":"","label":"MattBar settings","action":"mattbarctl settings"},
}
EOF
    log "created $file"
    return 0
  fi
  if command -v python3 >/dev/null 2>&1; then
    python3 - "$file" <<'PY'
from pathlib import Path
import sys
p = Path(sys.argv[1])
t = p.read_text()
if '"style.bar.mattbar"' in t:
    sys.exit(0)
entry = '  "style.bar.mattbar": {"icon":"","label":"MattBar settings","action":"mattbarctl settings"},\n'
idx = t.rfind("}")
if idx < 0:
    p.write_text("{\n" + entry + "}\n")
    sys.exit(0)
head = t[:idx]
check = head.rstrip()
while True:
    nl = check.rfind("\n")
    last = check[nl + 1 :] if nl >= 0 else check
    s = last.strip()
    if s == "" or s.startswith("//"):
        check = check[:nl] if nl >= 0 else ""
        continue
    break
tail = check.rstrip()
need_comma = bool(tail) and not tail.endswith("{") and not tail.endswith(",")
insert = (",\n" if need_comma else "") + "\n" + entry
p.write_text(t[:idx] + insert + t[idx:])
PY
    log "updated $file"
  else
    printf '\n  "style.bar.mattbar": {"icon":"","label":"MattBar settings","action":"mattbarctl settings"},\n' >>"$file"
    log "appended MattBar settings to $file"
  fi
}

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
    + "\n\n-- MattBar: Omarchy shortcuts\n"
    + 'require("hypr.mattbar-shell-keys")'
)
if needle in t:
    p.write_text(t.replace(needle, block, 1))
else:
    p.write_text(t.rstrip() + "\n\n" + block + "\n")
PY
  else
    printf '\n-- MattBar: Omarchy shortcuts\nrequire("hypr.mattbar-shell-keys")\n' >>"$file"
  fi
  log "require hypr.mattbar-shell-keys in $file"
}

# ---------------------------------------------------------------------------
log "installing for user $REAL_USER (home $REAL_HOME)"
log "prefix: $PREFIX"

SRC_BIN=$(find_binary) || die "no mattbar binary found.
  Place the downloaded 'mattbar' file next to this script and run:

    chmod +x mattbar install.sh
    ./install.sh
"
log "binary: $SRC_BIN ($("$SRC_BIN" --version 2>/dev/null || echo unknown))"

# ---------------------------------------------------------------------------
DEST_BIN=$PREFIX/bin/mattbar
need_copy=1
if [[ -x $DEST_BIN ]] && cmp -s "$SRC_BIN" "$DEST_BIN" 2>/dev/null; then
  need_copy=0
  log "binary already up to date at $DEST_BIN"
fi

if ((need_copy)); then
  log "installing binary to $DEST_BIN"
  if [[ -d ${PREFIX}/bin && -w ${PREFIX}/bin ]] || [[ $PREFIX == "$REAL_HOME"* ]]; then
    mkdir -p "$PREFIX/bin"
    install -m 755 "$SRC_BIN" "$DEST_BIN"
    ln -sfn mattbar "$PREFIX/bin/mattbarctl"
  else
    if [[ ${EUID:-$(id -u)} -eq 0 ]]; then
      mkdir -p "$PREFIX/bin"
      install -m 755 "$SRC_BIN" "$DEST_BIN"
      ln -sfn mattbar "$PREFIX/bin/mattbarctl"
    else
      log "password required to install into $PREFIX/bin"
      sudo mkdir -p "$PREFIX/bin"
      sudo install -m 755 "$SRC_BIN" "$DEST_BIN"
      sudo ln -sfn mattbar "$PREFIX/bin/mattbarctl"
    fi
  fi
  log "installed $DEST_BIN and mattbarctl symlink"
fi

# ---------------------------------------------------------------------------
UNIT_DIR=$REAL_HOME/.config/systemd/user
mkdir -p "$UNIT_DIR"
UNIT=$UNIT_DIR/mattbar.service
if [[ -f $ROOT/contrib/mattbar.service ]]; then
  cp "$ROOT/contrib/mattbar.service" "$UNIT"
  sed -i "s|/usr/local/bin/mattbar|${DEST_BIN}|g" "$UNIT"
else
  write_service "$UNIT" "$DEST_BIN"
fi
if ! grep -q "Environment=PATH=" "$UNIT"; then
  sed -i "/^\[Service\]/a Environment=PATH=${PREFIX}/bin:/usr/local/bin:/usr/bin" "$UNIT" || true
fi

if [[ ${EUID:-$(id -u)} -eq 0 ]]; then
  run_as_user systemctl --user daemon-reload
  run_as_user systemctl --user enable mattbar.service
  run_as_user systemctl --user restart mattbar.service \
    || run_as_user systemctl --user start mattbar.service
else
  systemctl --user daemon-reload
  systemctl --user enable mattbar.service
  systemctl --user restart mattbar.service 2>/dev/null \
    || systemctl --user start mattbar.service
fi
log "systemd user unit enabled"

# ---------------------------------------------------------------------------
SHIM_DIR=$REAL_HOME/.config/mattbar/shims
mkdir -p "$SHIM_DIR"
if [[ -f $ROOT/contrib/omarchy-4x-media-keys/shims/omarchy-osd ]]; then
  cp "$ROOT/contrib/omarchy-4x-media-keys/shims/omarchy-osd" "$SHIM_DIR/omarchy-osd"
  chmod +x "$SHIM_DIR/omarchy-osd"
else
  write_osd_shim "$SHIM_DIR/omarchy-osd"
fi
log "OSD shim -> $SHIM_DIR/omarchy-osd"

CONF=$REAL_HOME/.config/mattbar/mattbar.conf
set_conf_key "$CONF" enable_osd true
set_conf_key "$CONF" enable_notifications true
set_conf_key "$CONF" notifications_takeover true
if ((DO_TAKEOVER)); then
  set_conf_key "$CONF" quickshell_shutdown true
  log "takeover on (quickshell_shutdown = true)"
fi

# ---------------------------------------------------------------------------
HYPR=$REAL_HOME/.config/hypr
LUA_MAIN=$HYPR/hyprland.lua
LUA_BIND=$HYPR/bindings.lua
LUA_LOOK=$HYPR/looknfeel.lua

if [[ -f $LUA_MAIN ]]; then
  log "Omarchy 4 / Quattro detected"

  append_unless "$LUA_MAIN" \
'hl.layer_rule({ match = { namespace = "mattbar-dismiss" }, no_anim = true, animation = "none" })' \
    'namespace = "mattbar-dismiss"'

  if ((DO_BLUR)); then
    append_unless "${LUA_LOOK:-$HYPR/looknfeel.lua}" \
'hl.layer_rule({ match = { namespace = "mattbar" }, blur = true })' \
      'namespace = "mattbar"'
  fi

  if [[ -f $ROOT/contrib/omarchy-4x-shell-keys.lua ]]; then
    cp "$ROOT/contrib/omarchy-4x-shell-keys.lua" "$HYPR/mattbar-shell-keys.lua"
  else
    write_shell_keys_lua "$HYPR/mattbar-shell-keys.lua"
  fi
  inject_shell_keys_require "$LUA_MAIN"

  MEDIA_TMP=$(mktemp)
  if [[ -f $ROOT/contrib/omarchy-4x-media-keys/mattbar-media-keys.lua ]]; then
    cp "$ROOT/contrib/omarchy-4x-media-keys/mattbar-media-keys.lua" "$MEDIA_TMP"
  else
    write_media_keys_lua "$MEDIA_TMP"
  fi
  append_unless "$LUA_BIND" "$(cat "$MEDIA_TMP")" 'MattBar media keys for Omarchy 4'
  rm -f "$MEDIA_TMP"

elif [[ -f $HYPR/hyprland.conf ]]; then
  log "Omarchy 3.x / plain Hyprland detected"
  KEYS=$HYPR/mattbar-media-keys.conf
  if [[ -f $ROOT/contrib/omarchy-3x-media-keys/mattbar-media-keys.conf ]]; then
    cp "$ROOT/contrib/omarchy-3x-media-keys/mattbar-media-keys.conf" "$KEYS"
  else
    write_media_keys_conf "$KEYS"
  fi
  append_unless "$HYPR/hyprland.conf" "source = $KEYS" 'mattbar-media-keys.conf'

  append_unless "$HYPR/hyprland.conf" \
'# MattBar notification keybinds
unbind = SUPER, comma
unbind = SUPER SHIFT, comma
unbind = SUPER CTRL, comma
unbind = SUPER ALT, comma
bind = SUPER, comma, exec, pkill -RTMIN+2 mattbar
bind = SUPER SHIFT, comma, exec, pkill -RTMIN+3 mattbar
bind = SUPER CTRL, comma, exec, pkill -RTMIN+5 mattbar
bind = SUPER ALT, comma, exec, pkill -RTMIN+4 mattbar' \
    'pkill -RTMIN+2 mattbar'
else
  log "no ~/.config/hypr found — bar will still start via systemd"
fi

# ---------------------------------------------------------------------------
# Always drop the null-bar plugin files when we have them. The qs_plugins
# sidecar refuses to start without this directory present. Rewriting
# shell.json (so a still-running Quickshell hides its own bar) stays
# opt-in via --null-bar and is skipped during full takeover.
if [[ -d $ROOT/contrib/omarchy-null-bar ]]; then
  mkdir -p "$REAL_HOME/.config/omarchy/plugins"
  rm -rf "$REAL_HOME/.config/omarchy/plugins/mattbar.null-bar"
  cp -a "$ROOT/contrib/omarchy-null-bar" "$REAL_HOME/.config/omarchy/plugins/mattbar.null-bar"
  log "null-bar plugin files -> $REAL_HOME/.config/omarchy/plugins/mattbar.null-bar"
fi
# Plugin row (mattbar.plugin-bar): files only. Sidecar selects it when
# Settings → Plugin row is on; we never rewrite the user's bar.id.
if [[ -d $ROOT/contrib/omarchy-plugin-bar ]]; then
  mkdir -p "$REAL_HOME/.config/omarchy/plugins"
  rm -rf "$REAL_HOME/.config/omarchy/plugins/mattbar.plugin-bar"
  cp -a "$ROOT/contrib/omarchy-plugin-bar" "$REAL_HOME/.config/omarchy/plugins/mattbar.plugin-bar"
  log "plugin-bar files -> $REAL_HOME/.config/omarchy/plugins/mattbar.plugin-bar"
fi

if ((DO_NULL_BAR)) && ((DO_TAKEOVER == 0)); then
  if command -v jq >/dev/null 2>&1 && [[ -d /usr/share/omarchy || -n ${OMARCHY_PATH:-} ]]; then
    OMARCHY_PATH=${OMARCHY_PATH:-/usr/share/omarchy}
    if [[ -d $REAL_HOME/.config/omarchy/plugins/mattbar.null-bar ]]; then
      SHELL_JSON=$REAL_HOME/.config/omarchy/shell.json
      [[ -f $SHELL_JSON ]] || cp "$OMARCHY_PATH/config/omarchy/shell.json" "$SHELL_JSON"
      backup_once "$SHELL_JSON"
      tmp=$(mktemp)
      jq '.bar.id = "mattbar.null-bar"' "$SHELL_JSON" >"$tmp"
      mv "$tmp" "$SHELL_JSON"
      log "null-bar plugin enabled in shell.json"
      command -v omarchy-restart-shell >/dev/null && omarchy-restart-shell || true
    else
      warn "--null-bar needs the full release (contrib/omarchy-null-bar)"
    fi
  else
    warn "skipping --null-bar shell.json rewrite (need jq + Omarchy)"
  fi
fi

# ---------------------------------------------------------------------------
install_menu_entry

# ---------------------------------------------------------------------------
if command -v hyprctl >/dev/null 2>&1; then
  if [[ -n ${HYPRLAND_INSTANCE_SIGNATURE:-} || -n ${WAYLAND_DISPLAY:-} ]]; then
    log "reloading Hyprland"
    run_as_user hyprctl reload >/dev/null 2>&1 || hyprctl reload >/dev/null 2>&1 || true
  fi
fi

run_as_user systemctl --user restart mattbar.service 2>/dev/null || true

log "────────────────────────────────────────"
log "done"
"$DEST_BIN" --version 2>/dev/null || true
run_as_user systemctl --user --no-pager --full status mattbar.service 2>/dev/null | head -n 12 || true

cat <<EOF

Verify:
  systemctl --user status mattbar
  notify-send "MattBar test"
  # volume keys should show one OSD with a percentage

Update later:
  Download the new mattbar binary into the same folder as install.sh
  and run ./install.sh again.

Uninstall:
  ./uninstall.sh
  # or: ./install.sh --uninstall
EOF

if ((DO_TAKEOVER)); then
  log "Quickshell takeover is ON. Turn off in Settings if you need QS back."
fi

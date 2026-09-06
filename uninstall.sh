#!/usr/bin/env bash
# =============================================================================
# MattBar uninstaller
# =============================================================================
# Stops MattBar, removes the binary and user unit, and reverses every
# Omarchy / Hyprland customization install.sh made. Does not edit
# /usr/share/omarchy/. Personal Hyprland binds (bindings.lua lines that
# were not appended by the installer) are left alone.
#
#   ./uninstall.sh              # interactive
#   ./uninstall.sh --yes        # no prompt
#   ./uninstall.sh --dry-run    # print what would change
#   ./uninstall.sh --keep-config
#   ./uninstall.sh --prefix DIR
#   ./uninstall.sh --help
# =============================================================================

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
PREFIX=/usr/local
DRY_RUN=0
ASSUME_YES=0
KEEP_CONFIG=0

usage() {
  sed -n '2,16p' "$0" | sed 's/^# \?//'
  exit "${1:-0}"
}

while (($#)); do
  case "$1" in
  --prefix)
    PREFIX=${2:?--prefix needs a path}
    shift
    ;;
  --dry-run)     DRY_RUN=1 ;;
  --yes | -y)    ASSUME_YES=1 ;;
  --keep-config) KEEP_CONFIG=1 ;;
  -h | --help)   usage 0 ;;
  *)
    echo "unknown option: $1" >&2
    usage 1
    ;;
  esac
  shift
done

log()  { printf 'mattbar-uninstall: %s\n' "$*"; }
warn() { printf 'mattbar-uninstall: WARNING: %s\n' "$*" >&2; }
die()  { printf 'mattbar-uninstall: %s\n' "$*" >&2; exit 1; }

# ---------------------------------------------------------------------------
if [[ ${EUID:-$(id -u)} -eq 0 ]]; then
  if [[ -n ${SUDO_USER:-} && $SUDO_USER != root ]]; then
    REAL_USER=$SUDO_USER
    REAL_HOME=$(getent passwd "$SUDO_USER" | cut -d: -f6)
    log "running as root via sudo — removing user config for $REAL_USER ($REAL_HOME)"
  else
    die "do not run this script as root.
  Run:

    ./uninstall.sh

  Your password is only needed when deleting the binary from $PREFIX/bin."
  fi
else
  REAL_USER=$(id -un)
  REAL_HOME=$HOME
fi

export HOME=$REAL_HOME
export XDG_CONFIG_HOME="${XDG_CONFIG_HOME:-$REAL_HOME/.config}"
export XDG_STATE_HOME="${XDG_STATE_HOME:-$REAL_HOME/.local/state}"
export XDG_DATA_HOME="${XDG_DATA_HOME:-$REAL_HOME/.local/share}"
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u "$REAL_USER")}"

run_as_user() {
  if [[ ${EUID:-$(id -u)} -eq 0 ]]; then
    sudo -u "$REAL_USER" -- "$@"
  else
    "$@"
  fi
}

do_rm() {
  local f
  for f in "$@"; do
    [[ -e $f || -L $f ]] || continue
    if ((DRY_RUN)); then
      log "dry-run: rm $f"
    else
      rm -f "$f"
      log "removed $f"
    fi
  done
}

do_rm_rf() {
  local d
  for d in "$@"; do
    [[ -e $d || -L $d ]] || continue
    if ((DRY_RUN)); then
      log "dry-run: rm -rf $d"
    else
      rm -rf "$d"
      log "removed $d"
    fi
  done
}

# Privileged unlink (binary in /usr/local/bin).
do_rm_priv() {
  local f=$1
  [[ -e $f || -L $f ]] || return 0
  if ((DRY_RUN)); then
    log "dry-run: rm $f"
    return 0
  fi
  if [[ -w $(dirname "$f") ]]; then
    rm -f "$f"
  elif [[ ${EUID:-$(id -u)} -eq 0 ]]; then
    rm -f "$f"
  else
    log "password required to remove $f"
    sudo rm -f "$f"
  fi
  log "removed $f"
}

edit_file() {
  local file=$1
  [[ -f $file ]] || return 0
  if ! command -v python3 >/dev/null 2>&1; then
    warn "python3 not found; skip edit of $file"
    return 0
  fi
  python3 - "$file" "$DRY_RUN" <<'PY'
from pathlib import Path
import json
import re
import sys

path = Path(sys.argv[1])
dry = sys.argv[2] == "1"
old = path.read_text()
new = old
kind = path.name


def drop_from_marker(text, marker):
    out = []
    skipping = False
    for line in text.splitlines(True):
        if not skipping and marker in line:
            skipping = True
            while out and out[-1].strip() == "":
                out.pop()
            continue
        if skipping:
            continue
        out.append(line)
    return "".join(out)


def drop_preceding_comments(out):
    saved = list(out)
    while out and out[-1].strip() == "":
        out.pop()
    comments = []
    while out:
        s = out[-1].lstrip()
        if s.startswith("--") or s.startswith("#"):
            comments.append(out.pop())
        else:
            break
    joined = "".join(reversed(comments))
    if comments and ("MattBar" in joined or "mattbar" in joined):
        return
    out[:] = saved


if kind == "hyprland.lua":
    out = []
    for s in old.splitlines(True):
        if re.search(r'require\(\s*["\']hypr\.mattbar-shell-keys["\']\s*\)', s):
            drop_preceding_comments(out)
            continue
        if 'namespace = "mattbar-dismiss"' in s or "namespace = 'mattbar-dismiss'" in s:
            drop_preceding_comments(out)
            continue
        if re.search(r'namespace\s*=\s*["\']mattbar["\']', s) and "layer_rule" in s:
            drop_preceding_comments(out)
            continue
        out.append(s)
    new = re.sub(r"\n{3,}", "\n\n", "".join(out))

elif kind == "looknfeel.lua":
    out = []
    for s in old.splitlines(True):
        if re.search(r'namespace\s*=\s*["\']mattbar(-dismiss)?["\']', s) and "layer_rule" in s:
            drop_preceding_comments(out)
            continue
        out.append(s)
    new = "".join(out)

elif kind == "bindings.lua":
    new = drop_from_marker(old, "MattBar media keys")

elif kind in ("hyprland.conf", "autostart.conf"):
    new = drop_from_marker(old, "MattBar OSD media keys")
    new = drop_from_marker(new, "MattBar notification keybinds")
    keep = []
    for line in new.splitlines(True):
        if "mattbar-media-keys.conf" in line:
            continue
        if re.search(r"exec-once\s*=.*mattbar", line, re.I):
            continue
        if re.search(r"layerrule\s*=.*\bmattbar\b", line):
            continue
        if "pkill -RTMIN+" in line and "mattbar" in line:
            continue
        keep.append(line)
    new = re.sub(r"\n{3,}", "\n\n", "".join(keep))

elif kind == "autostart.lua":
    keep = []
    for line in old.splitlines(True):
        if re.search(r"mattbar", line, re.I) and not line.lstrip().startswith("--"):
            continue
        keep.append(line)
    new = "".join(keep)

elif kind == "omarchy-menu.jsonc":
    out = []
    for line in old.splitlines(True):
        if '"style.bar.mattbar"' in line:
            if out and out[-1].strip().startswith("//") and "MattBar" in out[-1]:
                out.pop()
            if out and out[-1].strip() == "":
                out.pop()
            continue
        out.append(line)
    new = "".join(out)

elif kind == "shell.json":
    bak = path.with_name("shell.json.mattbar-sidecar-bak")
    if bak.is_file():
        new = bak.read_text()
        if not dry:
            bak.unlink()
            print("mattbar-uninstall: restored", path, "from sidecar backup")
    try:
        data = json.loads(new)
    except json.JSONDecodeError:
        data = None
    if isinstance(data, dict):
        bar = data.get("bar")
        if isinstance(bar, dict) and bar.get("id") == "mattbar.null-bar":
            bar["id"] = "omarchy.bar"
            data["bar"] = bar
        dis = data.get("disabledPlugins")
        if isinstance(dis, list) and "omarchy.notifications" in dis:
            data["disabledPlugins"] = [x for x in dis if x != "omarchy.notifications"]
        new = json.dumps(data, indent=2) + "\n"

if new == old:
    sys.exit(0)
if dry:
    print("mattbar-uninstall: dry-run: would edit", path)
    sys.exit(0)
path.write_text(new)
print("mattbar-uninstall: updated", path)
PY
}

# ---------------------------------------------------------------------------
HYPR=$REAL_HOME/.config/hypr
OMARCHY=$REAL_HOME/.config/omarchy
UNIT=$REAL_HOME/.config/systemd/user/mattbar.service
CONF_DIR=$REAL_HOME/.config/mattbar
STATE_DIR=$XDG_STATE_HOME/mattbar
SHIM_DIR=$XDG_RUNTIME_DIR/mattbar
SOCK=$XDG_RUNTIME_DIR/mattbar.sock
SIDECAR_MARK=$XDG_STATE_HOME/omarchy/mattbar-sidecar-on
SIDECAR_BAK=$OMARCHY/shell.json.mattbar-sidecar-bak

# Discover the installed binary from the unit, then well-known paths.
BIN_CANDIDATES=()
if [[ -f $UNIT ]]; then
  es=$(sed -n 's/^ExecStart=//p' "$UNIT" | head -n1)
  [[ -n $es ]] && BIN_CANDIDATES+=("$es")
fi
BIN_CANDIDATES+=(
  "$PREFIX/bin/mattbar"
  /usr/local/bin/mattbar
  "$REAL_HOME/.local/bin/mattbar"
)

# ---------------------------------------------------------------------------
if ((DRY_RUN == 0 && ASSUME_YES == 0)); then
  cat <<EOF
This will:
  • stop and disable the MattBar user service
  • delete the mattbar / mattbarctl binaries
  • reverse installer edits in ~/.config/hypr and ~/.config/omarchy
  • remove the MattBar menu entry, null-bar plugin, shims, and state
  • restart the Omarchy shell (Quickshell) and reload Hyprland

Personal keybinds in ~/.config/hypr/bindings.lua that were not appended
by install.sh are left in place. /usr/share/omarchy/ is not touched.
EOF
  if ((KEEP_CONFIG)); then
    echo "  • keeping $CONF_DIR (--keep-config)"
  else
    echo "  • deleting $CONF_DIR"
  fi
  printf 'Continue? [y/N] '
  read -r ans
  [[ $ans == [yY] || $ans == [yY][eE][sS] ]] || { log "aborted"; exit 0; }
fi

log "uninstalling for user $REAL_USER (home $REAL_HOME)"
((DRY_RUN)) && log "dry-run: no files will be changed"

# ---------------------------------------------------------------------------
# 1. Stop MattBar first so it cannot rewrite PATH, shell.json, or shims.
# ---------------------------------------------------------------------------
if ((DRY_RUN)); then
  log "dry-run: systemctl --user disable --now mattbar.service"
else
  run_as_user systemctl --user disable --now mattbar.service 2>/dev/null || true
  # Exact process name only — never pkill -f on a class string.
  if pgrep -u "$REAL_USER" -x mattbar >/dev/null 2>&1; then
    run_as_user pkill -x mattbar 2>/dev/null || true
    sleep 0.3
  fi
  log "MattBar service stopped"
fi

# Drop the runtime PATH shim before anything calls omarchy-shell.
do_rm_rf "$SHIM_DIR"
do_rm "$SOCK"

if ((DRY_RUN == 0)); then
  # systemd --user may still have $XDG_RUNTIME_DIR/mattbar/bin prepended.
  cur=$(run_as_user systemctl --user show-environment 2>/dev/null | sed -n 's/^PATH=//p' || true)
  if [[ -n $cur ]]; then
    newpath=$(printf '%s' "$cur" | tr ':' '\n' | grep -v '/mattbar/bin$' | paste -sd: -)
    if [[ $newpath != "$cur" ]]; then
      run_as_user systemctl --user set-environment "PATH=$newpath" 2>/dev/null || true
      log "removed MattBar shim from systemd user PATH"
    fi
  fi
  # This script's own PATH, so later omarchy/hyprctl calls skip the shim.
  export PATH=$(printf '%s' "$PATH" | tr ':' '\n' | grep -v '/mattbar/bin$' | paste -sd: -)
  run_as_user systemctl --user try-restart omarchy-sleep-lock.service 2>/dev/null || true
fi

# ---------------------------------------------------------------------------
# 2. Reverse Omarchy user customizations (never /usr/share/omarchy/).
# ---------------------------------------------------------------------------
edit_file "$OMARCHY/shell.json"
do_rm "$SIDECAR_BAK" "$SIDECAR_MARK"
do_rm_rf "$OMARCHY/plugins/mattbar.null-bar"
do_rm_rf "$OMARCHY/plugins/mattbar.plugin-bar"
edit_file "$OMARCHY/extensions/omarchy-menu.jsonc"

# ---------------------------------------------------------------------------
# 3. Reverse Hyprland installer edits. Personal binds stay.
# ---------------------------------------------------------------------------
edit_file "$HYPR/hyprland.lua"
edit_file "$HYPR/looknfeel.lua"
edit_file "$HYPR/bindings.lua"
edit_file "$HYPR/hyprland.conf"
edit_file "$HYPR/autostart.conf"
edit_file "$HYPR/autostart.lua"
do_rm "$HYPR/mattbar-shell-keys.lua" "$HYPR/mattbar-media-keys.conf"
do_rm "$HYPR/hyprland.lua.bak.mattbar" \
      "$HYPR/looknfeel.lua.bak.mattbar" \
      "$HYPR/bindings.lua.bak.mattbar" \
      "$HYPR/hyprland.conf.bak.mattbar" \
      "$OMARCHY/shell.json.bak.mattbar"

# ---------------------------------------------------------------------------
# 4. User unit, config, state.
# ---------------------------------------------------------------------------
do_rm "$UNIT" \
      "$REAL_HOME/.config/systemd/user/graphical-session.target.wants/mattbar.service"
if ((DRY_RUN == 0)); then
  run_as_user systemctl --user daemon-reload 2>/dev/null || true
fi
if ((KEEP_CONFIG)); then
  log "keeping $CONF_DIR"
else
  do_rm_rf "$CONF_DIR"
fi
do_rm_rf "$STATE_DIR"

# ---------------------------------------------------------------------------
# 5. Binaries (mattbar + mattbarctl symlink).
# ---------------------------------------------------------------------------
declare -A seen_bin=()
for b in "${BIN_CANDIDATES[@]}"; do
  [[ -n $b ]] || continue
  [[ -e $b || -L $b ]] || continue
  [[ ${seen_bin[$b]+x} ]] && continue
  seen_bin[$b]=1
  do_rm_priv "$b"
  ctl=$(dirname "$b")/mattbarctl
  [[ -e $ctl || -L $ctl ]] && do_rm_priv "$ctl"
done

# ---------------------------------------------------------------------------
# 6. Bring Omarchy / Hyprland back.
# ---------------------------------------------------------------------------
if ((DRY_RUN == 0)); then
  if [[ -n ${HYPRLAND_INSTANCE_SIGNATURE:-} || -n ${WAYLAND_DISPLAY:-} ]]; then
    if command -v hyprctl >/dev/null 2>&1; then
      log "reloading Hyprland"
      run_as_user hyprctl reload >/dev/null 2>&1 || hyprctl reload >/dev/null 2>&1 || true
      errs=$(hyprctl configerrors 2>/dev/null || true)
      if [[ -n $errs && $errs != "nothing" ]]; then
        warn "hyprctl configerrors:"$'\n'"$errs"
      fi
    fi
  fi
  if command -v omarchy >/dev/null 2>&1; then
    log "restarting Omarchy shell"
    run_as_user omarchy restart shell >/dev/null 2>&1 \
      || run_as_user omarchy-restart-shell >/dev/null 2>&1 \
      || omarchy-restart-shell >/dev/null 2>&1 \
      || warn "could not restart Omarchy shell — run: omarchy restart shell"
  elif command -v omarchy-restart-shell >/dev/null 2>&1; then
    run_as_user omarchy-restart-shell >/dev/null 2>&1 || true
  fi
fi

# ---------------------------------------------------------------------------
# 7. Leftovers that were not installer-owned (e.g. personal mattbarctl binds).
# ---------------------------------------------------------------------------
if ((DRY_RUN == 0)) && command -v grep >/dev/null 2>&1; then
  leftover=$(
    grep -RIn --exclude='*.bak*' --exclude='*.save' \
      -E 'mattbar|mattbarctl' \
      "$HYPR" "$OMARCHY" "$REAL_HOME/.config/systemd/user" \
      2>/dev/null | grep -v '/mattbar\.null-bar/' || true
  )
  if [[ -n $leftover ]]; then
    warn "references remain (not installer-owned; left untouched):"
    printf '%s\n' "$leftover" | sed 's/^/  /' >&2
  fi
fi

log "────────────────────────────────────────"
if ((DRY_RUN)); then
  log "dry-run complete (nothing changed)"
else
  log "done. MattBar is removed; Omarchy's shell should be back."
  log "If Super+Space or volume keys misbehave: hyprctl reload"
fi

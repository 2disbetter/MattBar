# Installing MattBar

This walks a complete setup: build, the supervised systemd service, and
every Hyprland-side change. Sections 4–6 are written for **Omarchy 4
("Quattro")**; the 3.x equivalents are noted where they differ. On plain
Hyprland (no Omarchy), only sections 1–3 apply. Section 8 is the full
Quickshell takeover used when MattBar replaces the Omarchy shell.

On a fresh tree, one command does the file copies and Hyprland/systemd
edits (build, unit, OSD shim, layer rule, media keys, notification
binds). It does **not** clone the battery plugin or change power
profiles:

```sh
./install.sh              # bar + OSD + Omarchy launcher/panel shortcut rebinds
./install.sh --takeover   # also shut down Quickshell (full shell)
./install.sh --help
```

`--takeover` (and a normal install on Omarchy 4) writes
`~/.config/hypr/mattbar-shell-keys.lua` and `require`s it from
`hyprland.lua` **before** your personal `bindings.lua`. Super+Space,
the apps menu, system menu, audio/network/bluetooth/display/clipboard
panels, emojis, and media transport then call `mattbarctl` instead of
Quickshell. Keys you already rebound (for example Super+Ctrl+H to
hibernate) stay yours.

The complete source is this directory. Copy **everything except
`build/`** (that tree is machine-local CMake output). You need:

```
CMakeLists.txt
INSTALL.md  README.md  SHELL.md
src/          # C++ sources + stb_image.h
protocols/    # extra Wayland XMLs not always in wayland-protocols
contrib/      # systemd unit, media-key drop-ins, null-bar plugin
```

`test_agents.cpp` is optional. Do not copy `~/.config/mattbar/` unless
you want that machine’s saved settings too.

## 1. Dependencies

Arch: `wayland` `cairo` `systemd` `libxkbcommon` `pam` (runtime) and
`cmake` `wayland-protocols` `pkgconf` `gcc` (build). `sed` is used at
configure time to patch generated protocol headers.
Debian/Ubuntu: `libwayland-dev` `libcairo2-dev` `libsystemd-dev`
`libxkbcommon-dev` `libpam0g-dev` `wayland-protocols` `libwayland-bin`
`cmake` `pkg-config` `g++`.

Lock and polkit link `-lpam`. Without the PAM development package the
configure step fails on `find_library(pam)`.

Optional runtime helpers (no build-time cost): `librsvg` (SVG tray
icons, dlopened on first use), `iw` (SSID display), `wpctl`/`pactl`
(PipeWire volume control and the shared audio event stream — both
present on any Omarchy install), `curl` (weather geocode and forecast
fetch). Full Quickshell takeover also needs
the Omarchy CLIs already on PATH (`hyprctl`, `omarchy-system-lock`,
`omarchy-system-wake`, `omarchy-launch-screensaver`,
`omarchy-hyprland-session-locked`, `omarchy-brightness-display`,
`powerprofilesctl`, `nmcli`, …) and the PAM service
`/etc/pam.d/omarchy-lock-password` that Omarchy ships.

## 2. Build & install

```sh
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
sudo cmake --install build
```

This installs `/usr/local/bin/mattbar` plus a `mattbarctl` symlink (the
same binary dispatching on argv[0]) for runtime control.

## 3. Run it — the systemd service (recommended)

The bar is your notification daemon, tray host, and OSD, so it is worth
supervising: the unit uses a watchdog pinged from inside the event loop
(a wedged loop stops pinging), `Restart=always` (a bar must come back
from *any* exit, including the post-resume Wayland error path), and
notification history / DND state are restored from
`$XDG_STATE_HOME/mattbar`, so a crash costs a sub-second flicker.

```sh
cp contrib/mattbar.service ~/.config/systemd/user/
systemctl --user daemon-reload
systemctl --user enable --now mattbar
```

On Omarchy (3.x and Quattro), that is the complete setup: the session
runs under **uwsm**, which imports the Wayland environment into the
user manager and activates `graphical-session.target` at login — so the
enabled unit starts with every session on its own. No autostart entry
is needed.

Only on a plain Hyprland session *not* run under uwsm (started bare
from a TTY) does `graphical-session.target` never activate; there you
must start it from Hyprland instead — and export the compositor's
environment first, or the unit cannot find the display:

```ini
exec-once = systemctl --user import-environment WAYLAND_DISPLAY XDG_CURRENT_DESKTOP
exec-once = systemctl --user start mattbar
```

(If you skip systemd entirely, `exec-once = /usr/local/bin/mattbar`
works too — you just lose the supervision.)

MattBar reserves no screen space, so no `gaps_out` changes are needed.
Optional cosmetic layer rule if you use blur — Quattro
(`~/.config/hypr/looknfeel.lua`):

```lua
hl.layer_rule({ match = { namespace = "mattbar" }, blur = true })
```

(3.x: `layerrule = blur, mattbar`.)

## 4. Media keys: one OSD instead of two

MattBar's OSD is event-driven (PipeWire + backlight uevents), so it
fires on any change — but Omarchy's stock keybinds also summon the
shell's own overlay, giving you two. Fix it at the keybind level:

**Quattro:** follow `contrib/omarchy-4x-media-keys/README.md` — copy the
no-op `omarchy-osd` shim to `~/.config/mattbar/shims/`, paste
`mattbar-media-keys.lua` into `~/.config/hypr/bindings.lua`, reload.
This keeps Omarchy's scripts (DSP-sink resolution, debounce, DDC/Apple
external-display brightness, mic-mute LED) and nulls only their OSD
call.

**3.x:** source `contrib/omarchy-3x-media-keys/mattbar-media-keys.conf`
from `hyprland.conf` — swayosd can only be suppressed by rebinding to
plain `wpctl`/`brightnessctl`. (If upgrading 3.x → Quattro, delete the
old `source =` line; hyprlang config stops applying and the overlays
return.)

Enable the OSD itself in MattBar's settings (gear icon in the tray →
Notifications & OSD) or `enable_osd = true` in
`~/.config/mattbar/mattbar.conf`.

## 5. Notifications: choosing one daemon

Only one process can own `org.freedesktop.Notifications`. Enable
MattBar's daemon in settings (`enable_notifications = true`), then hand
it the name:

**Quattro:** the name is owned by the Quickshell *shell* process. By default
MattBar will not SIGTERM it (that would kill the whole desktop shell, including
the wifi / bluetooth / volume panels). With `notifications_takeover` on it
asks the shell to disable `omarchy.notifications` over IPC, then restarts the
shell once — Quickshell's NotificationServer is a process-lifetime singleton,
so unloading the plugin does not drop the bus name until the process exits.
The disable is persisted in `~/.config/omarchy/shell.json`, so the next login
never races. Turning the daemon off in settings re-enables the plugin.

To also stop those panels, enable **Shut down the Quickshell instance
completely** in the same settings section (`quickshell_shutdown = true`). That
kills the Quickshell process after the plugin disable (and starts it again
when you turn the option off). The lock screen, menus, and `omarchy-shell`
IPC go with it — that is the point of the option.

If takeover cannot reach the shell (it is still starting), MattBar
retries for a few seconds and logs the outcome on
`journalctl --user -u mattbar`. The equivalent manual step is
`omarchy plugin disable omarchy.notifications` followed by
`omarchy-restart-shell`.

**3.x:** mako owns the name; MattBar's takeover SIGTERMs it (mako and
dunst are the only daemons it will terminate — both exit cleanly when
they cannot reacquire). Also remove `exec-once = uwsm-app -- mako` from
`~/.config/hypr/autostart.conf` so it stops being respawned.

**Notification keybinds.** Rebind Omarchy's notification keys to
MattBar's signals — Quattro, in `~/.config/hypr/bindings.lua`:

```lua
hl.unbind("SUPER + comma")
hl.unbind("SUPER + SHIFT + comma")
hl.unbind("SUPER + CTRL + comma")
hl.unbind("SUPER + ALT + comma")
o.bind("SUPER + comma", "Dismiss last notification", "pkill -RTMIN+2 mattbar")
o.bind("SUPER + SHIFT + comma", "Dismiss all notifications", "pkill -RTMIN+3 mattbar")
o.bind("SUPER + CTRL + comma", "Toggle do-not-disturb", "pkill -RTMIN+5 mattbar")
o.bind("SUPER + ALT + comma", "Invoke last notification", "pkill -RTMIN+4 mattbar")
```

(`pkill -RTMIN+6 mattbar` restores the last dismissed one, if you want a
binding for it; history is also always available from the bell module in
the bar. 3.x users replace the `makoctl` binds in
`~/.config/hypr/bindings/utilities.conf` with the same pkill commands.)

## 6. Optional: remove the shell's bar (Quattro)

To run MattBar as *the* bar while keeping the rest of the Quattro shell
(lock screen, menus, polkit agent), install the null-bar plugin — see
`contrib/omarchy-null-bar/README.md`:

```sh
mkdir -p ~/.config/omarchy/plugins
cp -r contrib/omarchy-null-bar ~/.config/omarchy/plugins/mattbar.null-bar
[ -f ~/.config/omarchy/shell.json ] || cp "$OMARCHY_PATH/config/omarchy/shell.json" ~/.config/omarchy/
jq '.bar.id = "mattbar.null-bar"' ~/.config/omarchy/shell.json > /tmp/shell.json && mv /tmp/shell.json ~/.config/omarchy/shell.json
omarchy-restart-shell
```

This unloads the built-in bar and all its widget polling; a load failure
falls back to the stock bar, so you cannot end up barless. (3.x: disable
Waybar's autostart instead.)

## 7. Verify

- `systemctl --user status mattbar` — active, watchdog enabled.
- Volume key → exactly one OSD (MattBar's, with percentage). If the
  shell overlay persists, `hyprctl binds | grep omarchy-audio-output-volume`
  should show the `env PATH=".../mattbar/shims:$PATH"` prefix.
- `notify-send test` → MattBar popup. If it lands elsewhere,
  `journalctl --user -u mattbar | grep notifyd` says who owns the name.
- Theme following: toggle **Follow Omarchy theme** in settings, run
  `omarchy-theme-set <theme>` — the bar recolors instantly, no restart.

## 8. Full Quickshell takeover (Omarchy 4)

This is the “MattBar owns the desktop shell” mode: lock, wallpaper,
idle, polkit, Super+Space, and every `omarchy-shell` bind. Nothing
under `/usr/share/omarchy/` is edited. The unit file in `contrib/`
already contains the systemd tweaks that matter.

### 8.1 systemd user unit

Copy `contrib/mattbar.service` (do not invent a slimmer one):

```
KillMode=process          # never SIGTERM the cgroup (apps launched from
                          # the bar used to die on restart)
TimeoutStopSec=8
Restart=always
WatchdogSec=30            # suspend/hibernate thaw must fit this window
StartLimitIntervalSec=60  # these two keys belong in [Unit], not [Service]
StartLimitBurst=60
Slice=session.slice
Type=notify
```

Then:

```sh
cp contrib/mattbar.service ~/.config/systemd/user/
systemctl --user daemon-reload
systemctl --user enable --now mattbar
```

Omarchy’s `omarchy-sleep-lock.service` is left as shipped. MattBar does
**not** replace it. Takeover prepends a PATH shim and runs
`systemctl --user import-environment PATH` plus
`try-restart omarchy-sleep-lock.service` so that unit calls MattBar’s
`omarchy-shell` instead of `qs ipc`. You should not hand-edit the
sleep-lock unit.

Omarchy already ships `/etc/systemd/logind.conf.d/20-inhibit-delay.conf`
(`InhibitDelayMaxSec=15`). That delay is what lets the lock complete
before suspend/hibernate. Do not lower it.

### 8.2 Hyprland (user config only)

**Layer rule** — the outside-click dismiss surface must not animate, or
TUIs feel laggy. In `~/.config/hypr/hyprland.lua` (after Omarchy’s
`require("default.hypr.omarchy")`):

```lua
hl.layer_rule({ match = { namespace = "mattbar-dismiss" }, no_anim = true, animation = "none" })
```

Optional blur on the bar itself, in `~/.config/hypr/looknfeel.lua`:

```lua
hl.layer_rule({ match = { namespace = "mattbar" }, blur = true })
```

**Media keys** — section 4 / `contrib/omarchy-4x-media-keys/` so
volume and brightness do not also summon Quickshell’s OSD.

**Omarchy shortcuts** — `install.sh` installs
`~/.config/hypr/mattbar-shell-keys.lua` (launcher, panels, notifications,
media transport). Loaded after Omarchy defaults, before `bindings.lua`.

**Notification keys** — included in that file (`mattbarctl notifications …`).

No `exec-once = mattbar` is needed on uwsm/Omarchy. No `gaps_out`
change. Screensaver window rules stay Omarchy’s (`org.omarchy.screensaver`).

Takeover also injects `PATH=$XDG_RUNTIME_DIR/mattbar/bin:…` into the
Hyprland session with `hyprctl keyword env PATH,…` so existing binds
that call `omarchy-shell` / `omarchy-menu` keep working. That is
runtime state, not a file you copy.

### 8.3 Settings (MattBar, then Omarchy)

In the tray gear (or `~/.config/mattbar/mattbar.conf`):

```
enable_notifications = true
notifications_takeover = true
quickshell_shutdown = true
```

User Omarchy plugins keep working during takeover without bringing
back the full first-party Quickshell shell: enable **Settings → Shell →
Quickshell plugins** (`qs_plugins` in `mattbar.conf`). MattBar then
starts a stripped Quickshell sidecar that loads only plugins you
installed (`omarchy plugin add …` / Settings → Plugins). Turn the
toggle off and the sidecar exits. The sidecar needs the null-bar
plugin in `~/.config/omarchy/plugins/mattbar.null-bar` (install.sh
copies it, or copy `contrib/omarchy-null-bar`).

**Plugin row** (`qs_plugin_bar`, off by default): a thin Quickshell
strip (`mattbar.plugin-bar`) stacked on the desktop-facing side of
MattBar — below a top bar, above a bottom bar. Only `bar-widget`
plugins render there, placed with the same **L / C / R / M** zones as
MattBar modules (`qs_plugin_bar_left` / `_center` / `_right` /
`_more`). More is a ⋯ overflow on the plugin row, not MattBar's Cairo
More. Overlay, panel, and menu plugins stay on the Plugins chip /
summon path. Same auto-hide family, zero exclusive zone. install.sh
copies `contrib/omarchy-plugin-bar` next to null-bar; it never
rewrites the user's `bar.id`.

`quickshell_shutdown` is the actual takeover switch. Turning it on:

- stops Quickshell (`quickshell kill`)
- maps wallpaper + lock + idle + polkit
- installs the PATH shim and restarts `omarchy-sleep-lock`
- shows Settings → **Shell** (idle timeouts, panel sizes)

Idle timeouts are Omarchy’s, not a second MattBar clock:

```
~/.config/omarchy/shell.json   →  idle.screensaver, idle.lock
~/.config/mattbar/mattbar.conf →  idle_blank_s   (seconds after lock
                                                 before backlight off; 5)
```

The Shell tab writes `shell.json` so Quickshell would use the same
numbers if you turn takeover off.

### 8.4 Optional: stop Quickshell from changing the power profile

Stock `omarchy.battery` reapplies AC/battery profiles on every shell
start (a missing battery file defaults to **balanced**). If you always
want power-saver:

```sh
mkdir -p ~/.local/state/omarchy/powerprofiles
printf 'power-saver\n' > ~/.local/state/omarchy/powerprofiles/ac
printf 'power-saver\n' > ~/.local/state/omarchy/powerprofiles/battery
omarchy plugin clone omarchy.battery
```

Then in `~/.config/omarchy/plugins/<user>.battery/Service.qml` remove
`applyPowerProfile` / the `omarchy-powerprofiles-set` process. Keep the
low-battery warning. The clone disables stock `omarchy.battery`.

### 8.5 Verify takeover

```sh
systemctl --user status mattbar
# PATH shim used by Hyprland *and* by omarchy-sleep-lock:
systemctl --user show-environment | grep ^PATH=
tr '\0' '\n' < /proc/$(systemctl --user show -p MainPID --value omarchy-sleep-lock.service)/environ | grep ^PATH=
# should start with $XDG_RUNTIME_DIR/mattbar/bin
omarchy-shell lock status    # JSON from MattBar, not "omarchy-shell is not running"
pgrep -a quickshell          # empty while takeover is on
journalctl --user -u mattbar | grep -E 'PrepareForSleep|lock: session'
```

Super+L (or `omarchy system lock`) should show MattBar’s lock. Suspend
and hibernate should lock first (`PrepareForSleep entering` in the
journal). After a thaw, the password field must come back even if
MattBar restarted.

### 8.6 What you do not copy or edit

| Leave alone | Why |
|---|---|
| `/usr/share/omarchy/` | package-owned; overwritten on `omarchy update` |
| `omarchy-sleep-lock.service` | MattBar restarts it with a new PATH |
| `/etc/systemd/logind.conf.d/` | Omarchy’s 15s inhibit delay |
| `~/.config/hypr/hypridle.conf` | unused while takeover owns idle; keep for fallback |
| `build/` | regenerate with cmake on the new machine |

## 9. If the lock is a black screen after resume

Hyprland keeps `ext-session-lock` after the lock client dies. From
another TTY:

```sh
loginctl unlock-session
systemctl --user restart mattbar
```

Current MattBar reclaims Hyprland’s LOCK blocker on startup and does
not exit the event loop on a thaw `epoll` EPERM. If you are on a
build older than 1.39.9, upgrade first.

## 10. Uninstall

To remove MattBar and reverse every Omarchy / Hyprland change the
installer made (user config only — never `/usr/share/omarchy/`):

```sh
./uninstall.sh
# or: ./install.sh --uninstall
```

That stops the user service, deletes `/usr/local/bin/mattbar` and
`mattbarctl`, removes the systemd unit, PATH shim, null-bar plugin,
Style → Menu Bar menu entry, media-key / shell-key drop-ins, layer
rules, and the `omarchy.notifications` disable in
`~/.config/omarchy/shell.json`, then reloads Hyprland and restarts
the Omarchy shell.

Personal binds in `~/.config/hypr/bindings.lua` that you wrote
yourself (for example Super+A) are left alone. `--keep-config` keeps
`~/.config/mattbar/`. `--dry-run` prints the plan without changing
anything.

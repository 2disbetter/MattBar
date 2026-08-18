# Installing MattBar

This walks a complete setup: build, the supervised systemd service, and
every Hyprland-side change. Sections 4–6 are written for **Omarchy 4
("Quattro")**; the 3.x equivalents are noted where they differ. On plain
Hyprland (no Omarchy), only sections 1–3 apply.

## 1. Dependencies

Arch: `wayland` `cairo` `systemd` (runtime) and `cmake`
`wayland-protocols` `pkgconf` `gcc` (build).
Debian/Ubuntu: `libwayland-dev` `libcairo2-dev` `libsystemd-dev`
`wayland-protocols` `libwayland-bin` `cmake` `pkg-config` `g++`.

Optional runtime helpers (no build-time cost): `librsvg` (SVG tray
icons, dlopened on first use), `iw` (SSID display), `wpctl`/`pactl`
(PipeWire volume control and the shared audio event stream — both
present on any Omarchy install).

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

Then have Hyprland start it with the session. On Quattro, add to
`~/.config/hypr/autostart.lua`:

```lua
o.exec_on_start("systemctl --user start mattbar")
```

(3.x / plain Hyprland: `exec-once = systemctl --user start mattbar` in
`hyprland.conf`. If you skip systemd entirely, `exec-once =
/usr/local/bin/mattbar` works too — you just lose the supervision.)

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

**Quattro:** the name is owned by the Quickshell *shell* process, which
MattBar deliberately will not terminate (that would kill your whole
desktop shell — it detects this case, stays queued, and logs a message
instead). Disable the shell's notification service properly, in
`~/.config/omarchy/shell.json`:

```sh
[ -f ~/.config/omarchy/shell.json ] || cp "$OMARCHY_PATH/config/omarchy/shell.json" ~/.config/omarchy/
jq '.disabledPlugins = ((.disabledPlugins // []) + ["omarchy.notifications"] | unique)' \
  ~/.config/omarchy/shell.json > /tmp/shell.json && mv /tmp/shell.json ~/.config/omarchy/shell.json
omarchy-restart-shell
```

MattBar's queued claim promotes the moment the shell releases the name.

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

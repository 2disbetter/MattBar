# Null bar for Omarchy quattro (4.x)

Omarchy 4 ("quattro") replaced Waybar with `omarchy-shell`, a single
Quickshell process whose built-in status bar is one plugin among many.
There is no "no bar" setting: `bar.id` in `shell.json` selects the active
bar plugin, and anything missing or invalid falls back to the built-in
`omarchy.bar`. The supported way to *not* have the shell's bar is to
select a different bar plugin — so this directory is one: a bar that
renders nothing, creates no windows, and runs no timers. Selecting it
unloads `omarchy.bar` and all of its widgets (including their polling),
while the rest of the shell — notifications, OSD, lock screen, polkit
agent, background switcher, menus — keeps running. MattBar then provides
the bar.

## Install

```sh
mkdir -p ~/.config/omarchy/plugins
cp -r contrib/omarchy-null-bar ~/.config/omarchy/plugins/mattbar.null-bar
```

Select it in `~/.config/omarchy/shell.json`. If you don't have that file
yet, start from the stock one (the user file replaces the built-in config
wholesale, so don't create a minimal file from scratch):

```sh
[ -f ~/.config/omarchy/shell.json ] || cp "$OMARCHY_PATH/config/omarchy/shell.json" ~/.config/omarchy/
jq '.bar.id = "mattbar.null-bar"' ~/.config/omarchy/shell.json > /tmp/shell.json && mv /tmp/shell.json ~/.config/omarchy/shell.json
```

Then reload:

```sh
omarchy-shell shell rescanPlugins   # or: omarchy-restart-shell
```

Switching back: set `bar.id` to `omarchy.bar` (or delete the key) and
reload. If the plugin ever fails to load, the shell logs a warning and
falls back to the built-in bar on its own — you cannot end up barless by
accident.

## What changes

- The built-in bar and every bar widget (workspaces, clock, tray,
  weather, monitor/network pollers, ...) unload. The idle process-spawn
  traffic those widgets generate goes away with them.
- Bar-anchored popover panels (audio, network, bluetooth, power, weather)
  can no longer be summoned — their call sites in the shell are guarded
  and no-op cleanly. MattBar's own volume/network click actions cover the
  common cases.
- Notifications reposition to their default screen-edge margin: this
  plugin reports `barHidden: true` / `barSize: 0`, mirroring the property
  surface the notification service reads (a bar object *without* those
  properties would NaN the anchor math, which is why they're declared).
- Everything else in the shell is untouched.

Note this removes the *bar*, not the shell: the Quickshell process and
its Qt runtime keep running to provide the rest of the desktop.

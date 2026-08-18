# MattBar media keys on Omarchy 4 ("Quattro")

Omarchy 4 dropped swayosd. Media keys are now bound in Lua
(`default/hypr/bindings/media.lua`) to `omarchy-audio-output-volume`,
`omarchy-audio-input-mute`, and `omarchy-brightness-display`, each of
which ends by calling `omarchy-osd`, a thin client that asks the
Quickshell shell to draw its overlay over IPC. The shell's OSD panel is
IPC-only - it does not watch PipeWire or the backlight - so no
`omarchy-osd` call means no Quattro overlay.

This drop-in therefore keeps Omarchy's scripts (they resolve the
physical sink through DSP chains, debounce mute keys, handle DDC and
Apple external-display brightness, and drive the hardware mic-mute LED)
and only silences the overlay:

- **Volume & mic** keys run the stock scripts with a no-op `omarchy-osd`
  shim prepended to PATH for those invocations only.
- **Brightness** keys use the script's native `--no-osd` flag.

MattBar's OSD is event-driven (PipeWire + backlight uevents), so it
shows the change regardless of what triggered it - now with the numeric
percentage next to the bar.

## Install

```sh
mkdir -p ~/.config/mattbar/shims
cp shims/omarchy-osd ~/.config/mattbar/shims/
chmod +x ~/.config/mattbar/shims/omarchy-osd
```

Then paste the contents of `mattbar-media-keys.lua` into
`~/.config/hypr/bindings.lua` and reload Hyprland (`SUPER + ESC` menu →
Reload, or `hyprctl reload`).

Upgrading from the 3.x drop-in: remove the old
`source = .../mattbar-media-keys.conf` line - Quattro's config moved to
Lua, so the old hyprlang unbinds silently stopped applying, which is why
the stock overlays reappeared after the upgrade.

## Notes

- The shim is scoped to these bindings; every other `omarchy-osd` user
  (caps-lock indicator, screen recording, screenshots, ...) still shows
  the Quattro overlay. If you'd rather kill *all* Quattro OSDs, put the
  shim in a directory that precedes `/usr/bin` in your session PATH
  (e.g. `~/.local/bin`) instead - but then nothing replaces those
  non-media overlays.
- External DDC monitors have no kernel backlight device, so MattBar
  shows no brightness OSD for them; the change still applies.
- Transport keys (`omarchy-shell media ...`) summon their track popup
  inside the shell, not through `omarchy-osd`; an optional commented
  block in the Lua file rebinds them to `playerctl` if you want those
  quiet too.

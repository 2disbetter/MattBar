# MattBar
**Ultra-efficient, configurable drop-in replacement for Waybar and the Omarchy 4 / Quattro Quickshell bar.**

<img width="1600" height="720" alt="image" src="https://github.com/user-attachments/assets/79846e09-ef13-4e43-8f5d-b0cb0bd544ea" />

Written in C++ against raw `wayland-client` + Cairo — no GTK, no Qt.  
~19–42 MB RSS, essentially zero CPU when idle/hidden, fully event-driven.

MattBar lives on the layer-shell **overlay** layer with a **zero exclusive zone**, so windows never shrink or reflow. It auto-hides to a thin hot strip and reveals over your windows when you touch the edge (optional dwell delay + pin module). It docks to any edge (top/bottom/left/right), supports multi-monitor and HiDPI natively, and can optionally take over the entire Omarchy 4 Quickshell host (panels, menu, lock, polkit, wallpaper, idle) — see [SHELL.md](SHELL.md).

A built-in click-only settings window (gear in the tray) lets you change colours, sizes, delays, module visibility/order, and more — everything applies live. An optional **Follow Omarchy theme** toggle tracks the active Omarchy theme instantly via inotify (both `colors.toml` and older `waybar.css`).

---

## Position

MattBar docks to any screen edge: `position = top | bottom | left | right`
(settings: the Position selector in General; changes apply live). The hot
strip, hover zone, dwell delay, and tray menus all follow the chosen edge —
menus open away from it. Vertical bars stack modules top-to-bottom with a
separate thickness (`vertical_width`, default 72 px) and compact text: the
clock shows time only, battery/volume/temperature drop their prefixes, long
labels ellipsize, and workspace pills and tray icons stack.

## The auto-hide model

MattBar creates a `wlr-layer-shell` surface on the **overlay** layer with an
**exclusive zone of 0**. That combination is the whole trick:

- Windows tile and maximize to the display's *full* height, as if no bar
  existed. They never resize, move, or reflow — ever.
- When hidden, the surface resizes down to the hover-zone height
  (default 8 px — deliberately larger than the 2 px visible strip line
  drawn at its top, so the reveal target is easy to hit; the remainder is
  transparent). Hiding and revealing are plain surface resizes — the one
  mechanism every compositor handles identically — with interim draws
  suppressed until the compositor acknowledges each resize.
- Moving the mouse into the hover zone reveals the bar, drawn *on top of*
  your windows. An optional reveal delay (`reveal_delay_ms`) requires the
  pointer to *dwell* in the zone that long before revealing — brushing the
  screen edge on the way to a titlebar no longer misfires the bar. Leaving
  the zone cancels a pending reveal. Move the pointer away and it collapses
  again after the configured hide delay (default 500 ms). The hover zone
  height adjusts in 1 px steps down to 1 px.
- Multi-monitor: one bar per display (each hiding/revealing independently)
  or a single bar pinned to one output.

Compare with Waybar's default: Waybar reserves an exclusive zone, so windows
shrink to fit under it. MattBar's windows are completely oblivious to the bar.

## Multi-monitor

**Off by default** (`multi_monitor`). When enabled, MattBar creates one
surface per output listed in `monitors` (empty = every connected output).
Each bar hides and reveals independently. Singleton services (tray, settings
window, notification/OSD popups) live only on the primary bar so two SNI
hosts or two notification daemons never fight each other.

## HiDPI

Native integer scaling via the Wayland scale protocol. Fractional scaling
(`wp-fractional-scale-v1`) is not yet supported; fractional outputs render
at the next integer scale and the compositor downsamples.

## Modules

Every module’s position is pure data (`layout_left` / `layout_center` /
`layout_right` / `layout_more`). The settings window’s **Modules & layout**
section lets you toggle visibility, move modules between zones, and reorder
them live.

| Module          | Typical zone | Notes |
|-----------------|--------------|-------|
| omarchy         | left         | Omarchy logo; left-click menu, right-click terminal |
| workspaces      | left         | Hyprland IPC; click / scroll |
| clock           | center       | Configurable format; click opens calendar popup |
| pin             | center       | Keeps bar revealed |
| more            | any          | ⋯ overflow for modules parked in the “more” zone |
| plugins         | any          | Quickshell-style command plugins |
| tray            | right        | Full SNI implementation + dbusmenu; auto-collapse |
| update          | right        | Shows only when update is available |
| agents          | right        | Shell agents status |
| microphone      | right        | Mic mute / level |
| screenrecord    | right        | Screen recording indicator |
| temp            | right        | Any hwmon sensor; click/scroll to cycle |
| network         | right        | SSID / interface; event-driven (rtnetlink) |
| volume          | right        | Headset-aware; PipeWire events; mixer popup |
| bluetooth       | right        | Device name, battery, multi-connection count; fully event-driven |
| display         | right        | Display / resolution controls |
| brightness      | right        | Vector sun icon + slider popup; kernel uevents |
| media           | right        | MPRIS |
| caffeine        | right        | Stay-awake / idle inhibitor |
| nightlight      | right        | Night-light / redshift control |
| weather         | right        | Weather data |
| kblayout        | right        | Keyboard layout |
| activewindow    | any          | Current window title |
| reminder        | any          | Simple reminders |
| dictation       | any          | Dictation indicator |
| tailscale       | any          | Tailscale status |
| dropbox         | any          | Dropbox status |
| battery         | right        | Percentage + optional time estimate |
| notifications   | right        | Bell + history / DND |
| power           | right        | power-profiles-daemon |

Nearly everything is event-driven (PipeWire, BlueZ D-Bus, kernel uevents,
inotify for themes, Hyprland IPC, rtnetlink) so an idle/hidden bar costs
essentially zero wakeups.

## Notifications & OSD (opt-in)

MattBar can act as the full `org.freedesktop.Notifications` daemon (with
do-not-disturb, history/restore, actions, per-type toggles, emoji rendering)
and can draw a volume / mic / brightness OSD that reacts to the *actual*
PipeWire / backlight change rather than keypresses.

Ready-made drop-ins under `contrib/` rebind Omarchy 3.x (swayosd) and 4.x /
Quattro media keys so MattBar draws the only overlay — on Quattro without
losing the mic-mute LED or DDC external-display brightness.

See the detailed sections in the full source README for takeover behaviour,
keybind examples, and `mattbarctl` / signal control (`pkill -RTMIN+N`).

## Configuration

Plain `key = value` file at `~/.config/mattbar/mattbar.conf` plus the
built-in settings window (gear icon). All changes apply and persist
instantly — there is no separate Save button.

**Follow Omarchy theme** (off by default) tracks the active Omarchy theme
via inotify (both the modern `colors.toml` palette and older `waybar.css`).
Your own palette is always preserved; turning the toggle off restores it
immediately. Baked presets: Default, Light, Nord, Gruvbox, Dracula,
Solarized Light.

## Shell replacement (optional)

MattBar can completely replace Omarchy 4’s Quickshell host (`omarchy-shell`)
— bar, panels, overlays, lock screen, polkit, wallpaper, idle auto-lock —
in a single process. See **[SHELL.md](SHELL.md)** for the full inventory,
design decisions, and current status. The install script supports a
`--takeover` mode that rebinds the relevant keys and shuts Quickshell down
cleanly.

## Quick start

Full walkthrough (systemd unit, media-key rebinds, brightness/volume OSD,
etc.): **[INSTALL.md](INSTALL.md)**. Note: that you can also use the aforementioned install.sh script.

```sh
# Build
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)

# Run
./build/mattbar

# Optional systemd user service
cp contrib/mattbar.service ~/.config/systemd/user/
systemctl --user daemon-reload
systemctl --user enable --now mattbar

# MattBar

A minimal auto-hiding status bar for Hyprland (and other wlroots compositors),
written in C++ with raw `wayland-client` + Cairo. No GTK, no Qt.

Replacing Omarchy 4’s Quickshell host (panels, menu, lock, polkit,
wallpaper) is documented in [SHELL.md](SHELL.md).

## Position

MattBar docks to any screen edge: `position = top | bottom | left | right`
(settings: the Position selector in General; changes apply live). The hot
strip, hover zone, dwell delay, and tray menus all follow the chosen edge —
menus open away from it. Vertical bars stack modules top-to-bottom with a
separate thickness (`vertical_width`, default 72 px, settings stepper) and
compact text: the clock shows time only, battery/volume/temperature drop
their prefixes, long labels ellipsize, and workspace pills and tray icons
stack.

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
  your windows. An optional reveal delay (`reveal_delay_ms`, settings:
  "Reveal delay") requires the pointer to *dwell* in the zone that long
  before revealing — brushing the screen edge on the way to a titlebar
  no longer misfires the bar. Leaving the zone cancels a pending reveal.
  Move the pointer away and it collapses again after 500 ms. The hover
  zone height adjusts in 1 px steps down to 1 px, which matters on short
  displays where every top-edge pixel is precious.
- Multi-monitor: see below. One bar per display, each hiding and
  revealing independently, or a single bar pinned to one display.

Compare with Waybar's default: Waybar reserves an exclusive zone, so windows
shrink to fit under it. MattBar's windows are completely oblivious to the bar.

An optional **plugin row** (`qs_plugin_bar`, Settings → Shell → Plugin row)
hosts user Omarchy `bar-widget` plugins in a thin Quickshell strip stacked
on the desktop-facing side of MattBar — below a top bar, above a bottom
bar. Placement matches MattBar modules: **L / C / R / M**. Left, center,
and right pack on the strip; More is a ⋯ overflow on the row. Overlay,
panel, and menu plugins stay behind the Plugins chip. The row shares
MattBar's auto-hide family and also uses a zero exclusive zone. Off by
default.

## Multi-monitor

**Off by default** (`multi_monitor`), in which case nothing changes: one
bar, pinned to `output = DP-2` if you name a display, otherwise wherever
the compositor puts it. The settings window's **Monitors** section lists
every connected output, so pinning no longer means hand-editing the conf
file — and clicking the pinned one again unpins it.

Switch **One bar per monitor** on and MattBar creates a layer surface per
display. Each bar is genuinely independent: its own hot strip, its own
hover state, its own hide and reveal timers. Hovering the top edge of your
left monitor reveals *that* bar and leaves the others collapsed. Pinning
is the deliberate exception — one click of the pin keeps every bar up, so
glancing at another screen can't collapse the one you pinned.

Which displays get a bar is a checkbox list in settings (`monitors` in the
conf, a comma-separated list of output names; empty means every output).
One of them is the **primary**, and that is where the singletons live:

- the **tray**, because a second StatusNotifier host would register for the
  same items and fight the first,
- the **settings window**,
- **notification popups and the OSD**, which should appear once, not once
  per screen.

Everything else — workspaces, clock, volume, battery, the bell, the power
profile — draws on every bar. Module popups (calendar, notification
centre, power menu, brightness slider) open on the monitor whose bar you
clicked, so clicking the clock on the right-hand screen doesn't put the
calendar on the left.

The **workspaces** module becomes monitor-aware in this mode: each bar
shows the workspaces that live on its own display, with that display's
active workspace highlighted, queried from Hyprland's `j/workspaces` and
`j/monitors`. With multi-monitor off, the extra `j/monitors` round-trip
per refresh isn't made at all.

**Hovering one bar reveals all** (`reveal_all_monitors`, settings:
Monitors) is for people who treat their displays as one surface: entering
any bar's hot zone brings every bar up, and they hide together once the
pointer has left them all. Off by default — the default remains fully
independent bars.

Monitors can come and go while MattBar runs. Plugging a display in creates
its bar; unplugging one tears its bar down and re-elects the primary if
that was the display that vanished. Nothing needs restarting.

```ini
multi_monitor = true
monitors =                    # empty = every connected output
# monitors = DP-2,HDMI-A-1    # or just these
primary_output = DP-2         # tray, settings and notification popups
```

## HiDPI

MattBar renders at each output's scale. Every surface is covered:
the bars themselves (per monitor, so mixed-DPI setups get a crisp bar on
both the 4K panel and the 1080p external), all popups (calendar,
notification centre, power menu, brightness slider, notification stack,
OSD), the settings window, tray menus, wallpaper, and the pointer cursor.
All layout, hit-testing, and configuration stay in logical pixels — a
scale-2 monitor simply gets a 2× buffer with the same geometry. Integer
scale uses `wl_surface.set_buffer_scale`; fractional outputs use
`wp-fractional-scale-v1` + `wp_viewporter` so a 1.5× panel is a 45 px
buffer covering 30 logical px instead of a 2× buffer the compositor
downsamples. Scale changes apply live
(e.g. `hyprctl keyword monitor DP-2,preferred,auto,2` re-renders the
affected bar immediately), and a compositor-picked bar learns its output's
scale from `wl_surface.enter`. Older compositors without the fractional
protocols keep the integer path.

## Modules

| Module       | Position | Interactions                                          |
|--------------|----------|-------------------------------------------------------|
| Workspaces   | left     | click to switch, scroll to cycle (Hyprland IPC)       |
| Clock        | center   | format is `clock_format` / `clock_format_vertical` (any strftime string; settings has 24h/12h/seconds presets). Click opens a **calendar** popup (month grid, today highlighted; `‹`/`›` or scroll to page months, click the title or right-click to jump back to today; Monday-first and week numbers are settings) |
| Pin          | center   | click to keep the bar revealed (auto-hide paused); click again to resume. Windows are never affected either way |
| Tray         | right    | gear opens **settings**; chevron expands/collapses (auto-collapses after a configurable idle timeout); left-click activates an app, middle = secondary activate, right = **rendered menu** (dbusmenu popup with navigable submenus, checkmarks and radio state — the active one filled with the accent — and per-item icons from `icon-name`/`icon-data`), scroll forwarded |
| Omarchy menu | left     | Omarchy logo glyph; click runs `omarchy-menu`, right-click opens a terminal (mirrors Omarchy's Waybar `custom/omarchy`) |
| Update icon  | right    | shows only when the check command **exits 0** (Waybar custom-module
semantics, which Omarchy's scripts rely on — `omarchy-update-available`
prints a message in *both* states and signals via exit code alone). Clears promptly after updating: MattBar tracks the launched updater and re-checks the moment it exits, then keeps re-checking every 10 s for 30 min (Omarchy's own refresh signal targets waybar by name, so it can't be relied on — though `pkill -RTMIN+7 mattbar` also works). Otherwise checked every 6 h and on `SIGRTMIN+7`; click launches the updater |
| Temperature  | right    | any hwmon sensor (CPU/NVMe/GPU/ACPI/...); defaults to the CPU package sensor via auto-detect. Click or scroll to cycle sensors; selection + warn threshold configurable in settings. Turns red above the warn temperature. Pure sysfs reads, no process spawns |
| Network      | right    | shows SSID (Wi-Fi) or interface name; click opens the WiFi manager (`network_click`, default `omarchy-launch-wifi`). **Event-driven**: an rtnetlink socket delivers link/route changes (interface up/down, Wi-Fi association, default-route moves), so `iw` runs only when the network actually changed — zero subprocess spawns at steady state. Falls back to the old 5 s visible-poll if the netlink socket can't be created |
| Volume       | right    | shows a headphone rune beside the percentage while the default sink is a headset-class device (`volume_headset_indicator`, default on; rune via `volume_headset_glyph`) — decided from the node's own `device.form-factor`/`device.icon-name` for Bluetooth and the active headphones port for wired jacks, re-checked on the same events as the volume itself. Click opens the mixer (`volume_click`, default `omarchy-launch-audio`), right-click = mute toggle, scroll = ±5 % — matching Omarchy's Waybar bindings. **Event-driven**: one shared, persistent `pactl subscribe` stream (served natively by pipewire-pulse) tells MattBar when the sink changed; the mixer is queried only then, and never while hidden (changes are marked stale and picked up at reveal). Falls back to the old 2 s visible-poll if no `pactl` exists |
| Bluetooth    | right    | BlueZ over the system bus, fully event-driven (zero polling; an idle adapter costs zero wakeups). Hidden when no adapter exists; the Bluetooth rune (Nerd Font `\uf294` by default, `bluetooth_glyph` to override). The module verifies at draw time that the bar font actually maps the glyph, and walks a fallback chain: the configured rune, the Material Design rune `U+F00AF`, then — new — any installed Nerd-glyph-capable family (JetBrainsMono/Caskaydia/Cascadia NF, Symbols Nerd Font), used for the rune alone while the rest of the text stays in the bar font, and finally plain `bt` text. A bar font without Nerd glyphs therefore still shows the symbol if *any* Nerd Font is installed, and readable text if none is (a stderr line reports each substitution) alone / with `off` when idle; shows the connected device's name, its battery % when the device reports one (urgent-red at ≤15 %), and `+N` for extra connections. Click opens the manager (`bluetooth_click`, default `omarchy-launch-bluetooth`); right-click toggles adapter power directly through BlueZ. What appears beyond the icon is settings-controlled — see the **Bluetooth** section in settings, conf keys `bluetooth_show_name`, `bluetooth_show_battery`, `bluetooth_show_count`, `bluetooth_name_len`: the device name (truncated UTF-8-safely at `bluetooth_name_len` chars), battery percent, and the `+N` extra-connections count can each be hidden. Icon-only mode still signals connected vs idle by color, and a low device battery still turns the icon urgent. If BlueZ or the system bus is unavailable, the module retries with bounded exponential backoff (2s..64s, then it relies on bus signals) and reconnects automatically after D-Bus or bluetoothd restarts; each retry prints a stderr line naming the failure. Note: stock `bluetooth.service` has no `Restart=` policy, so if bluetoothd itself dies it stays dead until rebooted or manually restarted - `systemctl status bluetooth` is the first thing to check when the module is missing |
| Brightness   | right    | `/sys/class/backlight` state kept fresh by kernel uevents (zero polling); hidden without a backlight device. Shows a **vector-drawn sun** + percent — drawn with cairo, not a font, because the common Nerd Fonts' sun glyph reads as a second gear next to the tray's settings cog at bar sizes (filled disc + detached rays vs. the gear's hollow ring + fused teeth). Set `brightness_glyph` to any glyph string to use the bar font instead; the old default `\uf185` is treated as the vector sun on load, so existing configs get the fix automatically. Left-click opens a slider popup on an overlay surface - click or drag to set, scroll to step; scrolling the module itself steps by `brightness_step` (default 5%). Writes go through systemd-logind `Session.SetBrightness` (unprivileged, no helper tools), falling back to a direct sysfs write, then `brightnessctl`. Hidden means the kernel exposes no backlight device (`ls /sys/class/backlight` to confirm) - normal for desktops, whose external monitors are only adjustable over DDC/I2C (`ddcutil`), which this module does not drive. A backlight driver that loads after the bar is picked up live via uevents |
| Media        | right    | MPRIS over the session bus, event-driven (zero polling). Shows play/pause glyph (font-verified, ASCII fallback) + artist – title for the active player (playing beats paused, then most recent). Left-click = play/pause, right-click = next, scroll = next/previous  Players with sparse metadata (web radio, mpv, players mid-load) show the player's name instead of hiding; a session-bus outage or a timed-out startup name sweep (busy bus at login) is retried with bounded backoff, and any event from an undiscovered player triggers a rediscovery sweep, so the module self-heals within one track regardless of what discovery missed. Display is settings-controlled (see the **Media** section in settings, conf keys `media_show_artist`, `media_len`): toggle the artist prefix, and cap the label length (8-80 chars, UTF-8-safe truncation) |
| Stay awake   | right    | holds a Wayland idle-inhibitor on the bar surface while active (coffee glyph, accent when on; runtime-only, like Waybar's idle_inhibitor) |
| Notifications| right    | bell; click opens the **notification centre** (see below), right-click toggles do-not-disturb. While DND is on the bell wears a thin red slash — the one deliberate exception to the bell's no-signals rule, because DND is *your* state, not the notifications'. Active entries with an action show a `↳ label` tag in the panel; clicking it fires the app's default action ("open", "reply") right from history |
| Power profile| right    | current power-profiles-daemon profile; click opens a **radio menu** — the active profile is marked with a filled accent dot and its row highlighted — scroll cycles profiles without opening it. Below the profiles (toggleable: `power_show_actions`, settings: Power profile) sit **session actions**: suspend, hibernate, restart, shut down. Suspend/hibernate fire on one click; restart and shut down arm on the first click ("Restart? click again") and execute on the second — walking away or clicking elsewhere disarms. Commands are `power_cmd_suspend/hibernate/restart/shutdown` (default `systemctl ...`). With actions on, the module stays visible even without power-profiles-daemon, as a plain power button |
| Battery      | right    | red below 15 %; hidden on desktops without a battery. `battery_show_time` (settings: Clock & calendar) appends a "2h05" estimate — to empty while discharging, to full while charging — from the kernel's energy/power (or charge/current) readings |

The tray is a full StatusNotifierItem implementation over sd-bus: MattBar
acquires `org.kde.StatusNotifierWatcher` itself if nothing else owns it, or
registers as a host with an existing watcher otherwise. App icons come from
`IconName` (PNG theme lookup) or `IconPixmap` (rendered directly).

## Dependencies

Arch: `wayland` `cairo` `systemd` `libxkbcommon` `pam` (runtime) and
`cmake` `wayland-protocols` `pkgconf` `gcc` (build). Lock and polkit
link `-lpam`; the settings/lock/menu keyboard path needs `libxkbcommon`.
Optional: `librsvg` **at runtime** enables SVG tray icons — MattBar
dlopens it the first time an SVG icon is actually encountered, so a system
without it (or a session that never meets an SVG) pays zero build
dependency and zero resident glib setup. The no-GTK/no-Qt promise holds
either way.

Debian/Ubuntu: `libwayland-dev` `libcairo2-dev` `libsystemd-dev`
`libxkbcommon-dev` `libpam0g-dev` `wayland-protocols` `libwayland-bin`
`cmake` `pkg-config` `g++`.

Optional runtime helpers: `iw` (SSID display), `wpctl` (PipeWire volume,
falls back to `pactl`), `pactl` (the shared audio *event stream* — served
natively by pipewire-pulse, present on any Omarchy install; without it the
volume module reverts to visible-polling), `curl` (weather geocode /
Open-Meteo / wttr.in; the pill stays empty without it).

## Build & run

(Full walkthrough including the systemd service, every Omarchy-side
change, and uninstall: [INSTALL.md](INSTALL.md).)

```sh
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
./build/mattbar
```

Autostart in `~/.config/hypr/hyprland.conf`:

```ini
exec-once = /path/to/mattbar
```

Since MattBar reserves no space, you don't need `gaps_out` tweaks. If you use
blur, you can target it with a layer rule:

```ini
layerrule = blur, mattbar
```

## Module placement

Every module's position is data, not code: `layout_left`, `layout_center`,
`layout_right`, and `layout_more` in the conf file are ordered, comma-separated
lists of module ids (`omarchy, workspaces, clock, pin, tray, more, update, …`).
**More** is a fourth assignment target, not a fourth stretch of the bar: modules
parked there hide behind a ⋯ control (itself placed in left/center/right).
The settings window's **Modules & layout** section edits this live: each module
row has a visibility checkbox, an L/C/R/M zone selector, and up/down arrows
to reorder within its zone — the bar rearranges instantly as you click.
Hand-edited configs are normalized on load: unknown ids and duplicates are
dropped, and any module you forgot to mention is appended to the right zone,
so nothing can be lost.

## Configuration

MattBar is configured at runtime via `~/.config/mattbar/mattbar.conf`
(simple `key = value`; created automatically on first change). Click the **gear icon in the
tray** to open the built-in settings window: bar height, hide delay, tray
auto-collapse timeout, font size, hot-strip heights, **every bar color
(see Colors below)**, and per-module visibility toggles. All
changes apply and persist instantly — there is no Save button because
there is nothing to save separately. The window scrolls with the mouse wheel (fixed header/footer, scrollbar
indicator), so it stays compact on small displays. It is click-only by
design (steppers/sliders/checkboxes), so it needs no keyboard focus and
never steals input from your windows.

### Colors

**Follow Omarchy theme** (the toggle at the top of the Colors section,
`follow_omarchy_theme` in the conf, **off by default**) makes MattBar track
Omarchy's active theme the way Waybar does. Both theme generations are
understood: the rich `colors.toml` palette (Omarchy ≥ 3.8 and 4.x/quattro,
mapping accent/selection/dark_foreground/red onto MattBar's accent, pills,
dim, and urgent colors) and the older two-tone `waybar.css`
(`@define-color foreground/background`, with the remaining colors derived).
The theme directory is found automatically at
`~/.local/state/omarchy/current/theme` (4.x) or
`~/.config/omarchy/current/theme` (3.x). Because Omarchy *restarts* Waybar
on theme change rather than signaling it, MattBar instead watches the
theme directory with inotify — running `omarchy-theme-set` re-colors the
bar instantly, no restarts, no script changes. The alpha of your
background and hot-strip colors is preserved, so translucency preferences
survive theme swaps.

Your own palette is never lost: the conf file always stores *your* colors,
the Omarchy theme is only overlaid at runtime, and switching the toggle
off restores your colors immediately. While following, manual color
editing is hidden (it would be clobbered on the next theme swap) and the
section shows the active theme's name, a read-only swatch grid, and the
live preview.

With the toggle off, the **theme selector** works as before:
one click applies a complete baked palette — **Default** (the stock dark
theme, sourced from the same defaults in `config.hpp`), **Light**,
**Nord**, **Gruvbox**, **Dracula**, and **Solarized Light**. The preset
that exactly matches your current colors is highlighted; the moment you
fine-tune any slider, no preset lights up — you're on a custom theme, and
presets remain one click away as clean starting points.

Below that, every color MattBar draws with is individually themable: pick a swatch (Background, Text, Dim text, Accent,
Urgent, Pills & hover, Hot strip), adjust its RGBA sliders, and the bar —
and the settings window itself — restyle live. Swatches sit on a
checkerboard so translucent values are readable, and a miniature bar
preview shows all seven colors in context (strip line, active/inactive
workspace pills, clock, dim label, urgent reading). **Reset** restores the
selected color's default; **Reset all** restores the stock theme. The
shipped defaults are exactly what you see before touching anything.

The same colors are plain conf keys (`#RRGGBB` or `#RRGGBBAA`), so themes
can also be scripted or copied between machines:

```ini
bg_color = #12131880      # bar background
fg_color = #dee0e8        # normal text
dim_color = #858b99       # inactive text
accent_color = #498ff5    # active workspace, pin, selection
urgent_color = #f0615c    # low battery / over-temperature
pill_color = #262933      # workspace pills, menu hover
strip_color = #498ff559   # hidden-state hot strip line
```

The Temperature section lets you pick the sensor (cycles through
everything discovered under `/sys/class/hwmon`, with an `auto` mode that
prefers `k10temp`/`zenpower`/`coretemp` CPU package sensors) and set the
red-warning threshold. `MATTBAR_HWMON` overrides the hwmon path for
debugging.

New settings sections: **Monitors** (multi-monitor, which displays carry a
bar, which one is primary), **Notification centre** (bell count, history
size, per-app mute list), **Clock & calendar** (enable, Monday-first, week
numbers) and **Power profile** (show the profile name).

Options without a settings widget yet (fonts, glyphs, Omarchy commands,
update interval/signal, the calendar and power-profile glyphs) can be
edited directly in the conf file.

### Omarchy integration

The Omarchy menu button and update indicator replicate Omarchy's Waybar
`custom/omarchy` and `custom/update` modules, including the private
`omarchy` logo font glyph (`\ue900`), `omarchy-update-available` polling,
and the `pkill -RTMIN+7`-style refresh signal Omarchy's scripts send after
updates (send it to mattbar instead: `pkill -RTMIN+7 mattbar`). The default
font is CaskaydiaMono Nerd Font (Omarchy's default) so Nerd Font glyphs
render out of the box; on non-Omarchy systems the two modules can be
switched off in settings or the commands repointed in the conf file.

## Notifications & OSD (opt-in)

MattBar can act as the desktop notification daemon
(`org.freedesktop.Notifications`) and draw a volume/brightness OSD. Both are
**off by default** (`enable_notifications`, `enable_osd`) so a stock setup
keeps mako or the Omarchy shell in charge.

**Taking over the notification name.** Only one daemon can own it.
`notifications_takeover` (on by default once you enable the daemon) does
the handoff:

- **mako / dunst:** MattBar queues, then SIGTERMs the owner. The queued
  claim promotes instantly. A later mako that starts while MattBar holds
  the name fails its own acquire and exits.
- **Omarchy Quattro (quickshell):** by default MattBar does **not** kill
  the shell (that would take down menus, the lock screen, and every
  `omarchy-shell` IPC caller — including the wifi / bluetooth / volume
  panels the bar itself opens). It asks the shell to disable
  `omarchy.notifications` over IPC, then restarts the shell once.
  Quickshell's NotificationServer is a process-lifetime singleton —
  unloading the plugin does not drop the bus name until the process
  exits. The disable persists in `~/.config/omarchy/shell.json` so the
  next login does not race. Turning MattBar's daemon off re-enables
  the plugin. **Shut down the Quickshell instance completely**
  (`quickshell_shutdown`) is the opt-in to stop the whole process
  instead: those panels (and the rest of the shell) go away, and
  toggling it off starts the shell again.

If you see *both* MattBar and another daemon's popups, takeover is off
or the other process is not one of the above — MattBar's own popups in
that state are only its OSD and battery alerts. On 3.x, also stop mako
from being autostarted: in `~/.config/hypr/autostart.conf` remove or
comment `exec-once = uwsm-app -- mako`.

**Volume keys on Omarchy 3.x (swayosd).** If a volume overlay still appears
after MattBar has taken over notifications, that overlay is swayosd, not a
notification: Omarchy 3.x binds the media keys to `omarchy-swayosd-client`,
which performs the change *and* draws its own OSD. swayosd-server is D-Bus
activated, so terminating it is pointless - the next keypress brings it
back. The fix is at the keybind level: rebind the keys to plain
`wpctl` / `brightnessctl` / `playerctl`. MattBar's OSD is event-driven
(PipeWire volume and mic events, backlight uevents), so it shows the OSD
for any change no matter what triggered it - including the mic-mute key,
which gets its own `mic` OSD. A ready-made drop-in with all the
unbind/rebind lines is included at
`contrib/omarchy-3x-media-keys/mattbar-media-keys.conf`.

**Omarchy 4 ("Quattro")** replaced swayosd with an overlay drawn by the
Quickshell shell, and moved the keybinds to Lua - so the 3.x hyprlang
drop-in silently stops applying after the upgrade and the stock overlays
return. The shell's OSD is IPC-only (it never watches PipeWire itself),
which allows a gentler fix: keep Omarchy's media scripts and just null
their `omarchy-osd` call. See `contrib/omarchy-4x-media-keys/` - it
retains Quattro's DSP-sink resolution, DDC/Apple external-display
brightness, and the hardware mic-mute LED the 3.x drop-in had to trade
away.

**Control signals** (replace the `makoctl` keybinds in
`~/.config/hypr/bindings/utilities.conf`):

| action | mako binding used by Omarchy | MattBar equivalent |
|---|---|---|
| dismiss last | `makoctl dismiss` | `pkill -RTMIN+2 mattbar` |
| dismiss all | `makoctl dismiss --all` | `pkill -RTMIN+3 mattbar` |
| invoke last action | `makoctl invoke` | `pkill -RTMIN+4 mattbar` |
| toggle do-not-disturb | `omarchy-toggle-notification-silencing` | `pkill -RTMIN+5 mattbar` |
| restore last | `makoctl restore` | `pkill -RTMIN+6 mattbar` |

### Notification centre (the bell)

The **notifications** module is a bell in the bar. Click it and the last
30 notifications (`history_max`, 5–100) drop down in a panel: app name,
what it said, and how long ago, newest first, with anything still on
screen at the top. Scroll for older ones. **Clear all** in the corner
dismisses what's showing and empties the history in one go.

The bell is deliberately undemanding. It does not grow a badge, pulse,
change color, or otherwise ask to be looked at — its only ambient signal
is being slightly dimmed when there's nothing to read
(`bell_dim_when_empty`), and you can turn even that off. If you *want* a
count, `bell_show_count` puts one next to it, but it is off by default:
the point of the panel is that notifications wait for you, not the
reverse. Right-clicking the bell toggles do-not-disturb, the same as
`SIGRTMIN+5`.

**Muting an app.** Right-click any entry in the panel — right where you
just read the thing that annoyed you — and that app is muted. Muted apps
never pop up again, but they are *still recorded*, so muting costs you
nothing: everything is in the history when you go looking. The same list
is editable in settings (**Notification centre → Muted apps**), which
remembers every app that has ever notified you, so you can mute Discord
at three in the afternoon rather than at the moment it interrupts you.
In the conf file it is one key:

```ini
muted_apps = Spotify,discord
```

History and muting only apply while MattBar is the notification daemon
(`enable_notifications`); under mako the panel says so rather than
pretending to be empty.

### mattbarctl

Signals can't carry arguments or answer questions, so MattBar also listens
on `$XDG_RUNTIME_DIR/mattbar.sock` (mode 0600, same-uid only). There is no
`/tmp` fallback — without a runtime directory the bar still runs, but
`mattbarctl` cannot reach it. `mattbarctl` (a symlink to the mattbar
binary, installed by `cmake --install`; `mattbar ctl ...` works too) talks
to it:

```sh
mattbarctl dnd on|off|toggle|status   # do-not-disturb, with an answer
mattbarctl dismiss | dismiss-all | invoke | restore
mattbarctl profile                    # prints active + available profiles
mattbarctl profile performance        # switches, radio dot follows
mattbarctl pin on|off|toggle|status
mattbarctl reveal | hide              # momentary show/hide of every bar
mattbarctl settings                   # open the settings window (same as the tray gear)
mattbarctl status                     # one-line summary
```

The signal bindings keep working; the socket is additive. Scripts can now
*read* state ("is DND on?") instead of only poking it.

Each feedback type can be toggled individually in the same settings
section: the volume, microphone, and brightness OSDs (`osd_volume`,
`osd_mic`, `osd_brightness`) and MattBar's own alerts - low/critical
battery (`notify_battery`) and Bluetooth device connect/disconnect
(`notify_bluetooth`, name-aware and quiet during the initial state sync).

Timing is settings-controlled: `notification_timeout_s` steps from 1-300s or down to `never` (0; critical notifications never expire regardless), `osd_timeout_ms` sets how long the volume/mic/brightness OSD lingers (250ms steps), and `notification_max_shown` caps how many popups stack at once (1-10); do-not-disturb suppresses everything below critical while
keeping history. Notification text supports emoji: the daemon splits each
line into runs, drawing what the bar font maps with the bar font and
everything else with `notification_emoji_font` (default `Noto Color Emoji`,
which Omarchy ships) - color rendering needs cairo >= 1.17.8, and since no
text shaper is involved, ZWJ sequences degrade gracefully to their
constituent emoji. The OSD shows the numeric percentage right-aligned
beside the bar (the bar's extent is reserved off `100%` so it doesn't
jitter as digits change while a key is held; boosted volume reads e.g.
`150%`). It reacts to PipeWire volume/mute and backlight
changes, event-driven end to end - and it displays only on an *actual*
level or mute change: PipeWire emits sink/source events for streams
starting and nodes waking or suspending too, so the daemon caches the last
known state and stays quiet for events that carry no change.

## Design notes

- **Event loop**: single-threaded epoll multiplexing the Wayland fd, two
  timerfds (periodic tick + hide delay), Hyprland's `.socket2` event stream,
  and the sd-bus fd. Modules register fds via `Bar::add_fd`.
- **Rendering**: on-demand only. Each frame allocates a `memfd` SHM buffer,
  draws with Cairo, and frees it on the compositor's `release`. There is no
  animation loop.
- **Idle cost**: the hidden frame is cached — module updates, Hyprland
  IPC events, and tray D-Bus traffic cause zero redraws and zero IPC while
  the bar is hidden (workspace state is marked stale and re-queried at the
  moment of reveal; tray icon reloads are throttled to 1/s per item).
  Measured under a headless compositor: 0.0% CPU hidden even at 100
  synthetic workspace events/sec. While hidden, the periodic tick timer is
  also fully disarmed —
  an idle hidden MattBar schedules **zero** wakeups and sleeps in `epoll`
  until a pointer touch or workspace event arrives. All modules refresh at
  the instant of reveal. No subprocess ever blocks the event loop: every external query (wpctl,
pactl, iw, the update check) runs through an async runner that delivers
results via epoll and coalesces bursts, so the bar keeps painting and
popups keep expiring even while the audio or Bluetooth stack is being
restarted underneath it. Volume and network are event-driven too
  (shared `pactl subscribe` stream, rtnetlink socket), so at steady state a
  *visible* bar spawns no subprocesses either — `wpctl`/`iw` run only when
  the mixer or the network actually changes, and never while hidden.
- **Tray menus**: right-clicking a tray icon fetches the app's
  `com.canonical.dbusmenu` layout over D-Bus and renders it in an
  `xdg_popup` parented to the layer surface, with an input grab so clicking
  elsewhere dismisses it. Submenus navigate in place with a `< Back` row.
- **Hyprland IPC**: one-shot request sockets for `j/workspaces` /
  `dispatch …`, persistent event socket for change notifications. Falls back
  to polling if the event socket is unavailable.

All subprocess invocations (wpctl/pactl/iw/update checks) are wrapped in
`timeout`, so a hung audio daemon or network tool can briefly fail a
reading but can never freeze the bar.

## Troubleshooting

**SNI spec compliance (registration feedback loop):** the watcher
broadcasts `StatusNotifierItemRegistered` / `StatusNotifierHostRegistered`
only for genuinely NEW registrations, as the spec requires. Echoing every
registration call creates a feedback loop with tray libraries that
legitimately re-register upon hearing those broadcasts — observed in the
wild as one client hammering the watcher thousands of times per second
(12M+ calls). Verified with a reactive test client: 2 calls total, then
silence.

**D-Bus event-loop correctness:** MattBar drives sd-bus by the book —
it polls with exactly the events `sd_bus_get_events()` requests
(POLLIN/POLLOUT, re-armed after every dispatch) and drives the connection
at `sd_bus_get_timeout()` via a dedicated timer, instead of a hardcoded
EPOLLIN (which can spin at thousands of empty wakeups/second on some
systemd versions when sd-bus's state machine doesn't currently want to
read). A watchdog backs this up: ~2000 consecutive wakeups that consume
nothing trigger a diagnostic line with the raw fd state and a connection
reset. Lost connections reconnect automatically with backoff (5 attempts,
3 s apart); tray apps re-register when the watcher name reappears.

**Constant CPU wakeups from D-Bus:** fixed — MattBar previously
subscribed to every `NameOwnerChanged` broadcast on the session bus, so
one chatty or reconnect-looping client anywhere in the session (each
short-lived connection produces two such broadcasts) meant thousands of
deliveries per second to MattBar. Subscriptions are now installed
per tracked tray item and scoped to that item's bus name, so the bus
daemon filters everything else; verified 0% CPU under a 14,000-msg/s
synthetic churn storm. With `MATTBAR_DEBUG=1`, any D-Bus message that
still reaches MattBar is tallied by member and sender in a
`dbus messages over Ns:` line, naming the source directly.

**Constant CPU usage while hidden:** fixed in this version — the cause was
the D-Bus connection dying (a dead fd is permanently readable, so the
event loop spun on it at thousands of wakeups/second). MattBar now
detects a lost bus, logs `D-Bus connection lost`, disables the tray, and
goes back to zero wakeups. Related: tray-app registration used to be able
to deadlock MattBar against the registering app for sd-bus's 25-second
default timeout (frozen, unresponsive startup); registration now replies
before calling back into the app, and every synchronous D-Bus call is
capped at 500 ms.

**Debugging interaction issues:** run `MATTBAR_DEBUG=1 mattbar` from a
terminal — it logs pointer enter/leave, expand/hide decisions (with the
exact reason a hide was skipped: pinned, pointer inside, menu open), every
draw with its mode and size, and — whenever wakeups occur — a summary line
every ~5 s attributing them to their source: `wayland` (compositor
protocol traffic), `hypr-events` (Hyprland IPC), `dbus` (tray bus
traffic), `tick-timer`/`hide-timer`/`tray-collapse`/`update-signal`
(internal timers), plus dispatched Wayland event and draw counts. An idle
hidden bar prints nothing; whatever appears in these lines is what's
consuming CPU.

**Strip visible but hovering does nothing:** raise "Hover zone height" in
settings (or `strip_hit_height` in the conf) — very thin edge targets are
unreliable on some compositor/output combinations, which is why the input
zone is decoupled from the visible line. If you run multiple monitors,
also check the bar landed on the display you expect and pin it with
`output = <name>` (`hyprctl monitors` shows names). If Omarchy's stock
Waybar is still autostarting, disable it so two bars aren't stacked; on
Omarchy 4.x/quattro the built-in bar lives inside `omarchy-shell` instead
— see `contrib/omarchy-null-bar/` for the supported way to switch it off
while keeping the rest of the shell running.
With blur rules, add `layerrule = ignorezero, mattbar` so the transparent
hidden-state area isn't blurred.

## Memory footprint

Deliberately small, and audited: notification icons are downscaled at
intake to 96 px (3x their 32 px draw size, HiDPI-proof) so 30 history
entries cost ~1 MB worst case instead of ~8 MB; the pointer cursor uses
the compositor-side `cursor-shape-v1` protocol on compositors that offer
it (Hyprland does), so no multi-megabyte cursor theme is ever loaded
client-side (classic `wl_cursor` remains the fallback); and librsvg is
dlopen'd on first SVG icon rather than linked, keeping glib's resident
setup out of every session that never meets an SVG.

## Current limitations

- Items without a dbusmenu interface fall back to the SNI `ContextMenu`
  call. Menu size changes on submenu navigation redraw in place rather
  than repositioning, so a much wider submenu may extend past the screen
  edge in rare cases.
- Without librsvg installed at runtime, SVG-only tray icons fall back to
  a placeholder square.
- On compositors without `wp-fractional-scale-v1` / `wp_viewporter`,
  fractional outputs render at the next integer scale and the compositor
  downsamples.
- The calendar shows dates only — no event or reminder integration.
- Power profiles come from power-profiles-daemon; `tuned-ppd` presents the
  same D-Bus interface and works, but bare `tuned` without the shim does
  not.
- Text labels are plain ASCII by default — swap `FONT` for a Nerd Font and
  use glyphs in the module strings if you prefer icons.

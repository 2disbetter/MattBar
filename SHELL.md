# MattBar Shell

A complete, in-process replacement for Omarchy 4’s Quickshell host
(`omarchy-shell`). Same job — bar, panels, overlays, lock, polkit,
wallpaper, idle — in the MattBar process: C++ , `wayland-client` +
Cairo + `sd-bus`, no Qt, no GTK, no QML plugin loader.

This document is the inventory and the plan. Implementation status is
at the bottom.

## Why

MattBar exists because the bar itself should be configurable, small,
C++, and auto-hiding without stealing exclusive zone from windows.
On this machine the running costs are not close:

| | MattBar | Quickshell (`omarchy-shell`) |
|---|---|---|
| RSS | ~20 MB | ~400 MB |
| PSS | ~5.5 MB | ~227 MB |
| Threads | 1 | 25 |
| Mapped `.so` | 32 | 196 |
| Binary | 1.2 MB | 189 MB |

The gap is the Qt 6 / QML / OpenGL stack, not the wifi panel. Adding
Cairo overlays does not import that stack. The panels are code; Qt is
the tax.

Killing Quickshell without a replacement bricks the session: lock
screen, polkit prompts, wallpaper, idle auto-lock, Super+Space menu,
and every `omarchy-shell` keybind go with it. That is why MattBar grew
`quickshell_shutdown` as an opt-in, and why this shell exists as the
thing you turn on *instead*.

## Decisions

**One process, opt-in.** The replacement lives in `mattbar`, but it
only *takes over* when **Shut down the Quickshell instance completely**
is on (`quickshell_shutdown`). Off (the default): Quickshell stays up
for Super+Space, wifi/bluetooth/volume panels, wallpaper and lock;
MattBar only claims notifications if takeover is on. On: Quickshell is
stopped, MattBar maps wallpaper and lock, the logo click opens
MattBar’s menu, and a runtime PATH shim (`$XDG_RUNTIME_DIR/mattbar/bin`)
is prepended so `omarchy-shell` / `omarchy-menu` / `omarchy-system-lock`
hit MattBar instead of the dead Quickshell.

**No plugin host.** Quickshell’s QML `PluginRegistry` / third-party
`~/.config/omarchy/plugins` / `{type:"qml"}` bar modules are a
platform, not a feature. MattBar Shell is a closed set of C++
modules. Custom bar slots can stay as `type: command` (exec a script)
if we ever want them.

**Match the Quickshell TUIs.** The replacement is not a “good enough”
subset. If Omarchy’s panel shows a control, a row, or a reading, MattBar
should show it too — same sections, same actions, same CLIs behind them
(`omarchy-network-status`, `omarchy-network-band`, `omarchy-dns`,
`omarchy-bluetooth-device`, `omarchy-display-text-size`, …). Chrome can
be Cairo instead of QML; the information architecture should not
shrink. New work starts by reading the corresponding Quickshell
`Panel.qml`, not by inventing a slimmer layout.

**Compatible IPC, not a compatible QML ABI.** Stock Omarchy scripts
and Hyprland binds call `omarchy-shell <target> <method> [args]`,
which is `qs ipc -p $OMARCHY_PATH/shell`. We do not speak Quickshell
IPC. We speak `mattbarctl`, and ship a shim that looks like
`omarchy-shell` so existing binds keep working once Quickshell is
down. See [IPC](#ipc).

**No extra toolkit.** Dependencies stay in the same class as today:
Wayland, Cairo, libsystemd, plus **libxkbcommon** (keyboard — there
is no honest way to do lock/menu/wifi passwords without it) and
**PAM** (lock + polkit). JPEG/WebP for wallpapers: header-only
`stb_image`, not libjpeg. QR: a small encoder in-tree. System
daemons stay system daemons (PipeWire, NetworkManager, bluetoothd,
polkitd, logind, Hyprland).

**Build order is forced.** Chassis, then session-critical services,
then the menu, then the panels people click. Completeness extras last.
See [Build order](#build-order).

## Architecture

```
Hyprland binds / omarchy-* scripts
        │
        ▼
  omarchy-shell shim  ──►  mattbarctl  ──►  $XDG_RUNTIME_DIR/mattbar.sock
                                                │
                                                ▼
                                            Shell host
                          ┌──────────── overlays / panels ────────────┐
                          │ menu, clipboard, emoji, image-picker,     │
                          │ audio, network, bluetooth, display, …     │
                          └───────────────────────────────────────────┘
                          ┌──────────── session services ─────────────┐
                          │ lock, polkit, wallpaper, idle,            │
                          │ night light, notifications, OSD, media    │
                          └───────────────────────────────────────────┘
                          ┌──────────── bar (already exists) ─────────┐
                          │ auto-hide layer surfaces, modules, tray   │
                          └───────────────────────────────────────────┘
```

The bar stays the bar. New work is a **named overlay registry**
(`toggle` / `summon` / `hide` / `call` by plugin id such as
`omarchy.audio`) plus session services that have no UI on the bar.

Layer-shell keyboard: bind protocol v4 so popups can use
`on_demand` focus (click to type, do not steal keys from apps).
Lock uses `ext-session-lock-v1`, not layer-shell exclusive.

## Chassis (must exist before the rest)

| Piece | Notes |
|---|---|
| Keyboard-interactive layer surfaces | `wl_keyboard` + xkbcommon; `on_demand` for panels, exclusive only where required |
| Text fields | Password, search, Wi-Fi PSK. ASCII via keymap UTF-8 first; IME (`text-input-v3`) later if CJK/compose is needed |
| Scrollable lists / grids | One widget used by every panel |
| Image decode | Cairo PNG already; `stb_image` for wallpaper / clipboard JPEG/WebP |
| Named summon/hide | Registry keyed by Omarchy plugin id |
| Control socket | Extend `mattbarctl`; shim `omarchy-shell` |
| Theme tokens | Bar palette already follows Omarchy; lock/polkit/menu have extra tokens in `shell.toml` |

Wayland protocols to add: `ext-session-lock-v1` (lock),
`ext-idle-notify-v1` (auto-lock; we already have idle-*inhibit* for
caffeine). Clipboard can keep using a helper (`wl-clipboard` /
Omarchy’s `capture.sh`) rather than `wlr-data-control`.

## Inventory

**have** = MattBar already does it. **partial** = status or a lesser
UI exists. **new** = not started.

### Session-critical (desktop is broken without these)

| Module | Status | What it must do |
|---|---|---|
| Session lock | done | `ext-session-lock-v1` on every output; PAM `omarchy-lock-password`; parallel `omarchy-lock-fingerprint` when fprintd reports an enrolled finger |
| Polkit agent | done | `org.freedesktop.PolicyKit1.AuthenticationAgent`; password via `polkit-agent-helper-1`. Fingerprint via `pam_fprintd` (lid-closed skips it) |
| Wallpaper | done | Fullscreen layer per output; 420ms slanted wipe on `set` / `transition` / `refresh`; `setInstant` snaps |
| Idle | done | Screensaver after N s, lock after M s (`idle.lock` / `idle.screensaver` in shell.json); enable/disable/toggle (Stay Awake); caffeine inhibitor respected |
| Night light | done | Own hyprsunset temperature (4000 / 6500 K); enable/disable/toggle/status/refresh; bar moon indicator |
| Notifications | have | Daemon, popups, DND, history. Still need IPC aliases (`dismissOne`, `dismissAll`, `invokeLast`, `showHistory`, `toggleDnd`) |
| OSD | partial | Volume/mic/brightness exist. Missing generic `omarchy-osd` payload (icon/message/progress/duration), app-launch feedback, media toasts |
| Media transport | partial | Bar MPRIS exists. Hardware keys call `media playPause/next/previous/sourceSwitch` even while locked |

### Bar modules

| Module | Status | Gap |
|---|---|---|
| Workspaces | have | |
| Clock + calendar | done | Calendar; right-click cycles format; middle-click timezone picker |
| Pin / auto-hide | have | Keep; Omarchy’s bar does not auto-hide |
| Tray | have | |
| Menu button | partial | Must open *our* menu, not `omarchy-menu` → Quickshell |
| Update | have | |
| Agents indicator | done | Status + usage panel (`omarchy.agents`) |
| Screen recording | have | |
| Network status | partial | SSID yes; panel is new |
| Bluetooth status | partial | Status yes; device list is new |
| Volume / mic | partial | Readout yes; mixer panel is new |
| Brightness | done | Slider, scale, monitors; laptop lid/mirror when eDP+external exist |
| Media | done | Play/pause, scroll skip; right-click cover-art popup; middle-click / `media sourceSwitch` cycles player |
| Notifications bell | have | |
| Power / battery | done | Profiles popup; battery click opens stats panel; right-click toggles % |
| Caffeine | have | Must agree with the idle service |
| Temp | have | Omarchy has no equivalent; keep it |
| Active window title | done | Hyprland activewindow; middle/right closes |
| Keyboard layout | done | Hidden until 2+ layouts; click cycles |
| Weather pill | done | Icon + temp; click opens the panel |
| Indicators | done | DND, rec, caffeine, night light, reminder, dictation |
| Tailscale / Dropbox pills | done | Hide when the CLI is missing |
| Command widgets | optional | Omarchy `{type:"command", exec:…}` slots |

### Panels

| Panel | Status | Backend + UI |
|---|---|---|
| Audio | done | Matches QS: hero mood name + mute, OUTPUT / INPUT sliders, sink/source picker, APPLICATIONS mixer |
| Microphone | partial | Folded into the audio panel; bar module still has mute/scroll |
| Bluetooth | done | Matches QS: hero + power, CONNECTED / PAIRED / AVAILABLE, battery, pair/connect/forget |
| Network | done | Matches QS: hero + radio/QR/speed-test, ping/loss/throughput/IP, band pin, DNS pills, KNOWN/OTHER wifi + PSK |
| Display | done | Matches QS: brightness mood, TEXT SIZE, SCALE pills, MONITORS |
| Power | done | Battery stats, profiles, `togglePercentage` |
| Clock / calendar | done | Month grid + timezone overlay |
| Agents | done | Hero, subscription switch, limits/balance, tokens by day/model |
| Weather | done | Open-Meteo (coords) + wttr.in (IP auto); hero + 3-day; location search |
| Tailscale | done | On/off, self IP, machines (click copies IP), EXIT NODES, Mullvad region picker |
| Dropbox | done | Status, storage meter, pause/resume, login, recent files |
| Wi-Fi QR | new | Encode current SSID/PSK (in-tree encoder) |
| Speed test | new | Download/upload dials |
| Disk speed test | done | Live READ/WRITE MB/s dials via `omarchy-disk-speedtest` |

### Overlays

| Overlay | Status | Notes |
|---|---|---|
| Omarchy menu | new | Parse `omarchy-menu.jsonc` + `~/.config/omarchy/extensions/omarchy-menu.jsonc`; `when:` / `checked:`; run `action:`; routes (`apps`, `system`, `capture`, …). This is Super+Space |
| App library | new | `.desktop` scan, hidden-entry filter, icons, launch OSD. Menu’s Apps page |
| Clipboard history | done | Reads Omarchy's history JSON; search, paste, confirm-clear |
| Emoji picker | done | Search Omarchy's emoji list; insert or copy |
| Image picker | done | Directory grid; `selectionFile` / `doneFile` round-trip used by wallpaper and theme tools |
| Reminders flow | done | Minutes + message overlay; `omarchy-reminder` still runs the timer |
| Dev gallery | skip | Internal UI kit preview |

## IPC

Callers keep using `omarchy-shell`. The shim turns that into
`mattbarctl`. Native form:

```
mattbarctl shell ping
mattbarctl shell toggle omarchy.audio
mattbarctl shell summon omarchy.menu '{"menu":"root"}'
mattbarctl shell hide omarchy.menu
mattbarctl lock lock
mattbarctl osd show '{"icon":"brightness","value":"50"}'
mattbarctl media playPause
mattbarctl notifications dismissOne
mattbarctl idle toggle
mattbarctl nightlight toggle
mattbarctl background refresh
```

Targets that must eventually answer (Omarchy binds and scripts):

| Target | Callers |
|---|---|
| `shell` ping / toggle / summon / hide / togglePanelAt / setPluginEnabled / applyTheme | `omarchy-shell`, `omarchy-menu`, theme apply, MattBar takeover |
| `lock` lock / status / isLocked | `omarchy-system-lock`, sleep-lock, `omarchy-restart-shell` |
| `osd` show | `omarchy-osd` |
| `media` playPause / next / previous / sourceSwitch | media keys, including while locked |
| `notifications` dismissOne / dismissAll / invokeLast / showHistory / toggleDnd | Super+comma family |
| `idle` enable / disable / toggle / status | Stay Awake, Super+Ctrl+I |
| `nightlight` toggle / status | Super+Ctrl+N |
| `background` refresh / set / transition / themeTransition | wallpaper + theme tools |
| `image-selector` open / cancel | `omarchy-menu-images` |
| `omarchy.{audio,bluetooth,network,monitor,power,clock,agents,…}` toggle | Super+Ctrl+A/B/W/… and bar module clicks |

`setPluginEnabled` for `omarchy.notifications` is already handled by
MattBar’s takeover path talking to Quickshell. Once we *are* the
shell, that call becomes a no-op or a local flag.

## What we will not recreate

- Qt / QML / Quickshell plugin ABI
- Omarchy’s built-in bar as a second bar
- Dev gallery
- PipeWire, NetworkManager, bluetoothd, polkitd, logind, PAM, Hyprland
  themselves

## Build order

1. **Chassis** — keyboard, text field, overlay registry, `mattbarctl shell *`, shim, Escape-to-dismiss
2. **Session** — lock, polkit, wallpaper, idle
3. **Menu + app library** — otherwise Super+Space is dead
4. **OSD + media IPC** — otherwise volume/media keys are dead once Quickshell is gone
5. **Audio / network / bluetooth / display panels** — the original reason
6. **Clipboard, emoji, image picker** — theme/wallpaper tools
7. **Night light, weather, agents panel, tailscale/dropbox, QR, speed tests**

1–4 are the session. 5 is the product. 6–7 are completeness.

Do not enable `quickshell_shutdown` for daily use until 1–4 work.
Lock in particular: a bug there can leave the session frozen.

## Implementation status

Updated as work lands.

| Area | State |
|---|---|
| This document | done |
| Keyboard + xkbcommon | done (1.30.0) |
| Overlay registry + `mattbarctl shell` | done |
| Takeover gated on `quickshell_shutdown` | done (logo click, PATH shim, wallpaper) |
| Command menu | done — Super+Space while takeover is on; Super+Space returns to Quickshell’s `omarchy-menu` when the toggle is off. Nerd Font / `iconFont` / app PNG icons; font size is **Settings → Shell** (`shell_font_size`) while Quickshell is shut down |
| Calendar IPC (`omarchy.clock`) | done |
| Notifications / OSD / media IPC aliases | done |
| Wallpaper (JPEG/PNG, shutdown only) | done |
| Session lock (ext-session-lock-v1 + PAM) | done (password; no fingerprint yet) |
| Polkit agent | done (password dialog; only while takeover is on) |
| Idle auto-lock / screensaver | done (`ext-idle-notify-v1`; only while takeover is on) |
| Panels (audio, network, bluetooth, display) | done (1.31.1) — layout matches the Quickshell TUIs (hero, sections, stats, band/DNS, text size). QR + speed-test overlays included. Laptop clamshell/mirror still later |
| Clipboard, emoji, image picker | done (1.31.0) — Super+Ctrl+V / Super+Ctrl+E and `image-selector` IPC |
| Night light | done (1.32.0) — hyprsunset 4000/6500 K, bar moon, `mattbarctl nightlight *`, Super+Ctrl+N still uses `omarchy toggle nightlight` then refreshes us |
| Weather | done (1.33.0) — bar pill + panel (Open-Meteo / wttr, location search, °C/°F) |
| Agents panel | done (1.34.0) — usage dashboard from `omarchy-agent-usage-update` records |
| Disk speed test | done (1.35.0) — live READ/WRITE MB/s dials (`omarchy-disk-speedtest`) |
| Clock timezone, laptop lid/mirror, power panel, active window, keyboard layout, reminders, dictation | done (1.36.0) |
| Tailscale / Dropbox | done (1.36.0) — last on the list, pills hide if the CLI is missing |

## Related

- Bar behaviour, modules, notifications: [README.md](README.md)
- Install / takeover / media-key shims: [INSTALL.md](INSTALL.md)
- `quickshell_shutdown` setting: Notifications & OSD in the in-bar settings window

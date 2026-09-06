# MattBar plugin row (`mattbar.plugin-bar`)

Optional Quickshell bar plugin that draws a thin strip of *user*
`bar-widget` plugins on the desktop-facing side of MattBar.

MattBar itself stays a Cairo / wayland-client bar on the screen edge with
a zero exclusive zone. This plugin is not a second MattBar: it is the
sidecar's `bar.id`, selected only while Settings → Shell → **Plugin row**
is on and at least one checked plugin has `kind: "bar-widget"`.

Overlay, panel, and menu plugins stay on MattBar's Plugins chip and the
`omarchy-shell shell summon <id>` path. They are not placed on this row.

Bar-widgets use the same four zones as MattBar modules:

| Zone | On the strip |
|------|----------------|
| Left | packed to the start (top on a vertical row) |
| Center | centered as a group |
| Right | packed to the end (bottom on a vertical row) |
| More | ⋯ overflow on the row, not mixed into MattBar's Cairo More |

Settings → Shell → Plugins shows L/C/R/M plus ^/v on each checked
bar-widget when Plugin row is on. Existing `qs_plugin_layout` order
migrates into left.

## Stacking

Always inward from MattBar so a pointer coming from windows hits the
plugin row first:

| MattBar edge | Plugin row |
|--------------|------------|
| top          | below MattBar |
| bottom       | above MattBar |
| left         | to the right of MattBar |
| right        | to the left of MattBar |

When MattBar is hidden, this row is parked just off the screen edge —
no second hot strip in the middle of the desktop. The edge hover zone
is still MattBar's strip. Revealing MattBar writes
`$XDG_RUNTIME_DIR/mattbar.plugin-bar`; hovering the row writes
`$XDG_RUNTIME_DIR/mattbar.plugin-bar-hover` so both stay up as one
auto-hide family.

The layer uses `ExclusionMode.Ignore` (and does **not** set
`exclusiveZone`, which would flip the mode back to Normal). Windows
never shrink.

## Install

`install.sh` copies this directory to
`~/.config/omarchy/plugins/mattbar.plugin-bar`. You do not select it as
the user session's `bar.id` — that would replace Omarchy's bar while
Quickshell is still the shell. MattBar's sidecar writes `bar.id` into
its private `shell.json` only while the plugin row is on.

```
~/.config/omarchy/plugins/mattbar.plugin-bar/
  manifest.json
  Bar.qml
  README.md
```

Needs `mattbar.null-bar` as well: the sidecar falls back to the null bar
when the row is off or no bar-widget plugin is placed.

## Contract with MattBar

State file (`$XDG_RUNTIME_DIR/mattbar.plugin-bar`), key=value lines:

```
on=0|1
expanded=0|1
pinned=0|1
position=top|bottom|left|right
offset=<px to clear MattBar>
height=<row thickness>
font=<optional family>
fg=<r,g,b,a>
bg=<r,g,b,a>
urgent=<r,g,b,a>
```

Hover file (`$XDG_RUNTIME_DIR/mattbar.plugin-bar-hover`): `0` or `1`.

The QML root accepts host-injected properties *without* `required`
(`omarchyPath`, `barWidgetRegistry`, `barConfig`, `shell`, `manifest`).
Omarchy constructs third-party bars first and calls `configureBar()`
after; `required` would fail the load the same way a cloned `omarchy.bar`
does.

`barHidden: true` so notification anchoring does not treat this strip as
the session bar. `barSize` is the row thickness so widgets that size
themselves from `bar.barSize` still lay out. `run()`, `requestPopout`,
and `releasePopout` are implemented for the widget contract.
`summonBarWidget` / `hideBarWidget` / `isBarWidgetOpen` let a dual-kind
plugin (bar-widget + panel) still be summoned onto the row.

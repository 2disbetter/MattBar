-- MattBar media keys for Omarchy 4 ("Quattro").
--
-- Quattro replaced swayosd: the overlay is now drawn by the Quickshell
-- shell, but only when a media script explicitly calls `omarchy-osd`
-- (the OSD panel is IPC-triggered - it does not watch PipeWire or the
-- backlight itself). That makes suppression clean: keep Omarchy's
-- scripts, which carry real logic worth keeping (physical-sink
-- resolution through DSP chains, mute-toggle debounce, DDC and Apple
-- external-display brightness, the hardware mic-mute LED), and just
-- null the one `omarchy-osd` call by putting a no-op shim first in
-- PATH for these bindings only. Brightness doesn't even need the shim:
-- the script grew a native --no-osd flag.
--
-- MattBar's OSD is event-driven (PipeWire events + backlight uevents),
-- so it reacts to the resulting change no matter what caused it.
--
-- One-time setup:
--   mkdir -p ~/.config/mattbar/shims
--   cp shims/omarchy-osd ~/.config/mattbar/shims/
--   chmod +x ~/.config/mattbar/shims/omarchy-osd
--
-- Then paste this whole block into ~/.config/hypr/bindings.lua (it is
-- loaded after the Omarchy defaults) and reload Hyprland. If you are
-- upgrading from the 3.x drop-in, also delete the old
-- `source = ...mattbar-media-keys.conf` line / file - Quattro's config
-- is Lua now and the old hyprlang unbinds no longer apply, which is
-- why the stock overlays came back after the upgrade.

local no_osd = 'env PATH="' .. os.getenv("HOME")
    .. '/.config/mattbar/shims:$PATH" '

-- --- volume ---------------------------------------------------------------
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

-- --- microphone -----------------------------------------------------------
-- The shim keeps the hardware mic-mute LED working (the 3.x drop-in had
-- to trade the LED away; Quattro's script drives it before the OSD call).
hl.unbind("XF86AudioMicMute")
o.bind("XF86AudioMicMute", "Mute microphone",
  no_osd .. "omarchy-audio-input-mute", { locked = true })

-- --- display brightness ---------------------------------------------------
-- Native --no-osd flag; keeps Omarchy's non-uniform stepping and DDC /
-- Apple external-display support. Note: on external DDC monitors there
-- is no kernel backlight device, so MattBar (which watches backlight
-- uevents) shows no OSD for them - the change still applies silently.
hl.unbind("XF86MonBrightnessUp")
hl.unbind("XF86MonBrightnessDown")
hl.unbind("SHIFT + XF86MonBrightnessUp")
hl.unbind("SHIFT + XF86MonBrightnessDown")
hl.unbind("ALT + XF86MonBrightnessUp")
hl.unbind("ALT + XF86MonBrightnessDown")
o.bind("XF86MonBrightnessUp", "Brightness up",
  "omarchy-brightness-display --no-osd +5%",
  { locked = true, repeating = true })
o.bind("XF86MonBrightnessDown", "Brightness down",
  "omarchy-brightness-display --no-osd 5%-",
  { locked = true, repeating = true })
o.bind("SHIFT + XF86MonBrightnessUp", "Brightness maximum",
  "omarchy-brightness-display --no-osd 100%",
  { locked = true, repeating = true })
o.bind("SHIFT + XF86MonBrightnessDown", "Brightness minimum",
  "omarchy-brightness-display --no-osd 1%",
  { locked = true, repeating = true })
o.bind("ALT + XF86MonBrightnessUp", "Brightness up precise",
  "omarchy-brightness-display --no-osd +1%",
  { locked = true, repeating = true })
o.bind("ALT + XF86MonBrightnessDown", "Brightness down precise",
  "omarchy-brightness-display --no-osd 1%-",
  { locked = true, repeating = true })

-- --- media transport (optional) --------------------------------------------
-- Quattro's transport keys go through `omarchy-shell media ...`, whose
-- track-title popup is summoned *inside* the shell, not via omarchy-osd,
-- so the shim can't intercept it. If you want those popups gone too,
-- uncomment this block to use plain playerctl (MattBar's media module
-- reflects state via MPRIS either way):
--
-- hl.unbind("XF86AudioNext")
-- hl.unbind("XF86AudioPrev")
-- hl.unbind("XF86AudioPlay")
-- hl.unbind("XF86AudioPause")
-- o.bind("XF86AudioNext", "Next track", "playerctl next", { locked = true })
-- o.bind("XF86AudioPrev", "Previous track", "playerctl previous", { locked = true })
-- o.bind("XF86AudioPlay", "Play/Pause", "playerctl play-pause", { locked = true })
-- o.bind("XF86AudioPause", "Play/Pause", "playerctl play-pause", { locked = true })

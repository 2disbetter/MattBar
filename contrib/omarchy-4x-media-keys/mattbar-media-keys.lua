-- MattBar media keys for Omarchy 4 ("Quattro").
--
-- Quattro draws OSD via Quickshell only on explicit `omarchy-osd` call.
-- Suppress with no-op shim first in PATH for these bindings; keep Omarchy
-- scripts (sink resolution, debounce, DDC/Apple brightness, mic LED).
-- Brightness has native --no-osd. MattBar OSD is event-driven.
--
-- Setup: mkdir -p ~/.config/mattbar/shims; cp shims/omarchy-osd there;
-- chmod +x. Paste into ~/.config/hypr/bindings.lua (after defaults),
-- reload. From 3.x: also remove old source=mattbar-media-keys.conf.

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
-- Shim keeps hardware mic-mute LED (Quattro script drives it pre-OSD).
hl.unbind("XF86AudioMicMute")
o.bind("XF86AudioMicMute", "Mute microphone",
  no_osd .. "omarchy-audio-input-mute", { locked = true })

-- --- display brightness ---------------------------------------------------
-- Native --no-osd; keeps Omarchy stepping/DDC/Apple support. External DDC
-- monitors lack kernel backlight so MattBar shows no OSD (change still applies).
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
-- Quattro transport uses `omarchy-shell media ...` (popup inside shell,
-- shim can't intercept). Uncomment for plain playerctl (MattBar uses MPRIS):
--
-- hl.unbind("XF86AudioNext")
-- hl.unbind("XF86AudioPrev")
-- hl.unbind("XF86AudioPlay")
-- hl.unbind("XF86AudioPause")
-- o.bind("XF86AudioNext", "Next track", "playerctl next", { locked = true })
-- o.bind("XF86AudioPrev", "Previous track", "playerctl previous", { locked = true })
-- o.bind("XF86AudioPlay", "Play/Pause", "playerctl play-pause", { locked = true })
-- o.bind("XF86AudioPause", "Play/Pause", "playerctl play-pause", { locked = true })

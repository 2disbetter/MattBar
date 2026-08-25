-- MattBar replacements for Omarchy shortcuts that called omarchy-menu /
-- omarchy-shell (Quickshell IPC). Lua binds do not pick up the runtime
-- PATH shim, so these must be real unbind+bind.
--
-- Loaded from hyprland.lua AFTER Omarchy defaults and BEFORE
-- hypr.bindings, so personal overrides in bindings.lua still win
-- (e.g. SUPER+CTRL+H rebound to hibernate).
--
-- Installer copies this to ~/.config/hypr/mattbar-shell-keys.lua.

local mb = "mattbarctl"

-- --- launcher / menus -----------------------------------------------------
hl.unbind("SUPER + SPACE")
o.bind("SUPER + SPACE", "Omarchy menu", mb .. " shell toggle omarchy.menu")
hl.unbind("SUPER + ALT + SPACE")
o.bind("SUPER + ALT + SPACE", "Apps menu",
  mb .. " shell toggle omarchy.menu '{\"menu\":\"apps\"}'")
hl.unbind("SUPER + SHIFT + code:201")
o.bind("SUPER + SHIFT + code:201", "Omarchy menu",
  mb .. " shell toggle omarchy.menu '{\"menu\":\"root\"}'")
hl.unbind("SUPER + ESCAPE")
o.bind("SUPER + ESCAPE", "System menu",
  mb .. " shell toggle omarchy.menu '{\"menu\":\"system\"}'")
hl.unbind("XF86PowerOff")
o.bind("XF86PowerOff", "Power menu",
  mb .. " shell toggle omarchy.menu '{\"menu\":\"system\"}'", { locked = true })
hl.unbind("SUPER + CTRL + C")
o.bind("SUPER + CTRL + C", "Capture menu",
  mb .. " shell toggle omarchy.menu '{\"menu\":\"capture\"}'")
hl.unbind("SUPER + CTRL + O")
o.bind("SUPER + CTRL + O", "Toggle menu",
  mb .. " shell toggle omarchy.menu '{\"menu\":\"toggle\"}'")
hl.unbind("SUPER + CTRL + SPACE")
o.bind("SUPER + CTRL + SPACE", "Background switcher",
  mb .. " shell toggle omarchy.menu '{\"menu\":\"background\"}'")
hl.unbind("SUPER + SHIFT + CTRL + SPACE")
o.bind("SUPER + SHIFT + CTRL + SPACE", "Theme menu",
  mb .. " shell toggle omarchy.menu '{\"menu\":\"theme\"}'")
hl.unbind("SUPER + CTRL + R")
o.bind("SUPER + CTRL + R", "Set reminder",
  mb .. " shell toggle omarchy.menu '{\"menu\":\"reminder-set\"}'")
-- SUPER + CTRL + H (hardware) and SUPER + CTRL + S (share) are often
-- rebound by the user; they stay Omarchy defaults unless you add them
-- here yourself, so personal suspend/hibernate binds keep working.

-- --- panels ---------------------------------------------------------------
hl.unbind("SUPER + CTRL + A")
o.bind("SUPER + CTRL + A", "Audio", mb .. " shell toggle omarchy.audio")
hl.unbind("SUPER + CTRL + B")
o.bind("SUPER + CTRL + B", "Bluetooth", mb .. " shell toggle omarchy.bluetooth")
hl.unbind("SUPER + CTRL + W")
o.bind("SUPER + CTRL + W", "Network", mb .. " shell toggle omarchy.network")
hl.unbind("SUPER + CTRL + L")
o.bind("SUPER + CTRL + L", "Lock system", mb .. " lock lock")
hl.unbind("SUPER + CTRL + D")
o.bind("SUPER + CTRL + D", "Display", mb .. " shell toggle omarchy.monitor")
hl.unbind("SUPER + CTRL + P")
o.bind("SUPER + CTRL + P", "Power", mb .. " shell toggle omarchy.power")
hl.unbind("SUPER + CTRL + ALT + D")
o.bind("SUPER + CTRL + ALT + D", "Calendar", mb .. " shell toggle omarchy.clock")
hl.unbind("SUPER + CTRL + E")
o.bind("SUPER + CTRL + E", "Emojis", mb .. " shell toggle omarchy.emojis")
hl.unbind("SUPER + CTRL + V")
o.bind("SUPER + CTRL + V", "Clipboard manager",
  mb .. " shell toggle omarchy.clipboard")

-- --- notifications --------------------------------------------------------
hl.unbind("SUPER + comma")
hl.unbind("SUPER + SHIFT + comma")
hl.unbind("SUPER + CTRL + comma")
hl.unbind("SUPER + ALT + comma")
hl.unbind("SUPER + SHIFT + ALT + comma")
o.bind("SUPER + comma", "Dismiss last notification",
  mb .. " notifications dismissOne")
o.bind("SUPER + SHIFT + comma", "Dismiss all notifications",
  mb .. " notifications dismissAll")
o.bind("SUPER + CTRL + comma", "Toggle do-not-disturb",
  mb .. " notifications toggleDnd")
o.bind("SUPER + ALT + comma", "Invoke last notification",
  mb .. " notifications invokeLast")
o.bind("SUPER + SHIFT + ALT + comma", "Open notification history",
  mb .. " notifications showHistory")

-- --- media transport ------------------------------------------------------
hl.unbind("XF86AudioNext")
hl.unbind("XF86AudioPrev")
hl.unbind("XF86AudioPlay")
hl.unbind("XF86AudioPause")
hl.unbind("ALT + XF86AudioPlay")
hl.unbind("ALT + SHIFT + XF86AudioPlay")
o.bind("XF86AudioNext", "Next track", mb .. " media next", { locked = true })
o.bind("XF86AudioPrev", "Previous track", mb .. " media previous", { locked = true })
o.bind("XF86AudioPlay", "Play", mb .. " media playPause", { locked = true })
o.bind("XF86AudioPause", "Pause", mb .. " media playPause", { locked = true })
o.bind("ALT + XF86AudioPlay", "Next track", mb .. " media next", { locked = true })
o.bind("ALT + SHIFT + XF86AudioPlay", "Previous track",
  mb .. " media previous", { locked = true })

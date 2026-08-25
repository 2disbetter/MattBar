#pragma once
// ---------------------------------------------------------------------------
// Reads the active Omarchy theme and maps its palette onto MattBar's colors,
// the way Omarchy themes Waybar. Understands both theme generations:
//   - colors.toml  (Omarchy >= ~3.8 and quattro/4.x) — rich semantic palette
//   - waybar.css   (earlier 3.x) — "@define-color foreground/background" only
// The active theme is found at ~/.local/state/omarchy/current/theme (4.x)
// or ~/.config/omarchy/current/theme (3.x), whichever exists.
// ---------------------------------------------------------------------------
#include <string>

struct Config;

// Overlays the current Omarchy theme onto c's EFFECTIVE colors (c_*).
// The user's own palette (u_*) is never touched. The alpha channels of the
// user's background and hot-strip colors are preserved, so translucency
// preferences survive theme following. Returns false if no theme was found
// (colors are then left as they were). Updates omarchy_theme_status().
bool omarchy_theme_apply(Config& c);

// Directory to watch (inotify) for theme swaps: ".../omarchy/current".
// Empty if no Omarchy installation was found.
std::string omarchy_theme_watch_dir();

// Human-readable status for the settings UI: the active theme's name
// ("tokyo-night"), or an explanation ("no Omarchy theme found").
const std::string& omarchy_theme_status();

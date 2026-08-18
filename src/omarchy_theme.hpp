#pragma once
// Map active Omarchy theme palette onto MattBar colors (like Omarchy does for Waybar).
#include <string>

struct Config;

// Overlay current Omarchy theme onto c's EFFECTIVE colors (c_*).
bool omarchy_theme_apply(Config& c);

// Directory to watch (inotify) for theme swaps. Empty if no Omarchy.
std::string omarchy_theme_watch_dir();

// Status for settings UI: theme name or "no Omarchy theme found".
const std::string& omarchy_theme_status();

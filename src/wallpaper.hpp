#pragma once
// Desktop background, mapped only while cfg.quickshell_shutdown is on
// (otherwise Omarchy's Quickshell background stays in charge).
#include <cairo/cairo.h>
#include <functional>
#include <string>
#include <vector>

class Bar;

void wallpaper_init(Bar&);
void wallpaper_apply();   // create/destroy per-output surfaces to match cfg
void wallpaper_refresh(); // reread ~/.local/state/omarchy/current/background
// Display `path` (and point the Omarchy current-background link at it).
// `instant` skips the wipe, matching Quickshell's setInstant.
void wallpaper_set(const std::string& path, bool instant);
// Crossfade from `from_path` (or the currently shown image) to `path`.
void wallpaper_transition(const std::string& from_path,
                          const std::string& path);
// Small copy (longest edge <= 768 px) of the current background, for the
// lock blur; nullptr if none. Owned by wallpaper.
cairo_surface_t* wallpaper_image();
// Decode a local image file (JPEG/PNG/WebP/…) into a new ARGB32 surface.
// Caller owns the result and must cairo_surface_destroy it. nullptr on fail.
cairo_surface_t* image_load_file(const std::string& path);
// One line per output: sizes, image, frames drawn (ctl render-stats).
std::string wallpaper_debug_state();
// The background MattBar shows on the main monitor (resolved path).
std::string wallpaper_current_path();
// Settings: show `path` and point Omarchy's current-background link at it.
void wallpaper_choose(const std::string& path);
// Point the link only (the background IPC).
void wallpaper_point_link(const std::string& path);
// Images offered by the settings picker: the current background, the
// theme's backgrounds, ~/.config/omarchy/backgrounds/<theme>,
// ~/Pictures/Wallpapers and ~/Wallpapers.
std::vector<std::string> wallpaper_candidates();
// Called after decoded images land (settings preview). Empty = none.
void wallpaper_set_listener(std::function<void()> fn);

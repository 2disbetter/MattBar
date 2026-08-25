#pragma once
// Desktop background, mapped only while cfg.quickshell_shutdown is on
// (otherwise Omarchy's Quickshell background stays in charge).
#include <cairo/cairo.h>
#include <string>

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
// Cached ARGB32 image, or nullptr if none loaded. Owned by wallpaper.
cairo_surface_t* wallpaper_image();
// Decode a local image file (JPEG/PNG/WebP/…) into a new ARGB32 surface.
// Caller owns the result and must cairo_surface_destroy it. nullptr on fail.
cairo_surface_t* image_load_file(const std::string& path);

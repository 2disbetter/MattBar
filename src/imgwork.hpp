#pragma once
// Background image decoding: one worker thread, results delivered on the
// main loop through an eventfd. Nothing that decodes a file of unknown
// size (wallpapers, picker thumbnails, launcher icons, album art) should
// run stbi_load on the main loop — a 5K JPEG is ~150 ms of frozen bar.
//
// A job is a list of files; each file is decoded ONCE and scaled to any
// number of targets, all in the worker. The done callback runs on the
// main loop; it takes the surfaces it keeps by moving them out of the
// result (std::exchange(slot, nullptr)); whatever is left is destroyed.
#include <cairo/cairo.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include <utility>

class Bar;

struct ImgTarget {
    enum Mode {
        Cover, // exactly w x h, scaled to cover and centred (wallpaper)
        Fit,   // longest edge scaled down to fit w x h, aspect kept, never
               // upscaled (thumbnails, icons, album art)
    };
    int  w = 0, h = 0; // Fit: 0 = no limit on that axis
    Mode mode = Fit;
};
// Cover results from an opaque source are CAIRO_FORMAT_RGB24 (blit, no
// blend); everything else is premultiplied ARGB32.

struct ImgItem {
    std::string            path;
    std::vector<ImgTarget> targets;
};

struct ImgResult {
    // surfs[item][target]; nullptr where decoding failed.
    std::vector<std::vector<cairo_surface_t*>> surfs;
    // Source dimensions per item (0 x 0 on failure).
    std::vector<std::pair<int, int>> src_size;
};

using ImgDone = std::function<void(ImgResult&)>;

// Queue a job. Returns a ticket (never 0) for img_cancel.
uint64_t img_submit(Bar& bar, std::vector<ImgItem> items, ImgDone done);
// Forget a job: removed if still queued; its result is discarded if the
// worker already has it. Safe with 0 or a finished ticket.
void img_cancel(uint64_t ticket);
// Join the worker (Bar::shutdown). Pending jobs are dropped.
void img_shutdown();

// Synchronous decode with the same scaling (tests, tiny files).
cairo_surface_t* img_decode_now(const std::string& path, ImgTarget t);

// Debug counters.
uint64_t img_decodes_total();

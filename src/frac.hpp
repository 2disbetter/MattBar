#pragma once
// ---------------------------------------------------------------------------
// Fractional scaling (wp-fractional-scale-v1 + wp_viewporter), one helper
// shared by every render site (bar surfaces, popups, settings, tray menus).
//
// The contract mirrors the integer-scale design: ALL layout, hit rects, and
// pointer math stay in logical pixels; only the buffer knows the scale. At
// 1.5x a 30-logical-px bar renders into a 45px buffer and the viewport
// declares "this covers 30 logical px" — the compositor composites 1:1
// instead of downsampling a 2x buffer, which is exactly the crispness
// difference on a bar full of 11px glyphs.
//
// Absent protocol (older compositors) => active() is false everywhere and
// every site falls back to the existing integer set_buffer_scale path,
// pixel-identical to v1.23.
// ---------------------------------------------------------------------------
#include <cstdint>
#include <functional>

#include "fractional-scale-v1-client-protocol.h"
#include "viewporter-client-protocol.h"

struct FracSurface {
    wp_fractional_scale_v1* obj      = nullptr;
    wp_viewport*            vp       = nullptr;
    uint32_t                scale120 = 0; // compositor-preferred, x120
    std::function<void()>   on_change;    // e.g. mark surface dirty

    void attach(wp_fractional_scale_manager_v1* mgr, wp_viewporter* vpr,
                wl_surface* s) {
        if (!mgr || !vpr || obj) return;
        obj = wp_fractional_scale_manager_v1_get_fractional_scale(mgr, s);
        vp  = wp_viewporter_get_viewport(vpr, s);
        static const wp_fractional_scale_v1_listener lst = {
            .preferred_scale = [](void* d, wp_fractional_scale_v1*,
                                  uint32_t sc) {
                auto* self = static_cast<FracSurface*>(d);
                if (sc == self->scale120) return;
                self->scale120 = sc;
                if (self->on_change) self->on_change();
            }};
        wp_fractional_scale_v1_add_listener(obj, &lst, this);
    }
    void destroy() {
        if (obj) wp_fractional_scale_v1_destroy(obj);
        if (vp) wp_viewport_destroy(vp);
        obj      = nullptr;
        vp       = nullptr;
        scale120 = 0;
    }

    // A preferred scale that is a whole multiple of 120 gains nothing over
    // the integer path — use it only when genuinely fractional.
    bool active() const { return vp && scale120 > 0 && scale120 % 120 != 0; }

    // Buffer pixels for a logical size (spec rounding: round half up).
    int px(int logical) const { return (int)((logical * scale120 + 60) / 120); }

    // Attach-side surface state. Integer path: classic set_buffer_scale.
    // Fractional: buffer scale 1 + viewport destination in logical px.
    void apply(wl_surface* s, int logical_w, int logical_h,
               int int_scale) const {
        if (active()) {
            if (wl_surface_get_version(s) >= 3)
                wl_surface_set_buffer_scale(s, 1);
            wp_viewport_set_destination(vp, logical_w, logical_h);
        } else {
            // Unset a leftover destination so the integer path is not
            // fighting a viewport from a previous fractional scale.
            if (vp) wp_viewport_set_destination(vp, -1, -1);
            if (wl_surface_get_version(s) >= 3)
                wl_surface_set_buffer_scale(s, int_scale);
        }
    }
};

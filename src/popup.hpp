#pragma once
#include "bar.hpp"
#include "config.hpp"
#include "shm.hpp"
#include <cairo/cairo.h>
#include <algorithm>
#include <functional>

// ---------------------------------------------------------------------
// Small overlay window on a wlr layer surface: used by the notification
// daemon (notes, OSD) and by modules that need a popup (brightness
// slider). Headless-safe: ensure() is a no-op without a compositor.
// ---------------------------------------------------------------------
// Placement for a popup hanging off the bar: it opens away from whichever
// edge the bar is docked to, horizontally (or vertically) aligned to the
// module that spawned it, and clamped to stay on screen.
struct PopupPlace {
    uint32_t anchor;
    int mt = 0, mr = 0, mb = 0, ml = 0;
};
// `avail` is the bar's along-axis length (screen width for a horizontal
// bar). 0 = unknown: only the near edge is clamped, as before.
inline PopupPlace popup_place(double along, int size_along,
                              double avail = 0) {
    PopupPlace p{};
    int off = static_cast<int>(along + 0.5) - size_along / 2;
    // Keep the whole panel on screen: clamp the far edge first, then the
    // near one — so an over-wide panel loses its far side, never its
    // controls at the near edge.
    if (avail > 0) off = std::min(off, static_cast<int>(avail) - size_along - 8);
    off = std::max(8, off);
    int gap = cfg_thickness() + 6;
    if (!cfg_vertical()) {
        bool top = cfg.position == "top";
        p.anchor = (top ? ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP
                        : ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM) |
                   ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT;
        (top ? p.mt : p.mb) = gap;
        p.ml                = off;
    } else {
        bool left = cfg.position == "left";
        p.anchor  = (left ? ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT
                          : ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT) |
                   ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP;
        (left ? p.ml : p.mr) = gap;
        p.mt                 = off;
    }
    return p;
}

// Hang a shell panel off the bar the way Omarchy's Quickshell TUIs do:
// below a top bar (above a bottom bar), pinned to the right; beside a
// vertical bar, pinned to the top. Super+Ctrl+A/B/W/D land here too.
inline PopupPlace popup_place_bar_end() {
    PopupPlace p{};
    int gap   = cfg_thickness() + 8;
    int inset = 12;
    if (!cfg_vertical()) {
        bool top = cfg.position == "top";
        p.anchor = (top ? ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP
                        : ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM) |
                   ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
        (top ? p.mt : p.mb) = gap;
        p.mr                = inset;
    } else {
        bool left = cfg.position == "left";
        p.anchor  = (left ? ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT
                          : ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT) |
                    ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP;
        (left ? p.ml : p.mr) = gap;
        p.mt                 = inset;
    }
    return p;
}

struct PopupWin {
    Bar*                          bar = nullptr;
    wl_surface*                   surf = nullptr;
    zwlr_layer_surface_v1*        ls   = nullptr;
    FracSurface                   frac; // fractional scaling (see frac.hpp)
    int                           w = 0, h = 0;
    bool                          configured = false;
    double                        mx = -1, my = -1;
    std::function<void(cairo_t*)> paint;
    std::function<void(double, double, int)> click;
    std::function<void(double, double)> pmotion; // live drag tracking
    std::function<void(int)>      prelease; // for drag interactions
    std::function<void(int)>      pscroll;
    std::function<void()>         pleave;
    std::function<void(const Bar::KeyEvent&)> pkey;
    // 0 = none (default, same as before). 1 = exclusive. 2 = on_demand
    // (falls back to exclusive if layer-shell < v4).
    uint32_t kb_mode = 0;
    // No anchors: compositor centres the surface (command menu, lock
    // preview, etc.).
    bool centered = false;
    std::function<void()> on_configured;

    // Default input region is infinite. Clip to the buffer so protocol-
    // correct compositors do not send us hits past the card. Hyprland's
    // exclusive-keyboard grab still delivers out-of-geometry clicks; the
    // Host click wrapper turns those into dismiss.
    void clip_input_to_buffer() {
        if (!surf || !bar || !bar->compositor() || w <= 0 || h <= 0) return;
        wl_region* r = wl_compositor_create_region(bar->compositor());
        if (!r) return;
        wl_region_add(r, 0, 0, w, h);
        wl_surface_set_input_region(surf, r);
        wl_region_destroy(r);
    }

    static void on_configure(void* d, zwlr_layer_surface_v1* ls,
                             uint32_t serial, uint32_t width, uint32_t height) {
        auto* p = static_cast<PopupWin*>(d);
        zwlr_layer_surface_v1_ack_configure(ls, serial);
        if (width > 0 && height > 0) {
            p->w = (int)width;
            p->h = (int)height;
        }
        p->configured = true;
        p->clip_input_to_buffer();
        if (p->on_configured) p->on_configured();
        p->draw();
    }
    static void on_closed(void* d, zwlr_layer_surface_v1*) {
        static_cast<PopupWin*>(d)->destroy();
    }

    // `out` pins the popup to a specific monitor (nullptr = compositor
    // picks). Module popups pass the output of the bar that was clicked, so
    // a calendar opens on the monitor whose clock you clicked; the
    // notification daemon passes the primary output.
    wl_output* out = nullptr;

    void ensure(Bar& b, uint32_t anchor, int mt, int mr, int mb, int ml,
                const char* ns, int nw, int nh, wl_output* on_output = nullptr) {
        bar = &b;
        if (!bar->compositor() || !bar->layer_shell()) return; // headless test
        w = nw;
        h = nh;
        // Reopening on a different monitor needs a fresh surface: an output
        // can only be chosen at layer-surface creation.
        if (surf && on_output != out) destroy();
        out = on_output;
        if (surf) {
            zwlr_layer_surface_v1_set_size(ls, w, h);
            if (!centered) {
                zwlr_layer_surface_v1_set_anchor(ls, anchor);
                zwlr_layer_surface_v1_set_margin(ls, mt, mr, mb, ml);
            }
            uint32_t kb = kb_mode;
            if (kb == 2) kb = bar->popup_kb_mode();
            zwlr_layer_surface_v1_set_keyboard_interactivity(ls, kb);
            clip_input_to_buffer();
            wl_surface_commit(surf);
            return;
        }
        surf = wl_compositor_create_surface(bar->compositor());
        ls   = zwlr_layer_shell_v1_get_layer_surface(
            bar->layer_shell(), surf, out,
            ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, ns);
        static const zwlr_layer_surface_v1_listener lst = {
            .configure = on_configure,
            .closed    = on_closed,
        };
        zwlr_layer_surface_v1_add_listener(ls, &lst, this);
        frac.on_change = [this] { if (configured) draw(); };
        frac.attach(bar->frac_mgr(), bar->viewporter(), surf);
        if (!centered) {
            zwlr_layer_surface_v1_set_anchor(ls, anchor);
            zwlr_layer_surface_v1_set_margin(ls, mt, mr, mb, ml);
        }
        zwlr_layer_surface_v1_set_size(ls, w, h);
        zwlr_layer_surface_v1_set_exclusive_zone(ls, 0);
        uint32_t kb = kb_mode;
        if (kb == 2) kb = bar->popup_kb_mode();
        zwlr_layer_surface_v1_set_keyboard_interactivity(ls, kb);
        clip_input_to_buffer();
        configured = false;
        bar->register_surface(
            surf, Bar::SurfaceHooks{
                      .motion =
                          [this](double x, double y) {
                              mx = x;
                              my = y;
                              if (pmotion) pmotion(x, y);
                          },
                      .button =
                          [this](int btn) {
                              if (click) click(mx, my, btn);
                          },
                      .leave =
                          [this] {
                              mx = my = -1;
                              if (pleave) pleave();
                          },
                      .scroll =
                          [this](int d) {
                              if (pscroll) pscroll(d);
                          },
                      .release =
                          [this](int btn) {
                              if (prelease) prelease(btn);
                          },
                      .key =
                          [this](const Bar::KeyEvent& e) {
                              if (pkey) pkey(e);
                          },
                  });
        wl_surface_commit(surf);
    }

    void draw() {
        if (!surf || !configured || w <= 0 || h <= 0) return;
        // HiDPI: buffer at the target output's scale, drawing code stays in
        // logical coordinates (same scheme as BarSurface::draw). For a
        // popup without a pinned output the primary's scale is the best
        // guess — every popup MattBar opens is pinned these days anyway.
        int sc = 1;
        if (wl_surface_get_version(surf) >= 3)
            sc = bar->scale_of(out ? out : bar->primary_output());
        const int bw = frac.active() ? frac.px(w) : w * sc;
        const int bh = frac.active() ? frac.px(h) : h * sc;
        void*      data   = nullptr;
        wl_buffer* buffer = create_argb_buffer(bar->shm(), bw, bh, &data);
        if (!buffer) return;
        cairo_surface_t* cs = cairo_image_surface_create_for_data(
            static_cast<unsigned char*>(data), CAIRO_FORMAT_ARGB32, bw, bh,
            bw * 4);
        cairo_t* cr = cairo_create(cs);
        cairo_scale(cr, (double)bw / w, (double)bh / h);
        cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
        cairo_set_source_rgba(cr, 0, 0, 0, 0);
        cairo_paint(cr);
        cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
        cairo_select_font_face(cr, cfg.font.c_str(), CAIRO_FONT_SLANT_NORMAL,
                               CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, cfg.font_size);
        if (paint) paint(cr);
        cairo_destroy(cr);
        cairo_surface_destroy(cs);
        frac.apply(surf, w, h, sc);
        wl_surface_attach(surf, buffer, 0, 0);
        if (wl_surface_get_version(surf) >= 4)
            wl_surface_damage_buffer(surf, 0, 0, bw, bh);
        else
            wl_surface_damage(surf, 0, 0, w, h);
        wl_surface_commit(surf);
    }

    void destroy() {
        if (!surf) return;
        bar->unregister_surface(surf);
        if (ls) zwlr_layer_surface_v1_destroy(ls);
        frac.destroy();
        wl_surface_destroy(surf);
        surf = nullptr;
        ls   = nullptr;
        configured = false;
    }
};

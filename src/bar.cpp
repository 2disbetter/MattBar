#include "bar.hpp"
#include <systemd/sd-daemon.h>
#include "cursor-shape-v1-client-protocol.h"
#include "ext-idle-notify-v1-client-protocol.h"
#include "ext-session-lock-v1-client-protocol.h"
#include "idle-inhibit-unstable-v1-client-protocol.h"
#include "config.hpp"
#include "omarchy_theme.hpp"
#include "notify.hpp"
#include "idle.hpp"
#include "lock.hpp"
#include "qs_plugins.hpp"
#include "settings.hpp"
#include "shell.hpp"
#include "shm.hpp"
#include "wallpaper.hpp"
#include "util.hpp"

#include <cairo/cairo.h>
#include <linux/input-event-codes.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>

#include <cstdlib>
static bool dbg() {
    static bool v = getenv("MATTBAR_DEBUG") != nullptr;
    return v;
}
#define DBG(...) do { if (dbg()) { fprintf(stderr, "mattbar: " __VA_ARGS__); fputc('\n', stderr); } } while (0)

namespace {
void set_color(cairo_t* cr, const Color& c) {
    cairo_set_source_rgba(cr, c.r, c.g, c.b, c.a);
}
} // namespace

// ---------------------------------------------------------------------------
// Registry
// ---------------------------------------------------------------------------
void Bar::on_global(void* data, wl_registry* reg, uint32_t name,
                    const char* iface, uint32_t version) {
    auto* self = static_cast<Bar*>(data);
    if (!strcmp(iface, wl_compositor_interface.name)) {
        self->compositor_ = static_cast<wl_compositor*>(
            wl_registry_bind(reg, name, &wl_compositor_interface,
                             version < 4 ? version : 4));
    } else if (!strcmp(iface, wl_shm_interface.name)) {
        self->shm_ = static_cast<wl_shm*>(
            wl_registry_bind(reg, name, &wl_shm_interface, 1));
    } else if (!strcmp(iface, zwlr_layer_shell_v1_interface.name)) {
        uint32_t ver = version < 4 ? version : 4;
        self->layer_shell_ver_ = ver;
        self->layer_shell_ = static_cast<zwlr_layer_shell_v1*>(
            wl_registry_bind(reg, name, &zwlr_layer_shell_v1_interface, ver));
    } else if (!strcmp(iface, ext_idle_notifier_v1_interface.name)) {
        uint32_t ver = version < 1 ? version : 1;
        self->idle_notif_ = static_cast<ext_idle_notifier_v1*>(
            wl_registry_bind(reg, name, &ext_idle_notifier_v1_interface, ver));
    } else if (!strcmp(iface, ext_session_lock_manager_v1_interface.name)) {
        self->lock_mgr_ = static_cast<ext_session_lock_manager_v1*>(
            wl_registry_bind(reg, name, &ext_session_lock_manager_v1_interface,
                             1));
    } else if (!strcmp(iface, zwp_idle_inhibit_manager_v1_interface.name)) {
        self->idle_mgr_ = static_cast<zwp_idle_inhibit_manager_v1*>(
            wl_registry_bind(reg, name, &zwp_idle_inhibit_manager_v1_interface,
                             1));
    } else if (!strcmp(iface, xdg_wm_base_interface.name)) {
        self->wm_base_ = static_cast<xdg_wm_base*>(
            wl_registry_bind(reg, name, &xdg_wm_base_interface, 1));
        static const xdg_wm_base_listener wm_listener = {
            .ping = [](void*, xdg_wm_base* wm, uint32_t serial) {
                xdg_wm_base_pong(wm, serial);
            },
        };
        xdg_wm_base_add_listener(self->wm_base_, &wm_listener, self);
    } else if (!strcmp(iface, wp_viewporter_interface.name)) {
        self->viewporter_ = static_cast<wp_viewporter*>(
            wl_registry_bind(reg, name, &wp_viewporter_interface, 1));
    } else if (!strcmp(iface, wp_fractional_scale_manager_v1_interface.name)) {
        self->frac_mgr_ = static_cast<wp_fractional_scale_manager_v1*>(
            wl_registry_bind(reg, name,
                             &wp_fractional_scale_manager_v1_interface, 1));
    } else if (!strcmp(iface, wp_cursor_shape_manager_v1_interface.name)) {
        self->cursor_shape_mgr_ =
            static_cast<wp_cursor_shape_manager_v1*>(wl_registry_bind(
                reg, name, &wp_cursor_shape_manager_v1_interface, 1));
    } else if (!strcmp(iface, wl_output_interface.name)) {
        auto* o = new BarOutput{};
        o->reg = name;
        o->bar = self;
        o->wl = static_cast<wl_output*>(wl_registry_bind(
            reg, name, &wl_output_interface, version < 4 ? version : 4));
        static const wl_output_listener out_listener = {
            .geometry = [](void*, wl_output*, int32_t, int32_t, int32_t,
                           int32_t, int32_t, const char*, const char*,
                           int32_t) {},
            .mode = [](void* data, wl_output*, uint32_t flags, int32_t w,
                       int32_t h, int32_t) {
                if (flags & WL_OUTPUT_MODE_CURRENT) {
                    auto* bo = static_cast<BarOutput*>(data);
                    bo->px_w = w;
                    bo->px_h = h;
                }
            },
            // done arrives after name, and again on every hotplug/mode
            // change: the cue to reconcile our surfaces with the outputs.
            .done = [](void* data, wl_output*) {
                auto* bo = static_cast<BarOutput*>(data);
                if (bo->bar) bo->bar->outputs_dirty_ = true;
            },
            .scale = [](void* data, wl_output*, int32_t f) {
                auto* bo = static_cast<BarOutput*>(data);
                if (f >= 1 && f != bo->scale) {
                    bo->scale = f;
                    // .done follows and flags outputs_dirty_, where the
                    // affected surfaces get their redraw at the new scale.
                }
            },
            .name = [](void* data, wl_output*, const char* n) {
                static_cast<BarOutput*>(data)->name = n ? n : "";
            },
            .description = [](void*, wl_output*, const char*) {},
        };
        wl_output_add_listener(o->wl, &out_listener, o);
        self->outputs_.push_back(o);
    } else if (!strcmp(iface, wl_seat_interface.name)) {
        self->seat_ = static_cast<wl_seat*>(
            wl_registry_bind(reg, name, &wl_seat_interface,
                             version < 5 ? version : 5));
        static const wl_seat_listener seat_listener = {
            .capabilities = on_seat_caps,
            .name = [](void*, wl_seat*, const char*) {},
        };
        wl_seat_add_listener(self->seat_, &seat_listener, self);
    }
}

// A monitor was unplugged (or the compositor dropped the global). Tear down
// the bar that lived on it before releasing the wl_output it references.
void Bar::on_global_remove(void* data, wl_registry*, uint32_t name) {
    auto* self = static_cast<Bar*>(data);
    for (auto it = self->outputs_.begin(); it != self->outputs_.end(); ++it) {
        if ((*it)->reg != name) continue;
        BarOutput* o = *it;
        DBG("output %s removed", o->name.c_str());
        for (auto sit = self->surfaces_.begin();
             sit != self->surfaces_.end();) {
            if ((*sit)->out == o->wl) {
                if (self->cur_ == *sit) self->cur_ = nullptr;
                if (self->primary_ == *sit) self->primary_ = nullptr;
                (*sit)->destroy();
                delete *sit;
                sit = self->surfaces_.erase(sit);
            } else {
                ++sit;
            }
        }
        if (o->wl) wl_output_destroy(o->wl);
        delete o;
        self->outputs_.erase(it);
        self->outputs_dirty_ = true; // re-elect a primary if we lost it
        return;
    }
}

// ---------------------------------------------------------------------------
// Seat / pointer
// ---------------------------------------------------------------------------
void Bar::on_seat_caps(void* data, wl_seat* seat, uint32_t caps) {
    auto* self = static_cast<Bar*>(data);
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !self->pointer_) {
        self->pointer_ = wl_seat_get_pointer(seat);
        static const wl_pointer_listener ptr_listener = {
            .enter  = on_ptr_enter,
            .leave  = on_ptr_leave,
            .motion = on_ptr_motion,
            .button = on_ptr_button,
            .axis   = on_ptr_axis,
            .frame        = [](void*, wl_pointer*) {},
            .axis_source  = [](void*, wl_pointer*, uint32_t) {},
            .axis_stop    = [](void*, wl_pointer*, uint32_t, uint32_t) {},
            .axis_discrete = [](void*, wl_pointer*, uint32_t, int32_t) {},
            .axis_value120 = [](void*, wl_pointer*, uint32_t, int32_t) {},
            .axis_relative_direction =
                [](void*, wl_pointer*, uint32_t, uint32_t) {},
        };
        wl_pointer_add_listener(self->pointer_, &ptr_listener, self);
    } else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && self->pointer_) {
        wl_pointer_destroy(self->pointer_);
        self->pointer_ = nullptr;
        // no pointer -> no leave event will ever arrive; don't let the bars
        // stay revealed forever on stale state
        self->ptr_surface_ = nullptr;
        self->cur_         = nullptr;
        for (auto* bs : self->surfaces_) {
            bs->ptr_inside = false;
            bs->arm_reveal(false);
            if (bs->expanded) bs->arm_hide(true);
        }
        DBG("seat lost pointer capability");
    }
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !self->keyboard_) {
        if (!self->xkb_ctx_)
            self->xkb_ctx_ = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
        self->keyboard_ = wl_seat_get_keyboard(seat);
        static const wl_keyboard_listener kb_listener = {
            .keymap      = on_kb_keymap,
            .enter       = on_kb_enter,
            .leave       = on_kb_leave,
            .key         = on_kb_key,
            .modifiers   = on_kb_mods,
            .repeat_info = on_kb_repeat,
        };
        wl_keyboard_add_listener(self->keyboard_, &kb_listener, self);
        if (self->kb_repeat_fd_ < 0) {
            self->kb_repeat_fd_ =
                timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
            if (self->kb_repeat_fd_ >= 0)
                self->add_fd(
                    self->kb_repeat_fd_,
                    [self](uint32_t) {
                        uint64_t n;
                        while (read(self->kb_repeat_fd_, &n, sizeof n) > 0) {
                        }
                        if (self->kb_repeat_code_)
                            self->deliver_key(self->kb_repeat_ev_);
                    },
                    "key-repeat");
        }
    } else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && self->keyboard_) {
        self->arm_key_repeat(false);
        wl_keyboard_destroy(self->keyboard_);
        self->keyboard_  = nullptr;
        self->kb_surface_ = nullptr;
    }
}

uint32_t Bar::popup_kb_mode() const {
    return layer_shell_ver_ >= 4
               ? ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_ON_DEMAND
               : ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE;
}

void Bar::on_kb_keymap(void* data, wl_keyboard*, uint32_t format, int32_t fd,
                       uint32_t size) {
    auto* self = static_cast<Bar*>(data);
    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 || fd < 0 ||
        !self->xkb_ctx_) {
        if (fd >= 0) close(fd);
        return;
    }
    char* map = static_cast<char*>(
        mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0));
    close(fd);
    if (map == MAP_FAILED) return;
    xkb_keymap* km = xkb_keymap_new_from_string(
        self->xkb_ctx_, map, XKB_KEYMAP_FORMAT_TEXT_V1,
        XKB_KEYMAP_COMPILE_NO_FLAGS);
    munmap(map, size);
    if (!km) return;
    xkb_state* st = xkb_state_new(km);
    if (self->xkb_state_) xkb_state_unref(self->xkb_state_);
    if (self->xkb_keymap_) xkb_keymap_unref(self->xkb_keymap_);
    self->xkb_keymap_ = km;
    self->xkb_state_  = st;
}

void Bar::on_kb_enter(void* data, wl_keyboard*, uint32_t, wl_surface* surf,
                      wl_array*) {
    static_cast<Bar*>(data)->kb_surface_ = surf;
}

void Bar::on_kb_leave(void* data, wl_keyboard*, uint32_t, wl_surface* surf) {
    auto* self = static_cast<Bar*>(data);
    if (self->kb_surface_ == surf) self->kb_surface_ = nullptr;
    self->arm_key_repeat(false);
}

void Bar::on_kb_key(void* data, wl_keyboard*, uint32_t, uint32_t, uint32_t key,
                    uint32_t state) {
    auto* self = static_cast<Bar*>(data);
    if (!self->xkb_state_) return;
    const xkb_keycode_t code = key + 8;
    const bool pressed = state == WL_KEYBOARD_KEY_STATE_PRESSED;
    xkb_state_update_key(self->xkb_state_, code,
                         pressed ? XKB_KEY_DOWN : XKB_KEY_UP);
    KeyEvent ev;
    ev.keycode = code;
    ev.keysym  = xkb_state_key_get_one_sym(self->xkb_state_, code);
    ev.pressed = pressed;
    char utf[16]{};
    int  n = xkb_state_key_get_utf8(self->xkb_state_, code, utf, sizeof utf);
    if (n > 0 && utf[0] >= 32) ev.utf8.assign(utf, n);
    ev.mods = 0;
    if (xkb_state_mod_name_is_active(self->xkb_state_, XKB_MOD_NAME_SHIFT,
                                     XKB_STATE_MODS_EFFECTIVE) > 0)
        ev.mods |= KEY_MOD_SHIFT;
    if (xkb_state_mod_name_is_active(self->xkb_state_, XKB_MOD_NAME_CTRL,
                                     XKB_STATE_MODS_EFFECTIVE) > 0)
        ev.mods |= KEY_MOD_CTRL;
    if (xkb_state_mod_name_is_active(self->xkb_state_, XKB_MOD_NAME_ALT,
                                     XKB_STATE_MODS_EFFECTIVE) > 0)
        ev.mods |= KEY_MOD_ALT;
    if (xkb_state_mod_name_is_active(self->xkb_state_, XKB_MOD_NAME_LOGO,
                                     XKB_STATE_MODS_EFFECTIVE) > 0)
        ev.mods |= KEY_MOD_SUPER;
    self->deliver_key(ev);
    bool repeats = pressed && self->xkb_keymap_ &&
                   xkb_keymap_key_repeats(self->xkb_keymap_, code);
    self->kb_repeat_code_ = repeats ? code : 0;
    self->kb_repeat_ev_   = ev;
    self->kb_repeat_ev_.pressed = true;
    self->arm_key_repeat(repeats);
}

void Bar::on_kb_mods(void* data, wl_keyboard*, uint32_t, uint32_t depressed,
                     uint32_t latched, uint32_t locked, uint32_t group) {
    auto* self = static_cast<Bar*>(data);
    if (self->xkb_state_)
        xkb_state_update_mask(self->xkb_state_, depressed, latched, locked, 0,
                              0, group);
}

void Bar::on_kb_repeat(void* data, wl_keyboard*, int32_t rate, int32_t delay) {
    auto* self = static_cast<Bar*>(data);
    if (rate > 0) self->kb_repeat_rate_ = rate;
    if (delay > 0) self->kb_repeat_delay_ = delay;
}

void Bar::deliver_key(const KeyEvent& ev) {
    if (!kb_surface_) return;
    auto it = extra_surfaces_.find(kb_surface_);
    if (it != extra_surfaces_.end() && it->second.key) it->second.key(ev);
}

void Bar::arm_key_repeat(bool on) {
    if (kb_repeat_fd_ < 0) return;
    itimerspec ts{};
    if (on && kb_repeat_rate_ > 0) {
        ts.it_value.tv_sec  = kb_repeat_delay_ / 1000;
        ts.it_value.tv_nsec = (kb_repeat_delay_ % 1000) * 1000000L;
        long ns = 1000000000L / kb_repeat_rate_;
        ts.it_interval.tv_sec  = ns / 1000000000L;
        ts.it_interval.tv_nsec = ns % 1000000000L;
    }
    timerfd_settime(kb_repeat_fd_, 0, &ts, nullptr);
}

void Bar::set_cursor(wl_pointer* ptr, uint32_t serial) {
    // Preferred path: hand the compositor a shape enum and let IT render
    // the cursor. Zero cursor pixels live in this process — the multi-MB
    // wl_cursor theme pool below is never allocated on compositors with
    // cursor-shape-v1 (Hyprland has it), and fractional scales come out
    // right for free.
    if (cursor_shape_mgr_) {
        if (!cursor_shape_dev_)
            cursor_shape_dev_ = wp_cursor_shape_manager_v1_get_pointer(
                cursor_shape_mgr_, ptr);
        wp_cursor_shape_device_v1_set_shape(
            cursor_shape_dev_, serial,
            WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT);
        return;
    }
    // Fallback: classic client-side cursor from the theme.
    // HiDPI: load the theme at 24 × the highest output scale so the cursor
    // is crisp everywhere, and declare the buffer scale so mixed-DPI
    // setups show it at the right SIZE on every monitor.
    int maxsc = 1;
    for (auto* o : outputs_) maxsc = std::max(maxsc, (int)o->scale);
    if (cursor_theme_ && cursor_scale_ != maxsc) {
        wl_cursor_theme_destroy(cursor_theme_);
        cursor_theme_ = nullptr;
        cursor_       = nullptr;
    }
    if (!cursor_theme_) {
        cursor_theme_ = wl_cursor_theme_load(nullptr, 24 * maxsc, shm_);
        cursor_scale_ = maxsc;
        if (cursor_theme_) {
            cursor_ = wl_cursor_theme_get_cursor(cursor_theme_, "left_ptr");
            if (!cursor_)
                cursor_ = wl_cursor_theme_get_cursor(cursor_theme_, "default");
            if (!cursor_surface_)
                cursor_surface_ = wl_compositor_create_surface(compositor_);
        }
    }
    if (cursor_ && cursor_surface_) {
        wl_cursor_image* img = cursor_->images[0];
        if (wl_surface_get_version(cursor_surface_) >= 3)
            wl_surface_set_buffer_scale(cursor_surface_, maxsc);
        wl_surface_attach(cursor_surface_, wl_cursor_image_get_buffer(img), 0,
                          0);
        wl_surface_damage(cursor_surface_, 0, 0, img->width, img->height);
        wl_surface_commit(cursor_surface_);
        wl_pointer_set_cursor(ptr, serial, cursor_surface_,
                              img->hotspot_x / maxsc, img->hotspot_y / maxsc);
    }
}

void Bar::on_ptr_enter(void* data, wl_pointer* ptr, uint32_t serial,
                       wl_surface* surf, wl_fixed_t x, wl_fixed_t y) {
    auto* self = static_cast<Bar*>(data);
    self->ptr_surface_ = surf;
    self->set_cursor(ptr, serial);
    if (BarSurface* bs = self->surface_for(surf)) {
        DBG("pointer enter bar %s (%.0f,%.0f)", bs->name.c_str(),
            wl_fixed_to_double(x), wl_fixed_to_double(y));
        self->cur_     = bs;
        bs->ptr_inside = true;
        bs->ptr_x = wl_fixed_to_double(x);
        bs->ptr_y = wl_fixed_to_double(y);
        bs->arm_hide(false);
        if (cfg.multi_monitor && cfg.reveal_all_monitors)
            for (auto* o : self->surfaces_) o->arm_hide(false);
        if (!bs->expanded) {
            // Optional dwell requirement: the pointer must STAY in the hot
            // zone for reveal_delay_ms before the bar shows, so brushing
            // the screen edge doesn't misfire the reveal.
            if (cfg.reveal_delay_ms <= 0) {
                bs->set_expanded(true);
                if (cfg.multi_monitor && cfg.reveal_all_monitors)
                    self->reveal_all();
            } else {
                bs->arm_reveal(true);
            }
        } else if (cfg.multi_monitor && cfg.reveal_all_monitors) {
            self->reveal_all(); // entering an already-up bar syncs the rest
        }
    } else if (auto it = self->extra_surfaces_.find(surf);
               it != self->extra_surfaces_.end() && it->second.motion) {
        it->second.motion(wl_fixed_to_double(x), wl_fixed_to_double(y));
    }
}

void Bar::on_ptr_leave(void* data, wl_pointer*, uint32_t, wl_surface* surf) {
    auto* self = static_cast<Bar*>(data);
    if (self->ptr_surface_ == surf) self->ptr_surface_ = nullptr;
    if (BarSurface* bs = self->surface_for(surf)) {
        DBG("pointer leave bar %s", bs->name.c_str());
        bs->ptr_inside = false;
        bs->arm_reveal(false); // left the zone: cancel pending reveal
        bs->arm_hide(true);
        if (cfg.multi_monitor && cfg.reveal_all_monitors)
            for (auto* o : self->surfaces_)
                if (o->expanded) o->arm_hide(true);
        if (self->cur_ == bs) self->cur_ = nullptr;
    } else if (auto it = self->extra_surfaces_.find(surf);
               it != self->extra_surfaces_.end() && it->second.leave) {
        it->second.leave();
    }
}

void Bar::on_ptr_motion(void* data, wl_pointer*, uint32_t,
                        wl_fixed_t x, wl_fixed_t y) {
    auto* self = static_cast<Bar*>(data);
    if (BarSurface* bs = self->surface_for(self->ptr_surface_)) {
        self->cur_ = bs;
        bs->ptr_x  = wl_fixed_to_double(x);
        bs->ptr_y  = wl_fixed_to_double(y);
        if (bs->expanded) {
            double relx;
            if (Module* m = bs->hit(bs->along(), &relx)) m->on_hover(relx);
        }
    } else if (auto it = self->extra_surfaces_.find(self->ptr_surface_);
               it != self->extra_surfaces_.end() && it->second.motion) {
        it->second.motion(wl_fixed_to_double(x), wl_fixed_to_double(y));
    }
}

void Bar::on_ptr_button(void* data, wl_pointer*, uint32_t serial, uint32_t,
                        uint32_t button, uint32_t state) {
    auto* self = static_cast<Bar*>(data);
    self->last_button_serial_ = serial;
    if (state != WL_POINTER_BUTTON_STATE_PRESSED) {
        // releases matter to popup surfaces that implement dragging
        if (state == WL_POINTER_BUTTON_STATE_RELEASED &&
            !self->surface_for(self->ptr_surface_))
            if (auto it = self->extra_surfaces_.find(self->ptr_surface_);
                it != self->extra_surfaces_.end() && it->second.release)
                it->second.release(static_cast<int>(button));
        return;
    }
    if (BarSurface* bs = self->surface_for(self->ptr_surface_)) {
        self->cur_ = bs;
        self->route_click(static_cast<int>(button));
    } else if (auto it = self->extra_surfaces_.find(self->ptr_surface_);
               it != self->extra_surfaces_.end() && it->second.button) {
        it->second.button(static_cast<int>(button));
    }
}

void Bar::on_ptr_axis(void* data, wl_pointer*, uint32_t, uint32_t axis,
                      wl_fixed_t value) {
    auto* self = static_cast<Bar*>(data);
    if (axis != WL_POINTER_AXIS_VERTICAL_SCROLL) return;
    const int dir = wl_fixed_to_double(value) > 0 ? 1 : -1;
    if (BarSurface* bs = self->surface_for(self->ptr_surface_)) {
        self->cur_ = bs;
        self->route_scroll(dir);
    } else if (auto it = self->extra_surfaces_.find(self->ptr_surface_);
               it != self->extra_surfaces_.end() && it->second.scroll) {
        it->second.scroll(dir);
    }
}

// ---------------------------------------------------------------------------
// BarSurface — one bar on one output
// ---------------------------------------------------------------------------
bool BarSurface::owner_vertical() const { return cfg_vertical(); }

int BarSurface::hidden_thickness() const {
    int t = std::max(cfg.strip_hit_height, cfg.strip_height);
    if (t > cfg_thickness()) t = cfg_thickness();
    return t < 1 ? 1 : t;
}

static uint32_t anchors_for_position() {
    if (cfg.position == "bottom")
        return ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
               ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
               ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
    if (cfg.position == "left")
        return ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
               ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
               ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM;
    if (cfg.position == "right")
        return ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT |
               ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
               ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM;
    return ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
           ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
           ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
}

void BarSurface::on_configure(void* data, zwlr_layer_surface_v1* ls,
                              uint32_t serial, uint32_t w_, uint32_t h_) {
    auto* bs = static_cast<BarSurface*>(data);
    zwlr_layer_surface_v1_ack_configure(ls, serial);
    bs->awaiting_configure = false;
    bs->w = w_;
    bs->h = h_;
    bs->configured = true;
    bs->hidden_frame_valid = false; // size changed
    bs->draw(); // must attach a buffer in response to configure
    bs->dirty = false;
}

void BarSurface::on_closed(void* data, zwlr_layer_surface_v1*) {
    // The compositor closed this one bar (output going away). Drop it, but
    // keep the process alive for the other monitors.
    auto* bs = static_cast<BarSurface*>(data);
    if (bs->owner) bs->owner->outputs_dirty_ = true;
    bs->destroy();
}

// The scale to render at: the pinned output's, or — for a
// compositor-picked surface — whatever output the compositor put us on
// (learned from wl_surface.enter). set_buffer_scale needs wl_surface >= 3.
int BarSurface::scale() const {
    if (!owner || !surf || wl_surface_get_version(surf) < 3) return 1;
    return owner->scale_of(out ? out : entered_out);
}

void BarSurface::create(Bar& b, wl_output* o, const std::string& n) {
    owner = &b;
    out   = o;
    name  = n;
    surf  = wl_compositor_create_surface(b.compositor());
    static const wl_surface_listener surf_listener = {
        .enter = [](void* data, wl_surface*, wl_output* wo) {
            auto* bs = static_cast<BarSurface*>(data);
            if (bs->entered_out == wo) return;
            bs->entered_out = wo;
            // A compositor-picked bar just learned (or changed) its
            // output: repaint if that moves the scale.
            if (!bs->out && bs->scale() != bs->drawn_scale) {
                bs->hidden_frame_valid = false;
                bs->dirty = true;
            }
        },
        .leave = [](void* data, wl_surface*, wl_output* wo) {
            auto* bs = static_cast<BarSurface*>(data);
            if (bs->entered_out == wo) bs->entered_out = nullptr;
        },
    };
    wl_surface_add_listener(surf, &surf_listener, this);
    frac.on_change = [this] { // compositor moved this surface's scale
        hidden_frame_valid = false;
        dirty              = true;
    };
    frac.attach(b.frac_mgr(), b.viewporter(), surf);
    ls    = zwlr_layer_shell_v1_get_layer_surface(
        b.layer_shell(), surf, out, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
        "mattbar");
    static const zwlr_layer_surface_v1_listener lst = {
        .configure = on_configure,
        .closed    = on_closed,
    };
    zwlr_layer_surface_v1_add_listener(ls, &lst, this);
    zwlr_layer_surface_v1_set_anchor(ls, anchors_for_position());
    // THE auto-hide core: zero exclusive zone -> windows keep full height
    // and never move; the bar simply overlays them when revealed.
    zwlr_layer_surface_v1_set_exclusive_zone(ls, 0);
    zwlr_layer_surface_v1_set_keyboard_interactivity(ls, 0);
    if (cfg_vertical())
        zwlr_layer_surface_v1_set_size(ls, hidden_thickness(), 0);
    else
        zwlr_layer_surface_v1_set_size(ls, 0, hidden_thickness());

    hide_fd   = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    reveal_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    b.add_fd(reveal_fd, [this](uint32_t) {
        uint64_t v;
        while (read(reveal_fd, &v, sizeof v) > 0) {}
        if (ptr_inside && !expanded) {
            set_expanded(true);
            if (cfg.multi_monitor && cfg.reveal_all_monitors)
                owner->reveal_all();
        }
    }, "reveal-timer");
    b.add_fd(hide_fd, [this](uint32_t) {
        uint64_t v;
        while (read(hide_fd, &v, sizeof v) > 0) {}
        bool inside = cfg.multi_monitor && cfg.reveal_all_monitors
                          ? owner->any_pointer_inside()
                          : ptr_inside;
        if (!inside && expanded && !owner->pinned() && owner->hold_ == 0)
            set_expanded(false);
    }, "hide-timer");

    wl_surface_commit(surf);
    DBG("bar surface created on %s%s", name.empty() ? "(auto)" : name.c_str(),
        primary ? " [primary]" : "");
}

void BarSurface::destroy() {
    if (owner) {
        if (reveal_fd >= 0) owner->remove_fd(reveal_fd);
        if (hide_fd >= 0) owner->remove_fd(hide_fd);
    }
    if (reveal_fd >= 0) { close(reveal_fd); reveal_fd = -1; }
    if (hide_fd >= 0) { close(hide_fd); hide_fd = -1; }
    if (ls) { zwlr_layer_surface_v1_destroy(ls); ls = nullptr; }
    frac.destroy();
    if (surf) { wl_surface_destroy(surf); surf = nullptr; }
    configured = false;
}

void BarSurface::arm_reveal(bool arm) {
    if (reveal_fd < 0) return;
    itimerspec ts{};
    if (arm) {
        ts.it_value.tv_sec  = cfg.reveal_delay_ms / 1000;
        ts.it_value.tv_nsec = (cfg.reveal_delay_ms % 1000) * 1000000L;
        if (ts.it_value.tv_sec == 0 && ts.it_value.tv_nsec == 0)
            ts.it_value.tv_nsec = 1;
    }
    timerfd_settime(reveal_fd, 0, &ts, nullptr);
}

void BarSurface::arm_hide(bool arm) {
    if (hide_fd < 0) return;
    itimerspec ts{};
    if (arm) {
        ts.it_value.tv_sec  = cfg.hide_delay_ms / 1000;
        ts.it_value.tv_nsec = (cfg.hide_delay_ms % 1000) * 1000000L;
        // all-zero DISARMS a timerfd; delay 0 must mean "immediately"
        if (ts.it_value.tv_sec == 0 && ts.it_value.tv_nsec == 0)
            ts.it_value.tv_nsec = 1;
    }
    timerfd_settime(hide_fd, 0, &ts, nullptr);
}

void BarSurface::set_expanded(bool on) {
    if (expanded == on || !ls) return;
    DBG("set_expanded(%d) on %s", on, name.c_str());
    expanded = on;
    if (on) owner->tick_modules(); // wake with fresh clock/battery/etc.
    owner->update_tick();          // tick runs while ANY bar is revealed
    const uint32_t req =
        on ? static_cast<uint32_t>(cfg_thickness()) : hidden_thickness();
    if (req != (cfg_vertical() ? w : h)) awaiting_configure = true;
    if (cfg_vertical())
        zwlr_layer_surface_v1_set_size(ls, req, 0);
    else
        zwlr_layer_surface_v1_set_size(ls, 0, req);
    wl_surface_commit(surf); // compositor answers with configure -> draw
}

void BarSurface::apply_geometry() {
    if (!ls) return;
    hidden_frame_valid = false; // strip color/height may have changed
    zwlr_layer_surface_v1_set_anchor(ls, anchors_for_position());
    const uint32_t req =
        expanded ? static_cast<uint32_t>(cfg_thickness()) : hidden_thickness();
    if (req != (cfg_vertical() ? w : h)) awaiting_configure = true;
    if (cfg_vertical())
        zwlr_layer_surface_v1_set_size(ls, req, 0);
    else
        zwlr_layer_surface_v1_set_size(ls, 0, req);
    wl_surface_commit(surf);
}

Module* BarSurface::hit(double along_px, double* relx) {
    // Reverse order: slots are appended in draw order (left, center,
    // right), so the last match is the topmost-drawn module. Normally the
    // rects are disjoint and this changes nothing, but when the expanded
    // tray makes the right group overrun the center modules, the pointer
    // must resolve to what the user actually sees on top — otherwise
    // hovers and clicks over the tray icons land on the buried text.
    for (auto it = hits.rbegin(); it != hits.rend(); ++it)
        if (along_px >= it->x && along_px < it->x + it->w) {
            *relx = along_px - it->x;
            return it->mod;
        }
    return nullptr;
}

double BarSurface::slot_along(Module* m) const {
    for (auto& r : hits)
        if (r.mod == m) return r.x;
    return -1;
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------
bool Bar::init() {
    display_ = wl_display_connect(nullptr);
    if (!display_) {
        fprintf(stderr, "mattbar: cannot connect to Wayland display\n");
        return false;
    }
    registry_ = wl_display_get_registry(display_);
    static const wl_registry_listener reg_listener = {
        .global        = on_global,
        .global_remove = on_global_remove,
    };
    wl_registry_add_listener(registry_, &reg_listener, this);
    wl_display_roundtrip(display_);

    if (!compositor_ || !shm_ || !layer_shell_) {
        fprintf(stderr,
                "mattbar: missing globals (compositor/shm/layer-shell). "
                "Is your compositor wlr-layer-shell capable?\n");
        return false;
    }

    wl_display_roundtrip(display_); // second pass: wl_output name events

    // event loop plumbing
    epoll_fd_ = epoll_create1(EPOLL_CLOEXEC);
    tick_fd_  = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    // NOTE: tick timer starts DISARMED; the bars begin hidden and an idle
    // hidden bar schedules zero wakeups. It is armed on the first reveal.
    add_fd(tick_fd_, [this](uint32_t) {
        uint64_t n;
        while (read(tick_fd_, &n, sizeof n) > 0) {}
        if (any_expanded()) tick_modules();
    }, "tick-timer");

    // One bar per wanted output (or exactly one, wherever the compositor
    // puts it, when multi-monitor is off).
    sync_surfaces();
    if (surfaces_.empty()) {
        fprintf(stderr, "mattbar: no matching output; no bar to show\n");
        return false;
    }

    setup_omarchy_theme_watch();

    // ALL registered modules initialize (fds, D-Bus, ...) regardless of
    // where — or whether — the current layout places them.
    rebuild_layout();
    for (auto& [id, m] : modules_) m->init(*this);
    return true;
}

wl_output* Bar::pick_output() const {
    if (cfg.output.empty()) return nullptr; // compositor decides
    for (auto* o : outputs_)
        if (o->name == cfg.output) return o->wl;
    fprintf(stderr,
            "mattbar: output '%s' not found; letting the compositor pick "
            "(available:", cfg.output.c_str());
    for (auto* o : outputs_) fprintf(stderr, " %s", o->name.c_str());
    fprintf(stderr, ")\n");
    return nullptr;
}

void Bar::add_module(const std::string& id, Module* m) {
    modules_[id] = m;
}

void Bar::rebuild_layout() {
    cfg.layout_normalize();
    left.clear();
    center.clear();
    right.clear();
    more.clear();
    for (int z = 0; z < 4; ++z) {
        auto* dst = z == 0   ? &left
                    : z == 1 ? &center
                    : z == 2 ? &right
                             : &more;
        for (auto& id : cfg.layout_get(z)) {
            auto it = modules_.find(id);
            if (it != modules_.end()) dst->push_back(it->second);
        }
    }
}

void Bar::add_fd(int fd, std::function<void(uint32_t)> cb,
                 const char* label) {
    epoll_event ev{};
    ev.events  = EPOLLIN;
    ev.data.fd = fd;
    epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev);
    fd_cbs_[fd]    = std::move(cb);
    fd_labels_[fd] = label;
}

void Bar::mod_fd(int fd, uint32_t events) {
    epoll_event ev{};
    ev.events  = events; // 0 is valid: only ERR/HUP will be delivered
    ev.data.fd = fd;
    epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &ev);
}

void Bar::remove_fd(int fd) {
    epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
    fd_cbs_.erase(fd);
    fd_labels_.erase(fd);
}

void Bar::profiler_note_wake(int fd, int wl_fd) {
    if (!dbg()) return;
    if (fd == wl_fd) {
        ++wake_counts_["wayland"];
    } else {
        auto it = fd_labels_.find(fd);
        ++wake_counts_[it != fd_labels_.end() ? it->second : "unknown"];
    }
}

void Bar::profiler_report() {
    if (!dbg()) return;
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    long now = ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
    if (prof_last_ms_ == 0) { prof_last_ms_ = now; return; }
    if (now - prof_last_ms_ < 5000) return;
    std::string line;
    for (auto& [k, v] : wake_counts_)
        if (v) line += " " + k + "=" + std::to_string(v);
    if (!line.empty() || wl_event_count_ || draw_count_)
        fprintf(stderr,
                "mattbar: wakeups over %.1fs:%s wl-events=%llu draws=%llu\n",
                (now - prof_last_ms_) / 1000.0, line.c_str(),
                (unsigned long long)wl_event_count_,
                (unsigned long long)draw_count_);
    wake_counts_.clear();
    wl_event_count_ = 0;
    draw_count_ = 0;
    prof_last_ms_ = now;
}

void Bar::register_surface(wl_surface* s, SurfaceHooks hooks) {
    extra_surfaces_[s] = std::move(hooks);
}

void Bar::unregister_surface(wl_surface* s) {
    extra_surfaces_.erase(s);
    if (ptr_surface_ == s) ptr_surface_ = nullptr;
    if (kb_surface_ == s) {
        kb_surface_ = nullptr;
        arm_key_repeat(false);
    }
}

// ---------------------------------------------------------------------------
// Auto-hide / pin / hold
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Bar facade: "the bar" means the surface currently being drawn or handling
// input; outside that context it means the primary one (or "any", for
// state queries like expanded()).
// ---------------------------------------------------------------------------
BarSurface* Bar::surface_for(wl_surface* s) const {
    if (!s) return nullptr;
    for (auto* bs : surfaces_)
        if (bs->surf == s) return bs;
    return nullptr;
}

bool Bar::any_pointer_inside() const {
    for (auto* bs : surfaces_)
        if (bs->ptr_inside) return true;
    return false;
}

// With reveal_all on, the bars behave as one: enter anywhere reveals every
// bar, and hiding waits until the pointer has left them all.
void Bar::reveal_all() {
    for (auto* bs : surfaces_)
        if (!bs->expanded) bs->set_expanded(true);
}

void Bar::ctl_reveal() {
    for (auto* bs : surfaces_) {
        if (!bs->expanded) bs->set_expanded(true);
        if (!bs->ptr_inside) bs->arm_hide(true); // collapse after hide delay
    }
}

void Bar::ctl_hide() {
    for (auto* bs : surfaces_)
        if (bs->expanded && !pinned_ && hold_ == 0) bs->set_expanded(false);
}

bool Bar::any_expanded() const {
    for (auto* bs : surfaces_)
        if (bs->expanded) return true;
    return false;
}

bool Bar::expanded() const { return cur_ ? cur_->expanded : any_expanded(); }

void Bar::request_draw() {
    for (auto* bs : surfaces_) bs->dirty = true;
}

wl_surface* Bar::bar_surface() const {
    BarSurface* bs = current();
    return bs ? bs->surf : nullptr;
}

zwlr_layer_surface_v1* Bar::layer_surface() const {
    BarSurface* bs = current();
    return bs ? bs->ls : nullptr;
}

double Bar::pointer_x() const { return current() ? current()->ptr_x : 0; }

double Bar::pointer_along() const {
    BarSurface* bs = current();
    if (!bs) return 0;
    return cfg_vertical() ? bs->ptr_y : bs->ptr_x;
}

std::string Bar::current_output_name() const {
    BarSurface* bs = current();
    return bs ? bs->name : std::string();
}

wl_output* Bar::current_output() const {
    BarSurface* bs = current();
    return bs ? bs->out : nullptr;
}

wl_output* Bar::input_output() const {
    return cur_ ? cur_->out : nullptr;
}

wl_output* Bar::output_named(const std::string& name) const {
    if (name.empty()) return nullptr;
    for (auto* o : outputs_)
        if (o->name == name) return o->wl;
    return nullptr;
}

wl_output* Bar::focused_output() const {
    // hyprctl -j activeworkspace is a small JSON blob with "monitor":"DP-1".
    std::string j = cmd_output("hyprctl -j activeworkspace 2>/dev/null");
    auto k = j.find("\"monitor\"");
    if (k != std::string::npos) {
        auto q = j.find('"', j.find(':', k));
        if (q != std::string::npos) {
            auto e = j.find('"', q + 1);
            if (e != std::string::npos)
                if (wl_output* o = output_named(j.substr(q + 1, e - q - 1)))
                    return o;
        }
    }
    return nullptr;
}

wl_output* Bar::primary_output() const {
    return primary_ ? primary_->out : nullptr;
}

std::string Bar::primary_name() const {
    return primary_ ? primary_->name : std::string();
}

bool Bar::current_is_primary() const {
    BarSurface* bs = current();
    return bs ? bs->primary : true;
}

double Bar::slot_along(Module* m) const {
    BarSurface* bs = current();
    if (!bs) return -1;
    double a = bs->slot_along(m);
    if (a >= 0) return a;
    // Overflow modules are not on the bar; hang their popups off More.
    auto it = modules_.find("more");
    if (it != modules_.end() && it->second != m)
        return bs->slot_along(it->second);
    return -1;
}

int Bar::scale_of(wl_output* o) const {
    if (!o) return 1;
    for (auto* bo : outputs_)
        if (bo->wl == o) return bo->scale;
    return 1;
}

double Bar::along_length() const {
    BarSurface* bs = current();
    if (!bs) return 0;
    return cfg_vertical() ? bs->h : bs->w;
}

std::vector<std::string> Bar::output_names() const {
    std::vector<std::string> v;
    for (auto* o : outputs_)
        if (!o->name.empty()) v.push_back(o->name);
    std::sort(v.begin(), v.end());
    return v;
}

std::vector<Bar::OutputRef> Bar::output_list() const {
    std::vector<OutputRef> v;
    for (auto* o : outputs_) {
        int sc = o->scale > 0 ? o->scale : 1;
        v.push_back({o->wl, o->name, sc, sc ? o->px_w / sc : 0,
                     sc ? o->px_h / sc : 0});
    }
    return v;
}

// ---------------------------------------------------------------------------
// Reconcile bar surfaces with the outputs the config asks for. This is the
// single entry point for startup, monitor hotplug, and settings changes, so
// there is only ever one rule deciding which monitors carry a bar.
// ---------------------------------------------------------------------------
void Bar::sync_surfaces() {
    if (!compositor_ || !layer_shell_) return;

    // A surface the compositor closed under us (output going away mid-frame)
    // leaves a husk behind; drop those first so the logic below can decide
    // freshly whether that monitor should get a bar again.
    for (auto it = surfaces_.begin(); it != surfaces_.end();) {
        if ((*it)->surf == nullptr) {
            if (cur_ == *it) cur_ = nullptr;
            if (primary_ == *it) primary_ = nullptr;
            delete *it;
            it = surfaces_.erase(it);
        } else {
            ++it;
        }
    }

    if (!cfg.multi_monitor) {
        // Single-bar mode, unchanged from before: one surface, pinned to
        // cfg.output when it names a connected monitor, otherwise wherever
        // the compositor decides to put it.
        wl_output* want = pick_output();
        std::string want_name;
        for (auto* o : outputs_)
            if (o->wl == want) want_name = o->name;
        if (surfaces_.size() == 1 && surfaces_[0]->out == want) {
            surfaces_[0]->primary = true;
            primary_ = surfaces_[0];
            return;
        }
        for (auto* bs : surfaces_) { bs->destroy(); delete bs; }
        surfaces_.clear();
        cur_ = primary_ = nullptr;
        auto* bs = new BarSurface{};
        bs->primary = true;
        bs->create(*this, want, want_name);
        surfaces_.push_back(bs);
        primary_ = bs;
        return;
    }

    // Multi-monitor: drop bars on outputs we no longer want...
    for (auto it = surfaces_.begin(); it != surfaces_.end();) {
        BarSurface* bs = *it;
        bool keep = bs->out && cfg.wants_monitor(bs->name);
        if (!keep) {
            DBG("dropping bar on %s", bs->name.c_str());
            if (cur_ == bs) cur_ = nullptr;
            if (primary_ == bs) primary_ = nullptr;
            bs->destroy();
            delete bs;
            it = surfaces_.erase(it);
        } else {
            ++it;
        }
    }
    // ...and add bars on the ones we do.
    for (auto* o : outputs_) {
        if (o->name.empty()) continue; // name event hasn't landed yet
        if (!cfg.wants_monitor(o->name)) continue;
        bool have = false;
        for (auto* bs : surfaces_)
            if (bs->out == o->wl) have = true;
        if (have) continue;
        auto* bs = new BarSurface{};
        bs->create(*this, o->wl, o->name);
        surfaces_.push_back(bs);
    }

    // A compositor that reports no output names (wl_output < v4) would
    // otherwise leave us with no bar at all. Fall back to one
    // compositor-placed bar rather than showing nothing.
    if (surfaces_.empty() && !outputs_.empty()) {
        fprintf(stderr,
                "mattbar: no output matched (multi-monitor); falling back to "
                "a single compositor-placed bar\n");
        auto* bs = new BarSurface{};
        bs->create(*this, nullptr, "");
        surfaces_.push_back(bs);
    }

    // An output's scale may have changed (that is one of the events that
    // lands us here): repaint any bar whose committed buffer is stale.
    for (auto* bs : surfaces_) {
        if (bs->scale() != bs->drawn_scale) {
            bs->hidden_frame_valid = false;
            bs->dirty = true;
        }
    }

    // Elect the primary: the configured one if it is present, else the
    // first bar we have. Everything singleton (tray, settings window,
    // notification and OSD popups) follows this election.
    BarSurface* want_primary = nullptr;
    if (!cfg.primary_output.empty())
        for (auto* bs : surfaces_)
            if (bs->name == cfg.primary_output) want_primary = bs;
    if (!want_primary && !surfaces_.empty()) want_primary = surfaces_.front();
    if (want_primary != primary_) {
        for (auto* bs : surfaces_) bs->primary = (bs == want_primary);
        primary_ = want_primary;
        if (primary_)
            DBG("primary bar is %s", primary_->name.c_str());
        // The tray only draws on the primary bar, so a re-election means
        // both the old and the new primary need a repaint.
        for (auto* bs : surfaces_) bs->hidden_frame_valid = false;
        request_draw();
    }
    wallpaper_apply();
}

// The tick timer runs while ANY bar is revealed, and is fully disarmed once
// every bar is hidden — the zero-wakeup idle property survives multi-monitor.
void Bar::update_tick() { arm_tick(any_expanded()); }

void Bar::tick_modules() {
    qs_plugins_reap();
    // Overflow children first so the More popup redraws with fresh text.
    for (auto* m : more)
        if (m->enabled()) m->tick();
    for (auto* lst : {&left, &center, &right})
        for (auto* m : *lst)
            if (m->enabled()) m->tick();
}

void Bar::toggle_pinned() {
    pinned_ = !pinned_;
    // Pin is global: one click keeps every bar up, so a glance at another
    // monitor doesn't collapse the one you pinned.
    for (auto* bs : surfaces_) {
        if (pinned_) {
            bs->arm_hide(false);
            if (!bs->expanded) bs->set_expanded(true);
        } else if (!bs->ptr_inside && hold_ == 0) {
            bs->arm_hide(true);
        }
    }
    request_draw();
}

void Bar::toggle_settings() {
    if (settings_) {
        close_settings_later();
    } else {
        settings_ = new SettingsWindow(*this);
    }
}

void Bar::close_settings_later() { settings_close_pending_ = true; }

void Bar::refresh_settings() {
    if (settings_) settings_->refresh();
}

void Bar::apply_config() {
    // Sidecar first: restore user shell.json before notifyd relaunches
    // a full Omarchy shell, and skip notifyd's kill while plugins run.
    qs_plugins_apply(*this);
    if (notify_daemon()) notify_daemon()->apply_enabled();
    if (mattbar_shell()) mattbar_shell()->apply_takeover();
    more_close();
    rebuild_layout();
    // A settings change may have switched multi-monitor on/off, edited the
    // monitor list, or moved the primary: reconcile before re-geometry.
    sync_surfaces();
    for (auto* bs : surfaces_) bs->apply_geometry();
    update_tick();  // pick up a changed tick interval / revealed state
    tick_modules(); // recompose module text under the new settings now
    request_draw();
}

void Bar::setup_omarchy_theme_watch() {
    // Watch even while the toggle is off, so enabling it later still tracks
    // live; theme swaps are rare, and without an Omarchy install there is
    // no watch (and no wakeups) at all.
    std::string dir = omarchy_theme_watch_dir();
    if (dir.empty()) return;
    omarchy_inotify_fd_ = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (omarchy_inotify_fd_ < 0) return;
    // A theme swap replaces the "theme" entry in .../omarchy/current
    // (rm -rf + mv in current Omarchy, ln -nsf in early versions); watching
    // the parent survives the replacement.
    if (inotify_add_watch(omarchy_inotify_fd_, dir.c_str(),
                          IN_CREATE | IN_MOVED_TO | IN_DELETE |
                              IN_CLOSE_WRITE) < 0) {
        close(omarchy_inotify_fd_);
        omarchy_inotify_fd_ = -1;
        return;
    }
    add_fd(omarchy_inotify_fd_, [this](uint32_t) {
        on_omarchy_theme_event();
    }, "omarchy-theme");
    DBG("omarchy theme watch on %s", dir.c_str());
}

void Bar::on_omarchy_theme_event() {
    // Drain everything queued; one re-read covers the whole swap burst.
    char buf[2048];
    bool relevant = false;
    ssize_t n;
    while ((n = read(omarchy_inotify_fd_, buf, sizeof buf)) > 0) {
        for (ssize_t off = 0; off < n;) {
            auto* ev = reinterpret_cast<inotify_event*>(buf + off);
            if (ev->len && (strncmp(ev->name, "theme", 5) == 0))
                relevant = true; // "theme", "theme.name", "next-theme"...
            off += sizeof(inotify_event) + ev->len;
        }
    }
    if (!relevant || !cfg.follow_omarchy_theme) return;
    DBG("omarchy theme changed; re-applying");
    if (omarchy_theme_apply(cfg)) {
        apply_config();
        if (settings_) settings_->refresh();
    }
}

void Bar::hold_open(bool acquire) {
    hold_ += acquire ? 1 : -1;
    if (hold_ < 0) hold_ = 0;
    if (hold_ == 0 && !pinned_)
        for (auto* bs : surfaces_)
            if (!bs->ptr_inside) bs->arm_hide(true);
}

void Bar::arm_tick(bool arm) {
    itimerspec ts{};
    if (arm) {
        ts.it_interval.tv_sec  = cfg.tick_ms / 1000;
        ts.it_interval.tv_nsec = (cfg.tick_ms % 1000) * 1000000L;
        ts.it_value = ts.it_interval;
    }
    timerfd_settime(tick_fd_, 0, &ts, nullptr);
    DBG("tick timer %s", arm ? "armed" : "disarmed");
}

// ---------------------------------------------------------------------------
// Input routing
// ---------------------------------------------------------------------------
void Bar::route_click(int button) {
    BarSurface* bs = cur_;
    if (!bs || !bs->expanded) return;
    double relx;
    Module* m = bs->hit(cfg_vertical() ? bs->ptr_y : bs->ptr_x, &relx);
    // Observer sees every bar click (m may be null for dead space) so a
    // module can react to interaction elsewhere on the bar — e.g. the
    // agents dropdown dismissing itself.
    if (click_observer_) click_observer_(m);
    auto mit = modules_.find("more");
    if (mit != modules_.end() && m != mit->second && more_is_open())
        more_close();
    auto pit = modules_.find("plugins");
    if (pit != modules_.end() && m != pit->second && plugins_is_open())
        plugins_close();
    if (m && m->on_click(relx, button)) request_draw();
}

void Bar::route_scroll(int dir) {
    BarSurface* bs = cur_;
    if (!bs || !bs->expanded) return;
    double relx;
    if (Module* m = bs->hit(cfg_vertical() ? bs->ptr_y : bs->ptr_x, &relx))
        if (m->on_scroll(relx, dir)) request_draw();
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------
void BarSurface::draw() {
    if (!configured || w == 0 || h == 0 || !surf) return;
    if (!expanded && hidden_frame_valid) return; // nothing visible changed
    DBG("draw %s %s %ux%u", name.c_str(), expanded ? "expanded" : "hidden", w,
        h);
    ++owner->draw_count_;

    // Module code asks the bar which surface it is rendering for (workspaces
    // filters by monitor, popups open on the monitor you clicked).
    BarSurface* prev = owner->cur_;
    owner->cur_ = this;
    struct Restore {
        Bar* b; BarSurface* p;
        ~Restore() { b->cur_ = p; }
    } restore{owner, prev};

    // HiDPI: the buffer is scale× the logical size; a global cairo scale
    // keeps every module drawing in logical coordinates, so no drawing code
    // anywhere needs to know. Hit rects and pointer coords stay logical.
    const int sc = scale();
    drawn_scale  = sc;
    const int wi = static_cast<int>(w);
    const int hi = static_cast<int>(h);
    // Buffer size: exact fractional pixels when the compositor prefers a
    // non-integer scale (frac.active()), else logical x integer scale. The
    // cairo transform maps logical drawing onto whichever buffer this is,
    // so no module code changes either way.
    const int bw = frac.active() ? frac.px(wi) : wi * sc;
    const int bh = frac.active() ? frac.px(hi) : hi * sc;
    void* data = nullptr;
    wl_buffer* buffer = create_argb_buffer(owner->shm(), bw, bh, &data);
    if (!buffer) return;

    cairo_surface_t* cs = cairo_image_surface_create_for_data(
        static_cast<unsigned char*>(data), CAIRO_FORMAT_ARGB32, bw, bh,
        bw * 4);
    cairo_t* cr = cairo_create(cs);
    cairo_scale(cr, (double)bw / wi, (double)bh / hi);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);

    hits.clear();
    const int w_ = wi, h_ = hi; // shadow names used by the body below
    const bool expanded_ = expanded;

    if (!expanded_) {
        // hidden state: fully transparent except the visible strip line.
        // Interactivity is limited to the hover zone by the input region
        // below, so clicks in the transparent area reach the windows.
        cairo_set_source_rgba(cr, 0, 0, 0, 0);
        cairo_paint(cr);
        set_color(cr, cfg.c_strip);
        const int sh = cfg.strip_height;
        if (cfg.position == "bottom")
            cairo_rectangle(cr, 0, h_ - sh, w_, sh);
        else if (cfg.position == "left")
            cairo_rectangle(cr, 0, 0, sh, h_);
        else if (cfg.position == "right")
            cairo_rectangle(cr, w_ - sh, 0, sh, h_);
        else
            cairo_rectangle(cr, 0, 0, w_, sh);
        cairo_fill(cr);
        hidden_frame_valid = true;
    } else {
        hidden_frame_valid = false; // next hide must repaint once
        set_color(cr, cfg.c_bg);
        cairo_paint(cr);
        cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
        // Custom modules may select their own font, so re-select the default
        // before every measure/draw call.
        auto setfont = [&] {
            cairo_select_font_face(cr, cfg.font.c_str(),
                                   CAIRO_FONT_SLANT_NORMAL,
                                   CAIRO_FONT_WEIGHT_NORMAL);
            cairo_set_font_size(cr, cfg.font_size);
        };
        using Placed = std::vector<std::pair<Module*, double>>;
        auto measure = [&](std::vector<Module*>& list) {
            Placed out;
            for (auto* m : list) {
                if (!m->enabled()) continue;
                // Singleton modules (the tray) render on the primary bar
                // only: a second SNI host would fight the first.
                if (m->primary_only() && !primary) continue;
                setfont();
                double mw = m->width(cr);
                if (mw <= 0.5) continue; // empty module: no slot, no gap
                out.push_back({m, mw});
            }
            return out;
        };
        auto total = [&](const Placed& items) {
            double t = 0;
            for (auto& [m, mw] : items) t += mw;
            if (!items.empty()) t += MODULE_GAP * (items.size() - 1);
            return t;
        };
        const double thick = cfg_vertical() ? w_ : h_;
        auto place = [&](const Placed& items, double x) {
            for (auto& [m, mw] : items) {
                setfont();
                m->draw(cr, x, thick); // (along-offset, thickness)
                hits.push_back({x, mw, m});
                x += mw + MODULE_GAP;
            }
        };
        const double along_len = cfg_vertical() ? h_ : w_;
        Placed L = measure(owner->left), C = measure(owner->center),
               R = measure(owner->right);
        place(L, SIDE_PADDING);
        place(C, (along_len - total(C)) / 2.0);
        place(R, along_len - SIDE_PADDING - total(R));
    }

    cairo_destroy(cr);
    cairo_surface_destroy(cs);

    frac.apply(surf, w_, h_, sc); // viewport dest (fractional) or buffer scale
    wl_surface_attach(surf, buffer, 0, 0);
    if (wl_surface_get_version(surf) >= 4)
        wl_surface_damage_buffer(surf, 0, 0, bw, bh);
    else
        wl_surface_damage(surf, 0, 0, w_, h_);
    wl_surface_commit(surf);
}

// ---------------------------------------------------------------------------
// Main loop
// ---------------------------------------------------------------------------
static int g_stop_fd = -1;

void Bar::request_stop() {
    running_ = 0;
    if (g_stop_fd >= 0) {
        uint64_t one = 1;
        (void)!write(g_stop_fd, &one, sizeof one);
    }
}

void Bar::flush_wayland() {
    while (wl_display_prepare_read(display_) != 0) {
        if (wl_display_dispatch_pending(display_) < 0) {
            fprintf(stderr, "mattbar: wayland error %d, exiting\n",
                    wl_display_get_error(display_));
            running_ = 0;
            return;
        }
    }
    wl_display_flush(display_);
}

void Bar::run() {
    const int wl_fd = wl_display_get_fd(display_);
    epoll_event ev{};
    ev.events  = EPOLLIN;
    ev.data.fd = wl_fd;
    epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, wl_fd, &ev);

    // Under a Type=notify unit: declare readiness, then ping the watchdog
    // from a timerfd *inside* this loop — so the ping is a liveness proof
    // of the event loop itself, not of a side thread. A wedged loop (the
    // exact failure the 7s-freeze taught us to fear) stops pinging and
    // systemd restarts us; with notification state persisted, the visible
    // cost is a sub-second flicker. Outside systemd: no-ops.
    uint64_t wd_usec = 0;
    int      wd_fd   = -1;
    if (g_stop_fd < 0) {
        g_stop_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        if (g_stop_fd >= 0)
            add_fd(g_stop_fd, [this](uint32_t) {
                uint64_t x;
                while (read(g_stop_fd, &x, sizeof x) > 0) {}
                running_ = 0;
            }, "stop");
    }

    sd_notify(0, "READY=1");
    if (sd_watchdog_enabled(0, &wd_usec) > 0 && wd_usec > 0) {
        wd_usec_ = wd_usec;
        // Ping at a third of the budget (not half): suspend/resume kills
        // happen because the pre-suspend ping age PLUS the post-resume
        // thaw window must fit inside WatchdogSec, and the process is
        // frozen for the whole overlap — the only defences are a small
        // ping age going into suspend and unit-side headroom.
        wd_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
        if (wd_fd >= 0) {
            itimerspec ts{};
            uint64_t third       = wd_usec / 3;
            ts.it_value.tv_sec   = third / 1000000;
            ts.it_value.tv_nsec  = (third % 1000000) * 1000;
            ts.it_interval       = ts.it_value;
            timerfd_settime(wd_fd, 0, &ts, nullptr);
            add_fd(wd_fd, [this, wd_fd](uint32_t) {
                uint64_t x;
                while (read(wd_fd, &x, sizeof x) > 0) {}
                ping_watchdog();
            }, "sd-watchdog");
        }
        ping_watchdog(); // first ping immediately, not at +interval
    }

    while (running_) {
        wl_display_dispatch_pending(display_);
        if (settings_close_pending_) {
            settings_close_pending_ = false;
            delete settings_;
            settings_ = nullptr;
        }
        // Monitor hotplug (or a wl_output name landing late) asks for a
        // reconcile; doing it here keeps surface creation out of callbacks.
        if (outputs_dirty_) {
            outputs_dirty_ = false;
            sync_surfaces();
            lock_sync_outputs();
        }
        lock_flush();
        for (auto* bs : surfaces_) {
            if (bs->dirty && bs->configured && !bs->awaiting_configure) {
                bs->dirty = false;
                bs->draw();
            }
        }

        flush_wayland();

        epoll_event evs[16];
        // Bounded wait + overdue check: after a resume the loop pings on
        // its very first iteration instead of waiting for a timer slot,
        // and a callback storm can never starve the ping for a full
        // budget.
        int n = epoll_wait(epoll_fd_, evs, 16, wd_usec_ ? 2000 : -1);
        // Capture errno before ping_watchdog()/clock_gettime. After
        // hibernate thaw, sd_notify can fail with EPERM and clobber a
        // real EINTR — we then exited, Hyprland kept the session lock,
        // and the password field was gone.
        int ep_err = n < 0 ? errno : 0;
        if (wd_usec_) {
            timespec tsn;
            clock_gettime(CLOCK_MONOTONIC, &tsn);
            uint64_t now =
                tsn.tv_sec * 1000000ULL + tsn.tv_nsec / 1000ULL;
            if (now - last_wd_ping_ > wd_usec_ / 3) ping_watchdog();
        }
        if (n < 0) {
            wl_display_cancel_read(display_);
            if (ep_err == EINTR || ep_err == EAGAIN || ep_err == EPERM) {
                if (ep_err != EINTR)
                    fprintf(stderr,
                            "mattbar: event loop: epoll %s after wait; "
                            "retrying (do not drop a live lock client)\n",
                            strerror(ep_err));
                continue;
            }
            fprintf(stderr, "mattbar: event loop: epoll failed: %s — "
                    "exiting for restart\n", strerror(ep_err));
            exit_code_ = 1; // abnormal: systemd must restart us
            break;
        }

        bool wl_ready = false;
        for (int i = 0; i < n; ++i) {
            if (evs[i].data.fd == wl_fd) wl_ready = true;
            profiler_note_wake(evs[i].data.fd, wl_fd);
        }

        if (wl_ready) {
            if (wl_display_read_events(display_) < 0) {
                // Resume-thaw and compositor churn can error the Wayland
                // connection. This exit used to return SUCCESS, which
                // made Restart=on-failure leave the bar down after
                // suspend once the watchdog no longer fired first.
                int werr = wl_display_get_error(display_);
                fprintf(stderr, "mattbar: event loop: wayland read "
                        "failed (display error %d, errno %s) — exiting "
                        "for restart\n", werr, strerror(errno));
                exit_code_ = 1;
                break;
            }
        } else {
            wl_display_cancel_read(display_);
        }
        {
            int d = wl_display_dispatch_pending(display_);
            if (dbg() && d > 0) wl_event_count_ += d;
        }
        lock_flush();
        profiler_report();

        for (int i = 0; i < n; ++i) {
            if (evs[i].data.fd == wl_fd) continue;
            auto it = fd_cbs_.find(evs[i].data.fd);
            if (it != fd_cbs_.end()) {
                // Copy before invoking: a callback may remove_fd(itself)
                // (e.g. a stream hitting EOF); calling through the map
                // reference would destroy the std::function mid-execution.
                auto cb = it->second;
                cb(evs[i].events);
            }
        }
    }
}

void Bar::shutdown() {
    running_ = 0;
    idle_shutdown();
    qs_plugins_stop();
    delete settings_;
    settings_ = nullptr;
    for (auto& [id, m] : modules_) delete m;
    for (auto* bs : surfaces_) { bs->destroy(); delete bs; }
    surfaces_.clear();
    cur_ = primary_ = nullptr;
    for (auto* o : outputs_) {
        if (o->wl) wl_output_destroy(o->wl);
        delete o;
    }
    outputs_.clear();
    modules_.clear();
    left.clear();
    center.clear();
    right.clear();
    more.clear();
    arm_key_repeat(false);
    if (keyboard_) {
        wl_keyboard_destroy(keyboard_);
        keyboard_ = nullptr;
    }
    if (xkb_state_) {
        xkb_state_unref(xkb_state_);
        xkb_state_ = nullptr;
    }
    if (xkb_keymap_) {
        xkb_keymap_unref(xkb_keymap_);
        xkb_keymap_ = nullptr;
    }
    if (xkb_ctx_) {
        xkb_context_unref(xkb_ctx_);
        xkb_ctx_ = nullptr;
    }
    if (wm_base_) xdg_wm_base_destroy(wm_base_);
    if (cursor_shape_dev_) wp_cursor_shape_device_v1_destroy(cursor_shape_dev_);
    if (cursor_shape_mgr_) wp_cursor_shape_manager_v1_destroy(cursor_shape_mgr_);
    if (cursor_theme_) wl_cursor_theme_destroy(cursor_theme_);
    if (display_) wl_display_disconnect(display_);
}

void Bar::ping_watchdog() {
    sd_notify(0, "WATCHDOG=1");
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    last_wd_ping_ = ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000ULL;
}

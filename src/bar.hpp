#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include <wayland-client.h>
#include <wayland-cursor.h>
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#include "config.hpp"
#include "modules.hpp"

class SettingsWindow;
class Bar;

// Hit-test region belonging to a module.
struct HitRect {
    double x, w;
    Module* mod;
};

// Per-monitor bar surface.
struct wp_viewporter;
struct wp_fractional_scale_manager_v1;
#include "frac.hpp"

struct BarSurface {
    Bar*        owner = nullptr;
    wl_output*  out   = nullptr;  // nullptr = compositor picks (single-bar)
    std::string name;             // "DP-2"; empty when compositor picked

    wl_surface*            surf = nullptr;
    zwlr_layer_surface_v1* ls   = nullptr;

    uint32_t w = 0, h = 0;
    bool     configured = false;
    bool     expanded   = false;
    bool     dirty      = false;
    bool     hidden_frame_valid = false;
    bool     awaiting_configure = false;
    bool     primary    = false;  // hosts tray, settings, notification popups

    bool   ptr_inside = false;
    double ptr_x = 0, ptr_y = 0;

    // HiDPI: geometry is LOGICAL; only the pixel buffer is scaled.
    wl_output* entered_out = nullptr;
    int        drawn_scale = 1; // scale of last committed buffer
    int        scale() const;
    // Fractional scaling: render at exact preferred scale via viewport; geometry stays logical.
    FracSurface frac;

    int hide_fd = -1, reveal_fd = -1;

    std::vector<HitRect> hits;

    void   create(Bar& b, wl_output* o, const std::string& n);
    void   destroy();
    void   set_expanded(bool on);
    void   apply_geometry();      // re-push size/anchors after config change
    void   draw();
    void   arm_hide(bool arm);
    void   arm_reveal(bool arm);
    double along() const { return owner_vertical() ? ptr_y : ptr_x; }
    Module* hit(double along_px, double* relx);
    double  slot_along(Module* m) const; // where m sits on this bar, or -1

private:
    bool owner_vertical() const;
    int  hidden_thickness() const;

    static void on_configure(void*, zwlr_layer_surface_v1*, uint32_t,
                             uint32_t, uint32_t);
    static void on_closed(void*, zwlr_layer_surface_v1*);
};

class Bar {
public:
    // Observe all bar clicks (hit module or nullptr). Used by agents dropdown to dismiss on interaction elsewhere.
    void set_click_observer(std::function<void(Module*)> f) {
        click_observer_ = std::move(f);
    }
    // Send WATCHDOG=1. Call between long module steps so slow compositor work cannot starve liveness pings.
    void ping_watchdog();
    int  exit_code() const { return exit_code_; }
    bool init();
    void run();
    void shutdown();

    // --- services for modules -------------------------------------------
    void request_draw(); // marks every bar surface dirty
    // Register fd with main epoll loop; cb receives epoll events mask.
    void add_fd(int fd, std::function<void(uint32_t)> cb,
                const char* label = "module");
    void remove_fd(int fd);
    void mod_fd(int fd, uint32_t events);
    // During draw/pointer event: current surface; else "any bar".
    bool expanded() const;

    // --- multi-monitor --------------------------------------------------- Surface currently drawn/handling input and its output.
    BarSurface* current() const { return cur_ ? cur_ : primary_; }
    std::string current_output_name() const;
    wl_output*  current_output() const;
    wl_output*  primary_output() const;
    // Name of the bar elected primary (settings shows this).
    std::string primary_name() const;
    bool        current_is_primary() const;
    // Module slot start on current bar (-1 if not drawn).
    double      slot_along(Module* m) const;
    // Current bar length along main axis (screen width/height in logical px).
    double      along_length() const;
    // Integer buffer scale of an output (1 when unknown).
    int         scale_of(wl_output*) const;
    // Reconcile surfaces with config-requested outputs (hotplug/settings).
    void        sync_surfaces();
    std::vector<std::string> output_names() const;

    // Pin: keep revealed until unpinned. Does not touch exclusive zone.
    void toggle_pinned();
    bool pinned() const { return pinned_; }

    // Hold: modules (e.g. open tray menu) temporarily prevent auto-hide. Balanced acquire/release.
    void hold_open(bool acquire);

    // Settings window (gear icon in tray)
    void toggle_settings();
    void close_settings_later(); // safe from settings' own callbacks
    // Re-apply cfg-derived surface geometry after settings change.
    void apply_config();

    // Auxiliary surfaces (popups): receive pointer events routed by the bar.
    struct SurfaceHooks {
        std::function<void(double, double)> motion; // surface-local coords
        std::function<void(int)>            button; // linux button code
        std::function<void()>               leave;
        std::function<void(int)>            scroll; // +1 down / -1 up
        std::function<void(int)>            release; // button release
    };
    void register_surface(wl_surface*, SurfaceHooks);
    void unregister_surface(wl_surface*);

    // Accessors for popup construction
    wl_compositor* compositor() const { return compositor_; }
    wl_shm*        shm() const { return shm_; }
    wl_seat*       seat() const { return seat_; }
    xdg_wm_base*   wm_base() const { return wm_base_; }
    zwlr_layer_surface_v1* layer_surface() const;
    zwlr_layer_shell_v1*   layer_shell() const { return layer_shell_; }
    struct wp_viewporter* viewporter() const { return viewporter_; }
    struct wp_fractional_scale_manager_v1* frac_mgr() const {
        return frac_mgr_;
    }
    struct zwp_idle_inhibit_manager_v1* idle_inhibit_manager() const {
        return idle_mgr_;
    }
    wl_surface* bar_surface() const;
    uint32_t last_button_serial() const { return last_button_serial_; }
    double   pointer_x() const;
    // pointer coord along bar main axis (x horizontal, y vertical)
    double   pointer_along() const;

    // Register module under its layout id; placement from cfg.
    void add_module(const std::string& id, Module* m);
    // Re-derive left/center/right from cfg.layout_* (on config apply).
    void rebuild_layout();

    std::vector<Module*> left, center, right; // derived from cfg layout

private:
    // Wayland globals
    wl_display*    display_    = nullptr;
    wl_registry*   registry_   = nullptr;
    wl_compositor* compositor_ = nullptr;
    wl_shm*        shm_        = nullptr;
    wl_seat*       seat_       = nullptr;
    wl_pointer*    pointer_    = nullptr;
    xdg_wm_base*   wm_base_    = nullptr;
    zwlr_layer_shell_v1* layer_shell_ = nullptr;

    std::vector<BarSurface*> surfaces_;
    BarSurface*              primary_ = nullptr; // tray/settings/popups host
    BarSurface*              cur_     = nullptr; // draw / input context
    bool                     outputs_dirty_ = false;
    BarSurface* surface_for(wl_surface*) const;

    // cursor
    wl_cursor_theme* cursor_theme_   = nullptr;
    wl_surface*      cursor_surface_ = nullptr;
    wl_cursor*       cursor_         = nullptr;
    int              cursor_scale_   = 1;
    // cursor-shape-v1: compositor draws cursor from shape enum (no theme load / buffers). wl_cursor theme is fallback only.
    struct wp_cursor_shape_manager_v1* cursor_shape_mgr_ = nullptr;
    struct wp_viewporter* viewporter_ = nullptr;
    struct wp_fractional_scale_manager_v1* frac_mgr_ = nullptr;
    struct wp_cursor_shape_device_v1*  cursor_shape_dev_ = nullptr;

    // pin/hold are global: pin one bar pins all; open menu holds all open
    bool pinned_     = false;
    int  hold_       = 0;             // >0: don't auto-hide (menus etc.)
    bool running_    = true;
    wl_surface* ptr_surface_ = nullptr; // which of our surfaces has pointer
    uint32_t last_button_serial_ = 0;

    struct zwp_idle_inhibit_manager_v1* idle_mgr_ = nullptr;
    int epoll_fd_ = -1;
    int tick_fd_  = -1;   // periodic module refresh (armed only when visible)
    std::map<int, std::function<void(uint32_t)>> fd_cbs_;
    // MATTBAR_DEBUG wakeup profiler: attribute every epoll wakeup to its fd
    std::map<int, std::string> fd_labels_;
    std::map<std::string, uint64_t> wake_counts_;
    uint64_t wl_event_count_ = 0, draw_count_ = 0;
    long prof_last_ms_ = 0;
    void profiler_note_wake(int fd, int wl_fd);
    void profiler_report();
    std::map<wl_surface*, SurfaceHooks> extra_surfaces_;

    std::vector<HitRect> hits_;
    std::map<std::string, Module*> modules_; // owns all modules

    struct BarOutput {
        wl_output*  wl   = nullptr;
        std::string name; // "DP-2" etc. (wl_output v4)
        uint32_t    reg  = 0; // registry name, for global_remove
        int32_t     scale = 1; // integer output scale (HiDPI)
        Bar*        bar  = nullptr;
    };
    std::vector<BarOutput*> outputs_;
    wl_output* pick_output() const;
    SettingsWindow* settings_ = nullptr;
    bool settings_close_pending_ = false;

    // Omarchy theme following: inotify on .../omarchy/current so theme swap re-colors the bar immediately.
    void setup_omarchy_theme_watch();
    void on_omarchy_theme_event();
    int omarchy_inotify_fd_ = -1;

    // internals
    void arm_tick(bool arm);
    void update_tick();   // tick runs while ANY bar is revealed
    void tick_modules();
    void set_cursor(wl_pointer*, uint32_t serial);
    void route_click(int button);
    std::function<void(Module*)> click_observer_;
    uint64_t wd_usec_ = 0, last_wd_ping_ = 0;
    int      exit_code_ = 0;
    void route_scroll(int dir);
    void flush_wayland();
    bool any_expanded() const;
    bool any_pointer_inside() const;
public:
    // reveal_all_monitors: hovering one bar brings up every bar
    void reveal_all();
    // mattbarctl: momentary show/hide of every bar (normal timers resume)
    void ctl_reveal();
    void ctl_hide();
private:

    friend struct BarSurface;

    // Wayland listener trampolines
    static void on_global(void*, wl_registry*, uint32_t, const char*, uint32_t);
    static void on_global_remove(void*, wl_registry*, uint32_t);
    static void on_ptr_enter(void*, wl_pointer*, uint32_t, wl_surface*, wl_fixed_t, wl_fixed_t);
    static void on_ptr_leave(void*, wl_pointer*, uint32_t, wl_surface*);
    static void on_ptr_motion(void*, wl_pointer*, uint32_t, wl_fixed_t, wl_fixed_t);
    static void on_ptr_button(void*, wl_pointer*, uint32_t, uint32_t, uint32_t, uint32_t);
    static void on_ptr_axis(void*, wl_pointer*, uint32_t, uint32_t, wl_fixed_t);
    static void on_seat_caps(void*, wl_seat*, uint32_t);
};

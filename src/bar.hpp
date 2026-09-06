#pragma once
#include <cstdint>
#include <csignal>
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

// One region of the drawn bar that belongs to a module (for hit testing).
struct HitRect {
    double x, w;
    Module* mod;
};

// ---------------------------------------------------------------------------
// One bar, on one output. Everything that is genuinely per-monitor lives
// here — the layer surface, its size, whether it is currently revealed, its
// own hide/reveal timers and its own hit rects — so two bars hide and
// reveal independently of each other. Everything shared (Wayland globals,
// the epoll loop, the modules themselves, config) stays in Bar.
// ---------------------------------------------------------------------------
struct wp_viewporter;
struct wp_fractional_scale_manager_v1;
struct ext_session_lock_manager_v1;
struct ext_idle_notifier_v1;
#include "frac.hpp"

struct BarSurface {
    Bar*        owner = nullptr;
    wl_output*  out   = nullptr;  // nullptr = compositor picks (single-bar)
    std::string name;             // "DP-2"; empty when the compositor picked

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

    // HiDPI: all geometry above stays LOGICAL; only the pixel buffer is
    // scaled. For a compositor-picked surface (out == nullptr) the entered
    // output, reported by wl_surface.enter, tells us the scale.
    wl_output* entered_out = nullptr;
    int        drawn_scale = 1; // scale of the last committed buffer
    int        scale() const;
    // Fractional scaling: when the compositor prefers a non-integer scale
    // for this surface, frac renders the buffer at exactly that scale and
    // a viewport maps it to the logical size — no downsampling. Geometry
    // stays logical either way. Inactive => the integer path above.
    FracSurface frac;

    int hide_fd = -1, reveal_fd = -1;

    std::vector<HitRect> hits;

    void   create(Bar& b, wl_output* o, const std::string& n);
    void   destroy();
    void   set_expanded(bool on);
    void   apply_geometry();      // re-push size/anchors after a config change
    void   draw();
    void   arm_hide(bool arm);
    void   arm_reveal(bool arm);
    double along() const { return owner_vertical() ? ptr_y : ptr_x; }
    Module* hit(double along_px, double* relx);
    double  slot_along(Module* m) const; // where m sits on THIS bar, or -1

private:
    bool owner_vertical() const;
    int  hidden_thickness() const;

    static void on_configure(void*, zwlr_layer_surface_v1*, uint32_t,
                             uint32_t, uint32_t);
    static void on_closed(void*, zwlr_layer_surface_v1*);
};

class Bar {
public:
    // A single module may observe all bar clicks (receives the hit
    // module, or nullptr for dead space) — used by the agents dropdown
    // to dismiss on interaction elsewhere on the bar.
    void set_click_observer(std::function<void(Module*)> f) {
        click_observer_ = std::move(f);
    }
    // Send WATCHDOG=1 now. Long-running module work (e.g. sequential
    // compositor queries) must call this between steps so a slow
    // compositor cannot starve the liveness pings.
    void ping_watchdog();
    int  exit_code() const { return exit_code_; }
    bool init();
    void run();
    void shutdown();
    // Signal-safe: drop out of the event loop so destructors, pactl, and
    // the lock client can tear down. `_exit` from SIGTERM left orphan
    // children and a locked session with no password field.
    void request_stop();

    // --- services for modules -------------------------------------------
    void request_draw(); // marks every bar surface dirty
    // Register an fd with the main epoll loop; cb receives epoll events mask.
    void add_fd(int fd, std::function<void(uint32_t)> cb,
                const char* label = "module");
    void remove_fd(int fd);
    void mod_fd(int fd, uint32_t events);
    // During a draw or a pointer event, "the bar" means the surface that is
    // being drawn / was clicked; outside that context it means "any bar".
    bool expanded() const;

    // --- multi-monitor ---------------------------------------------------
    // The surface currently being drawn or handling input, and its output.
    // Modules use these to render per-monitor content (workspaces) and to
    // put their popups on the monitor the user actually clicked.
    BarSurface* current() const { return cur_ ? cur_ : primary_; }
    std::string current_output_name() const;
    wl_output*  current_output() const;
    // Output of the bar that is handling this pointer/key event, or
    // nullptr if we are not inside an input callback (hotkeys, IPC).
    wl_output*  input_output() const;
    wl_output*  output_named(const std::string& name) const;
    // Hyprland's focused monitor, matched to a known wl_output.
    // Fed by socket2 `focusedmon` / j/activeworkspace — never forks hyprctl.
    wl_output*  focused_output() const;
    void        note_focused_output(const std::string& name);
    wl_output*  primary_output() const;
    // Name of the bar actually elected primary right now (the settings
    // window displays this rather than re-deriving the election rule).
    std::string primary_name() const;
    bool        current_is_primary() const;
    // Where a module's slot starts on the current bar (-1 if not drawn).
    double      slot_along(Module* m) const;
    // Length of the current bar along its main axis. The bar spans the
    // whole output edge, so this IS the screen width (or height, vertical)
    // in logical pixels — what popups need to stay on screen.
    double      along_length() const;
    // Integer buffer scale of an output (1 when unknown). Popups render at
    // this scale so they are as crisp as the bar on HiDPI displays.
    int         scale_of(wl_output*) const;
    // Reconcile bar surfaces with the outputs the config asks for. Safe to
    // call at any time: monitor hotplug and settings changes both land here.
    void        sync_surfaces();
    std::vector<std::string> output_names() const;
    struct OutputRef {
        wl_output*  wl = nullptr;
        std::string name;
        int         scale = 1;
        int         logical_w = 0, logical_h = 0;
    };
    std::vector<OutputRef> output_list() const;

    // Pinning: keep the bar revealed until unpinned. Never touches the
    // exclusive zone, so windows are unaffected either way.
    void toggle_pinned();
    bool pinned() const { return pinned_; }

    // Hold: modules (e.g. an open tray menu) can temporarily prevent
    // auto-hiding without pinning. Balanced acquire/release.
    void hold_open(bool acquire);
    // Plugin-row hover family: the QS strip sits inward of this bar.
    // Hovering it must reveal/keep MattBar the same way ptr_inside does.
    void set_plugin_row_hover(bool on);
    bool plugin_row_hover() const { return plugin_row_hover_; }

    // Settings window lifecycle (gear icon in the tray)
    void toggle_settings();
    void open_settings();        // spawn if closed; same window as the gear
    bool settings_open() const;
    void close_settings_later(); // safe to call from settings' own callbacks
    void refresh_settings();     // redraw if the window is open
    // Re-apply cfg-derived surface geometry after a settings change.
    void apply_config();
    void update_tick();   // arm module tick while revealed or lazy plugin session

    // One key event, already translated through xkbcommon. keysym is an
    // XKB_KEY_* value (Escape = 0xff1b) so popups need not include
    // xkbcommon themselves.
    struct KeyEvent {
        uint32_t    keysym  = 0;
        uint32_t    keycode = 0; // evdev + 8, as Wayland delivers it
        std::string utf8;
        uint32_t    mods    = 0; // KEY_MOD_*
        bool        pressed = false;
        bool escape() const { return pressed && keysym == 0xff1b; }
        bool enter() const {
            return pressed && (keysym == 0xff0d || keysym == 0xff8d);
        }
        bool backspace() const { return pressed && keysym == 0xff08; }
        bool tab() const { return pressed && keysym == 0xff09; }
        bool up() const { return pressed && keysym == 0xff52; }
        bool down() const { return pressed && keysym == 0xff54; }
        bool left() const { return pressed && keysym == 0xff51; }
        bool right() const { return pressed && keysym == 0xff53; }
        bool ctrl() const { return mods & 2; }
        bool shift() const { return mods & 1; }
    };
    static constexpr uint32_t KEY_MOD_SHIFT = 1;
    static constexpr uint32_t KEY_MOD_CTRL  = 2;
    static constexpr uint32_t KEY_MOD_ALT   = 4;
    static constexpr uint32_t KEY_MOD_SUPER = 8;

    // Auxiliary surfaces (popups): receive pointer (and optional key)
    // events routed by the bar.
    struct SurfaceHooks {
        std::function<void(double, double)> motion; // surface-local coords
        std::function<void(int)>            button; // linux button code
        std::function<void()>               leave;
        std::function<void(int)>            scroll; // +1 down / -1 up
        std::function<void(int)>            release; // button release
        std::function<void(const KeyEvent&)> key;
    };
    void register_surface(wl_surface*, SurfaceHooks);
    void unregister_surface(wl_surface*);
    // 0=none, 1=exclusive, 2=on_demand (falls back to exclusive if the
    // compositor's layer-shell is older than v4).
    uint32_t popup_kb_mode() const;

    // Accessors for popup construction
    wl_display*    display() const { return display_; }
    wl_compositor* compositor() const { return compositor_; }
    wl_shm*        shm() const { return shm_; }
    wl_seat*       seat() const { return seat_; }
    xdg_wm_base*   wm_base() const { return wm_base_; }
    zwlr_layer_surface_v1* layer_surface() const;
    zwlr_layer_shell_v1*   layer_shell() const { return layer_shell_; }
    uint32_t               layer_shell_version() const {
        return layer_shell_ver_;
    }
    struct wp_viewporter* viewporter() const { return viewporter_; }
    struct wp_fractional_scale_manager_v1* frac_mgr() const {
        return frac_mgr_;
    }
    struct zwp_idle_inhibit_manager_v1* idle_inhibit_manager() const {
        return idle_mgr_;
    }
    struct ext_session_lock_manager_v1* lock_mgr() const { return lock_mgr_; }
    struct ext_idle_notifier_v1* idle_notifier() const { return idle_notif_; }
    wl_surface* bar_surface() const;
    uint32_t last_button_serial() const { return last_button_serial_; }
    double   pointer_x() const;
    // pointer coordinate along the bar's main axis (x horizontal, y vertical)
    double   pointer_along() const;

    // Register a module under its layout id; placement comes from cfg.
    void add_module(const std::string& id, Module* m);
    // Re-derive left/center/right/more from cfg.layout_* (called on config apply).
    void rebuild_layout();

    std::vector<Module*> left, center, right; // on-bar zones
    std::vector<Module*> more;                // overflow group (zone 3)

private:
    // Wayland globals
    wl_display*    display_    = nullptr;
    wl_registry*   registry_   = nullptr;
    wl_compositor* compositor_ = nullptr;
    wl_shm*        shm_        = nullptr;
    wl_seat*       seat_       = nullptr;
    wl_pointer*    pointer_    = nullptr;
    wl_keyboard*   keyboard_   = nullptr;
    xdg_wm_base*   wm_base_    = nullptr;
    zwlr_layer_shell_v1* layer_shell_ = nullptr;
    uint32_t             layer_shell_ver_ = 1;

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
    // cursor-shape-v1: the compositor draws the cursor from a shape enum —
    // no theme load, no cursor buffers in this process at all. The
    // wl_cursor theme path below survives only as the fallback.
    struct wp_cursor_shape_manager_v1* cursor_shape_mgr_ = nullptr;
    struct wp_viewporter* viewporter_ = nullptr;
    struct wp_fractional_scale_manager_v1* frac_mgr_ = nullptr;
    struct wp_cursor_shape_device_v1*  cursor_shape_dev_ = nullptr;

    // state (pin and hold are deliberately global: pinning one bar pins
    // them all, and an open menu holds every bar open)
    bool pinned_     = false;
    int  hold_       = 0;             // >0: don't auto-hide (menus etc.)
    bool plugin_row_hover_ = false;   // QS plugin-bar pointer is inside
    volatile sig_atomic_t running_ = 1;
    wl_surface* ptr_surface_ = nullptr; // which of our surfaces has pointer
    wl_surface* kb_surface_  = nullptr; // which of our surfaces has keyboard
    uint32_t last_button_serial_ = 0;

    struct xkb_context* xkb_ctx_    = nullptr;
    struct xkb_keymap*  xkb_keymap_ = nullptr;
    struct xkb_state*   xkb_state_  = nullptr;
    int     kb_repeat_fd_     = -1;
    int32_t kb_repeat_rate_   = 25;  // Hz
    int32_t kb_repeat_delay_  = 600; // ms
    uint32_t kb_repeat_code_  = 0;
    KeyEvent kb_repeat_ev_{};

    struct zwp_idle_inhibit_manager_v1* idle_mgr_ = nullptr;
    struct ext_session_lock_manager_v1* lock_mgr_ = nullptr;
    struct ext_idle_notifier_v1*        idle_notif_ = nullptr;
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
        int32_t     px_w = 0, px_h = 0; // current mode, compositor pixels
        Bar*        bar  = nullptr;
    };
    std::vector<BarOutput*> outputs_;
    wl_output* pick_output() const;
    SettingsWindow* settings_ = nullptr;
    bool settings_close_pending_ = false;

    // Omarchy theme following: inotify watch on .../omarchy/current so a
    // theme swap re-colors the bar immediately (Omarchy restarts Waybar on
    // theme change rather than signaling it, so a signal can't be relied on).
    void setup_omarchy_theme_watch();
    void on_omarchy_theme_event();
    int omarchy_inotify_fd_ = -1;

    // internals
    void arm_tick(bool arm);
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

    std::string focused_mon_;

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
    static void on_kb_keymap(void*, wl_keyboard*, uint32_t, int32_t, uint32_t);
    static void on_kb_enter(void*, wl_keyboard*, uint32_t, wl_surface*,
                            wl_array*);
    static void on_kb_leave(void*, wl_keyboard*, uint32_t, wl_surface*);
    static void on_kb_key(void*, wl_keyboard*, uint32_t, uint32_t, uint32_t,
                          uint32_t);
    static void on_kb_mods(void*, wl_keyboard*, uint32_t, uint32_t, uint32_t,
                           uint32_t, uint32_t);
    static void on_kb_repeat(void*, wl_keyboard*, int32_t, int32_t);
    void        deliver_key(const KeyEvent&);
    void        arm_key_repeat(bool on);
};

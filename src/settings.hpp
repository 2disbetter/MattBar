#pragma once
#include "shm.hpp"
#include "frac.hpp"
// In-bar settings window: a centered overlay layer surface with click-only
// widgets (steppers, checkboxes, click-to-set sliders). Changes apply live;
// "Save" persists them to ~/.config/mattbar/mattbar.conf.
#include <wayland-client.h>
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

#include "ui.hpp"
#include <functional>
#include <vector>
#include <cstdint>

class Bar;

class SettingsWindow {
public:
    explicit SettingsWindow(Bar& bar);
    ~SettingsWindow();
    SettingsWindow(const SettingsWindow&) = delete;
    SettingsWindow& operator=(const SettingsWindow&) = delete;
    void refresh() { draw(); } // e.g. after an Omarchy theme swap

private:
    struct Widget {
        double x, y, w, h;
        std::function<void()>       click;    // buttons / checkboxes
        std::function<void(double)> set_frac; // sliders: 0..1 from click x
        std::function<void(int)>    scroll;   // mouse wheel over this widget
    };

    void draw();
    void on_motion(double x, double y);
    void on_button(int button);
    void on_scroll(int dir);
    void on_key(const Bar::KeyEvent& e);
    void apply(); // push live changes into the bar + redraw
    void commit_fields();
    TextField* editing();
    void set_kb_on_demand(bool on);

    static void on_configure(void*, zwlr_layer_surface_v1*, uint32_t,
                             uint32_t, uint32_t);
    static void on_closed(void*, zwlr_layer_surface_v1*);

    Bar& bar_;
    wl_surface* surf_ = nullptr;
    FracSurface frac_; // fractional scaling (see frac.hpp)
    ShmPool     pool_; // reused buffers (see shm.hpp)
    FrameGate   gate_; // one commit per shown frame
    zwlr_layer_surface_v1* ls_ = nullptr;
    int  w_ = 500, h_ = 520;
    bool mapped_ = false;
    double mx_ = -1, my_ = -1;
    double scroll_ = 0, scroll_max_ = 0;
    int color_sel_ = 0; // index into the Colors section's swatch table
    int tab_ = 0;       // TabBar / Layout / Modules / Notify / Look / Shell
    int shell_tab_ = 0; // Idle / Menu / Panels / Pickers / Desktop / Plugins
    double woff_ = 0; // content-y -> surface-y offset during widget capture
    std::vector<Widget> widgets_;
    int       edit_ = -1; // focused Local LLM text field, or -1
    TextField f_url_, f_chat_, f_start_;
    // Bar > About: redraw when the binary on disk is replaced while the
    // window is open (inotify on its directory; no polling).
    int exe_watch_fd_  = -1;
    int exe_state_shown_ = -1; // ExeState last drawn, -1 = not drawn
};

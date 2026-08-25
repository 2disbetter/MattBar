#pragma once
#include "frac.hpp"
// In-bar settings window: a centered overlay layer surface with click-only
// widgets (steppers, checkboxes, click-to-set sliders). Changes apply live;
// "Save" persists them to ~/.config/mattbar/mattbar.conf.
#include <wayland-client.h>
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

#include <functional>
#include <vector>

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
    };

    void draw();
    void on_motion(double x, double y);
    void on_button(int button);
    void on_scroll(int dir);
    void apply(); // push live changes into the bar + redraw

    static void on_configure(void*, zwlr_layer_surface_v1*, uint32_t,
                             uint32_t, uint32_t);
    static void on_closed(void*, zwlr_layer_surface_v1*);

    Bar& bar_;
    wl_surface* surf_ = nullptr;
    FracSurface frac_; // fractional scaling (see frac.hpp)
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
};

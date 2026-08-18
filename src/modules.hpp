#pragma once
#include <cairo/cairo.h>

class Bar;

// Bar widget. Bar calls width() then draw() each frame (cr has bar font). x is left edge of the module's slot.
class Module {
public:
    virtual ~Module() = default;

    virtual void init(Bar&) {}
    virtual void tick() {}  // every TICK_MS; call bar->request_draw() if changed

    virtual double width(cairo_t* cr) = 0;
    virtual void   draw(cairo_t* cr, double x, double h) = 0;

    // relx relative to module left edge. Return true if handled.
    virtual bool on_click(double relx, int button) { (void)relx; (void)button; return false; }
    virtual bool on_scroll(double relx, int dir)   { (void)relx; (void)dir;    return false; }
    virtual void on_hover(double relx)             { (void)relx; }

    // enabled()==false modules are skipped (layout, hits, tick).
    virtual bool enabled() const { return true; }

    // primary_only: draw only on primary bar (e.g. tray — one StatusNotifier host).
    virtual bool primary_only() const { return false; }
};

// Factories (modules.cpp / tray.cpp)
Module* make_clock();
Module* make_workspaces();
Module* make_battery();
Module* make_network();
Module* make_volume();
Module* make_bluetooth();
Module* make_agents();
Module* make_microphone();
Module* make_screenrecord();
Module* make_brightness();
// slider popup painter (for headless render tests)
void draw_brightness_slider(cairo_t* cr, int w, int h, double frac,
                            const char* label);
// vector sun icon (brightness); for headless render tests.
#include "config.hpp"
void draw_sun_icon(cairo_t* cr, double cx, double cy, double size,
                   const Color& c);
bool brightness_vector_icon();
Module* make_media();
Module* make_caffeine();
Module* make_tray();
Module* make_pin();
Module* make_omarchy_button();
Module* make_update_button();
Module* make_temp();
// Notification centre: bell + history popup.
void calendar_toggle(Bar& bar, Module* owner);
Module* make_notifications();
// power-profiles-daemon selector; hides when daemon absent.
Module* make_power();
// mattbarctl hooks into power module (empty/false when PPD absent)
#include <string>
#include <vector>
// fire-and-forget process launch (double-fork; used by click commands)
void spawn_detached(const std::string& cmd);

#include <functional>
class Bar;
// Non-blocking shell command; callback fires with stdout on completion (or timeout kill).
class AsyncCmd {
public:
    using Done = std::function<void(const std::string& output, int status)>;
    void run(Bar& bar, const std::string& cmd, Done cb,
             int timeout_ms = 2000);
    bool running() const { return pid_ > 0; }
    ~AsyncCmd();

private:
    void start(const std::string& cmd);
    void finish(int status);
    Bar*        bar_ = nullptr;
    int         fd_ = -1, timer_fd_ = -1;
    pid_t       pid_ = -1;
    std::string buf_, pending_cmd_;
    bool        pending_ = false;
    Done        cb_;
    int         timeout_ms_ = 2000;
};
std::string              power_ctl_active();
std::vector<std::string> power_ctl_profiles();
bool                     power_ctl_set(const std::string&);

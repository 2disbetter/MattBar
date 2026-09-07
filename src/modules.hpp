#pragma once
#include <cairo/cairo.h>

class Bar;

// A bar widget. Bar calls width() then draw() each frame (cr already has the
// bar font selected). x is the left edge of the slot the module was given.
class Module {
public:
    virtual ~Module() = default;

    virtual void init(Bar&) {}
    virtual void tick() {}  // called every TICK_MS; call bar->request_draw() if changed

    virtual double width(cairo_t* cr) = 0;
    virtual void   draw(cairo_t* cr, double x, double h) = 0;

    // relx is pointer x relative to the module's left edge. Return true if handled.
    virtual bool on_click(double relx, int button) { (void)relx; (void)button; return false; }
    virtual bool on_scroll(double relx, int dir)   { (void)relx; (void)dir;    return false; }
    virtual void on_hover(double relx)             { (void)relx; }

    // Modules with enabled()==false are skipped entirely (layout, hits, tick).
    virtual bool enabled() const { return true; }

    // Singleton modules draw on the primary bar only. The tray is one: a
    // second StatusNotifier host on another monitor would fight the first
    // for the same apps.
    virtual bool primary_only() const { return false; }
};

// Factories (defined in modules.cpp / tray.cpp)
Module* make_clock();
Module* make_workspaces();
Module* make_battery();
Module* make_network();
Module* make_volume();
Module* make_bluetooth();
Module* make_agents();
// Same action as left-clicking the agents chip (grok session popup, not
// the Claude/Codex usage dashboard). Used by `mattbarctl agents toggle`.
void    agents_hotkey();
void    agents_pick();
Module* make_microphone();
Module* make_screenrecord();
Module* make_brightness();
Module* make_display();
// slider popup painter, exposed for headless render tests
void draw_brightness_slider(cairo_t* cr, int w, int h, double frac,
                            const char* label);
// vector sun icon (brightness); exposed for headless render tests.
// (Color lives in config.hpp, which every consumer already includes.)
#include "config.hpp"
void draw_sun_icon(cairo_t* cr, double cx, double cy, double size,
                   const Color& c);
bool brightness_vector_icon();
Module* make_media();
Module* make_caffeine();
Module* make_nightlight();
Module* make_tray();
Module* make_pin();
Module* make_more();
void    more_close();
bool    more_is_open();
Module* make_omarchy_button();
Module* make_update_button();
// `omarchy-shell omarchy.system-update refresh|clear` (omarchy-update-status).
void    update_refresh();
void    update_clear();
Module* make_temp();
// Notification centre: bell + history popup (last cfg.history_max entries,
// clear-all, per-app mute). Deliberately quiet — see cfg.bell_show_count.
// The clock's month popup, implemented in panels.cpp.
void calendar_toggle(Bar& bar, Module* owner);
void calendar_open(Bar& bar, Module* owner);
void calendar_close();
bool calendar_is_open();
Module* make_notifications();
// power-profiles-daemon selector; hides itself when the daemon is absent.
Module* make_power();
Module* make_active_window();
Module* make_kblayout();
Module* make_reminder();
Module* make_dictation();
Module* make_tailscale();
Module* make_dropbox();
void    media_source_switch();
void    clock_cycle_format();
// mattbarctl hooks into the power module (empty/false when PPD is absent)
#include <string>
#include <vector>
// fire-and-forget process launch (double-fork; used by click commands)
void spawn_detached(const std::string& cmd);
// Wrap a shell command so the current power-profiles-daemon profile is
// saved for both AC and battery, then restored after the command. The
// Omarchy shell reapplies ac/battery on start; a missing battery file
// defaults to balanced and clobbers power-saver.
std::string with_preserved_power_profile(const std::string& cmd);
void persist_power_profile(const std::string& profile);
// When Quickshell is shut down, rewrite stock omarchy-shell / launch
// wrappers so bar clicks open MattBar's overlay instead of a dead qs.
std::string live_panel_click(const std::string& stored, const char* overlay_id);

#include <functional>
class Bar;
// Run a shell command WITHOUT blocking the event loop; the callback fires
// with its stdout when it completes (or with what it produced when the
// timeout kills it). Requests while a run is in flight coalesce into one
// follow-up run with the latest command — a burst of change events costs
// at most two subprocesses, and the bar keeps painting throughout.
//
// This exists because blocking popen chains froze the whole bar for ~7 s
// during Bluetooth reconnect storms (three bluetoothd restarts in a row,
// each spraying sink add/remove events, each event serially running up to
// 2.4 s of wpctl/pactl). Nothing audio-shaped may block the loop again.
class AsyncCmd {
public:
    using Done = std::function<void(const std::string& output, int status)>;
    using Line = std::function<void(const std::string& line)>;
    void run(Bar& bar, const std::string& cmd, Done cb,
             int timeout_ms = 2000, Line on_line = {});
    bool running() const { return pid_ > 0; }
    void cancel(); // SIGTERM; Done still fires
    ~AsyncCmd();

private:
    void start(const std::string& cmd);
    void finish(int status);
    void drain_lines();
    Bar*        bar_ = nullptr;
    int         fd_ = -1, timer_fd_ = -1;
    pid_t       pid_ = -1;
    std::string buf_, pending_cmd_;
    bool        pending_ = false;
    Done        cb_;
    Line        on_line_;
    size_t      line_pos_ = 0;
    int         timeout_ms_ = 2000;
};
std::string              power_ctl_active();
std::vector<std::string> power_ctl_profiles();
bool                     power_ctl_set(const std::string&);

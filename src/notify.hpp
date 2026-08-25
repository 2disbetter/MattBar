#pragma once
#include <cairo/cairo.h>
// ---------------------------------------------------------------------------
// Notification daemon (org.freedesktop.Notifications) + OSD, both opt-in
// (cfg.enable_notifications / cfg.enable_osd, default off so the incumbent
// daemon — mako on Omarchy 3.x, the shell on quattro — is never fought).
// When enabled, the name is requested with REPLACE+QUEUE. Takeover from
// mako/dunst is SIGTERM; from the Omarchy shell it is plugin IPC
// (never kill quickshell) unless cfg.quickshell_shutdown is on, in
// which case the whole Quickshell instance is stopped. The daemon stays
// queued behind any later owner, which doubles as failover.
// Control surface (bindable like Waybar/mako):
//   SIGRTMIN+2 dismiss last   SIGRTMIN+3 dismiss all   SIGRTMIN+4 invoke last
//   SIGRTMIN+5 toggle do-not-disturb                   SIGRTMIN+6 restore last
// ---------------------------------------------------------------------------
#include <cstdint>
#include <string>
#include <vector>

class Bar;

// One entry as the notification centre sees it: currently-showing popups
// first (active=true), then dismissed/expired ones, newest first.
struct NoteRecord {
    uint32_t    id = 0;
    std::string app, summary, body;
    int         urgency = 1;
    uint64_t    age_s   = 0; // seconds since it arrived
    bool        active  = false;
    // Default action, when the notification carries one and is still
    // active (the app is still listening for ActionInvoked). The centre
    // shows the label and left-click fires it.
    bool        has_action = false;
    std::string action_label;
    // Borrowed from the live/history Note; valid until the next
    // history()/dismiss. Null when the sender gave no image.
    cairo_surface_t* icon = nullptr;
};

// Circular (or rounded) notification avatar. size is logical px.
void draw_note_avatar(cairo_t* cr, cairo_surface_t* icon, double x, double y,
                      double size);

class NotifyDaemon {
public:
    NotifyDaemon();
    ~NotifyDaemon();
    void init(Bar&);
    void apply_enabled(); // reconcile running state with the cfg toggles

    void dismiss_last();
    void dismiss_all();
    void invoke_last();
    void restore_last();
    void toggle_dnd();

    // Internal notification source (e.g. battery alerts).
    void post(const std::string& summary, const std::string& body,
              int urgency);
    void osd_show(const std::string& label, double frac, bool muted);

    bool owns_name() const;
    // Drop or restore toast surfaces when the panel sleeps / wakes.
    void refresh_popups();

    // --- notification centre (the bell module) ---------------------------
    // Newest first, capped at cfg.history_max. Empty when the daemon is not
    // the one owning org.freedesktop.Notifications.
    std::vector<NoteRecord> history() const;
    void                    clear_history();
    size_t                  history_count() const;
    // Fire a specific active notification's default action (the centre's
    // left-click). No-op for ids that are gone.
    void                    invoke(uint32_t id);
    bool                    dnd() const; // for the bell's indicator

    struct Impl;

private:
    Impl* im_;
};

NotifyDaemon* notify_daemon();

// Post an alert via the daemon when it owns the name, else via notify-send —
// so alerts work identically under mako or the Omarchy shell.
// Mixed-font text: runs the bar font can map are drawn with it, everything
// else (emoji) with cfg.notification_emoji_font. Exposed for render tests.
double rich_text_width(cairo_t* cr, const std::string& s, double size);
void   draw_rich_text(cairo_t* cr, const std::string& s, double x, double y,
                      double size);

void notify_post(const std::string& summary, const std::string& body,
                 int urgency);

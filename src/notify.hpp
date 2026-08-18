#pragma once
#include <cairo/cairo.h>
// Notification daemon (org.freedesktop.Notifications) + OSD, both opt-in (cfg.enable_notifications / cfg.enable_osd).
#include <cstdint>
#include <string>
#include <vector>

class Bar;

// History entry: active popups first, then dismissed/expired, newest first.
struct NoteRecord {
    uint32_t    id = 0;
    std::string app, summary, body;
    int         urgency = 1;
    uint64_t    age_s   = 0; // seconds since arrival
    bool        active  = false;
    // Default action if still active (app listening for ActionInvoked).
    bool        has_action = false;
    std::string action_label;
};

class NotifyDaemon {
public:
    NotifyDaemon();
    ~NotifyDaemon();
    void init(Bar&);
    void apply_enabled(); // reconcile with cfg toggles

    void dismiss_last();
    void dismiss_all();
    void invoke_last();
    void restore_last();
    void toggle_dnd();

    // Internal source (e.g. battery alerts).
    void post(const std::string& summary, const std::string& body,
              int urgency);
    void osd_show(const std::string& label, double frac, bool muted);

    bool owns_name() const;

    // --- notification centre (bell module) --------------------------- Newest first, capped at cfg.history_max.
    std::vector<NoteRecord> history() const;
    void                    clear_history();
    size_t                  history_count() const;
    // Fire active notification's default action (centre left-click).
    void                    invoke(uint32_t id);
    bool                    dnd() const; // bell indicator

    struct Impl;

private:
    Impl* im_;
};

NotifyDaemon* notify_daemon();

// Post via daemon if owns name, else notify-send (works under mako/shell).
double rich_text_width(cairo_t* cr, const std::string& s, double size);
void   draw_rich_text(cairo_t* cr, const std::string& s, double x, double y,
                      double size);

void notify_post(const std::string& summary, const std::string& body,
                 int urgency);

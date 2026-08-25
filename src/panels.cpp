// ---------------------------------------------------------------------------
// Pop-up panels: the clock's calendar, the notification centre behind the
// bell, and the power-profile selector.
//
// All three follow the same rules as the brightness slider: an overlay layer
// surface built from PopupWin, opened on the monitor whose bar was clicked,
// holding the bar open while it is up, and closing itself a moment after the
// pointer leaves (or on a second click of its module).
// ---------------------------------------------------------------------------
#include "modules.hpp"
#include "bar.hpp"
#include "config.hpp"
#include "notify.hpp"
#include "popup.hpp"
#include "sdpump.hpp"

#include <linux/input-event-codes.h>
#include <sys/timerfd.h>
#include <systemd/sd-bus.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

namespace {

bool dbgp() {
    static bool v = getenv("MATTBAR_DEBUG") != nullptr;
    return v;
}
#define PDBG(...)                                                              \
    do {                                                                       \
        if (dbgp()) {                                                          \
            fprintf(stderr, "mattbar: " __VA_ARGS__);                          \
            fputc('\n', stderr);                                               \
        }                                                                      \
    } while (0)

void col(cairo_t* cr, const Color& c, double a = -1) {
    cairo_set_source_rgba(cr, c.r, c.g, c.b, a < 0 ? c.a : a);
}

double tw(cairo_t* cr, const std::string& s) {
    cairo_text_extents_t e;
    cairo_text_extents(cr, s.c_str(), &e);
    return e.x_advance;
}

void say(cairo_t* cr, double x, double ymid, const std::string& s,
         const Color& c) {
    cairo_font_extents_t fe;
    cairo_font_extents(cr, &fe);
    col(cr, c, 1.0);
    cairo_move_to(cr, x, ymid + (fe.ascent - fe.descent) / 2.0);
    cairo_show_text(cr, s.c_str());
}

void rrect(cairo_t* cr, double x, double y, double w, double h, double r) {
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -M_PI_2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI_2);
    cairo_arc(cr, x + r, y + h - r, r, M_PI_2, M_PI);
    cairo_arc(cr, x + r, y + r, r, M_PI, 1.5 * M_PI);
    cairo_close_path(cr);
}

// Panel background: the bar's own background, opaque enough to read text on
// top of whatever window it covers, with a subtle border.
void panel_bg(cairo_t* cr, int w, int h) {
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    col(cr, cfg.c_bg, std::max(cfg.c_bg.a, 0.96));
    rrect(cr, 0.5, 0.5, w - 1, h - 1, 10);
    cairo_fill_preserve(cr);
    col(cr, cfg.c_ws_bg, 1.0);
    cairo_set_line_width(cr, 1);
    cairo_stroke(cr);
}

// Does the bar font actually map this glyph? Same check the bluetooth and
// brightness modules make before falling back to text.
bool glyph_mapped(cairo_t* cr, const std::string& t) {
    cairo_scaled_font_t* sf = cairo_get_scaled_font(cr);
    cairo_glyph_t*       g  = nullptr;
    int                  n  = 0;
    bool ok = cairo_scaled_font_text_to_glyphs(sf, 0, 0, t.c_str(),
                                               (int)t.size(), &g, &n, nullptr,
                                               nullptr, nullptr) ==
                  CAIRO_STATUS_SUCCESS &&
              n > 0;
    for (int i = 0; ok && i < n; ++i)
        if (g[i].index == 0) ok = false;
    if (g) cairo_glyph_free(g);
    return ok;
}

std::string utf8_trunc(const std::string& s, size_t maxchars) {
    size_t chars = 0, i = 0;
    while (i < s.size() && chars < maxchars) {
        unsigned char c = s[i];
        i += c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xe ? 3 : 4;
        ++chars;
    }
    if (i >= s.size()) return s;
    return s.substr(0, i) + "\u2026";
}

// A popup that closes itself shortly after the pointer leaves, and holds the
// bar open for as long as it exists. Shared by all three panels.
struct AutoPanel {
    PopupWin win;
    Bar*     bar      = nullptr;
    int      close_fd = -1;
    bool     holding  = false;

    void init(Bar& b, const char* label) {
        bar      = &b;
        win.kb_mode = 2;
        win.pkey    = [this](const Bar::KeyEvent& e) {
            if (e.escape()) close_now();
        };
        close_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
        b.add_fd(close_fd, [this](uint32_t) {
            uint64_t n;
            while (read(close_fd, &n, sizeof n) > 0) {}
            close_now();
        }, label);
    }
    bool open() const { return win.surf != nullptr; }
    void arm_close(int ms = 1200) {
        if (close_fd < 0) return;
        itimerspec ts{};
        ts.it_value.tv_sec  = ms / 1000;
        ts.it_value.tv_nsec = (ms % 1000) * 1000000L;
        timerfd_settime(close_fd, 0, &ts, nullptr);
    }
    void disarm_close() {
        if (close_fd < 0) return;
        itimerspec off{};
        timerfd_settime(close_fd, 0, &off, nullptr);
    }
    void hold(bool on) {
        if (on == holding || !bar) return;
        holding = on;
        bar->hold_open(on); // an open panel must not let the bar collapse
    }
    void close_now() {
        disarm_close();
        hold(false);
        win.destroy();
    }
};

// ---------------------------------------------------------------------------
// Calendar (left-click the clock)
// ---------------------------------------------------------------------------
constexpr int CAL_CELL = 30, CAL_HDR = 34, CAL_DOW = 22, CAL_PAD = 10;

struct Calendar {
    AutoPanel p;
    int       month_off = 0; // 0 = current month
    bool      ready     = false;

    int width() const {
        return CAL_PAD * 2 + 7 * CAL_CELL + (cfg.calendar_week_numbers ? 28 : 0);
    }
    int height() const {
        // Computed from the month itself, not from the last paint: the
        // surface has to be sized before anything is drawn into it, or a
        // 6-week month would be clipped on the frame it first appears.
        return CAL_HDR + CAL_DOW + weeks_now() * CAL_CELL + CAL_PAD;
    }
    int weeks_now() const {
        View v = view();
        int w = (v.lead + v.days + 6) / 7;
        return w < 4 ? 4 : w;
    }

    // The first of the displayed month, and how the grid is laid out.
    struct View {
        int year, mon;    // mon: 0-11
        int lead;         // blank cells before the 1st
        int days;         // days in month
        int today_cell;   // grid index of today, or -1
    };
    View view() const {
        time_t t = time(nullptr);
        tm     lt{};
        localtime_r(&t, &lt);
        int year = lt.tm_year + 1900, mon = lt.tm_mon;
        int today = lt.tm_mday;
        bool this_month = month_off == 0;
        mon += month_off;
        while (mon < 0) { mon += 12; --year; }
        while (mon > 11) { mon -= 12; ++year; }

        tm first{};
        first.tm_year = year - 1900;
        first.tm_mon  = mon;
        first.tm_mday = 1;
        first.tm_hour = 12; // avoid DST edges shifting the weekday
        time_t ft = mktime(&first);
        tm fl{};
        localtime_r(&ft, &fl);
        int wday = fl.tm_wday; // 0 = Sunday
        int lead = cfg.calendar_monday_first ? (wday + 6) % 7 : wday;

        static const int md[] = {31, 28, 31, 30, 31, 30,
                                 31, 31, 30, 31, 30, 31};
        int days = md[mon];
        if (mon == 1 &&
            ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0))
            days = 29;

        View v{year, mon, lead, days, -1};
        if (this_month) v.today_cell = lead + today - 1;
        return v;
    }

    void paint(cairo_t* cr) {
        const int W = width(), H = height();
        panel_bg(cr, W, H);
        View v = view();

        static const char* mn[] = {"January", "February", "March",
                                   "April",   "May",      "June",
                                   "July",    "August",   "September",
                                   "October", "November", "December"};
        std::string title =
            std::string(mn[v.mon]) + " " + std::to_string(v.year);

        // header: ‹  Month Year  ›   (title itself jumps back to today)
        double hy = CAL_HDR / 2.0 + 2;
        say(cr, CAL_PAD + 2, hy, "\u2039", cfg.c_dim);
        say(cr, W - CAL_PAD - 12, hy, "\u203a", cfg.c_dim);
        say(cr, (W - tw(cr, title)) / 2.0, hy, title,
            month_off == 0 ? cfg.c_fg : cfg.c_accent);

        const double gx = CAL_PAD + (cfg.calendar_week_numbers ? 28 : 0);
        // weekday header
        static const char* dow_mon[] = {"Mo", "Tu", "We", "Th",
                                        "Fr", "Sa", "Su"};
        static const char* dow_sun[] = {"Su", "Mo", "Tu", "We",
                                        "Th", "Fr", "Sa"};
        const char** dow = cfg.calendar_monday_first ? dow_mon : dow_sun;
        for (int i = 0; i < 7; ++i) {
            std::string d = dow[i];
            say(cr, gx + i * CAL_CELL + (CAL_CELL - tw(cr, d)) / 2.0,
                CAL_HDR + CAL_DOW / 2.0, d, cfg.c_dim);
        }

        // day grid
        const int weeks = weeks_now();
        for (int c = 0; c < weeks * 7; ++c) {
            int day = c - v.lead + 1;
            int r = c / 7, cc = c % 7;
            double cx = gx + cc * CAL_CELL;
            double cy = CAL_HDR + CAL_DOW + r * CAL_CELL;
            if (cfg.calendar_week_numbers && cc == 0) {
                // ISO-ish week number: good enough to orient by
                tm wt{};
                wt.tm_year = v.year - 1900;
                wt.tm_mon  = v.mon;
                wt.tm_mday = std::clamp(day, 1, v.days);
                wt.tm_hour = 12;
                time_t wtt = mktime(&wt);
                tm wl{};
                localtime_r(&wtt, &wl);
                char wk[8];
                strftime(wk, sizeof wk, "%V", &wl);
                say(cr, CAL_PAD + 2, cy + CAL_CELL / 2.0, wk, cfg.c_dim);
            }
            if (day < 1 || day > v.days) continue;
            std::string s = std::to_string(day);
            bool today = (c == v.today_cell);
            if (today) {
                col(cr, cfg.c_accent, 1.0);
                rrect(cr, cx + 2, cy + 2, CAL_CELL - 4, CAL_CELL - 4, 7);
                cairo_fill(cr);
            }
            say(cr, cx + (CAL_CELL - tw(cr, s)) / 2.0, cy + CAL_CELL / 2.0, s,
                today ? contrast_on(cfg.c_accent) : cfg.c_fg);
        }
    }

    void resize_and_draw() {
        // The grid can be 5 or 6 rows tall; re-request the size when it
        // changes so the panel never clips a week.
        p.win.ensure(*p.bar, last_place.anchor, last_place.mt, last_place.mr,
                     last_place.mb, last_place.ml, "mattbar-calendar", width(),
                     height(), last_out);
        p.win.draw();
    }

    PopupPlace last_place{};
    wl_output* last_out = nullptr;

    void open(Bar& bar, Module* owner) {
        double a = bar.slot_along(owner);
        if (a < 0) a = bar.pointer_along();
        last_place = popup_place(a, width(), bar.along_length());
        last_out   = bar.current_output();
        p.win.paint = [this](cairo_t* cr) { paint(cr); };
        p.win.click = [this](double x, double y, int btn) {
            p.disarm_close();
            if (btn == BTN_RIGHT) { month_off = 0; resize_and_draw(); return; }
            if (btn != BTN_LEFT) return;
            if (y < CAL_HDR) {
                if (x < 40) --month_off;
                else if (x > width() - 40) ++month_off;
                else month_off = 0; // tap the title to come back to today
                resize_and_draw();
            }
        };
        p.win.pscroll = [this](int d) {
            p.disarm_close();
            month_off += d > 0 ? 1 : -1;
            resize_and_draw();
        };
        p.win.pmotion = [this](double, double) { p.disarm_close(); };
        p.win.pleave  = [this] { p.arm_close(); };
        p.hold(true);
        resize_and_draw();
    }
};

Calendar& calendar() {
    static Calendar c;
    return c;
}

} // namespace

// Called by the clock module. Second click closes, as with every panel.
void calendar_toggle(Bar& bar, Module* owner) {
    if (calendar_is_open()) calendar_close();
    else calendar_open(bar, owner);
}
void calendar_open(Bar& bar, Module* owner) {
    Calendar& c = calendar();
    if (!c.ready) {
        c.p.init(bar, "calendar-close");
        c.ready = true;
    }
    if (c.p.open()) return;
    c.month_off = 0;
    c.open(bar, owner);
}
void calendar_close() { calendar().p.close_now(); }
bool calendar_is_open() { return calendar().p.open(); }

// ---------------------------------------------------------------------------
// Notification centre (the bell)
// ---------------------------------------------------------------------------
namespace {

constexpr int NC_W = 380, NC_HDR = 38, NC_ROW = 52, NC_FOOT = 26,
              NC_ROWS_MAX = 8;

class NotificationsModule : public Module {
public:
    bool enabled() const override { return cfg.show_notifications; }

    void init(Bar& bar) override {
        bar_ = &bar;
        panel_.init(bar, "notifications-close");
    }

    double width(cairo_t* cr) override {
        resolve_glyph(cr);
        std::string s = label();
        if (!cfg_vertical()) return tw(cr, s);
        cairo_font_extents_t fe;
        cairo_font_extents(cr, &fe);
        return fe.ascent + fe.descent + 10;
    }

    void draw(cairo_t* cr, double a, double t) override {
        std::string s = label();
        // The bell's only ambient signal is a slight dimming when there is
        // nothing to read. It never grows a badge unless you turn one on,
        // and it never pulses, flashes, or changes color to demand a look.
        // Do-not-disturb is the exception, because it is YOUR state, not
        // the notifications': a thin slash through the bell says "silenced"
        // at a glance. Still static, still quiet.
        auto* d   = notify_daemon();
        bool  dnd = d && d->dnd();
        const Color c = dnd ? cfg.c_dim
                        : (count() == 0 && cfg.bell_dim_when_empty)
                            ? cfg.c_dim
                            : cfg.c_fg;
        auto slash = [&](double cx, double cy) {
            if (!dnd) return;
            double r = 7.5;
            col(cr, cfg.c_urgent, 1.0);
            cairo_set_line_width(cr, 1.6);
            cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
            cairo_move_to(cr, cx - r, cy + r);
            cairo_line_to(cr, cx + r, cy - r);
            cairo_stroke(cr);
        };
        if (!cfg_vertical()) {
            say(cr, a, t / 2.0, s, c);
            slash(a + 7, t / 2.0);
        } else {
            cairo_font_extents_t fe;
            cairo_font_extents(cr, &fe);
            col(cr, c, 1.0);
            cairo_move_to(cr, (t - tw(cr, s)) / 2.0,
                          a + (fe.ascent + fe.descent + 10) / 2.0 +
                              (fe.ascent - fe.descent) / 2.0);
            cairo_show_text(cr, s.c_str());
            slash(t / 2.0, a + (fe.ascent + fe.descent + 10) / 2.0);
        }
    }

    bool on_click(double, int button) override {
        if (button == BTN_LEFT) {
            if (panel_.open()) { panel_.close_now(); return true; }
            open_panel();
            return true;
        }
        if (button == BTN_RIGHT) { // quick DND toggle, same as SIGRTMIN+5
            if (notify_daemon()) notify_daemon()->toggle_dnd();
            bar_->request_draw(); // the slash appears/disappears at once
            if (panel_.open()) redraw(); // header state line too
            return true;
        }
        return false;
    }

private:
    size_t count() const {
        auto* d = notify_daemon();
        return d ? d->history_count() : 0;
    }

    std::string label() const {
        std::string g = resolved_.empty() ? cfg.notify_glyph : resolved_;
        if (!cfg.bell_show_count) return g;
        size_t n = count();
        if (n == 0) return g;
        return g + " " + std::to_string(n);
    }

    void resolve_glyph(cairo_t* cr) {
        if (checked_ == cfg.font + cfg.notify_glyph) return;
        checked_        = cfg.font + cfg.notify_glyph;
        std::string pick = cfg.notify_glyph;
        if (!glyph_mapped(cr, pick)) pick = "\U000F009A"; // md-bell
        if (!glyph_mapped(cr, pick)) pick = "!";
        resolved_ = pick;
    }

    // ---- the panel -------------------------------------------------------
    int rows_shown() const {
        return std::min<int>(NC_ROWS_MAX, std::max<int>(1, (int)recs_.size()));
    }
    int panel_h() const { return NC_HDR + rows_shown() * NC_ROW + NC_FOOT; }

    void reload() {
        auto* d = notify_daemon();
        recs_   = d ? d->history() : std::vector<NoteRecord>();
        if (scroll_ > (int)recs_.size() - 1) scroll_ = 0;
    }

    static std::string ago(uint64_t s) {
        if (s < 60) return "now";
        if (s < 3600) return std::to_string(s / 60) + "m";
        if (s < 86400) return std::to_string(s / 3600) + "h";
        return std::to_string(s / 86400) + "d";
    }

    void paint(cairo_t* cr) {
        const int H = panel_h();
        panel_bg(cr, NC_W, H);

        say(cr, 12, NC_HDR / 2.0, "Notifications", cfg.c_fg);
        // "Clear all" sits top-right; harmless when there's nothing to clear
        std::string clear = "Clear all";
        double cw = tw(cr, clear) + 16;
        clear_x_ = NC_W - 12 - cw;
        col(cr, cfg.c_ws_bg, 1.0);
        rrect(cr, clear_x_, 8, cw, NC_HDR - 16, 6);
        cairo_fill(cr);
        say(cr, clear_x_ + 8, NC_HDR / 2.0, clear,
            recs_.empty() ? cfg.c_dim : cfg.c_fg);

        if (recs_.empty()) {
            auto* d = notify_daemon();
            std::string msg = (d && d->owns_name())
                                  ? "Nothing here"
                                  : "Notification daemon is off";
            say(cr, (NC_W - tw(cr, msg)) / 2.0, NC_HDR + NC_ROW / 2.0, msg,
                cfg.c_dim);
        }

        int shown = rows_shown();
        for (int i = 0; i < shown; ++i) {
            int idx = scroll_ + i;
            if (idx >= (int)recs_.size()) break;
            const NoteRecord& r = recs_[idx];
            double y = NC_HDR + i * NC_ROW;
            if (i % 2 == 0) {
                col(cr, cfg.c_ws_bg, 0.35);
                cairo_rectangle(cr, 6, y + 2, NC_W - 12, NC_ROW - 4);
                cairo_fill(cr);
            }
            double tx = 14;
            if (r.icon) {
                draw_note_avatar(cr, r.icon, 10, y + (NC_ROW - 32) / 2.0, 32);
                tx = 48;
            }
            // app + age on the first line, summary/body on the second
            std::string app = utf8_trunc(r.app.empty() ? "unknown" : r.app, 22);
            const Color ac  = r.urgency >= 2 ? cfg.c_urgent
                              : r.active    ? cfg.c_accent
                                            : cfg.c_dim;
            say(cr, tx, y + 15, app, ac);
            std::string age = ago(r.age_s);
            say(cr, NC_W - 14 - tw(cr, age), y + 15, age, cfg.c_dim);
            double tagx = NC_W - 30 - tw(cr, age);
            if (cfg.app_muted(r.app)) {
                std::string m = "muted";
                tagx -= tw(cr, m);
                say(cr, tagx, y + 15, m, cfg.c_dim);
                tagx -= 12;
            }
            if (r.has_action) { // click-to-invoke, marked with its label
                std::string act = "\u21b3 " + utf8_trunc(r.action_label, 14);
                tagx -= tw(cr, act);
                say(cr, tagx, y + 15, act, cfg.c_accent);
            }
            std::string line = r.summary;
            if (!r.body.empty()) line += line.empty() ? r.body : " \u2014 " + r.body;
            // collapse newlines: one row is one line
            for (auto& ch : line)
                if (ch == '\n' || ch == '\r') ch = ' ';
            say(cr, tx, y + 35, utf8_trunc(line, r.icon ? 40 : 46), cfg.c_fg);
        }

        std::string hint = recs_.size() > (size_t)shown
                               ? "scroll for more \u00b7 click \u21b3 to act \u00b7 right-click to mute"
                               : "click \u21b3 to act \u00b7 right-click an app to mute it";
        say(cr, 12, H - NC_FOOT / 2.0, hint, cfg.c_dim);
    }

    void redraw() {
        panel_.win.ensure(*bar_, place_.anchor, place_.mt, place_.mr,
                          place_.mb, place_.ml, "mattbar-notifications-centre",
                          NC_W, panel_h(), out_);
        panel_.win.draw();
    }

    void open_panel() {
        reload();
        double a = bar_->slot_along(this);
        if (a < 0) a = bar_->pointer_along();
        place_ = popup_place(a, NC_W, bar_->along_length());
        out_   = bar_->current_output();
        panel_.win.paint = [this](cairo_t* cr) { paint(cr); };
        panel_.win.pmotion = [this](double, double) { panel_.disarm_close(); };
        panel_.win.pleave  = [this] { panel_.arm_close(2000); };
        panel_.win.pscroll = [this](int d) {
            panel_.disarm_close();
            int maxs = std::max(0, (int)recs_.size() - NC_ROWS_MAX);
            scroll_  = std::clamp(scroll_ + (d > 0 ? 1 : -1), 0, maxs);
            redraw();
        };
        panel_.win.click = [this](double x, double y, int btn) {
            panel_.disarm_close();
            if (y < NC_HDR) {
                if (btn == BTN_LEFT && x >= clear_x_) {
                    // Clear all: drop the history AND dismiss anything still
                    // on screen, so the bell really is empty afterwards.
                    if (auto* d = notify_daemon()) {
                        d->dismiss_all();
                        d->clear_history();
                    }
                    reload();
                    scroll_ = 0;
                    redraw();
                    bar_->request_draw();
                }
                return;
            }
            int i = (int)((y - NC_HDR) / NC_ROW) + scroll_;
            if (i < 0 || i >= (int)recs_.size()) return;
            const std::string app = recs_[i].app;
            if (btn == BTN_RIGHT) { // per-app mute toggle, right where you saw it
                cfg.set_app_muted(app, !cfg.app_muted(app));
                PDBG("notifications: %s is now %s", app.c_str(),
                     cfg.app_muted(app) ? "muted" : "unmuted");
                redraw();
            } else if (btn == BTN_LEFT && recs_[i].has_action) {
                // fire the default action ("open the mail", "join the
                // call") straight from the history panel
                if (auto* d = notify_daemon()) d->invoke(recs_[i].id);
                reload();
                redraw();
            }
        };
        panel_.hold(true);
        redraw();
    }

    Bar*                    bar_ = nullptr;
    AutoPanel               panel_;
    std::vector<NoteRecord> recs_;
    int                     scroll_  = 0;
    double                  clear_x_ = 0;
    PopupPlace              place_{};
    wl_output*              out_ = nullptr;
    std::string             checked_, resolved_;
};

} // namespace

Module* make_notifications() { return new NotificationsModule; }

// ---------------------------------------------------------------------------
// Power profiles (power-profiles-daemon)
// ---------------------------------------------------------------------------
namespace {

// PPD moved from net.hadess to org.freedesktop.UPower in 0.20; try the
// current name first and fall back, so both eras work unchanged.
struct PpdEndpoint {
    const char* dest;
    const char* path;
    const char* iface;
};
constexpr PpdEndpoint PPD[] = {
    {"org.freedesktop.UPower.PowerProfiles",
     "/org/freedesktop/UPower/PowerProfiles",
     "org.freedesktop.UPower.PowerProfiles"},
    {"net.hadess.PowerProfiles", "/net/hadess/PowerProfiles",
     "net.hadess.PowerProfiles"},
};

constexpr int PP_W = 210, PP_ROW = 34, PP_PAD = 8, PP_SEP = 11;

// The session actions, in escalating severity. Suspend and hibernate are
// recoverable and fire on one click; restart and shutdown are not, so they
// arm on the first click and execute on the second.
struct PowerAction {
    const char*  label;
    const char*  glyph;    // preferred rune
    const char*  fallback; // md rune if the bar font lacks the first
    std::string Config::*cmd;
    bool         confirm;
};
constexpr PowerAction PACTS[] = {
    {"Suspend", "\uf186", "\U000F0904", &Config::power_cmd_suspend, false},
    {"Hibernate", "\U000F02CA", "\U000F0904", &Config::power_cmd_hibernate,
     false},
    {"Restart", "\uf021", "\U000F0450", &Config::power_cmd_restart, true},
    {"Shut down", "\uf011", "\U000F0425", &Config::power_cmd_shutdown,
     true},
};
constexpr int N_ACTS = 4;

class PowerModule : public Module {
public:
    ~PowerModule() override {
        if (retry_fd_ >= 0) close(retry_fd_);
    }

    // Without power-profiles-daemon the module either hides (actions off:
    // nothing to show, like the battery on a desktop) or stays as a plain
    // power button whose popup has only the session actions.
    bool enabled() const override {
        return cfg.show_power &&
               (!active_.empty() || cfg.power_show_actions);
    }

    void init(Bar& bar) override {
        bar_ = &bar;
        panel_.init(bar, "power-close");
        pump_.on_teardown = [this](const char* why) {
            bus_ = nullptr;
            active_.clear();
            profiles_.clear();
            bar_->request_draw();
            schedule_retry(why);
        };
        retry_fd_ = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
        bar.add_fd(retry_fd_, [this](uint32_t) {
            uint64_t n;
            while (read(retry_fd_, &n, sizeof n) > 0) {}
            if (!bus_ && setup_bus()) attempts_ = 0;
            else if (!bus_) schedule_retry("system bus still unavailable");
        }, "ppd-retry");
        if (!setup_bus()) schedule_retry(nullptr);
    }

    double width(cairo_t* cr) override {
        if (active_.empty() && !actions_on()) return 0;
        resolve_glyphs(cr);
        std::string s = label();
        if (!cfg_vertical()) return tw(cr, s);
        cairo_font_extents_t fe;
        cairo_font_extents(cr, &fe);
        return fe.ascent + fe.descent + 10;
    }

    void draw(cairo_t* cr, double a, double t) override {
        if (active_.empty() && !actions_on()) return;
        std::string s = label();
        // Performance gets the accent so a glance tells you the machine is
        // not in its quiet mode.
        const Color c = active_ == "performance" ? cfg.c_accent : cfg.c_fg;
        if (!cfg_vertical()) {
            say(cr, a, t / 2.0, s, c);
        } else {
            cairo_font_extents_t fe;
            cairo_font_extents(cr, &fe);
            col(cr, c, 1.0);
            cairo_move_to(cr, (t - tw(cr, s)) / 2.0,
                          a + (fe.ascent + fe.descent + 10) / 2.0 +
                              (fe.ascent - fe.descent) / 2.0);
            cairo_show_text(cr, s.c_str());
        }
    }

    bool on_click(double, int button) override {
        if (button != BTN_LEFT) return false;
        if (active_.empty() && !actions_on()) return false;
        if (panel_.open()) { panel_.close_now(); return true; }
        open_panel();
        return true;
    }

    // ---- mattbarctl ------------------------------------------------------
    std::string              ctl_active() const { return active_; }
    std::vector<std::string> ctl_profiles() const { return profiles_; }
    bool ctl_set(const std::string& p) {
        if (active_.empty()) return false; // daemon absent
        if (std::find(profiles_.begin(), profiles_.end(), p) ==
            profiles_.end())
            return false;
        set_profile(p);
        return true;
    }

    bool on_scroll(double, int dir) override { // cycle without opening
        if (profiles_.size() < 2) return false;
        auto it = std::find(profiles_.begin(), profiles_.end(), active_);
        int  i  = it == profiles_.end() ? 0 : (int)(it - profiles_.begin());
        i = (i + (dir > 0 ? 1 : -1) + (int)profiles_.size()) %
            (int)profiles_.size();
        set_profile(profiles_[i]);
        return true;
    }

private:
    // ---- D-Bus -----------------------------------------------------------
    bool setup_bus() {
        sd_bus* b = nullptr;
        if (sd_bus_open_system(&b) < 0) {
            fprintf(stderr, "mattbar: power: system bus unavailable\n");
            return false;
        }
        bus_ = b;
        sd_bus_set_method_call_timeout(bus_, 500 * 1000ULL);
        pump_.attach(*bar_, bus_, "ppd");
        // Probe the two interface generations; whichever answers wins.
        for (auto& e : PPD) {
            if (read_active(e)) {
                ep_ = &e;
                break;
            }
        }
        if (!ep_) {
            fprintf(stderr,
                    "mattbar: power: power-profiles-daemon not present; "
                    "module hidden\n");
            return true; // bus is fine; the daemon just isn't there
        }
        read_profiles();
        std::string match =
            std::string("type='signal',sender='") + ep_->dest + "',path='" +
            ep_->path +
            "',interface='org.freedesktop.DBus.Properties',member='"
            "PropertiesChanged'";
        sd_bus_add_match(bus_, nullptr, match.c_str(), on_props, this);
        pump_.process();
        PDBG("power: %s, active=%s", ep_->iface, active_.c_str());
        return true;
    }

    bool read_active(const PpdEndpoint& e) {
        if (!bus_) return false;
        sd_bus_error err = SD_BUS_ERROR_NULL;
        char*        v   = nullptr;
        int r = sd_bus_get_property_string(bus_, e.dest, e.path, e.iface,
                                           "ActiveProfile", &err, &v);
        sd_bus_error_free(&err);
        if (r < 0 || !v) return false;
        std::string s = v;
        free(v);
        if (s != active_) {
            active_ = s;
            if (bar_) bar_->request_draw();
        }
        return true;
    }

    void read_profiles() {
        profiles_.clear();
        if (!bus_ || !ep_) return;
        sd_bus_error    err = SD_BUS_ERROR_NULL;
        sd_bus_message* m   = nullptr;
        if (sd_bus_get_property(bus_, ep_->dest, ep_->path, ep_->iface,
                                "Profiles", &err, &m, "aa{sv}") >= 0 &&
            m) {
            if (sd_bus_message_enter_container(m, 'a', "a{sv}") > 0) {
                while (sd_bus_message_enter_container(m, 'a', "{sv}") > 0) {
                    while (sd_bus_message_enter_container(m, 'e', "sv") > 0) {
                        const char* k = nullptr;
                        sd_bus_message_read(m, "s", &k);
                        if (k && !strcmp(k, "Profile")) {
                            const char* v = nullptr;
                            if (sd_bus_message_enter_container(m, 'v', "s") >
                                0) {
                                sd_bus_message_read(m, "s", &v);
                                sd_bus_message_exit_container(m);
                                if (v) profiles_.push_back(v);
                            } else {
                                sd_bus_message_skip(m, "v");
                            }
                        } else {
                            sd_bus_message_skip(m, "v");
                        }
                        sd_bus_message_exit_container(m);
                    }
                    sd_bus_message_exit_container(m);
                }
                sd_bus_message_exit_container(m);
            }
            sd_bus_message_unref(m);
        }
        sd_bus_error_free(&err);
        if (profiles_.empty()) // daemon answered but told us nothing useful
            profiles_ = {"power-saver", "balanced", "performance"};
    }

    void set_profile(const std::string& p) {
        if (!bus_ || !ep_ || p == active_) return;
        sd_bus_error err = SD_BUS_ERROR_NULL;
        int r = sd_bus_set_property(bus_, ep_->dest, ep_->path, ep_->iface,
                                    "ActiveProfile", &err, "s", p.c_str());
        if (r < 0)
            fprintf(stderr, "mattbar: power: cannot set %s (%s)\n", p.c_str(),
                    err.message ? err.message : "no detail");
        sd_bus_error_free(&err);
        pump_.process();
        // Optimistic update; PropertiesChanged confirms (or corrects) it.
        if (r >= 0) {
            active_ = p;
            persist_power_profile(p);
            bar_->request_draw();
            if (panel_.open()) redraw();
        }
    }

    static int on_props(sd_bus_message*, void* ud, sd_bus_error*) {
        auto* self = static_cast<PowerModule*>(ud);
        if (self->ep_) self->read_active(*self->ep_);
        if (self->panel_.open()) self->redraw();
        return 0;
    }

    void schedule_retry(const char* why) {
        if (++attempts_ > 6) return; // ~2 min, then we rely on bus signals
        if (why) fprintf(stderr, "mattbar: power: %s; retrying\n", why);
        itimerspec ts{};
        ts.it_value.tv_sec = 1 << std::min(attempts_, 6);
        timerfd_settime(retry_fd_, 0, &ts, nullptr);
    }

    // ---- presentation ----------------------------------------------------
    static const char* pretty(const std::string& p) {
        if (p == "power-saver") return "Power saver";
        if (p == "balanced") return "Balanced";
        if (p == "performance") return "Performance";
        return p.c_str();
    }

    const std::string& glyph_for(const std::string& p) const {
        if (p == "power-saver") return g_saver_;
        if (p == "performance") return g_perf_;
        return g_bal_;
    }

    std::string label() const {
        if (active_.empty()) return g_power_; // plain power button mode
        std::string s = glyph_for(active_);
        if (cfg.power_show_label) s += " " + std::string(pretty(active_));
        return s;
    }

    void resolve_glyphs(cairo_t* cr) {
        std::string key = cfg.font + cfg.power_glyph_saver +
                          cfg.power_glyph_balanced + cfg.power_glyph_perf;
        if (checked_ == key) return;
        checked_ = key;
        auto pick = [&](const std::string& want, const char* md,
                        const char* txt) {
            if (glyph_mapped(cr, want)) return want;
            if (glyph_mapped(cr, md)) return std::string(md);
            return std::string(txt);
        };
        g_saver_ = pick(cfg.power_glyph_saver, "\U000F00E2", "eco");
        g_bal_   = pick(cfg.power_glyph_balanced, "\U000F0241", "bal");
        g_perf_  = pick(cfg.power_glyph_perf, "\U000F0D0D", "perf");
        g_power_ = pick("\uf011", "\U000F0425", "PWR");
        for (int i = 0; i < N_ACTS; ++i) {
            g_act_[i] = PACTS[i].glyph;
            if (!glyph_mapped(cr, g_act_[i])) g_act_[i] = PACTS[i].fallback;
            if (!glyph_mapped(cr, g_act_[i])) g_act_[i].clear(); // text only
        }
    }

    int n_prof() const { return (int)profiles_.size(); }
    bool actions_on() const { return cfg.power_show_actions; }
    int panel_h() const {
        int h = PP_PAD * 2 + n_prof() * PP_ROW;
        if (actions_on()) {
            if (n_prof() > 0) h += PP_SEP;
            h += N_ACTS * PP_ROW;
        }
        if (h <= PP_PAD * 2) h += PP_ROW; // degenerate: nothing at all
        return h;
    }
    double actions_y0() const {
        return PP_PAD + n_prof() * PP_ROW + (n_prof() > 0 ? PP_SEP : 0);
    }

    void paint(cairo_t* cr) {
        const int H = panel_h();
        panel_bg(cr, PP_W, H);
        if (actions_on()) {
            if (n_prof() > 0) { // hairline between profiles and actions
                col(cr, cfg.c_ws_bg, 1.0);
                cairo_set_line_width(cr, 1);
                double sy = PP_PAD + n_prof() * PP_ROW + PP_SEP / 2.0;
                cairo_move_to(cr, 10, sy);
                cairo_line_to(cr, PP_W - 10, sy);
                cairo_stroke(cr);
            }
            for (int i = 0; i < N_ACTS; ++i) {
                double y  = actions_y0() + i * PP_ROW;
                double cy = y + PP_ROW / 2.0;
                bool   armed = pending_ == i;
                if (armed) { // the "are you sure" state wears urgent
                    col(cr, cfg.c_urgent, 0.18);
                    rrect(cr, 6, y + 2, PP_W - 12, PP_ROW - 4, 7);
                    cairo_fill(cr);
                }
                const Color& tc = armed ? cfg.c_urgent : cfg.c_fg;
                if (!g_act_[i].empty()) say(cr, 16, cy, g_act_[i], tc);
                say(cr, 42, cy,
                    armed ? std::string(PACTS[i].label) + "? click again"
                          : PACTS[i].label,
                    tc);
            }
        }
        for (size_t i = 0; i < profiles_.size(); ++i) {
            const std::string& p = profiles_[i];
            double y = PP_PAD + i * PP_ROW;
            bool on = (p == active_);
            if (on) {
                col(cr, cfg.c_ws_bg, 1.0);
                rrect(cr, 6, y + 2, PP_W - 12, PP_ROW - 4, 7);
                cairo_fill(cr);
            }
            // radio dot: filled + accent for the profile currently in force
            double cx = 22, cy = y + PP_ROW / 2.0;
            col(cr, on ? cfg.c_accent : cfg.c_dim, 1.0);
            cairo_set_line_width(cr, 1.5);
            cairo_arc(cr, cx, cy, 6, 0, 2 * M_PI);
            cairo_stroke(cr);
            if (on) {
                col(cr, cfg.c_accent, 1.0);
                cairo_arc(cr, cx, cy, 3.2, 0, 2 * M_PI);
                cairo_fill(cr);
            }
            std::string g = glyph_for(p);
            say(cr, 38, cy, g, on ? cfg.c_accent : cfg.c_dim);
            say(cr, 62, cy, pretty(p), on ? cfg.c_fg : cfg.c_dim);
        }
    }

    void redraw() {
        panel_.win.ensure(*bar_, place_.anchor, place_.mt, place_.mr,
                          place_.mb, place_.ml, "mattbar-power", PP_W,
                          panel_h(), out_);
        panel_.win.draw();
    }

    void open_panel() {
        pending_ = -1;
        if (!active_.empty() && profiles_.empty()) read_profiles();
        double a = bar_->slot_along(this);
        if (a < 0) a = bar_->pointer_along();
        place_ = popup_place(a, PP_W, bar_->along_length());
        out_   = bar_->current_output();
        panel_.win.paint   = [this](cairo_t* cr) { paint(cr); };
        panel_.win.pmotion = [this](double, double) { panel_.disarm_close(); };
        panel_.win.pleave  = [this] {
            pending_ = -1; // walking away disarms restart/shutdown
            redraw();
            panel_.arm_close();
        };
        panel_.win.click   = [this](double, double y, int btn) {
            panel_.disarm_close();
            if (btn != BTN_LEFT) { pending_ = -1; redraw(); return; }
            // profiles first
            int i = (int)((y - PP_PAD) / PP_ROW);
            if (i >= 0 && i < n_prof() && y < PP_PAD + n_prof() * PP_ROW) {
                pending_ = -1;
                set_profile(profiles_[i]);
                // Stay open briefly: the moved radio dot is the feedback.
                panel_.arm_close(700);
                return;
            }
            if (!actions_on()) return;
            int a = (int)((y - actions_y0()) / PP_ROW);
            if (a < 0 || a >= N_ACTS || y < actions_y0()) {
                pending_ = -1;
                redraw();
                return;
            }
            if (PACTS[a].confirm && pending_ != a) {
                // First click arms; anything else disarms. Nobody reboots
                // off a stray click at the bottom of a popup.
                pending_ = a;
                redraw();
                return;
            }
            pending_ = -1;
            PDBG("power: %s -> %s", PACTS[a].label,
                 (cfg.*(PACTS[a].cmd)).c_str());
            spawn_detached(cfg.*(PACTS[a].cmd));
            panel_.close_now();
        };
        panel_.hold(true);
        redraw();
    }

    Bar*                     bar_ = nullptr;
    AutoPanel                panel_;
    SdPump                   pump_;
    sd_bus*                  bus_ = nullptr;
    const PpdEndpoint*       ep_  = nullptr;
    std::string              active_;
    std::vector<std::string> profiles_;
    int                      retry_fd_ = -1, attempts_ = 0;
    PopupPlace               place_{};
    wl_output*               out_ = nullptr;
    int                      pending_ = -1; // armed confirm row, or -1
    std::string              checked_, g_saver_, g_bal_, g_perf_, g_power_;
    std::string              g_act_[N_ACTS];
};

PowerModule* g_power = nullptr;

} // namespace

Module* make_power() { return g_power = new PowerModule; }

// mattbarctl hooks (ctl.cpp): read and set the profile without the popup.
std::string              power_ctl_active() {
    return g_power ? g_power->ctl_active() : std::string();
}
std::vector<std::string> power_ctl_profiles() {
    return g_power ? g_power->ctl_profiles() : std::vector<std::string>();
}
bool power_ctl_set(const std::string& p) {
    return g_power && g_power->ctl_set(p);
}

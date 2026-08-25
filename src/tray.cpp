// Expandable system tray (StatusNotifierItem / KDE spec) via sd-bus.
//
// MattBar tries to own org.kde.StatusNotifierWatcher itself; if another
// watcher already runs (e.g. a second bar), it registers as a host with it
// instead. Icons come from IconName (theme PNG lookup, including the
// item's IconThemePath for apps like Dropbox/Spotify/Steam that ship
// icons outside /usr/share/icons) or IconPixmap (ARGB32 network byte
// order -> premultiplied cairo).
//
// Right-click renders the item's com.canonical.dbusmenu menu in an
// xdg_popup parented to the bar's layer surface. Submenus navigate in
// place with a "< Back" row. Falls back to the SNI ContextMenu method for
// items that expose no menu. Left-click on ItemIsMenu / Dropbox-style
// appindicators opens that same menu (Activate is a no-op on Wayland).
//
// Omarchy-style behavior: collapsed by default, a chevron toggles the icons.

#include "modules.hpp"
#include "bar.hpp"
#include "config.hpp"
#include "shm.hpp"

#include <linux/input-event-codes.h>
#include <cerrno>
#include <poll.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <systemd/sd-bus.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <map>
#include <set>
#include <dlfcn.h>
#include <memory>
#include <string>
#include <vector>

namespace {

constexpr const char* WATCHER_NAME  = "org.kde.StatusNotifierWatcher";
constexpr const char* WATCHER_PATH  = "/StatusNotifierWatcher";
constexpr const char* WATCHER_IFACE = "org.kde.StatusNotifierWatcher";
constexpr const char* MENU_IFACE    = "com.canonical.dbusmenu";
constexpr const char* ITEM_IFACES[] = {"org.kde.StatusNotifierItem",
                                       "org.freedesktop.StatusNotifierItem"};

struct TrayItem {
    std::string service;  // bus name
    std::string path;     // object path
    std::string iface;    // whichever SNI interface answered
    std::string menu;     // dbusmenu object path ("" if none)
    std::string id;       // StatusNotifierItem.Id
    std::string title;    // StatusNotifierItem.Title
    std::string icon_name;
    std::string theme_path; // StatusNotifierItem.IconThemePath
    bool only_menu = false; // ItemIsMenu, or appindicator with no Activate UI
    cairo_surface_t* icon = nullptr;
    long icon_ms = 0;     // last icon load (throttles NewIcon storms)
    // Bus-side filtered subscriptions, scoped to THIS item's name only.
    sd_bus_slot* watch_owner = nullptr; // its NameOwnerChanged
    sd_bus_slot* watch_icon  = nullptr; // its NewIcon
};

static long now_ms() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

// ---------------------------------------------------------------------------
// dbusmenu layout parsing
// ---------------------------------------------------------------------------
struct MenuNode {
    int32_t id = 0;
    std::string label;
    bool separator = false;
    bool enabled   = true;
    bool visible   = true;
    // dbusmenu toggle rendering: 0 none, 1 checkmark, 2 radio;
    // state -1 unknown/indeterminate, 0 off, 1 on
    int  toggle_type  = 0;
    int  toggle_state = -1;
    // per-item icon: theme name and/or raw PNG bytes ("icon-data")
    std::string icon_name;
    std::shared_ptr<cairo_surface_t> icon; // decoded lazily, shared on copy
    std::vector<uint8_t> icon_png;
    std::vector<MenuNode> children;
};

// cairo PNG-from-memory reader for dbusmenu "icon-data"
struct PngMem {
    const uint8_t* p;
    size_t         n, off;
};
cairo_status_t png_mem_read(void* closure, unsigned char* out,
                            unsigned int len) {
    auto* m = static_cast<PngMem*>(closure);
    if (m->off + len > m->n) return CAIRO_STATUS_READ_ERROR;
    memcpy(out, m->p + m->off, len);
    m->off += len;
    return CAIRO_STATUS_SUCCESS;
}

// strip dbusmenu access-key markers: "_File" -> "File", "__" -> "_"
std::string clean_label(const char* s) {
    std::string out;
    for (const char* p = s; *p; ++p) {
        if (*p == '_' && p[1] == '_') { out += '_'; ++p; }
        else if (*p != '_') out += *p;
    }
    return out;
}

// parses one "(ia{sv}av)" node; message must be positioned at it
int parse_menu_node(sd_bus_message* m, MenuNode& out) {
    int r = sd_bus_message_enter_container(m, 'r', "ia{sv}av");
    if (r <= 0) return r;
    sd_bus_message_read(m, "i", &out.id);

    sd_bus_message_enter_container(m, 'a', "{sv}");
    while (sd_bus_message_enter_container(m, 'e', "sv") > 0) {
        const char* key = nullptr;
        sd_bus_message_read(m, "s", &key);
        char t = 0;
        const char* contents = nullptr;
        sd_bus_message_peek_type(m, &t, &contents);
        sd_bus_message_enter_container(m, 'v', contents);
        std::string k = key ? key : "";
        if (k == "label" && contents && !strcmp(contents, "s")) {
            const char* v = nullptr;
            sd_bus_message_read(m, "s", &v);
            if (v) out.label = clean_label(v);
        } else if (k == "type" && contents && !strcmp(contents, "s")) {
            const char* v = nullptr;
            sd_bus_message_read(m, "s", &v);
            out.separator = v && !strcmp(v, "separator");
        } else if (k == "enabled" && contents && !strcmp(contents, "b")) {
            int b = 1;
            sd_bus_message_read(m, "b", &b);
            out.enabled = b;
        } else if (k == "visible" && contents && !strcmp(contents, "b")) {
            int b = 1;
            sd_bus_message_read(m, "b", &b);
            out.visible = b;
        } else if (k == "toggle-type" && contents && !strcmp(contents, "s")) {
            const char* v = nullptr;
            sd_bus_message_read(m, "s", &v);
            out.toggle_type = !v ? 0
                              : !strcmp(v, "checkmark") ? 1
                              : !strcmp(v, "radio")     ? 2
                                                        : 0;
        } else if (k == "toggle-state" && contents &&
                   !strcmp(contents, "i")) {
            int32_t v = -1;
            sd_bus_message_read(m, "i", &v);
            out.toggle_state = v;
        } else if (k == "icon-name" && contents && !strcmp(contents, "s")) {
            const char* v = nullptr;
            sd_bus_message_read(m, "s", &v);
            if (v) out.icon_name = v;
        } else if (k == "icon-data" && contents && !strcmp(contents, "ay")) {
            const void* d = nullptr;
            size_t      n = 0;
            if (sd_bus_message_read_array(m, 'y', &d, &n) >= 0 && d && n)
                out.icon_png.assign((const uint8_t*)d,
                                    (const uint8_t*)d + n);
        } else {
            sd_bus_message_skip(m, contents);
        }
        sd_bus_message_exit_container(m); // variant
        sd_bus_message_exit_container(m); // dict entry
    }
    sd_bus_message_exit_container(m); // a{sv}

    sd_bus_message_enter_container(m, 'a', "v");
    while (sd_bus_message_enter_container(m, 'v', "(ia{sv}av)") > 0) {
        MenuNode child;
        parse_menu_node(m, child);
        sd_bus_message_exit_container(m); // variant
        if (child.visible) out.children.push_back(std::move(child));
    }
    sd_bus_message_exit_container(m); // av
    sd_bus_message_exit_container(m); // struct
    return 1;
}

// ---------------------------------------------------------------------------
// Tray module
// ---------------------------------------------------------------------------
class TrayModule : public Module {
public:
    ~TrayModule() override {
        close_menu();
        for (auto& it : items_) {
            unwatch_item(it);
            if (it.icon) cairo_surface_destroy(it.icon);
        }
        if (bus_) sd_bus_unref(bus_);
    }

    void init(Bar& bar) override {
        bar_ = &bar;
        // one-time auxiliary timers
        bus_timer_fd_ =
            timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
        bar.add_fd(bus_timer_fd_, [this](uint32_t) {
            uint64_t n;
            while (read(bus_timer_fd_, &n, sizeof n) > 0) {}
            process(); // sd-bus asked to be driven at this deadline
        }, "dbus-timeout");
        reconnect_fd_ =
            timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
        bar.add_fd(reconnect_fd_, [this](uint32_t) {
            uint64_t n;
            while (read(reconnect_fd_, &n, sizeof n) > 0) {}
            ++reconnect_attempts_;
            if (setup_bus()) {
                fprintf(stderr, "mattbar: tray reconnected to D-Bus\n");
                reconnect_attempts_ = 0;
            } else {
                schedule_reconnect();
            }
        }, "dbus-reconnect");
        setup_bus();
        init_collapse_timer(bar);
    }

    bool setup_bus() {
        if (sd_bus_open_user(&bus_) < 0) { bus_ = nullptr; return false; }
        // Cap EVERY synchronous call (icon/menu property gets, GetLayout,
        // host registration) at 500 ms. The sd-bus default is 25 s, which
        // let one hung or mutually-waiting peer freeze the entire bar.
        sd_bus_set_method_call_timeout(bus_, 500 * 1000ULL);

        // Try to be THE watcher.
        int r = sd_bus_request_name(bus_, WATCHER_NAME, 0);
        if (r >= 0) {
            we_are_watcher_ = true;
            sd_bus_add_object_vtable(bus_, &watcher_slot_, WATCHER_PATH,
                                     WATCHER_IFACE, watcher_vtable, this);
        }

        // Register a host name either way.
        host_name_ = std::string("org.kde.StatusNotifierHost-") +
                     std::to_string(getpid());
        sd_bus_request_name(bus_, host_name_.c_str(), 0);

        if (we_are_watcher_) {
            sd_bus_emit_signal(bus_, WATCHER_PATH, WATCHER_IFACE,
                               "StatusNotifierHostRegistered", "");
        } else {
            // External watcher: announce ourselves, pull existing items,
            // and follow its (un)register signals.
            sd_bus_call_method(bus_, WATCHER_NAME, WATCHER_PATH, WATCHER_IFACE,
                               "RegisterStatusNotifierHost", nullptr, nullptr,
                               "s", host_name_.c_str());
            char** strv = nullptr;
            if (sd_bus_get_property_strv(bus_, WATCHER_NAME, WATCHER_PATH,
                                         WATCHER_IFACE,
                                         "RegisteredStatusNotifierItems",
                                         nullptr, &strv) >= 0 && strv) {
                for (char** p = strv; *p; ++p) {
                    add_item_from_spec(*p, "");
                    free(*p);
                }
                free(strv);
            }
            sd_bus_match_signal(bus_, nullptr, WATCHER_NAME, WATCHER_PATH,
                                WATCHER_IFACE, "StatusNotifierItemRegistered",
                                on_ext_registered, this);
            sd_bus_match_signal(bus_, nullptr, WATCHER_NAME, WATCHER_PATH,
                                WATCHER_IFACE, "StatusNotifierItemUnregistered",
                                on_ext_unregistered, this);
        }

        // NOTE: deliberately NO broad matches here. A global
        // NameOwnerChanged subscription receives a broadcast for EVERY
        // connection appearing or vanishing on the bus — one chatty or
        // reconnect-looping client elsewhere in the session translated to
        // thousands of wakeups/second in this process. Item-scoped matches
        // are installed per tracked item instead (see watch_item), so the
        // bus daemon filters everything else before it reaches us.

        bus_fd_ = sd_bus_get_fd(bus_);
        bus_epoll_events_ = EPOLLIN;
        bar_->add_fd(bus_fd_, [this](uint32_t ev) {
            if (ev & (EPOLLHUP | EPOLLERR)) {
                bus_fail("connection hung up");
                return;
            }
            process();
        }, "dbus");
        process();
        return bus_ != nullptr; // process() may have detected failure

    }

    void init_collapse_timer(Bar& bar) {
        collapse_fd_ =
            timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
        bar.add_fd(collapse_fd_, [this](uint32_t) {
            uint64_t n;
            while (read(collapse_fd_, &n, sizeof n) > 0) {}
            if (expanded_) {
                // on_hover re-arms on pointer *motion*, but a pointer
                // parked motionless over the icons generates no events —
                // don't collapse under it, check again in one period.
                if (pointer_on_tray()) { arm_collapse(true); return; }
                expanded_ = false;
                armed_ms_ = -1;
                bar_->request_draw();
            }
        }, "tray-collapse");
        if (expanded_) arm_collapse(true);
    }

    // Is the pointer currently resting anywhere on this module's slot?
    // (Used by the collapse timer; hovers only fire on motion.)
    bool pointer_on_tray() const {
        BarSurface* bs = bar_->current();
        if (!bs || !bs->ptr_inside) return false;
        double x = bs->slot_along(const_cast<TrayModule*>(this));
        if (x < 0) return false;  // pointer is on a bar without the tray
        double along = bar_->pointer_along();
        return along >= x && along < x + const_cast<TrayModule*>(this)
                                            ->width(nullptr);
    }

    // (Re)start or cancel the auto-collapse countdown.
    void arm_collapse(bool arm) {
        if (collapse_fd_ < 0) return;
        itimerspec ts{};
        if (arm && cfg.tray_collapse_ms > 0) {
            ts.it_value.tv_sec  = cfg.tray_collapse_ms / 1000;
            ts.it_value.tv_nsec = (cfg.tray_collapse_ms % 1000) * 1000000L;
            armed_ms_ = cfg.tray_collapse_ms;
        } else {
            armed_ms_ = arm ? 0 : -1;  // 0: expanded with "off"; -1: idle
        }
        timerfd_settime(collapse_fd_, 0, &ts, nullptr);
    }

    // Live re-arm: if the user changes the timeout in settings (or the
    // conf file) while the tray is open, restart the countdown with the
    // new duration instead of letting the old deadline fire — otherwise
    // the stepper appears to do nothing until the next expand.
    void tick() override {
        if (expanded_ && armed_ms_ >= 0 &&
            armed_ms_ != cfg.tray_collapse_ms)
            arm_collapse(true);
    }

    // ---- rendering -------------------------------------------------------
    double width(cairo_t*) override {
        double w = GEAR_W + CHEV_W;
        if (expanded_) {
            int n = 0;
            for (auto& it : items_)
                if (!item_hidden(it)) ++n;
            if (n) w += n * (TRAY_ICON_SIZE + TRAY_ICON_GAP);
        }
        return w;
    }

    void draw(cairo_t* cr, double a, double t) override {
        const bool vert = cfg_vertical();
        if (expanded_) {
            // On a crowded bar the grown right group can overrun the
            // center modules, and modules composite OVER whatever was
            // drawn before them — buried text would bleed through around
            // the icons. Replace (not composite: OPERATOR_SOURCE) the
            // pixels under the tray's whole extent with the bar
            // background, so the text is erased rather than dimmed and a
            // translucent bar keeps exactly its configured translucency.
            cairo_save(cr);
            cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
            cairo_set_source_rgba(cr, cfg.c_bg.r, cfg.c_bg.g, cfg.c_bg.b,
                                  cfg.c_bg.a);
            double w = width(cr);
            if (vert) cairo_rectangle(cr, 0, a, t, w);
            else      cairo_rectangle(cr, a, 0, w, t);
            cairo_fill(cr);
            cairo_restore(cr);
        }
        // center of the gear along/across
        double gx = vert ? t / 2.0 : a + GEAR_W / 2.0 - 1;
        double gy = vert ? a + GEAR_W / 2.0 - 1 : t / 2.0;
        cairo_set_source_rgba(cr, cfg.c_dim.r, cfg.c_dim.g, cfg.c_dim.b, 1.0);
        cairo_set_line_width(cr, 1.4);
        cairo_new_path(cr);
        cairo_arc(cr, gx, gy, 4.2, 0, 2 * M_PI);
        cairo_stroke(cr);
        for (int i = 0; i < 6; ++i) {
            double ang = i * M_PI / 3.0;
            cairo_move_to(cr, gx + 4.6 * cos(ang), gy + 4.6 * sin(ang));
            cairo_line_to(cr, gx + 6.6 * cos(ang), gy + 6.6 * sin(ang));
        }
        cairo_stroke(cr);
        cairo_arc(cr, gx, gy, 1.2, 0, 2 * M_PI);
        cairo_fill(cr);

        // chevron: points toward where the icons appear when collapsed
        double cx = vert ? t / 2.0 : a + GEAR_W + CHEV_W / 2.0;
        double cy = vert ? a + GEAR_W + CHEV_W / 2.0 : t / 2.0;
        cairo_new_path(cr);
        if (!vert) {
            if (expanded_) { // icons shown to the right -> point right
                cairo_move_to(cr, cx - 3, cy - 5);
                cairo_line_to(cr, cx + 3, cy);
                cairo_line_to(cr, cx - 3, cy + 5);
            } else {
                cairo_move_to(cr, cx + 3, cy - 5);
                cairo_line_to(cr, cx - 3, cy);
                cairo_line_to(cr, cx + 3, cy + 5);
            }
        } else {
            if (expanded_) { // icons shown below -> point down
                cairo_move_to(cr, cx - 5, cy - 3);
                cairo_line_to(cr, cx, cy + 3);
                cairo_line_to(cr, cx + 5, cy - 3);
            } else {
                cairo_move_to(cr, cx - 5, cy + 3);
                cairo_line_to(cr, cx, cy - 3);
                cairo_line_to(cr, cx + 5, cy + 3);
            }
        }
        cairo_close_path(cr);
        cairo_fill(cr);

        if (!expanded_) return;
        double along = a + GEAR_W + CHEV_W + TRAY_ICON_GAP;
        for (auto& it : items_) {
            if (item_hidden(it)) continue;
            double ix = vert ? (t - TRAY_ICON_SIZE) / 2.0 : along;
            double iy = vert ? along : (t - TRAY_ICON_SIZE) / 2.0;
            if (it.icon) {
                double sw = cairo_image_surface_get_width(it.icon);
                cairo_save(cr);
                cairo_translate(cr, ix, iy);
                cairo_scale(cr, TRAY_ICON_SIZE / sw, TRAY_ICON_SIZE / sw);
                cairo_set_source_surface(cr, it.icon, 0, 0);
                cairo_paint(cr);
                cairo_restore(cr);
            } else { // placeholder
                cairo_set_source_rgba(cr, cfg.c_ws_bg.r, cfg.c_ws_bg.g,
                                      cfg.c_ws_bg.b, 1);
                cairo_rectangle(cr, ix, iy, TRAY_ICON_SIZE, TRAY_ICON_SIZE);
                cairo_fill(cr);
            }
            along += TRAY_ICON_SIZE + TRAY_ICON_GAP;
        }
    }

    // ---- input -----------------------------------------------------------
    bool on_click(double relx, int button) override {
        if (!bus_ && relx >= GEAR_W + CHEV_W) return false;
        if (relx < GEAR_W) {
            if (button == BTN_LEFT) bar_->toggle_settings();
            return false;
        }
        if (relx < GEAR_W + CHEV_W) {
            if (button == BTN_LEFT) {
                expanded_ = !expanded_;
                arm_collapse(expanded_);
                return true;
            }
            return false;
        }
        if (!expanded_) return false;
        int idx = item_at(relx);
        if (idx < 0) return false;
        auto& it = items_[idx];

        if (button == BTN_RIGHT ||
            (button == BTN_LEFT && it.only_menu && !it.menu.empty())) {
            if (!it.menu.empty() && bar_->wm_base()) {
                open_menu(it);
                return false;
            }
            // no dbusmenu: ask the app to show its own menu
            sd_bus_call_method_async(bus_, nullptr, it.service.c_str(),
                                     it.path.c_str(), it.iface.c_str(),
                                     "ContextMenu", nullptr, nullptr, "ii", 0,
                                     0);
            return false;
        }
        const char* method = (button == BTN_LEFT)     ? "Activate"
                             : (button == BTN_MIDDLE) ? "SecondaryActivate"
                                                      : nullptr;
        if (!method) return false;
        sd_bus_call_method_async(bus_, nullptr, it.service.c_str(),
                                 it.path.c_str(), it.iface.c_str(), method,
                                 nullptr, nullptr, "ii", 0, 0);
        return false;
    }

    void on_hover(double) override {
        if (expanded_) arm_collapse(true);
    }

    bool enabled() const override { return cfg.show_tray; }
    // One tray, on the primary bar: a second SNI host on another monitor
    // would register for the same items and fight the first.
    bool primary_only() const override { return true; }

    bool on_scroll(double relx, int dir) override {
        if (!bus_) return false;
        int idx = item_at(relx);
        if (idx < 0) return false;
        auto& it = items_[idx];
        sd_bus_call_method_async(bus_, nullptr, it.service.c_str(),
                                 it.path.c_str(), it.iface.c_str(), "Scroll",
                                 nullptr, nullptr, "is", dir * 120, "vertical");
        return false;
    }

private:
    static constexpr double GEAR_W = 18;
    static constexpr double CHEV_W = 14;
    static constexpr double ROW_H = 28, PAD_X = 14, MARGIN = 6;
    static constexpr double MENU_MIN_W = 140, MENU_MAX_W = 420;

    static bool item_named(const TrayItem& it, const char* needle) {
        auto has = [needle](const std::string& s) {
            if (s.empty() || !needle || !*needle) return false;
            std::string a = s, b = needle;
            for (char& c : a) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
            for (char& c : b) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
            return a.find(b) != std::string::npos;
        };
        return has(it.id) || has(it.title) || has(it.icon_name);
    }

    // Omarchy hides the Dropbox SNI item when the dedicated dropbox
    // widget is on the bar (left/center/right). Keep that: two copies
    // of the same status, and the native icon is the worse of them.
    // If the widget lives only in More (or is disabled), show the SNI.
    bool item_hidden(const TrayItem& it) const {
        if (!cfg.show_dropbox) return false;
        int z = cfg.layout_zone_of("dropbox");
        if (z < 0 || z > 2) return false;
        return item_named(it, "dropbox");
    }

    int item_at(double relx) const {
        double off = relx - GEAR_W - CHEV_W - TRAY_ICON_GAP;
        if (off < 0) return -1;
        int idx = static_cast<int>(off / (TRAY_ICON_SIZE + TRAY_ICON_GAP));
        int vis = 0;
        for (int i = 0; i < static_cast<int>(items_.size()); ++i) {
            if (item_hidden(items_[i])) continue;
            if (vis == idx) return i;
            ++vis;
        }
        return -1;
    }

    void process() {
        if (!bus_) return;
        ++cb_count_;
        int r;
        int msgs = 0, progress = 0;
        const bool tally = getenv("MATTBAR_DEBUG") != nullptr;
        sd_bus_message* msg = nullptr;
        while ((r = sd_bus_process(bus_, tally ? &msg : nullptr)) > 0) {
            ++progress;
            if (msg) {
                ++msgs;
                const char* mb = sd_bus_message_get_member(msg);
                const char* sn = sd_bus_message_get_sender(msg);
                ++msg_counts_[std::string(mb ? mb : "?") + "(" +
                              (sn ? sn : "?") + ")"];
                sd_bus_message_unref(msg);
                msg = nullptr;
            }
        }
        if (r < 0) {
            bus_fail(strerror(-r));
            return;
        }
        progress_count_ += progress;
        msgs_count_ += msgs;
        if (tally) dump_health();

        // Watchdog: wakeups that consume nothing mean our poll interest is
        // wrong for sd-bus's current state, or the connection is sick.
        // Either way, spinning forever is never acceptable: log the raw fd
        // state and reset the connection.
        if (progress == 0 && msgs == 0) {
            if (++barren_wakes_ > 2000) {
                char c;
                ssize_t pk = recv(bus_fd_, &c, 1, MSG_PEEK | MSG_DONTWAIT);
                uint64_t to = 0;
                sd_bus_get_timeout(bus_, &to);
                fprintf(stderr,
                        "mattbar: dbus wakeup storm (fd peek=%zd errno=%d "
                        "sd_events=0x%x timeout=%llu); resetting tray "
                        "connection\n",
                        pk, pk < 0 ? errno : 0, sd_bus_get_events(bus_),
                        (unsigned long long)to);
                bus_fail("wakeup storm with no messages");
                return;
            }
        } else {
            barren_wakes_ = 0;
        }

        // sd-bus contract: poll with exactly the events IT wants right now
        // (POLLIN and/or POLLOUT), and drive it again at its timeout. A
        // hardcoded EPOLLIN violates this and can spin when sd-bus's state
        // machine doesn't currently want to read.
        int ev = sd_bus_get_events(bus_);
        uint32_t e = 0;
        if (ev > 0) {
            if (ev & POLLIN) e |= EPOLLIN;
            if (ev & POLLOUT) e |= EPOLLOUT;
        } else if (ev == 0) {
            e = 0;
        } else {
            e = EPOLLIN; // query failed; fall back
        }
        if (e != bus_epoll_events_) {
            bus_epoll_events_ = e;
            bar_->mod_fd(bus_fd_, e);
        }
        uint64_t to = UINT64_MAX;
        sd_bus_get_timeout(bus_, &to);
        itimerspec ts{};
        int flags = 0;
        if (to == 0) {
            ts.it_value.tv_nsec = 1; // "drive me again immediately"
        } else if (to != UINT64_MAX) {
            ts.it_value.tv_sec  = to / 1000000ULL;
            ts.it_value.tv_nsec = (to % 1000000ULL) * 1000ULL;
            flags = TFD_TIMER_ABSTIME; // CLOCK_MONOTONIC absolute usec
        } // UINT64_MAX: all-zero disarms
        timerfd_settime(bus_timer_fd_, flags, &ts, nullptr);
    }

    void dump_health() {
        long now = now_ms();
        if (msg_last_ms_ == 0) msg_last_ms_ = now;
        if (now - msg_last_ms_ < 5000) return;
        double secs = (now - msg_last_ms_) / 1000.0;
        char c;
        ssize_t pk = recv(bus_fd_, &c, 1, MSG_PEEK | MSG_DONTWAIT);
        uint64_t to = UINT64_MAX;
        sd_bus_get_timeout(bus_, &to);
        if (cb_count_)
            fprintf(stderr,
                    "mattbar: dbus health over %.1fs: cb=%llu progress=%llu "
                    "msgs=%llu sd_events=0x%x timeout=%llu peek=%zd/e%d\n",
                    secs, (unsigned long long)cb_count_,
                    (unsigned long long)progress_count_,
                    (unsigned long long)msgs_count_, sd_bus_get_events(bus_),
                    (unsigned long long)to, pk, pk < 0 ? errno : 0);
        if (!msg_counts_.empty()) {
            std::string line;
            for (auto& [k, v] : msg_counts_)
                line += " " + k + "=" + std::to_string(v);
            fprintf(stderr, "mattbar: dbus messages over %.1fs:%s\n", secs,
                    line.c_str());
        }
        // Storm with (almost) no real messages: capture the raw bytes stuck
        // on the socket — the D-Bus wire header identifies the message —
        // then reset the connection rather than keep spinning.
        if (cb_count_ > 5000 && msgs_count_ < 10) {
            uint8_t raw[64];
            ssize_t rn = recv(bus_fd_, raw, sizeof raw,
                              MSG_PEEK | MSG_DONTWAIT);
            std::string hex;
            for (ssize_t i = 0; i < rn; ++i) {
                char b[4];
                snprintf(b, sizeof b, "%02x ", raw[i]);
                hex += b;
            }
            fprintf(stderr,
                    "mattbar: dbus storm: %lld wakeups, %lld msgs; socket "
                    "bytes[%zd]: %s\n",
                    (long long)cb_count_, (long long)msgs_count_, rn,
                    hex.empty() ? "(none)" : hex.c_str());
            msg_counts_.clear();
            msg_last_ms_ = now;
            cb_count_ = progress_count_ = msgs_count_ = 0;
            bus_fail("wakeup storm; see socket bytes above");
            return;
        }
        msg_counts_.clear();
        msg_last_ms_ = now;
        cb_count_ = progress_count_ = msgs_count_ = 0;
    }

    void schedule_reconnect() {
        if (reconnect_attempts_ >= 5) {
            fprintf(stderr,
                    "mattbar: giving up on D-Bus after 5 attempts; tray "
                    "disabled\n");
            return;
        }
        itimerspec ts{};
        ts.it_value.tv_sec = 3;
        timerfd_settime(reconnect_fd_, 0, &ts, nullptr);
    }

    // The bus died. A closed fd is PERMANENTLY readable: if it stays in
    // epoll the main loop spins on it at thousands of wakeups per second
    // (observed as constant ~2% CPU). Tear everything down instead.
    void bus_fail(const char* why) {
        fprintf(stderr,
                "mattbar: D-Bus connection lost (%s); tray disabled, "
                "reconnecting shortly\n", why);
        close_menu();
        for (auto& it : items_) unwatch_item(it);
        if (bus_fd_ >= 0) bar_->remove_fd(bus_fd_);
        bus_fd_ = -1;
        if (watcher_slot_) sd_bus_slot_unref(watcher_slot_);
        watcher_slot_ = nullptr;
        if (bus_) sd_bus_flush_close_unref(bus_);
        bus_ = nullptr;
        for (auto& it : items_)
            if (it.icon) cairo_surface_destroy(it.icon);
        items_.clear();
        hosts_.clear();
        barren_wakes_ = 0;
        bus_epoll_events_ = 0;
        itimerspec off{};
        timerfd_settime(bus_timer_fd_, 0, &off, nullptr); // disarm
        bar_->request_draw();
        schedule_reconnect();
    }

    // ---- item bookkeeping ------------------------------------------------
    // spec is either "/obj/path" (service = sender), ":1.42/obj/path",
    // "busname" (path defaults to /StatusNotifierItem)
    bool add_item_from_spec(const std::string& spec, const std::string& sender) {
        std::string svc, path;
        if (!spec.empty() && spec[0] == '/') {
            svc = sender;
            path = spec;
        } else if (auto slash = spec.find('/'); slash != std::string::npos) {
            svc  = spec.substr(0, slash);
            path = spec.substr(slash);
        } else {
            svc  = spec.empty() ? sender : spec;
            path = "/StatusNotifierItem";
        }
        if (svc.empty()) return false;
        for (auto& it : items_)
            if (it.service == svc && it.path == path)
                return false; // already known: NOT a new registration
        TrayItem item;
        item.service = svc;
        item.path    = path;
        item.iface   = ITEM_IFACES[0];
        fetch_menu_path(item);
        load_icon(item);
        if (item.menu.empty()) fetch_menu_path(item);
        if (!item.only_menu && !item.menu.empty() && item_named(item, "dropbox"))
            item.only_menu = true;
        watch_item(item);
        items_.push_back(item);
        bar_->request_draw();
        return true;
    }

    void watch_item(TrayItem& item) {
        std::string m1 =
            "type='signal',sender='org.freedesktop.DBus',"
            "path='/org/freedesktop/DBus',interface='org.freedesktop.DBus',"
            "member='NameOwnerChanged',arg0='" + item.service + "'";
        sd_bus_add_match(bus_, &item.watch_owner, m1.c_str(),
                         on_name_owner_changed, this);
        std::string m2 = "type='signal',sender='" + item.service +
                         "',member='NewIcon'";
        sd_bus_add_match(bus_, &item.watch_icon, m2.c_str(), on_new_icon,
                         this);
    }

    static void unwatch_item(TrayItem& item) {
        if (item.watch_owner) sd_bus_slot_unref(item.watch_owner);
        if (item.watch_icon) sd_bus_slot_unref(item.watch_icon);
        item.watch_owner = item.watch_icon = nullptr;
    }

    void remove_items_for(const std::string& svc) {
        bool changed = false;
        for (auto it = items_.begin(); it != items_.end();) {
            if (it->service == svc) {
                if (menu_ && menu_->service == svc) close_menu();
                unwatch_item(*it);
                if (it->icon) cairo_surface_destroy(it->icon);
                it = items_.erase(it);
                changed = true;
            } else {
                ++it;
            }
        }
        if (changed) {
            if (we_are_watcher_) emit_items_changed();
            bar_->request_draw();
        }
    }

    void emit_items_changed() {
        sd_bus_emit_properties_changed(bus_, WATCHER_PATH, WATCHER_IFACE,
                                       "RegisteredStatusNotifierItems",
                                       nullptr);
    }

    void fetch_menu_path(TrayItem& item) {
        sd_bus_message* reply = nullptr;
        if (sd_bus_get_property(bus_, item.service.c_str(), item.path.c_str(),
                                item.iface.c_str(), "Menu", nullptr, &reply,
                                "o") >= 0) {
            const char* mp = nullptr;
            sd_bus_message_read(reply, "o", &mp);
            if (mp && *mp && strcmp(mp, "/") != 0) item.menu = mp;
            sd_bus_message_unref(reply);
        }
    }

    static void take_string(char* s, std::string& out) {
        if (s) {
            out = s;
            free(s);
        }
    }

    // ---- icons -----------------------------------------------------------
    void load_icon(TrayItem& item) {
        if (item.icon) { cairo_surface_destroy(item.icon); item.icon = nullptr; }
        for (const char* iface : ITEM_IFACES) {
            char* name = nullptr;
            if (sd_bus_get_property_string(bus_, item.service.c_str(),
                                           item.path.c_str(), iface,
                                           "IconName", nullptr, &name) >= 0) {
                item.iface = iface;
                if (name && *name) item.icon_name = name;
                char* tp = nullptr;
                if (sd_bus_get_property_string(bus_, item.service.c_str(),
                                               item.path.c_str(), iface,
                                               "IconThemePath", nullptr,
                                               &tp) >= 0)
                    take_string(tp, item.theme_path);
                char* id = nullptr;
                if (sd_bus_get_property_string(bus_, item.service.c_str(),
                                               item.path.c_str(), iface, "Id",
                                               nullptr, &id) >= 0)
                    take_string(id, item.id);
                char* title = nullptr;
                if (sd_bus_get_property_string(bus_, item.service.c_str(),
                                               item.path.c_str(), iface,
                                               "Title", nullptr, &title) >= 0)
                    take_string(title, item.title);
                int is_menu = 0;
                if (sd_bus_get_property_trivial(
                        bus_, item.service.c_str(), item.path.c_str(), iface,
                        "ItemIsMenu", nullptr, 'b', &is_menu) >= 0)
                    item.only_menu = is_menu != 0;
                else if (!item.menu.empty() && item_named(item, "dropbox"))
                    item.only_menu = true;
                if (name && *name)
                    item.icon =
                        icon_from_theme(item.icon_name, item.theme_path);
                free(name);
                if (item.icon) return;
                if (load_pixmap(item, iface)) return;
                return; // interface answered; don't retry the other
            }
        }
    }

    bool load_pixmap(TrayItem& item, const char* iface) {
        sd_bus_message* reply = nullptr;
        if (sd_bus_get_property(bus_, item.service.c_str(), item.path.c_str(),
                                iface, "IconPixmap", nullptr, &reply,
                                "a(iiay)") < 0)
            return false;
        int best_w = 0, best_h = 0;
        std::vector<uint8_t> best;
        sd_bus_message_enter_container(reply, 'a', "(iiay)");
        while (sd_bus_message_enter_container(reply, 'r', "iiay") > 0) {
            int32_t w = 0, h = 0;
            sd_bus_message_read(reply, "ii", &w, &h);
            const void* data = nullptr;
            size_t len = 0;
            sd_bus_message_read_array(reply, 'y', &data, &len);
            sd_bus_message_exit_container(reply);
            bool better = best_w == 0 ||
                          (best_w < TRAY_ICON_SIZE && w > best_w) ||
                          (w >= TRAY_ICON_SIZE &&
                           (best_w < TRAY_ICON_SIZE || w < best_w));
            if (w > 0 && h > 0 && len == static_cast<size_t>(w) * h * 4 &&
                better) {
                best_w = w;
                best_h = h;
                best.assign(static_cast<const uint8_t*>(data),
                            static_cast<const uint8_t*>(data) + len);
            }
        }
        sd_bus_message_exit_container(reply);
        sd_bus_message_unref(reply);
        if (best.empty()) return false;

        // ARGB32 network byte order -> premultiplied native cairo ARGB32
        int stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, best_w);
        cairo_surface_t* s =
            cairo_image_surface_create(CAIRO_FORMAT_ARGB32, best_w, best_h);
        uint8_t* dst = cairo_image_surface_get_data(s);
        for (int y = 0; y < best_h; ++y) {
            uint32_t* row = reinterpret_cast<uint32_t*>(dst + y * stride);
            for (int x = 0; x < best_w; ++x) {
                const uint8_t* p = &best[(y * best_w + x) * 4];
                uint32_t a = p[0], r = p[1], g = p[2], b = p[3];
                r = r * a / 255; g = g * a / 255; b = b * a / 255;
                row[x] = (a << 24) | (r << 16) | (g << 8) | b;
            }
        }
        cairo_surface_mark_dirty(s);
        item.icon = s;
        return true;
    }

    // SVG rendering via librsvg, loaded with dlopen ON FIRST SVG ONLY.
    // Linking librsvg costs 1-3 MB of resident glib/gobject setup at every
    // startup whether or not an SVG ever appears; dlopen defers that to
    // the moment an SVG icon is actually encountered, and a system without
    // librsvg behaves exactly as a build without it used to (PNG lookup +
    // placeholder). No build-time dependency remains at all.
    struct Rsvg {
        // minimal local declarations: the three symbols we call
        struct Rect { double x, y, width, height; };
        void* (*new_from_file)(const char*, void**)         = nullptr;
        int (*render_document)(void*, cairo_t*, Rect*, void**) = nullptr;
        void (*unref)(void*)                                = nullptr;
        bool ok = false;
        Rsvg() {
            void* h = dlopen("librsvg-2.so.2", RTLD_NOW | RTLD_LOCAL);
            // g_object_unref lives in gobject, a hard dependency of
            // librsvg, so it is guaranteed present when librsvg is.
            void* g = dlopen("libgobject-2.0.so.0", RTLD_NOW | RTLD_LOCAL);
            if (!h || !g) {
                fprintf(stderr,
                        "mattbar: tray: librsvg not available; SVG icons "
                        "fall back to placeholder\n");
                return;
            }
            *(void**)&new_from_file = dlsym(h, "rsvg_handle_new_from_file");
            *(void**)&render_document =
                dlsym(h, "rsvg_handle_render_document");
            *(void**)&unref = dlsym(g, "g_object_unref");
            ok = new_from_file && render_document && unref;
        }
    };
    static Rsvg& rsvg() {
        static Rsvg r; // constructed on first SVG encounter, never before
        return r;
    }

    static cairo_surface_t* icon_from_svg(const std::string& path) {
        Rsvg& r = rsvg();
        if (!r.ok) return nullptr;
        void* h = r.new_from_file(path.c_str(), nullptr);
        if (!h) return nullptr;
        const int        px = 64; // crisp at bar size and in menus
        cairo_surface_t* s =
            cairo_image_surface_create(CAIRO_FORMAT_ARGB32, px, px);
        cairo_t*   cr = cairo_create(s);
        Rsvg::Rect vp{0, 0, (double)px, (double)px};
        int ok = r.render_document(h, cr, &vp, nullptr);
        cairo_destroy(cr);
        r.unref(h);
        if (ok && cairo_surface_status(s) == CAIRO_STATUS_SUCCESS) return s;
        cairo_surface_destroy(s);
        return nullptr;
    }

    static cairo_surface_t* icon_from_file(const std::string& path) {
        if (path.empty() || access(path.c_str(), R_OK) != 0) return nullptr;
        auto ends = [&](const char* ext) {
            size_t n = strlen(ext);
            return path.size() >= n &&
                   path.compare(path.size() - n, n, ext) == 0;
        };
        if (ends(".svg") || ends(".SVG")) return icon_from_svg(path);
        cairo_surface_t* s = cairo_image_surface_create_from_png(path.c_str());
        if (cairo_surface_status(s) == CAIRO_STATUS_SUCCESS) return s;
        cairo_surface_destroy(s);
        if (!ends(".png") && !ends(".PNG"))
            if (cairo_surface_t* svg = icon_from_svg(path)) return svg;
        return nullptr;
    }

    static cairo_surface_t* icon_from_theme(const std::string& name,
                                            const std::string& extra_path = {}) {
        if (name.empty()) return nullptr;
        if (name[0] == '/') { // absolute path
            if (cairo_surface_t* s = icon_from_file(name)) return s;
            return nullptr;
        }
        // IconThemePath: apps (Dropbox, Spotify, Steam) ship a private
        // icon dir. It may be a freedesktop theme tree (hicolor/16x16/…)
        // or a flat folder of name.png files.
        if (!extra_path.empty()) {
            if (cairo_surface_t* s =
                    icon_from_file(extra_path + "/" + name + ".png"))
                return s;
            if (cairo_surface_t* s =
                    icon_from_file(extra_path + "/" + name + ".svg"))
                return s;
            if (cairo_surface_t* s = icon_from_file(extra_path + "/" + name))
                return s;
        }
        const char* home = getenv("HOME");
        std::vector<std::string> roots;
        if (!extra_path.empty()) {
            roots.push_back(extra_path + "/hicolor");
            roots.push_back(extra_path);
        }
        if (home)
            roots.push_back(std::string(home) + "/.local/share/icons/hicolor");
        roots.push_back("/usr/share/icons/hicolor");
        roots.push_back("/usr/local/share/icons/hicolor");
        const char* sizes[] = {"16x16", "22x22", "24x24", "32x32", "48x48",
                               "64x64", "128x128"};
        const char* ctxs[]  = {"apps", "status", "devices", "panel"};
        for (auto& root : roots)
            for (const char* sz : sizes)
                for (const char* ctx : ctxs) {
                    std::string p =
                        root + "/" + sz + "/" + ctx + "/" + name + ".png";
                    if (cairo_surface_t* s = icon_from_file(p)) return s;
                }
        // No PNG anywhere: SVG-only themes/apps (scalable dir, then the
        // same size dirs, where some themes ship .svg despite the name)
        for (auto& root : roots)
            for (const char* ctx : ctxs) {
                std::string p =
                    root + "/scalable/" + ctx + "/" + name + ".svg";
                if (cairo_surface_t* s = icon_from_file(p)) return s;
            }
        for (auto& root : roots)
            for (const char* sz : sizes)
                for (const char* ctx : ctxs) {
                    std::string p = root + "/" + std::string(sz) + "/" + ctx +
                                    "/" + name + ".svg";
                    if (cairo_surface_t* s = icon_from_file(p)) return s;
                }
        std::string p = "/usr/share/pixmaps/" + name + ".png";
        if (cairo_surface_t* s = icon_from_file(p)) return s;
        p = "/usr/share/pixmaps/" + name + ".svg";
        if (cairo_surface_t* s = icon_from_file(p)) return s;
        return nullptr;
    }

    // =======================================================================
    // dbusmenu popup
    // =======================================================================
    struct Row {
        const MenuNode* node = nullptr; // null => "back" row
        bool separator = false;
    };

    struct Menu {
        std::string service, menu_path;
        MenuNode root;
        std::vector<const MenuNode*> stack; // navigation; back() = level shown
        double anchor_x = 0;

        wl_surface*  surf  = nullptr;
        FracSurface  frac; // fractional scaling (see frac.hpp)
        xdg_surface* xsurf = nullptr;
        xdg_popup*   popup = nullptr;
        int w = 0, h = 0;
        int hover = -1;
        bool mapped = false;
        std::vector<Row> rows;
    };
    std::unique_ptr<Menu> menu_;

    void open_menu(const TrayItem& item) {
        close_menu();

        // AboutToShow lets apps populate lazily; errors are fine to ignore
        sd_bus_call_method(bus_, item.service.c_str(), item.menu.c_str(),
                           MENU_IFACE, "AboutToShow", nullptr, nullptr, "i",
                           0);
        sd_bus_message* reply = nullptr;
        if (sd_bus_call_method(bus_, item.service.c_str(), item.menu.c_str(),
                               MENU_IFACE, "GetLayout", nullptr, &reply,
                               "iias", 0, -1, 0) < 0)
            return;
        uint32_t revision = 0;
        sd_bus_message_read(reply, "u", &revision);
        MenuNode root;
        parse_menu_node(reply, root);
        sd_bus_message_unref(reply);
        if (root.children.empty()) return;

        menu_ = std::make_unique<Menu>();
        menu_->service   = item.service;
        menu_->menu_path = item.menu;
        menu_->root      = std::move(root);
        menu_->stack     = {&menu_->root};
        menu_->anchor_x  = bar_->pointer_along();
        bar_->hold_open(true);
        create_popup();
    }

    void build_rows() {
        Menu& m = *menu_;
        m.rows.clear();
        if (m.stack.size() > 1) m.rows.push_back({nullptr, false}); // back
        for (const auto& c : m.stack.back()->children)
            m.rows.push_back({&c, c.separator});
        m.hover = -1;
    }

    // A level with any toggle or icon gets a left gutter so labels align.
    static double level_gutter(const Menu& m) {
        for (auto& r : m.rows)
            if (r.node && (r.node->toggle_type ||
                           !r.node->icon_name.empty() ||
                           !r.node->icon_png.empty()))
                return 22;
        return 0;
    }

    void measure() {
        Menu& m = *menu_;
        cairo_surface_t* ms =
            cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
        cairo_t* mc = cairo_create(ms);
        cairo_select_font_face(mc, cfg.font.c_str(), CAIRO_FONT_SLANT_NORMAL,
                               CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(mc, cfg.font_size);
        double maxw = MENU_MIN_W - 2 * PAD_X;
        cairo_text_extents_t ext;
        for (auto& r : m.rows) {
            std::string label = row_label(r);
            cairo_text_extents(mc, label.c_str(), &ext);
            double w = ext.x_advance;
            if (r.node && !r.node->children.empty()) w += 18; // "  >"
            if (w > maxw) maxw = w;
        }
        cairo_destroy(mc);
        cairo_surface_destroy(ms);
        m.w = static_cast<int>(
            std::min(MENU_MAX_W, maxw + 2 * PAD_X + level_gutter(m)));
        m.h = static_cast<int>(m.rows.size() * ROW_H + 2 * MARGIN);
    }

    // Decode a menu item's icon once and cache it on the node (icon-data
    // PNG bytes first, then a theme lookup by name, reusing the tray's own
    // theme walker). shared_ptr so submenu navigation copies are free.
    cairo_surface_t* menu_icon(MenuNode& n) {
        if (n.icon) return n.icon.get();
        if (n.icon_name.empty() && n.icon_png.empty()) return nullptr;
        cairo_surface_t* out = nullptr;
        if (!n.icon_png.empty()) {
            PngMem m{n.icon_png.data(), n.icon_png.size(), 0};
            cairo_surface_t* srf =
                cairo_image_surface_create_from_png_stream(png_mem_read, &m);
            if (cairo_surface_status(srf) == CAIRO_STATUS_SUCCESS) out = srf;
            else cairo_surface_destroy(srf);
        }
        if (!out && !n.icon_name.empty())
            out = icon_from_theme(n.icon_name);
        if (!out) { // negative-cache: 1x1 transparent, so we try only once
            out = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
        }
        n.icon.reset(out, cairo_surface_destroy);
        return n.icon.get();
    }

    static std::string row_label(const Row& r) {
        if (!r.node) return "< Back";
        if (r.separator) return "";
        return r.node->label.empty() ? "(item)" : r.node->label;
    }

    void create_popup() {
        Menu& m = *menu_;
        build_rows();
        measure();

        m.surf  = wl_compositor_create_surface(bar_->compositor());
        m.frac.on_change = [this] {
            if (menu_ && menu_->mapped) draw_menu();
        };
        m.frac.attach(bar_->frac_mgr(), bar_->viewporter(), m.surf);
        m.xsurf = xdg_wm_base_get_xdg_surface(bar_->wm_base(), m.surf);
        static const xdg_surface_listener xsurf_listener = {
            .configure = [](void* data, xdg_surface* xs, uint32_t serial) {
                auto* self = static_cast<TrayModule*>(data);
                xdg_surface_ack_configure(xs, serial);
                if (self->menu_) {
                    self->menu_->mapped = true;
                    self->draw_menu();
                }
            },
        };
        xdg_surface_add_listener(m.xsurf, &xsurf_listener, this);

        xdg_positioner* pos = xdg_wm_base_create_positioner(bar_->wm_base());
        xdg_positioner_set_size(pos, m.w, m.h);
        const int th = cfg_thickness();
        const int ax = static_cast<int>(m.anchor_x);
        if (cfg.position == "bottom") {
            xdg_positioner_set_anchor_rect(pos, ax, 0, 1, th);
            xdg_positioner_set_anchor(pos, XDG_POSITIONER_ANCHOR_TOP);
            xdg_positioner_set_gravity(pos, XDG_POSITIONER_GRAVITY_TOP);
            xdg_positioner_set_constraint_adjustment(
                pos, XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X);
        } else if (cfg.position == "left") {
            xdg_positioner_set_anchor_rect(pos, 0, ax, th, 1);
            xdg_positioner_set_anchor(pos, XDG_POSITIONER_ANCHOR_RIGHT);
            xdg_positioner_set_gravity(pos, XDG_POSITIONER_GRAVITY_RIGHT);
            xdg_positioner_set_constraint_adjustment(
                pos, XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y);
        } else if (cfg.position == "right") {
            xdg_positioner_set_anchor_rect(pos, 0, ax, th, 1);
            xdg_positioner_set_anchor(pos, XDG_POSITIONER_ANCHOR_LEFT);
            xdg_positioner_set_gravity(pos, XDG_POSITIONER_GRAVITY_LEFT);
            xdg_positioner_set_constraint_adjustment(
                pos, XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y);
        } else { // top
            xdg_positioner_set_anchor_rect(pos, ax, 0, 1, th);
            xdg_positioner_set_anchor(pos, XDG_POSITIONER_ANCHOR_BOTTOM);
            xdg_positioner_set_gravity(pos, XDG_POSITIONER_GRAVITY_BOTTOM);
            xdg_positioner_set_constraint_adjustment(
                pos, XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X);
        }

        m.popup = xdg_surface_get_popup(m.xsurf, nullptr, pos);
        xdg_positioner_destroy(pos);
        zwlr_layer_surface_v1_get_popup(bar_->layer_surface(), m.popup);

        static const xdg_popup_listener popup_listener = {
            .configure = [](void*, xdg_popup*, int32_t, int32_t, int32_t,
                            int32_t) {},
            .popup_done = [](void* data, xdg_popup*) {
                static_cast<TrayModule*>(data)->close_menu();
            },
            .repositioned = [](void*, xdg_popup*, uint32_t) {},
        };
        xdg_popup_add_listener(m.popup, &popup_listener, this);
        xdg_popup_grab(m.popup, bar_->seat(), bar_->last_button_serial());

        bar_->register_surface(
            m.surf,
            Bar::SurfaceHooks{
                .motion = [this](double, double y) { menu_hover(y); },
                .button = [this](int b) { menu_click(b); },
                .leave  = [this] { menu_hover(-1); },
                .scroll = {},
            });
        wl_surface_commit(m.surf); // map; compositor replies with configure
    }

    void close_menu() {
        if (!menu_) return;
        bar_->unregister_surface(menu_->surf);
        menu_->frac.destroy();
        if (menu_->popup) xdg_popup_destroy(menu_->popup);
        if (menu_->xsurf) xdg_surface_destroy(menu_->xsurf);
        if (menu_->surf) wl_surface_destroy(menu_->surf);
        menu_.reset();
        bar_->hold_open(false);
    }

    void menu_hover(double y) {
        if (!menu_ || !menu_->mapped) return;
        int idx = -1;
        if (y >= MARGIN && y < menu_->h - MARGIN)
            idx = static_cast<int>((y - MARGIN) / ROW_H);
        if (idx >= static_cast<int>(menu_->rows.size())) idx = -1;
        if (idx != menu_->hover) {
            menu_->hover = idx;
            draw_menu();
        }
    }

    void menu_click(int button) {
        if (!menu_ || button != BTN_LEFT) return;
        Menu& m = *menu_;
        if (m.hover < 0 || m.hover >= static_cast<int>(m.rows.size())) return;
        const Row& row = m.rows[m.hover];

        if (!row.node) { // back
            m.stack.pop_back();
            relayout();
            return;
        }
        if (row.separator || !row.node->enabled) return;
        if (!row.node->children.empty()) { // enter submenu
            sd_bus_call_method(bus_, m.service.c_str(), m.menu_path.c_str(),
                               MENU_IFACE, "AboutToShow", nullptr, nullptr,
                               "i", row.node->id);
            m.stack.push_back(row.node);
            relayout();
            return;
        }
        // leaf: fire the action, then close
        sd_bus_call_method_async(
            bus_, nullptr, m.service.c_str(), m.menu_path.c_str(), MENU_IFACE,
            "Event", nullptr, nullptr, "isvu", row.node->id, "clicked", "s",
            "", static_cast<uint32_t>(time(nullptr)));
        close_menu();
    }

    // resize + redraw in place after submenu navigation
    void relayout() {
        build_rows();
        measure();
        draw_menu();
    }

    void draw_menu() {
        if (!menu_ || !menu_->mapped) return;
        Menu& m = *menu_;
        void* data = nullptr;
        // HiDPI: the tray lives on the primary bar, so its menu renders at
        // the primary output's scale. Layout/hit math stays logical.
        const int sc = wl_surface_get_version(m.surf) >= 3
                           ? bar_->scale_of(bar_->primary_output())
                           : 1;
        const int bw = m.frac.active() ? m.frac.px(m.w) : m.w * sc;
        const int bh = m.frac.active() ? m.frac.px(m.h) : m.h * sc;
        wl_buffer* buffer = create_argb_buffer(bar_->shm(), bw, bh, &data);
        if (!buffer) return;

        cairo_surface_t* cs = cairo_image_surface_create_for_data(
            static_cast<unsigned char*>(data), CAIRO_FORMAT_ARGB32, bw, bh,
            bw * 4);
        cairo_t* cr = cairo_create(cs);
        cairo_scale(cr, (double)bw / m.w, (double)bh / m.h);
        cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
        cairo_set_source_rgba(cr, cfg.c_bg.r, cfg.c_bg.g, cfg.c_bg.b, 0.98);
        cairo_paint(cr);
        cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
        cairo_set_source_rgba(cr, cfg.c_ws_bg.r, cfg.c_ws_bg.g, cfg.c_ws_bg.b, 1);
        cairo_set_line_width(cr, 1);
        cairo_rectangle(cr, 0.5, 0.5, m.w - 1, m.h - 1);
        cairo_stroke(cr);

        cairo_select_font_face(cr, cfg.font.c_str(), CAIRO_FONT_SLANT_NORMAL,
                               CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, cfg.font_size);
        cairo_font_extents_t fe;
        cairo_font_extents(cr, &fe);

        double y = MARGIN;
        for (size_t i = 0; i < m.rows.size(); ++i, y += ROW_H) {
            const Row& r = m.rows[i];
            if (r.separator) {
                cairo_set_source_rgba(cr, cfg.c_ws_bg.r, cfg.c_ws_bg.g, cfg.c_ws_bg.b, 1);
                cairo_move_to(cr, PAD_X, y + ROW_H / 2.0);
                cairo_line_to(cr, m.w - PAD_X, y + ROW_H / 2.0);
                cairo_stroke(cr);
                continue;
            }
            if (static_cast<int>(i) == m.hover) {
                cairo_set_source_rgba(cr, cfg.c_ws_bg.r, cfg.c_ws_bg.g, cfg.c_ws_bg.b, 1);
                cairo_rectangle(cr, 2, y, m.w - 4, ROW_H);
                cairo_fill(cr);
            }
            bool disabled = r.node && !r.node->enabled;
            const Color& tc = disabled ? cfg.c_dim : cfg.c_fg;
            const double gut = level_gutter(m);
            if (r.node && gut > 0) {
                const double cx = PAD_X + 7, cy = y + ROW_H / 2.0;
                const bool   on = r.node->toggle_state == 1;
                const Color& ic = disabled ? cfg.c_dim
                                  : on     ? cfg.c_accent
                                           : cfg.c_dim;
                if (r.node->toggle_type == 1) { // checkmark: box + tick
                    cairo_set_source_rgba(cr, ic.r, ic.g, ic.b, 1);
                    cairo_set_line_width(cr, 1.2);
                    cairo_rectangle(cr, cx - 6, cy - 6, 12, 12);
                    cairo_stroke(cr);
                    if (on) {
                        cairo_set_line_width(cr, 1.8);
                        cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
                        cairo_move_to(cr, cx - 3.5, cy + 0.5);
                        cairo_line_to(cr, cx - 1, cy + 3);
                        cairo_line_to(cr, cx + 3.5, cy - 3.5);
                        cairo_stroke(cr);
                    } else if (r.node->toggle_state < 0) { // indeterminate
                        cairo_rectangle(cr, cx - 3, cy - 1, 6, 2);
                        cairo_fill(cr);
                    }
                } else if (r.node->toggle_type == 2) { // radio: ring + dot
                    cairo_set_source_rgba(cr, ic.r, ic.g, ic.b, 1);
                    cairo_set_line_width(cr, 1.2);
                    cairo_arc(cr, cx, cy, 6, 0, 2 * M_PI);
                    cairo_stroke(cr);
                    if (on) {
                        cairo_arc(cr, cx, cy, 3, 0, 2 * M_PI);
                        cairo_fill(cr);
                    }
                } else if (cairo_surface_t* mi = menu_icon(
                               *const_cast<MenuNode*>(r.node))) {
                    // per-item icon, scaled into a 16 px slot
                    double iw = cairo_image_surface_get_width(mi);
                    double ih = cairo_image_surface_get_height(mi);
                    if (iw > 0 && ih > 0) {
                        double sf = 16.0 / std::max(iw, ih);
                        cairo_save(cr);
                        cairo_translate(cr, cx - iw * sf / 2,
                                        cy - ih * sf / 2);
                        cairo_scale(cr, sf, sf);
                        cairo_set_source_surface(cr, mi, 0, 0);
                        cairo_paint_with_alpha(cr, disabled ? 0.4 : 1.0);
                        cairo_restore(cr);
                    }
                }
            }
            cairo_set_source_rgba(cr, tc.r, tc.g, tc.b, 1);
            cairo_move_to(cr, PAD_X + (r.node ? gut : 0),
                          y + ROW_H / 2.0 + (fe.ascent - fe.descent) / 2.0);
            cairo_show_text(cr, row_label(r).c_str());
            if (r.node && !r.node->children.empty()) {
                cairo_text_extents_t ext;
                cairo_text_extents(cr, ">", &ext);
                cairo_move_to(cr, m.w - PAD_X - ext.x_advance,
                              y + ROW_H / 2.0 + (fe.ascent - fe.descent) / 2.0);
                cairo_show_text(cr, ">");
            }
        }

        cairo_destroy(cr);
        cairo_surface_destroy(cs);

        m.frac.apply(m.surf, m.w, m.h, sc);
        wl_surface_attach(m.surf, buffer, 0, 0);
        if (wl_surface_get_version(m.surf) >= 4)
            wl_surface_damage_buffer(m.surf, 0, 0, bw, bh);
        else
            wl_surface_damage(m.surf, 0, 0, m.w, m.h);
        wl_surface_commit(m.surf);
    }

    // ---- D-Bus callbacks -------------------------------------------------
    static int method_register_item(sd_bus_message* m, void* userdata,
                                    sd_bus_error*) {
        auto* self = static_cast<TrayModule*>(userdata);
        const char* spec = nullptr;
        sd_bus_message_read(m, "s", &spec);
        const char* sender = sd_bus_message_get_sender(m);
        // Reply FIRST: the registering app blocks on this reply, and
        // add_item_from_spec makes synchronous property calls back to that
        // same app. Replying afterwards deadlocked both sides until the
        // 25 s timeout.
        int r = sd_bus_reply_method_return(m, "");
        bool added =
            self->add_item_from_spec(spec ? spec : "", sender ? sender : "");
        if (added) { // spec: broadcast only NEW registrations
            self->emit_items_changed();
            std::string full = (spec && *spec == '/')
                                   ? std::string(sender ? sender : "") + spec
                                   : std::string(spec ? spec : "");
            sd_bus_emit_signal(self->bus_, WATCHER_PATH, WATCHER_IFACE,
                               "StatusNotifierItemRegistered", "s",
                               full.c_str());
        }
        return r;
    }

    static int method_register_host(sd_bus_message* m, void* userdata,
                                    sd_bus_error*) {
        auto* self = static_cast<TrayModule*>(userdata);
        const char* host = nullptr;
        sd_bus_message_read(m, "s", &host);
        const char* sender = sd_bus_message_get_sender(m);
        std::string key = host && *host ? host : (sender ? sender : "?");
        if (self->hosts_.insert(key).second) // spec: broadcast only NEW hosts
            sd_bus_emit_signal(self->bus_, WATCHER_PATH, WATCHER_IFACE,
                               "StatusNotifierHostRegistered", "");
        return sd_bus_reply_method_return(m, "");
    }

    static int prop_items(sd_bus*, const char*, const char*, const char*,
                          sd_bus_message* reply, void* userdata,
                          sd_bus_error*) {
        auto* self = static_cast<TrayModule*>(userdata);
        sd_bus_message_open_container(reply, 'a', "s");
        for (auto& it : self->items_) {
            std::string s = it.service + it.path;
            sd_bus_message_append(reply, "s", s.c_str());
        }
        return sd_bus_message_close_container(reply);
    }

    static int prop_host_registered(sd_bus*, const char*, const char*,
                                    const char*, sd_bus_message* reply, void*,
                                    sd_bus_error*) {
        return sd_bus_message_append(reply, "b", 1);
    }

    static int prop_protocol_version(sd_bus*, const char*, const char*,
                                     const char*, sd_bus_message* reply, void*,
                                     sd_bus_error*) {
        return sd_bus_message_append(reply, "i", 0);
    }

    static int on_ext_registered(sd_bus_message* m, void* userdata,
                                 sd_bus_error*) {
        auto* self = static_cast<TrayModule*>(userdata);
        const char* spec = nullptr;
        sd_bus_message_read(m, "s", &spec);
        if (spec) self->add_item_from_spec(spec, "");
        return 0;
    }

    static int on_ext_unregistered(sd_bus_message* m, void* userdata,
                                   sd_bus_error*) {
        auto* self = static_cast<TrayModule*>(userdata);
        const char* spec = nullptr;
        sd_bus_message_read(m, "s", &spec);
        if (spec) {
            std::string s(spec);
            auto slash = s.find('/');
            self->remove_items_for(slash == std::string::npos
                                       ? s
                                       : s.substr(0, slash));
        }
        return 0;
    }

    static int on_name_owner_changed(sd_bus_message* m, void* userdata,
                                     sd_bus_error*) {
        auto* self = static_cast<TrayModule*>(userdata);
        const char *name = nullptr, *old_o = nullptr, *new_o = nullptr;
        sd_bus_message_read(m, "sss", &name, &old_o, &new_o);
        if (name && new_o && *new_o == '\0') self->remove_items_for(name);
        return 0;
    }

    static int on_new_icon(sd_bus_message* m, void* userdata, sd_bus_error*) {
        auto* self = static_cast<TrayModule*>(userdata);
        const char* sender = sd_bus_message_get_sender(m);
        if (!sender) return 0;
        for (auto& it : self->items_)
            if (it.service == sender) {
                // Some apps emit NewIcon in bursts or animate their icon;
                // a full reload (D-Bus + theme search) once per second is
                // plenty for a 20 px tray icon.
                long now = now_ms();
                if (now - it.icon_ms < 1000) continue;
                it.icon_ms = now;
                self->load_icon(it);
                self->bar_->request_draw();
            }
        return 0;
    }

    static const sd_bus_vtable watcher_vtable[];

    Bar* bar_ = nullptr;
    sd_bus* bus_ = nullptr;
    sd_bus_slot* watcher_slot_ = nullptr;
    bool we_are_watcher_ = false;
    bool expanded_ = cfg.tray_start_expanded;
    std::string host_name_;
    std::vector<TrayItem> items_;
    int collapse_fd_ = -1;
    int armed_ms_    = -1;  // duration of the running countdown; -1 = none
    int bus_fd_ = -1;
    int bus_timer_fd_ = -1;
    int reconnect_fd_ = -1;
    int reconnect_attempts_ = 0;
    uint32_t bus_epoll_events_ = 0;
    uint64_t barren_wakes_ = 0;
    uint64_t cb_count_ = 0, progress_count_ = 0, msgs_count_ = 0;
    std::set<std::string> hosts_; // hosts we've already announced
    std::map<std::string, uint64_t> msg_counts_;
    long msg_last_ms_ = 0;
};

const sd_bus_vtable TrayModule::watcher_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("RegisterStatusNotifierItem", "s", "",
                  TrayModule::method_register_item, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("RegisterStatusNotifierHost", "s", "",
                  TrayModule::method_register_host, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_PROPERTY("RegisteredStatusNotifierItems", "as",
                    TrayModule::prop_items, 0,
                    SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("IsStatusNotifierHostRegistered", "b",
                    TrayModule::prop_host_registered, 0,
                    SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("ProtocolVersion", "i", TrayModule::prop_protocol_version,
                    0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_SIGNAL("StatusNotifierItemRegistered", "s", 0),
    SD_BUS_SIGNAL("StatusNotifierItemUnregistered", "s", 0),
    SD_BUS_SIGNAL("StatusNotifierHostRegistered", nullptr, 0),
    SD_BUS_VTABLE_END,
};

} // namespace

Module* make_tray() { return new TrayModule; }

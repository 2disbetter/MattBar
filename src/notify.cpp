#include "notify.hpp"
#include "popup.hpp"
#include "bar.hpp"
#include "config.hpp"
#include "sdpump.hpp"
#include "audio.hpp"
#include "modules.hpp" // AsyncCmd
#include "shm.hpp"
#include "util.hpp"

#include "wlr-layer-shell-unstable-v1-client-protocol.h"

#include <cairo/cairo.h>
#include <linux/input-event-codes.h>
#include <linux/netlink.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <csignal>
#include <cmath>
#include <dirent.h>
#include <sstream>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/wait.h>

#include <algorithm>
#include <deque>
#include <memory>
#include <vector>

#ifndef DBG
#define DBG(...)                                                              \
    do {                                                                      \
        if (getenv("MATTBAR_DEBUG")) {                                        \
            fprintf(stderr, "mattbar: " __VA_ARGS__);                         \
            fputc('\n', stderr);                                              \
        }                                                                     \
    } while (0)
#endif

namespace {

void col(cairo_t* cr, const Color& c, double a = -1) {
    cairo_set_source_rgba(cr, c.r, c.g, c.b, a < 0 ? c.a : a);
}

constexpr int    NOTE_W    = 360;
constexpr int    NOTE_PAD  = 12;
constexpr int    MAX_SHOWN = 5;
constexpr size_t HIST_MAX  = 30;

struct Note {
    uint32_t                                         id = 0;
    std::string                                      app, summary, body;
    int                                              urgency = 1;
    std::vector<std::pair<std::string, std::string>> actions; // key,label
    uint64_t         expires_at = 0;                          // ms mono, 0=never
    uint64_t         posted_at  = 0;                          // wall clock, s
    cairo_surface_t* icon       = nullptr;
    int              height     = 0; // computed at draw
};

// Icons draw at 32 px logical; retaining them any larger is pure waste.
constexpr int ICON_KEEP = 96;
cairo_surface_t* downscale_icon(cairo_surface_t* s) {
    if (!s) return nullptr;
    int w = cairo_image_surface_get_width(s);
    int h = cairo_image_surface_get_height(s);
    if (w <= ICON_KEEP && h <= ICON_KEEP) return s;
    double sf = (double)ICON_KEEP / std::max(w, h);
    int nw = std::max(1, (int)(w * sf + 0.5));
    int nh = std::max(1, (int)(h * sf + 0.5));
    cairo_surface_t* d =
        cairo_image_surface_create(CAIRO_FORMAT_ARGB32, nw, nh);
    cairo_t* cr = cairo_create(d);
    cairo_scale(cr, (double)nw / w, (double)nh / h);
    cairo_set_source_surface(cr, s, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
    cairo_paint(cr);
    cairo_destroy(cr);
    cairo_surface_destroy(s);
    return d;
}

void free_note_icon(Note& n) {
    if (n.icon) cairo_surface_destroy(n.icon);
    n.icon = nullptr;
}

uint64_t now_ms() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000ULL;
}

std::string strip_markup(const std::string& s) {
    std::string out;
    bool        in = false;
    for (char c : s) {
        if (c == '<') in = true;
        else if (c == '>') in = false;
        else if (!in) out += c;
    }
    // minimal entity handling for the common ones
    auto sub = [&](const char* a, const char* b) {
        size_t p;
        while ((p = out.find(a)) != std::string::npos)
            out.replace(p, strlen(a), b);
    };
    sub("&amp;", "&"); sub("&lt;", "<"); sub("&gt;", ">"); sub("&quot;", "\"");
    return out;
}


// --------------------------------------------------------------------------- Mixed bar-font / emoji-font text.
namespace {
struct RichRun {
    std::string text;
    bool        emoji;
};

std::string strip_joiners(const std::string& in) {
    std::string out;
    for (size_t i = 0; i < in.size();) {
        unsigned char c = in[i];
        size_t        n = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        std::string   cp = in.substr(i, n);
        i += n;
        if (cp == "\u200D" || cp == "\uFE0E" || cp == "\uFE0F") continue;
        out += cp;
    }
    return out;
}

std::vector<RichRun> rich_split(cairo_t* cr, const std::string& s) {
    std::vector<RichRun> runs;
    cairo_scaled_font_t* sf = cairo_get_scaled_font(cr);
    cairo_glyph_t*       g  = nullptr;
    int                  ng = 0;
    cairo_text_cluster_t* cl = nullptr;
    int                   ncl = 0;
    cairo_text_cluster_flags_t fl{};
    if (cairo_scaled_font_text_to_glyphs(sf, 0, 0, s.c_str(), (int)s.size(),
                                         &g, &ng, &cl, &ncl, &fl)
            != CAIRO_STATUS_SUCCESS || ncl <= 0) {
        if (g) cairo_glyph_free(g);
        if (cl) cairo_text_cluster_free(cl);
        runs.push_back({s, false});
        return runs;
    }
    size_t byte = 0;
    int    gi   = 0;
    for (int i = 0; i < ncl; ++i) {
        bool unmapped = false;
        for (int k = 0; k < cl[i].num_glyphs; ++k)
            if (g[gi + k].index == 0) unmapped = true;
        gi += cl[i].num_glyphs;
        std::string cluster = s.substr(byte, cl[i].num_bytes);
        byte += cl[i].num_bytes;
        if (!runs.empty() && runs.back().emoji == unmapped)
            runs.back().text += cluster;
        else
            runs.push_back({cluster, unmapped});
    }
    cairo_glyph_free(g);
    cairo_text_cluster_free(cl);
    return runs;
}

void rich_sel(cairo_t* cr, bool emoji, double size) {
    cairo_select_font_face(cr,
                           emoji ? cfg.notification_emoji_font.c_str()
                                 : cfg.font.c_str(),
                           CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, size);
}
} // namespace
} // namespace (file-local helpers; the rich-text API below is public)

double rich_text_width(cairo_t* cr, const std::string& s, double size) {
    rich_sel(cr, false, size);
    double w = 0;
    for (auto& r : rich_split(cr, s)) {
        std::string t = r.emoji ? strip_joiners(r.text) : r.text;
        rich_sel(cr, r.emoji, size);
        cairo_text_extents_t e;
        cairo_text_extents(cr, t.c_str(), &e);
        w += e.x_advance;
        rich_sel(cr, false, size);
    }
    return w;
}

void draw_rich_text(cairo_t* cr, const std::string& s, double x, double y,
                    double size) {
    rich_sel(cr, false, size);
    for (auto& r : rich_split(cr, s)) {
        std::string t = r.emoji ? strip_joiners(r.text) : r.text;
        rich_sel(cr, r.emoji, size);
        cairo_move_to(cr, x, y);
        cairo_show_text(cr, t.c_str());
        cairo_text_extents_t e;
        cairo_text_extents(cr, t.c_str(), &e);
        x += e.x_advance;
        rich_sel(cr, false, size);
    }
}

namespace { // reopen file-local helpers

std::vector<std::string> wrap(cairo_t* cr, const std::string& text,
                              double maxw, size_t max_lines) {
    std::vector<std::string> lines;
    std::string              cur;
    auto width = [&](const std::string& s) {
        return rich_text_width(cr, s, cfg.font_size);
    };
    size_t i = 0;
    while (i < text.size() && lines.size() < max_lines) {
        size_t sp = text.find_first_of(" \n", i);
        std::string word =
            text.substr(i, (sp == std::string::npos ? text.size() : sp) - i);
        std::string cand = cur.empty() ? word : cur + " " + word;
        if (!cur.empty() && width(cand) > maxw) {
            lines.push_back(cur);
            cur = word;
        } else {
            cur = cand;
        }
        i = sp == std::string::npos ? text.size() : sp + 1;
    }
    if (!cur.empty() && lines.size() < max_lines) lines.push_back(cur);
    if (i < text.size() && !lines.empty()) lines.back() += "\u2026";
    return lines;
}

// A small anchored layer surface (notification stack / OSD).

NotifyDaemon* g_daemon = nullptr;

} // namespace

struct NotifyDaemon::Impl {
    Bar*    bar = nullptr;
    SdPump  pump;
    sd_bus* bus       = nullptr;
    bool    owns      = false;
    bool    name_req  = false;
    bool    dnd       = false;
    uint32_t next_id  = 1;

    std::deque<Note> active;  // newest first
    std::deque<Note> history; // dismissed/expired, newest first

    PopupWin win, osd;
    int      expiry_fd = -1, osd_fd = -1, sig_fd = -1;

    // OSD sources
    int    audio_sub = 0; // shared AudioEvents subscription (0 = none)
    AsyncCmd vol_cmd, mic_cmd;
    double last_vol = -1, last_mic = -1;
    int    last_vol_mut = -1, last_mic_mut = -1;
    int   uevent_fd = -1;
    std::string backlight;

    // ---- D-Bus server -----------------------------------------------------
    bool ensure_bus() {
        if (bus) return true;
        sd_bus* b = nullptr;
        if (sd_bus_open_user(&b) < 0) {
            fprintf(stderr, "mattbar: notifications: session bus unavailable\n");
            return false;
        }
        bus = b;
        sd_bus_set_method_call_timeout(bus, 500 * 1000ULL);
        static const sd_bus_vtable vt[] = {
            SD_BUS_VTABLE_START(0),
            SD_BUS_METHOD("Notify", "susssasa{sv}i", "u", m_notify, 0),
            SD_BUS_METHOD("CloseNotification", "u", "", m_close, 0),
            SD_BUS_METHOD("GetCapabilities", "", "as", m_caps, 0),
            SD_BUS_METHOD("GetServerInformation", "", "ssss", m_info, 0),
            SD_BUS_SIGNAL("NotificationClosed", "uu", 0),
            SD_BUS_SIGNAL("ActionInvoked", "us", 0),
            SD_BUS_VTABLE_END,
        };
        sd_bus_add_object_vtable(bus, nullptr, "/org/freedesktop/Notifications",
                                 "org.freedesktop.Notifications", vt, this);
        sd_bus_add_match(bus, nullptr,
                         "type='signal',sender='org.freedesktop.DBus',"
                         "path='/org/freedesktop/DBus',"
                         "interface='org.freedesktop.DBus',"
                         "member='NameOwnerChanged',"
                         "arg0='org.freedesktop.Notifications'",
                         on_owner_changed, this);
        pump.on_teardown = [this](const char* why) {
            fprintf(stderr, "mattbar: notifications disabled (%s)\n", why);
            bus  = nullptr;
            owns = name_req = false;
        };
        pump.attach(*bar, bus, "notifyd");
        pump.process();
        return true;
    }

    void acquire() {
        if (!ensure_bus() || name_req) return;
        int r = sd_bus_request_name(bus, "org.freedesktop.Notifications",
                                    SD_BUS_NAME_REPLACE_EXISTING |
                                        SD_BUS_NAME_QUEUE);
        name_req = true;
        update_owner();
        DBG("notifyd: request_name -> %d, owner=%d", r, owns);
        if (!owns) takeover();
        pump.process();
    }
    // We could not replace the current owner (mako requests the name with no ALLOW_REPLACEMENT).
    void takeover() {
        if (owns || !bus || !cfg.notifications_takeover) return;
        sd_bus_error    e   = SD_BUS_ERROR_NULL;
        sd_bus_message* m   = nullptr;
        uint32_t        pid = 0;
        if (sd_bus_call_method(bus, "org.freedesktop.DBus",
                               "/org/freedesktop/DBus",
                               "org.freedesktop.DBus",
                               "GetConnectionUnixProcessID", &e, &m,
                               "s", "org.freedesktop.Notifications")
            >= 0) {
            sd_bus_message_read(m, "u", &pid);
            sd_bus_message_unref(m);
        }
        sd_bus_error_free(&e);
        if (pid == 0 || pid == (uint32_t)getpid()) return;
        std::string comm =
            trim(slurp("/proc/" + std::to_string(pid) + "/comm"));
        if (comm == "mako" || comm == "dunst") {
            fprintf(stderr,
                    "mattbar: notifyd: taking over the notification "
                    "name from %s pid %u (SIGTERM)\n", comm.c_str(), pid);
            kill((pid_t)pid, SIGTERM);
            return;
        }
        fprintf(stderr,
                "mattbar: notifyd: notification name is owned by '%s' "
                "(pid %u) — NOT terminating it (it may be your shell). "
                "MattBar notifications stay queued behind it; set "
                "notifications_takeover = false or disable the other "
                "daemon to choose one.\n",
                comm.empty() ? "?" : comm.c_str(), pid);
    }

    void release() {
        if (!bus || !name_req) return;
        sd_bus_release_name(bus, "org.freedesktop.Notifications");
        name_req = false;
        owns     = false;
        pump.process();
    }
    void update_owner() {
        owns = false;
        if (!bus) return;
        sd_bus_error  e = SD_BUS_ERROR_NULL;
        sd_bus_message* m = nullptr;
        const char* uniq = nullptr;
        sd_bus_get_unique_name(bus, &uniq);
        if (sd_bus_call_method(bus, "org.freedesktop.DBus",
                               "/org/freedesktop/DBus", "org.freedesktop.DBus",
                               "GetNameOwner", &e, &m, "s",
                               "org.freedesktop.Notifications") >= 0) {
            const char* o = nullptr;
            sd_bus_message_read(m, "s", &o);
            owns = o && uniq && !strcmp(o, uniq);
            sd_bus_message_unref(m);
        }
        sd_bus_error_free(&e);
    }
    static int on_owner_changed(sd_bus_message* m, void* ud, sd_bus_error*) {
        auto*       self = static_cast<Impl*>(ud);
        const char *n = nullptr, *o = nullptr, *nw = nullptr;
        if (sd_bus_message_read(m, "sss", &n, &o, &nw) < 0) return 0;
        const char* uniq = nullptr;
        sd_bus_get_unique_name(self->bus, &uniq);
        bool was    = self->owns;
        self->owns  = nw && uniq && !strcmp(nw, uniq);
        if (self->owns != was)
            DBG("notifyd: %s the notification name",
                self->owns ? "acquired" : "lost");
        return 0;
    }

    static int m_info(sd_bus_message* c, void*, sd_bus_error*) {
        return sd_bus_reply_method_return(c, "ssss", "mattbar", "mattbar",
                                          MATTBAR_VERSION, "1.2");
    }
    static int m_caps(sd_bus_message* c, void*, sd_bus_error*) {
        return sd_bus_reply_method_return(c, "as", 3, "body", "actions",
                                          "persistence");
    }
    static int m_close(sd_bus_message* c, void* ud, sd_bus_error*) {
        auto*    self = static_cast<Impl*>(ud);
        uint32_t id   = 0;
        if (sd_bus_message_read(c, "u", &id) >= 0)
            self->close_note(id, 3); // 3 = closed by CloseNotification
        return sd_bus_reply_method_return(c, "");
    }
    static int m_notify(sd_bus_message* c, void* ud, sd_bus_error*) {
        auto* self = static_cast<Impl*>(ud);
        Note  n;
        const char *app = nullptr, *icon = nullptr, *sum = nullptr,
                   *body = nullptr;
        uint32_t replaces = 0;
        if (sd_bus_message_read(c, "sus", &app, &replaces, &icon) < 0)
            return sd_bus_reply_method_return(c, "u", 0u);
        sd_bus_message_read(c, "ss", &sum, &body);
        n.app     = app ? app : "";
        n.summary = strip_markup(sum ? sum : "");
        n.body    = strip_markup(body ? body : "");
        // actions: flat list of key,label pairs
        if (sd_bus_message_enter_container(c, 'a', "s") >= 0) {
            const char* v = nullptr;
            std::string key;
            int         i = 0;
            while (sd_bus_message_read(c, "s", &v) > 0) {
                if (i++ % 2 == 0) key = v ? v : "";
                else n.actions.emplace_back(key, v ? v : "");
            }
            sd_bus_message_exit_container(c);
        }
        int32_t expire = -1;
        self->read_hints(c, n);
        sd_bus_message_read(c, "i", &expire);
        if (!n.icon && icon && icon[0] == '/' &&
            strstr(icon, ".png")) { // PNG paths: cairo-native, no new deps
            cairo_surface_t* s = cairo_image_surface_create_from_png(icon);
            if (cairo_surface_status(s) == CAIRO_STATUS_SUCCESS)
                n.icon = downscale_icon(s);
            else cairo_surface_destroy(s);
        }
        uint64_t timeout =
            expire > 0 ? (uint64_t)expire
                       : (n.urgency >= 2 || cfg.notification_timeout_s <= 0
                              ? 0
                              : cfg.notification_timeout_s * 1000ULL);
        n.expires_at = timeout ? now_ms() + timeout : 0;
        n.posted_at  = (uint64_t)time(nullptr);
        if (replaces) self->erase_note(replaces);
        n.id = replaces ? replaces : self->next_id++;
        // Remember the app so settings can offer a mute toggle for it even when it isn't running.
        cfg.note_app(n.app);
        // A muted app never pops up — but it is still recorded, so muting costs you nothing: everything is in the bell's history.
        if (cfg.app_muted(n.app)) {
            DBG("notifyd: #%u [%s] muted -> history only", n.id,
                n.app.c_str());
            Note copy = n;
            n.icon = nullptr; // ownership moves to history
            self->to_history(std::move(copy));
            self->redraw();
            return sd_bus_reply_method_return(c, "u", n.id);
        }
        self->active.push_front(n);
        self->mark_dirty(); // active notes persist too (as history)
        DBG("notifyd: #%u [%s] '%s' / '%s'%s", n.id, n.app.c_str(),
            n.summary.c_str(), n.body.c_str(),
            self->dnd && n.urgency < 2 ? " (dnd: hidden)" : "");
        self->arm_expiry();
        self->redraw();
        return sd_bus_reply_method_return(c, "u", n.id);
    }

    void read_hints(sd_bus_message* c, Note& n) {
        if (sd_bus_message_enter_container(c, 'a', "{sv}") < 0) return;
        while (sd_bus_message_enter_container(c, 'e', "sv") > 0) {
            const char* key = nullptr;
            sd_bus_message_read(c, "s", &key);
            std::string k    = key ? key : "";
            bool        used = false;
            if (k == "urgency") {
                uint8_t u = 1;
                if (sd_bus_message_enter_container(c, 'v', "y") >= 0) {
                    sd_bus_message_read(c, "y", &u);
                    sd_bus_message_exit_container(c);
                    n.urgency = u;
                    used      = true;
                }
            } else if (k == "image-data" || k == "image_data" ||
                       k == "icon_data") {
                if (sd_bus_message_enter_container(c, 'v', "(iiibiiay)") >= 0) {
                    read_image(c, n);
                    sd_bus_message_exit_container(c);
                    used = true;
                }
            }
            if (!used) sd_bus_message_skip(c, "v");
            sd_bus_message_exit_container(c);
        }
        sd_bus_message_exit_container(c);
    }

    void read_image(sd_bus_message* c, Note& n) {
        if (sd_bus_message_enter_container(c, 'r', "iiibiiay") < 0) return;
        int32_t w = 0, h = 0, stride = 0, bps = 0, ch = 0;
        int     has_a = 0;
        sd_bus_message_read(c, "iiibii", &w, &h, &stride, &has_a, &bps, &ch);
        const void* data = nullptr;
        size_t      len  = 0;
        sd_bus_message_read_array(c, 'y', &data, &len);
        sd_bus_message_exit_container(c);
        if (w <= 0 || h <= 0 || w > 512 || h > 512 || bps != 8 ||
            (ch != 3 && ch != 4) || !data ||
            len < (size_t)stride * (h - 1) + (size_t)w * ch)
            return;
        cairo_surface_t* s =
            cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
        if (cairo_surface_status(s) != CAIRO_STATUS_SUCCESS) {
            cairo_surface_destroy(s);
            return;
        }
        auto* dst   = cairo_image_surface_get_data(s);
        int   dstst = cairo_image_surface_get_stride(s);
        auto* src   = static_cast<const uint8_t*>(data);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const uint8_t* p = src + y * stride + x * ch;
                uint8_t a = ch == 4 && has_a ? p[3] : 255;
                auto*   q = reinterpret_cast<uint32_t*>(dst + y * dstst) + x;
                *q = (uint32_t)a << 24 | (uint32_t)(p[0] * a / 255) << 16 |
                     (uint32_t)(p[1] * a / 255) << 8 | (uint32_t)(p[2] * a / 255);
            }
        cairo_surface_mark_dirty(s);
        free_note_icon(n);
        n.icon = downscale_icon(s);
    }

    // ---- lifecycle --------------------------------------------------------
    void erase_note(uint32_t id) {
        for (auto it = active.begin(); it != active.end(); ++it)
            if (it->id == id) {
                free_note_icon(*it);
                active.erase(it);
                mark_dirty();
                return;
            }
    }
    void to_history(Note&& n) {
        history.push_front(std::move(n));
        while (history.size() > (size_t)std::max(5, cfg.history_max)) {
            free_note_icon(history.back());
            history.pop_back();
        }
        mark_dirty();
    }

    // ---- crash-safe state ------------------------------------------------- History, DND, and the id counter survive restarts: serialized to $XDG_STATE_HOME/mattbar (tiny; icons ride along as the already-96px PNGs).
    std::string state_dir;
    int         save_fd     = -1;
    bool        state_dirty = false;

    static std::string state_home() {
        const char* s = getenv("XDG_STATE_HOME");
        if (s && *s) return std::string(s) + "/mattbar";
        const char* h = getenv("HOME");
        return std::string(h ? h : ".") + "/.local/state/mattbar";
    }
    void mark_dirty() {
        state_dirty = true;
        if (save_fd < 0) return;
        itimerspec ts{};
        ts.it_value.tv_nsec = 500 * 1000000L;
        timerfd_settime(save_fd, 0, &ts, nullptr);
    }
    static void esc(std::string& out, const std::string& s) {
        for (char c : s) {
            if (c == '%')         out += "%25";
            else if (c == '\n')   out += "%0A";
            else if (c == '\x1f') out += "%1F";
            else out += c;
        }
    }
    static std::string unesc(const std::string& s) {
        std::string out;
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '%' && i + 2 < s.size()) {
                out += (char)strtol(s.substr(i + 1, 2).c_str(), nullptr, 16);
                i += 2;
            } else out += s[i];
        }
        return out;
    }
    void save_state() {
        if (state_dir.empty()) return;
        state_dirty = false;
        std::string tmp = state_dir + "/notes.tmp";
        FILE* f = fopen(tmp.c_str(), "w");
        if (!f) return;
        fprintf(f, "v1 dnd=%d next_id=%u\n", dnd ? 1 : 0, next_id);
        std::vector<std::string> keep_icons;
        auto put = [&](const Note& n) {
            std::string icon_file;
            if (n.icon) {
                icon_file = "icon-" + std::to_string(n.id) + ".png";
                std::string p = state_dir + "/" + icon_file;
                if (access(p.c_str(), F_OK) != 0)
                    cairo_surface_write_to_png(n.icon, p.c_str());
                keep_icons.push_back(icon_file);
            }
            std::string a, s, b;
            esc(a, n.app); esc(s, n.summary); esc(b, n.body);
            fprintf(f, "%u\x1f%d\x1f%llu\x1f%s\x1f%s\x1f%s\x1f%s\n", n.id,
                    n.urgency, (unsigned long long)n.posted_at,
                    icon_file.c_str(), a.c_str(), s.c_str(), b.c_str());
        };
        // Active popups persist as history: after a restart they cannot be live again (expiry clocks and senders died with the ...
        for (auto& n : active) put(n);
        for (auto& n : history) put(n);
        fclose(f);
        rename(tmp.c_str(), (state_dir + "/notes").c_str());
        // drop icon files nothing references any more
        if (DIR* d = opendir(state_dir.c_str())) {
            while (dirent* e = readdir(d)) {
                std::string name = e->d_name;
                if (name.rfind("icon-", 0) != 0) continue;
                bool ref = false;
                for (auto& k : keep_icons)
                    if (k == name) { ref = true; break; }
                if (!ref) unlink((state_dir + "/" + name).c_str());
            }
            closedir(d);
        }
    }
    void load_state() {
        std::string data = slurp((state_dir + "/notes").c_str());
        if (data.empty()) return;
        std::istringstream in(data);
        std::string line;
        if (!std::getline(in, line)) return;
        if (line.rfind("v1 ", 0) != 0) return; // unknown format: start fresh
        int d = 0; unsigned nid = 1;
        sscanf(line.c_str(), "v1 dnd=%d next_id=%u", &d, &nid);
        dnd     = d != 0;
        next_id = std::max(next_id, nid);
        while (std::getline(in, line) &&
               history.size() < (size_t)std::max(5, cfg.history_max)) {
            std::vector<std::string> fld;
            size_t p = 0, q;
            while ((q = line.find('\x1f', p)) != std::string::npos) {
                fld.push_back(line.substr(p, q - p));
                p = q + 1;
            }
            fld.push_back(line.substr(p));
            if (fld.size() != 7) continue;
            Note n;
            n.id        = (uint32_t)strtoul(fld[0].c_str(), nullptr, 10);
            n.urgency   = atoi(fld[1].c_str());
            n.posted_at = strtoull(fld[2].c_str(), nullptr, 10);
            n.app       = unesc(fld[4]);
            n.summary   = unesc(fld[5]);
            n.body      = unesc(fld[6]);
            if (!fld[3].empty()) {
                cairo_surface_t* s = cairo_image_surface_create_from_png(
                    (state_dir + "/" + fld[3]).c_str());
                if (s && cairo_surface_status(s) == CAIRO_STATUS_SUCCESS)
                    n.icon = s;
                else if (s)
                    cairo_surface_destroy(s);
            }
            history.push_back(std::move(n)); // file is newest-first already
        }
        DBG("notifyd: restored %zu notes from state%s", history.size(),
            dnd ? " (dnd on)" : "");
    }
    // ---- history, for the bell module ------------------------------------
    std::vector<NoteRecord> history_snapshot() const {
        std::vector<NoteRecord> out;
        uint64_t nowsec = (uint64_t)time(nullptr);
        // Anything still on screen belongs at the top of the list too: opening the bell shouldn't hide what is currently showing.
        auto add = [&](const Note& n, bool live) {
            NoteRecord r;
            r.id      = n.id;
            r.app     = n.app;
            r.summary = n.summary;
            r.body    = n.body;
            r.urgency = n.urgency;
            r.age_s   = n.posted_at && nowsec > n.posted_at
                            ? nowsec - n.posted_at
                            : 0;
            r.active  = live;
            // Only ACTIVE notes are invokable: once a notification closed, the app was told so and stopped listening for its actions.
            if (live) {
                if (auto* a = default_action(n)) {
                    r.has_action = true;
                    r.action_label =
                        a->second.empty() || a->first == "default"
                            ? "open"
                            : a->second;
                }
            }
            out.push_back(std::move(r));
        };
        for (auto& n : active) add(n, true);
        for (auto& n : history) add(n, false);
        if (out.size() > (size_t)std::max(5, cfg.history_max))
            out.resize((size_t)std::max(5, cfg.history_max));
        return out;
    }

    void clear_history() {
        for (auto& n : history) free_note_icon(n);
        history.clear();
        mark_dirty();
    }

    void close_note(uint32_t id, uint32_t reason) {
        for (auto it = active.begin(); it != active.end(); ++it)
            if (it->id == id) {
                if (bus)
                    sd_bus_emit_signal(bus, "/org/freedesktop/Notifications",
                                       "org.freedesktop.Notifications",
                                       "NotificationClosed", "uu", id, reason);
                DBG("notifyd: closed #%u reason %u", id, reason);
                to_history(std::move(*it));
                active.erase(it);
                arm_expiry();
                redraw();
                return;
            }
    }
    void arm_expiry() {
        uint64_t next = 0;
        for (auto& n : active)
            if (n.expires_at && (!next || n.expires_at < next))
                next = n.expires_at;
        itimerspec ts{};
        if (next) {
            uint64_t in = next > now_ms() ? next - now_ms() : 1;
            ts.it_value.tv_sec  = in / 1000;
            ts.it_value.tv_nsec = (in % 1000) * 1000000L;
        }
        timerfd_settime(expiry_fd, 0, &ts, nullptr);
    }
    void on_expiry() {
        uint64_t t = now_ms();
        for (auto it = active.begin(); it != active.end();) {
            if (it->expires_at && it->expires_at <= t) {
                uint32_t id = it->id;
                ++it;
                close_note(id, 1); // 1 = expired
                it = active.begin();
                t  = now_ms();
            } else
                ++it;
        }
        arm_expiry();
    }

    // ---- rendering --------------------------------------------------------
    std::vector<const Note*> shown() const {
        std::vector<const Note*> v;
        for (auto& n : active) {
            if (dnd && n.urgency < 2) continue;
            v.push_back(&n);
            if ((int)v.size() >= cfg.notification_max_shown) break;
        }
        return v;
    }

    void redraw() {
        if (!bar || !bar->compositor()) return; // headless
        auto v = shown();
        if (v.empty()) {
            win.destroy();
            return;
        }
        // measure with a throwaway context
        cairo_surface_t* ms = cairo_image_surface_create(CAIRO_FORMAT_A8, 1, 1);
        cairo_t*         mc = cairo_create(ms);
        cairo_select_font_face(mc, cfg.font.c_str(), CAIRO_FONT_SLANT_NORMAL,
                               CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(mc, cfg.font_size);
        int total = 0;
        for (auto* np : v) {
            auto*  n     = const_cast<Note*>(np);
            double textw = NOTE_W - 2 * NOTE_PAD - (n->icon ? 42 : 0);
            auto   lines = wrap(mc, n->body, textw, 3);
            n->height    = NOTE_PAD * 2 + 18 + (int)lines.size() * 17;
            if (n->icon) n->height = std::max(n->height, NOTE_PAD * 2 + 34);
            total += n->height + 8;
        }
        cairo_destroy(mc);
        cairo_surface_destroy(ms);
        total -= 8;
        int mt = cfg.position == "top" ? cfg.bar_height + 10 : 10;
        win.paint = [this, v](cairo_t* cr) { paint_notes(cr, v); };
        win.click = [this](double x, double y, int btn) {
            on_note_click(x, y, btn);
        };
        win.ensure(*bar,
                   ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
                       ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT,
                   mt, 10, 0, 0, "mattbar-notifications", NOTE_W, total,
                   bar->primary_output());
        win.draw();
    }

    void paint_notes(cairo_t* cr, const std::vector<const Note*>& v) {
        double y = 0;
        for (auto* n : v) {
            col(cr, cfg.c_bg, std::min(1.0, cfg.c_bg.a + 0.04));
            cairo_rectangle(cr, 0, y, NOTE_W, n->height);
            cairo_fill(cr);
            col(cr, n->urgency >= 2 ? cfg.c_urgent : cfg.c_ws_bg, 1.0);
            cairo_set_line_width(cr, 1);
            cairo_rectangle(cr, 0.5, y + 0.5, NOTE_W - 1, n->height - 1);
            cairo_stroke(cr);
            double tx = NOTE_PAD;
            if (n->icon) {
                double iw = cairo_image_surface_get_width(n->icon);
                double ih = cairo_image_surface_get_height(n->icon);
                cairo_save(cr);
                cairo_translate(cr, NOTE_PAD, y + NOTE_PAD);
                cairo_scale(cr, 32.0 / iw, 32.0 / ih);
                cairo_set_source_surface(cr, n->icon, 0, 0);
                cairo_paint(cr);
                cairo_restore(cr);
                tx += 42;
            }
            cairo_font_extents_t fe;
            cairo_font_extents(cr, &fe);
            double ty = y + NOTE_PAD + fe.ascent;
            col(cr, n->urgency >= 2 ? cfg.c_urgent : cfg.c_fg, 1.0);
            draw_rich_text(cr, n->summary, tx, ty, cfg.font_size);
            ty += 17;
            col(cr, cfg.c_dim, 1.0);
            for (auto& l : wrap(cr, n->body, NOTE_W - NOTE_PAD - tx, 3)) {
                draw_rich_text(cr, l, tx, ty, cfg.font_size);
                ty += 17;
            }
            y += n->height + 8;
        }
    }

    void on_note_click(double, double y, int btn) {
        double off = 0;
        for (auto* n : shown()) {
            if (y >= off && y < off + n->height) {
                if (btn == BTN_LEFT) invoke_or_close(n->id);
                else close_note(n->id, 2); // 2 = dismissed by user
                return;
            }
            off += n->height + 8;
        }
    }
    // The action a bare click means: "default" when the app names one, otherwise its first action (many apps only register ...
    static const std::pair<std::string, std::string>* default_action(
        const Note& n) {
        for (auto& a : n.actions)
            if (a.first == "default") return &a;
        return n.actions.empty() ? nullptr : &n.actions.front();
    }
    void invoke_or_close(uint32_t id) {
        for (auto& n : active)
            if (n.id == id) {
                if (auto* a = default_action(n)) {
                    if (bus)
                        sd_bus_emit_signal(
                            bus, "/org/freedesktop/Notifications",
                            "org.freedesktop.Notifications",
                            "ActionInvoked", "us", id, a->first.c_str());
                    DBG("notifyd: invoked #%u '%s'", id, a->first.c_str());
                }
                close_note(id, 2);
                return;
            }
    }

    // ---- OSD --------------------------------------------------------------
    void osd_display(const std::string& label, double frac, bool muted) {
        if (!bar || !bar->compositor()) {
            DBG("osd: %s %.0f%%%s", label.c_str(), frac * 100,
                muted ? " (muted)" : "");
            return;
        }
        osd.paint = [this, label, frac, muted](cairo_t* cr) {
            col(cr, cfg.c_bg, std::min(1.0, cfg.c_bg.a + 0.04));
            cairo_rectangle(cr, 0, 0, osd.w, osd.h);
            cairo_fill(cr);
            col(cr, cfg.c_ws_bg, 1.0);
            cairo_rectangle(cr, 0.5, 0.5, osd.w - 1, osd.h - 1);
            cairo_stroke(cr);
            cairo_font_extents_t fe;
            cairo_font_extents(cr, &fe);
            double ty = osd.h / 2.0 + (fe.ascent - fe.descent) / 2;
            col(cr, muted ? cfg.c_urgent : cfg.c_fg, 1.0);
            cairo_move_to(cr, 12, ty);
            cairo_show_text(cr, label.c_str());
            // Numeric readout, right-aligned.
            char pct[8];
            std::snprintf(pct, sizeof pct, "%d%%",
                          (int)std::lround(frac * 100));
            cairo_text_extents_t pe, re;
            cairo_text_extents(cr, pct, &pe);
            cairo_text_extents(cr, "100%", &re);
            double reserve = std::max(pe.x_advance, re.x_advance);
            cairo_move_to(cr, osd.w - 12 - pe.x_advance, ty);
            cairo_show_text(cr, pct);
            double bx = 60, bw = osd.w - bx - reserve - 12 - 8;
            col(cr, cfg.c_ws_bg);
            cairo_rectangle(cr, bx, osd.h / 2.0 - 3, bw, 6);
            cairo_fill(cr);
            col(cr, muted ? cfg.c_dim : cfg.c_accent, 1.0);
            cairo_rectangle(cr, bx, osd.h / 2.0 - 3,
                            bw * std::clamp(frac, 0.0, 1.0), 6);
            cairo_fill(cr);
        };
        // Notification stack and OSD are singletons too: primary monitor only, never mirrored onto every screen.
        osd.ensure(*bar, ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM, 0, 0, 90, 0,
                   "mattbar-osd", 292, 44, bar->primary_output());
        osd.draw();
        itimerspec ts{};
        long ms             = cfg.osd_timeout_ms;
        ts.it_value.tv_sec  = ms / 1000;
        ts.it_value.tv_nsec = (ms % 1000) * 1000000L;
        timerfd_settime(osd_fd, 0, &ts, nullptr);
    }

    // PipeWire emits sink/source *change* events for much more than volume: streams starting, nodes suspending and waking, latency and port changes.
    void query_volume() {
        vol_cmd.run(*bar, "wpctl get-volume @DEFAULT_AUDIO_SINK@ 2>/dev/null",
                    [this](const std::string& out, int) {
                        auto p = out.find("Volume:");
                        if (p == std::string::npos) return;
                        double frac  = atof(out.c_str() + p + 7);
                        int    muted =
                            out.find("[MUTED]") != std::string::npos ? 1 : 0;
                        bool seed = last_vol < 0;
                        bool chg  = !seed &&
                                   (std::fabs(frac - last_vol) > 0.001 ||
                                    muted != last_vol_mut);
                        last_vol     = frac;
                        last_vol_mut = muted;
                        if (chg && cfg.osd_volume)
                            osd_display("vol", frac, muted);
                    },
                    1500);
    }

    void query_mic() {
        mic_cmd.run(*bar,
                    "wpctl get-volume @DEFAULT_AUDIO_SOURCE@ 2>/dev/null",
                    [this](const std::string& out, int) {
                        auto p = out.find("Volume:");
                        if (p == std::string::npos) return;
                        double frac  = atof(out.c_str() + p + 7);
                        int    muted =
                            out.find("[MUTED]") != std::string::npos ? 1 : 0;
                        bool seed = last_mic < 0;
                        bool chg  = !seed &&
                                   (std::fabs(frac - last_mic) > 0.001 ||
                                    muted != last_mic_mut);
                        last_mic     = frac;
                        last_mic_mut = muted;
                        if (chg && cfg.osd_mic)
                            osd_display("mic", frac, muted);
                    },
                    1500);
    }

    void start_osd_sources() {
        // Audio changes come from the shared AudioEvents stream (one pactl process serving both this OSD and the volume module).
        if (audio_sub == 0)
            audio_sub = audio_events().subscribe(*bar, [this](bool sink,
                                                              bool src) {
                if (sink) query_volume();
                if (src) query_mic();
            });
        if (uevent_fd < 0) { // backlight change uevents, dependency-free
            uevent_fd = socket(AF_NETLINK, SOCK_RAW | SOCK_NONBLOCK | SOCK_CLOEXEC,
                               NETLINK_KOBJECT_UEVENT);
            if (uevent_fd >= 0) {
                sockaddr_nl nl{};
                nl.nl_family = AF_NETLINK;
                nl.nl_groups = 1;
                if (bind(uevent_fd, (sockaddr*)&nl, sizeof nl) < 0) {
                    close(uevent_fd);
                    uevent_fd = -1;
                } else {
                    bar->add_fd(uevent_fd, [this](uint32_t) {
                        char buf[2048];
                        ssize_t n;
                        bool hit = false;
                        while ((n = recv(uevent_fd, buf, sizeof buf, 0)) > 0)
                            for (ssize_t i = 0; i + 19 < n; ++i)
                                if (!memcmp(buf + i, "SUBSYSTEM=backlight", 19))
                                    hit = true;
                        if (hit) show_brightness();
                    }, "uevent");
                }
            }
            DIR* d = opendir("/sys/class/backlight");
            if (d) {
                while (dirent* e = readdir(d))
                    if (e->d_name[0] != '.') {
                        backlight = std::string("/sys/class/backlight/") +
                                    e->d_name;
                        break;
                    }
                closedir(d);
            }
        }
    }
    void stop_osd_sources() {
        if (audio_sub) audio_events().unsubscribe(audio_sub);
        audio_sub = 0;
        if (uevent_fd >= 0) close(uevent_fd);
        uevent_fd = -1;
        osd.destroy();
    }
    void show_brightness() {
        if (backlight.empty()) return;
        double b = atof(slurp(backlight + "/brightness").c_str());
        double m = atof(slurp(backlight + "/max_brightness").c_str());
        if (m > 0 && cfg.osd_brightness) osd_display("bri", b / m, false);
    }
};

NotifyDaemon::NotifyDaemon() : im_(new Impl) { g_daemon = this; }
NotifyDaemon::~NotifyDaemon() { delete im_; }
bool NotifyDaemon::owns_name() const { return im_->owns; }

std::vector<NoteRecord> NotifyDaemon::history() const {
    return im_->history_snapshot();
}

void NotifyDaemon::clear_history() {
    im_->clear_history();
    if (im_->bar) im_->bar->request_draw();
}

size_t NotifyDaemon::history_count() const {
    return im_->active.size() + im_->history.size();
}

void NotifyDaemon::invoke(uint32_t id) {
    im_->invoke_or_close(id);
    if (im_->bar) im_->bar->request_draw();
}

bool NotifyDaemon::dnd() const { return im_->dnd; }

void NotifyDaemon::init(Bar& bar) {
    im_->bar = &bar;
    // crash-safe state: restore history/DND before anything can post
    im_->state_dir = Impl::state_home();
    mkdir(im_->state_dir.c_str(), 0700); // parent = XDG dirs, they exist
    im_->load_state();
    im_->save_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    bar.add_fd(im_->save_fd, [this](uint32_t) {
        uint64_t x;
        while (read(im_->save_fd, &x, sizeof x) > 0) {}
        if (im_->state_dirty) im_->save_state();
    }, "notify-state-save");
    im_->expiry_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    im_->osd_fd    = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    auto drain = [](int fd) {
        uint64_t x;
        while (read(fd, &x, sizeof x) > 0) {}
    };
    bar.add_fd(im_->expiry_fd, [this, drain](uint32_t) {
        drain(im_->expiry_fd);
        im_->on_expiry();
    }, "notify-expiry");
    bar.add_fd(im_->osd_fd, [this, drain](uint32_t) {
        drain(im_->osd_fd);
        im_->osd.destroy();
    }, "osd-hide");
    // Control signals (SIGRTMIN+2..6)
    sigset_t ss;
    sigemptyset(&ss);
    for (int i = 2; i <= 6; ++i) sigaddset(&ss, SIGRTMIN + i);
    sigprocmask(SIG_BLOCK, &ss, nullptr);
    im_->sig_fd = signalfd(-1, &ss, SFD_NONBLOCK | SFD_CLOEXEC);
    bar.add_fd(im_->sig_fd, [this](uint32_t) {
        signalfd_siginfo si;
        while (read(im_->sig_fd, &si, sizeof si) == sizeof si) {
            int off = (int)si.ssi_signo - SIGRTMIN;
            if (off == 2) dismiss_last();
            else if (off == 3) dismiss_all();
            else if (off == 4) invoke_last();
            else if (off == 5) toggle_dnd();
            else if (off == 6) restore_last();
        }
    }, "notify-signals");

    apply_enabled();
}

void NotifyDaemon::apply_enabled() {
    if (cfg.enable_notifications) im_->acquire();
    else im_->release();
    if (cfg.enable_osd && im_->bar) {
        im_->start_osd_sources();
        im_->query_volume(); // seed the change caches silently
        im_->query_mic();
    }
    else if (!cfg.enable_osd) im_->stop_osd_sources();
}

void NotifyDaemon::dismiss_last() {
    auto v = im_->shown();
    if (!v.empty()) im_->close_note(v.front()->id, 2);
}
void NotifyDaemon::dismiss_all() {
    while (!im_->active.empty()) im_->close_note(im_->active.front().id, 2);
}
void NotifyDaemon::invoke_last() {
    auto v = im_->shown();
    if (!v.empty()) im_->invoke_or_close(v.front()->id);
}
void NotifyDaemon::restore_last() {
    if (im_->history.empty()) return;
    Note n = std::move(im_->history.front());
    im_->history.pop_front();
    n.expires_at = // critical notifications never expire, restored or not
        n.urgency >= 2 || cfg.notification_timeout_s <= 0
            ? 0
            : now_ms() + cfg.notification_timeout_s * 1000ULL;
    im_->active.push_front(std::move(n));
    im_->arm_expiry();
    im_->redraw();
    im_->mark_dirty();
    DBG("notifyd: restored #%u", im_->active.front().id);
}
void NotifyDaemon::toggle_dnd() {
    im_->dnd = !im_->dnd;
    DBG("notifyd: dnd %s", im_->dnd ? "on" : "off");
    im_->redraw();
    im_->mark_dirty();
}
void NotifyDaemon::post(const std::string& summary, const std::string& body,
                        int urgency) {
    Note n;
    n.id      = im_->next_id++;
    n.app     = "mattbar";
    n.summary = summary;
    n.body    = body;
    n.urgency = urgency;
    n.expires_at =
        urgency >= 2 || cfg.notification_timeout_s <= 0
            ? 0
            : now_ms() + cfg.notification_timeout_s * 1000ULL;
    im_->active.push_front(std::move(n));
    im_->mark_dirty();
    DBG("notifyd: internal #%u '%s'", im_->active.front().id, summary.c_str());
    im_->arm_expiry();
    im_->redraw();
}
void NotifyDaemon::osd_show(const std::string& label, double frac,
                            bool muted) {
    im_->osd_display(label, frac, muted);
}

NotifyDaemon* notify_daemon() { return g_daemon; }

void notify_post(const std::string& summary, const std::string& body,
                 int urgency) {
    DBG("notify_post: '%s' / '%s' (u%d)", summary.c_str(), body.c_str(),
        urgency);
    if (g_daemon && g_daemon->owns_name()) {
        g_daemon->post(summary, body, urgency);
        return;
    }
    // self-contained double-fork spawn (spawn_detached is modules-internal)
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        if (fork() == 0) {
            const char* u = urgency >= 2 ? "critical"
                            : urgency == 1 ? "normal" : "low";
            execlp("notify-send", "notify-send", "-u", u, summary.c_str(),
                   body.c_str(), (char*)nullptr);
            _exit(127);
        }
        _exit(0);
    }
    if (pid > 0) waitpid(pid, nullptr, 0);
}

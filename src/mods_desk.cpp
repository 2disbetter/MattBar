// Desktop bar extras matching Omarchy's first-party widgets: active window
// title, keyboard layout, reminder count, dictation, Tailscale/Dropbox pills.
#include "modules.hpp"
#include "bar.hpp"
#include "config.hpp"
#include "shell.hpp"
#include "util.hpp"

#include <cairo/cairo.h>
#include <linux/input-event-codes.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h>
#include <unistd.h>

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

class MiniText : public Module {
public:
    double width(cairo_t* cr) override {
        if (text_.empty()) return 0;
        cairo_text_extents_t e;
        cairo_text_extents(cr, text_.c_str(), &e);
        return e.x_advance;
    }
    void draw(cairo_t* cr, double x, double h) override {
        if (text_.empty()) return;
        cairo_font_extents_t fe;
        cairo_font_extents(cr, &fe);
        cairo_set_source_rgba(cr, color_.r, color_.g, color_.b, 1);
        cairo_move_to(cr, x, h / 2.0 + (fe.ascent - fe.descent) / 2.0);
        cairo_show_text(cr, text_.c_str());
    }

protected:
    void set_text(Bar& bar, std::string t, Color c = cfg.c_fg) {
        if (t != text_ || c.r != color_.r || c.g != color_.g ||
            c.b != color_.b) {
            text_  = std::move(t);
            color_ = c;
            bar.request_draw();
        }
    }
    std::string text_;
    Color       color_ = cfg.c_fg;
};

} // namespace

namespace {

std::string hypr_dir() {
    const char* sig = getenv("HYPRLAND_INSTANCE_SIGNATURE");
    const char* rt  = getenv("XDG_RUNTIME_DIR");
    if (!sig) return {};
    if (rt) {
        std::string p = std::string(rt) + "/hypr/" + sig;
        if (access((p + "/.socket2.sock").c_str(), F_OK) == 0) return p;
    }
    return {};
}

int unix_nb(const std::string& path) {
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return -1;
    sockaddr_un a{};
    a.sun_family = AF_UNIX;
    strncpy(a.sun_path, path.c_str(), sizeof a.sun_path - 1);
    if (connect(fd, (sockaddr*)&a, sizeof a) < 0 && errno != EINPROGRESS) {
        close(fd);
        return -1;
    }
    return fd;
}

std::string utf8_clip(std::string s, size_t maxc) {
    size_t chars = 0, i = 0;
    while (i < s.size() && chars < maxc) {
        unsigned char c = s[i];
        i += c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xe ? 3 : 4;
        ++chars;
    }
    if (i >= s.size()) return s;
    return s.substr(0, i) + "\u2026";
}

bool untyped_kb(const std::string& n) {
    return n.rfind("hl-virtual-keyboard", 0) == 0 ||
           n.rfind("power-button", 0) == 0 || n.rfind("sleep-button", 0) == 0 ||
           n.rfind("lid-switch", 0) == 0 || n.find("consumer-control") != std::string::npos ||
           n.find("system-control") != std::string::npos ||
           n.find("video-bus") != std::string::npos;
}

class ActiveWindowModule : public MiniText {
public:
    bool enabled() const override { return cfg.show_active_window; }
    bool primary_only() const override { return true; }
    void init(Bar& bar) override {
        bar_ = &bar;
        std::string d = hypr_dir();
        if (d.empty()) return;
        fd_ = unix_nb(d + "/.socket2.sock");
        if (fd_ < 0) return;
        bar.add_fd(fd_, [this](uint32_t) {
            char b[2048];
            ssize_t n;
            while ((n = read(fd_, b, sizeof b)) > 0) buf_.append(b, n);
            size_t p;
            while ((p = buf_.find('\n')) != std::string::npos) {
                on_line(buf_.substr(0, p));
                buf_.erase(0, p + 1);
            }
        }, "activewindow");
    }
    bool on_click(double, int button) override {
        if (title_.empty()) return false;
        if (button == BTN_MIDDLE || button == BTN_RIGHT) {
            spawn_detached("hyprctl dispatch killactive");
            return true;
        }
        spawn_detached("hyprctl dispatch focuscurrentorlast");
        return true;
    }

private:
    void on_line(const std::string& l) {
        if (l.rfind("activewindow>>", 0) != 0) return;
        std::string rest = l.substr(15);
        auto c = rest.find(',');
        title_ = c == std::string::npos ? rest : rest.substr(c + 1);
        if (!bar_) return;
        set_text(*bar_, utf8_clip(title_, (size_t)cfg.active_window_max),
                 cfg.c_dim);
    }
    Bar*        bar_ = nullptr;
    int         fd_  = -1;
    std::string buf_, title_;
};

class KbLayoutModule : public MiniText {
public:
    bool enabled() const override { return cfg.show_kblayout; }
    void init(Bar& bar) override {
        bar_ = &bar;
        refresh();
    }
    void tick() override {
        if (++n_ >= 3) {
            n_ = 0;
            refresh();
        }
    }
    bool on_click(double, int) override {
        if (kb_.empty()) return false;
        spawn_detached("hyprctl switchxkblayout " + shell_q(kb_) + " next");
        refresh();
        return true;
    }

private:
    static std::string shell_q(const std::string& s) {
        std::string o = "'";
        for (char c : s) {
            if (c == '\'') o += "'\\''";
            else o += c;
        }
        return o + "'";
    }
    void refresh() {
        if (!bar_) return;
        cmd_.run(*bar_, "hyprctl devices -j 2>/dev/null",
                 [this](const std::string& out, int) { parse(out); }, 1500);
    }
    void parse(const std::string& j) {
        layouts_ = 0;
        kb_.clear();
        label_.clear();
        size_t p = 0;
        std::string best_name, best_map;
        int best_idx = -1;
        while ((p = j.find("\"name\"", p)) != std::string::npos) {
            std::string name = json_get(j, p);
            auto km = j.find("\"active_keymap\"", p);
            auto nx = j.find("\"name\"", p + 6);
            std::string map;
            if (km != std::string::npos && (nx == std::string::npos || km < nx))
                map = json_get(j, km);
            auto li = j.find("\"active_layout_index\"", p);
            int idx = 0;
            if (li != std::string::npos && (nx == std::string::npos || li < nx))
                idx = atoi(j.c_str() + li + 22);
            auto lay = j.find("\"layout\"", p);
            std::string layv;
            if (lay != std::string::npos && (nx == std::string::npos || lay < nx))
                layv = json_get(j, lay);
            p += 6;
            if (untyped_kb(name)) continue;
            int commas = 0;
            for (char c : layv)
                if (c == ',') ++commas;
            layouts_ = std::max(layouts_, commas + 1);
            if (idx > best_idx) {
                best_idx  = idx;
                best_name = name;
                best_map  = map;
            }
        }
        kb_ = best_name;
        label_ = short_lab(best_map);
        if (layouts_ < 2) {
            set_text(*bar_, "");
            return;
        }
        set_text(*bar_, label_.empty() ? "KB" : label_, cfg.c_fg);
    }
    static std::string json_get(const std::string& j, size_t at) {
        auto c = j.find(':', at);
        if (c == std::string::npos) return {};
        auto q = j.find('"', c + 1);
        if (q == std::string::npos) return {};
        auto e = j.find('"', q + 1);
        if (e == std::string::npos) return {};
        return j.substr(q + 1, e - q - 1);
    }
    static std::string short_lab(std::string desc) {
        if (desc.empty()) return {};
        auto p = desc.find('(');
        std::string w = p == std::string::npos ? desc : desc.substr(0, p);
        while (!w.empty() && w.back() == ' ') w.pop_back();
        auto sp = w.find(' ');
        if (sp != std::string::npos) w = w.substr(0, sp);
        for (char& c : w) c = (char)toupper((unsigned char)c);
        if (w.size() > 3) w.resize(3);
        if (w == "ENGLISH") return "EN";
        if (w == "US") return "EN";
        return w;
    }
    Bar*        bar_ = nullptr;
    AsyncCmd    cmd_;
    std::string kb_, label_;
    int         layouts_ = 0, n_ = 0;
};

class ReminderModule : public MiniText {
public:
    bool enabled() const override { return cfg.show_reminder; }
    void init(Bar& bar) override {
        bar_ = &bar;
        refresh();
    }
    void tick() override {
        if (++n_ >= 5) {
            n_ = 0;
            refresh();
        }
    }
    bool on_click(double, int) override {
        auto* sh = mattbar_shell();
        if (count_ > 0)
            spawn_detached("omarchy-reminder show");
        else if (sh)
            sh->toggle("omarchy.reminders", "");
        else
            spawn_detached("omarchy-reminder -i");
        return true;
    }

private:
    void refresh() {
        if (!bar_) return;
        cmd_.run(*bar_, "omarchy-reminder show --json 2>/dev/null",
                 [this](const std::string& out, int st) {
                     count_ = 0;
                     if (st == 0) {
                         auto p = out.find("\"count\"");
                         if (p != std::string::npos)
                             count_ = atoi(out.c_str() + p + 8);
                     }
                     if (count_ <= 0) set_text(*bar_, "");
                     else
                         set_text(*bar_,
                                  std::string("\U000F08CC ") +
                                      std::to_string(count_),
                                  cfg.c_accent);
                 },
                 1500);
    }
    Bar*     bar_   = nullptr;
    AsyncCmd cmd_;
    int      count_ = 0, n_ = 0;
};

class DictationModule : public MiniText {
public:
    bool enabled() const override { return cfg.show_dictation; }
    void init(Bar& bar) override {
        bar_ = &bar;
        start();
    }
    void tick() override {
        if (!cmd_.running()) start();
    }
    bool on_click(double, int) override {
        spawn_detached("omarchy-voxtype-config");
        return true;
    }

private:
    void start() {
        if (!bar_ || cmd_.running()) return;
        cmd_.run(*bar_, "omarchy-voxtype-status 2>/dev/null",
                 [this](const std::string&, int) {}, 3600000,
                 [this](const std::string& line) { on_line(line); });
    }
    void on_line(const std::string& line) {
        std::string st;
        auto p = line.find("\"alt\"");
        if (p == std::string::npos) p = line.find("\"class\"");
        if (p != std::string::npos) {
            auto q = line.find('"', p + 6);
            if (q != std::string::npos) {
                auto e = line.find('"', q + 1);
                if (e != std::string::npos) st = line.substr(q + 1, e - q - 1);
            }
        }
        if (st.empty()) st = "idle";
        if (st == "recording")
            set_text(*bar_, "\U000F036C", cfg.c_urgent);
        else if (st == "transcribing")
            set_text(*bar_, "\U000F053F", cfg.c_accent);
        else
            set_text(*bar_, "");
    }
    Bar*     bar_ = nullptr;
    AsyncCmd cmd_;
};

class TailscalePill : public MiniText {
public:
    bool enabled() const override { return cfg.show_tailscale; }
    void init(Bar& bar) override {
        bar_ = &bar;
        refresh();
    }
    void tick() override {
        if (++n_ >= 8) {
            n_ = 0;
            refresh();
        }
    }
    bool on_click(double, int button) override {
        auto* sh = mattbar_shell();
        if (button == BTN_RIGHT) {
            spawn_detached("tailscale status >/dev/null && tailscale down || "
                           "tailscale up");
            refresh();
            return true;
        }
        if (sh) sh->toggle("omarchy.tailscale", "");
        return true;
    }

private:
    void refresh() {
        if (!bar_) return;
        cmd_.run(*bar_, "command -v tailscale >/dev/null && tailscale status "
                        "--json 2>/dev/null",
                 [this](const std::string& out, int st) {
                     if (st != 0 || out.find("BackendState") == std::string::npos) {
                         set_text(*bar_, "");
                         return;
                     }
                     bool on = out.find("\"Running\"") != std::string::npos;
                     set_text(*bar_, "\U000F0444", on ? cfg.c_fg : cfg.c_dim);
                 },
                 2000);
    }
    Bar*     bar_ = nullptr;
    AsyncCmd cmd_;
    int      n_   = 0;
};

class DropboxPill : public MiniText {
public:
    bool enabled() const override { return cfg.show_dropbox; }
    void init(Bar& bar) override {
        bar_ = &bar;
        refresh();
    }
    void tick() override {
        if (++n_ >= 10) {
            n_ = 0;
            refresh();
        }
    }
    bool on_click(double, int) override {
        auto* sh = mattbar_shell();
        if (sh) sh->toggle("omarchy.dropbox", "");
        return true;
    }

private:
    void refresh() {
        if (!bar_) return;
        const char* om = getenv("OMARCHY_PATH");
        std::string helper =
            std::string(om && *om ? om : "/usr/share/omarchy") +
            "/shell/plugins/panels/dropbox/status.py";
        cmd_.run(*bar_, "python3 " + helper + " 8 2>/dev/null",
                 [this](const std::string& out, int st) {
                     if (st != 0 || out.find("\"installed\": true") ==
                                        std::string::npos) {
                         set_text(*bar_, "");
                         return;
                     }
                     bool on = out.find("\"running\": true") != std::string::npos &&
                               out.find("\"authenticated\": true") !=
                                   std::string::npos;
                     set_text(*bar_, "\uf16b", on ? cfg.c_fg : cfg.c_dim);
                 },
                 4000);
    }
    Bar*     bar_ = nullptr;
    AsyncCmd cmd_;
    int      n_   = 0;
};

} // namespace

Module* make_active_window() { return new ActiveWindowModule; }
Module* make_kblayout() { return new KbLayoutModule; }
Module* make_reminder() { return new ReminderModule; }
Module* make_dictation() { return new DictationModule; }
Module* make_tailscale() { return new TailscalePill; }
Module* make_dropbox() { return new DropboxPill; }

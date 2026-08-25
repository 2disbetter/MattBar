// Remaining shell overlays: timezone picker, power/battery dashboard,
// reminders flow, Tailscale, Dropbox.
#include "overlay.hpp"
#include "ui.hpp"
#include "util.hpp"

#include <linux/input-event-codes.h>
#include <dirent.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <sstream>
#include <vector>

using ov::Host;
using ov::col;
using ov::say;
using ov::tw;
using ov::rrect;
using ov::panel_bg;
using ov::select_shell_font;
using ov::utf8_trunc;
using ov::json_str;
using ov::json_object_array;
using ov::draw_slider;
using ov::parse_kv;
using ov::format_bytes;

namespace {

struct Hit {
    double x, y, w, h;
    int    kind = 0, idx = 0;
};

bool is_mullvad_host(std::string n) {
    for (char& c : n) c = (char)tolower((unsigned char)c);
    const char* suf = ".mullvad.ts.net";
    return n.size() > 15 && n.rfind(suf) == n.size() - 15;
}

std::string ago(long ts) {
    if (ts <= 0) return {};
    long diff = (long)time(nullptr) - ts;
    if (diff < 0) diff = 0;
    if (diff < 60) return "Just now";
    long m = diff / 60;
    if (m < 60) return std::to_string(m) + "m ago";
    long h = m / 60;
    if (h < 24) return std::to_string(h) + "h ago";
    long d = h / 24;
    if (d < 30) return std::to_string(d) + "d ago";
    long mo = d / 30;
    if (mo < 12) return std::to_string(mo) + "mo ago";
    return std::to_string(d / 365) + "y ago";
}

const char* file_glyph(const std::string& name) {
    auto dot = name.rfind('.');
    std::string e = dot == std::string::npos ? "" : name.substr(dot + 1);
    for (char& c : e) c = (char)tolower((unsigned char)c);
    if (e == "jpg" || e == "jpeg" || e == "png" || e == "gif" || e == "webp" ||
        e == "svg" || e == "bmp")
        return "IMG";
    if (e == "mp4" || e == "mov" || e == "mkv" || e == "webm" || e == "avi")
        return "VID";
    if (e == "pdf" || e == "txt" || e == "md" || e == "doc" || e == "docx")
        return "DOC";
    return "FILE";
}

// ---- Timezone -------------------------------------------------------------
class TimezoneOverlay : public Overlay {
public:
    const char* id() const override { return "omarchy.clock-timezone"; }
    void summon(const std::string&) override { open(); }
    void hide() override { host_.close(); }
    bool is_open() const override { return host_.is_open(); }
    ~TimezoneOverlay() override { host_.close(); }

private:
    Host host_;
    AsyncCmd cmd_;
    TextField field_;
    std::vector<std::string> all_, view_;
    int sel_ = 0, scroll_ = 0;
    int W() const { return 420; }
    int H() const { return 480; }
    double fs() const { return cfg.shell_font_size - 2; }

    void open() {
        field_.clear();
        sel_ = scroll_ = 0;
        host_.win.paint = [this](cairo_t* cr) { paint(cr); };
        host_.win.click = [this](double x, double y, int b) {
            if (b != BTN_LEFT) return;
            int row = (int)((y - 50) / ov::row_h(fs())) + scroll_;
            if (row >= 0 && row < (int)view_.size()) apply(view_[row]);
        };
        host_.win.pscroll = [this](int d) {
            sel_ = ov::clamp_sel(sel_ + d, (int)view_.size());
            host_.redraw();
        };
        host_.win.pkey = [this](const Bar::KeyEvent& e) { on_key(e); };
        host_.open(W(), H(), "mattbar-timezone", id(), true);
        auto* sh = mattbar_shell();
        if (sh && sh->bar())
            cmd_.run(*sh->bar(), "timedatectl list-timezones",
                     [this](const std::string& out, int) {
                         all_.clear();
                         std::istringstream ss(out);
                         std::string line;
                         while (std::getline(ss, line)) {
                             line = trim(line);
                             if (!line.empty()) all_.push_back(line);
                         }
                         filter();
                         host_.redraw();
                     },
                     4000);
    }
    void filter() {
        view_.clear();
        std::string q = field_.text;
        for (char& c : q) c = (char)tolower((unsigned char)c);
        for (auto& z : all_) {
            std::string l = z;
            for (char& c : l) c = (char)tolower((unsigned char)c);
            if (q.empty() || l.find(q) != std::string::npos) view_.push_back(z);
        }
        sel_ = ov::clamp_sel(sel_, (int)view_.size());
    }
    void apply(const std::string& z) {
        spawn_detached("pkexec timedatectl set-timezone " + ov::shell_quote(z) +
                       "; omarchy-notification-send 'Timezone is now " + z +
                       "'");
        host_.dismiss();
    }
    void on_key(const Bar::KeyEvent& e) {
        if (!e.pressed) return;
        if (e.escape()) {
            host_.dismiss();
            return;
        }
        if (e.enter() && !view_.empty()) {
            apply(view_[sel_]);
            return;
        }
        if (e.down()) {
            sel_ = ov::clamp_sel(sel_ + 1, (int)view_.size());
            host_.redraw();
            return;
        }
        if (e.up()) {
            sel_ = ov::clamp_sel(sel_ - 1, (int)view_.size());
            host_.redraw();
            return;
        }
        if (field_.handle(e)) {
            filter();
            host_.redraw();
        }
    }
    void paint(cairo_t* cr) {
        panel_bg(cr, W(), H());
        select_shell_font(cr, fs());
        say(cr, 18, 22, "Set timezone", cfg.c_fg);
        field_.draw(cr, 16, 36, W() - 32, 26, "Search");
        int rh = ov::row_h(fs());
        int vis = std::max(1, (H() - 70) / rh);
        ov::keep_visible(sel_, scroll_, vis, (int)view_.size());
        for (int i = 0; i < vis; ++i) {
            int idx = scroll_ + i;
            if (idx >= (int)view_.size()) break;
            double y = 70 + i * rh;
            if (idx == sel_) {
                col(cr, cfg.c_accent, 0.28);
                rrect(cr, 12, y, W() - 24, rh - 2, 6);
                cairo_fill(cr);
            }
            say(cr, 20, y + rh / 2.0, utf8_trunc(view_[idx], 36),
                idx == sel_ ? contrast_on(cfg.c_accent) : cfg.c_fg);
        }
    }
};

// ---- Power / battery ------------------------------------------------------
class PowerOverlay : public Overlay {
public:
    const char* id() const override { return "omarchy.power"; }
    void summon(const std::string&) override { open(); }
    void hide() override { host_.close(); }
    bool is_open() const override { return host_.is_open(); }
    std::string call(const std::string& method, const std::string&) override {
        if (method == "togglePercentage") {
            cfg.power_show_pct = !cfg.power_show_pct;
            cfg.save();
            auto* sh = mattbar_shell();
            if (sh && sh->bar()) sh->bar()->request_draw();
            return cfg.power_show_pct ? "on" : "off";
        }
        return "ok";
    }
    ~PowerOverlay() override { host_.close(); }

private:
    Host host_;
    AsyncCmd cmd_;
    std::map<std::string, std::string> bat_, sys_;
    std::vector<std::string> profiles_;
    std::string active_;
    int W() const { return 380; }
    int H() const { return 420; }
    double fs() const { return cfg.shell_display_font_size; }

    void open() {
        host_.win.paint = [this](cairo_t* cr) { paint(cr); };
        host_.win.click = [this](double x, double y, int b) { on_click(x, y, b); };
        host_.win.pkey  = [this](const Bar::KeyEvent& e) {
            if (e.escape()) host_.dismiss();
        };
        host_.open(W(), H(), "mattbar-power", id(), true, Host::Place::BarEnd);
        reload();
    }
    void reload() {
        auto* sh = mattbar_shell();
        if (!sh || !sh->bar()) return;
        cmd_.run(*sh->bar(),
                 "echo '===BAT==='; omarchy-battery-status --shell 2>/dev/null; "
                 "echo '===SYS==='; omarchy-system-stats 2>/dev/null; "
                 "echo '===PROF==='; omarchy-powerprofiles-list --active-state "
                 "2>/dev/null",
                 [this](const std::string& out, int) {
                     parse(out);
                     host_.redraw();
                 },
                 3000);
    }
    void parse(const std::string& out) {
        auto bat = out.find("===BAT==="), sys = out.find("===SYS==="),
             pr  = out.find("===PROF===");
        if (bat != std::string::npos && sys != std::string::npos)
            bat_ = parse_kv(out.substr(bat, sys - bat));
        if (sys != std::string::npos && pr != std::string::npos)
            sys_ = parse_kv(out.substr(sys, pr - sys));
        profiles_.clear();
        active_.clear();
        if (pr == std::string::npos) return;
        std::istringstream ss(out.substr(pr));
        std::string line;
        while (std::getline(ss, line)) {
            line = trim(line);
            if (line.empty() || line.rfind("===", 0) == 0) continue;
            if (line[0] == '*') {
                active_ = trim(line.substr(1));
                profiles_.push_back(active_);
            } else
                profiles_.push_back(line);
        }
    }
    std::vector<Hit> hits_;
    void on_click(double x, double y, int b) {
        if (b != BTN_LEFT) return;
        for (auto& h : hits_) {
            if (x < h.x || y < h.y || x >= h.x + h.w || y >= h.y + h.h) continue;
            if (h.kind == 0 && h.idx >= 0 && h.idx < (int)profiles_.size()) {
                persist_power_profile(profiles_[h.idx]);
                reload();
            }
        }
    }
    void paint(cairo_t* cr) {
        panel_bg(cr, W(), H());
        select_shell_font(cr, fs());
        hits_.clear();
        std::string pct = bat_.count("percentage") ? bat_["percentage"] : "";
        std::string st  = bat_.count("state") ? bat_["state"] : "Battery";
        cairo_set_font_size(cr, fs() + 8);
        say(cr, 18, 28, pct.empty() ? "Power" : pct, cfg.c_fg);
        cairo_set_font_size(cr, fs());
        say(cr, 18, 50, st, cfg.c_dim);
        double y = 72;
        auto row = [&](const char* k, const std::string& v) {
            if (v.empty()) return;
            say(cr, 18, y + 8, k, cfg.c_dim);
            say(cr, W() - 18 - tw(cr, v), y + 8, v, cfg.c_fg);
            y += 22;
        };
        row("DRAW", bat_.count("rate") ? bat_["rate"] : "");
        row("TIME", bat_.count("time") ? bat_["time"] : "");
        row("SIZE", bat_.count("size") ? bat_["size"] : "");
        row("CYCLES", bat_.count("cycles") ? bat_["cycles"] : "");
        row("HOLD", bat_.count("threshold") ? bat_["threshold"] : "");
        row("CPU", sys_.count("cpu") ? sys_["cpu"] : "");
        row("MEM", sys_.count("memory") ? sys_["memory"] : "");
        if (!profiles_.empty()) {
            cairo_set_font_size(cr, std::max(9.0, fs() - 1));
            say(cr, 18, y + 10, "PROFILES", cfg.c_accent);
            y += 24;
            cairo_set_font_size(cr, fs());
            for (int i = 0; i < (int)profiles_.size(); ++i) {
                bool on = profiles_[i] == active_;
                if (on) {
                    col(cr, cfg.c_accent, 0.28);
                    rrect(cr, 14, y, W() - 28, 26, 6);
                    cairo_fill(cr);
                }
                say(cr, 22, y + 13, profiles_[i],
                    on ? contrast_on(cfg.c_accent) : cfg.c_fg);
                hits_.push_back({14, y, W() - 28.0, 26, 0, i});
                y += 28;
            }
        }
    }
};

// ---- Reminders ------------------------------------------------------------
class RemindersOverlay : public Overlay {
public:
    const char* id() const override { return "omarchy.reminders"; }
    void summon(const std::string&) override { open(); }
    void hide() override { host_.close(); }
    bool is_open() const override { return host_.is_open(); }
    ~RemindersOverlay() override { host_.close(); }

private:
    Host host_;
    TextField field_;
    std::string minutes_;
    int step_ = 0; // 0 minutes, 1 message
    int W() const { return 360; }
    int H() const { return 140; }
    void open() {
        step_ = 0;
        minutes_.clear();
        field_.clear();
        field_.focused = true;
        host_.win.paint = [this](cairo_t* cr) { paint(cr); };
        host_.win.pkey  = [this](const Bar::KeyEvent& e) { on_key(e); };
        host_.open(W(), H(), "mattbar-reminders", id(), true);
    }
    void on_key(const Bar::KeyEvent& e) {
        if (!e.pressed) return;
        if (e.escape()) {
            host_.dismiss();
            return;
        }
        if (e.enter()) {
            if (step_ == 0) {
                minutes_ = field_.text;
                field_.clear();
                step_ = 1;
                host_.redraw();
            } else {
                spawn_detached("omarchy-reminder " + ov::shell_quote(minutes_) +
                               " " + ov::shell_quote(field_.text));
                host_.dismiss();
            }
            return;
        }
        if (field_.handle(e)) host_.redraw();
    }
    void paint(cairo_t* cr) {
        panel_bg(cr, W(), H());
        select_shell_font(cr, cfg.shell_font_size);
        say(cr, 18, 28, step_ == 0 ? "Remind in minutes" : "Reminder message",
            cfg.c_fg);
        field_.draw(cr, 16, 50, W() - 32, 32,
                    step_ == 0 ? "15" : "Pickup Jack");
        cairo_set_font_size(cr, std::max(10.0, cfg.shell_font_size - 4));
        say(cr, 18, 110, "Enter to confirm  \u00b7  Esc to cancel", cfg.c_dim);
    }
};

// ---- Tailscale ------------------------------------------------------------
class TailscaleOverlay : public Overlay {
public:
    const char* id() const override { return "omarchy.tailscale"; }
    void summon(const std::string&) override { open(); }
    void hide() override { host_.close(); }
    bool is_open() const override { return host_.is_open(); }
    ~TailscaleOverlay() override { host_.close(); }

private:
    struct Peer {
        std::string name, dns, ip;
        bool        online = false, exit_opt = false, exit_on = false;
    };
    struct ExitNode {
        std::string id, host, display, ip, country, city;
        bool        active = false, mullvad = false, add = false;
    };

    Host host_;
    AsyncCmd status_cmd_, exit_cmd_;
    TextField search_;
    bool installed_ = false, running_ = false, picker_ = false;
    std::string self_, ip_, err_;
    std::vector<Peer>     peers_;
    std::vector<ExitNode> tailnet_exits_, regions_, shown_exits_;
    int sel_ = 0, scroll_ = 0, region_sel_ = 0;
    int W() const { return 420; }
    int H() const { return 540; }
    double fs() const { return cfg.shell_network_font_size; }

    static bool has_true(const std::string& s, const char* key) {
        std::string a = std::string("\"") + key + "\": true";
        std::string b = std::string("\"") + key + "\":true";
        return s.find(a) != std::string::npos || s.find(b) != std::string::npos;
    }
    static std::string first_ip(const std::string& s) {
        auto ipp = s.find("\"TailscaleIPs\"");
        if (ipp == std::string::npos) return {};
        auto lb = s.find('[', ipp);
        auto q  = s.find('"', lb);
        auto e  = s.find('"', q + 1);
        if (q == std::string::npos || e == std::string::npos) return {};
        return s.substr(q + 1, e - q - 1);
    }

    void open() {
        search_.clear();
        picker_ = false;
        scroll_ = 0;
        host_.win.paint = [this](cairo_t* cr) { paint(cr); };
        host_.win.click = [this](double x, double y, int b) { on_click(x, y, b); };
        host_.win.pscroll = [this](int d) {
            scroll_ = std::max(0, scroll_ + d);
            host_.redraw();
        };
        host_.win.pkey = [this](const Bar::KeyEvent& e) { on_key(e); };
        host_.open(W(), H(), "mattbar-tailscale", id(), true,
                   Host::Place::BarEnd);
        reload();
    }
    void reload() {
        auto* sh = mattbar_shell();
        if (!sh || !sh->bar()) return;
        status_cmd_.run(*sh->bar(),
                        "command -v tailscale >/dev/null && "
                        "tailscale status --json || echo MISSING",
                        [this](const std::string& out, int) {
                            parse_status(out);
                            rebuild_exits();
                            host_.redraw();
                        },
                        4000);
        exit_cmd_.run(*sh->bar(),
                      "command -v tailscale >/dev/null && "
                      "tailscale exit-node list 2>/dev/null || true",
                      [this](const std::string& out, int) {
                          parse_exit_list(out);
                          rebuild_exits();
                          host_.redraw();
                      },
                      6000);
    }
    void parse_status(const std::string& j) {
        peers_.clear();
        tailnet_exits_.clear();
        installed_ = j.find("MISSING") == std::string::npos &&
                     j.find("BackendState") != std::string::npos;
        running_ = j.find("\"Running\"") != std::string::npos;
        self_    = json_str(j, "HostName");
        ip_      = first_ip(j);
        err_.clear();
        if (!installed_) err_ = "tailscale is not installed";
        size_t p = j.find("\"Peer\"");
        if (p == std::string::npos) return;
        while (true) {
            p = j.find("\"HostName\"", p + 1);
            if (p == std::string::npos) break;
            std::string slice = j.substr(p, 1600);
            Peer pe;
            pe.name     = json_str(slice, "HostName");
            pe.dns      = json_str(slice, "DNSName");
            if (!pe.dns.empty() && pe.dns.back() == '.') pe.dns.pop_back();
            pe.ip       = first_ip(slice);
            pe.online   = has_true(slice, "Online");
            pe.exit_opt = has_true(slice, "ExitNodeOption");
            pe.exit_on  = has_true(slice, "ExitNode");
            bool mv     = is_mullvad_host(pe.dns) || is_mullvad_host(pe.name);
            if (pe.name.empty() || pe.name == self_ || mv || !pe.online)
                continue;
            peers_.push_back(pe);
            if (pe.exit_opt) {
                ExitNode e;
                e.id      = pe.dns.empty() ? pe.name : pe.dns;
                e.host    = e.id;
                e.display = pe.name;
                e.ip      = pe.ip;
                e.active  = pe.exit_on;
                tailnet_exits_.push_back(e);
            }
        }
    }
    static std::string slice_col(const std::string& line, int a, int b) {
        if (a < 0 || a >= (int)line.size()) return {};
        int e = b < 0 ? (int)line.size() : std::min(b, (int)line.size());
        if (e < a) return {};
        return trim(line.substr((size_t)a, (size_t)(e - a)));
    }
    void parse_exit_list(const std::string& raw) {
        regions_.clear();
        std::istringstream ss(raw);
        std::string line, header;
        int ipS = -1, hostS = -1, countryS = -1, cityS = -1, statusS = -1;
        std::map<std::string, ExitNode> by_region;
        while (std::getline(ss, line)) {
            if (ipS < 0) {
                if (line.find("HOSTNAME") != std::string::npos &&
                    line.find("COUNTRY") != std::string::npos &&
                    line.find("CITY") != std::string::npos) {
                    header   = line;
                    ipS      = (int)line.find("IP");
                    hostS    = (int)line.find("HOSTNAME");
                    countryS = (int)line.find("COUNTRY");
                    cityS    = (int)line.find("CITY");
                    statusS  = (int)line.find("STATUS");
                }
                continue;
            }
            if (line.empty() || line[0] == '#') continue;
            std::string host = slice_col(line, hostS, countryS);
            if (!is_mullvad_host(host)) continue;
            std::string country = slice_col(line, countryS, cityS);
            std::string city    = slice_col(line, cityS, statusS);
            std::string status  = slice_col(line, statusS, -1);
            if (country.empty() || city.empty() || city == "Any") continue;
            std::string key = country + "\n" + city;
            if (by_region.count(key)) {
                if (status != "" && status != "-")
                    by_region[key].active = true;
                continue;
            }
            ExitNode n;
            n.id      = "mullvad-region:" + key;
            n.host    = host;
            n.ip      = slice_col(line, ipS, hostS);
            n.country = country;
            n.city    = city;
            n.display = city + ", " + country;
            n.mullvad = true;
            n.active  = status != "" && status != "-";
            by_region[key] = n;
        }
        for (auto& [k, n] : by_region) regions_.push_back(n);
        std::sort(regions_.begin(), regions_.end(),
                  [](const ExitNode& a, const ExitNode& b) {
                      if (a.country != b.country) return a.country < b.country;
                      return a.city < b.city;
                  });
    }
    void rebuild_exits() {
        shown_exits_ = tailnet_exits_;
        for (auto& r : regions_)
            if (r.active) shown_exits_.push_back(r);
        if (!regions_.empty()) {
            ExitNode add;
            add.id      = "mullvad:add";
            add.display = "Choose Mullvad region";
            add.add     = true;
            shown_exits_.push_back(add);
        }
    }
    std::vector<ExitNode> filtered_regions() {
        std::string q = search_.text;
        for (char& c : q) c = (char)tolower((unsigned char)c);
        std::vector<ExitNode> out;
        for (auto& r : regions_) {
            std::string l = r.display;
            for (char& c : l) c = (char)tolower((unsigned char)c);
            if (q.empty() || l.find(q) != std::string::npos) out.push_back(r);
        }
        return out;
    }
    void toggle() {
        spawn_detached(running_ ? "tailscale down" : "tailscale up");
        running_ = !running_;
        host_.redraw();
        reload();
    }
    void set_exit(const ExitNode& n) {
        if (n.add) {
            picker_     = !picker_;
            search_.clear();
            region_sel_ = 0;
            host_.redraw();
            return;
        }
        std::string target;
        if (!n.active) target = !n.ip.empty() ? n.ip : n.host;
        spawn_detached("tailscale set --exit-node=" +
                       (target.empty() ? std::string() : ov::shell_quote(target)));
        picker_ = false;
        err_    = n.active ? "Exit node cleared" : "Using " + n.display;
        host_.redraw();
        reload();
    }
    std::vector<Hit> hits_;
    void on_click(double x, double y, int b) {
        if (b != BTN_LEFT) return;
        for (auto& h : hits_) {
            if (x < h.x || y < h.y || x >= h.x + h.w || y >= h.y + h.h) continue;
            if (h.kind == 0) toggle();
            if (h.kind == 1 && h.idx >= 0 && h.idx < (int)peers_.size()) {
                spawn_detached("wl-copy " + ov::shell_quote(peers_[h.idx].ip));
                err_ = "Copied " + peers_[h.idx].ip;
                host_.redraw();
            }
            if (h.kind == 3 && h.idx >= 0 && h.idx < (int)shown_exits_.size())
                set_exit(shown_exits_[h.idx]);
            if (h.kind == 4) {
                auto regs = filtered_regions();
                if (h.idx >= 0 && h.idx < (int)regs.size()) set_exit(regs[h.idx]);
            }
        }
    }
    void on_key(const Bar::KeyEvent& e) {
        if (!e.pressed) return;
        if (e.escape()) {
            if (picker_) {
                picker_ = false;
                host_.redraw();
                return;
            }
            host_.dismiss();
            return;
        }
        if (picker_) {
            auto regs = filtered_regions();
            if (e.enter() && !regs.empty()) {
                set_exit(regs[ov::clamp_sel(region_sel_, (int)regs.size())]);
                return;
            }
            if (e.down()) {
                region_sel_ = ov::clamp_sel(region_sel_ + 1, (int)regs.size());
                host_.redraw();
                return;
            }
            if (e.up()) {
                region_sel_ = ov::clamp_sel(region_sel_ - 1, (int)regs.size());
                host_.redraw();
                return;
            }
            if (search_.handle(e)) {
                region_sel_ = 0;
                host_.redraw();
            }
            return;
        }
        if (e.enter()) toggle();
    }
    void paint(cairo_t* cr) {
        panel_bg(cr, W(), H());
        select_shell_font(cr, fs());
        hits_.clear();
        say(cr, 18, 24, self_.empty() ? "Tailscale" : self_, cfg.c_fg);
        cairo_set_font_size(cr, std::max(10.0, fs() - 2));
        say(cr, 18, 44, ip_.empty() ? (running_ ? "Connected" : "Off") : ip_,
            cfg.c_dim);
        cairo_set_font_size(cr, fs());
        std::string tog = running_ ? "Turn off" : "Turn on";
        double twid     = tw(cr, tog) + 16;
        col(cr, running_ ? cfg.c_accent : cfg.c_ws_bg, 1);
        rrect(cr, W() - 18 - twid, 16, twid, 22, 6);
        cairo_fill(cr);
        say(cr, W() - 18 - twid + 8, 27, tog,
            running_ ? contrast_on(cfg.c_accent) : cfg.c_fg);
        hits_.push_back({W() - 18.0 - twid, 16, twid, 22, 0, 0});
        if (!err_.empty() && !installed_) {
            say(cr, 18, 90, err_, cfg.c_urgent);
            return;
        }
        cairo_save(cr);
        cairo_rectangle(cr, 0, 62, W(), H() - 86);
        cairo_clip(cr);
        double y = 70 - scroll_ * 8.0;
        auto section = [&](const char* title) {
            cairo_set_font_size(cr, std::max(9.0, fs() - 1));
            say(cr, 18, y + 8, title, cfg.c_accent);
            cairo_set_font_size(cr, fs());
            y += 22;
        };
        if (running_ && (!shown_exits_.empty() || !regions_.empty())) {
            section("EXIT NODES");
            for (int i = 0; i < (int)shown_exits_.size(); ++i) {
                auto& e = shown_exits_[i];
                if (e.active) {
                    col(cr, cfg.c_accent, 0.28);
                    rrect(cr, 12, y - 2, W() - 24, 24, 6);
                    cairo_fill(cr);
                }
                say(cr, 18, y + 10,
                    utf8_trunc(e.add ? "+  " + e.display : e.display, 34),
                    cfg.c_fg);
                if (e.active)
                    say(cr, W() - 18 - tw(cr, "on"), y + 10, "on", cfg.c_accent);
                hits_.push_back({12, y - 2, W() - 24.0, 24, 3, i});
                y += 26;
            }
            if (picker_) {
                search_.focused = true;
                search_.draw(cr, 16, y, W() - 32, 28, "Search regions");
                y += 36;
                auto regs = filtered_regions();
                if (regs.empty())
                    say(cr, 18, y + 8, "No Mullvad regions found.", cfg.c_dim);
                for (int i = 0; i < (int)regs.size(); ++i) {
                    if (i == region_sel_) {
                        col(cr, cfg.c_accent, 0.22);
                        rrect(cr, 12, y - 2, W() - 24, 28, 6);
                        cairo_fill(cr);
                    }
                    say(cr, 18, y + 6, utf8_trunc(regs[i].city, 22), cfg.c_fg);
                    cairo_set_font_size(cr, std::max(9.0, fs() - 2));
                    say(cr, 18, y + 20, regs[i].country, cfg.c_dim);
                    cairo_set_font_size(cr, fs());
                    hits_.push_back({12, y - 2, W() - 24.0, 28, 4, i});
                    y += 30;
                }
            }
            y += 8;
        }
        section("MACHINES");
        if (peers_.empty() && installed_)
            say(cr, 18, y + 8, "No other machines yet", cfg.c_dim);
        for (int i = 0; i < (int)peers_.size(); ++i) {
            say(cr, 18, y + 8, utf8_trunc(peers_[i].name, 22), cfg.c_fg);
            say(cr, W() - 18 - tw(cr, peers_[i].ip), y + 8, peers_[i].ip,
                cfg.c_dim);
            hits_.push_back({14, y, W() - 28.0, 22, 1, i});
            y += 24;
        }
        cairo_restore(cr);
        if (!err_.empty() && installed_)
            say(cr, 18, H() - 24, err_, cfg.c_accent);
    }
};

// ---- Dropbox --------------------------------------------------------------
class DropboxOverlay : public Overlay {
public:
    const char* id() const override { return "omarchy.dropbox"; }
    void summon(const std::string&) override { open(); }
    void hide() override { host_.close(); }
    bool is_open() const override { return host_.is_open(); }
    ~DropboxOverlay() override { host_.close(); }

private:
    struct File {
        std::string name, path, folder;
        long        modified = 0;
    };
    Host host_;
    AsyncCmd cmd_;
    bool installed_ = false, running_ = false, authed_ = false;
    std::string status_, plan_, err_;
    double used_ = 0, quota_ = 0;
    std::vector<File> files_;
    int scroll_ = 0;
    int W() const { return 400; }
    int H() const { return 520; }
    double fs() const { return cfg.shell_network_font_size; }
    std::string helper() {
        const char* om = getenv("OMARCHY_PATH");
        return std::string(om && *om ? om : "/usr/share/omarchy") +
               "/shell/plugins/panels/dropbox/status.py";
    }
    void open() {
        scroll_ = 0;
        host_.win.paint = [this](cairo_t* cr) { paint(cr); };
        host_.win.click = [this](double x, double y, int b) { on_click(x, y, b); };
        host_.win.pscroll = [this](int d) {
            scroll_ = std::max(0, scroll_ + d);
            host_.redraw();
        };
        host_.win.pkey = [this](const Bar::KeyEvent& e) {
            if (!e.pressed) return;
            if (e.escape()) host_.dismiss();
            if (e.enter()) toggle();
        };
        host_.open(W(), H(), "mattbar-dropbox", id(), true, Host::Place::BarEnd);
        reload();
    }
    void reload() {
        auto* sh = mattbar_shell();
        if (!sh || !sh->bar()) return;
        cmd_.run(*sh->bar(), "python3 " + helper() + " 25 2>/dev/null",
                 [this](const std::string& out, int) {
                     parse(out);
                     host_.redraw();
                 },
                 8000);
    }
    void parse(const std::string& j) {
        files_.clear();
        installed_ = j.find("\"installed\": true") != std::string::npos;
        running_   = j.find("\"running\": true") != std::string::npos;
        authed_    = j.find("\"authenticated\": true") != std::string::npos;
        status_    = json_str(j, "statusText");
        plan_      = json_str(j, "plan");
        used_      = atof(json_str(j, "usedBytes").c_str());
        quota_     = atof(json_str(j, "quotaBytes").c_str());
        if (!installed_) err_ = "Dropbox is not installed";
        else err_.clear();
        for (auto& obj : json_object_array(j, "files")) {
            File f;
            f.name     = json_str(obj, "name");
            f.path     = json_str(obj, "path");
            f.folder   = json_str(obj, "folder");
            f.modified = (long)atof(json_str(obj, "modifiedTs").c_str());
            if (!f.name.empty()) files_.push_back(f);
        }
    }
    void toggle() {
        if (!installed_) {
            spawn_detached("omarchy-pkg add dropbox 2>/dev/null || true");
            return;
        }
        spawn_detached(running_ ? "dropbox-cli stop 2>/dev/null || dropbox stop"
                                : "dropbox-cli start 2>/dev/null || dropbox start");
        running_ = !running_;
        host_.redraw();
        reload();
    }
    void login() {
        spawn_detached("dropbox-cli start 2>/dev/null || dropbox start");
        reload();
    }
    void open_file(const File& f) {
        if (f.path.empty()) return;
        spawn_detached("nautilus --select " + ov::shell_quote(f.path) +
                       " 2>/dev/null || xdg-open " + ov::shell_quote(f.path));
    }
    std::vector<Hit> hits_;
    void on_click(double x, double y, int b) {
        if (b != BTN_LEFT) return;
        for (auto& h : hits_) {
            if (x < h.x || y < h.y || x >= h.x + h.w || y >= h.y + h.h) continue;
            if (h.kind == 0) toggle();
            if (h.kind == 2) login();
            if (h.kind == 1 && h.idx >= 0 && h.idx < (int)files_.size())
                open_file(files_[h.idx]);
        }
    }
    void paint(cairo_t* cr) {
        panel_bg(cr, W(), H());
        select_shell_font(cr, fs());
        hits_.clear();
        say(cr, 18, 26, "Dropbox", cfg.c_fg);
        cairo_set_font_size(cr, std::max(10.0, fs() - 2));
        say(cr, 18, 46,
            status_.empty() ? (installed_ ? "Stopped" : "Not installed")
                            : status_,
            cfg.c_dim);
        cairo_set_font_size(cr, fs());
        std::string tog =
            !installed_ ? "Install" : (running_ ? "Pause" : "Resume");
        double twid = tw(cr, tog) + 16;
        col(cr, running_ && authed_ ? cfg.c_accent : cfg.c_ws_bg, 1);
        rrect(cr, W() - 18 - twid, 16, twid, 22, 6);
        cairo_fill(cr);
        say(cr, W() - 18 - twid + 8, 27, tog,
            running_ && authed_ ? contrast_on(cfg.c_accent) : cfg.c_fg);
        hits_.push_back({W() - 18.0 - twid, 16, twid, 22, 0, 0});
        if (!err_.empty() && !installed_) {
            say(cr, 18, 90, err_, cfg.c_urgent);
            return;
        }
        double y = 70;
        if (!authed_ && installed_) {
            col(cr, cfg.c_ws_bg, 1);
            rrect(cr, 14, y, W() - 28, 48, 8);
            cairo_fill(cr);
            say(cr, 24, y + 16, "Login to Dropbox", cfg.c_fg);
            cairo_set_font_size(cr, std::max(10.0, fs() - 2));
            say(cr, 24, y + 34, "Start the authentication flow", cfg.c_dim);
            cairo_set_font_size(cr, fs());
            hits_.push_back({14, y, W() - 28.0, 48, 2, 0});
            y += 60;
        }
        if (authed_ && quota_ > 0) {
            cairo_set_font_size(cr, std::max(9.0, fs() - 1));
            say(cr, 18, y + 8, "STORAGE", cfg.c_accent);
            draw_slider(cr, 18, y + 20, W() - 36, 8,
                        std::clamp(used_ / quota_, 0.0, 1.0));
            cairo_set_font_size(cr, fs());
            std::string usage =
                format_bytes(used_) + " of " + format_bytes(quota_);
            say(cr, 18, y + 42, usage, cfg.c_fg);
            if (!plan_.empty())
                say(cr, W() - 18 - tw(cr, plan_), y + 42, plan_, cfg.c_dim);
            y += 62;
        }
        if (authed_) {
            cairo_set_font_size(cr, std::max(9.0, fs() - 1));
            say(cr, 18, y + 8, "RECENT FILES", cfg.c_accent);
            cairo_set_font_size(cr, fs());
            y += 24;
            if (files_.empty())
                say(cr, 18, y + 8, "No synced files found.", cfg.c_dim);
            int start = std::min(scroll_, std::max(0, (int)files_.size() - 1));
            for (int i = start; i < (int)files_.size(); ++i) {
                if (y > H() - 36) break;
                auto& f = files_[i];
                cairo_set_font_size(cr, std::max(9.0, fs() - 2));
                say(cr, 18, y + 6, file_glyph(f.name), cfg.c_dim);
                cairo_set_font_size(cr, fs());
                say(cr, 52, y + 6, utf8_trunc(f.name, 28), cfg.c_fg);
                cairo_set_font_size(cr, std::max(9.0, fs() - 2));
                std::string meta = ago(f.modified);
                if (!f.folder.empty())
                    meta += (meta.empty() ? "" : " \u00b7 ") + f.folder;
                say(cr, 52, y + 22, utf8_trunc(meta, 36), cfg.c_dim);
                cairo_set_font_size(cr, fs());
                hits_.push_back({14, y, W() - 28.0, 32, 1, i});
                y += 36;
            }
        }
        if (!err_.empty() && installed_)
            say(cr, 18, H() - 24, err_, cfg.c_accent);
    }
};

} // namespace

Overlay* make_timezone_overlay() { return new TimezoneOverlay; }
Overlay* make_power_overlay() { return new PowerOverlay; }
Overlay* make_reminders_overlay() { return new RemindersOverlay; }
Overlay* make_tailscale_overlay() { return new TailscaleOverlay; }
Overlay* make_dropbox_overlay() { return new DropboxOverlay; }

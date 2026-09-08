// Network panel — laid out like Omarchy's Quickshell omarchy.network:
// hero (SSID / Ethernet + radio + QR + speed test), live stats, band
// pin, DNS provider, then KNOWN / OTHER wifi rows with a PSK prompt.
#include "overlay.hpp"
#include "util.hpp"

#include <linux/input-event-codes.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <map>
#include <sstream>
#include <vector>

namespace {

using ov::Host;
using ov::col;
using ov::say;
using ov::tw;
using ov::rrect;
using ov::panel_bg;
using ov::select_shell_font;
using ov::shell_quote;
using ov::utf8_trunc;
using ov::draw_pill;
using ov::format_bytes;
using ov::format_rate;
using ov::format_ping;
using ov::parse_kv;
using ov::wifi_icon;

struct Hit {
    double x, y, w, h;
    int    row  = -1;
    int    kind = 0; // 0 wifi, 4 wifi-toggle, 5 qr, 6 speed, 7 band-auto,
                     // 8 band-pill, 9 dns-pill, 10 forget, 11 scan
    std::string tag;
};
int hit_at(const std::vector<Hit>& hits, double x, double y) {
    for (int i = (int)hits.size() - 1; i >= 0; --i) {
        const Hit& h = hits[i];
        if (x >= h.x && y >= h.y && x < h.x + h.w && y < h.y + h.h) return i;
    }
    return -1;
}

std::vector<std::string> split_esc(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            cur += s[++i];
            continue;
        }
        if (s[i] == sep) {
            out.push_back(cur);
            cur.clear();
        } else cur += s[i];
    }
    out.push_back(cur);
    return out;
}

struct NetRow {
    std::string ssid, security;
    int  signal = 0;
    bool in_use = false;
    bool saved  = false;
};

class NetworkOverlay : public Overlay {
public:
    const char* id() const override { return "omarchy.network"; }
    void summon(const std::string&) override { open(); }
    void hide() override { close(); }
    bool is_open() const override { return host_.is_open(); }
    ~NetworkOverlay() override { close(); }

private:
    Host host_;
    AsyncCmd list_cmd_, details_cmd_;
    TextField psk_;
    std::vector<NetRow> all_, nets_;
    std::vector<Hit> hits_;
    std::map<std::string, std::string> info_;
    std::string filter_, psk_ssid_, dns_, band_current_, band_selected_ = "auto";
    std::vector<std::string> bands_;
    int sel_ = 0, scroll_ = 0, poll_fd_ = -1;
    bool wifi_on_ = true, loading_ = false, scanning_ = false;
    double prev_rx_ = 0, prev_tx_ = 0, prev_t_ = 0;
    double down_rate_ = 0, up_rate_ = 0;
    double inet_ping_ = -1;
    int    pkt_loss_  = 0;
    bool   have_xfer_ = false, have_ping_ = false;

    static constexpr const char* kDns[] = {"DHCP", "Cloudflare", "Google",
                                           "Custom"};

    int W() const { return cfg.shell_panel_width; }
    int H() const { return std::max(cfg.shell_panel_height, 560); }
    double fs() const { return cfg.shell_network_font_size; }

    void close() {
        disarm_poll();
        psk_.clear();
        psk_ssid_.clear();
        filter_.clear();
        host_.close();
    }

    void arm_poll() {
        auto* sh = mattbar_shell();
        if (!sh || !sh->bar()) return;
        if (poll_fd_ < 0) {
            poll_fd_ = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
            sh->bar()->add_fd(poll_fd_, [this](uint32_t) {
                uint64_t n;
                while (read(poll_fd_, &n, sizeof n) > 0) {}
                if (host_.is_open()) refresh_details();
            }, "net-panel-poll");
        }
        itimerspec ts{};
        ts.it_interval.tv_sec  = 1;
        ts.it_interval.tv_nsec = 500 * 1000000L;
        ts.it_value            = ts.it_interval;
        timerfd_settime(poll_fd_, 0, &ts, nullptr);
    }
    void disarm_poll() {
        if (poll_fd_ < 0) return;
        itimerspec off{};
        timerfd_settime(poll_fd_, 0, &off, nullptr);
    }

    void open() {
        sel_ = scroll_ = 0;
        psk_.password = true;
        prev_t_ = 0;
        have_xfer_ = have_ping_ = false;
        host_.win.paint = [this](cairo_t* cr) { paint(cr); };
        host_.win.click = [this](double x, double y, int b) { on_click(x, y, b); };
        host_.win.pscroll = [this](int d) {
            sel_ = ov::clamp_sel(sel_ + d, (int)nets_.size());
            host_.redraw();
        };
        host_.win.pkey = [this](const Bar::KeyEvent& e) { on_key(e); };
        host_.open(W(), H(), "mattbar-network", id(), true, Host::Place::BarEnd);
        // Cached list only. A live rescan is the Scan button (or R), and
        // never starts on its own while we already have a connection.
        reload_list(false);
        refresh_details();
        arm_poll();
    }

    void reload_list(bool scan) {
        auto* sh = mattbar_shell();
        if (!sh || !sh->bar()) return;
        loading_ = true;
        scanning_ = scan;
        std::string cmd =
            "echo WIFI $(nmcli -t radio wifi 2>/dev/null); "
            "echo ---SAVED---; "
            "nmcli -t -e yes -f NAME connection show 2>/dev/null; "
            "echo ---NET---; "
            "nmcli -t -e yes -f IN-USE,SSID,SIGNAL,SECURITY device wifi list "
            "--rescan " +
            std::string(scan ? "yes" : "no") + " 2>/dev/null";
        list_cmd_.run(*sh->bar(), cmd,
                      [this](const std::string& out, int) {
                          parse_list(out);
                          loading_ = scanning_ = false;
                          host_.redraw();
                      },
                      scan ? 8000 : 3000);
    }

    void refresh_details() {
        auto* sh = mattbar_shell();
        if (!sh || !sh->bar()) return;
        details_cmd_.run(
            *sh->bar(),
            "echo '===STATUS==='; omarchy-network-status --verbose 2>/dev/null; "
            "echo '===BAND==='; omarchy-network-band 2>/dev/null; "
            "echo '===DNS==='; omarchy-dns 2>/dev/null",
            [this](const std::string& out, int) {
                parse_details(out);
                host_.redraw();
            },
            4000);
    }

    void parse_list(const std::string& out) {
        wifi_on_ = out.find("WIFI enabled") != std::string::npos ||
                   out.find("WIFI yes") != std::string::npos;
        if (out.find("WIFI disabled") != std::string::npos ||
            out.find("WIFI no") != std::string::npos)
            wifi_on_ = false;
        std::map<std::string, bool> saved;
        auto sp = out.find("---SAVED---");
        auto np = out.find("---NET---");
        if (sp != std::string::npos) {
            std::istringstream ss(out.substr(
                sp + 12, np == std::string::npos ? np : np - (sp + 12)));
            std::string line;
            while (std::getline(ss, line)) {
                line = trim(line);
                if (!line.empty() && line.find("---") == std::string::npos)
                    saved[line] = true;
            }
        }
        all_.clear();
        std::map<std::string, int> seen;
        if (np != std::string::npos) {
            std::istringstream ss(out.substr(np + 9));
            std::string line;
            while (std::getline(ss, line)) {
                if (line.empty()) continue;
                auto f = split_esc(line, ':');
                if (f.size() < 3) continue;
                NetRow r;
                r.in_use   = !f[0].empty() && f[0][0] == '*';
                r.ssid     = f.size() > 1 ? f[1] : "";
                r.signal   = f.size() > 2 ? atoi(f[2].c_str()) : 0;
                r.security = f.size() > 3 ? f[3] : "";
                if (r.ssid.empty()) continue;
                r.saved = saved.count(r.ssid) != 0;
                auto it = seen.find(r.ssid);
                if (it != seen.end()) {
                    if (r.in_use || r.signal > all_[it->second].signal)
                        all_[it->second] = r;
                    continue;
                }
                seen[r.ssid] = (int)all_.size();
                all_.push_back(std::move(r));
            }
        }
        std::sort(all_.begin(), all_.end(), [](const NetRow& a, const NetRow& b) {
            if (a.in_use != b.in_use) return a.in_use;
            if (a.saved != b.saved) return a.saved;
            return a.signal > b.signal;
        });
        apply_filter();
    }

    void apply_filter() {
        if (filter_.empty()) nets_ = all_;
        else {
            std::string q = filter_;
            std::transform(q.begin(), q.end(), q.begin(),
                           [](unsigned char c) { return (char)std::tolower(c); });
            nets_.clear();
            for (auto& n : all_) {
                std::string l = n.ssid;
                std::transform(l.begin(), l.end(), l.begin(),
                               [](unsigned char c) { return (char)std::tolower(c); });
                if (l.find(q) != std::string::npos) nets_.push_back(n);
            }
        }
        sel_ = ov::clamp_sel(sel_, (int)nets_.size());
    }

    void parse_details(const std::string& out) {
        auto st = out.find("===STATUS===");
        auto bd = out.find("===BAND===");
        auto dn = out.find("===DNS===");
        if (st != std::string::npos) {
            info_ = parse_kv(out.substr(
                st + 12, bd == std::string::npos ? bd : bd - (st + 12)));
            double now = time(nullptr);
            // sub-second: use clock_gettime
            timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            now = ts.tv_sec + ts.tv_nsec / 1e9;
            double rx = atof(info_["rx_bytes"].c_str());
            double tx = atof(info_["tx_bytes"].c_str());
            if (prev_t_ > 0 && now > prev_t_ && info_["iface"] != "") {
                down_rate_ = std::max(0.0, (rx - prev_rx_) / (now - prev_t_));
                up_rate_   = std::max(0.0, (tx - prev_tx_) / (now - prev_t_));
                have_xfer_ = true;
            }
            prev_rx_ = rx;
            prev_tx_ = tx;
            prev_t_  = now;
            if (info_.count("internet_ping_ms")) {
                std::string p = info_["internet_ping_ms"];
                have_ping_    = true;
                inet_ping_    = p.empty() ? -1 : atof(p.c_str());
                pkt_loss_     = p.empty() ? 100 : 0;
            }
        }
        if (bd != std::string::npos) {
            auto kv = parse_kv(out.substr(
                bd + 10, dn == std::string::npos ? dn : dn - (bd + 10)));
            band_current_  = kv["band"];
            band_selected_ = kv["selected"].empty() ? "auto" : kv["selected"];
            bands_.clear();
            std::istringstream bs(kv["available"]);
            std::string tok;
            while (bs >> tok) bands_.push_back(tok);
        }
        if (dn != std::string::npos) {
            dns_ = trim(out.substr(dn + 9));
            if (dns_.empty()) dns_ = "DHCP";
        }
    }

    std::string inf(const char* k) const {
        auto it = info_.find(k);
        return it == info_.end() ? std::string() : it->second;
    }
    bool can_select_band() const {
        return inf("type") == "wifi" &&
               (bands_.size() > 1 || band_selected_ != "auto");
    }
    bool wifi_connected() const {
        return inf("type") == "wifi" && !inf("ssid").empty();
    }

    std::string hero_title() const {
        if (inf("type") == "wifi") {
            std::string s = inf("ssid").empty() ? "Wi-Fi" : inf("ssid");
            return s;
        }
        if (inf("type") == "ethernet") {
            std::string sp = inf("speed");
            if (!sp.empty()) {
                int v = atoi(sp.c_str());
                if (v >= 1000)
                    return std::string("Ethernet (") +
                           (v % 1000 == 0 ? std::to_string(v / 1000) + "gbit)"
                                          : std::to_string(v / 1000.0).substr(0, 3) +
                                                "gbit)");
                return "Ethernet (" + sp + "mbit)";
            }
            return "Ethernet";
        }
        return wifi_on_ ? "No connection" : "Disconnected";
    }
    std::string hero_meta() const {
        if (inf("type") == "wifi" || inf("type") == "ethernet") {
            static const char* phrases[] = {
                "Wiring bits", "Handling packets", "Sorting frames",
                "Hauling bytes", "Routing crumbs", "Counting collisions",
                "Bending light"};
            return phrases[(time(nullptr) / 3) % 7];
        }
        return "Not connected";
    }
    static std::string band_label(const std::string& b) {
        if (b == "auto") return "Auto";
        if (b.empty()) return "";
        return b + "ghz";
    }
    std::string band_title() const {
        if (band_selected_ != "auto") return "WI-FI BAND";
        std::string l = band_label(band_current_);
        if (l.empty()) return "WI-FI BAND";
        std::string u = l;
        std::transform(u.begin(), u.end(), u.begin(),
                       [](unsigned char c) { return (char)std::toupper(c); });
        return "WI-FI BAND: " + u;
    }
    static bool is_open_sec(const std::string& sec) {
        return sec.empty() || sec == "--" || sec == "none";
    }

    void connect_sel() {
        if (sel_ < 0 || sel_ >= (int)nets_.size()) return;
        const NetRow& n = nets_[sel_];
        if (n.in_use) {
            spawn_detached("nmcli connection down id " + shell_quote(n.ssid));
            reload_list(false);
            return;
        }
        if (n.saved || is_open_sec(n.security)) {
            spawn_detached(n.saved ? "nmcli connection up id " + shell_quote(n.ssid)
                                   : "nmcli device wifi connect " +
                                         shell_quote(n.ssid));
            reload_list(false);
            return;
        }
        psk_ssid_ = n.ssid;
        psk_.clear();
        psk_.password = true;
        host_.redraw();
    }
    void submit_psk() {
        if (psk_ssid_.empty()) return;
        spawn_detached("nmcli device wifi connect " + shell_quote(psk_ssid_) +
                       " password " + shell_quote(psk_.text));
        psk_ssid_.clear();
        psk_.clear();
        reload_list(false);
    }
    void forget_sel() {
        if (sel_ < 0 || sel_ >= (int)nets_.size()) return;
        spawn_detached("nmcli connection delete id " +
                       shell_quote(nets_[sel_].ssid));
        reload_list(false);
    }
    void set_dns(const std::string& p) {
        if (p == "Custom") {
            spawn_detached(
                "omarchy-launch-floating-terminal-with-presentation "
                "omarchy-dns Custom");
            close();
            return;
        }
        spawn_detached("omarchy-dns " + shell_quote(p));
        dns_ = p;
        host_.redraw();
    }
    void set_band(const std::string& b) {
        spawn_detached("omarchy-network-band " + b);
        band_selected_ = b;
        host_.redraw();
    }

    void on_click(double x, double y, int btn) {
        int hi = hit_at(hits_, x, y);
        if (hi < 0) return;
        const Hit& h = hits_[hi];
        if (h.kind == 4) {
            spawn_detached(std::string("nmcli radio wifi ") +
                           (wifi_on_ ? "off" : "on"));
            reload_list(false);
            return;
        }
        if (h.kind == 11) {
            if (!scanning_) reload_list(true);
            return;
        }
        if (h.kind == 5) {
            auto* sh = mattbar_shell();
            if (sh) sh->summon("omarchy.wifiqr", "{}");
            return;
        }
        if (h.kind == 6) {
            auto* sh = mattbar_shell();
            if (sh)
                sh->summon("omarchy.speedtest",
                           std::string("{\"connection\":\"") + hero_title() +
                               "\"}");
            return;
        }
        if (h.kind == 7) {
            if (band_selected_ != "auto") set_band("auto");
            else if (!band_current_.empty()) set_band(band_current_);
            return;
        }
        if (h.kind == 8) {
            set_band(h.tag);
            return;
        }
        if (h.kind == 9) {
            set_dns(h.tag);
            return;
        }
        if (h.kind == 10 || btn == BTN_RIGHT) {
            if (h.row >= 0) sel_ = h.row;
            forget_sel();
            return;
        }
        if (h.row < 0 || h.row >= (int)nets_.size()) return;
        sel_ = h.row;
        if (btn == BTN_LEFT) connect_sel();
    }

    void on_key(const Bar::KeyEvent& e) {
        if (!e.pressed) return;
        if (!psk_ssid_.empty()) {
            if (e.escape()) {
                psk_ssid_.clear();
                psk_.clear();
                host_.redraw();
                return;
            }
            if (e.enter()) {
                submit_psk();
                return;
            }
            if (psk_.handle(e)) host_.redraw();
            return;
        }
        if (e.escape()) {
            if (!filter_.empty()) {
                filter_.clear();
                apply_filter();
                host_.redraw();
                return;
            }
            close();
            return;
        }
        if (e.enter()) {
            connect_sel();
            return;
        }
        if (e.up()) {
            if (sel_ > 0) --sel_;
            host_.redraw();
            return;
        }
        if (e.down()) {
            if (sel_ + 1 < (int)nets_.size()) ++sel_;
            host_.redraw();
            return;
        }
        if (e.keysym == 0x72 || e.keysym == 0x52) {
            reload_list(true);
            refresh_details();
            return;
        }
        if (e.keysym == 0x77 || e.keysym == 0x57) {
            spawn_detached(std::string("nmcli radio wifi ") +
                           (wifi_on_ ? "off" : "on"));
            reload_list(false);
            return;
        }
        TextField dummy;
        dummy.text   = filter_;
        dummy.cursor = filter_.size();
        if (dummy.handle(e)) {
            filter_ = dummy.text;
            apply_filter();
            host_.redraw();
        }
    }

    void paint(cairo_t* cr) {
        panel_bg(cr, W(), H());
        select_shell_font(cr, fs());
        hits_.clear();
        const int rh = ov::row_h(fs());
        double y = 14;

        // ---- Hero --------------------------------------------------------
        const char* ic =
            info_["type"] == "ethernet"
                ? "\U000F1200"
                : (info_["type"] == "wifi" ? wifi_icon(atoi(info_.count("signal")
                                                                ? info_["signal"].c_str()
                                                                : "70"))
                                           : "\U000F092E");
        cairo_set_font_size(cr, fs() + 8);
        say(cr, 16, y + 16, ic, cfg.c_fg);
        cairo_set_font_size(cr, fs());
        say(cr, 52, y + 10, utf8_trunc(hero_title(), 22), cfg.c_fg);
        {
            std::string meta = hero_meta();
            std::transform(meta.begin(), meta.end(), meta.begin(),
                           [](unsigned char c) { return (char)std::toupper(c); });
            cairo_set_font_size(cr, std::max(10.0, fs() - 3));
            say(cr, 52, y + 28, meta, cfg.c_dim);
            cairo_set_font_size(cr, fs());
        }
        double ax = W() - 18;
        auto hero_btn = [&](const char* label, int kind) {
            double bw = tw(cr, label) + 14;
            ax -= bw;
            col(cr, cfg.c_ws_bg, 1);
            rrect(cr, ax, y + 6, bw, 24, 6);
            cairo_fill(cr);
            say(cr, ax + 7, y + 18, label, cfg.c_fg);
            hits_.push_back({ax, y + 6, bw, 24, -1, kind, {}});
            ax -= 8;
        };
        {
            std::string tog = wifi_on_ ? "On" : "Off";
            double bw = tw(cr, tog) + 16;
            ax -= bw;
            col(cr, wifi_on_ ? cfg.c_accent : cfg.c_ws_bg, 1);
            rrect(cr, ax, y + 6, bw, 24, 6);
            cairo_fill(cr);
            say(cr, ax + 8, y + 18, tog,
                wifi_on_ ? contrast_on(cfg.c_accent) : cfg.c_dim);
            hits_.push_back({ax, y + 6, bw, 24, -1, 4, {}});
            ax -= 8;
        }
        if (!info_["iface"].empty()) hero_btn("Speed", 6);
        if (wifi_connected()) hero_btn("QR", 5);
        if (wifi_on_) {
            const char* sl = scanning_ ? "Scanning" : "Scan";
            double bw = tw(cr, sl) + 14;
            ax -= bw;
            col(cr, scanning_ ? cfg.c_accent : cfg.c_ws_bg, 1);
            rrect(cr, ax, y + 6, bw, 24, 6);
            cairo_fill(cr);
            say(cr, ax + 7, y + 18, sl,
                scanning_ ? contrast_on(cfg.c_accent) : cfg.c_fg);
            hits_.push_back({ax, y + 6, bw, 24, -1, 11, {}});
        }
        y += 48;

        // ---- Stats (same grid as Quickshell: ping/loss, rx/tx, totals, IP) -
        if (!info_["iface"].empty()) {
            auto cell = [&](const char* lab, const std::string& val, bool urg) {
                cairo_set_font_size(cr, std::max(10.0, fs() - 3));
                say(cr, 18, y + 8, lab, cfg.c_dim);
                cairo_set_font_size(cr, fs());
                say(cr, 18, y + 24, val.empty() ? "--" : val,
                    urg ? cfg.c_urgent : cfg.c_fg);
            };
            double colw = (W() - 36) / 2.0;
            auto row2 = [&](const char* l1, const std::string& v1, bool u1,
                            const char* l2, const std::string& v2, bool u2) {
                cairo_save(cr);
                cell(l1, v1, u1);
                cairo_translate(cr, colw, 0);
                cell(l2, v2, u2);
                cairo_restore(cr);
                y += 36;
            };
            row2("Ping", format_ping(inet_ping_, have_ping_),
                 have_ping_ && pkt_loss_ > 0, "Packet Loss",
                 have_ping_ ? (std::to_string(pkt_loss_) + "%") : "--",
                 pkt_loss_ > 0);
            row2("Receiving", have_xfer_ ? format_rate(down_rate_) : "--", false,
                 "Sending", have_xfer_ ? format_rate(up_rate_) : "--", false);
            row2("Downloaded",
                 have_xfer_ ? format_bytes(atof(info_["rx_bytes"].c_str())) : "--",
                 false, "Uploaded",
                 have_xfer_ ? format_bytes(atof(info_["tx_bytes"].c_str())) : "--",
                 false);
            row2("IP Address", info_["ip"].empty() ? "--" : info_["ip"], false,
                 "Gateway", info_["gateway"].empty() ? "--" : info_["gateway"],
                 false);
        }

        // ---- Band --------------------------------------------------------
        if (can_select_band()) {
            cairo_set_font_size(cr, std::max(10.0, fs() - 2));
            say(cr, 18, y + 10, band_title(), cfg.c_accent);
            cairo_set_font_size(cr, fs());
            bool auto_on = band_selected_ == "auto";
            double pw = draw_pill(cr, W() - 18 - tw(cr, "Auto") - 20, y, "Auto",
                                  auto_on, false);
            hits_.push_back({W() - 18.0 - tw(cr, "Auto") - 20, y, pw, 24, -1, 7, {}});
            y += 28;
            if (!auto_on) {
                double x = 18;
                for (auto& b : bands_) {
                    bool on = band_current_ == b || band_selected_ == b;
                    double w = draw_pill(cr, x, y, band_label(b), on, false);
                    hits_.push_back({x, y, w, 24, -1, 8, b});
                    x += w + 8;
                }
                y += 32;
            } else y += 8;
        }

        // ---- DNS ---------------------------------------------------------
        {
            cairo_set_font_size(cr, std::max(10.0, fs() - 2));
            say(cr, 18, y + 10, "DNS", cfg.c_accent);
            cairo_set_font_size(cr, fs());
            y += 22;
            double x = 18;
            for (auto* p : kDns) {
                bool on = dns_ == p;
                double w = draw_pill(cr, x, y, p, on, false);
                hits_.push_back({x, y, w, 24, -1, 9, p});
                x += w + 8;
            }
            y += 36;
        }

        if (!psk_ssid_.empty()) {
            say(cr, 18, y + 10, "Password for " + psk_ssid_, cfg.c_fg);
            psk_.draw(cr, 18, y + 26, W() - 36, ov::search_h(fs()), "passphrase");
            say(cr, 18, y + 26 + ov::search_h(fs()) + 16,
                "Enter to connect   Esc to cancel", cfg.c_dim);
            return;
        }

        if (scanning_) {
            cairo_set_font_size(cr, std::max(10.0, fs() - 2));
            say(cr, 18, y + 10, "SCANNING WI-FI\u2026", cfg.c_accent);
            cairo_set_font_size(cr, fs());
            y += 24;
        }

        int vis = std::max(1, (int)((H() - y - 8) / rh));
        ov::keep_visible(sel_, scroll_, vis, (int)nets_.size());
        bool shown_known = false, shown_other = false;
        for (int i = 0; i < vis; ++i) {
            int idx = scroll_ + i;
            if (idx >= (int)nets_.size()) break;
            const NetRow& n = nets_[idx];
            bool known = n.saved || n.in_use;
            if (known && !shown_known) {
                cairo_set_font_size(cr, std::max(10.0, fs() - 2));
                say(cr, 18, y + 10, "KNOWN NETWORKS", cfg.c_accent);
                cairo_set_font_size(cr, fs());
                y += 22;
                shown_known = true;
                --i;
                continue;
            }
            if (!known && !shown_other) {
                cairo_set_font_size(cr, std::max(10.0, fs() - 2));
                say(cr, 18, y + 10, "OTHER NETWORKS", cfg.c_accent);
                cairo_set_font_size(cr, fs());
                y += 22;
                shown_other = true;
                --i;
                continue;
            }
            double mid = y + rh / 2.0;
            if (idx == sel_) {
                col(cr, cfg.c_accent, 0.28);
                rrect(cr, 10, y + 2, W() - 20, rh - 4, 6);
                cairo_fill(cr);
            }
            Color fg = idx == sel_ ? contrast_on(cfg.c_accent) : cfg.c_fg;
            say(cr, 18, mid, wifi_icon(n.signal), n.in_use ? cfg.c_accent : fg);
            std::string lab = utf8_trunc(n.ssid, 22);
            say(cr, 44, mid, lab, n.in_use ? cfg.c_accent : fg);
            std::string st = n.in_use ? "Connected" : (n.saved ? "" : "");
            if (!is_open_sec(n.security)) {
                say(cr, W() - 36, mid, "\U000F033E", cfg.c_dim); // lock
                if (n.saved && !n.in_use)
                    hits_.push_back(
                        {W() - 48.0, y, 36, (double)rh, idx, 10, n.ssid});
            }
            if (!st.empty())
                say(cr, W() - 18 - tw(cr, st) - (is_open_sec(n.security) ? 0 : 22),
                    mid, st, cfg.c_dim);
            hits_.push_back({10, y, W() - 56.0, (double)rh, idx, 0, n.ssid});
            y += rh;
        }
        if (!filter_.empty())
            say(cr, 18, H() - 16, "/" + filter_, cfg.c_dim);
        (void)loading_;
    }
};

} // namespace

Overlay* make_network_overlay() { return new NetworkOverlay; }

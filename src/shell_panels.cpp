// Audio, network, bluetooth, and display panels. Named overlays so
// `omarchy-shell shell toggle omarchy.audio` (and Super+Ctrl+A/B/W/D)
// land here once Quickshell is shut down. Backends are the same CLIs
// Omarchy's QML panels already call: pactl, nmcli, bluetoothctl, hyprctl.
#include "overlay.hpp"
#include "audio.hpp"
#include "sdpump.hpp"
#include "util.hpp"
#include "weather.hpp"

#include <linux/input-event-codes.h>
#include <dirent.h>
#include <unistd.h>

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
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
using ov::draw_slider;
using ov::draw_checkbox;

int parse_pct_line(const std::string& line) {
    auto p = line.find('%');
    if (p == std::string::npos || p == 0) return 0;
    size_t b = p;
    while (b > 0 && std::isdigit(static_cast<unsigned char>(line[b - 1]))) --b;
    return std::clamp(atoi(line.c_str() + b), 0, 150);
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

struct Hit {
    double x, y, w, h;
    int    row  = -1;
    int    kind = 0; // 0 row, 1 slider, 2 mute, 3 action, 4 header
};

int hit_at(const std::vector<Hit>& hits, double x, double y, int want = -1) {
    for (int i = (int)hits.size() - 1; i >= 0; --i) {
        const Hit& h = hits[i];
        if (x < h.x || y < h.y || x >= h.x + h.w || y >= h.y + h.h) continue;
        if (want >= 0 && h.kind != want) continue;
        return i;
    }
    return -1;
}

// ---------------------------------------------------------------------------
// Audio
// ---------------------------------------------------------------------------
struct ARow {
    enum Kind { Header, Slider, Device, App } kind = Device;
    std::string id, label, detail;
    int  pct    = 0;
    bool muted  = false;
    bool is_def = false;
    bool is_src = false;
};

class AudioOverlay : public Overlay {
public:
    const char* id() const override { return "omarchy.audio"; }
    void summon(const std::string&) override { open(); }
    void hide() override { close(); }
    bool is_open() const override { return host_.is_open(); }
    ~AudioOverlay() override { close(); }

private:
    Host host_;
    AsyncCmd cmd_;
    std::vector<ARow> rows_;
    std::vector<Hit> hits_;
    int sel_ = 0, scroll_ = 0, drag_ = -1;
    int sub_ = 0;
    bool loading_ = false;

    int W() const { return cfg.shell_panel_width; }
    int H() const { return cfg.shell_panel_height; }
    double fs() const { return cfg.shell_audio_font_size; }

    void close() {
        if (sub_ && mattbar_shell() && mattbar_shell()->bar()) {
            audio_events().unsubscribe(sub_);
            sub_ = 0;
        }
        drag_ = -1;
        host_.close();
    }

    void open() {
        auto* sh = mattbar_shell();
        if (!sh || !sh->bar()) return;
        if (!sub_) {
            sub_ = audio_events().subscribe(*sh->bar(), [this](bool, bool) {
                if (host_.is_open()) reload();
            });
        }
        sel_ = scroll_ = 0;
        host_.win.paint   = [this](cairo_t* cr) { paint(cr); };
        host_.win.click   = [this](double x, double y, int b) { on_click(x, y, b); };
        host_.win.pmotion = [this](double x, double) { on_drag(x); };
        host_.win.prelease = [this](int) {
            if (drag_ >= 0) apply_slider(drag_, true);
            drag_ = -1;
        };
        host_.win.pscroll = [this](int d) {
            sel_ = ov::clamp_sel(sel_ + d, (int)rows_.size());
            host_.redraw();
        };
        host_.win.pkey = [this](const Bar::KeyEvent& e) { on_key(e); };
        host_.open(W(), H(), "mattbar-audio", id(), true, Host::Place::BarEnd);
        reload();
    }

    void reload() {
        auto* sh = mattbar_shell();
        if (!sh || !sh->bar()) return;
        loading_ = true;
        cmd_.run(*sh->bar(),
                 "pactl get-default-sink 2>/dev/null; echo '==='; "
                 "pactl get-default-source 2>/dev/null; echo '==='; "
                 "pactl list sinks 2>/dev/null; echo '===IN==='; "
                 "pactl list sink-inputs 2>/dev/null; echo '===SRC==='; "
                 "pactl list sources 2>/dev/null",
                 [this](const std::string& out, int) {
                     parse(out);
                     loading_ = false;
                     host_.redraw();
                 },
                 2500);
    }

    void parse_devices(const std::string& blob, const char* kind,
                       const std::string& def, bool sources,
                       std::vector<ARow>& into) {
        std::string tag = std::string(kind) + " #";
        size_t p = 0;
        while ((p = blob.find(tag, p)) != std::string::npos) {
            size_t n = blob.find(tag, p + tag.size());
            std::string b = blob.substr(p, n == std::string::npos ? n : n - p);
            ARow r;
            r.kind   = ARow::Device;
            r.is_src = sources;
            auto np  = b.find("Name:");
            if (np != std::string::npos) {
                auto e = b.find('\n', np);
                r.id   = trim(b.substr(np + 5, e - (np + 5)));
            }
            auto dp = b.find("Description:");
            if (dp != std::string::npos) {
                auto e   = b.find('\n', dp);
                r.label  = trim(b.substr(dp + 12, e - (dp + 12)));
            }
            if (r.label.empty()) r.label = r.id;
            auto mp = b.find("Mute:");
            if (mp != std::string::npos) {
                auto e = b.find('\n', mp);
                r.muted = b.substr(mp, e - mp).find("yes") != std::string::npos;
            }
            auto vp = b.find("\nVolume:");
            if (vp == std::string::npos) vp = b.find("Volume:");
            if (vp != std::string::npos) {
                auto e = b.find('\n', vp + 1);
                r.pct  = parse_pct_line(b.substr(vp, e - vp));
            }
            r.is_def = !r.id.empty() && r.id == def;
            if (sources && r.id.size() > 8 &&
                r.id.compare(r.id.size() - 8, 8, ".monitor") == 0) {
                p = n == std::string::npos ? blob.size() : n;
                continue;
            }
            if (!r.id.empty()) into.push_back(std::move(r));
            p = n == std::string::npos ? blob.size() : n;
        }
    }

    void parse_apps(const std::string& blob, std::vector<ARow>& into) {
        const char* tag = "Sink Input #";
        size_t p = 0;
        while ((p = blob.find(tag, p)) != std::string::npos) {
            size_t n = blob.find(tag, p + 12);
            std::string b = blob.substr(p, n == std::string::npos ? n : n - p);
            ARow r;
            r.kind = ARow::App;
            r.id   = trim(b.substr(12, b.find('\n', 12) - 12));
            auto ap = b.find("application.name = \"");
            if (ap != std::string::npos) {
                ap += 20;
                auto e = b.find('"', ap);
                r.label = b.substr(ap, e - ap);
            }
            if (r.label.empty() || r.label == "EasyEffects" ||
                r.label.rfind("omarchy_speaker", 0) == 0) {
                p = n == std::string::npos ? blob.size() : n;
                continue;
            }
            auto mp = b.find("Mute:");
            if (mp != std::string::npos) {
                auto e = b.find('\n', mp);
                r.muted = b.substr(mp, e - mp).find("yes") != std::string::npos;
            }
            auto vp = b.find("\nVolume:");
            if (vp == std::string::npos) vp = b.find("Volume:");
            if (vp != std::string::npos) {
                auto e = b.find('\n', vp + 1);
                r.pct  = parse_pct_line(b.substr(vp, e - vp));
            }
            into.push_back(std::move(r));
            p = n == std::string::npos ? blob.size() : n;
        }
    }

    void parse(const std::string& out) {
        std::string def_sink, def_src;
        auto p1 = out.find("===");
        if (p1 != std::string::npos) {
            def_sink = trim(out.substr(0, p1));
            auto p2  = out.find("===", p1 + 3);
            if (p2 != std::string::npos)
                def_src = trim(out.substr(p1 + 3, p2 - (p1 + 3)));
        }
        std::vector<ARow> sinks, apps, sources;
        auto sinks_blob = out;
        auto in_at      = out.find("===IN===");
        auto src_at     = out.find("===SRC===");
        if (in_at != std::string::npos)
            sinks_blob = out.substr(0, in_at);
        parse_devices(sinks_blob, "Sink", def_sink, false, sinks);
        if (in_at != std::string::npos)
            parse_apps(out.substr(in_at, src_at == std::string::npos
                                             ? std::string::npos
                                             : src_at - in_at),
                       apps);
        if (src_at != std::string::npos)
            parse_devices(out.substr(src_at), "Source", def_src, true, sources);

        rows_.clear();
        ARow h;
        h.kind  = ARow::Header;
        h.label = "OUTPUT";
        rows_.push_back(h);
        for (auto& s : sinks) {
            if (s.is_def) {
                ARow sl = s;
                sl.kind = ARow::Slider;
                sl.label = s.label;
                rows_.push_back(sl);
            }
        }
        for (auto& s : sinks) rows_.push_back(s);
        h.label = "INPUT";
        rows_.push_back(h);
        for (auto& s : sources) {
            if (s.is_def) {
                ARow sl = s;
                sl.kind = ARow::Slider;
                sl.label = s.label;
                rows_.push_back(sl);
            }
        }
        for (auto& s : sources) rows_.push_back(s);
        if (cfg.shell_audio_show_apps && !apps.empty()) {
            h.label = "APPLICATIONS";
            rows_.push_back(h);
            for (auto& a : apps) rows_.push_back(a);
        }
        sel_ = ov::clamp_sel(sel_, (int)rows_.size());
    }

    void set_vol(const ARow& r, int pct) {
        pct = std::clamp(pct, 0, 150);
        std::string cmd;
        if (r.kind == ARow::App)
            cmd = "pactl set-sink-input-volume " + r.id + " " +
                  std::to_string(pct) + "%";
        else if (r.is_src)
            cmd = "pactl set-source-volume " + shell_quote(r.id) + " " +
                  std::to_string(pct) + "%";
        else
            cmd = "pactl set-sink-volume " + shell_quote(r.id) + " " +
                  std::to_string(pct) + "%";
        spawn_detached(cmd);
    }
    void toggle_mute(const ARow& r) {
        std::string cmd;
        if (r.kind == ARow::App)
            cmd = "pactl set-sink-input-mute " + r.id + " toggle";
        else if (r.is_src)
            cmd = "pactl set-source-mute " + shell_quote(r.id) + " toggle";
        else
            cmd = "pactl set-sink-mute " + shell_quote(r.id) + " toggle";
        spawn_detached(cmd);
    }
    void set_default(const ARow& r) {
        if (r.is_src)
            spawn_detached("pactl set-default-source " + shell_quote(r.id));
        else {
            spawn_detached("pactl set-default-sink " + shell_quote(r.id) +
                           "; pactl list short sink-inputs 2>/dev/null | "
                           "awk '{print $1}' | while read i; do "
                           "pactl move-sink-input \"$i\" " +
                           shell_quote(r.id) + "; done");
        }
    }

    void apply_slider(int row, bool commit) {
        if (row < 0 || row >= (int)rows_.size()) return;
        ARow& r = rows_[row];
        if (r.kind != ARow::Slider && r.kind != ARow::App &&
            r.kind != ARow::Device)
            return;
        if (commit) set_vol(r, r.pct);
    }

    void on_drag(double x) {
        if (drag_ < 0 || drag_ >= (int)hits_.size()) return;
        const Hit& h = hits_[drag_];
        if (h.kind != 1 || h.row < 0 || h.row >= (int)rows_.size()) return;
        double f = std::clamp((x - h.x) / std::max(1.0, h.w), 0.0, 1.0);
        rows_[h.row].pct = (int)std::lround(f * 100);
        host_.redraw();
    }

    void on_click(double x, double y, int btn) {
        int hi = hit_at(hits_, x, y);
        if (hi < 0) return;
        const Hit& h = hits_[hi];
        if (h.kind == 4) {
            for (auto& r : rows_) {
                if (r.kind == ARow::Slider && !r.is_src) {
                    toggle_mute(r);
                    return;
                }
            }
            return;
        }
        if (h.row < 0 || h.row >= (int)rows_.size()) return;
        sel_ = h.row;
        ARow& r = rows_[h.row];
        if (btn == BTN_RIGHT) {
            if (r.kind != ARow::Header) toggle_mute(r);
            return;
        }
        if (btn != BTN_LEFT) return;
        if (h.kind == 1) {
            drag_ = hi;
            on_drag(x);
            return;
        }
        if (h.kind == 2) {
            toggle_mute(r);
            return;
        }
        if (r.kind == ARow::Device) set_default(r);
    }

    void on_key(const Bar::KeyEvent& e) {
        if (!e.pressed) return;
        if (e.escape()) {
            close();
            return;
        }
        if (e.up()) {
            if (sel_ > 0) --sel_;
            host_.redraw();
            return;
        }
        if (e.down()) {
            if (sel_ + 1 < (int)rows_.size()) ++sel_;
            host_.redraw();
            return;
        }
        if (sel_ < 0 || sel_ >= (int)rows_.size()) return;
        ARow& r = rows_[sel_];
        int step = cfg.shell_audio_step;
        if (e.left() && r.kind != ARow::Header) {
            r.pct = std::max(0, r.pct - step);
            set_vol(r, r.pct);
            host_.redraw();
            return;
        }
        if (e.right() && r.kind != ARow::Header) {
            r.pct = std::min(150, r.pct + step);
            set_vol(r, r.pct);
            host_.redraw();
            return;
        }
        if (e.enter()) {
            if (r.kind == ARow::Device) set_default(r);
            else if (r.kind != ARow::Header) toggle_mute(r);
        }
    }

    void paint(cairo_t* cr) {
        panel_bg(cr, W(), H());
        select_shell_font(cr, fs());
        hits_.clear();
        const int hh = ov::hdr_h(fs()), rh = ov::row_h(fs());
        int def_pct = 0;
        bool def_muted = false;
        std::string def_name = "Audio";
        for (auto& r : rows_) {
            if (r.kind == ARow::Slider && !r.is_src) {
                def_pct = r.pct;
                def_muted = r.muted;
                def_name = r.label;
                break;
            }
        }
        auto volume_name = [&]() {
            if (def_muted) return std::string("MUTED");
            int p = def_pct;
            if (p == 0) return std::string("SILENCED");
            if (p >= 100) return std::string("CONCERT HALL");
            if (p >= 85) return std::string("PARTY MODE");
            if (p >= 70) return std::string("CRANKED UP");
            if (p >= 50) return std::string("STEADY GROOVE");
            if (p >= 30) return std::string("EASY LISTENING");
            if (p >= 15) return std::string("MURMUR");
            return std::string("WHISPER");
        };
        say(cr, 18, hh / 2.0 - 6, loading_ ? "Audio\u2026" : utf8_trunc(def_name, 22),
            cfg.c_fg);
        cairo_set_font_size(cr, std::max(10.0, fs() - 3));
        say(cr, 18, hh / 2.0 + 10, volume_name(), cfg.c_dim);
        cairo_set_font_size(cr, fs());
        std::string tog = def_muted ? "Unmute" : "Mute";
        double twid = tw(cr, tog) + 16;
        col(cr, def_muted ? cfg.c_ws_bg : cfg.c_accent, 1);
        rrect(cr, W() - 18 - twid, (hh - 22) / 2.0, twid, 22, 6);
        cairo_fill(cr);
        say(cr, W() - 18 - twid + 8, hh / 2.0, tog,
            def_muted ? cfg.c_dim : contrast_on(cfg.c_accent));
        hits_.push_back({W() - 18.0 - twid, 8, twid, 24, -1, 4});
        int y0 = hh;
        int vis = std::max(1, (H() - y0 - 8) / rh);
        ov::keep_visible(sel_, scroll_, vis, (int)rows_.size());
        for (int i = 0; i < vis; ++i) {
            int idx = scroll_ + i;
            if (idx >= (int)rows_.size()) break;
            double y = y0 + i * rh, mid = y + rh / 2.0;
            const ARow& r = rows_[idx];
            if (idx == sel_ && r.kind != ARow::Header) {
                col(cr, cfg.c_accent, 0.28);
                rrect(cr, 10, y + 2, W() - 20, rh - 4, 6);
                cairo_fill(cr);
            }
            if (r.kind == ARow::Header) {
                say(cr, 18, mid, r.label, cfg.c_accent);
                continue;
            }
            Color fg = idx == sel_ ? contrast_on(cfg.c_accent) : cfg.c_fg;
            if (r.kind == ARow::Device && r.is_def) fg = cfg.c_accent;
            std::string mark = r.kind == ARow::Device && r.is_def ? "* " : "";
            std::string lab  = mark + utf8_trunc(r.label, 28);
            if (r.muted) lab += "  mute";
            say(cr, 18, mid - (r.kind == ARow::Slider || r.kind == ARow::App ? 6 : 0),
                lab, r.muted ? cfg.c_dim : fg);
            double sx = 18, sw = W() - 90, sh = 8;
            double sy = mid + (r.kind == ARow::Slider || r.kind == ARow::App ? 8
                                                                            : 0);
            bool show_sl = r.kind == ARow::Slider || r.kind == ARow::App ||
                           (r.kind == ARow::Device && r.is_def);
            if (show_sl) {
                draw_slider(cr, sx, sy - sh / 2, sw, sh, r.pct / 100.0);
                hits_.push_back({sx, y + 4, sw, (double)rh - 8, idx, 1});
                if (cfg.shell_audio_show_pct) {
                    char buf[8];
                    snprintf(buf, sizeof buf, "%d", r.pct);
                    say(cr, W() - 36, mid, buf, cfg.c_dim);
                }
            } else {
                hits_.push_back({10, y, W() - 20.0, (double)rh, idx, 0});
            }
            hits_.push_back({W() - 48.0, y, 38, (double)rh, idx, 2});
        }
    }
};

// Bluetooth — BlueZ over the system bus, with a pairing agent so new
// devices can actually complete JustWorks/SSP. bluetoothctl without an
// agent is why clicks looked dead: pair hung 20s and then || true.
// ---------------------------------------------------------------------------
struct BtDev {
    std::string path, addr, name;
    bool connected = false, paired = false, trusted = false;
    int  battery   = -1;
    enum Sect { Connected, Paired, Available } sect = Available;
};

static int bt_agent_ok(sd_bus_message* m, void*, sd_bus_error*) {
    return sd_bus_reply_method_return(m, nullptr);
}
static int bt_agent_pin(sd_bus_message* m, void*, sd_bus_error*) {
    return sd_bus_reply_method_return(m, "s", "0000");
}
static int bt_agent_pass(sd_bus_message* m, void*, sd_bus_error*) {
    return sd_bus_reply_method_return(m, "u", (uint32_t)0);
}
static const sd_bus_vtable bt_agent_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("Release", "", "", bt_agent_ok, 0),
    SD_BUS_METHOD("RequestPinCode", "o", "s", bt_agent_pin, 0),
    SD_BUS_METHOD("RequestPasskey", "o", "u", bt_agent_pass, 0),
    SD_BUS_METHOD("DisplayPinCode", "os", "", bt_agent_ok, 0),
    SD_BUS_METHOD("DisplayPasskey", "ouq", "", bt_agent_ok, 0),
    SD_BUS_METHOD("RequestConfirmation", "ou", "", bt_agent_ok, 0),
    SD_BUS_METHOD("RequestAuthorization", "o", "", bt_agent_ok, 0),
    SD_BUS_METHOD("AuthorizeService", "os", "", bt_agent_ok, 0),
    SD_BUS_METHOD("Cancel", "", "", bt_agent_ok, 0),
    SD_BUS_VTABLE_END};

static bool bt_human_name(const std::string& n) {
    if (n.empty()) return false;
    // UUID-like or raw MAC: Omarchy hides these until they advertise a
    // real alias (otherwise the panel fills with scanner junk).
    auto ishex = [](char c) {
        return std::isxdigit(static_cast<unsigned char>(c));
    };
    std::string t;
    for (char c : n)
        if (c != ':' && c != '-' && c != ' ') t += (char)std::tolower((unsigned char)c);
    if (t.size() == 12) {
        bool mac = true;
        for (char c : t) if (!ishex(c)) mac = false;
        if (mac) return false;
    }
    if (t.size() == 32 || t.size() == 36) {
        bool uuid = true;
        for (char c : t) if (c != '-' && !ishex(c)) uuid = false;
        if (uuid) return false;
    }
    return true;
}

class BluetoothOverlay : public Overlay {
public:
    const char* id() const override { return "omarchy.bluetooth"; }
    void summon(const std::string&) override { open(); }
    void hide() override { close(); }
    bool is_open() const override { return host_.is_open(); }
    ~BluetoothOverlay() override { close(); }

private:
    Host host_;
    SdPump pump_;
    sd_bus_slot* agent_slot_ = nullptr;
    std::string adapter_;
    std::map<std::string, BtDev> by_path_;
    std::vector<BtDev> devs_;
    std::vector<Hit> hits_;
    int sel_ = 0, scroll_ = 0;
    bool powered_ = false, discovering_ = false;
    std::string pending_path_, pending_act_;

    int W() const { return cfg.shell_panel_width; }
    int H() const { return cfg.shell_panel_height; }
    double fs() const { return cfg.shell_bluetooth_font_size; }

    void close() {
        stop_bus();
        host_.close();
    }

    void open() {
        sel_ = scroll_ = 0;
        host_.win.paint = [this](cairo_t* cr) { paint(cr); };
        host_.win.click = [this](double x, double y, int b) { on_click(x, y, b); };
        host_.win.pscroll = [this](int d) {
            sel_ = ov::clamp_sel(sel_ + d, (int)devs_.size());
            host_.redraw();
        };
        host_.win.pkey = [this](const Bar::KeyEvent& e) { on_key(e); };
        host_.open(W(), H(), "mattbar-bluetooth", id(), true, Host::Place::BarEnd);
        start_bus();
    }

    void rebuild() {
        if (powered_ && !discovering_) start_discovery();
        devs_.clear();
        for (auto& [p, d] : by_path_) {
            if (p.find("/dev_") == std::string::npos && adapter_.empty())
                continue;
            if (!bt_human_name(d.name) && !d.paired && !d.connected)
                continue;
            BtDev x = d;
            x.sect = x.connected ? BtDev::Connected
                   : (x.paired || x.trusted) ? BtDev::Paired
                                             : BtDev::Available;
            if (x.sect == BtDev::Available && !discovering_) continue;
            devs_.push_back(std::move(x));
        }
        if (!adapter_.empty()) {
            // Powered is stored on the adapter path, not a device.
        }
        std::sort(devs_.begin(), devs_.end(), [](const BtDev& a, const BtDev& b) {
            if (a.connected != b.connected) return a.connected;
            if ((a.paired || a.trusted) != (b.paired || b.trusted))
                return a.paired || a.trusted;
            return a.name < b.name;
        });
        sel_ = ov::clamp_sel(sel_, (int)devs_.size());
        host_.redraw();
    }

    void read_props(sd_bus_message* m, const std::string& path,
                    const std::string& iface) {
        if (sd_bus_message_enter_container(m, 'a', "{sv}") < 0) return;
        bool ad = iface == "org.bluez.Adapter1";
        bool dv = iface == "org.bluez.Device1";
        bool bt = iface == "org.bluez.Battery1";
        if (ad) adapter_ = path;
        if (dv) by_path_[path].path = path;
        while (sd_bus_message_enter_container(m, 'e', "sv") > 0) {
            const char* key = nullptr;
            sd_bus_message_read(m, "s", &key);
            const char* contents = nullptr;
            char t = 0;
            sd_bus_message_peek_type(m, &t, &contents);
            std::string k = key ? key : "";
            bool used = false;
            auto take_b = [&](int* out) {
                int b = 0;
                sd_bus_message_enter_container(m, 'v', "b");
                sd_bus_message_read(m, "b", &b);
                sd_bus_message_exit_container(m);
                *out = b;
                used = true;
            };
            auto take_s = [&](std::string* out) {
                const char* v = nullptr;
                sd_bus_message_enter_container(m, 'v', "s");
                sd_bus_message_read(m, "s", &v);
                sd_bus_message_exit_container(m);
                if (v) *out = v;
                used = true;
            };
            if (ad && k == "Powered" && contents && !strcmp(contents, "b")) {
                int b = 0; take_b(&b); powered_ = b;
            } else if (ad && k == "Discovering" && contents && !strcmp(contents, "b")) {
                int b = 0; take_b(&b); discovering_ = b;
            } else if (dv && k == "Address" && contents && !strcmp(contents, "s")) {
                take_s(&by_path_[path].addr);
            } else if (dv && (k == "Alias" || k == "Name") && contents &&
                       !strcmp(contents, "s")) {
                std::string n;
                take_s(&n);
                if (k == "Alias" || by_path_[path].name.empty())
                    by_path_[path].name = n;
            } else if (dv && k == "Connected" && contents && !strcmp(contents, "b")) {
                int b = 0; take_b(&b); by_path_[path].connected = b;
            } else if (dv && k == "Paired" && contents && !strcmp(contents, "b")) {
                int b = 0; take_b(&b); by_path_[path].paired = b;
            } else if (dv && k == "Trusted" && contents && !strcmp(contents, "b")) {
                int b = 0; take_b(&b); by_path_[path].trusted = b;
            } else if (bt && k == "Percentage" && contents && !strcmp(contents, "y")) {
                uint8_t p = 0;
                sd_bus_message_enter_container(m, 'v', "y");
                sd_bus_message_read(m, "y", &p);
                sd_bus_message_exit_container(m);
                by_path_[path].battery = p;
                used = true;
            }
            if (!used) sd_bus_message_skip(m, "v");
            sd_bus_message_exit_container(m);
        }
        sd_bus_message_exit_container(m);
    }

    void read_ifaces(sd_bus_message* m, const std::string& path) {
        if (sd_bus_message_enter_container(m, 'a', "{sa{sv}}") < 0) return;
        while (sd_bus_message_enter_container(m, 'e', "sa{sv}") > 0) {
            const char* iface = nullptr;
            sd_bus_message_read(m, "s", &iface);
            read_props(m, path, iface ? iface : "");
            sd_bus_message_exit_container(m);
        }
        sd_bus_message_exit_container(m);
    }

    static int on_managed(sd_bus_message* m, void* ud, sd_bus_error*) {
        auto* self = static_cast<BluetoothOverlay*>(ud);
        if (sd_bus_message_is_method_error(m, nullptr)) {
            self->rebuild();
            return 0;
        }
        if (sd_bus_message_enter_container(m, 'a', "{oa{sa{sv}}}") < 0)
            return 0;
        while (sd_bus_message_enter_container(m, 'e', "oa{sa{sv}}") > 0) {
            const char* path = nullptr;
            sd_bus_message_read(m, "o", &path);
            self->read_ifaces(m, path ? path : "");
            sd_bus_message_exit_container(m);
        }
        sd_bus_message_exit_container(m);
        self->start_discovery();
        self->rebuild();
        return 0;
    }
    static int on_added(sd_bus_message* m, void* ud, sd_bus_error*) {
        auto* self = static_cast<BluetoothOverlay*>(ud);
        const char* path = nullptr;
        if (sd_bus_message_read(m, "o", &path) < 0) return 0;
        self->read_ifaces(m, path ? path : "");
        self->rebuild();
        return 0;
    }
    static int on_removed(sd_bus_message* m, void* ud, sd_bus_error*) {
        auto* self = static_cast<BluetoothOverlay*>(ud);
        const char* path = nullptr;
        if (sd_bus_message_read(m, "o", &path) < 0 || !path) return 0;
        if (sd_bus_message_enter_container(m, 'a', "s") < 0) return 0;
        const char* iface = nullptr;
        while (sd_bus_message_read(m, "s", &iface) > 0) {
            std::string i = iface ? iface : "";
            if (i == "org.bluez.Device1") self->by_path_.erase(path);
            else if (i == "org.bluez.Adapter1" && self->adapter_ == path)
                self->adapter_.clear();
        }
        sd_bus_message_exit_container(m);
        self->rebuild();
        return 0;
    }
    static int on_props(sd_bus_message* m, void* ud, sd_bus_error*) {
        auto* self = static_cast<BluetoothOverlay*>(ud);
        const char* iface = nullptr;
        const char* path = sd_bus_message_get_path(m);
        if (sd_bus_message_read(m, "s", &iface) < 0 || !path) return 0;
        self->read_props(m, path, iface ? iface : "");
        sd_bus_message_skip(m, "as");
        self->rebuild();
        return 0;
    }
    static int on_done(sd_bus_message* m, void* ud, sd_bus_error*) {
        auto* self = static_cast<BluetoothOverlay*>(ud);
        bool err = sd_bus_message_is_method_error(m, nullptr);
        if (err) {
            const sd_bus_error* e = sd_bus_message_get_error(m);
            fprintf(stderr, "mattbar: bluetooth: %s failed (%s)\n",
                    self->pending_act_.c_str(),
                    e && e->name ? e->name : "?");
        }
        std::string act = self->pending_act_;
        std::string path = self->pending_path_;
        self->pending_act_.clear();
        self->pending_path_.clear();
        if (!err && act == "pair" && path.size() && self->pump_.bus) {
            self->pending_act_ = "connect";
            self->pending_path_ = path;
            sd_bus_call_method_async(self->pump_.bus, nullptr, "org.bluez",
                                     path.c_str(), "org.bluez.Device1",
                                     "Connect", on_done, self, nullptr);
            self->pump_.process();
        }
        self->rebuild();
        return 0;
    }

    void start_discovery() {
        if (!pump_.bus || adapter_.empty() || !powered_) return;
        sd_bus_call_method_async(pump_.bus, nullptr, "org.bluez",
                                 adapter_.c_str(), "org.bluez.Adapter1",
                                 "StartDiscovery", nullptr, nullptr, nullptr);
        discovering_ = true;
    }
    void stop_discovery() {
        if (!pump_.bus || adapter_.empty()) return;
        sd_bus_call_method_async(pump_.bus, nullptr, "org.bluez",
                                 adapter_.c_str(), "org.bluez.Adapter1",
                                 "StopDiscovery", nullptr, nullptr, nullptr);
        discovering_ = false;
    }

    void start_bus() {
        auto* sh = mattbar_shell();
        if (!sh || !sh->bar()) return;
        stop_bus();
        sd_bus* b = nullptr;
        if (sd_bus_open_system(&b) < 0) return;
        sd_bus_set_method_call_timeout(b, 30 * 1000 * 1000ULL);
        sd_bus_add_object_vtable(b, &agent_slot_, "/org/bluez/mattbar/agent",
                                 "org.bluez.Agent1", bt_agent_vtable, this);
        sd_bus_call_method_async(b, nullptr, "org.bluez", "/org/bluez",
                                 "org.bluez.AgentManager1", "RegisterAgent",
                                 nullptr, nullptr, "os",
                                 "/org/bluez/mattbar/agent", "NoInputNoOutput");
        sd_bus_call_method_async(b, nullptr, "org.bluez", "/org/bluez",
                                 "org.bluez.AgentManager1", "RequestDefaultAgent",
                                 nullptr, nullptr, "o",
                                 "/org/bluez/mattbar/agent");
        sd_bus_match_signal(b, nullptr, "org.bluez", "/",
                            "org.freedesktop.DBus.ObjectManager",
                            "InterfacesAdded", on_added, this);
        sd_bus_match_signal(b, nullptr, "org.bluez", "/",
                            "org.freedesktop.DBus.ObjectManager",
                            "InterfacesRemoved", on_removed, this);
        sd_bus_match_signal(b, nullptr, "org.bluez", nullptr,
                            "org.freedesktop.DBus.Properties",
                            "PropertiesChanged", on_props, this);
        sd_bus_call_method_async(b, nullptr, "org.bluez", "/",
                                 "org.freedesktop.DBus.ObjectManager",
                                 "GetManagedObjects", on_managed, this,
                                 nullptr);
        pump_.attach(*sh->bar(), b, "bluez-panel");
        pump_.process();
    }

    void stop_bus() {
        stop_discovery();
        if (pump_.bus) {
            sd_bus_call_method(pump_.bus, "org.bluez", "/org/bluez",
                               "org.bluez.AgentManager1", "UnregisterAgent",
                               nullptr, nullptr, "o",
                               "/org/bluez/mattbar/agent");
        }
        if (agent_slot_) {
            sd_bus_slot_unref(agent_slot_);
            agent_slot_ = nullptr;
        }
        if (pump_.bus) pump_.teardown("panel closed");
        by_path_.clear();
        adapter_.clear();
        discovering_ = false;
        pending_act_.clear();
        pending_path_.clear();
    }

    void activate() {
        if (sel_ < 0 || sel_ >= (int)devs_.size() || !pump_.bus) return;
        const BtDev& d = devs_[sel_];
        if (d.path.empty()) return;
        if (d.connected) {
            pending_act_ = "disconnect";
            pending_path_ = d.path;
            sd_bus_call_method_async(pump_.bus, nullptr, "org.bluez",
                                     d.path.c_str(), "org.bluez.Device1",
                                     "Disconnect", on_done, this, nullptr);
        } else if (d.paired || d.trusted) {
            pending_act_ = "connect";
            pending_path_ = d.path;
            sd_bus_call_method_async(pump_.bus, nullptr, "org.bluez",
                                     d.path.c_str(), "org.bluez.Device1",
                                     "Connect", on_done, this, nullptr);
        } else {
            pending_act_ = "pair";
            pending_path_ = d.path;
            sd_bus_call_method_async(pump_.bus, nullptr, "org.bluez",
                                     d.path.c_str(), "org.bluez.Device1",
                                     "Pair", on_done, this, nullptr);
        }
        pump_.process();
        host_.redraw();
    }
    void forget() {
        if (sel_ < 0 || sel_ >= (int)devs_.size() || !pump_.bus) return;
        const BtDev& d = devs_[sel_];
        if (d.path.empty() || adapter_.empty()) return;
        pending_act_ = "forget";
        pending_path_ = d.path;
        sd_bus_call_method_async(pump_.bus, nullptr, "org.bluez",
                                 adapter_.c_str(), "org.bluez.Adapter1",
                                 "RemoveDevice", on_done, this, "o",
                                 d.path.c_str());
        pump_.process();
        host_.redraw();
    }
    void toggle_power() {
        if (!pump_.bus || adapter_.empty()) {
            spawn_detached("omarchy-bluetooth-power toggle");
            return;
        }
        sd_bus_message* m = nullptr;
        if (sd_bus_message_new_method_call(pump_.bus, &m, "org.bluez",
                                           adapter_.c_str(),
                                           "org.freedesktop.DBus.Properties",
                                           "Set") >= 0) {
            sd_bus_message_append(m, "ss", "org.bluez.Adapter1", "Powered");
            sd_bus_message_open_container(m, 'v', "b");
            sd_bus_message_append(m, "b", powered_ ? 0 : 1);
            sd_bus_message_close_container(m);
            sd_bus_call_async(pump_.bus, nullptr, m, nullptr, nullptr, 0);
            sd_bus_message_unref(m);
            pump_.process();
        }
    }

    void on_click(double x, double y, int btn) {
        int hi = hit_at(hits_, x, y);
        if (hi < 0) return;
        const Hit& h = hits_[hi];
        if (h.kind == 4) {
            toggle_power();
            return;
        }
        if (h.row < 0 || h.row >= (int)devs_.size()) return;
        sel_ = h.row;
        if (btn == BTN_RIGHT) forget();
        else if (btn == BTN_LEFT) activate();
        else host_.redraw();
    }

    void on_key(const Bar::KeyEvent& e) {
        if (!e.pressed) return;
        if (e.escape()) {
            close();
            return;
        }
        if (e.up()) {
            if (sel_ > 0) --sel_;
            host_.redraw();
            return;
        }
        if (e.down()) {
            if (sel_ + 1 < (int)devs_.size()) ++sel_;
            host_.redraw();
            return;
        }
        if (e.enter()) activate();
        if (e.keysym == 0xffff) forget();
    }

    void paint(cairo_t* cr) {
        panel_bg(cr, W(), H());
        select_shell_font(cr, fs());
        hits_.clear();
        const int hh = ov::hdr_h(fs()), rh = ov::row_h(fs());
        say(cr, 18, hh / 2.0 - 6, "Bluetooth", cfg.c_fg);
        {
            const char* phrase = "No adapter";
            if (adapter_.empty()) phrase = "No adapter";
            else if (!powered_) phrase = "Turned Off";
            else {
                static const char* ps[] = {
                    "Untangling wires", "Streaming vikings", "Pairing mysteries",
                    "Herding headsets", "Taming radios", "Summoning speakers",
                    "Wrangling codecs", "Polishing packets"};
                phrase = ps[(time(nullptr) / 3) % 8];
            }
            std::string meta = phrase;
            std::transform(meta.begin(), meta.end(), meta.begin(),
                           [](unsigned char c) { return (char)std::toupper(c); });
            cairo_set_font_size(cr, std::max(10.0, fs() - 3));
            const char* sub = discovering_ ? "SCANNING\u2026" : meta.c_str();
            if (!pending_act_.empty()) {
                if (pending_act_ == "pair") sub = "PAIRING\u2026";
                else if (pending_act_ == "connect") sub = "CONNECTING\u2026";
                else if (pending_act_ == "disconnect") sub = "DISCONNECTING\u2026";
                else if (pending_act_ == "forget") sub = "FORGETTING\u2026";
            }
            say(cr, 18, hh / 2.0 + 10, sub, cfg.c_dim);
            cairo_set_font_size(cr, fs());
        }
        std::string tog = powered_ ? "On" : "Off";
        double twid = tw(cr, tog) + 16;
        col(cr, powered_ ? cfg.c_accent : cfg.c_ws_bg, 1);
        rrect(cr, W() - 18 - twid, (hh - 22) / 2.0, twid, 22, 6);
        cairo_fill(cr);
        say(cr, W() - 18 - twid + 8, hh / 2.0, tog,
            powered_ ? contrast_on(cfg.c_accent) : cfg.c_dim);
        hits_.push_back({W() - 18.0 - twid, 8, twid, 24, -1, 4});

        int y0 = hh + 4;
        int vis = std::max(1, (H() - y0 - 8) / rh);
        ov::keep_visible(sel_, scroll_, vis, (int)devs_.size());
        if (devs_.empty())
            say(cr, 18, y0 + rh / 2.0,
                powered_ ? (discovering_ ? "Scanning for devices\u2026"
                                         : "No devices")
                         : "Turn Bluetooth on",
                cfg.c_dim);
        int last_sect = -1;
        for (int i = 0; i < vis; ++i) {
            int idx = scroll_ + i;
            if (idx >= (int)devs_.size()) break;
            const BtDev& d = devs_[idx];
            if ((int)d.sect != last_sect) {
                last_sect = d.sect;
                const char* shdr = d.sect == BtDev::Connected ? "CONNECTED"
                                 : d.sect == BtDev::Paired    ? "PAIRED"
                                                              : "AVAILABLE";
                cairo_set_font_size(cr, std::max(10.0, fs() - 2));
                say(cr, 18, y0 + 10, shdr, cfg.c_accent);
                cairo_set_font_size(cr, fs());
                y0 += 22;
            }
            double y = y0, mid = y + rh / 2.0;
            if (idx == sel_) {
                col(cr, cfg.c_accent, 0.28);
                rrect(cr, 10, y + 2, W() - 20, rh - 4, 6);
                cairo_fill(cr);
            }
            Color fg = idx == sel_ ? contrast_on(cfg.c_accent) : cfg.c_fg;
            say(cr, 18, mid, utf8_trunc(d.name.empty() ? d.addr : d.name, 24),
                d.connected ? cfg.c_accent : fg);
            std::string st;
            bool pend = !pending_path_.empty() && pending_path_ == d.path;
            if (pend) st = pending_act_;
            else if (d.battery >= 0) st = std::to_string(d.battery) + "%";
            else if (d.connected) st = "connected";
            else if (d.paired) st = "paired";
            else st = "new";
            say(cr, W() - 18 - tw(cr, st), mid, st, cfg.c_dim);
            hits_.push_back({10, y, W() - 20.0, (double)rh, idx, 0});
            y0 += rh;
        }
    }
};

// ---------------------------------------------------------------------------
// Display (omarchy.monitor)
// ---------------------------------------------------------------------------
struct Mon {
    std::string name;
    bool enabled = true, focused = false;
    int w = 0, h = 0;
};

class DisplayOverlay : public Overlay {
public:
    const char* id() const override { return "omarchy.monitor"; }
    void summon(const std::string&) override { open(); }
    void hide() override { close(); }
    bool is_open() const override { return host_.is_open(); }
    ~DisplayOverlay() override { close(); }

private:
    Host host_;
    AsyncCmd cmd_;
    std::vector<Mon> mons_;
    std::vector<Hit> hits_;
    std::string scale_, focused_, intern_, externm_;
    bool intern_on_ = false, mirrored_ = false;
    int bright_ = -1; // -1 = no backlight
    int text_px_ = 12;
    int sel_ = 0, scroll_ = 0, drag_ = -1;
    bool loading_ = false;

    int W() const { return cfg.shell_panel_width; }
    int H() const { return cfg.shell_panel_height; }
    double fs() const { return cfg.shell_display_font_size; }

    void close() {
        drag_ = -1;
        host_.close();
    }

    static int sys_brightness() {
        DIR* d = opendir("/sys/class/backlight");
        if (!d) return -1;
        std::string dev;
        while (dirent* e = readdir(d)) {
            if (e->d_name[0] == '.') continue;
            dev = e->d_name;
            break;
        }
        closedir(d);
        if (dev.empty()) return -1;
        std::string base = "/sys/class/backlight/" + dev;
        int cur = atoi(slurp(base + "/brightness").c_str());
        int max = atoi(slurp(base + "/max_brightness").c_str());
        if (max <= 0) return -1;
        return (int)std::lround(cur * 100.0 / max);
    }

    void open() {
        sel_ = scroll_ = 0;
        bright_ = sys_brightness();
        host_.win.paint   = [this](cairo_t* cr) { paint(cr); };
        host_.win.click   = [this](double x, double y, int b) { on_click(x, y, b); };
        host_.win.pmotion = [this](double x, double) { on_drag(x); };
        host_.win.prelease = [this](int) {
            if (drag_ >= 0) {
                const Hit& h = hits_[drag_];
                if (h.kind == 1) commit_bright();
                else if (h.kind == 5) commit_text();
            }
            drag_ = -1;
        };
        host_.win.pscroll = [this](int d) {
            if (bright_ >= 0 && sel_ == 0) {
                bright_ = std::clamp(bright_ - d * cfg.brightness_step, 0, 100);
                commit_bright();
                host_.redraw();
                return;
            }
            sel_ = ov::clamp_sel(sel_ + d, 8);
            host_.redraw();
        };
        host_.win.pkey = [this](const Bar::KeyEvent& e) { on_key(e); };
        host_.open(W(), H(), "mattbar-display", id(), true, Host::Place::BarEnd);
        reload();
    }

    void reload() {
        auto* sh = mattbar_shell();
        if (!sh || !sh->bar()) return;
        loading_ = true;
        cmd_.run(*sh->bar(),
                 "omarchy-monitor-state 2>/dev/null; echo '===TEXT==='; "
                 "omarchy-display-text-size 2>/dev/null",
                 [this](const std::string& out, int) {
                     parse(out);
                     loading_ = false;
                     host_.redraw();
                 },
                 2500);
    }

    void parse(const std::string& out) {
        intern_.clear();
        externm_.clear();
        intern_on_ = mirrored_ = false;
        {
            std::istringstream hs(out);
            std::string ln[8];
            for (int i = 0; i < 8 && std::getline(hs, ln[i]); ++i)
                ln[i] = trim(ln[i]);
            intern_    = ln[1];
            externm_   = ln[2];
            intern_on_ = !ln[3].empty();
            mirrored_  = !ln[4].empty() && !externm_.empty() && ln[4] == externm_;
        }
        mons_.clear();
        size_t i = 0;
        while ((i = out.find("\"name\"", i)) != std::string::npos) {
            Mon m;
            m.name = ov::json_str(out.substr(i, 200), "name");
            std::string chunk = out.substr(i, 280);
            m.enabled = chunk.find("\"enabled\":false") == std::string::npos &&
                        chunk.find("\"disabled\":true") == std::string::npos;
            m.focused = chunk.find("\"focused\":true") != std::string::npos;
            auto wp = chunk.find("\"width\"");
            if (wp != std::string::npos) m.w = atoi(chunk.c_str() + wp + 8);
            auto hp = chunk.find("\"height\"");
            if (hp != std::string::npos) m.h = atoi(chunk.c_str() + hp + 9);
            if (!m.name.empty()) {
                if (m.focused) focused_ = m.name;
                mons_.push_back(m);
            }
            i += 6;
        }
        // last non-json line is scale
        std::istringstream ss(out);
        std::string line, last;
        while (std::getline(ss, line)) {
            line = trim(line);
            if (!line.empty() && line[0] != '[' && line[0] != '{' &&
                line[0] != '"')
                last = line;
        }
        if (!last.empty() && last.find_first_not_of("0123456789.") ==
                                 std::string::npos)
            scale_ = last;
        if (scale_.empty()) scale_ = "1";
        bright_ = sys_brightness();
        auto tp = out.find("text size:");
        if (tp != std::string::npos) text_px_ = atoi(out.c_str() + tp + 10);
        if (text_px_ < 9) text_px_ = 12;
    }

    void commit_bright() {
        if (bright_ < 0) return;
        spawn_detached("omarchy-brightness-display --no-osd " +
                       std::to_string(bright_) + "%");
    }
    void commit_text() {
        spawn_detached("omarchy-display-text-size " + std::to_string(text_px_));
    }
    static int nearest_text(int px) {
        static const int stops[] = {9, 10, 11, 12, 14, 16, 20};
        int best = 12, bd = 99;
        for (int s : stops) {
            int d = std::abs(s - px);
            if (d < bd) {
                bd   = d;
                best = s;
            }
        }
        return best;
    }
    static const char* brightness_name(int p) {
        if (p >= 95) return "Sun blast";
        if (p >= 80) return "Solar flare";
        if (p >= 65) return "Golden hour";
        if (p >= 45) return "Even day";
        if (p >= 30) return "Soft glow";
        if (p >= 20) return "Lamp light";
        if (p >= 10) return "Candlelit";
        return "Night owl";
    }
    void set_scale(const std::string& s) {
        scale_ = s;
        spawn_detached("omarchy-hyprland-monitor-scaling " + s);
    }
    void toggle_mon(const Mon& m) {
        int on = 0;
        for (auto& x : mons_)
            if (x.enabled) ++on;
        if (m.enabled && on <= 1) return; // never blank the session
        if (m.enabled)
            spawn_detached("hyprctl keyword monitor " + shell_quote(m.name) +
                           ",disable");
        else
            spawn_detached("hyprctl keyword monitor " + shell_quote(m.name) +
                           ",preferred,auto," + (scale_.empty() ? "1" : scale_));
        reload();
    }

    void on_drag(double x) {
        if (drag_ < 0 || drag_ >= (int)hits_.size()) return;
        const Hit& h = hits_[drag_];
        double f = std::clamp((x - h.x) / std::max(1.0, h.w), 0.0, 1.0);
        if (h.kind == 1) {
            bright_ = (int)std::lround(f * 100);
            host_.redraw();
        } else if (h.kind == 5) {
            static const int stops[] = {9, 10, 11, 12, 14, 16, 20};
            int i = std::clamp((int)std::lround(f * 6), 0, 6);
            text_px_ = stops[i];
            host_.redraw();
        }
    }

    void on_click(double x, double y, int btn) {
        if (btn != BTN_LEFT) return;
        int hi = hit_at(hits_, x, y);
        if (hi < 0) return;
        const Hit& h = hits_[hi];
        if (h.kind == 1 || h.kind == 5) {
            drag_ = hi;
            on_drag(x);
            return;
        }
        if (h.kind == 3 && h.row >= 0 && h.row < 6) {
            static const char* presets[] = {"1", "1.25", "1.6", "2", "3", "4"};
            set_scale(presets[h.row]);
            host_.redraw();
            return;
        }
        if (h.kind == 6 && !intern_.empty()) {
            if (intern_on_)
                spawn_detached("hyprctl keyword monitor " +
                               shell_quote(intern_) + ",disable");
            else
                spawn_detached("hyprctl keyword monitor " +
                               shell_quote(intern_) + ",preferred,auto,auto");
            reload();
            return;
        }
        if (h.kind == 7 && !intern_.empty() && !externm_.empty()) {
            if (mirrored_)
                spawn_detached("hyprctl keyword monitor " +
                               shell_quote(intern_) + ",preferred,auto,auto");
            else
                spawn_detached("hyprctl keyword monitor " +
                               shell_quote(intern_) + ",preferred,auto,1,mirror," +
                               shell_quote(externm_));
            reload();
            return;
        }
        if (h.kind == 0 && h.row >= 0 && h.row < (int)mons_.size()) {
            sel_ = h.row;
            toggle_mon(mons_[h.row]);
        }
    }

    void on_key(const Bar::KeyEvent& e) {
        if (!e.pressed) return;
        if (e.escape()) {
            close();
            return;
        }
        if (e.left() && bright_ >= 0) {
            bright_ = std::max(0, bright_ - cfg.brightness_step);
            commit_bright();
            host_.redraw();
            return;
        }
        if (e.right() && bright_ >= 0) {
            bright_ = std::min(100, bright_ + cfg.brightness_step);
            commit_bright();
            host_.redraw();
            return;
        }
        if (e.up() && sel_ > 0) {
            --sel_;
            host_.redraw();
        }
        if (e.down() && sel_ + 1 < (int)mons_.size()) {
            ++sel_;
            host_.redraw();
        }
        if (e.enter() && sel_ >= 0 && sel_ < (int)mons_.size())
            toggle_mon(mons_[sel_]);
    }

    void paint(cairo_t* cr) {
        panel_bg(cr, W(), H());
        select_shell_font(cr, fs());
        hits_.clear();
        const int hh = ov::hdr_h(fs()), rh = ov::row_h(fs());
        say(cr, 18, hh / 2.0 - 6, loading_ ? "Display\u2026" : "Display", cfg.c_fg);
        if (bright_ >= 0) {
            cairo_set_font_size(cr, std::max(10.0, fs() - 3));
            std::string mood = brightness_name(bright_);
            std::transform(mood.begin(), mood.end(), mood.begin(),
                           [](unsigned char c) { return (char)std::toupper(c); });
            say(cr, 18, hh / 2.0 + 10, mood, cfg.c_dim);
            cairo_set_font_size(cr, fs());
        }
        double y = hh + 8;
        if (bright_ >= 0) {
            cairo_set_font_size(cr, std::max(10.0, fs() - 2));
            say(cr, 18, y + 10, "BRIGHTNESS", cfg.c_accent);
            cairo_set_font_size(cr, fs());
            char buf[8];
            snprintf(buf, sizeof buf, "%d%%", bright_);
            say(cr, W() - 18 - tw(cr, buf), y + 10, buf, cfg.c_dim);
            y += 24;
            draw_slider(cr, 18, y, W() - 36, 10, bright_ / 100.0);
            hits_.push_back({18, y - 8, (double)W() - 36, 26, 0, 1});
            y += 28;
        }
        {
            cairo_set_font_size(cr, std::max(10.0, fs() - 2));
            say(cr, 18, y + 10, "TEXT SIZE", cfg.c_accent);
            cairo_set_font_size(cr, fs());
            std::string px = std::to_string(text_px_) + "px";
            say(cr, W() - 18 - tw(cr, px), y + 10, px, cfg.c_dim);
            y += 24;
            static const int stops[] = {9, 10, 11, 12, 14, 16, 20};
            int idx = 3;
            for (int i = 0; i < 7; ++i)
                if (stops[i] == nearest_text(text_px_)) idx = i;
            draw_slider(cr, 18, y, W() - 36, 10, idx / 6.0);
            hits_.push_back({18, y - 8, (double)W() - 36, 26, 0, 5});
            y += 32;
        }
        cairo_set_font_size(cr, std::max(10.0, fs() - 2));
        say(cr, 18, y + 10, "SCALE", cfg.c_accent);
        cairo_set_font_size(cr, fs());
        y += 28;
        static const char* presets[] = {"1", "1.25", "1.6", "2", "3", "4"};
        double x = 18;
        for (int i = 0; i < 6; ++i) {
            bool on = scale_ == presets[i];
            std::string lab = std::string(presets[i]) + "x";
            double bw = tw(cr, lab) + 16;
            col(cr, on ? cfg.c_accent : cfg.c_ws_bg, 1);
            rrect(cr, x, y, bw, 24, 6);
            cairo_fill(cr);
            say(cr, x + 8, y + 12, lab,
                on ? contrast_on(cfg.c_accent) : cfg.c_fg);
            hits_.push_back({x, y, bw, 24, i, 3});
            x += bw + 8;
        }
        y += 40;
        if (!intern_.empty() && !externm_.empty()) {
            cairo_set_font_size(cr, std::max(10.0, fs() - 2));
            say(cr, 18, y + 10, "LAPTOP", cfg.c_accent);
            cairo_set_font_size(cr, fs());
            y += 24;
            auto pill = [&](const char* lab, bool on, int kind) {
                double bw = tw(cr, lab) + 16;
                col(cr, on ? cfg.c_accent : cfg.c_ws_bg, 1);
                rrect(cr, 18, y, bw, 24, 6);
                cairo_fill(cr);
                say(cr, 26, y + 12, lab,
                    on ? contrast_on(cfg.c_accent) : cfg.c_fg);
                hits_.push_back({18, (double)y, bw, 24, 0, kind});
                return bw;
            };
            double bw = pill(intern_on_ ? "Lid on" : "Lid off", intern_on_, 6);
            (void)bw;
            double x2 = 18 + tw(cr, intern_on_ ? "Lid on" : "Lid off") + 28;
            col(cr, mirrored_ ? cfg.c_accent : cfg.c_ws_bg, 1);
            double mw = tw(cr, "Mirror") + 16;
            rrect(cr, x2, y, mw, 24, 6);
            cairo_fill(cr);
            say(cr, x2 + 8, y + 12, "Mirror",
                mirrored_ ? contrast_on(cfg.c_accent) : cfg.c_fg);
            hits_.push_back({x2, (double)y, mw, 24, 0, 7});
            y += 36;
        }
        cairo_set_font_size(cr, std::max(10.0, fs() - 2));
        say(cr, 18, y + 10, "MONITORS", cfg.c_accent);
        cairo_set_font_size(cr, fs());
        y += 24;
        for (int i = 0; i < (int)mons_.size(); ++i) {
            const Mon& m = mons_[i];
            double mid = y + rh / 2.0;
            if (i == sel_) {
                col(cr, cfg.c_accent, 0.28);
                rrect(cr, 10, y + 2, W() - 20, rh - 4, 6);
                cairo_fill(cr);
            }
            draw_checkbox(cr, 18, mid, m.enabled);
            std::string lab = m.name;
            if (m.w && m.h)
                lab += "  " + std::to_string(m.w) + "x" + std::to_string(m.h);
            if (m.focused) lab += "  focused";
            say(cr, 40, mid, lab, m.enabled ? cfg.c_fg : cfg.c_dim);
            hits_.push_back({10, y, W() - 20.0, (double)rh, i, 0});
            y += rh;
        }
        (void)scroll_;
    }
};

} // namespace

void register_shell_panels(Shell& sh) {
    sh.add(new AudioOverlay);
    sh.add(make_network_overlay());
    sh.add(new BluetoothOverlay);
    sh.add(new DisplayOverlay);
    sh.add(make_weather_overlay());
    sh.add(make_agents_overlay());
}

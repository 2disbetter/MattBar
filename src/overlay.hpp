#pragma once
// Shared chassis for MattBar Shell panels and overlays: layer surface
// (centered for menus/pickers, bar-corner for Quickshell-style panels),
// exclusive keyboard, bar-hold, and a few Cairo helpers.
#include "bar.hpp"
#include "config.hpp"
#include "modules.hpp"
#include "popup.hpp"
#include "shell.hpp"
#include "ui.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace ov {

inline void col(cairo_t* cr, const Color& c, double a = -1) {
    cairo_set_source_rgba(cr, c.r, c.g, c.b, a < 0 ? c.a : a);
}
inline double tw(cairo_t* cr, const std::string& s) {
    cairo_text_extents_t e;
    cairo_text_extents(cr, s.c_str(), &e);
    return e.x_advance;
}
inline void say(cairo_t* cr, double x, double ymid, const std::string& s,
                const Color& c) {
    cairo_font_extents_t fe;
    cairo_font_extents(cr, &fe);
    col(cr, c, 1.0);
    cairo_move_to(cr, x, ymid + (fe.ascent - fe.descent) / 2.0);
    cairo_show_text(cr, s.c_str());
}
inline void rrect(cairo_t* cr, double x, double y, double w, double h,
                  double r) {
    if (r < 0.5) {
        cairo_rectangle(cr, x, y, w, h);
        return;
    }
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -M_PI_2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI_2);
    cairo_arc(cr, x + r, y + h - r, r, M_PI_2, M_PI);
    cairo_arc(cr, x + r, y + r, r, M_PI, 1.5 * M_PI);
    cairo_close_path(cr);
}
inline void panel_bg(cairo_t* cr, int w, int h) {
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    col(cr, cfg.c_bg, std::max(cfg.c_bg.a, 0.96));
    rrect(cr, 0.5, 0.5, w - 1, h - 1, 12);
    cairo_fill_preserve(cr);
    col(cr, cfg.c_ws_bg, 1.0);
    cairo_set_line_width(cr, 1);
    cairo_stroke(cr);
}
inline void select_shell_font(cairo_t* cr, double size) {
    cairo_select_font_face(cr, cfg.font.c_str(), CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, size);
}
inline int font_px(double size) { return (int)std::lround(size); }
inline int row_h(double size) { return std::max(26, font_px(size) + 14); }
inline int hdr_h(double size) { return std::max(34, font_px(size) + 18); }
inline int search_h(double size) { return std::max(24, font_px(size) + 10); }

inline std::string shell_quote(const std::string& s) {
    std::string o = "'";
    for (char c : s) {
        if (c == '\'') o += "'\\''";
        else o += c;
    }
    o += "'";
    return o;
}

inline std::string utf8_trunc(const std::string& s, size_t maxchars) {
    size_t chars = 0, i = 0;
    while (i < s.size() && chars < maxchars) {
        unsigned char c = s[i];
        i += c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xe ? 3 : 4;
        ++chars;
    }
    if (i >= s.size()) return s;
    return s.substr(0, i) + "\u2026";
}

inline std::string json_str(const std::string& json, const char* key) {
    std::string pat = std::string("\"") + key + "\"";
    auto p = json.find(pat);
    if (p == std::string::npos) return {};
    p = json.find(':', p + pat.size());
    if (p == std::string::npos) return {};
    p = json.find_first_not_of(" \t\n\r", p + 1);
    if (p == std::string::npos) return {};
    if (json[p] == '"') {
        ++p;
        std::string out;
        while (p < json.size() && json[p] != '"') {
            if (json[p] == '\\' && p + 1 < json.size()) {
                ++p;
                char c = json[p++];
                if (c == 'n') out += '\n';
                else if (c == 't') out += '\t';
                else if (c == 'r') out += '\r';
                else out += c;
            } else out += json[p++];
        }
        return out;
    }
    auto e = json.find_first_of(",} \t\n", p);
    return json.substr(p, e - p);
}

inline bool json_truthy(const std::string& v) {
    return v == "true" || v == "1" || v == "yes";
}

// Each `{...}` object inside json[key] (an array). Strings are skipped so
// braces in values do not split objects.
inline std::vector<std::string> json_object_array(const std::string& json,
                                                  const char* key) {
    std::vector<std::string> out;
    std::string pat = std::string("\"") + key + "\"";
    auto p = json.find(pat);
    if (p == std::string::npos) return out;
    p = json.find('[', p + pat.size());
    if (p == std::string::npos) return out;
    int depth = 0;
    size_t start = std::string::npos;
    bool in_str = false, esc = false;
    for (size_t i = p + 1; i < json.size(); ++i) {
        char c = json[i];
        if (in_str) {
            if (esc) esc = false;
            else if (c == '\\') esc = true;
            else if (c == '"') in_str = false;
            continue;
        }
        if (c == '"') {
            in_str = true;
            continue;
        }
        if (c == '{') {
            if (depth == 0) start = i;
            ++depth;
        } else if (c == '}') {
            --depth;
            if (depth == 0 && start != std::string::npos) {
                out.push_back(json.substr(start, i - start + 1));
                start = std::string::npos;
            }
        } else if (c == ']' && depth == 0)
            break;
    }
    return out;
}

inline std::string parse_json_string(const std::string& s, size_t& i) {
    if (i >= s.size() || s[i] != '"') return {};
    ++i;
    std::string out;
    while (i < s.size() && s[i] != '"') {
        if (s[i] == '\\' && i + 1 < s.size()) {
            ++i;
            char c = s[i++];
            if (c == 'n') out += '\n';
            else if (c == 't') out += '\t';
            else if (c == 'r') out += '\r';
            else if (c == 'u' && i + 4 <= s.size()) {
                char hex[5] = {s[i], s[i + 1], s[i + 2], s[i + 3], 0};
                i += 4;
                unsigned cp = (unsigned)strtoul(hex, nullptr, 16);
                if (cp < 0x80) out += (char)cp;
                else if (cp < 0x800) {
                    out += (char)(0xc0 | (cp >> 6));
                    out += (char)(0x80 | (cp & 0x3f));
                } else {
                    out += (char)(0xe0 | (cp >> 12));
                    out += (char)(0x80 | ((cp >> 6) & 0x3f));
                    out += (char)(0x80 | (cp & 0x3f));
                }
            } else out += c;
        } else out += s[i++];
    }
    if (i < s.size()) ++i;
    return out;
}

// Split on spaces but keep empty fields (`open  b64` → ["open", "", "b64"]).
inline std::vector<std::string> split_keep_empty(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == ' ') {
            out.push_back(cur);
            cur.clear();
        } else cur += c;
    }
    out.push_back(cur);
    return out;
}

inline std::string b64_decode(const std::string& in) {
    auto val_of = [](unsigned char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    std::string out;
    out.reserve(in.size() * 3 / 4);
    int val = 0, valb = -8;
    for (unsigned char c : in) {
        if (c == '=' || c == '\n' || c == '\r') break;
        int d = val_of(c);
        if (d < 0) continue;
        val = (val << 6) + d;
        valb += 6;
        if (valb >= 0) {
            out.push_back(char((val >> valb) & 0xff));
            valb -= 8;
        }
    }
    return out;
}

inline void draw_slider(cairo_t* cr, double x, double y, double w, double h,
                        double frac) {
    frac = std::clamp(frac, 0.0, 1.0);
    col(cr, cfg.c_ws_bg, 1);
    rrect(cr, x, y, w, h, h / 2);
    cairo_fill(cr);
    col(cr, cfg.c_accent, 1);
    rrect(cr, x, y, std::max(h, w * frac), h, h / 2);
    cairo_fill(cr);
    cairo_arc(cr, x + w * frac, y + h / 2.0, h / 2.0 + 2, 0, 2 * M_PI);
    cairo_fill(cr);
}

inline std::string format_bytes(double n) {
    if (n < 0) n = 0;
    char b[32];
    if (n < 1024) snprintf(b, sizeof b, "%.0f B", n);
    else if (n < 1024 * 1024) snprintf(b, sizeof b, "%.1f KB", n / 1024.0);
    else if (n < 1024 * 1024 * 1024)
        snprintf(b, sizeof b, "%.1f MB", n / (1024.0 * 1024));
    else
        snprintf(b, sizeof b, "%.2f GB", n / (1024.0 * 1024 * 1024));
    return b;
}
inline std::string format_rate(double bytes_per_sec) {
    return format_bytes(bytes_per_sec) + "/s";
}
inline std::string format_ping(double ms, bool have) {
    if (!have) return "--";
    if (ms < 0) return "Timeout";
    char b[24];
    snprintf(b, sizeof b, ms > 0 && ms < 10 ? "%.1f ms" : "%.0f ms", ms);
    return b;
}
inline std::map<std::string, std::string> parse_kv(const std::string& raw) {
    std::map<std::string, std::string> m;
    std::istringstream ss(raw);
    std::string line;
    while (std::getline(ss, line)) {
        auto t = line.find('\t');
        if (t == std::string::npos) continue;
        std::string k = line.substr(0, t), v = line.substr(t + 1);
        while (!v.empty() && (v.back() == '\r' || v.back() == ' ')) v.pop_back();
        m[k] = v;
    }
    return m;
}
inline const char* wifi_icon(int strength) {
    // Same nf-md-wifi-strength runes Omarchy's network panel uses.
    static const char* icons[] = {"\U000F092F", "\U000F091F", "\U000F0922",
                                  "\U000F0925", "\U000F0928"};
    int i = (int)std::ceil(strength / 20.0) - 1;
    i = std::clamp(i, 0, 4);
    return icons[i];
}
inline double draw_pill(cairo_t* cr, double x, double y, const std::string& label,
                        bool active, bool cursor) {
    const double pad = 10, h = 24;
    double w = tw(cr, label) + pad * 2;
    if (active) col(cr, cfg.c_accent, 1);
    else if (cursor) col(cr, cfg.c_accent, 0.30);
    else col(cr, cfg.c_ws_bg, 1);
    rrect(cr, x, y, w, h, 6);
    cairo_fill(cr);
    say(cr, x + pad, y + h / 2.0, label,
        active ? contrast_on(cfg.c_accent) : cfg.c_fg);
    return w;
}

inline void draw_checkbox(cairo_t* cr, double x, double ymid, bool on) {
    const double s = 14;
    col(cr, cfg.c_ws_bg, 1);
    rrect(cr, x, ymid - s / 2, s, s, 3);
    cairo_fill(cr);
    if (on) {
        col(cr, cfg.c_accent, 1);
        rrect(cr, x + 3, ymid - s / 2 + 3, s - 6, s - 6, 2);
        cairo_fill(cr);
    }
}

static const char* k_exclusive[] = {
    "omarchy.menu",        "omarchy.audio",      "omarchy.network",
    "omarchy.bluetooth",   "omarchy.monitor",    "omarchy.clipboard",
    "omarchy.emojis",      "omarchy.image-picker", "omarchy.wifiqr",
    "omarchy.speedtest",   "omarchy.weather", "omarchy.agents",
    "omarchy.disk-speedtest", "omarchy.power", "omarchy.reminders",
    "omarchy.tailscale", "omarchy.dropbox", "omarchy.clock-timezone",
    "mattbar.plugin-add", "mattbar.plugin-remove"};

inline void close_other_overlays(const char* keep) {
    auto* sh = mattbar_shell();
    if (!sh) return;
    for (auto* id : k_exclusive) {
        if (keep && std::strcmp(id, keep) == 0) continue;
        if (sh->is_open(id)) sh->hide(id);
    }
}

struct Host {
    PopupWin    win; // the TUI card — stays a small popup
    bool        holding = false;
    int         w = 0, h = 0;
    enum class Place { Center, BarEnd };
    Place       place_ = Place::Center;
    std::string id_;

    bool is_open() const { return win.surf != nullptr; }

    void hold(bool on) {
        auto* sh = mattbar_shell();
        if (!sh || !sh->bar() || on == holding) return;
        holding = on;
        sh->bar()->hold_open(on);
    }

    void dismiss() {
        auto* sh = mattbar_shell();
        if (sh && !id_.empty()) sh->hide(id_);
        else close();
    }

    void open(int nw, int nh, const char* ns, const char* overlay_id,
              bool exclusive_kb = true, Place place = Place::Center) {
        auto* sh = mattbar_shell();
        if (!sh || !sh->bar()) return;
        close_other_overlays(overlay_id);
        Bar& bar = *sh->bar();
        w      = nw;
        h      = nh;
        place_ = place;
        id_    = overlay_id ? overlay_id : "";
        hold(true);

        // Hyprland (0.56) puts exclusive-keyboard layer surfaces on
        // m_exclusiveLSes and then *forces every pointer event onto that
        // surface*, even clicks that miss its geometry. A fullscreen
        // catcher therefore never sees those clicks — it only dimmed the
        // screen and blocked the bar. Translate the stolen out-of-bounds
        // clicks into dismiss, same as Quickshell's KeyboardPanel.
        // Callers assign win.click before open(); wrap whatever they set.
        auto inner = std::move(win.click);
        win.click  = [this, inner](double x, double y, int b) {
            int cw = win.w > 0 ? win.w : w;
            int ch = win.h > 0 ? win.h : h;
            if (x < 0 || y < 0 || x >= cw || y >= ch) {
                dismiss();
                return;
            }
            if (inner) inner(x, y, b);
        };

        win.kb_mode = exclusive_kb ? 1 : 2;
        // Bar click: the surface that received the pointer. Hotkey/IPC:
        // Hyprland's focused monitor. Last resort: the elected primary.
        wl_output* out = bar.input_output();
        if (!out) out = bar.focused_output();
        if (!out) out = bar.primary_output();
        win.out     = out;
        if (place == Place::BarEnd) {
            win.centered = false;
            auto p       = popup_place_bar_end();
            win.ensure(bar, p.anchor, p.mt, p.mr, p.mb, p.ml, ns, nw, nh, out);
        } else {
            win.centered = true;
            win.ensure(bar, 0, 0, 0, 0, 0, ns, nw, nh, out);
        }
        win.draw();
    }

    void close() {
        hold(false);
        win.destroy();
        win.click = nullptr;
    }

    void redraw() {
        if (is_open()) win.draw();
    }
};

inline int clamp_sel(int sel, int n) {
    if (n <= 0) return 0;
    return std::clamp(sel, 0, n - 1);
}

inline void keep_visible(int sel, int& scroll, int visible, int n) {
    if (visible < 1) visible = 1;
    if (sel < scroll) scroll = sel;
    if (sel >= scroll + visible) scroll = sel - visible + 1;
    if (scroll < 0) scroll = 0;
    int maxs = std::max(0, n - visible);
    if (scroll > maxs) scroll = maxs;
}

} // namespace ov

void register_shell_panels(Shell&);
void register_shell_overlays(Shell&);
Overlay* make_network_overlay();
Overlay* make_agents_overlay();
Overlay* make_timezone_overlay();
Overlay* make_power_overlay();
Overlay* make_reminders_overlay();
Overlay* make_tailscale_overlay();
Overlay* make_dropbox_overlay();
Overlay* make_plugin_add_overlay();
Overlay* make_plugin_remove_overlay();
std::string image_selector_dispatch(const std::string& rest);

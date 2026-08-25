#include "omarchy_theme.hpp"
#include "config.hpp"
#include "util.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>
#include <fstream>
#include <map>
#include <string>

namespace {

std::string g_status = "not checked yet";

std::string home() {
    const char* h = getenv("HOME");
    return h ? h : ".";
}

bool is_dir(const std::string& p) {
    struct stat st{};
    return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

// ".../omarchy/current" of whichever Omarchy generation is installed.
// Quattro keeps state under ~/.local/state; 3.x under $XDG_CONFIG_HOME.
std::string current_dir() {
    std::string cands[2];
    cands[0] = home() + "/.local/state/omarchy/current";
    const char* xdg = getenv("XDG_CONFIG_HOME");
    cands[1] = (xdg && *xdg ? std::string(xdg) : home() + "/.config") +
               "/omarchy/current";
    for (auto& c : cands)
        if (is_dir(c)) return c;
    return {};
}

// "#RRGGBB" / "#RRGGBBAA" -> Color (alpha defaults to 1)
bool hex_color(std::string s, Color& out) {
    s = trim(s);
    if (s.size() < 7 || s[0] != '#') return false;
    auto h2 = [&](size_t i) {
        return static_cast<int>(strtol(s.substr(i, 2).c_str(), nullptr, 16));
    };
    out.r = h2(1) / 255.0;
    out.g = h2(3) / 255.0;
    out.b = h2(5) / 255.0;
    out.a = s.size() >= 9 ? h2(7) / 255.0 : 1.0;
    return true;
}

// colors.toml: flat `key = "#hex"` lines (mode/comments skipped).
bool read_colors_toml(const std::string& path,
                      std::map<std::string, Color>& out) {
    std::ifstream f(path);
    if (!f) return false;
    std::string line;
    while (std::getline(f, line)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = trim(line.substr(0, eq));
        if (k.empty() || k[0] == '#') continue;
        std::string v = trim(line.substr(eq + 1));
        // strip surrounding quotes and anything after the closing quote
        auto q0 = v.find_first_of("\"'");
        if (q0 != std::string::npos) {
            auto q1 = v.find_first_of("\"'", q0 + 1);
            v = q1 != std::string::npos ? v.substr(q0 + 1, q1 - q0 - 1)
                                        : v.substr(q0 + 1);
        }
        Color c;
        if (hex_color(v, c)) out[k] = c;
    }
    return !out.empty();
}

// waybar.css: `@define-color <name> <#hex>;` lines (early-3.x themes ship
// this directly; late 3.x generates it from colors.toml — same file).
bool read_waybar_css(const std::string& path,
                     std::map<std::string, Color>& out) {
    std::ifstream f(path);
    if (!f) return false;
    std::string line;
    const std::string tag = "@define-color";
    while (std::getline(f, line)) {
        auto p = line.find(tag);
        if (p == std::string::npos) continue;
        std::string rest = trim(line.substr(p + tag.size()));
        auto sp = rest.find_first_of(" \t");
        if (sp == std::string::npos) continue;
        std::string name = trim(rest.substr(0, sp));
        std::string val  = trim(rest.substr(sp + 1));
        if (!val.empty() && val.back() == ';') val.pop_back();
        Color c;
        if (hex_color(val, c)) out[name] = c;
    }
    return !out.empty();
}

Color mix(const Color& a, const Color& b, double t) {
    return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t,
            a.b + (b.b - a.b) * t, 1.0};
}

std::string theme_name(const std::string& cur) {
    // All recent generations write current/theme.name; the earliest 3.x
    // versions used a symlink, whose target's basename is the name.
    std::ifstream f(cur + "/theme.name");
    std::string n;
    if (f && std::getline(f, n) && !trim(n).empty()) return trim(n);
    char buf[512];
    ssize_t len = readlink((cur + "/theme").c_str(), buf, sizeof buf - 1);
    if (len > 0) {
        buf[len] = 0;
        std::string t = buf;
        auto slash = t.find_last_of('/');
        return slash == std::string::npos ? t : t.substr(slash + 1);
    }
    return "active theme";
}

} // namespace

std::string omarchy_theme_watch_dir() { return current_dir(); }

const std::string& omarchy_theme_status() { return g_status; }

bool omarchy_theme_apply(Config& c) {
    std::string cur = current_dir();
    if (cur.empty()) {
        g_status = "no Omarchy install found";
        return false;
    }
    std::string dir = cur + "/theme";
    std::map<std::string, Color> pal;
    bool rich = read_colors_toml(dir + "/colors.toml", pal);
    if (!rich && !read_waybar_css(dir + "/waybar.css", pal)) {
        g_status = "no Omarchy theme found";
        return false;
    }
    auto has = [&](const char* k) { return pal.count(k) != 0; };
    if (!has("background") || !has("foreground")) {
        g_status = "theme lacks background/foreground";
        return false;
    }
    const Color bg = pal["background"], fg = pal["foreground"];

    // Effective colors get the theme; the user's palette (u_*) stays put.
    // Alpha of the user's background/strip is preserved so translucency
    // preferences survive (Omarchy palettes are opaque).
    c.c_bg = bg;
    c.c_bg.a = c.u_bg.a;
    c.c_fg = fg;

    Color accent = fg; // two-tone waybar.css look: fg doubles as accent
    if (rich && has("accent"))    accent = pal["accent"];
    else if (rich && has("blue")) accent = pal["blue"];
    c.c_accent = accent;

    if (rich && has("dark_foreground")) c.c_dim = pal["dark_foreground"];
    else if (rich && has("muted"))      c.c_dim = pal["muted"];
    else                                c.c_dim = mix(bg, fg, 0.55);

    if (rich && has("selection"))               c.c_ws_bg = pal["selection"];
    else if (rich && has("lighter_background")) c.c_ws_bg = pal["lighter_background"];
    else                                        c.c_ws_bg = mix(bg, fg, 0.12);

    if (rich && has("red"))             c.c_urgent = pal["red"];
    else if (rich && has("bright_red")) c.c_urgent = pal["bright_red"];
    else                                c.c_urgent = c.u_urgent;

    c.c_strip = accent;
    c.c_strip.a = c.u_strip.a;

    g_status = theme_name(cur) + (rich ? "" : " (waybar.css)");
    return true;
}

#include "shell.hpp"
#include "bar.hpp"
#include "config.hpp"
#include "modules.hpp"
#include "overlay.hpp"
#include "popup.hpp"
#include "ui.hpp"

#include <dirent.h>
#include <linux/input-event-codes.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <vector>

namespace {

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

void select_fam(cairo_t* cr, const std::string& fam, double size) {
    cairo_select_font_face(cr, fam.c_str(), CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, size);
}

void select_shell_font(cairo_t* cr) {
    select_fam(cr, cfg.font, cfg.shell_font_size);
}

void draw_menu_glyph(cairo_t* cr, const std::string& glyph,
                     const std::string& icon_font, double slot_x,
                     double ymid, double slot_w, double size,
                     const Color& c) {
    if (glyph.empty()) return;
    std::string fam = icon_font.empty() ? cfg.font : icon_font;
    if (fam == "omarchy") fam = cfg.omarchy_font;
    select_fam(cr, fam, size);
    if (!glyph_mapped(cr, glyph)) {
        static const char* fams[] = {
            "JetBrainsMono Nerd Font", "CaskaydiaMono Nerd Font",
            "CaskaydiaCove Nerd Font", "Cascadia Mono NF",
            "Symbols Nerd Font Mono",  "Symbols Nerd Font",
        };
        for (const char* f : fams) {
            select_fam(cr, f, size);
            if (glyph_mapped(cr, glyph)) break;
        }
    }
    cairo_font_extents_t fe;
    cairo_font_extents(cr, &fe);
    cairo_text_extents_t ext;
    cairo_text_extents(cr, glyph.c_str(), &ext);
    col(cr, c, 1.0);
    cairo_move_to(cr, slot_x + (slot_w - ext.x_advance) / 2.0,
                  ymid + (fe.ascent - fe.descent) / 2.0);
    cairo_show_text(cr, glyph.c_str());
    select_shell_font(cr);
}

void rrect(cairo_t* cr, double x, double y, double w, double h, double r) {
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -M_PI_2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI_2);
    cairo_arc(cr, x + r, y + h - r, r, M_PI_2, M_PI);
    cairo_arc(cr, x + r, y + r, r, M_PI, 1.5 * M_PI);
    cairo_close_path(cr);
}

std::string omarchy_path() {
    if (const char* p = getenv("OMARCHY_PATH"); p && *p) return p;
    return "/usr/share/omarchy";
}

// --- tiny JSONC object-of-objects parser (menu definition only) -----------
struct MenuDef {
    std::string icon, icon_font, label, action, when, checked, provider,
        title;
    std::vector<std::string> aliases;
};

std::string strip_jsonc(std::string s) {
    std::string out;
    out.reserve(s.size());
    bool in_str = false, esc = false;
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (in_str) {
            out += c;
            if (esc) esc = false;
            else if (c == '\\') esc = true;
            else if (c == '"') in_str = false;
            continue;
        }
        if (c == '"') {
            in_str = true;
            out += c;
            continue;
        }
        if (c == '/' && i + 1 < s.size() && s[i + 1] == '/') {
            while (i < s.size() && s[i] != '\n') ++i;
            continue;
        }
        if (c == '/' && i + 1 < s.size() && s[i + 1] == '*') {
            i += 2;
            while (i + 1 < s.size() && !(s[i] == '*' && s[i + 1] == '/')) ++i;
            i += 1;
            continue;
        }
        out += c;
    }
    // trailing commas
    std::string cleaned;
    cleaned.reserve(out.size());
    for (size_t i = 0; i < out.size(); ++i) {
        if (out[i] == ',') {
            size_t j = i + 1;
            while (j < out.size() && isspace(static_cast<unsigned char>(out[j])))
                ++j;
            if (j < out.size() && (out[j] == '}' || out[j] == ']')) continue;
        }
        cleaned += out[i];
    }
    return cleaned;
}

std::string parse_string(const std::string& s, size_t& i) {
    if (i >= s.size() || s[i] != '"') return {};
    ++i;
    std::string out;
    while (i < s.size() && s[i] != '"') {
        if (s[i] == '\\' && i + 1 < s.size()) {
            ++i;
            out += s[i++];
        } else out += s[i++];
    }
    if (i < s.size()) ++i;
    return out;
}
void skip_ws(const std::string& s, size_t& i) {
    while (i < s.size() && isspace(static_cast<unsigned char>(s[i]))) ++i;
}
std::vector<std::string> parse_str_array(const std::string& s, size_t& i) {
    std::vector<std::string> a;
    if (i >= s.size() || s[i] != '[') return a;
    ++i;
    skip_ws(s, i);
    while (i < s.size() && s[i] != ']') {
        skip_ws(s, i);
        if (s[i] == '"') a.push_back(parse_string(s, i));
        else {
            while (i < s.size() && s[i] != ',' && s[i] != ']') ++i;
        }
        skip_ws(s, i);
        if (i < s.size() && s[i] == ',') ++i;
        skip_ws(s, i);
    }
    if (i < s.size()) ++i;
    return a;
}
void skip_value(const std::string& s, size_t& i) {
    skip_ws(s, i);
    if (i >= s.size()) return;
    if (s[i] == '"') {
        parse_string(s, i);
        return;
    }
    if (s[i] == '{') {
        int d = 0;
        do {
            if (s[i] == '"') {
                parse_string(s, i);
                continue;
            }
            if (s[i] == '{') ++d;
            else if (s[i] == '}') --d;
            ++i;
        } while (i < s.size() && d > 0);
        return;
    }
    if (s[i] == '[') {
        parse_str_array(s, i);
        return;
    }
    while (i < s.size() && s[i] != ',' && s[i] != '}') ++i;
}

MenuDef parse_item(const std::string& s, size_t& i) {
    MenuDef m;
    skip_ws(s, i);
    if (i >= s.size() || s[i] != '{') return m;
    ++i;
    while (i < s.size()) {
        skip_ws(s, i);
        if (i < s.size() && s[i] == '}') {
            ++i;
            break;
        }
        if (s[i] != '"') {
            ++i;
            continue;
        }
        std::string key = parse_string(s, i);
        skip_ws(s, i);
        if (i < s.size() && s[i] == ':') ++i;
        skip_ws(s, i);
        if (key == "icon") m.icon = parse_string(s, i);
        else if (key == "iconFont") m.icon_font = parse_string(s, i);
        else if (key == "label") m.label = parse_string(s, i);
        else if (key == "action") m.action = parse_string(s, i);
        else if (key == "when") m.when = parse_string(s, i);
        else if (key == "checked") m.checked = parse_string(s, i);
        else if (key == "provider") m.provider = parse_string(s, i);
        else if (key == "title") m.title = parse_string(s, i);
        else if (key == "aliases") m.aliases = parse_str_array(s, i);
        else skip_value(s, i);
        skip_ws(s, i);
        if (i < s.size() && s[i] == ',') ++i;
    }
    return m;
}

std::vector<std::pair<std::string, MenuDef>>
parse_menu_file(const std::string& path) {
    std::vector<std::pair<std::string, MenuDef>> out;
    std::ifstream in(path);
    if (!in) return out;
    std::ostringstream ss;
    ss << in.rdbuf();
    std::string s = strip_jsonc(ss.str());
    size_t i = 0;
    skip_ws(s, i);
    if (i >= s.size() || s[i] != '{') return out;
    ++i;
    while (i < s.size()) {
        skip_ws(s, i);
        if (i < s.size() && s[i] == '}') break;
        if (s[i] != '"') {
            ++i;
            continue;
        }
        std::string id = parse_string(s, i);
        skip_ws(s, i);
        if (i < s.size() && s[i] == ':') ++i;
        skip_ws(s, i);
        out.emplace_back(id, parse_item(s, i));
        skip_ws(s, i);
        if (i < s.size() && s[i] == ',') ++i;
    }
    return out;
}

struct Row {
    std::string id, icon, icon_font, label, action, provider, detail,
        app_icon;
    bool submenu = false;
    bool is_app  = false;
    int  score   = 0;
};

std::string parent_of(const std::string& id) {
    auto d = id.rfind('.');
    return d == std::string::npos ? std::string() : id.substr(0, d);
}

struct DesktopApp {
    std::string name, exec, icon;
};

DesktopApp parse_desktop(const std::string& path) {
    std::ifstream in(path);
    std::string line;
    DesktopApp a;
    bool desktop = false, nodisplay = false;
    while (std::getline(in, line)) {
        if (line == "[Desktop Entry]") desktop = true;
        else if (!line.empty() && line[0] == '[') desktop = false;
        if (!desktop) continue;
        if (line.rfind("Name=", 0) == 0 && a.name.empty())
            a.name = line.substr(5);
        if (line.rfind("Exec=", 0) == 0 && a.exec.empty())
            a.exec = line.substr(5);
        if (line.rfind("Icon=", 0) == 0 && a.icon.empty())
            a.icon = line.substr(5);
        if (line == "NoDisplay=true" || line == "Hidden=true") nodisplay = true;
        if (line.rfind("Type=", 0) == 0 && line != "Type=Application")
            nodisplay = true;
    }
    if (nodisplay || a.name.empty() || a.exec.empty()) return {};
    std::string cleaned;
    for (size_t i = 0; i < a.exec.size(); ++i) {
        if (a.exec[i] == '%' && i + 1 < a.exec.size()) {
            ++i;
            continue;
        }
        cleaned += a.exec[i];
    }
    a.exec = cleaned;
    return a;
}

std::string find_icon_png(const std::string& name) {
    if (name.empty()) return {};
    if (name[0] == '/' && access(name.c_str(), R_OK) == 0) return name;
    auto try_file = [](const std::string& p) -> std::string {
        if (access(p.c_str(), R_OK) == 0) return p;
        if (p.size() < 4 || p.substr(p.size() - 4) != ".png") {
            std::string q = p + ".png";
            if (access(q.c_str(), R_OK) == 0) return q;
        }
        return {};
    };
    const char* home = getenv("HOME");
    std::string dirs[] = {
        "/usr/share/pixmaps",
        "/usr/share/icons/hicolor/48x48/apps",
        "/usr/share/icons/hicolor/32x32/apps",
        "/usr/share/icons/hicolor/64x64/apps",
        "/usr/share/icons/hicolor/128x128/apps",
        "/usr/share/icons/Adwaita/48x48/apps",
        "/usr/share/icons/Papirus/48x48/apps",
        home && *home ? std::string(home) + "/.local/share/icons/hicolor/48x48/apps"
                      : std::string(),
        home && *home ? std::string(home) + "/.local/share/icons"
                      : std::string(),
    };
    for (auto& d : dirs) {
        if (d.empty()) continue;
        auto hit = try_file(d + "/" + name);
        if (!hit.empty()) return hit;
    }
    return {};
}

std::vector<Row> load_apps() {
    std::vector<Row> rows;
    const char* dirs[] = {"/usr/share/applications",
                          "/usr/local/share/applications"};
    auto add_dir = [&](const std::string& dir) {
        DIR* d = opendir(dir.c_str());
        if (!d) return;
        while (dirent* e = readdir(d)) {
            std::string n = e->d_name;
            if (n.size() < 9 || n.substr(n.size() - 8) != ".desktop") continue;
            DesktopApp a = parse_desktop(dir + "/" + n);
            if (a.name.empty()) continue;
            Row r;
            r.label    = a.name;
            r.action   = a.exec;
            r.app_icon = a.icon;
            r.is_app   = true;
            rows.push_back(std::move(r));
        }
        closedir(d);
    };
    for (auto* d : dirs) add_dir(d);
    if (const char* h = getenv("HOME"))
        add_dir(std::string(h) + "/.local/share/applications");
    std::sort(rows.begin(), rows.end(),
              [](const Row& a, const Row& b) { return a.label < b.label; });
    return rows;
}

constexpr int MENU_W = 460, MENU_H = 520, ROW_H = 36, HDR = 44, SEARCH = 0;

class MenuOverlay : public Overlay {
public:
    const char* id() const override { return "omarchy.menu"; }

    void summon(const std::string& payload) override {
        if (!mattbar_shell() || !mattbar_shell()->bar()) return;
        load();
        parent_.clear();
        filter_.clear();
        sel_ = 0;
        scroll_ = 0;
        // payload: {"menu":"root"} / {"menu":"system"} or a bare route
        // ("apps") from the Super+Alt+Space bind.
        std::string route;
        auto p = payload.find("\"menu\"");
        if (p != std::string::npos) {
            auto q = payload.find('"', payload.find(':', p));
            if (q != std::string::npos) {
                auto e = payload.find('"', q + 1);
                if (e != std::string::npos)
                    route = payload.substr(q + 1, e - q - 1);
            }
        } else {
            route = payload;
            while (!route.empty() && (route.front() == ' ' || route.front() == '{' ||
                                      route.front() == '"'))
                route.erase(route.begin());
            while (!route.empty() && (route.back() == ' ' || route.back() == '}' ||
                                      route.back() == '"'))
                route.pop_back();
        }
        if (route != "root" && !route.empty() && route != "{}") parent_ = route;
        rebuild();
        open();
    }
    void hide() override { close(); }
    bool is_open() const override { return host_.is_open(); }
    ~MenuOverlay() override {
        for (auto& [_, s] : icon_surfs_)
            if (s) cairo_surface_destroy(s);
    }

private:
    int font_px() const { return (int)cfg.shell_font_size; }
    int icon_px() const {
        return std::max(14, (int)std::lround(cfg.shell_font_size * 18.0 / 16.0));
    }
    int row_h() const { return std::max(32, font_px() + 20); }
    int hdr_h() const { return std::max(40, font_px() + 28); }

    cairo_surface_t* icon_surf(const std::string& name) {
        if (name.empty()) return nullptr;
        auto it = icon_surfs_.find(name);
        if (it != icon_surfs_.end()) return it->second;
        std::string path = find_icon_png(name);
        cairo_surface_t* s = nullptr;
        if (!path.empty()) {
            s = cairo_image_surface_create_from_png(path.c_str());
            if (cairo_surface_status(s) != CAIRO_STATUS_SUCCESS) {
                cairo_surface_destroy(s);
                s = nullptr;
            }
        }
        icon_surfs_[name] = s;
        return s;
    }
    std::map<std::string, cairo_surface_t*> icon_surfs_;
    ov::Host  host_;
    TextField search_;
    std::vector<std::pair<std::string, MenuDef>> defs_order_;
    std::map<std::string, MenuDef> defs_;
    std::vector<Row> apps_;
    std::string parent_;
    std::string filter_;
    std::vector<Row> rows_;
    int sel_ = 0, scroll_ = 0;
    bool holding_ = false;

    void load() {
        defs_.clear();
        defs_order_.clear();
        auto stock = omarchy_path() + "/default/omarchy/omarchy-menu.jsonc";
        auto user  = omarchy_path() + "/config/omarchy/omarchy-menu.jsonc";
        if (access((std::string(getenv("HOME") ? getenv("HOME") : "") +
                    "/.config/omarchy/omarchy-menu.jsonc")
                       .c_str(),
                   F_OK) == 0)
            stock = std::string(getenv("HOME")) +
                    "/.config/omarchy/omarchy-menu.jsonc";
        else if (access(user.c_str(), F_OK) == 0)
            stock = user;
        defs_order_ = parse_menu_file(stock);
        auto ext = std::string(getenv("HOME") ? getenv("HOME") : "") +
                   "/.config/omarchy/extensions/omarchy-menu.jsonc";
        for (auto& kv : parse_menu_file(ext)) {
            bool found = false;
            for (auto& e : defs_order_)
                if (e.first == kv.first) {
                    e.second = kv.second;
                    found    = true;
                    break;
                }
            if (!found) defs_order_.push_back(kv);
        }
        for (auto& [k, v] : defs_order_) defs_[k] = v;
        apps_ = load_apps();
        for (auto& a : apps_) a.is_app = true;
    }

    std::vector<Row> children_of(const std::string& parent) const {
        std::vector<Row> rows;
        for (auto& [id, d] : defs_order_) {
            if (parent_of(id) != parent) continue;
            Row r;
            r.id        = id;
            r.icon      = d.icon;
            r.icon_font = d.icon_font;
            r.label     = d.label.empty() ? id : d.label;
            r.action    = d.action;
            r.provider  = d.provider;
            r.submenu   = r.action.empty();
            if (r.submenu) {
                for (auto& [oid, _] : defs_order_)
                    if (parent_of(oid) == id) {
                        r.submenu = true;
                        break;
                    }
                if (!r.provider.empty()) r.submenu = true;
            }
            rows.push_back(std::move(r));
        }
        return rows;
    }

    static std::string lower(std::string s) {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return (char)std::tolower(c); });
        return s;
    }
    // Lower is better, matching Omarchy's searchScore.
    static int search_score(const Row& r, const std::string& q) {
        std::string label = lower(r.label);
        int score = 80;
        if (label == q) score = r.is_app ? 0 : 2;
        else if (r.is_app && label.find(q) != std::string::npos &&
                 (label == q || label.find(q + " ") != std::string::npos ||
                  label.find(" " + q) != std::string::npos))
            score = 0;
        else if (label.rfind(q, 0) == 0) score = 10;
        else if (label.find(q) != std::string::npos) score = 30;
        else score = 80;
        if (r.submenu) score += 2;
        if (r.is_app) score -= 5;
        return score;
    }
    static bool matches(const Row& r, const std::string& q) {
        if (lower(r.label).find(q) != std::string::npos) return true;
        return false;
    }

    void rebuild() {
        if (!filter_.empty()) {
            rows_.clear();
            std::string q = lower(filter_);
            auto consider = [&](Row r) {
                if (!matches(r, q)) return;
                r.score = search_score(r, q);
                rows_.push_back(std::move(r));
            };
            // Current-menu children, then deeper items, then apps — the
            // same typeahead Omarchy uses from the root "Go" menu.
            std::string active = parent_;
            for (auto& [id, d] : defs_order_) {
                if (id == "root") continue;
                bool under = active.empty()
                                 ? true
                                 : (id == active || parent_of(id) == active ||
                                    id.rfind(active + ".", 0) == 0);
                if (!under) continue;
                Row r;
                r.id        = id;
                r.icon      = d.icon;
                r.icon_font = d.icon_font;
                r.label     = d.label.empty() ? id : d.label;
                r.action    = d.action;
                r.provider  = d.provider;
                r.submenu   = r.action.empty() || !d.provider.empty();
                r.detail    = parent_of(id);
                consider(std::move(r));
            }
            if (active.empty() || active == "apps") {
                for (auto& a : apps_) consider(a);
            }
            std::sort(rows_.begin(), rows_.end(), [](const Row& a, const Row& b) {
                if (a.score != b.score) return a.score < b.score;
                return a.label < b.label;
            });
        } else if (!parent_.empty()) {
            auto it = defs_.find(parent_);
            if (it != defs_.end() && it->second.provider == "apps")
                rows_ = apps_;
            else
                rows_ = children_of(parent_);
        } else {
            rows_ = children_of("");
        }
        if (sel_ >= (int)rows_.size()) sel_ = (int)rows_.size() - 1;
        if (sel_ < 0) sel_ = 0;
        int visible = (MENU_H - hdr_h() - SEARCH) / row_h();
        if (sel_ < scroll_) scroll_ = sel_;
        if (sel_ >= scroll_ + visible) scroll_ = sel_ - visible + 1;
        if (scroll_ < 0) scroll_ = 0;
    }

    void activate() {
        if (sel_ < 0 || sel_ >= (int)rows_.size()) return;
        Row r = rows_[sel_];
        if (!r.action.empty() && r.provider.empty() &&
            (r.id.empty() || !r.submenu)) {
            spawn_detached(r.action);
            close();
            return;
        }
        if (r.submenu || !r.provider.empty()) {
            parent_ = r.id;
            filter_.clear();
            search_.clear();
            sel_ = 0;
            scroll_ = 0;
            rebuild();
            host_.redraw();
            return;
        }
    }

    void back() {
        if (!filter_.empty()) {
            filter_.clear();
            search_.clear();
            rebuild();
            host_.redraw();
            return;
        }
        if (parent_.empty()) {
            close();
            return;
        }
        parent_ = parent_of(parent_);
        sel_ = 0;
        scroll_ = 0;
        rebuild();
        host_.win.draw();
    }

    void hold(bool on) {
        auto* sh = mattbar_shell();
        if (!sh || !sh->bar() || on == holding_) return;
        holding_ = on;
        sh->bar()->hold_open(on);
    }

    void open() {
        auto* sh = mattbar_shell();
        if (!sh || !sh->bar()) return;
        host_.win.paint    = [this](cairo_t* cr) { paint(cr); };
        host_.win.click    = [this](double x, double y, int btn) {
            on_click(x, y, btn);
        };
        host_.win.pscroll  = [this](int d) {
            sel_ = std::clamp(sel_ + d, 0, std::max(0, (int)rows_.size() - 1));
            rebuild();
            host_.redraw();
        };
        host_.win.pkey = [this](const Bar::KeyEvent& e) { on_key(e); };
        host_.open(MENU_W, MENU_H, "mattbar-menu", id(), true,
                   ov::Host::Place::Center);
    }

    void close() {
        host_.close();
        filter_.clear();
        search_.clear();
        parent_.clear();
    }

    void on_key(const Bar::KeyEvent& e) {
        if (!e.pressed) return;
        if (e.escape()) {
            back();
            return;
        }
        if (e.enter()) {
            activate();
            return;
        }
        if (e.up()) {
            if (sel_ > 0) --sel_;
            rebuild();
            host_.redraw();
            return;
        }
        if (e.down()) {
            if (sel_ + 1 < (int)rows_.size()) ++sel_;
            rebuild();
            host_.redraw();
            return;
        }
        if (search_.handle(e)) {
            filter_ = search_.text;
            sel_    = 0;
            rebuild();
            host_.redraw();
        }
    }

    void on_click(double /*x*/, double y, int btn) {
        if (btn != BTN_LEFT) return;
        if (y < hdr_h()) {
            back();
            return;
        }
        int row = (int)((y - hdr_h()) / row_h()) + scroll_;
        if (row >= 0 && row < (int)rows_.size()) {
            sel_ = row;
            activate();
        }
    }

    void paint(cairo_t* cr) {
        cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
        cairo_set_source_rgba(cr, 0, 0, 0, 0);
        cairo_paint(cr);
        cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
        col(cr, cfg.c_bg, std::max(cfg.c_bg.a, 0.96));
        rrect(cr, 0.5, 0.5, MENU_W - 1, MENU_H - 1, 12);
        cairo_fill_preserve(cr);
        col(cr, cfg.c_ws_bg, 1);
        cairo_set_line_width(cr, 1);
        cairo_stroke(cr);

        select_shell_font(cr);
        const int rh = row_h(), hh = hdr_h(), ip = icon_px();
        const double slot = ip + 16;

        std::string title = "Go";
        if (!parent_.empty()) {
            auto it = defs_.find(parent_);
            if (it != defs_.end())
                title = it->second.title.empty() ? it->second.label
                                                 : it->second.title;
        }
        if (!filter_.empty()) title = filter_;
        say(cr, 18, hh / 2.0, title + "\u2026",
            filter_.empty() ? cfg.c_dim : cfg.c_fg);
        if (!parent_.empty() || !filter_.empty())
            say(cr, MENU_W - 28, hh / 2.0, "\u2039", cfg.c_dim);

        int visible = (MENU_H - hh - SEARCH) / rh;
        int y0      = hh + SEARCH;
        for (int i = 0; i < visible; ++i) {
            int idx = scroll_ + i;
            if (idx >= (int)rows_.size()) break;
            double y = y0 + i * rh, mid = y + rh / 2.0;
            if (idx == sel_) {
                col(cr, cfg.c_accent, 0.35);
                rrect(cr, 10, y + 2, MENU_W - 20, rh - 4, 6);
                cairo_fill(cr);
            }
            const Row& r = rows_[idx];
            Color ic = idx == sel_ ? contrast_on(cfg.c_accent) : cfg.c_fg;
            cairo_surface_t* app = r.is_app ? icon_surf(r.app_icon) : nullptr;
            if (app) {
                int iw = cairo_image_surface_get_width(app);
                int ih = cairo_image_surface_get_height(app);
                if (iw > 0 && ih > 0) {
                    double s = (double)ip / std::max(iw, ih);
                    double dx = 12 + (slot - iw * s) / 2.0;
                    double dy = mid - ih * s / 2.0;
                    cairo_save(cr);
                    cairo_translate(cr, dx, dy);
                    cairo_scale(cr, s, s);
                    cairo_set_source_surface(cr, app, 0, 0);
                    cairo_paint(cr);
                    cairo_restore(cr);
                }
            } else {
                std::string glyph = r.icon.empty() ? (r.is_app ? "" : "•")
                                                   : r.icon;
                draw_menu_glyph(cr, glyph, r.icon_font, 12, mid, slot, ip, ic);
            }
            select_shell_font(cr);
            say(cr, 12 + slot + 6, mid, r.label,
                idx == sel_ ? contrast_on(cfg.c_accent) : cfg.c_fg);
            if (r.submenu)
                say(cr, MENU_W - 28, mid, "\u203a", cfg.c_dim);
        }
        (void)tw;
    }
};

} // namespace

Overlay* make_menu_overlay() { return new MenuOverlay; }

// Clipboard history, emoji picker, and image-selector overlays.
#include "overlay.hpp"
#include "util.hpp"

#include "stb_image.h"

#include <linux/input-event-codes.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>

#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
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
using ov::parse_json_string;

std::string omarchy_path() {
    if (const char* p = getenv("OMARCHY_PATH"); p && *p) return p;
    return "/usr/share/omarchy";
}
std::string home_dir() {
    const char* h = getenv("HOME");
    return h && *h ? h : ".";
}
std::string clipboard_path() {
    const char* x = getenv("XDG_STATE_HOME");
    if (x && *x) return std::string(x) + "/omarchy/clipboard-history.json";
    return home_dir() + "/.local/state/omarchy/clipboard-history.json";
}

cairo_surface_t* load_still(const std::string& path, int max_edge) {
    if (path.empty()) return nullptr;
    int w = 0, h = 0, n = 0;
    unsigned char* px = stbi_load(path.c_str(), &w, &h, &n, 4);
    if (!px || w <= 0 || h <= 0) {
        if (px) stbi_image_free(px);
        return nullptr;
    }
    int dw = w, dh = h;
    if (max_edge > 0 && std::max(w, h) > max_edge) {
        double s = (double)max_edge / std::max(w, h);
        dw = std::max(1, (int)(w * s));
        dh = std::max(1, (int)(h * s));
    }
    cairo_surface_t* cs = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, dw, dh);
    unsigned char* dst  = cairo_image_surface_get_data(cs);
    int stride          = cairo_image_surface_get_stride(cs);
    for (int y = 0; y < dh; ++y) {
        int sy = y * h / dh;
        auto* row = reinterpret_cast<uint32_t*>(dst + y * stride);
        unsigned char* src = px + sy * w * 4;
        for (int x = 0; x < dw; ++x) {
            int sx = x * w / dw;
            unsigned char r = src[sx * 4 + 0], g = src[sx * 4 + 1],
                          b = src[sx * 4 + 2], a = src[sx * 4 + 3];
            row[x] = ((uint32_t)a << 24) | ((uint32_t)r << 16) |
                     ((uint32_t)g << 8) | b;
        }
    }
    cairo_surface_mark_dirty(cs);
    stbi_image_free(px);
    return cs;
}

void touch_file(const std::string& path) {
    if (path.empty()) return;
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) close(fd);
}

void write_file(const std::string& path, const std::string& body) {
    if (path.empty()) return;
    std::ofstream f(path);
    if (f) f << body;
}

// ---------------------------------------------------------------------------
// Clipboard
// ---------------------------------------------------------------------------
struct Clip {
    std::string type, text, path, mime;
    int index = 0;
};

std::vector<Clip> parse_clipboard(const std::string& raw) {
    std::vector<Clip> out;
    size_t i = 0;
    while (i < raw.size()) {
        if (raw[i] != '{') {
            ++i;
            continue;
        }
        size_t start = i;
        int depth = 0;
        bool in_str = false, esc = false;
        for (; i < raw.size(); ++i) {
            char c = raw[i];
            if (in_str) {
                if (esc) esc = false;
                else if (c == '\\') esc = true;
                else if (c == '"') in_str = false;
                continue;
            }
            if (c == '"') in_str = true;
            else if (c == '{') ++depth;
            else if (c == '}') {
                --depth;
                if (depth == 0) {
                    ++i;
                    break;
                }
            }
        }
        std::string obj = raw.substr(start, i - start);
        Clip c;
        c.type = ov::json_str(obj, "type");
        if (c.type.empty()) c.type = ov::json_str(obj, "kind");
        c.text = ov::json_str(obj, "text");
        c.path = ov::json_str(obj, "path");
        c.mime = ov::json_str(obj, "mime");
        if (c.type.empty() && !c.text.empty()) c.type = "text";
        if (c.type.empty() && !c.path.empty()) c.type = "image";
        if (c.type == "text" && c.text.empty()) continue;
        if (c.type == "image" && c.path.empty()) continue;
        if (c.type.empty()) continue;
        out.push_back(std::move(c));
        if ((int)out.size() >= cfg.shell_clipboard_limit) break;
    }
    return out;
}

class ClipboardOverlay : public Overlay {
public:
    const char* id() const override { return "omarchy.clipboard"; }
    void summon(const std::string&) override { open(); }
    void hide() override { close(); }
    bool is_open() const override { return host_.is_open(); }
    ~ClipboardOverlay() override { close(); }

private:
    Host host_;
    TextField search_;
    std::vector<Clip> all_, rows_;
    std::string filter_;
    int sel_ = 0, scroll_ = 0;
    bool confirm_ = false;

    int W() const { return cfg.shell_overlay_width; }
    int H() const { return cfg.shell_overlay_height; }
    double fs() const { return cfg.shell_clipboard_font_size; }

    void close() {
        confirm_ = false;
        filter_.clear();
        search_.clear();
        host_.close();
    }

    void load() {
        all_ = parse_clipboard(slurp(clipboard_path()));
        for (int i = 0; i < (int)all_.size(); ++i) all_[i].index = i;
        rebuild();
    }
    void rebuild() {
        rows_.clear();
        std::string q = filter_;
        std::transform(q.begin(), q.end(), q.begin(),
                       [](unsigned char c) { return (char)std::tolower(c); });
        for (auto& c : all_) {
            if (!q.empty()) {
                std::string hay = c.type == "image" ? c.path : c.text;
                std::transform(hay.begin(), hay.end(), hay.begin(),
                               [](unsigned char c) { return (char)std::tolower(c); });
                if (hay.find(q) == std::string::npos) continue;
            }
            rows_.push_back(c);
        }
        sel_ = ov::clamp_sel(sel_, (int)rows_.size());
    }

    void open() {
        sel_ = scroll_ = 0;
        confirm_ = false;
        filter_.clear();
        search_.clear();
        load();
        host_.win.paint = [this](cairo_t* cr) { paint(cr); };
        host_.win.click = [this](double x, double y, int b) { on_click(x, y, b); };
        host_.win.pscroll = [this](int d) {
            sel_ = ov::clamp_sel(sel_ + d, (int)rows_.size());
            host_.redraw();
        };
        host_.win.pkey = [this](const Bar::KeyEvent& e) { on_key(e); };
        host_.open(W(), H(), "omarchy-clipboard", id());
    }

    void paste() {
        if (sel_ < 0 || sel_ >= (int)rows_.size()) return;
        const Clip& c = rows_[sel_];
        if (c.type == "image") {
            std::string mime = c.mime.empty() ? "image/png" : c.mime;
            spawn_detached(std::string("omarchy-clipboard-paste-file ") +
                           (cfg.shell_clipboard_paste ? "" : "--copy-only ") +
                           shell_quote(mime) + " " + shell_quote(c.path));
        } else {
            spawn_detached(std::string("omarchy-clipboard-paste-text ") +
                           (cfg.shell_clipboard_paste ? "--shift-insert "
                                                      : "--copy-only ") +
                           "--history-index " + std::to_string(c.index));
        }
        close();
    }

    void clear_history() {
        write_file(clipboard_path(), "[]\n");
        all_.clear();
        rows_.clear();
        confirm_ = false;
        host_.redraw();
    }

    void on_click(double x, double y, int btn) {
        const int hh = ov::hdr_h(fs()), sh = ov::search_h(fs()), rh = ov::row_h(fs());
        if (confirm_) {
            if (btn != BTN_LEFT) return;
            if (y > H() / 2.0 - 20 && y < H() / 2.0 + 30) {
                if (x < W() / 2.0) confirm_ = false;
                else clear_history();
                host_.redraw();
            }
            return;
        }
        if (btn == BTN_RIGHT) {
            confirm_ = !all_.empty();
            host_.redraw();
            return;
        }
        if (btn != BTN_LEFT) return;
        int y0 = hh + sh + 8;
        int row = (int)((y - y0) / rh) + scroll_;
        if (row >= 0 && row < (int)rows_.size()) {
            sel_ = row;
            paste();
        }
        (void)x;
    }

    void on_key(const Bar::KeyEvent& e) {
        if (!e.pressed) return;
        if (confirm_) {
            if (e.escape() || e.keysym == 0x6e /* n */) {
                confirm_ = false;
                host_.redraw();
                return;
            }
            if (e.enter() || e.keysym == 0x79 /* y */) {
                clear_history();
                return;
            }
            return;
        }
        if (e.escape()) {
            if (!filter_.empty()) {
                filter_.clear();
                search_.clear();
                rebuild();
                host_.redraw();
                return;
            }
            close();
            return;
        }
        if (e.enter()) {
            paste();
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
        if (e.ctrl() && (e.keysym == 0x6c || e.keysym == 0x4c)) {
            confirm_ = !all_.empty();
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

    void paint(cairo_t* cr) {
        panel_bg(cr, W(), H());
        select_shell_font(cr, fs());
        const int hh = ov::hdr_h(fs()), sh = ov::search_h(fs()), rh = ov::row_h(fs());
        say(cr, 18, hh / 2.0, "Clipboard", cfg.c_fg);
        char nbuf[32];
        snprintf(nbuf, sizeof nbuf, "%d", (int)rows_.size());
        say(cr, W() - 18 - tw(cr, nbuf), hh / 2.0, nbuf, cfg.c_dim);
        search_.draw(cr, 18, hh, W() - 36, sh, "search");
        if (confirm_) {
            say(cr, 18, H() / 2.0 - 24, "Clear clipboard history?", cfg.c_fg);
            col(cr, cfg.c_ws_bg, 1);
            rrect(cr, 40, H() / 2.0, 100, 28, 6);
            cairo_fill(cr);
            say(cr, 58, H() / 2.0 + 14, "Cancel", cfg.c_fg);
            col(cr, cfg.c_urgent, 1);
            rrect(cr, W() - 140, H() / 2.0, 100, 28, 6);
            cairo_fill(cr);
            say(cr, W() - 118, H() / 2.0 + 14, "Clear", contrast_on(cfg.c_urgent));
            return;
        }
        int y0 = hh + sh + 8;
        int vis = std::max(1, (H() - y0 - 8) / rh);
        ov::keep_visible(sel_, scroll_, vis, (int)rows_.size());
        if (rows_.empty())
            say(cr, 18, y0 + rh / 2.0, "No clipboard history", cfg.c_dim);
        for (int i = 0; i < vis; ++i) {
            int idx = scroll_ + i;
            if (idx >= (int)rows_.size()) break;
            double y = y0 + i * rh, mid = y + rh / 2.0;
            if (idx == sel_) {
                col(cr, cfg.c_accent, 0.28);
                rrect(cr, 10, y + 2, W() - 20, rh - 4, 6);
                cairo_fill(cr);
            }
            const Clip& c = rows_[idx];
            Color fg = idx == sel_ ? contrast_on(cfg.c_accent) : cfg.c_fg;
            std::string lab;
            if (c.type == "image") lab = "[image] " + utf8_trunc(c.path, 48);
            else {
                lab = c.text;
                for (char& ch : lab)
                    if (ch == '\n' || ch == '\t') ch = ' ';
                lab = utf8_trunc(lab, 64);
            }
            say(cr, 18, mid, lab, fg);
        }
    }
};

// ---------------------------------------------------------------------------
// Emoji
// ---------------------------------------------------------------------------
struct Emoji {
    std::string e, k;
};

std::vector<Emoji> g_emojis;
bool g_emojis_loaded = false;

void load_emojis() {
    if (g_emojis_loaded) return;
    g_emojis_loaded = true;
    std::string raw =
        slurp(omarchy_path() + "/shell/plugins/emojis/emojis.json");
    if (raw.empty())
        raw = slurp("/usr/share/omarchy/shell/plugins/emojis/emojis.json");
    size_t i = 0;
    while (i < raw.size()) {
        auto p = raw.find("\"e\"", i);
        if (p == std::string::npos) break;
        i = raw.find(':', p);
        if (i == std::string::npos) break;
        i = raw.find('"', i);
        if (i == std::string::npos) break;
        Emoji em;
        em.e = parse_json_string(raw, i);
        auto k = raw.find("\"k\"", i);
        if (k != std::string::npos && k < i + 80) {
            i = raw.find(':', k);
            i = raw.find('"', i);
            if (i != std::string::npos) em.k = parse_json_string(raw, i);
        } else ++i;
        if (!em.e.empty()) g_emojis.push_back(std::move(em));
    }
}

class EmojiOverlay : public Overlay {
public:
    const char* id() const override { return "omarchy.emojis"; }
    void summon(const std::string&) override { open(); }
    void hide() override { close(); }
    bool is_open() const override { return host_.is_open(); }
    ~EmojiOverlay() override { close(); }

private:
    Host host_;
    TextField search_;
    std::vector<int> rows_; // indices into g_emojis
    std::string filter_;
    int sel_ = 0, scroll_ = 0, cols_ = 8;

    int W() const { return cfg.shell_overlay_width; }
    int H() const { return cfg.shell_overlay_height; }
    double fs() const { return cfg.shell_emoji_font_size; }
    int cell() const { return std::max(40, ov::font_px(fs()) * 2 + 8); }

    void close() {
        filter_.clear();
        search_.clear();
        host_.close();
    }

    void rebuild() {
        rows_.clear();
        std::string q = filter_;
        std::transform(q.begin(), q.end(), q.begin(),
                       [](unsigned char c) { return (char)std::tolower(c); });
        for (int i = 0; i < (int)g_emojis.size(); ++i) {
            if (q.empty()) {
                rows_.push_back(i);
                continue;
            }
            std::string k = g_emojis[i].k;
            std::transform(k.begin(), k.end(), k.begin(),
                           [](unsigned char c) { return (char)std::tolower(c); });
            if (k.find(q) != std::string::npos ||
                g_emojis[i].e.find(filter_) != std::string::npos)
                rows_.push_back(i);
            if ((int)rows_.size() >= 400) break;
        }
        sel_ = ov::clamp_sel(sel_, (int)rows_.size());
    }

    void open() {
        load_emojis();
        sel_ = scroll_ = 0;
        filter_.clear();
        search_.clear();
        rebuild();
        host_.win.paint = [this](cairo_t* cr) { paint(cr); };
        host_.win.click = [this](double x, double y, int b) { on_click(x, y, b); };
        host_.win.pscroll = [this](int d) {
            sel_ = ov::clamp_sel(sel_ + d * cols_, (int)rows_.size());
            host_.redraw();
        };
        host_.win.pkey = [this](const Bar::KeyEvent& e) { on_key(e); };
        host_.open(W(), H(), "omarchy-emojis", id());
    }

    void activate() {
        if (sel_ < 0 || sel_ >= (int)rows_.size()) return;
        const std::string& e = g_emojis[rows_[sel_]].e;
        if (cfg.shell_emoji_insert)
            spawn_detached("omarchy-menu-emoji-insert " + shell_quote(e));
        else
            spawn_detached("printf %s " + shell_quote(e) + " | wl-copy");
        close();
    }

    void on_click(double x, double y, int btn) {
        if (btn != BTN_LEFT) return;
        const int hh = ov::hdr_h(fs()), sh = ov::search_h(fs()), c = cell();
        int pad = 16;
        cols_   = std::max(1, (W() - pad * 2) / c);
        int y0  = hh + sh + 8;
        int ci = (int)((x - pad) / c);
        int row = (int)((y - y0) / c) + scroll_;
        int idx = row * cols_ + ci;
        if (ci < 0 || ci >= cols_) return;
        if (idx >= 0 && idx < (int)rows_.size()) {
            sel_ = idx;
            activate();
        }
    }

    void on_key(const Bar::KeyEvent& e) {
        if (!e.pressed) return;
        if (e.escape()) {
            if (!filter_.empty()) {
                filter_.clear();
                search_.clear();
                rebuild();
                host_.redraw();
                return;
            }
            close();
            return;
        }
        if (e.enter()) {
            activate();
            return;
        }
        if (e.left()) {
            if (sel_ > 0) --sel_;
            host_.redraw();
            return;
        }
        if (e.right()) {
            if (sel_ + 1 < (int)rows_.size()) ++sel_;
            host_.redraw();
            return;
        }
        if (e.up()) {
            sel_ = ov::clamp_sel(sel_ - cols_, (int)rows_.size());
            host_.redraw();
            return;
        }
        if (e.down()) {
            sel_ = ov::clamp_sel(sel_ + cols_, (int)rows_.size());
            host_.redraw();
            return;
        }
        if (search_.handle(e)) {
            filter_ = search_.text;
            std::transform(filter_.begin(), filter_.end(), filter_.begin(),
                           [](unsigned char c) { return (char)std::tolower(c); });
            sel_ = 0;
            rebuild();
            host_.redraw();
        }
    }

    void paint(cairo_t* cr) {
        panel_bg(cr, W(), H());
        select_shell_font(cr, fs());
        const int hh = ov::hdr_h(fs()), sh = ov::search_h(fs()), c = cell();
        say(cr, 18, hh / 2.0, "Emoji", cfg.c_fg);
        search_.draw(cr, 18, hh, W() - 36, sh, "search");
        int pad = 16;
        cols_   = std::max(1, (W() - pad * 2) / c);
        int y0  = hh + sh + 8;
        int vis_rows = std::max(1, (H() - y0 - 8) / c);
        int row_of   = cols_ ? sel_ / cols_ : 0;
        ov::keep_visible(row_of, scroll_, vis_rows,
                         (int)((rows_.size() + cols_ - 1) / std::max(1, cols_)));
        cairo_select_font_face(cr, cfg.notification_emoji_font.c_str(),
                               CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, std::max(18.0, fs() + 6));
        if (rows_.empty()) {
            select_shell_font(cr, fs());
            say(cr, 18, y0 + 20, "No matches", cfg.c_dim);
            return;
        }
        for (int r = 0; r < vis_rows; ++r) {
            for (int ci = 0; ci < cols_; ++ci) {
                int idx = (scroll_ + r) * cols_ + ci;
                if (idx >= (int)rows_.size()) break;
                double x = pad + ci * c, y = y0 + r * c;
                if (idx == sel_) {
                    ov::col(cr, cfg.c_accent, 0.35);
                    rrect(cr, x + 2, y + 2, c - 4, c - 4, 8);
                    cairo_fill(cr);
                }
                const std::string& e = g_emojis[rows_[idx]].e;
                cairo_text_extents_t ext;
                cairo_text_extents(cr, e.c_str(), &ext);
                cairo_font_extents_t fe;
                cairo_font_extents(cr, &fe);
                cairo_set_source_rgba(cr, cfg.c_fg.r, cfg.c_fg.g, cfg.c_fg.b, 1);
                cairo_move_to(cr, x + (c - ext.x_advance) / 2.0,
                              y + c / 2.0 + (fe.ascent - fe.descent) / 2.0);
                cairo_show_text(cr, e.c_str());
            }
        }
        select_shell_font(cr, fs());
        if (sel_ >= 0 && sel_ < (int)rows_.size())
            say(cr, 18, H() - 14, g_emojis[rows_[sel_]].k, cfg.c_dim);
    }
};

// ---------------------------------------------------------------------------
// Image picker
// ---------------------------------------------------------------------------
struct Img {
    std::string path, thumb, label;
};

class ImagePickerOverlay : public Overlay {
public:
    const char* id() const override { return "omarchy.image-picker"; }
    void summon(const std::string& payload) override { open(payload); }
    void hide() override { cancel(""); }
    bool is_open() const override { return host_.is_open(); }
    std::string call(const std::string& method,
                     const std::string& arg) override {
        if (method == "cancel") {
            cancel(arg);
            return "ok";
        }
        if (method == "preload") return "ok";
        return "unknown";
    }
    ~ImagePickerOverlay() override {
        cancel("");
        for (auto& [_, s] : cache_)
            if (s) cairo_surface_destroy(s);
    }

private:
    Host host_;
    AsyncCmd cmd_;
    TextField search_;
    std::vector<Img> imgs_;
    std::vector<int> view_;
    std::map<std::string, cairo_surface_t*> cache_;
    std::string dirs_, rows_raw_, selected_, sel_file_, done_file_, filter_;
    bool show_labels_ = false, filterable_ = false, loading_ = false;
    int sel_ = 0, scroll_ = 0, cols_ = 4;

    int W() const { return std::max(cfg.shell_overlay_width, 640); }
    int H() const { return std::max(cfg.shell_overlay_height, 420); }
    double fs() const { return cfg.shell_image_font_size; }

    void cancel(const std::string& extra_done) {
        if (!done_file_.empty()) touch_file(done_file_);
        if (!extra_done.empty() && extra_done != done_file_)
            touch_file(extra_done);
        sel_file_.clear();
        done_file_.clear();
        host_.close();
    }

    void parse_payload(const std::string& payload) {
        dirs_.clear();
        rows_raw_.clear();
        selected_.clear();
        sel_file_.clear();
        done_file_.clear();
        show_labels_ = cfg.shell_image_show_labels;
        filterable_  = false;
        if (!payload.empty() && payload[0] == '{') {
            dirs_        = ov::json_str(payload, "imageDirs");
            rows_raw_    = ov::json_str(payload, "imageRows");
            selected_    = ov::json_str(payload, "selectedImage");
            sel_file_    = ov::json_str(payload, "selectionFile");
            done_file_   = ov::json_str(payload, "doneFile");
            auto sl      = ov::json_str(payload, "showLabels");
            auto fl      = ov::json_str(payload, "filterable");
            if (!sl.empty()) show_labels_ = ov::json_truthy(sl);
            if (!fl.empty()) filterable_ = ov::json_truthy(fl);
            return;
        }
        auto args = ov::split_keep_empty(payload);
        // positional: [imageDirs, rowsB64, selected, selFile, done, labels, filt]
        auto at = [&](size_t i) -> std::string {
            return i < args.size() ? args[i] : std::string();
        };
        dirs_      = at(0);
        rows_raw_  = ov::b64_decode(at(1));
        selected_  = at(2);
        sel_file_  = at(3);
        done_file_ = at(4);
        if (!at(5).empty()) show_labels_ = ov::json_truthy(at(5));
        if (!at(6).empty()) filterable_ = ov::json_truthy(at(6));
    }

    void parse_rows(const std::string& rows) {
        imgs_.clear();
        std::istringstream ss(rows);
        std::string line;
        while (std::getline(ss, line)) {
            if (line.empty()) continue;
            auto tab = line.find('\t');
            Img im;
            im.path  = tab == std::string::npos ? line : line.substr(0, tab);
            im.thumb = tab == std::string::npos ? im.path : line.substr(tab + 1);
            auto slash = im.path.find_last_of('/');
            im.label   = slash == std::string::npos ? im.path
                                                    : im.path.substr(slash + 1);
            if (!im.path.empty()) imgs_.push_back(std::move(im));
        }
        sel_ = 0;
        for (int i = 0; i < (int)imgs_.size(); ++i)
            if (imgs_[i].path == selected_) sel_ = i;
        rebuild_view();
    }

    void rebuild_view() {
        view_.clear();
        std::string q = filter_;
        std::transform(q.begin(), q.end(), q.begin(),
                       [](unsigned char c) { return (char)std::tolower(c); });
        for (int i = 0; i < (int)imgs_.size(); ++i) {
            if (q.empty()) {
                view_.push_back(i);
                continue;
            }
            std::string l = imgs_[i].label;
            std::transform(l.begin(), l.end(), l.begin(),
                           [](unsigned char c) { return (char)std::tolower(c); });
            if (l.find(q) != std::string::npos) view_.push_back(i);
        }
        if (!view_.empty()) {
            bool keep = false;
            for (int v : view_)
                if (v == sel_) {
                    keep = true;
                    break;
                }
            if (!keep) sel_ = view_[0];
        }
    }

    void load_from_dirs() {
        auto* sh = mattbar_shell();
        if (!sh || !sh->bar()) return;
        std::string d = dirs_;
        if (d.empty()) {
            d = home_dir() + "/.local/state/omarchy/current/theme/backgrounds";
            if (access(d.c_str(), R_OK) != 0)
                d = omarchy_path() + "/themes";
        }
        std::string script =
            omarchy_path() + "/shell/plugins/image-picker/list.sh";
        loading_ = true;
        std::string cmd = "if [ -x " + shell_quote(script) + " ]; then " +
                          shell_quote(script) + " " + shell_quote(d) +
                          "; else find -L " + shell_quote(d) +
                          " -maxdepth 1 -type f \\( -iname '*.jpg' -o "
                          "-iname '*.jpeg' -o -iname '*.png' -o -iname "
                          "'*.webp' -o -iname '*.gif' -o -iname '*.bmp' "
                          "\\) -printf '%p\\t%p\\n' 2>/dev/null | sort; fi";
        cmd_.run(*sh->bar(), cmd,
                 [this](const std::string& out, int) {
                     parse_rows(out);
                     loading_ = false;
                     host_.redraw();
                 },
                 8000);
    }

    void apply_sel() {
        int idx = current();
        if (idx < 0) {
            cancel("");
            return;
        }
        if (!sel_file_.empty())
            write_file(sel_file_, imgs_[idx].path + "\n");
        if (!done_file_.empty()) touch_file(done_file_);
        sel_file_.clear();
        done_file_.clear();
        host_.close();
    }

    int current() const {
        if (sel_ < 0 || sel_ >= (int)imgs_.size()) return -1;
        for (int v : view_)
            if (v == sel_) return sel_;
        return view_.empty() ? -1 : view_[0];
    }

    void open(const std::string& payload) {
        parse_payload(payload);
        filter_.clear();
        search_.clear();
        sel_ = scroll_ = 0;
        host_.win.paint = [this](cairo_t* cr) { paint(cr); };
        host_.win.click = [this](double x, double y, int b) { on_click(x, y, b); };
        host_.win.pscroll = [this](int d) { move(-d); };
        host_.win.pkey    = [this](const Bar::KeyEvent& e) { on_key(e); };
        host_.open(W(), H(), "omarchy-image-selector", id());
        if (!rows_raw_.empty()) parse_rows(rows_raw_);
        else load_from_dirs();
        host_.redraw();
    }

    void move(int delta) {
        if (view_.empty()) return;
        int pos = 0;
        for (int i = 0; i < (int)view_.size(); ++i)
            if (view_[i] == sel_) pos = i;
        pos = ov::clamp_sel(pos + delta, (int)view_.size());
        sel_ = view_[pos];
        host_.redraw();
    }

    void on_click(double x, double y, int btn) {
        if (btn != BTN_LEFT) return;
        const int hh = ov::hdr_h(fs());
        int pad = 16, gap = 8;
        int cw  = std::max(96, (W() - pad * 2 - gap * 3) / 4);
        cols_   = std::max(1, (W() - pad * 2 + gap) / (cw + gap));
        int ch  = cw * 9 / 16 + (show_labels_ ? 18 : 0);
        int y0  = hh + (filterable_ ? ov::search_h(fs()) + 8 : 8);
        int col = (int)((x - pad) / (cw + gap));
        int row = (int)((y - y0) / (ch + gap)) + scroll_;
        int idx = row * cols_ + col;
        if (col < 0 || col >= cols_) return;
        if (idx >= 0 && idx < (int)view_.size()) {
            sel_ = view_[idx];
            apply_sel();
        }
    }

    void on_key(const Bar::KeyEvent& e) {
        if (!e.pressed) return;
        if (e.escape()) {
            cancel("");
            return;
        }
        if (e.enter()) {
            apply_sel();
            return;
        }
        if (e.left()) {
            move(-1);
            return;
        }
        if (e.right()) {
            move(1);
            return;
        }
        if (e.up()) {
            move(-cols_);
            return;
        }
        if (e.down()) {
            move(cols_);
            return;
        }
        if (filterable_ && search_.handle(e)) {
            filter_ = search_.text;
            rebuild_view();
            host_.redraw();
        }
    }

    cairo_surface_t* thumb(const Img& im) {
        auto it = cache_.find(im.path);
        if (it != cache_.end()) return it->second;
        cairo_surface_t* s = load_still(im.thumb.empty() ? im.path : im.thumb, 240);
        cache_[im.path]    = s;
        return s;
    }

    void paint(cairo_t* cr) {
        panel_bg(cr, W(), H());
        select_shell_font(cr, fs());
        const int hh = ov::hdr_h(fs());
        say(cr, 18, hh / 2.0, loading_ ? "Images\u2026" : "Images", cfg.c_fg);
        int y0 = hh + 8;
        if (filterable_) {
            search_.draw(cr, 18, hh, W() - 36, ov::search_h(fs()), "filter");
            y0 = hh + ov::search_h(fs()) + 8;
        }
        if (view_.empty()) {
            say(cr, 18, y0 + 20, "No images", cfg.c_dim);
            return;
        }
        int pad = 16, gap = 8;
        int cw = std::max(96, (W() - pad * 2 - gap * 3) / 4);
        cols_  = std::max(1, (W() - pad * 2 + gap) / (cw + gap));
        int ch = cw * 9 / 16 + (show_labels_ ? 18 : 0);
        int vis_rows = std::max(1, (H() - y0 - 8) / (ch + gap));
        int pos = 0;
        for (int i = 0; i < (int)view_.size(); ++i)
            if (view_[i] == sel_) pos = i;
        int row_of = cols_ ? pos / cols_ : 0;
        ov::keep_visible(row_of, scroll_, vis_rows,
                         (int)((view_.size() + cols_ - 1) / std::max(1, cols_)));
        for (int r = 0; r < vis_rows; ++r) {
            for (int c = 0; c < cols_; ++c) {
                int vi = (scroll_ + r) * cols_ + c;
                if (vi >= (int)view_.size()) break;
                const Img& im = imgs_[view_[vi]];
                double x = pad + c * (cw + gap), y = y0 + r * (ch + gap);
                bool on = view_[vi] == sel_;
                col(cr, on ? cfg.c_accent : cfg.c_ws_bg, on ? 1 : 0.6);
                rrect(cr, x, y, cw, ch - (show_labels_ ? 18 : 0), 6);
                cairo_fill(cr);
                cairo_surface_t* s = thumb(im);
                if (s) {
                    int iw = cairo_image_surface_get_width(s);
                    int ih = cairo_image_surface_get_height(s);
                    if (iw > 0 && ih > 0) {
                        double tw = cw - 6, th = ch - (show_labels_ ? 22 : 6);
                        double sc = std::min(tw / iw, th / ih);
                        cairo_save(cr);
                        cairo_rectangle(cr, x + 3, y + 3, tw, th);
                        cairo_clip(cr);
                        cairo_translate(cr, x + 3 + (tw - iw * sc) / 2.0,
                                        y + 3 + (th - ih * sc) / 2.0);
                        cairo_scale(cr, sc, sc);
                        cairo_set_source_surface(cr, s, 0, 0);
                        cairo_paint(cr);
                        cairo_restore(cr);
                    }
                }
                if (show_labels_ || cfg.shell_image_show_labels) {
                    select_shell_font(cr, fs());
                    say(cr, x + 4, y + ch - 6, utf8_trunc(im.label, 18),
                        cfg.c_dim);
                }
            }
        }
    }
};

// ---------------------------------------------------------------------------
// Wi-Fi QR (header action on the network panel)
// ---------------------------------------------------------------------------
class WifiQrOverlay : public Overlay {
public:
    const char* id() const override { return "omarchy.wifiqr"; }
    void summon(const std::string&) override { open(); }
    void hide() override { host_.close(); }
    bool is_open() const override { return host_.is_open(); }
    ~WifiQrOverlay() override { host_.close(); }

private:
    Host host_;
    AsyncCmd cmd_;
    std::vector<std::string> rows_;
    std::string ssid_, err_;
    double fs() const { return cfg.shell_network_font_size; }

    void open() {
        rows_.clear();
        ssid_.clear();
        err_.clear();
        host_.win.paint = [this](cairo_t* cr) { paint(cr); };
        host_.win.click = [this](double, double, int) {};
        host_.win.pkey  = [this](const Bar::KeyEvent& e) {
            if (e.escape() || e.enter()) host_.close();
        };
        host_.open(360, 400, "mattbar-wifiqr", id());
        auto* sh = mattbar_shell();
        if (!sh || !sh->bar()) return;
        cmd_.run(*sh->bar(), "omarchy-network-qr --meta 2>/dev/null",
                 [this](const std::string& out, int st) {
                     parse(out, st);
                     host_.redraw();
                 },
                 4000);
    }
    void parse(const std::string& out, int st) {
        if (st != 0 && out.empty()) {
            err_ = "No active Wi-Fi connection";
            return;
        }
        std::istringstream ss(out);
        std::string line;
        rows_.clear();
        while (std::getline(ss, line)) {
            if (line.empty()) continue;
            if (line.rfind("meta\t", 0) == 0) {
                auto f = line;
                // meta \t iface \t security \t ssid
                auto a = f.find('\t');
                auto b = f.find('\t', a + 1);
                auto c = f.find('\t', b + 1);
                if (c != std::string::npos) ssid_ = f.substr(c + 1);
                continue;
            }
            if (line[0] == '0' || line[0] == '1') rows_.push_back(line);
        }
        if (ssid_.empty()) ssid_ = "Wi-Fi";
        if (rows_.empty()) err_ = "Could not build a QR code";
    }
    void paint(cairo_t* cr) {
        panel_bg(cr, 360, 400);
        select_shell_font(cr, fs());
        say(cr, 18, 22, ssid_.empty() ? "Wi-Fi QR" : ssid_, cfg.c_fg);
        if (!err_.empty()) {
            say(cr, 18, 60, err_, cfg.c_urgent);
            return;
        }
        if (rows_.empty()) {
            say(cr, 18, 60, "Building QR\u2026", cfg.c_dim);
            return;
        }
        int n = (int)rows_[0].size();
        double pad = 24, top = 48;
        double cell = std::min((360 - pad * 2) / n, (400 - top - pad) / (int)rows_.size());
        double ox = (360 - cell * n) / 2.0, oy = top;
        col(cr, contrast_on(cfg.c_bg), 1);
        cairo_rectangle(cr, ox - 6, oy - 6, cell * n + 12,
                        cell * (int)rows_.size() + 12);
        cairo_fill(cr);
        col(cr, cfg.c_bg.r + cfg.c_bg.g + cfg.c_bg.b > 1.5
                    ? Color{0, 0, 0, 1}
                    : Color{0.05, 0.05, 0.07, 1},
            1);
        for (int y = 0; y < (int)rows_.size(); ++y) {
            for (int x = 0; x < n && x < (int)rows_[y].size(); ++x) {
                if (rows_[y][x] != '1') continue;
                cairo_rectangle(cr, ox + x * cell, oy + y * cell, cell + 0.4,
                                cell + 0.4);
                cairo_fill(cr);
            }
        }
    }
};

// ---------------------------------------------------------------------------
// Internet speed test (header action on the network panel)
// ---------------------------------------------------------------------------
class SpeedTestOverlay : public Overlay {
public:
    const char* id() const override { return "omarchy.speedtest"; }
    void summon(const std::string& payload) override { open(payload); }
    void hide() override { close(); }
    bool is_open() const override { return host_.is_open(); }
    ~SpeedTestOverlay() override { close(); }

private:
    Host host_;
    AsyncCmd cmd_;
    std::string conn_, phase_, down_, up_, err_;
    bool running_ = false;
    double fs() const { return cfg.shell_network_font_size; }

    void close() {
        running_ = false;
        host_.close();
    }
    void open(const std::string& payload) {
        conn_ = ov::json_str(payload, "connection");
        if (conn_.empty()) conn_ = "Network";
        down_.clear();
        up_.clear();
        err_.clear();
        phase_   = "down";
        running_ = true;
        host_.win.paint = [this](cairo_t* cr) { paint(cr); };
        host_.win.pkey  = [this](const Bar::KeyEvent& e) {
            if (e.escape()) close();
            if (e.enter() && !running_) start();
        };
        host_.open(420, 220, "omarchy-speed-test", id());
        start();
    }
    void start() {
        auto* sh = mattbar_shell();
        if (!sh || !sh->bar()) return;
        running_ = true;
        phase_   = "down";
        down_.clear();
        up_.clear();
        err_.clear();
        host_.redraw();
        run_phase("down");
    }
    void run_phase(const char* dir) {
        auto* sh = mattbar_shell();
        if (!sh || !sh->bar() || !running_) return;
        phase_ = dir;
        cmd_.run(*sh->bar(),
                 std::string("timeout 5 omarchy-network-speedtest ") + dir +
                     " 2>/dev/null | tail -1",
                 [this, dir](const std::string& out, int) {
                     if (!running_) return;
                     std::string v = trim(out);
                     if (std::string(dir) == "down") {
                         down_ = v;
                         run_phase("up");
                     } else {
                         up_      = v;
                         phase_.clear();
                         running_ = false;
                     }
                     host_.redraw();
                 },
                 8000);
    }
    void paint(cairo_t* cr) {
        panel_bg(cr, 420, 220);
        select_shell_font(cr, fs());
        say(cr, 18, 24, conn_, cfg.c_fg);
        cairo_set_font_size(cr, std::max(10.0, fs() - 2));
        say(cr, 18, 46, running_ ? (phase_ == "down" ? "MEASURING DOWNLOAD"
                                                     : "MEASURING UPLOAD")
                                 : "DONE  ·  Enter to run again",
            cfg.c_dim);
        cairo_set_font_size(cr, fs() + 10);
        auto dial = [&](double x, const char* lab, const std::string& v,
                        bool live) {
            cairo_set_font_size(cr, std::max(10.0, fs() - 2));
            say(cr, x, 80, lab, live ? cfg.c_accent : cfg.c_dim);
            cairo_set_font_size(cr, fs() + 10);
            std::string n = v.empty() ? "--" : v;
            say(cr, x, 120, n, cfg.c_fg);
            cairo_set_font_size(cr, fs());
            say(cr, x, 148, "Mbps", cfg.c_dim);
        };
        dial(40, "DOWNLOAD", down_, phase_ == "down");
        dial(230, "UPLOAD", up_, phase_ == "up");
        if (!err_.empty()) say(cr, 18, 190, err_, cfg.c_urgent);
    }
};

// ---------------------------------------------------------------------------
// Disk speed test — same two-dial layout as the network overlay, streaming
// `omarchy-disk-speedtest` lines: "disk <model>", "read <MB/s>", "write …".
// ---------------------------------------------------------------------------
class DiskSpeedTestOverlay : public Overlay {
public:
    const char* id() const override { return "omarchy.disk-speedtest"; }
    void summon(const std::string&) override { open(); }
    void hide() override { close(); }
    bool is_open() const override { return host_.is_open(); }
    ~DiskSpeedTestOverlay() override { close(); }

private:
    Host host_;
    AsyncCmd cmd_;
    std::string disk_, read_, write_, phase_, err_;
    bool running_ = false;
    double scale_ = 500;
    struct Hit {
        double x, y, w, h;
    } again_{};

    static constexpr int W = 460, H = 260;
    double fs() const { return cfg.shell_network_font_size; }

    void close() {
        running_ = false;
        cmd_.cancel();
        host_.close();
    }
    void open() {
        host_.win.paint = [this](cairo_t* cr) { paint(cr); };
        host_.win.click = [this](double x, double y, int b) {
            if (b != BTN_LEFT || running_) return;
            if (x >= again_.x && y >= again_.y && x < again_.x + again_.w &&
                y < again_.y + again_.h)
                start();
        };
        host_.win.pkey = [this](const Bar::KeyEvent& e) {
            if (!e.pressed) return;
            if (e.escape()) host_.dismiss();
            if (e.enter() && !running_) start();
        };
        host_.open(W, H, "omarchy-disk-speedtest", id());
        start();
    }
    void start() {
        auto* sh = mattbar_shell();
        if (!sh || !sh->bar()) return;
        running_ = true;
        phase_   = "read";
        disk_.clear();
        read_.clear();
        write_.clear();
        err_.clear();
        scale_ = 500;
        host_.redraw();
        cmd_.run(
            *sh->bar(),
            "if command -v stdbuf >/dev/null; then "
            "exec stdbuf -oL omarchy-disk-speedtest; "
            "else exec omarchy-disk-speedtest; fi",
            [this](const std::string& out, int st) {
                if (!host_.is_open()) return;
                running_ = false;
                phase_.clear();
                if (st != 0 && st != 143 && err_.empty()) {
                    std::string t = trim(out);
                    auto nl = t.rfind('\n');
                    err_ = nl == std::string::npos ? t : t.substr(nl + 1);
                    if (err_.empty()) err_ = "Disk speed test failed";
                }
                host_.redraw();
            },
            45000,
            [this](const std::string& line) { on_line(line); });
    }
    void expand_scale(double v) {
        static const double stops[] = {500, 1000, 2500, 5000, 10000, 15000};
        for (double s : stops) {
            if (v <= s * 0.92) {
                if (s > scale_) scale_ = s;
                return;
            }
        }
        scale_ = 15000;
    }
    void on_line(const std::string& line) {
        if (!running_ || !host_.is_open()) return;
        auto sp = line.find(' ');
        if (sp == std::string::npos) return;
        std::string k = line.substr(0, sp), v = trim(line.substr(sp + 1));
        if (k == "disk") {
            disk_ = v;
        } else if (k == "read") {
            phase_ = "read";
            read_  = v;
            expand_scale(atof(v.c_str()));
        } else if (k == "write") {
            phase_ = "write";
            write_ = v;
            expand_scale(atof(v.c_str()));
        } else
            return;
        host_.redraw();
    }
    void dial(cairo_t* cr, double cx, double cy, const char* lab,
              const std::string& val, bool live) {
        const double r = 54;
        const double start = 0.75 * M_PI;
        const double span  = 1.50 * M_PI;
        cairo_set_line_width(cr, 8);
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
        col(cr, cfg.c_ws_bg, 1);
        cairo_new_sub_path(cr);
        cairo_arc(cr, cx, cy, r, start, start + span);
        cairo_stroke(cr);
        double n = atof(val.c_str());
        double t = std::clamp(scale_ > 0 ? n / scale_ : 0, 0.0, 1.0);
        if (t > 0) {
            col(cr, live ? cfg.c_accent : cfg.c_fg, 1);
            cairo_new_sub_path(cr);
            cairo_arc(cr, cx, cy, r, start, start + span * t);
            cairo_stroke(cr);
        }
        cairo_set_font_size(cr, std::max(10.0, fs() - 2));
        double lw = tw(cr, lab);
        say(cr, cx - lw / 2, cy - 18, lab, live ? cfg.c_accent : cfg.c_dim);
        cairo_set_font_size(cr, fs() + 8);
        std::string shown = val.empty() ? "--" : val;
        double nw = tw(cr, shown);
        say(cr, cx - nw / 2, cy + 8, shown, cfg.c_fg);
        cairo_set_font_size(cr, std::max(10.0, fs() - 2));
        const char* unit = "MB/s";
        say(cr, cx - tw(cr, unit) / 2, cy + 28, unit, cfg.c_dim);
    }
    void paint(cairo_t* cr) {
        panel_bg(cr, W, H);
        select_shell_font(cr, fs());
        std::string title = disk_.empty() ? "DISK" : disk_;
        for (char& c : title)
            if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        cairo_set_font_size(cr, std::max(10.0, fs() - 1));
        double twid = tw(cr, title);
        say(cr, (W - twid) / 2, 22, title, cfg.c_dim);
        cairo_set_font_size(cr, fs());
        dial(cr, 130, 130, "READ", read_, running_ && phase_ == "read");
        dial(cr, 330, 130, "WRITE", write_, running_ && phase_ == "write");
        if (!err_.empty()) {
            say(cr, 18, 236, utf8_trunc(err_, 52), cfg.c_urgent);
            return;
        }
        if (!running_) {
            const char* lab = "Run again";
            double bw = tw(cr, lab) + 20, bh = 24;
            double x = (W - bw) / 2, y = 220;
            col(cr, cfg.c_ws_bg, 1);
            rrect(cr, x, y, bw, bh, 6);
            cairo_fill(cr);
            say(cr, x + 10, y + 12, lab, cfg.c_fg);
            again_ = {x, y, bw, bh};
        } else {
            again_ = {};
            cairo_set_font_size(cr, std::max(10.0, fs() - 2));
            const char* st =
                phase_ == "write" ? "MEASURING WRITE" : "MEASURING READ";
            say(cr, (W - tw(cr, st)) / 2, 236, st, cfg.c_dim);
        }
    }
};

} // namespace

void register_shell_overlays(Shell& sh) {
    sh.add(new ClipboardOverlay);
    sh.add(new EmojiOverlay);
    sh.add(new ImagePickerOverlay);
    sh.add(new WifiQrOverlay);
    sh.add(new SpeedTestOverlay);
    sh.add(new DiskSpeedTestOverlay);
}

std::string image_selector_dispatch(const std::string& rest) {
    auto* sh = mattbar_shell();
    if (!sh) return "error: no shell";
    auto args = ov::split_keep_empty(rest);
    auto at   = [&](size_t i) { return i < args.size() ? args[i] : std::string(); };
    std::string method = at(0);
    if (method == "ping") return "ok";
    if (method == "cancel") {
        std::string done = at(1);
        return sh->call("omarchy.image-picker", "cancel", done);
    }
    if (method == "preload") return "ok";
    if (method == "open") {
        // rest after "open " keeps empty fields
        std::string payload;
        auto sp = rest.find(' ');
        payload = sp == std::string::npos ? std::string() : rest.substr(sp + 1);
        return sh->summon("omarchy.image-picker", payload);
    }
    return "unknown";
}

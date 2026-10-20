// Clipboard history, emoji picker, and image-selector overlays.
#include "overlay.hpp"
#include "util.hpp"

#include "imgwork.hpp"

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
#include <utility>
#include <sstream>
#include <vector>
#include <cstdint>
#include <string>
#include <functional>
#include <algorithm>
#include <cstdlib>

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

void touch_file(const std::string& path) {
    if (path.empty()) return;
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
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
            spawn_helper(std::string("omarchy-clipboard-paste-file ") +
                           (cfg.shell_clipboard_paste ? "" : "--copy-only ") +
                           shell_quote(mime) + " " + shell_quote(c.path));
        } else {
            spawn_helper(std::string("omarchy-clipboard-paste-text ") +
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
            spawn_helper("omarchy-menu-emoji-insert " + shell_quote(e));
        else
            spawn_helper("printf %s " + shell_quote(e) + " | wl-copy");
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

class ImagePickerOverlay;
ImagePickerOverlay* g_picker = nullptr; // the registered instance

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
        if (g_picker == this) g_picker = nullptr;
        on_close_ = nullptr; // shutting down: don't reopen settings
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
        drop_thumbs();
        on_pick_ = nullptr;
        struct After { // settings reopens once the picker has closed
            std::function<void()> f;
            ~After() { if (f) f(); }
        } after{std::move(on_close_)};
        on_close_ = nullptr;
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
        const std::string path = imgs_[idx].path;
        auto cb = std::move(on_pick_);
        auto oc = std::move(on_close_);
        on_pick_  = nullptr;
        on_close_ = nullptr;
        if (!sel_file_.empty())
            write_file(sel_file_, path + "\n");
        if (!done_file_.empty()) touch_file(done_file_);
        sel_file_.clear();
        done_file_.clear();
        drop_thumbs();
        host_.close();
        if (cb) cb(path);
        if (oc) oc();
    }

    int current() const {
        if (sel_ < 0 || sel_ >= (int)imgs_.size()) return -1;
        for (int v : view_)
            if (v == sel_) return sel_;
        return view_.empty() ? -1 : view_[0];
    }

public:
    // Settings' background chooser: the same grid, but the pick goes to
    // `cb` instead of Omarchy's selection/done files.
    void choose(const std::string& rows, const std::string& selected,
                std::function<void(const std::string&)> cb,
                std::function<void()> on_close) {
        if (host_.is_open()) cancel("");
        on_close_ = std::move(on_close);
        dirs_.clear();
        rows_raw_ = rows;
        selected_ = selected;
        sel_file_.clear();
        done_file_.clear();
        show_labels_ = true;
        filterable_  = true;
        on_pick_     = std::move(cb);
        open_parsed();
    }

private:
    std::function<void(const std::string&)> on_pick_;
    std::function<void()>                   on_close_; // pick or cancel

    void open(const std::string& payload) {
        on_pick_  = nullptr; // an Omarchy-driven open never feeds settings
        on_close_ = nullptr;
        parse_payload(payload);
        open_parsed();
    }

    void open_parsed() {
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

    // Thumbnails decode on the image worker: paint() only ever draws what
    // is ready (the tile shows its placeholder until then). Opening a
    // folder of 4K wallpapers used to freeze the bar for seconds here.
    static constexpr int kThumbEdge = 240;
    std::map<std::string, uint64_t> thumb_jobs_;
    cairo_surface_t* thumb(const Img& im) {
        auto it = cache_.find(im.path);
        if (it != cache_.end()) return it->second;
        if (thumb_jobs_.count(im.path)) return nullptr;
        auto* sh = mattbar_shell();
        const std::string src = im.thumb.empty() ? im.path : im.thumb;
        if (!sh || !sh->bar()) {
            return cache_[im.path] =
                       img_decode_now(src, {kThumbEdge, kThumbEdge, ImgTarget::Fit});
        }
        const std::string key = im.path;
        thumb_jobs_[key] = img_submit(
            *sh->bar(), {{src, {{kThumbEdge, kThumbEdge, ImgTarget::Fit}}}},
            [this, key](ImgResult& r) {
                thumb_jobs_.erase(key);
                cache_[key] = std::exchange(r.surfs[0][0], nullptr);
                if (host_.is_open()) host_.redraw();
            });
        return nullptr;
    }
    // Closing forgets queued decodes and the cache (100+ thumbnails of a
    // big folder are tens of MB held for a picker that isn't open).
    void drop_thumbs() {
        for (auto& [_, t] : thumb_jobs_) img_cancel(t);
        thumb_jobs_.clear();
        for (auto& [_, s] : cache_)
            if (s) cairo_surface_destroy(s);
        cache_.clear();
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

// ---------------------------------------------------------------------------
// Update preview. The bar chip opens this instead of starting omarchy-update.
// The lookup is read-only: checkupdates, yay -Qua, mise outdated, and the
// Omarchy release notes. Update at the bottom is what launches the updater.
// ---------------------------------------------------------------------------
struct UpPkg {
    std::string name, from, to;
};

bool split_arrow(const std::string& line, UpPkg& p) {
    auto arrow = line.find(" -> ");
    if (arrow == std::string::npos) return false;
    std::string left = trim(line.substr(0, arrow));
    p.to = trim(line.substr(arrow + 4));
    auto sp = left.find_last_of(' ');
    if (sp == std::string::npos || p.to.empty()) return false;
    p.name = trim(left.substr(0, sp));
    p.from = trim(left.substr(sp + 1));
    return !p.name.empty() && !p.from.empty();
}

class UpdatePreviewOverlay : public Overlay {
public:
    const char* id() const override { return "mattbar.update"; }
    void summon(const std::string&) override { open(); }
    void hide() override { close(); }
    bool is_open() const override { return host_.is_open(); }

private:
    Host host_;
    AsyncCmd cmd_;
    std::vector<UpPkg> pkgs_, aur_, mise_;
    std::vector<std::string> notes_, commits_, orphans_;
    UpPkg omarchy_{};
    bool have_omarchy_ = false;
    bool loading_ = false;
    int gen_ = 0, scroll_ = 0, nlines_ = 0, max_scroll_ = 0;
    double btn_x_ = 0, btn_y_ = 0, btn_w_ = 0, btn_h_ = 0;

    int W() const { return std::max(cfg.shell_panel_width, 420); }
    int H() const { return std::max(cfg.shell_panel_height, 560); }
    double fs() const { return std::max(11.0, cfg.shell_network_font_size); }

    void close() {
        ++gen_;
        cmd_.cancel();
        host_.close();
    }

    void open() {
        scroll_ = 0;
        loading_ = true;
        have_omarchy_ = false;
        omarchy_ = {};
        pkgs_.clear();
        aur_.clear();
        mise_.clear();
        notes_.clear();
        commits_.clear();
        orphans_.clear();
        host_.win.paint = [this](cairo_t* cr) { paint(cr); };
        host_.win.click = [this](double x, double y, int b) { on_click(x, y, b); };
        host_.win.pscroll = [this](int d) {
            scroll_ = std::clamp(scroll_ + d, 0, max_scroll_);
            host_.redraw();
        };
        host_.win.pkey = [this](const Bar::KeyEvent& e) { on_key(e); };
        host_.open(W(), H(), "mattbar-update", id(), true, Host::Place::BarEnd);
        load();
    }

    void load() {
        auto* sh = mattbar_shell();
        if (!sh || !sh->bar()) return;
        int g = ++gen_;
        cmd_.run(*sh->bar(), preview_script(),
                 [this, g](const std::string& out, int) {
                     if (g != gen_) return;
                     parse(out);
                     loading_ = false;
                     if (host_.is_open()) host_.redraw();
                 },
                 45000);
    }

    static const char* preview_script() {
        return R"UPD(exec 2>/dev/null
set +e
tmp=$(mktemp)
checkupdates --nocolor >"$tmp" 2>/dev/null
printf '\036PACKAGES\n'
cat "$tmp"
printf '\036OMARCHY\n'
pkg=
if pacman -Qq omarchy-dev >/dev/null 2>&1; then pkg=omarchy-dev
elif pacman -Qq omarchy >/dev/null 2>&1; then pkg=omarchy
fi
line=
if [[ -n $pkg ]]; then
  line=$(awk -v p="$pkg" '$1 == p { print; exit }' "$tmp")
fi
printf '%s\n' "$line"
printf '\036NOTES\n'
ver=$(printf '%s\n' "$line" | awk '{ print $NF }')
ver=${ver#*:}
ver=${ver%-*}
OMARCHY_REL=$ver python3 - <<'PY'
import json, os, re, urllib.request
ver = os.environ.get("OMARCHY_REL", "").strip()
if not ver:
    raise SystemExit(0)
link = re.compile(r"\[([^\]]+)\]\([^)]+\)")
def flatten(body):
    out = []
    fence = False
    for raw in body.splitlines():
        s = raw.strip()
        if s.startswith("```"):
            fence = not fence
            continue
        if s.startswith("![") or s.startswith("<img"):
            continue
        s = link.sub(r"\1", s)
        s = s.replace("**", "").replace("__", "")
        s = re.sub(r"^#{1,6}\s*", "", s)
        out.append(s)
    while out and not out[0].strip():
        out.pop(0)
    while out and not out[-1].strip():
        out.pop()
    print("\n".join(out))
for repo in ("basecamp/omarchy", "omacom/omarchy"):
    for tag in ("v" + ver, ver):
        url = "https://api.github.com/repos/%s/releases/tags/%s" % (repo, tag)
        req = urllib.request.Request(url, headers={
            "User-Agent": "mattbar",
            "Accept": "application/vnd.github+json",
        })
        try:
            with urllib.request.urlopen(req, timeout=8) as r:
                body = json.load(r).get("body") or ""
        except Exception:
            continue
        if body.strip():
            flatten(body)
            raise SystemExit(0)
PY
printf '\036GIT\n'
path=${OMARCHY_PATH:-/usr/share/omarchy}
if [[ $path != /usr/share/omarchy ]] && git -C "$path" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  upstream=$(git -C "$path" rev-parse --abbrev-ref --symbolic-full-name '@{upstream}' 2>/dev/null || true)
  if [[ -n $upstream ]]; then
    timeout 8 git -C "$path" fetch --quiet >/dev/null 2>&1 || true
    git -C "$path" log --pretty='%s' "HEAD..$upstream" 2>/dev/null | head -30
  fi
fi
printf '\036AUR\n'
if command -v yay >/dev/null 2>&1; then
  timeout 20 yay -Qua 2>/dev/null | sed -E 's/\x1b\[[0-9;]*[mK]//g'
fi
printf '\036MISE\n'
if command -v mise >/dev/null 2>&1; then
  MISE_MINIMUM_RELEASE_AGE=0 timeout 20 mise outdated --json 2>/dev/null | python3 -c '
import json,sys
raw=sys.stdin.read().strip()
if not raw:
    raise SystemExit(0)
data=json.loads(raw)
items=data.values() if isinstance(data, dict) else data
for t in items:
    if not isinstance(t, dict):
        continue
    name=t.get("name") or ""
    cur=str(t.get("current") or "")
    lat=str(t.get("latest") or "")
    if name and lat and lat != cur:
        print("%s %s -> %s" % (name, cur, lat))
'
fi
printf '\036ORPHANS\n'
pacman -Qtdq 2>/dev/null || true
printf '\036END\n'
rm -f "$tmp"
)UPD";
    }

    void parse(const std::string& out) {
        pkgs_.clear();
        aur_.clear();
        mise_.clear();
        notes_.clear();
        commits_.clear();
        orphans_.clear();
        have_omarchy_ = false;
        omarchy_ = {};
        std::string sec;
        int blanks = 0;
        std::istringstream ss(out);
        std::string line;
        while (std::getline(ss, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty() && line[0] == '\036') {
                sec = line.substr(1);
                blanks = 0;
                continue;
            }
            if (sec == "PACKAGES" || sec == "AUR" || sec == "MISE") {
                UpPkg p;
                if (!split_arrow(line, p)) continue;
                if (sec == "PACKAGES") pkgs_.push_back(std::move(p));
                else if (sec == "AUR") aur_.push_back(std::move(p));
                else mise_.push_back(std::move(p));
            } else if (sec == "OMARCHY") {
                if (!have_omarchy_) {
                    have_omarchy_ = split_arrow(line, omarchy_);
                }
            } else if (sec == "NOTES") {
                if (notes_.size() >= 80) continue;
                if (trim(line).empty()) {
                    if (blanks++) continue;
                    notes_.push_back({});
                } else {
                    blanks = 0;
                    notes_.push_back(line);
                }
            } else if (sec == "GIT") {
                if (commits_.size() < 30 && !trim(line).empty())
                    commits_.push_back(trim(line));
            } else if (sec == "ORPHANS") {
                std::string n = trim(line);
                if (!n.empty() && n.find(' ') == std::string::npos)
                    orphans_.push_back(n);
            }
        }
        if (notes_.size() >= 80) notes_.push_back("…");
    }

    struct VRow {
        int kind; // 0 section, 1 package, 2 note, 3 gap
        std::string a, b;
        double h = 0;
    };

    static void add_note(std::vector<VRow>& rows, cairo_t* cr,
                         const std::string& text, double maxw, double nh) {
        if (trim(text).empty()) {
            if (!rows.empty() && rows.back().kind != 3)
                rows.push_back({3, {}, {}, 8});
            return;
        }
        std::string cur, word;
        auto emit = [&](const std::string& s) {
            rows.push_back({2, s, {}, nh});
        };
        for (size_t i = 0; i <= text.size(); ++i) {
            char c = i == text.size() ? ' ' : text[i];
            if (c == ' ' || c == '\t') {
                if (word.empty()) continue;
                std::string trial = cur.empty() ? word : cur + " " + word;
                if (tw(cr, trial) <= maxw) cur = std::move(trial);
                else {
                    if (!cur.empty()) emit(cur);
                    if (tw(cr, word) <= maxw) cur = word;
                    else {
                        emit(utf8_trunc(word, 48));
                        cur.clear();
                    }
                }
                word.clear();
            } else word.push_back(c);
        }
        if (!cur.empty()) emit(cur);
    }

    std::vector<VRow> layout(cairo_t* cr) const {
        std::vector<VRow> rows;
        const double nh = std::max(16.0, fs() + 4);
        const double ih = (double)ov::row_h(fs());
        const double maxw = W() - 36;
        auto section = [&](const char* t) {
            rows.push_back({0, t, {}, 22});
        };
        auto item = [&](const std::string& name, const std::string& ver) {
            rows.push_back({1, name, ver, ih});
        };
        if (have_omarchy_ || !notes_.empty()) {
            section("OMARCHY");
            if (have_omarchy_)
                item(omarchy_.name, omarchy_.from + "  →  " + omarchy_.to);
            if (notes_.empty())
                add_note(rows, cr, "Release notes were not available.", maxw, nh);
            else
                for (const auto& n : notes_) add_note(rows, cr, n, maxw, nh);
        }
        if (!commits_.empty()) {
            section("DEV CHECKOUT");
            for (const auto& c : commits_) add_note(rows, cr, c, maxw, nh);
        }
        std::vector<UpPkg> sys;
        for (const auto& p : pkgs_) {
            if (have_omarchy_ && p.name == omarchy_.name) continue;
            sys.push_back(p);
        }
        if (!sys.empty()) {
            section("SYSTEM PACKAGES");
            for (const auto& p : sys)
                item(p.name, p.from + "  →  " + p.to);
        }
        if (!aur_.empty()) {
            section("AUR");
            for (const auto& p : aur_)
                item(p.name, p.from + "  →  " + p.to);
        }
        if (!mise_.empty()) {
            section("MISE TOOLS");
            for (const auto& p : mise_)
                item(p.name, p.from + "  →  " + p.to);
        }
        if (!orphans_.empty()) {
            section("ORPHANS");
            add_note(rows, cr,
                     "You will be asked about these after the update. "
                     "Nothing is removed unless you say yes.",
                     maxw, nh);
            for (const auto& n : orphans_) item(n, {});
        }
        if (!loading_ && rows.empty())
            add_note(rows, cr, "Nothing is waiting.", maxw, nh);
        return rows;
    }

    void on_click(double x, double y, int btn) {
        if (btn != BTN_LEFT) return;
        if (x < btn_x_ || y < btn_y_ || x >= btn_x_ + btn_w_ || y >= btn_y_ + btn_h_)
            return;
        close();
        update_launch();
    }

    void on_key(const Bar::KeyEvent& e) {
        if (!e.pressed) return;
        if (e.escape()) {
            close();
            return;
        }
        if (e.up()) {
            if (scroll_ > 0) --scroll_;
            host_.redraw();
        } else if (e.down()) {
            if (scroll_ < max_scroll_) ++scroll_;
            host_.redraw();
        }
    }

    void paint(cairo_t* cr) {
        panel_bg(cr, W(), H());
        select_shell_font(cr, fs());
        btn_x_ = btn_y_ = btn_w_ = btn_h_ = 0;
        cairo_set_font_size(cr, fs() + 1);
        say(cr, 18, 20, "Updates", cfg.c_fg);
        cairo_set_font_size(cr, std::max(10.0, fs() - 2));
        std::string sub = loading_ ? "Looking up updates\u2026"
                                   : "Nothing is installed until you press Update";
        say(cr, 18, 38, sub, cfg.c_dim);
        cairo_set_font_size(cr, fs());

        const double top = 56;
        const double foot = 52;
        const double bottom = H() - foot;
        auto rows = layout(cr);
        nlines_ = (int)rows.size();
        {
            double view = bottom - top;
            double acc = 0;
            max_scroll_ = 0;
            for (int i = nlines_ - 1; i >= 0; --i) {
                if (acc + rows[i].h > view && acc > 0) {
                    max_scroll_ = i + 1;
                    break;
                }
                acc += rows[i].h;
            }
            if (max_scroll_ >= nlines_) max_scroll_ = std::max(0, nlines_ - 1);
        }
        if (scroll_ > max_scroll_) scroll_ = max_scroll_;
        if (scroll_ < 0) scroll_ = 0;

        double y = top;
        if (loading_ && rows.empty()) {
            say(cr, 18, top + 16, "Looking up updates\u2026", cfg.c_dim);
        }
        for (int i = scroll_; i < nlines_ && y < bottom; ++i) {
            const VRow& r = rows[i];
            if (y + r.h > bottom && i > scroll_) break;
            if (r.kind == 0) {
                cairo_set_font_size(cr, std::max(10.0, fs() - 2));
                say(cr, 18, y + r.h / 2.0, r.a, cfg.c_accent);
                cairo_set_font_size(cr, fs());
            } else if (r.kind == 1) {
                say(cr, 18, y + r.h / 2.0, utf8_trunc(r.a, 28), cfg.c_fg);
                if (!r.b.empty()) {
                    cairo_set_font_size(cr, std::max(10.0, fs() - 1));
                    std::string ver = utf8_trunc(r.b, 32);
                    say(cr, W() - 18 - tw(cr, ver), y + r.h / 2.0, ver, cfg.c_dim);
                    cairo_set_font_size(cr, fs());
                }
            } else if (r.kind == 2) {
                cairo_set_font_size(cr, std::max(10.0, fs() - 2));
                say(cr, 18, y + r.h / 2.0, r.a, cfg.c_dim);
                cairo_set_font_size(cr, fs());
            }
            y += r.h;
        }

        col(cr, cfg.c_ws_bg, 1);
        cairo_rectangle(cr, 16, H() - foot + 4, W() - 32, 1);
        cairo_fill(cr);
        const char* lab = "Update";
        btn_h_ = 26;
        btn_w_ = tw(cr, lab) + 22;
        btn_x_ = W() - 16 - btn_w_;
        btn_y_ = H() - 16 - btn_h_;
        col(cr, cfg.c_accent, 1);
        rrect(cr, btn_x_, btn_y_, btn_w_, btn_h_, 6);
        cairo_fill(cr);
        say(cr, btn_x_ + 11, btn_y_ + btn_h_ / 2.0, lab, contrast_on(cfg.c_accent));
        cairo_set_font_size(cr, std::max(10.0, fs() - 2));
        const char* hint = "Confirms in the terminal";
        if (tw(cr, hint) < btn_x_ - 28)
            say(cr, 18, btn_y_ + btn_h_ / 2.0, hint, cfg.c_dim);
        cairo_set_font_size(cr, fs());
    }
};

} // namespace

void register_shell_overlays(Shell& sh) {
    sh.add(new ClipboardOverlay);
    sh.add(new EmojiOverlay);
    sh.add(g_picker = new ImagePickerOverlay);
    sh.add(new WifiQrOverlay);
    sh.add(new SpeedTestOverlay);
    sh.add(new DiskSpeedTestOverlay);
    sh.add(new UpdatePreviewOverlay);
}

bool image_picker_choose(const std::string& rows, const std::string& selected,
                         std::function<void(const std::string&)> cb,
                         std::function<void()> on_close) {
    auto* sh = mattbar_shell();
    if (!sh || !sh->bar()) return false;
    if (!g_picker) return false;
    g_picker->choose(rows, selected, std::move(cb), std::move(on_close));
    return true;
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

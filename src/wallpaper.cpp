#include "wallpaper.hpp"
#include "bar.hpp"
#include "config.hpp"
#include "frac.hpp"
#include "shm.hpp"

#include "wlr-layer-shell-unstable-v1-client-protocol.h"

#include <cairo/cairo.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_PSD
#define STBI_NO_PIC
#define STBI_NO_PNM
#include "stb_image.h"

namespace {

Bar*              g_bar  = nullptr;
cairo_surface_t*  g_img  = nullptr; // current (incoming during a wipe)
cairo_surface_t*  g_old  = nullptr; // previous, while revealProgress < 1
std::string       g_path;
double            g_progress = 1.0; // 0..1 wipe; 1 = settled
int               g_anim_fd  = -1;
uint64_t          g_anim_t0  = 0;
constexpr int     kAnimMs    = 420;

uint64_t mono_ms() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

double ease_inout_cubic(double t) {
    t = std::clamp(t, 0.0, 1.0);
    return t < 0.5 ? 4.0 * t * t * t
                   : 1.0 - std::pow(-2.0 * t + 2.0, 3.0) / 2.0;
}

struct Surf {
    wl_output*             out  = nullptr;
    std::string            name;
    wl_surface*            surf = nullptr;
    zwlr_layer_surface_v1* ls   = nullptr;
    FracSurface            frac;
    int                    w = 0, h = 0;
    bool                   configured = false;
    std::string            path;
    cairo_surface_t*       img = nullptr;
    cairo_surface_t*       old = nullptr;
};

std::vector<Surf*> surfs;

void draw_one(Surf& s);

std::string background_path() {
    const char* h = getenv("HOME");
    if (!h || !*h) return {};
    const char* xdg = getenv("XDG_STATE_HOME");
    std::string link = xdg && *xdg
                           ? std::string(xdg) + "/omarchy/current/background"
                           : std::string(h) + "/.local/state/omarchy/current/background";
    char buf[512];
    ssize_t n = readlink(link.c_str(), buf, sizeof buf - 1);
    if (n <= 0) return access(link.c_str(), R_OK) == 0 ? link : std::string();
    buf[n] = 0;
    if (buf[0] == '/') return buf;
    auto slash = link.rfind('/');
    return (slash == std::string::npos ? std::string() : link.substr(0, slash + 1)) +
           buf;
}

bool is_image_name(const char* n) {
    const char* dot = strrchr(n, '.');
    if (!dot || !dot[1]) return false;
    std::string e = dot + 1;
    for (char& c : e) c = (char)tolower((unsigned char)c);
    return e == "jpg" || e == "jpeg" || e == "png" || e == "webp" ||
           e == "bmp" || e == "tif" || e == "tiff";
}

void add_dir_images(std::vector<std::string>& out, const std::string& dir) {
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    std::vector<std::string> names;
    while (dirent* e = readdir(d)) {
        if (e->d_name[0] == '.' || !is_image_name(e->d_name)) continue;
        names.push_back(e->d_name);
    }
    closedir(d);
    std::sort(names.begin(), names.end());
    for (auto& n : names) {
        std::string p = dir + "/" + n;
        char* real = realpath(p.c_str(), nullptr);
        if (real) {
            p = real;
            free(real);
        }
        if (std::find(out.begin(), out.end(), p) == out.end())
            out.push_back(p);
    }
}

std::string theme_name() {
    const char* h = getenv("HOME");
    if (!h || !*h) return {};
    const char* xdg = getenv("XDG_STATE_HOME");
    std::string p = xdg && *xdg
                        ? std::string(xdg) + "/omarchy/current/theme.name"
                        : std::string(h) + "/.local/state/omarchy/current/theme.name";
    FILE* f = fopen(p.c_str(), "r");
    if (!f) return {};
    char buf[256];
    if (!fgets(buf, sizeof buf, f)) {
        fclose(f);
        return {};
    }
    fclose(f);
    std::string s = buf;
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s;
}

std::vector<std::string> background_pool() {
    std::vector<std::string> pool;
    std::string cur = background_path();
    if (!cur.empty()) {
        char* real = realpath(cur.c_str(), nullptr);
        pool.push_back(real ? real : cur);
        if (real) free(real);
    }
    const char* h = getenv("HOME");
    if (!h || !*h) return pool;
    const char* xdg = getenv("XDG_STATE_HOME");
    std::string state = xdg && *xdg ? std::string(xdg) : std::string(h) + "/.local/state";
    add_dir_images(pool, state + "/omarchy/current/theme/backgrounds");
    std::string tn = theme_name();
    if (!tn.empty())
        add_dir_images(pool, std::string(h) + "/.config/omarchy/backgrounds/" + tn);
    return pool;
}

std::string path_for_output(const std::string&, size_t index,
                            const std::vector<std::string>& pool) {
    if (pool.empty()) return {};
    if (index == 0 || pool.size() == 1) return pool[0];
    // Extra monitors get other images from the theme set, wrapping if
    // there are fewer files than outputs. Index 0 always keeps the
    // current-background symlink so the switcher still drives the
    // "main" display.
    return pool[1 + (index - 1) % (pool.size() - 1)];
}

cairo_surface_t* load_image(const std::string& path) {
    if (path.empty()) return nullptr;
    int w = 0, h = 0, n = 0;
    unsigned char* px = stbi_load(path.c_str(), &w, &h, &n, 4);
    if (!px || w <= 0 || h <= 0) {
        if (px) stbi_image_free(px);
        return nullptr;
    }
    cairo_surface_t* cs =
        cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    unsigned char* dst = cairo_image_surface_get_data(cs);
    int stride         = cairo_image_surface_get_stride(cs);
    for (int y = 0; y < h; ++y) {
        auto* row = reinterpret_cast<uint32_t*>(dst + y * stride);
        unsigned char* src = px + y * w * 4;
        for (int x = 0; x < w; ++x) {
            unsigned char r = src[x * 4 + 0], g = src[x * 4 + 1],
                          b = src[x * 4 + 2], a = src[x * 4 + 3];
            row[x] = ((uint32_t)a << 24) | ((uint32_t)r << 16) |
                     ((uint32_t)g << 8) | b;
        }
    }
    cairo_surface_mark_dirty(cs);
    stbi_image_free(px);
    return cs;
}

void paint_img(cairo_t* cr, int dw, int dh, cairo_surface_t* img) {
    if (!img) return;
    int sw = cairo_image_surface_get_width(img);
    int sh = cairo_image_surface_get_height(img);
    if (sw <= 0 || sh <= 0) return;
    double sx = (double)dw / sw, sy = (double)dh / sh;
    double s  = sx > sy ? sx : sy;
    double ox = (dw - sw * s) / 2.0, oy = (dh - sh * s) / 2.0;
    cairo_save(cr);
    cairo_translate(cr, ox, oy);
    cairo_scale(cr, s, s);
    cairo_set_source_surface(cr, img, 0, 0);
    cairo_paint(cr);
    cairo_restore(cr);
}

// Omarchy's background reveal: a slanted band that opens from the
// centre. Same geometry as Background.qml's ShapePath mask.
void paint_wipe_incoming(cairo_t* cr, int dw, int dh, double p,
                         cairo_surface_t* img) {
    if (!img || p <= 0) return;
    if (p >= 1) {
        paint_img(cr, dw, dh, img);
        return;
    }
    const double slant        = -0.18;
    const double center_top   = dw / 2.0 - slant * dh / 2.0;
    const double center_bot   = dw / 2.0 + slant * dh / 2.0;
    const double reach        = dw / 2.0 + std::fabs(slant) * dh / 2.0 + 4.0;
    const double spread       = reach * p;
    cairo_save(cr);
    cairo_move_to(cr, center_top - spread, 0);
    cairo_line_to(cr, center_top + spread, 0);
    cairo_line_to(cr, center_bot + spread, dh);
    cairo_line_to(cr, center_bot - spread, dh);
    cairo_close_path(cr);
    cairo_clip(cr);
    paint_img(cr, dw, dh, img);
    cairo_restore(cr);
}

void paint_cover(cairo_t* cr, int dw, int dh, Surf& s) {
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, cfg.c_bg.r, cfg.c_bg.g, cfg.c_bg.b, 1);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    cairo_surface_t* base =
        (s.old && g_progress < 1.0) ? s.old : (s.img ? s.img : g_img);
    paint_img(cr, dw, dh, base);
    if (s.old && s.img && g_progress < 1.0)
        paint_wipe_incoming(cr, dw, dh, g_progress, s.img);
}

void redraw_all() {
    for (auto* s : surfs) draw_one(*s);
}

void stop_anim() {
    if (g_anim_fd < 0) return;
    itimerspec off{};
    timerfd_settime(g_anim_fd, 0, &off, nullptr);
}

void finish_anim() {
    g_progress = 1.0;
    stop_anim();
    if (g_old) {
        cairo_surface_destroy(g_old);
        g_old = nullptr;
    }
    for (auto* s : surfs) {
        if (s->old) {
            cairo_surface_destroy(s->old);
            s->old = nullptr;
        }
    }
    redraw_all();
}

void tick_anim() {
    if (!g_old || g_progress >= 1.0) {
        finish_anim();
        return;
    }
    double t = (double)(mono_ms() - g_anim_t0) / (double)kAnimMs;
    if (t >= 1.0) {
        finish_anim();
        return;
    }
    g_progress = ease_inout_cubic(t);
    redraw_all();
}

void ensure_anim_fd() {
    if (g_anim_fd >= 0 || !g_bar) return;
    g_anim_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    if (g_anim_fd < 0) return;
    g_bar->add_fd(
        g_anim_fd,
        [](uint32_t) {
            uint64_t x;
            while (read(g_anim_fd, &x, sizeof x) > 0) {}
            tick_anim();
        },
        "wallpaper-anim");
}

void start_anim() {
    ensure_anim_fd();
    g_anim_t0  = mono_ms();
    g_progress = 0.0;
    if (g_anim_fd < 0) {
        finish_anim();
        return;
    }
    itimerspec ts{};
    ts.it_interval.tv_nsec = 16 * 1000000L;
    ts.it_value.tv_nsec    = 16 * 1000000L;
    timerfd_settime(g_anim_fd, 0, &ts, nullptr);
}

void assign_images(bool instant) {
    auto pool = background_pool();
    if (!g_path.empty()) {
        char* real = realpath(g_path.c_str(), nullptr);
        std::string cur = real ? real : g_path;
        if (real) free(real);
        auto it = std::find(pool.begin(), pool.end(), cur);
        if (it != pool.end() && it != pool.begin())
            std::rotate(pool.begin(), it, it + 1);
        else if (it == pool.end())
            pool.insert(pool.begin(), cur);
        g_path = cur;
    }
    std::vector<Surf*> order = surfs;
    std::sort(order.begin(), order.end(), [](Surf* a, Surf* b) {
        if (a->name.empty() != b->name.empty())
            return b->name.empty();
        return a->name < b->name;
    });
    if (!cfg.output.empty()) {
        for (size_t i = 1; i < order.size(); ++i)
            if (order[i]->name == cfg.output) {
                std::swap(order[0], order[i]);
                break;
            }
    }
    bool wipe = false;
    for (size_t i = 0; i < order.size(); ++i) {
        Surf& s = *order[i];
        std::string p = path_for_output(s.name, i, pool);
        if (p == s.path && s.img) continue;
        cairo_surface_t* next = p.empty() ? nullptr : load_image(p);
        if (s.old) {
            cairo_surface_destroy(s.old);
            s.old = nullptr;
        }
        if (instant || !s.img) {
            if (s.img) cairo_surface_destroy(s.img);
            s.img  = next;
            s.path = p;
        } else {
            s.old  = s.img;
            s.img  = next;
            s.path = p;
            if (s.old && s.img) wipe = true;
        }
    }
    if (wipe) start_anim();
}

void snap_to(cairo_surface_t* next, const std::string& path) {
    stop_anim();
    if (g_old) {
        cairo_surface_destroy(g_old);
        g_old = nullptr;
    }
    if (g_img) cairo_surface_destroy(g_img);
    g_img      = next;
    g_path     = path;
    g_progress = 1.0;
    assign_images(true);
    redraw_all();
}

void show_path(const std::string& path, bool instant,
               const std::string& from_path) {
    if (path.empty()) {
        snap_to(nullptr, {});
        return;
    }
    if (path == g_path && g_img && from_path.empty()) {
        if (instant && g_old) finish_anim();
        assign_images(instant);
        redraw_all();
        return;
    }
    cairo_surface_t* next = load_image(path);
    if (!next) {
        fprintf(stderr, "mattbar: wallpaper: could not decode %s\n",
                path.c_str());
        if (!g_img) g_path = path;
        redraw_all();
        return;
    }
    if (instant || !g_img) {
        snap_to(next, path);
        return;
    }
    if (g_old) cairo_surface_destroy(g_old);
    if (!from_path.empty() && from_path != g_path) {
        cairo_surface_t* from = load_image(from_path);
        g_old = from ? from : g_img;
        if (from) {
            // keep the live image until the wipe replaces it
        } else {
            g_img = nullptr; // ownership moved into g_old
        }
        if (g_img && g_old != g_img) {
            cairo_surface_destroy(g_img);
            g_img = nullptr;
        }
    } else {
        g_old = g_img;
        g_img = nullptr;
    }
    g_img  = next;
    g_path = path;
    start_anim();
    assign_images(false);
    redraw_all();
}

void draw_one(Surf& s) {
    if (!s.surf || !s.configured || s.w <= 0 || s.h <= 0 || !g_bar) return;
    int sc = g_bar->scale_of(s.out);
    if (sc < 1) sc = 1;
    const int bw = s.frac.active() ? s.frac.px(s.w) : s.w * sc;
    const int bh = s.frac.active() ? s.frac.px(s.h) : s.h * sc;
    void* data = nullptr;
    wl_buffer* buf = create_argb_buffer(g_bar->shm(), bw, bh, &data);
    if (!buf) return;
    cairo_surface_t* cs = cairo_image_surface_create_for_data(
        static_cast<unsigned char*>(data), CAIRO_FORMAT_ARGB32, bw, bh, bw * 4);
    cairo_t* cr = cairo_create(cs);
    cairo_scale(cr, (double)bw / s.w, (double)bh / s.h);
    paint_cover(cr, s.w, s.h, s);
    cairo_destroy(cr);
    cairo_surface_destroy(cs);
    s.frac.apply(s.surf, s.w, s.h, sc);
    wl_surface_attach(s.surf, buf, 0, 0);
    if (wl_surface_get_version(s.surf) >= 4)
        wl_surface_damage_buffer(s.surf, 0, 0, bw, bh);
    else
        wl_surface_damage(s.surf, 0, 0, s.w, s.h);
    wl_surface_commit(s.surf);
}

void on_configure(void* data, zwlr_layer_surface_v1* ls, uint32_t serial,
                  uint32_t w, uint32_t h) {
    auto* s = static_cast<Surf*>(data);
    zwlr_layer_surface_v1_ack_configure(ls, serial);
    if (w) s->w = (int)w;
    if (h) s->h = (int)h;
    s->configured = true;
    draw_one(*s);
}

void on_closed(void* data, zwlr_layer_surface_v1*) {
    auto* s = static_cast<Surf*>(data);
    s->configured = false;
}

void destroy_one(Surf& s) {
    if (s.img) {
        cairo_surface_destroy(s.img);
        s.img = nullptr;
    }
    if (s.old) {
        cairo_surface_destroy(s.old);
        s.old = nullptr;
    }
    s.path.clear();
    if (!s.surf) return;
    if (g_bar) g_bar->unregister_surface(s.surf);
    if (s.ls) zwlr_layer_surface_v1_destroy(s.ls);
    s.frac.destroy();
    wl_surface_destroy(s.surf);
    s.surf = nullptr;
    s.ls   = nullptr;
}

void destroy_all() {
    for (auto* s : surfs) {
        destroy_one(*s);
        delete s;
    }
    surfs.clear();
}

void create_one(Bar::OutputRef o) {
    if (!g_bar || !g_bar->compositor() || !g_bar->layer_shell() || !o.wl)
        return;
    auto* s  = new Surf;
    s->out   = o.wl;
    s->name  = o.name;
    s->surf  = wl_compositor_create_surface(g_bar->compositor());
    s->ls    = zwlr_layer_shell_v1_get_layer_surface(
        g_bar->layer_shell(), s->surf, o.wl,
        ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND, "mattbar-wallpaper");
    static const zwlr_layer_surface_v1_listener lst = {
        .configure = on_configure,
        .closed    = on_closed,
    };
    zwlr_layer_surface_v1_add_listener(s->ls, &lst, s);
    s->frac.on_change = [s] {
        if (s->configured) draw_one(*s);
    };
    s->frac.attach(g_bar->frac_mgr(), g_bar->viewporter(), s->surf);
    zwlr_layer_surface_v1_set_anchor(
        s->ls, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
                   ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
                   ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
                   ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    zwlr_layer_surface_v1_set_exclusive_zone(s->ls, -1);
    zwlr_layer_surface_v1_set_size(s->ls, 0, 0);
    zwlr_layer_surface_v1_set_keyboard_interactivity(s->ls, 0);
    surfs.push_back(s);
    g_bar->register_surface(s->surf, {});
    wl_surface_commit(s->surf);
}

} // namespace

void wallpaper_init(Bar& bar) { g_bar = &bar; }

cairo_surface_t* wallpaper_image() { return g_img; }

cairo_surface_t* image_load_file(const std::string& path) {
    return load_image(path);
}

void wallpaper_refresh() { show_path(background_path(), false, {}); }

void wallpaper_set(const std::string& path, bool instant) {
    show_path(path, instant, {});
}

void wallpaper_transition(const std::string& from_path,
                          const std::string& path) {
    show_path(path, false, from_path);
}

void wallpaper_apply() {
    if (!g_bar) return;
    if (!cfg.quickshell_shutdown) {
        destroy_all();
        return;
    }
    wallpaper_refresh();
    auto outs = g_bar->output_list();
    // Recreate if the output set changed.
    bool same = surfs.size() == outs.size();
    if (same) {
        for (size_t i = 0; i < outs.size(); ++i)
            if (surfs[i]->out != outs[i].wl) {
                same = false;
                break;
            }
    }
    if (same) {
        assign_images(true);
        for (auto* s : surfs) draw_one(*s);
        return;
    }
    destroy_all();
    for (auto& o : outs) create_one(o);
    assign_images(true);
}

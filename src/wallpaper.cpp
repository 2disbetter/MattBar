#include "wallpaper.hpp"
#include "bar.hpp"
#include "config.hpp"
#include "frac.hpp"
#include "imgwork.hpp"
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
#include <functional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_PSD
#define STBI_NO_PIC
#define STBI_NO_PNM
#include "stb_image.h"

namespace {

Bar*             g_bar  = nullptr;
// Small copy of the current background for the lock screen's blur (it
// scales to width/12 anyway); the full-size image is never retained.
cairo_surface_t* g_img  = nullptr;
std::string      g_img_path;          // what g_img was decoded from
std::string      g_path;              // wanted background (resolved)
std::set<std::string> g_failed;       // undecodable: don't retry per frame
uint64_t         g_ticket   = 0;      // decode job in flight
std::string      g_job_key;           // ...and what it asked for
bool             g_job_wipe = false;  // ...and whether changes wipe
uint64_t         g_next_surf_id = 0;
std::function<void()> g_listener;     // settings preview refresh
constexpr int    kAnimMs   = 420;
constexpr int    kLockEdge = 768;

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
    uint64_t               id   = 0;
    wl_output*             out  = nullptr;
    std::string            name;
    wl_surface*            surf = nullptr;
    zwlr_layer_surface_v1* ls   = nullptr;
    FracSurface            frac;
    int                    w = 0, h = 0;
    bool                   configured = false;
    bool                   fresh      = true; // first configure not seen
    std::string            path;          // what img was decoded from
    cairo_surface_t*       img = nullptr; // pre-scaled to the buffer size
    cairo_surface_t*       old = nullptr; // outgoing image during a wipe
    uint64_t               anim_t0  = 0;
    double                 progress = 1.0;
    uint64_t               frames   = 0;
    ShmPool                pool;
    FrameGate              gate;
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

// ---------------------------------------------------------------------------
// Images: decoded once, off the main loop (imgwork), and pre-scaled in
// the worker to each output's exact buffer size. Drawing a frame is then
// a 1:1 blit into a pooled buffer; the wipe is paced by frame callbacks.
// The lock screen's blur only ever needed a thumbnail, so the full-size
// image is no longer kept at all: g_img is a small copy (kLockEdge).
// ---------------------------------------------------------------------------

// Blit a pre-scaled image into a buffer of bw x bh device pixels. Exact
// size: straight copy. Otherwise (a refit is on its way after a resize
// or scale change) cover-scale it so the frame is never blank.
void blit(cairo_t* cr, cairo_surface_t* img, int bw, int bh) {
    if (!img) return;
    int iw = cairo_image_surface_get_width(img);
    int ih = cairo_image_surface_get_height(img);
    if (iw <= 0 || ih <= 0) return;
    cairo_save(cr);
    if (iw == bw && ih == bh) {
        cairo_set_source_surface(cr, img, 0, 0);
    } else {
        double s = std::max((double)bw / iw, (double)bh / ih);
        cairo_translate(cr, (bw - iw * s) / 2.0, (bh - ih * s) / 2.0);
        cairo_scale(cr, s, s);
        cairo_set_source_surface(cr, img, 0, 0);
        cairo_pattern_set_extend(cairo_get_source(cr), CAIRO_EXTEND_PAD);
    }
    cairo_paint(cr);
    cairo_restore(cr);
}

// Omarchy's background reveal: a slanted band that opens from the
// centre. Same geometry as Background.qml's ShapePath mask (in buffer
// pixels; the shape is proportional, so logical vs device is moot).
void clip_wipe(cairo_t* cr, int dw, int dh, double p) {
    const double slant      = -0.18;
    const double center_top = dw / 2.0 - slant * dh / 2.0;
    const double center_bot = dw / 2.0 + slant * dh / 2.0;
    const double reach      = dw / 2.0 + std::fabs(slant) * dh / 2.0 + 4.0;
    const double spread     = reach * p;
    cairo_move_to(cr, center_top - spread, 0);
    cairo_line_to(cr, center_top + spread, 0);
    cairo_line_to(cr, center_bot + spread, dh);
    cairo_line_to(cr, center_bot - spread, dh);
    cairo_close_path(cr);
    cairo_clip(cr);
}

bool img_opaque_at(cairo_surface_t* img, int bw, int bh) {
    return img && cairo_image_surface_get_format(img) == CAIRO_FORMAT_RGB24 &&
           cairo_image_surface_get_width(img) == bw &&
           cairo_image_surface_get_height(img) == bh;
}

void buffer_size(const Surf& s, int* bw, int* bh) {
    int sc = g_bar ? g_bar->scale_of(s.out) : 1;
    if (sc < 1) sc = 1;
    *bw = s.frac.active() ? s.frac.px(s.w) : s.w * sc;
    *bh = s.frac.active() ? s.frac.px(s.h) : s.h * sc;
}

void request(int mode);
enum { kInstant = 0, kWipe = 1, kRefit = 2 };

void finish_wipe(Surf& s) {
    if (s.old) {
        cairo_surface_destroy(s.old);
        s.old = nullptr;
    }
    s.progress = 1.0;
}

void draw_one(Surf& s) {
    if (!s.surf || !s.configured || s.w <= 0 || s.h <= 0 || !g_bar) return;
    if (!s.gate.ready()) return; // a frame is in flight; runs on its done
    int bw = 0, bh = 0;
    buffer_size(s, &bw, &bh);
    bool wiping = s.old && s.img && s.progress < 1.0;
    if (wiping) {
        double t = (double)(mono_ms() - s.anim_t0) / (double)kAnimMs;
        if (t >= 1.0) {
            finish_wipe(s);
            wiping = false;
        } else {
            s.progress = ease_inout_cubic(t);
        }
    }
    ShmBuf* b = s.pool.acquire(g_bar->shm(), bw, bh);
    if (!b) return;
    cairo_surface_t* cs = cairo_image_surface_create_for_data(
        static_cast<unsigned char*>(b->data), CAIRO_FORMAT_ARGB32, bw, bh,
        bw * 4);
    cairo_t* cr = cairo_create(cs);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_surface_t* base = wiping ? s.old : s.img;
    if (!base && !s.path.empty()) base = g_img; // decode pending: preview
    if (!img_opaque_at(base, bw, bh)) {
        cairo_set_source_rgba(cr, cfg.c_bg.r, cfg.c_bg.g, cfg.c_bg.b, 1);
        cairo_paint(cr);
        cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    }
    blit(cr, base, bw, bh);
    if (wiping) {
        cairo_set_operator(cr, img_opaque_at(s.img, bw, bh)
                                   ? CAIRO_OPERATOR_SOURCE
                                   : CAIRO_OPERATOR_OVER);
        clip_wipe(cr, bw, bh, s.progress);
        blit(cr, s.img, bw, bh);
    }
    cairo_destroy(cr);
    cairo_surface_destroy(cs);
    s.frac.apply(s.surf, s.w, s.h, g_bar->scale_of(s.out));
    wl_surface_attach(s.surf, b->wl, 0, 0);
    if (wl_surface_get_version(s.surf) >= 4)
        wl_surface_damage_buffer(s.surf, 0, 0, bw, bh);
    else
        wl_surface_damage(s.surf, 0, 0, s.w, s.h);
    s.gate.arm(s.surf);
    if (wiping) s.gate.want_next(); // next frame of the wipe on frame done
    wl_surface_commit(s.surf);
    ++s.frames;
    if (!wiping) {
        // Settled: keep no spare full-screen buffers around.
        s.pool.keep_idle = 0;
        s.pool.trim(0);
    }
    // Resized or rescaled since the image was scaled: fetch a refit.
    if (s.img && !s.path.empty() &&
        (cairo_image_surface_get_width(s.img) != bw ||
         cairo_image_surface_get_height(s.img) != bh))
        request(kRefit);
}

void redraw_all() {
    for (auto* s : surfs) draw_one(*s);
}

// Which image each surface should show. Index 0 is the main monitor
// (cfg.output, else the first by name). cfg.wallpaper_monitors decides
// the rest: same (the background everywhere, the default), main (only
// index 0; others show the fill colour), mix (other theme images, the
// pre-1.42.5 behaviour), each (per-monitor picks, else the background).
std::vector<std::pair<Surf*, std::string>> wanted_paths() {
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
        if (a->name.empty() != b->name.empty()) return b->name.empty();
        return a->name < b->name;
    });
    if (!cfg.output.empty()) {
        for (size_t i = 1; i < order.size(); ++i)
            if (order[i]->name == cfg.output) {
                std::swap(order[0], order[i]);
                break;
            }
    }
    const std::string& mode = cfg.wallpaper_monitors;
    const std::string  main = pool.empty() ? std::string() : pool[0];
    std::vector<std::pair<Surf*, std::string>> out;
    for (size_t i = 0; i < order.size(); ++i) {
        std::string p;
        if (mode == "mix") {
            p = path_for_output(order[i]->name, i, pool);
        } else if (mode == "main") {
            p = i == 0 ? main : std::string();
        } else if (mode == "each") {
            p = cfg.wallpaper_for(order[i]->name);
            if (p.empty() || access(p.c_str(), R_OK) != 0) p = main;
        } else {
            p = main; // "same"
        }
        out.push_back({order[i], p});
    }
    return out;
}

// Bring every surface (and the lock copy) to the image it should show
// at its current buffer size, with one decode per file, off the loop.
// Cheap when nothing changed; identical in-flight jobs are not resent.
void request(int mode) {
    if (!g_bar) return;
    // Outputs still waiting for their first configure will ask when it
    // lands; submitting now would decode once for nothing.
    for (auto* s : surfs)
        if (s->fresh && s->surf) return;
    const bool wipe = mode == kWipe || (mode == kRefit && g_ticket && g_job_wipe);
    struct Map {
        uint64_t    sid;   // Surf::id, 0 = lock copy
        size_t      item, target;
        std::string path;
    };
    std::vector<ImgItem> items;
    std::vector<Map>     maps;
    auto item_for = [&](const std::string& p) -> size_t {
        for (size_t i = 0; i < items.size(); ++i)
            if (items[i].path == p) return i;
        items.push_back({p, {}});
        return items.size() - 1;
    };
    std::string key = wipe ? "w" : "i";
    for (auto& [s, p] : wanted_paths()) {
        if (p.empty()) { // nothing to show here: plain fill
            if (s->img || s->old) {
                finish_wipe(*s);
                if (s->img) cairo_surface_destroy(s->img);
                s->img = nullptr;
                s->path.clear();
                draw_one(*s);
            }
            continue;
        }
        if (g_failed.count(p) && s->path != p) continue; // don't retry
        if (!s->configured || s->w <= 0 || s->h <= 0) continue;
        int bw = 0, bh = 0;
        buffer_size(*s, &bw, &bh);
        const bool fits = s->img && cairo_image_surface_get_width(s->img) == bw &&
                          cairo_image_surface_get_height(s->img) == bh;
        if (s->path == p && (fits || g_failed.count(p))) continue;
        size_t i = item_for(p);
        items[i].targets.push_back({bw, bh, ImgTarget::Cover});
        maps.push_back({s->id, i, items[i].targets.size() - 1, p});
        char buf[64];
        snprintf(buf, sizeof buf, "|%llu:%dx%d:", (unsigned long long)s->id,
                 bw, bh);
        key += buf + p;
    }
    if (!g_path.empty() && (g_img_path != g_path || !g_img) &&
        !g_failed.count(g_path)) {
        size_t i = item_for(g_path);
        items[i].targets.push_back({kLockEdge, kLockEdge, ImgTarget::Fit});
        maps.push_back({0, i, items[i].targets.size() - 1, g_path});
        key += "|lock:" + g_path;
    } else if (g_path.empty() && g_img) {
        cairo_surface_destroy(g_img);
        g_img = nullptr;
        g_img_path.clear();
    }
    if (items.empty()) {
        img_cancel(g_ticket); // whatever was in flight is moot now
        g_ticket = 0;
        g_job_key.clear();
        return;
    }
    if (g_ticket && key == g_job_key) return; // already on its way
    img_cancel(g_ticket);
    g_job_key  = key;
    g_job_wipe = wipe;
    g_ticket   = img_submit(
        *g_bar, std::move(items), [maps, wipe](ImgResult& r) {
            g_ticket = 0;
            g_job_key.clear();
            std::vector<Surf*> touched;
            for (size_t i = 0; i < r.surfs.size(); ++i)
                if (r.src_size[i].first == 0) {
                    for (auto& m : maps)
                        if (m.item == i) {
                            if (!g_failed.count(m.path))
                                fprintf(stderr, "mattbar: wallpaper: could "
                                        "not decode %s\n", m.path.c_str());
                            g_failed.insert(m.path);
                            break;
                        }
                }
            for (auto& m : maps) {
                cairo_surface_t* img =
                    std::exchange(r.surfs[m.item][m.target], nullptr);
                if (m.sid == 0) {
                    if (!img) continue;
                    if (g_img) cairo_surface_destroy(g_img);
                    g_img      = img;
                    g_img_path = m.path;
                    continue;
                }
                Surf* s = nullptr;
                for (auto* c : surfs)
                    if (c->id == m.sid) s = c;
                if (!s) { // output went away meanwhile
                    if (img) cairo_surface_destroy(img);
                    continue;
                }
                if (!img) { // failed: keep what is shown, stop asking
                    s->path = m.path;
                    continue;
                }
                const bool change = s->path != m.path;
                if (change && wipe && s->img) {
                    if (s->old) cairo_surface_destroy(s->old);
                    s->old        = s->img;
                    s->anim_t0    = mono_ms();
                    s->progress   = 0.0;
                    s->pool.keep_idle = 2; // reuse buffers across the wipe
                } else if (s->img) {
                    cairo_surface_destroy(s->img);
                }
                s->img  = img;
                s->path = m.path;
                touched.push_back(s);
            }
            for (auto* s : touched) draw_one(*s);
            // A resize while this job ran may need another pass.
            request(kRefit);
            if (g_listener) g_listener(); // settings preview
        });
}

void show_path(const std::string& path, bool instant) {
    g_failed.erase(path); // an explicit set retries a file that failed
    g_path = path;
    request(instant ? kInstant : kWipe);
    if (path.empty()) redraw_all();
}

void on_configure(void* data, zwlr_layer_surface_v1* ls, uint32_t serial,
                  uint32_t w, uint32_t h) {
    auto* s = static_cast<Surf*>(data);
    zwlr_layer_surface_v1_ack_configure(ls, serial);
    if (w) s->w = (int)w;
    if (h) s->h = (int)h;
    s->configured = true;
    s->fresh      = false;
    // Every pixel is painted opaque: the compositor can skip what's under.
    if (wl_region* r = wl_compositor_create_region(g_bar->compositor())) {
        wl_region_add(r, 0, 0, s->w, s->h);
        wl_surface_set_opaque_region(s->surf, r);
        wl_region_destroy(r);
    }
    draw_one(*s);
    request(kRefit);
}

void on_closed(void* data, zwlr_layer_surface_v1*) {
    auto* s = static_cast<Surf*>(data);
    s->configured = false;
}

void destroy_one(Surf& s) {
    finish_wipe(s);
    if (s.img) {
        cairo_surface_destroy(s.img);
        s.img = nullptr;
    }
    s.path.clear();
    s.gate.drop();
    s.pool.clear();
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
    img_cancel(g_ticket);
    g_ticket = 0;
    g_job_key.clear();
    if (g_img) {
        cairo_surface_destroy(g_img);
        g_img = nullptr;
    }
    g_img_path.clear();
}

void create_one(Bar::OutputRef o) {
    if (!g_bar || !g_bar->compositor() || !g_bar->layer_shell() || !o.wl)
        return;
    auto* s  = new Surf;
    s->id    = ++g_next_surf_id;
    s->out   = o.wl;
    s->name  = o.name;
    s->pool.keep_idle = 0;
    s->gate.fire      = [s] { draw_one(*s); };
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
        if (!s->configured) return;
        draw_one(*s);
        request(kRefit);
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
    // Wallpaper must never steal pointer events (default region is infinite).
    if (wl_region* r = wl_compositor_create_region(g_bar->compositor())) {
        wl_surface_set_input_region(s->surf, r);
        wl_region_destroy(r);
    }
    surfs.push_back(s);
    g_bar->register_surface(s->surf, {});
    wl_surface_commit(s->surf);
}

} // namespace

void wallpaper_init(Bar& bar) { g_bar = &bar; }

cairo_surface_t* wallpaper_image() { return g_img; }

cairo_surface_t* image_load_file(const std::string& path) {
    return img_decode_now(path, {0, 0, ImgTarget::Fit});
}

void wallpaper_refresh() { show_path(background_path(), false); }

void wallpaper_set(const std::string& path, bool instant) {
    show_path(path, instant);
}

// The wipe always runs from what each output currently shows; the
// switcher's "from" image was decoded and then never drawn, so it is
// no longer decoded at all.
void wallpaper_transition(const std::string&, const std::string& path) {
    show_path(path, false);
}

std::string wallpaper_current_path() {
    std::string p = g_path.empty() ? background_path() : g_path;
    if (p.empty()) return p;
    char* real = realpath(p.c_str(), nullptr);
    std::string r = real ? real : p;
    if (real) free(real);
    return r;
}

// Point Omarchy's current-background link at `path`, in-process and
// atomically (symlink to a temp name, then rename over the link).
void wallpaper_point_link(const std::string& path) {
    if (path.empty()) return;
    const char* h   = getenv("HOME");
    const char* xdg = getenv("XDG_STATE_HOME");
    std::string state = xdg && *xdg ? std::string(xdg)
                                    : std::string(h ? h : ".") + "/.local/state";
    std::string dir = state + "/omarchy/current";
    std::string acc;
    for (size_t i = 0; i <= dir.size(); ++i) // mkdir -p
        if (i == dir.size() || (dir[i] == '/' && i > 0)) {
            acc = dir.substr(0, i);
            mkdir(acc.c_str(), 0755);
        }
    std::string link = dir + "/background";
    std::string tmp  = dir + "/.background.mattbar." + std::to_string(getpid());
    unlink(tmp.c_str());
    if (symlink(path.c_str(), tmp.c_str()) != 0) return;
    if (rename(tmp.c_str(), link.c_str()) != 0) unlink(tmp.c_str());
}

void wallpaper_choose(const std::string& path) {
    wallpaper_point_link(path);
    show_path(path, false);
}

std::vector<std::string> wallpaper_candidates() {
    std::vector<std::string> v = background_pool();
    const char* h = getenv("HOME");
    if (h && *h) {
        add_dir_images(v, std::string(h) + "/Pictures/Wallpapers");
        add_dir_images(v, std::string(h) + "/Wallpapers");
    }
    std::vector<std::string> out;
    for (auto& p : v)
        if (std::find(out.begin(), out.end(), p) == out.end()) out.push_back(p);
    return out;
}

void wallpaper_set_listener(std::function<void()> fn) { g_listener = std::move(fn); }

std::string wallpaper_debug_state() {
    std::string o;
    char        buf[256];
    snprintf(buf, sizeof buf, "path=%s lock=%dx%d job=%llu decodes=%llu\n",
             g_path.c_str(),
             g_img ? cairo_image_surface_get_width(g_img) : 0,
             g_img ? cairo_image_surface_get_height(g_img) : 0,
             (unsigned long long)g_ticket,
             (unsigned long long)img_decodes_total());
    o += buf;
    for (auto* s : surfs) {
        int bw = 0, bh = 0;
        buffer_size(*s, &bw, &bh);
        snprintf(buf, sizeof buf,
                 "  %s %dx%d buf=%dx%d img=%dx%d%s frames=%llu pool=%zu %s\n",
                 s->name.c_str(), s->w, s->h, bw, bh,
                 s->img ? cairo_image_surface_get_width(s->img) : 0,
                 s->img ? cairo_image_surface_get_height(s->img) : 0,
                 s->img && cairo_image_surface_get_format(s->img) ==
                                   CAIRO_FORMAT_RGB24
                     ? "(rgb24)"
                     : "",
                 (unsigned long long)s->frames, s->pool.count(),
                 s->path.c_str());
        o += buf;
    }
    return o;
}

// What wallpaper_apply's result depends on, besides the image itself
// (which the background IPC handles directly). Settings applies, hotplug
// and takeover all call in; before this, each call re-resolved the
// background and repainted every output full-screen twice.
static std::string g_applied_layout; // takeover + outputs + cfg.output
static std::string g_applied_fill;   // cfg.c_bg (letterbox / no-image fill)

static std::string apply_layout_key(const std::vector<Bar::OutputRef>& outs) {
    std::string k = cfg.quickshell_shutdown ? "on|" : "off|";
    k += cfg.output + "|" + cfg.wallpaper_monitors + "|" + cfg.wallpaper_outputs;
    char buf[160];
    for (auto& o : outs) {
        snprintf(buf, sizeof buf, "|%p,%d,%d,%d,", (void*)o.wl, o.scale,
                 o.logical_w, o.logical_h);
        k += buf;
        k += o.name;
    }
    return k;
}

void wallpaper_apply() {
    if (!g_bar) return;
    auto outs = g_bar->output_list();
    const std::string layout = apply_layout_key(outs);
    char fill[96];
    snprintf(fill, sizeof fill, "%.4f,%.4f,%.4f", cfg.c_bg.r, cfg.c_bg.g,
             cfg.c_bg.b);
    if (layout == g_applied_layout) {
        if (fill == g_applied_fill) return; // nothing it depends on moved
        g_applied_fill = fill;
        if (cfg.quickshell_shutdown) redraw_all(); // repaint, no decode
        return;
    }
    g_applied_layout = layout;
    g_applied_fill   = fill;
    if (!cfg.quickshell_shutdown) {
        destroy_all();
        return;
    }
    // Reconcile per output: a monitor that went away loses its surface, a
    // new one (hotplug) gets one; the others are left alone, so plugging
    // in a monitor no longer blanks and re-decodes every other screen.
    for (auto it = surfs.begin(); it != surfs.end();) {
        bool live = false;
        for (auto& o : outs)
            if (o.wl == (*it)->out) live = true;
        if (live) {
            ++it;
            continue;
        }
        destroy_one(**it);
        delete *it;
        it = surfs.erase(it);
    }
    for (auto& o : outs) {
        bool have = false;
        for (auto* s : surfs)
            if (s->out == o.wl) {
                have   = true;
                s->name = o.name; // the name event can land after creation
            }
        if (!have) create_one(o);
    }
    // New surfaces decode once configured (on_configure -> request);
    // existing ones refit or wipe to a changed background here.
    wallpaper_refresh();
}

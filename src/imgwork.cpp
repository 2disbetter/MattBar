// Background image decoding — see imgwork.hpp.
#include "imgwork.hpp"

#include "bar.hpp"
#include "config.hpp"
#include "stb_image.h" // implementation lives in wallpaper.cpp

#include <pthread.h>
#include <signal.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <cstdint>
#include <string>
#include <vector>
#include <utility>

namespace {

struct Job {
    uint64_t             ticket = 0;
    std::vector<ImgItem> items;
};
struct Finished {
    uint64_t  ticket = 0;
    ImgResult res;
};

std::mutex              g_mu;
std::condition_variable g_cv;
std::deque<Job>         g_queue;   // guarded by g_mu
std::deque<Finished>    g_done;    // guarded by g_mu
bool                    g_stop = false;
std::atomic<uint64_t>   g_current{0}; // ticket the worker is on
std::atomic<uint64_t>   g_abandon{0}; // cancel request for g_current
std::atomic<uint64_t>   g_decodes{0};

// Main thread only.
std::thread                 g_thread;
bool                        g_started = false;
bool                        g_threadless = false; // thread failed: decode inline
int                         g_efd = -1;
Bar*                        g_bar = nullptr;
uint64_t                    g_next_ticket = 1;
std::map<uint64_t, ImgDone> g_callbacks;

// Refuse absurd images up front: the RGBA decode buffer is w*h*4 bytes.
constexpr long long kMaxPixels = 100LL * 1000 * 1000;

void free_result(ImgResult& r) {
    for (auto& v : r.surfs)
        for (auto*& s : v)
            if (s) {
                cairo_surface_destroy(s);
                s = nullptr;
            }
}

cairo_user_data_key_t k_stb_owner;

// Decode to a premultiplied ARGB32 surface that owns stb's buffer (no
// second full-size copy).
cairo_surface_t* decode(const std::string& path, int* ow, int* oh,
                        bool* opaque) {
    *ow = *oh = 0;
    *opaque = false;
    if (path.empty()) return nullptr;
    // One close-on-exec FILE for both passes. stbi_load() opens with plain
    // "rb", and this runs on the worker thread while the main loop may be
    // spawning: the child would inherit the open image file.
    FILE* f = fopen(path.c_str(), "rbe");
    if (!f) return nullptr;
    int w = 0, h = 0, n = 0;
    if (!stbi_info_from_file(f, &w, &h, &n) || w <= 0 || h <= 0 ||
        (long long)w * h > kMaxPixels) {
        fclose(f);
        return nullptr;
    }
    unsigned char* px = stbi_load_from_file(f, &w, &h, &n, 4);
    fclose(f);
    if (!px || w <= 0 || h <= 0) {
        if (px) stbi_image_free(px);
        return nullptr;
    }
    ++g_decodes;
    const size_t count = (size_t)w * h;
    auto*        out   = reinterpret_cast<uint32_t*>(px);
    bool         solid = true;
    for (size_t i = 0; i < count; ++i) {
        const unsigned char* p = px + i * 4;
        uint32_t r = p[0], g = p[1], b = p[2], a = p[3];
        if (a != 255) { // premultiply (cairo's ARGB32 is premultiplied)
            solid = false;
            r = (r * a + 127) / 255;
            g = (g * a + 127) / 255;
            b = (b * a + 127) / 255;
        }
        out[i] = a << 24 | r << 16 | g << 8 | b;
    }
    cairo_surface_t* cs = cairo_image_surface_create_for_data(
        px, CAIRO_FORMAT_ARGB32, w, h, w * 4);
    if (cairo_surface_status(cs) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(cs);
        stbi_image_free(px);
        return nullptr;
    }
    cairo_surface_set_user_data(cs, &k_stb_owner, px,
                                [](void* p) { stbi_image_free(p); });
    *ow     = w;
    *oh     = h;
    *opaque = solid;
    return cs;
}

// An opaque source scaled to Cover lands in an RGB24 surface: cairo then
// knows it is opaque, and blitting it is a plain copy (no blending).
cairo_surface_t* scale_to(cairo_surface_t* src, int sw, int sh, bool opaque,
                          const ImgTarget& t) {
    if (!src || sw <= 0 || sh <= 0) return nullptr;
    int    dw = 0, dh = 0;
    double s = 1, ox = 0, oy = 0;
    if (t.mode == ImgTarget::Cover) {
        dw = t.w;
        dh = t.h;
        if (dw < 1 || dh < 1) return nullptr;
        s  = std::max((double)dw / sw, (double)dh / sh);
        ox = (dw - sw * s) / 2.0;
        oy = (dh - sh * s) / 2.0;
    } else {
        s = 1;
        if (t.w > 0) s = std::min(s, (double)t.w / sw);
        if (t.h > 0) s = std::min(s, (double)t.h / sh);
        if (s >= 1.0) return cairo_surface_reference(src); // already fits
        dw = std::max(1, (int)std::lround(sw * s));
        dh = std::max(1, (int)std::lround(sh * s));
    }
    const bool rgb24 = t.mode == ImgTarget::Cover && opaque;
    if (!rgb24 && dw == sw && dh == sh) return cairo_surface_reference(src);
    cairo_surface_t* d = cairo_image_surface_create(
        rgb24 ? CAIRO_FORMAT_RGB24 : CAIRO_FORMAT_ARGB32, dw, dh);
    if (cairo_surface_status(d) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(d);
        return nullptr;
    }
    cairo_t* cr = cairo_create(d);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    if (t.mode == ImgTarget::Fit) {
        cairo_scale(cr, (double)dw / sw, (double)dh / sh);
    } else {
        cairo_translate(cr, ox, oy);
        cairo_scale(cr, s, s);
    }
    cairo_set_source_surface(cr, src, 0, 0);
    // PAD: no transparent fringe where the scaled image meets the edge.
    cairo_pattern_set_extend(cairo_get_source(cr), CAIRO_EXTEND_PAD);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
    cairo_paint(cr);
    cairo_destroy(cr);
    cairo_surface_flush(d);
    return d;
}

ImgResult run_job(const Job& job, bool honour_abandon) {
    ImgResult res;
    res.surfs.resize(job.items.size());
    res.src_size.resize(job.items.size(), {0, 0});
    for (size_t i = 0; i < job.items.size(); ++i) {
        const ImgItem& it = job.items[i];
        res.surfs[i].assign(it.targets.size(), nullptr);
        if (honour_abandon && g_abandon.load() == job.ticket) break;
        int  sw = 0, sh = 0;
        bool opaque = false;
        cairo_surface_t* src = decode(it.path, &sw, &sh, &opaque);
        if (!src) continue;
        res.src_size[i] = {sw, sh};
        for (size_t k = 0; k < it.targets.size(); ++k)
            res.surfs[i][k] = scale_to(src, sw, sh, opaque, it.targets[k]);
        cairo_surface_destroy(src);
    }
    return res;
}

void worker_main() {
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lk(g_mu);
            g_cv.wait(lk, [] { return g_stop || !g_queue.empty(); });
            if (g_stop) return;
            job = std::move(g_queue.front());
            g_queue.pop_front();
            g_current = job.ticket;
        }
        ImgResult res = run_job(job, true);
        {
            std::lock_guard<std::mutex> lk(g_mu);
            g_current = 0;
            if (g_stop) {
                free_result(res);
                return;
            }
            g_done.push_back({job.ticket, std::move(res)});
        }
        uint64_t one = 1;
        (void)!write(g_efd, &one, sizeof one);
    }
}

void drain() {
    uint64_t x;
    while (read(g_efd, &x, sizeof x) > 0) {}
    std::deque<Finished> got;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        got.swap(g_done);
    }
    for (auto& f : got) {
        auto it = g_callbacks.find(f.ticket);
        if (it != g_callbacks.end()) {
            ImgDone cb = std::move(it->second);
            g_callbacks.erase(it);
            if (cb) cb(f.res); // may submit or cancel: map entry is gone
        }
        free_result(f.res);
    }
}

bool ensure_started(Bar& bar) {
    if (g_started) return true;
    g_bar = &bar;
    g_efd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (g_efd < 0) return false;
    bar.add_fd(g_efd, [](uint32_t) { drain(); }, "img-worker");
    // The worker must never run a signal handler or steal a signal meant
    // for a signalfd: start it with everything blocked.
    sigset_t all, old;
    sigfillset(&all);
    pthread_sigmask(SIG_SETMASK, &all, &old);
    try {
        g_thread = std::thread(worker_main);
        pthread_setname_np(g_thread.native_handle(), "mattbar-img");
    } catch (...) {
        g_threadless = true;
        fprintf(stderr, "mattbar: image worker thread failed to start; "
                        "decoding on the main loop\n");
    }
    pthread_sigmask(SIG_SETMASK, &old, nullptr);
    g_started = true;
    return true;
}

} // namespace

uint64_t img_submit(Bar& bar, std::vector<ImgItem> items, ImgDone done) {
    const uint64_t ticket = g_next_ticket++;
    if (!ensure_started(bar)) {
        // No eventfd: decode inline, deliver inline (still correct).
        Job       job{ticket, std::move(items)};
        ImgResult res = run_job(job, false);
        if (done) done(res);
        free_result(res);
        return ticket;
    }
    g_callbacks[ticket] = std::move(done);
    Job job{ticket, std::move(items)};
    if (g_threadless) {
        ImgResult res = run_job(job, false);
        {
            std::lock_guard<std::mutex> lk(g_mu);
            g_done.push_back({ticket, std::move(res)});
        }
        uint64_t one = 1;
        (void)!write(g_efd, &one, sizeof one);
        return ticket;
    }
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_queue.push_back(std::move(job));
    }
    g_cv.notify_one();
    return ticket;
}

void img_cancel(uint64_t ticket) {
    if (!ticket) return;
    g_callbacks.erase(ticket);
    if (!g_started) return;
    std::lock_guard<std::mutex> lk(g_mu);
    for (auto it = g_queue.begin(); it != g_queue.end(); ++it)
        if (it->ticket == ticket) {
            g_queue.erase(it);
            return;
        }
    if (g_current.load() == ticket) g_abandon = ticket;
}

void img_shutdown() {
    if (!g_started) return;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_stop = true;
        g_queue.clear();
    }
    g_cv.notify_all();
    if (g_thread.joinable()) g_thread.join();
    for (auto& f : g_done) free_result(f.res);
    g_done.clear();
    g_callbacks.clear();
    Bar::close_fd(g_efd);
    g_started = false;
    g_stop    = false;
}

cairo_surface_t* img_decode_now(const std::string& path, ImgTarget t) {
    int  sw = 0, sh = 0;
    bool opaque = false;
    cairo_surface_t* src = decode(path, &sw, &sh, &opaque);
    if (!src) return nullptr;
    cairo_surface_t* out = scale_to(src, sw, sh, opaque, t);
    cairo_surface_destroy(src);
    return out;
}

uint64_t img_decodes_total() { return g_decodes.load(); }

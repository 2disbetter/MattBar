#pragma once
// Shared ARGB8888 SHM buffers and frame pacing for every MattBar surface
// (bar, popups, settings, tray menu, wallpaper).
//
//   ShmPool   — a few buffers per surface, reused once the compositor
//               releases them and rebuilt only when the size changes.
//               Replaces memfd_create + ftruncate + mmap + a fresh
//               wl_shm_pool + fresh page faults on every single draw.
//   FrameGate — wl_surface.frame pacing: at most one commit per frame
//               the compositor actually shows. A draw asked for while a
//               frame is in flight is remembered and runs when the
//               frame is done. A callback that never arrives (monitor
//               off, session locked, surface not shown) goes stale after
//               kFrameStaleMs and the draw runs anyway, from the main
//               loop (frame_gates_service), so nothing can wedge.
//
// lock.cpp keeps its own pool (grab_lock_buf), which this generalises.
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

// Cleared by Bar::shutdown right before wl_display_disconnect. Pools and
// gates can live in objects that outlast the display (polkit's file-
// scope PopupWin); after this point they must not touch a proxy.
inline bool g_wl_alive = true;

inline uint64_t shm_mono_us() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

// Debug counters (profiler / tests): buffers created and draws skipped.
inline uint64_t g_shm_allocs      = 0;
inline uint64_t g_frames_deferred = 0;

// ---------------------------------------------------------------------------
// Buffer pool
// ---------------------------------------------------------------------------
struct ShmPool;

struct ShmBuf {
    wl_buffer* wl   = nullptr;
    void*      data = nullptr;
    size_t     size = 0;
    int        w = 0, h = 0;
    bool       busy = false;     // attached; the compositor may read it
    ShmPool*   pool = nullptr;   // nullptr: orphan, freed on release
};

inline void shm_buf_free(ShmBuf* b) {
    if (!b) return;
    if (b->wl && g_wl_alive) wl_buffer_destroy(b->wl);
    if (b->data) munmap(b->data, b->size);
    delete b;
}

struct ShmPool {
    static constexpr int kMax = 3; // front + back + one in the pipeline

    // Idle buffers kept after release. 2 for small surfaces; the
    // wallpaper drops to 0 when not animating (a 4K buffer is 33 MB and
    // wlroots-based compositors copy SHM content at commit anyway).
    int keep_idle = 2;

    ShmPool() = default;
    ShmPool(const ShmPool&)            = delete;
    ShmPool& operator=(const ShmPool&) = delete;
    ~ShmPool() { clear(); }

    // A buffer of exactly w x h the compositor isn't holding, marked busy.
    // Its content is whatever was drawn last time: callers repaint every
    // pixel (all of MattBar's surfaces start with a SOURCE paint).
    ShmBuf* acquire(wl_shm* shm, int w, int h) {
        if (!shm || w < 1 || h < 1) return nullptr;
        want_w_ = w;
        want_h_ = h;
        // Idle buffers of another size are dead weight: drop them now.
        for (auto it = bufs_.begin(); it != bufs_.end();) {
            ShmBuf* b = *it;
            if (!b->busy && (b->w != w || b->h != h)) {
                shm_buf_free(b);
                it = bufs_.erase(it);
            } else {
                ++it;
            }
        }
        for (ShmBuf* b : bufs_)
            if (!b->busy) {
                b->busy = true;
                return b;
            }
        ShmBuf* b = make(shm, w, h);
        if (!b) return nullptr;
        if ((int)bufs_.size() < kMax) {
            b->pool = this;
            bufs_.push_back(b);
        } // else: every slot is in flight; this one frees itself on release
        return b;
    }

    // Free every idle buffer; busy ones become orphans and free themselves
    // when released. Called when the surface goes away.
    void clear() {
        for (ShmBuf* b : bufs_) {
            if (b->busy) b->pool = nullptr;
            else shm_buf_free(b);
        }
        bufs_.clear();
    }

    // Free idle buffers beyond `keep` (after an animation settles).
    void trim(int keep) {
        int idle = 0;
        for (auto it = bufs_.begin(); it != bufs_.end();) {
            ShmBuf* b = *it;
            if (!b->busy && ++idle > keep) {
                shm_buf_free(b);
                it = bufs_.erase(it);
            } else {
                ++it;
            }
        }
    }

    size_t count() const { return bufs_.size(); }

private:
    std::vector<ShmBuf*> bufs_;
    int                  want_w_ = 0, want_h_ = 0;

    static void on_release(void* data, wl_buffer*) {
        auto* b = static_cast<ShmBuf*>(data);
        b->busy = false;
        if (!b->pool) {
            shm_buf_free(b);
            return;
        }
        b->pool->released(b);
    }

    void released(ShmBuf* b) {
        int idle = 0;
        for (ShmBuf* o : bufs_)
            if (!o->busy) ++idle;
        const bool stale = b->w != want_w_ || b->h != want_h_;
        if (stale || idle > keep_idle) {
            bufs_.erase(std::remove(bufs_.begin(), bufs_.end(), b),
                        bufs_.end());
            shm_buf_free(b);
        }
    }

    static ShmBuf* make(wl_shm* shm, int w, int h) {
        static const wl_buffer_listener lst = {.release = on_release};
        const int    stride = w * 4;
        const size_t size   = static_cast<size_t>(stride) * h;
        int fd = memfd_create("mattbar-shm", MFD_CLOEXEC);
        if (fd < 0 || ftruncate(fd, static_cast<off_t>(size)) < 0) {
            if (fd >= 0) close(fd);
            return nullptr;
        }
        void* data =
            mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (data == MAP_FAILED) {
            close(fd);
            return nullptr;
        }
        wl_shm_pool* pool = wl_shm_create_pool(shm, fd, static_cast<int>(size));
        wl_buffer*   wl   = wl_shm_pool_create_buffer(pool, 0, w, h, stride,
                                                      WL_SHM_FORMAT_ARGB8888);
        wl_shm_pool_destroy(pool);
        close(fd);
        if (!wl) {
            munmap(data, size);
            return nullptr;
        }
        auto* b = new ShmBuf;
        b->wl   = wl;
        b->data = data;
        b->size = size;
        b->w    = w;
        b->h    = h;
        b->busy = true;
        wl_buffer_add_listener(wl, &lst, b);
        ++g_shm_allocs;
        return b;
    }
};

// One-shot buffer that frees itself on release (kept for any caller that
// genuinely draws once; the pooled path is ShmPool::acquire).
inline wl_buffer* create_argb_buffer(wl_shm* shm, int w, int h,
                                     void** out_data) {
    static ShmPool orphans; // never retains: every buffer is orphaned
    ShmBuf* b = orphans.acquire(shm, w, h);
    if (!b) return nullptr;
    orphans.clear(); // detach: it frees itself when the compositor is done
    *out_data = b->data;
    return b->wl;
}

// ---------------------------------------------------------------------------
// Frame pacing
// ---------------------------------------------------------------------------
constexpr int kFrameStaleMs = 200;

struct FrameGate;
inline std::vector<FrameGate*>& frame_gate_list() {
    static std::vector<FrameGate*> v;
    return v;
}

struct FrameGate {
    // Runs when a deferred draw may proceed (frame done, or stale).
    // BarSurface leaves it empty: its main-loop pass redraws dirty bars.
    std::function<void()> fire;

    FrameGate() = default;
    FrameGate(const FrameGate&)            = delete;
    FrameGate& operator=(const FrameGate&) = delete;
    ~FrameGate() {
        unlist();
        release_cb();
    }

    // Top of draw(): true = draw now. false = a frame is still in flight;
    // the request is remembered and `fire` runs when it may proceed.
    bool ready() {
        if (cb_ && shm_mono_us() - armed_us_ <= (uint64_t)kFrameStaleMs * 1000) {
            if (!want_) ++g_frames_deferred;
            want_next();
            return false;
        }
        release_cb(); // none in flight, or it never came (monitor off /
        unlist();     // locked): draw now; this draw serves the request
        return true;
    }

    // Right before wl_surface_commit of a new buffer.
    void arm(wl_surface* s) {
        release_cb();
        if (!s || !g_wl_alive) return;
        static const wl_callback_listener lst = {.done = on_done};
        cb_ = wl_surface_frame(s);
        if (cb_) wl_callback_add_listener(cb_, &lst, this);
        armed_us_ = shm_mono_us();
    }

    // Ask for `fire` on the next frame (continuous animation).
    void want_next() {
        want_ = true;
        if (!listed_) {
            frame_gate_list().push_back(this);
            listed_ = true;
        }
    }

    // The surface is going away: forget the callback and any request.
    void drop() {
        unlist();
        release_cb();
    }

    bool     pending() const { return cb_ != nullptr; }
    bool     wanted() const { return want_; }
    uint64_t stale_at_us() const {
        return armed_us_ + (uint64_t)kFrameStaleMs * 1000;
    }

    // Main-loop fallback for a callback that never arrives.
    void service_stale() {
        release_cb();
        take_and_fire();
    }

private:
    wl_callback* cb_       = nullptr;
    uint64_t     armed_us_ = 0;
    bool         want_     = false;
    bool         listed_   = false;

    void release_cb() {
        if (cb_ && g_wl_alive) wl_callback_destroy(cb_);
        cb_ = nullptr;
    }
    void unlist() {
        want_ = false;
        if (!listed_) return;
        auto& v = frame_gate_list();
        v.erase(std::remove(v.begin(), v.end(), this), v.end());
        listed_ = false;
    }
    void take_and_fire() {
        bool w = want_;
        unlist();
        if (w && fire) fire();
    }
    static void on_done(void* data, wl_callback* cb, uint32_t) {
        auto* g = static_cast<FrameGate*>(data);
        if (g->cb_ == cb) {
            wl_callback_destroy(cb);
            g->cb_ = nullptr;
            g->take_and_fire();
        } else {
            wl_callback_destroy(cb);
        }
    }
};

// Milliseconds until the earliest deferred draw goes stale; -1 if none.
inline int frame_gates_timeout_ms() {
    auto& v = frame_gate_list();
    if (v.empty()) return -1;
    uint64_t now = shm_mono_us(), first = UINT64_MAX;
    for (FrameGate* g : v) {
        if (!g->pending()) return 0; // wanted with nothing in flight
        first = std::min(first, g->stale_at_us());
    }
    if (first <= now) return 0;
    return (int)std::min<uint64_t>((first - now + 999) / 1000, 60000);
}

// Run deferred draws whose frame callback went stale (or never existed).
inline void frame_gates_service() {
    auto&    v   = frame_gate_list();
    if (v.empty()) return;
    uint64_t now = shm_mono_us();
    std::vector<FrameGate*> due;
    for (FrameGate* g : v)
        if (!g->pending() || g->stale_at_us() <= now) due.push_back(g);
    for (FrameGate* g : due) {
        // A fire() may destroy other gates: re-check membership.
        auto& cur = frame_gate_list();
        if (std::find(cur.begin(), cur.end(), g) == cur.end()) continue;
        g->service_stale();
    }
}

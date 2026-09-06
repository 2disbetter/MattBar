#include "lock.hpp"
#include "bar.hpp"
#include "config.hpp"
#include "idle.hpp"
#include "modules.hpp"
#include "notify.hpp"
#include "sdpump.hpp"
#include "shm.hpp"
#include "util.hpp"
#include "ui.hpp"
#include "wallpaper.hpp"

#include "ext-session-lock-v1-client-protocol.h"

#include <cairo/cairo.h>
#include <pwd.h>
#include <security/pam_appl.h>
#include <csignal>
#include <sys/mman.h>
#include <sys/timerfd.h>
#include <sys/wait.h>
#include <systemd/sd-bus.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

namespace {

Bar* g_bar = nullptr;
int  g_reclaim_fd = -1;
int  g_reclaim_n  = 0;
int  g_blank_fd   = -1;
bool g_display_off = false;
bool g_blank_reassert = false;
uint64_t g_blank_armed_ms = 0;
uint64_t g_wake_grace_until = 0;
bool g_locked = false;
bool g_busy   = false;
bool g_sleeping = false; // PrepareForSleep true..false; do not blank across it

enum NoteSrc { NoteMotion, NoteButton, NoteKey };
SdPump g_login_pump;

void redraw_lock();
void drain_blank();
void wake_display();
void arm_blank();

int on_prepare_for_sleep(sd_bus_message* m, void*, sd_bus_error*) {
    int entering = 0;
    if (sd_bus_message_read(m, "b", &entering) < 0) return 0;
    fprintf(stderr, "mattbar: lock: PrepareForSleep %s\n",
            entering ? "entering" : "leaving");
    if (!cfg.quickshell_shutdown) return 0;
    if (entering) {
        // Disarm blank first: CLOCK_MONOTONIC timerfds can fire on thaw
        // with a still-small armed-at age (monotonic does not count S4),
        // so the stale-timeout check treats them as real and blacks the
        // panel before the lock surface is visible.
        g_sleeping = true;
        drain_blank();
        lock_now();
        drain_blank();
        if (g_bar) g_bar->ping_watchdog();
        return 0;
    }
    // Hibernate restore and suspend thaw: firmware may leave DPMS off
    // and a pending blank timer may already be readable. Wake first,
    // then paint, then start a fresh blank countdown.
    g_sleeping         = false;
    g_blank_reassert   = false;
    g_wake_grace_until = 0;
    drain_blank();
    g_display_off = true;
    wake_display();
    lock_sync_outputs();
    redraw_lock();
    lock_reclaim();
    if (g_locked && !g_busy) arm_blank();
    idle_apply();
    if (g_bar) g_bar->ping_watchdog();
    return 0;
}

void watch_sleep() {
    if (g_login_pump.bus || !g_bar) return;
    sd_bus* b = nullptr;
    if (sd_bus_open_system(&b) < 0) {
        fprintf(stderr, "mattbar: lock: no system bus for PrepareForSleep\n");
        return;
    }
    int r = sd_bus_match_signal(
        b, nullptr, "org.freedesktop.login1", "/org/freedesktop/login1",
        "org.freedesktop.login1.Manager", "PrepareForSleep",
        on_prepare_for_sleep, nullptr);
    if (r < 0) {
        fprintf(stderr, "mattbar: lock: PrepareForSleep match failed (%s)\n",
                strerror(-r));
        sd_bus_unref(b);
        return;
    }
    g_login_pump.attach(*g_bar, b, "login1-sleep");
    g_login_pump.process();
    fprintf(stderr,
            "mattbar: lock: watching PrepareForSleep (suspend/hibernate)\n");
}

uint64_t now_ms_lock() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

void disarm_blank() {
    if (g_blank_fd < 0) return;
    itimerspec off{};
    timerfd_settime(g_blank_fd, 0, &off, nullptr);
}

void drain_blank() {
    disarm_blank();
    g_blank_reassert = false;
    if (g_blank_fd < 0) return;
    uint64_t x;
    while (read(g_blank_fd, &x, sizeof x) > 0) {}
}

void arm_blank() {
    if (g_blank_fd < 0 || !g_locked) return;
    if (cfg.idle_blank_s <= 0) {
        disarm_blank();
        return;
    }
    g_blank_reassert = false;
    g_blank_armed_ms = now_ms_lock();
    itimerspec ts{};
    ts.it_value.tv_sec = cfg.idle_blank_s;
    timerfd_settime(g_blank_fd, 0, &ts, nullptr);
}

void do_blank(bool reassert) {
    fprintf(stderr, "mattbar: lock: blanking display%s\n",
            reassert ? " (settle)" : "");
    spawn_detached("omarchy-brightness-keyboard off; "
                   "omarchy-brightness-display off");
    g_display_off       = true;
    if (auto* nd = notify_daemon()) nd->refresh_popups();
    g_wake_grace_until  = now_ms_lock() + 2000;
    // hyprland misc.mouse_move_enables_dpms turns the panel back on from
    // the pointer event the modeset itself generates. Ignore that burst,
    // then DPMS-off once more so the dim sticks.
    if (!reassert && g_blank_fd >= 0) {
        g_blank_reassert   = true;
        g_blank_armed_ms   = now_ms_lock();
        itimerspec ts{};
        ts.it_value.tv_sec  = 1;
        ts.it_value.tv_nsec = 500000000L;
        timerfd_settime(g_blank_fd, 0, &ts, nullptr);
    } else {
        g_blank_reassert = false;
    }
}

void wake_display() {
    if (g_display_off) {
        spawn_detached("omarchy-system-wake");
        g_display_off = false;
    }
}

void note_activity(NoteSrc src) {
    uint64_t now = now_ms_lock();
    if (src == NoteMotion && now < g_wake_grace_until) return;
    g_wake_grace_until = 0;
    g_blank_reassert   = false;
    bool was_off = g_display_off;
    wake_display();
    if (was_off)
        if (auto* nd = notify_daemon()) nd->refresh_popups();
    if (g_locked && !g_busy) arm_blank();
}

void disarm_reclaim() {
    if (g_reclaim_fd < 0) return;
    itimerspec off{};
    timerfd_settime(g_reclaim_fd, 0, &off, nullptr);
}

struct LockBuf {
    wl_buffer* wl   = nullptr;
    void*      data = nullptr;
    size_t     size = 0;
    int        w = 0, h = 0;
    bool       busy   = false;
    bool       orphan = false;
};

struct LSurf {
    wl_output*                     out  = nullptr;
    wl_surface*                    surf = nullptr;
    ext_session_lock_surface_v1*   ls   = nullptr;
    uint32_t                       serial = 0;
    int                            w = 0, h = 0;
    bool                           configured = false;
    cairo_surface_t*               bg = nullptr; // buffer-pixel frost + overlay
    int                            bg_bw = 0, bg_bh = 0;
    cairo_surface_t*               bg_src = nullptr;
    Color                          bg_color{};
    LockBuf*                       pool[2] = {};
};

ext_session_lock_v1* g_lock     = nullptr;
bool                 g_finished = false;
std::vector<LSurf*>  g_surfs;
TextField            g_field;
std::string          g_error;
int                  g_pam_fd   = -1;
pid_t                g_pam_pid  = -1;
pid_t                g_fp_pid   = -1;
int                  g_fp_retry = -1;
int                  g_wait_fd  = -1;
bool                 g_fp_ok    = false;
AsyncCmd             g_fp_probe;
bool                 g_lock_dirty = false;

const char* pam_service() {
    if (access("/etc/pam.d/omarchy-lock-password", R_OK) == 0)
        return "omarchy-lock-password";
    return "login";
}

// Best-effort wipe. A volatile walk so the store cannot be DSE'd after
// the last read of the secret. No libc feature-macro requirement.
void wipe_mem(void* p, size_t n) {
    if (!p || n == 0) return;
    volatile unsigned char* v = static_cast<volatile unsigned char*>(p);
    while (n--) *v++ = 0;
}

void wipe_string(std::string& s) {
    if (!s.empty()) wipe_mem(s.data(), s.size());
    s.clear();
}

// Session account for PAM. Never invent "root" — a getpwuid miss used to
// authenticate the typed password against root, which is fail-open.
bool session_username(std::string& out) {
    out.clear();
    const passwd* pw = getpwuid(getuid());
    if (!pw || !pw->pw_name || !pw->pw_name[0]) return false;
    out = pw->pw_name;
    return true;
}

void draw_one(LSurf& s);
void request_lock_draw() { g_lock_dirty = true; }

void set_error(const std::string& e) {
    g_error = e;
    g_field.clear();
    for (auto* s : g_surfs) draw_one(*s);
}

void unlock();
void start_fingerprint();
void stop_fingerprint();
void ensure_waiter();

bool fingerprint_pam_present() {
    return access("/etc/pam.d/omarchy-lock-fingerprint", R_OK) == 0;
}

int pam_conv_cb(int num_msg, const struct pam_message** msg,
                struct pam_response** resp, void* ud) {
    auto* pw = static_cast<const char*>(ud);
    if (num_msg <= 0) return PAM_CONV_ERR;
    auto* r = static_cast<pam_response*>(
        calloc((size_t)num_msg, sizeof(pam_response)));
    if (!r) return PAM_CONV_ERR;
    for (int i = 0; i < num_msg; ++i) {
        if (msg[i]->msg_style == PAM_PROMPT_ECHO_OFF ||
            msg[i]->msg_style == PAM_PROMPT_ECHO_ON) {
            r[i].resp = strdup(pw ? pw : "");
        }
    }
    *resp = r;
    return PAM_SUCCESS;
}

// SIGTERM, one WNOHANG, then SIGKILL. Never waitpid(..., 0) here —
// pam_authenticate / fprintd can block for seconds and would freeze
// the lock surface and the watchdog. A leftover pid is reaped by
// ensure_waiter(); the killed flag stops a late exit-0 from unlocking.
void reap_or_kill(pid_t& pid, bool& killed) {
    if (pid <= 0) return;
    killed = true;
    kill(pid, SIGTERM);
    int   st = 0;
    pid_t w  = waitpid(pid, &st, WNOHANG);
    if (w == pid || (w < 0 && errno == ECHILD)) {
        pid    = -1;
        killed = false;
        return;
    }
    kill(pid, SIGKILL);
    w = waitpid(pid, &st, WNOHANG);
    if (w == pid || (w < 0 && errno == ECHILD)) {
        pid    = -1;
        killed = false;
    }
}

bool g_pam_killed = false;
bool g_fp_killed  = false;

void stop_password_pam() {
    reap_or_kill(g_pam_pid, g_pam_killed);
    g_pam_fd = -1;
    g_busy   = false;
}

void pam_done(bool ok) {
    if (ok) {
        stop_password_pam();
        stop_fingerprint();
        unlock();
        return;
    }
    g_busy    = false;
    g_pam_fd  = -1;
    g_pam_pid = -1;
    set_error("incorrect password");
    wake_display();
    arm_blank();
    start_fingerprint();
}

int fp_conv_cb(int num_msg, const struct pam_message** msg,
               struct pam_response** resp, void*) {
    if (num_msg <= 0) return PAM_CONV_ERR;
    auto* r = static_cast<pam_response*>(
        calloc((size_t)num_msg, sizeof(pam_response)));
    if (!r) return PAM_CONV_ERR;
    for (int i = 0; i < num_msg; ++i) {
        if (msg[i]->msg_style == PAM_PROMPT_ECHO_OFF ||
            msg[i]->msg_style == PAM_PROMPT_ECHO_ON)
            r[i].resp = strdup("");
    }
    *resp = r;
    return PAM_SUCCESS;
}

void stop_fingerprint() {
    reap_or_kill(g_fp_pid, g_fp_killed);
}

void start_fingerprint() {
    if (!g_locked || g_fp_pid > 0 || !g_fp_ok) return;
    std::string user;
    if (!session_username(user)) {
        fprintf(stderr, "mattbar: lock: fingerprint skipped; no session user\n");
        return;
    }
    pid_t pid = fork();
    if (pid == 0) {
        pam_handle_t* pamh = nullptr;
        pam_conv conv{fp_conv_cb, nullptr};
        int rc = pam_start("omarchy-lock-fingerprint", user.c_str(), &conv, &pamh);
        if (rc == PAM_SUCCESS) rc = pam_authenticate(pamh, 0);
        if (rc == PAM_SUCCESS) rc = pam_acct_mgmt(pamh, 0);
        if (pamh) pam_end(pamh, rc);
        _exit(rc == PAM_SUCCESS ? 0 : 1);
    }
    if (pid < 0) return;
    g_fp_killed = false;
    g_fp_pid    = pid;
}

void ensure_waiter() {
    if (g_wait_fd >= 0 || !g_bar) return;
    g_wait_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    if (g_wait_fd < 0) return;
    g_bar->add_fd(
        g_wait_fd,
        [](uint32_t) {
            uint64_t x;
            while (read(g_wait_fd, &x, sizeof x) > 0) {}
            int st = 0;
            if (g_pam_pid > 0) {
                pid_t r = waitpid(g_pam_pid, &st, WNOHANG);
                if (r == g_pam_pid) {
                    bool killed = g_pam_killed;
                    g_pam_pid    = -1;
                    g_pam_killed = false;
                    if (!killed)
                        pam_done(WIFEXITED(st) && WEXITSTATUS(st) == 0);
                }
            }
            if (g_fp_pid > 0) {
                pid_t r = waitpid(g_fp_pid, &st, WNOHANG);
                if (r == g_fp_pid) {
                    bool killed = g_fp_killed;
                    g_fp_pid    = -1;
                    g_fp_killed = false;
                    if (killed) {
                        /* aborted; ignore the exit status */
                    } else if (WIFEXITED(st) && WEXITSTATUS(st) == 0)
                        pam_done(true);
                    else if (g_locked && g_fp_ok && g_fp_retry >= 0) {
                        itimerspec ts{};
                        ts.it_value.tv_nsec = 300 * 1000000L;
                        timerfd_settime(g_fp_retry, 0, &ts, nullptr);
                    }
                }
            }
        },
        "lock-pam");
    itimerspec ts{};
    ts.it_interval.tv_nsec = 50 * 1000000L;
    ts.it_value.tv_nsec    = 50 * 1000000L;
    timerfd_settime(g_wait_fd, 0, &ts, nullptr);
}

void start_pam() {
    if (g_busy || !g_bar) return;
    std::string user;
    if (!session_username(user)) {
        wipe_string(g_field.text);
        g_field.cursor = 0;
        set_error("cannot resolve session user");
        return;
    }
    int p[2];
    if (pipe(p) != 0) return;
    pid_t pid = fork();
    if (pid == 0) {
        close(p[1]);
        std::string pw;
        char buf[256];
        ssize_t n;
        while ((n = read(p[0], buf, sizeof buf)) > 0) pw.append(buf, n);
        close(p[0]);
        wipe_mem(buf, sizeof buf);
        if (user.empty()) {
            wipe_string(pw);
            _exit(1);
        }
        pam_handle_t* pamh = nullptr;
        pam_conv conv{pam_conv_cb, const_cast<char*>(pw.c_str())};
        int rc = pam_start(pam_service(), user.c_str(), &conv, &pamh);
        if (rc == PAM_SUCCESS) rc = pam_authenticate(pamh, 0);
        if (rc == PAM_SUCCESS) rc = pam_acct_mgmt(pamh, 0);
        if (pamh) pam_end(pamh, rc);
        wipe_string(pw);
        _exit(rc == PAM_SUCCESS ? 0 : 1);
    }
    close(p[0]);
    if (pid < 0) {
        close(p[1]);
        return;
    }
    (void)!write(p[1], g_field.text.c_str(), g_field.text.size());
    close(p[1]);
    wipe_string(g_field.text);
    g_field.cursor = 0;
    g_pam_killed = false;
    g_busy    = true;
    disarm_blank();
    g_pam_pid = pid;
    g_error   = "";
    ensure_waiter();
    for (auto* s : g_surfs) draw_one(*s);
}

void rrect(cairo_t* cr, double x, double y, double w, double h, double r) {
    if (r < 0.5) {
        cairo_rectangle(cr, x, y, w, h);
        return;
    }
    r = std::min(r, std::min(w, h) / 2.0);
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -M_PI_2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI_2);
    cairo_arc(cr, x + r, y + h - r, r, M_PI_2, M_PI);
    cairo_arc(cr, x + r, y + r, r, M_PI, 1.5 * M_PI);
    cairo_close_path(cr);
}

void cover_image(cairo_t* cr, cairo_surface_t* img, int dw, int dh) {
    int sw = cairo_image_surface_get_width(img);
    int sh = cairo_image_surface_get_height(img);
    if (sw <= 0 || sh <= 0) return;
    double s = std::max((double)dw / sw, (double)dh / sh);
    cairo_save(cr);
    cairo_translate(cr, (dw - sw * s) / 2.0, (dh - sh * s) / 2.0);
    cairo_scale(cr, s, s);
    cairo_set_source_surface(cr, img, 0, 0);
    cairo_paint(cr);
    cairo_restore(cr);
}

void box_blur(cairo_surface_t* cs, int radius) {
    if (radius < 1) return;
    unsigned char* data = cairo_image_surface_get_data(cs);
    int w = cairo_image_surface_get_width(cs);
    int h = cairo_image_surface_get_height(cs);
    int stride = cairo_image_surface_get_stride(cs);
    if (!data || w < 1 || h < 1) return;
    int div = radius * 2 + 1;
    std::vector<unsigned char> tmp((size_t)h * stride);
    auto accum = [&](int x, int y, int* b, int* g, int* r, int* a) {
        x = std::clamp(x, 0, w - 1);
        y = std::clamp(y, 0, h - 1);
        unsigned char* p = data + y * stride + x * 4;
        *b += p[0];
        *g += p[1];
        *r += p[2];
        *a += p[3];
    };
    for (int y = 0; y < h; ++y) {
        int b = 0, g = 0, r = 0, a = 0;
        for (int x = -radius; x <= radius; ++x) accum(x, y, &b, &g, &r, &a);
        for (int x = 0; x < w; ++x) {
            unsigned char* d = tmp.data() + y * stride + x * 4;
            d[0] = (unsigned char)(b / div);
            d[1] = (unsigned char)(g / div);
            d[2] = (unsigned char)(r / div);
            d[3] = (unsigned char)(a / div);
            int xout = x - radius, xin = x + radius + 1;
            unsigned char* po =
                data + y * stride + std::clamp(xout, 0, w - 1) * 4;
            unsigned char* pi =
                data + y * stride + std::clamp(xin, 0, w - 1) * 4;
            b += (int)pi[0] - (int)po[0];
            g += (int)pi[1] - (int)po[1];
            r += (int)pi[2] - (int)po[2];
            a += (int)pi[3] - (int)po[3];
        }
    }
    memcpy(data, tmp.data(), tmp.size());
    for (int x = 0; x < w; ++x) {
        int b = 0, g = 0, r = 0, a = 0;
        for (int y = -radius; y <= radius; ++y) accum(x, y, &b, &g, &r, &a);
        for (int y = 0; y < h; ++y) {
            unsigned char* d = tmp.data() + y * stride + x * 4;
            d[0] = (unsigned char)(b / div);
            d[1] = (unsigned char)(g / div);
            d[2] = (unsigned char)(r / div);
            d[3] = (unsigned char)(a / div);
            int yout = y - radius, yin = y + radius + 1;
            unsigned char* po =
                data + std::clamp(yout, 0, h - 1) * stride + x * 4;
            unsigned char* pi =
                data + std::clamp(yin, 0, h - 1) * stride + x * 4;
            b += (int)pi[0] - (int)po[0];
            g += (int)pi[1] - (int)po[1];
            r += (int)pi[2] - (int)po[2];
            a += (int)pi[3] - (int)po[3];
        }
        for (int y = 0; y < h; ++y)
            memcpy(data + y * stride + x * 4, tmp.data() + y * stride + x * 4, 4);
    }
    cairo_surface_mark_dirty(cs);
}

void on_lock_buf_release(void* data, wl_buffer*) {
    auto* b = static_cast<LockBuf*>(data);
    b->busy = false;
    if (!b->orphan) return;
    if (b->wl) {
        wl_buffer_destroy(b->wl);
        b->wl = nullptr;
    }
    if (b->data) {
        munmap(b->data, b->size);
        b->data = nullptr;
    }
    delete b;
}

const wl_buffer_listener lock_buf_listener = {.release = on_lock_buf_release};

void drop_lock_buf(LockBuf*& b) {
    if (!b) return;
    if (b->busy) {
        b->orphan = true;
        b         = nullptr;
        return;
    }
    if (b->wl) wl_buffer_destroy(b->wl);
    if (b->data) munmap(b->data, b->size);
    delete b;
    b = nullptr;
}

LockBuf* make_lock_buf(int w, int h) {
    if (!g_bar || w < 1 || h < 1) return nullptr;
    const int    stride = w * 4;
    const size_t size   = static_cast<size_t>(stride) * h;
    int fd = memfd_create("mattbar-lock", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, static_cast<off_t>(size)) < 0) {
        if (fd >= 0) close(fd);
        return nullptr;
    }
    void* data = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        close(fd);
        return nullptr;
    }
    wl_shm_pool* pool = wl_shm_create_pool(g_bar->shm(), fd, static_cast<int>(size));
    wl_buffer*   buf  = wl_shm_pool_create_buffer(pool, 0, w, h, stride,
                                                WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    if (!buf) {
        munmap(data, size);
        return nullptr;
    }
    auto* b  = new LockBuf;
    b->wl    = buf;
    b->data  = data;
    b->size  = size;
    b->w     = w;
    b->h     = h;
    b->busy  = true;
    wl_buffer_add_listener(buf, &lock_buf_listener, b);
    return b;
}

LockBuf* grab_lock_buf(LSurf& s, int bw, int bh) {
    for (LockBuf*& slot : s.pool) {
        if (slot && !slot->busy && slot->w == bw && slot->h == bh) {
            slot->busy = true;
            return slot;
        }
    }
    for (LockBuf*& slot : s.pool) {
        if (slot && slot->busy) continue;
        drop_lock_buf(slot);
        slot = make_lock_buf(bw, bh);
        return slot;
    }
    LockBuf* extra = make_lock_buf(bw, bh);
    if (extra) extra->orphan = true;
    return extra;
}

void free_surf_resources(LSurf& s) {
    if (s.bg) {
        cairo_surface_destroy(s.bg);
        s.bg = nullptr;
    }
    s.bg_bw = s.bg_bh = 0;
    s.bg_src          = nullptr;
    drop_lock_buf(s.pool[0]);
    drop_lock_buf(s.pool[1]);
}

void rebuild_bg(LSurf& s, int bw, int bh) {
    if (s.bg) cairo_surface_destroy(s.bg);
    s.bg = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, bw, bh);
    cairo_t* cr = cairo_create(s.bg);
    cairo_scale(cr, (double)bw / s.w, (double)bh / s.h);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, cfg.c_bg.r, cfg.c_bg.g, cfg.c_bg.b, 1);
    cairo_paint(cr);
    int sw = std::max(80, s.w / 12);
    int sh = std::max(45, s.h / 12);
    cairo_surface_t* frost =
        cairo_image_surface_create(CAIRO_FORMAT_ARGB32, sw, sh);
    cairo_t* fr = cairo_create(frost);
    cairo_set_operator(fr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(fr, cfg.c_bg.r, cfg.c_bg.g, cfg.c_bg.b, 1);
    cairo_paint(fr);
    if (cairo_surface_t* src = wallpaper_image()) {
        cairo_set_operator(fr, CAIRO_OPERATOR_OVER);
        cover_image(fr, src, sw, sh);
    }
    cairo_destroy(fr);
    box_blur(frost, 2);
    box_blur(frost, 2);
    cairo_save(cr);
    cairo_scale(cr, (double)s.w / sw, (double)s.h / sh);
    cairo_set_source_surface(cr, frost, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BEST);
    cairo_paint(cr);
    cairo_restore(cr);
    cairo_surface_destroy(frost);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    cairo_set_source_rgba(cr, 0, 0, 0, 0.08);
    cairo_paint(cr);
    cairo_destroy(cr);
    cairo_surface_flush(s.bg);
    s.bg_bw    = bw;
    s.bg_bh    = bh;
    s.bg_src   = wallpaper_image();
    s.bg_color = cfg.c_bg;
}

void ensure_bg(LSurf& s, int bw, int bh) {
    cairo_surface_t* src = wallpaper_image();
    if (s.bg && s.bg_bw == bw && s.bg_bh == bh && s.bg_src == src &&
        s.bg_color.r == cfg.c_bg.r && s.bg_color.g == cfg.c_bg.g &&
        s.bg_color.b == cfg.c_bg.b)
        return;
    rebuild_bg(s, bw, bh);
}

void paint_field(cairo_t* cr, int w, int h) {
    const double fw = 381, fh = 67, outline = 3, radius = 16;
    const double fx = (w - fw) / 2.0, fy = (h - fh) / 2.0;
    const double heading = std::max(16.0, cfg.font_size * 1.5);
    const double field_fs = heading * 1.125;
    const double dot_fs   = heading * 1.33;
    const double gap      = heading * 0.19;

    cairo_select_font_face(cr, cfg.font.c_str(), CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_NORMAL);

    const char* fp_glyph = "\uF0237";
    cairo_set_font_size(cr, field_fs * 1.1);
    cairo_text_extents_t fpe{};
    if (g_fp_ok) cairo_text_extents(cr, fp_glyph, &fpe);
    double fp_reserve = g_fp_ok ? fpe.x_advance + 12 : 0;

    bool err = !g_error.empty();
    Color ring = err ? cfg.c_urgent : cfg.c_accent;
    rrect(cr, fx, fy, fw, fh, radius);
    cairo_set_source_rgba(cr, ring.r, ring.g, ring.b, 1);
    cairo_fill(cr);
    rrect(cr, fx + outline, fy + outline, fw - outline * 2, fh - outline * 2,
          std::max(0.0, radius - outline));
    cairo_set_source_rgba(cr, cfg.c_bg.r, cfg.c_bg.g, cfg.c_bg.b, 0.80);
    cairo_fill(cr);

    double inner_l = fx + outline + 18 + fp_reserve;
    double inner_r = fx + fw - outline - 18 - fp_reserve;
    double inner_w = std::max(8.0, inner_r - inner_l);
    double mid_y   = fy + fh / 2.0;

    g_field.password = true;
    g_field.focused  = true;

    auto vcenter = [&](const std::string& s, double x) {
        cairo_font_extents_t fe;
        cairo_font_extents(cr, &fe);
        cairo_move_to(cr, x, mid_y + (fe.ascent - fe.descent) / 2.0);
        cairo_show_text(cr, s.c_str());
    };

    if (g_field.text.empty()) {
        cairo_select_font_face(cr, cfg.font.c_str(),
                               err ? CAIRO_FONT_SLANT_ITALIC
                                   : CAIRO_FONT_SLANT_NORMAL,
                               CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, field_fs);
        const char* msg = g_busy ? "Checking\u2026"
                          : err  ? g_error.c_str()
                                 : "Enter Password";
        Color c = g_busy ? cfg.c_fg
                  : err  ? cfg.c_urgent
                         : Color{cfg.c_fg.r, cfg.c_fg.g, cfg.c_fg.b, 0.66};
        cairo_text_extents_t te;
        cairo_text_extents(cr, msg, &te);
        cairo_set_source_rgba(cr, c.r, c.g, c.b, err || g_busy ? 1.0 : 0.66);
        vcenter(msg, inner_l + (inner_w - te.x_advance) / 2.0);
    } else {
        cairo_select_font_face(cr, cfg.font.c_str(), CAIRO_FONT_SLANT_NORMAL,
                               CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, dot_fs);
        const char* dot = "\u25CF";
        cairo_text_extents_t de;
        cairo_text_extents(cr, dot, &de);
        int n = (int)g_field.text.size();
        double scale = 1.0;
        double total = n * de.x_advance + (n > 1 ? (n - 1) * gap : 0);
        if (total > inner_w && total > 0) scale = inner_w / total;
        cairo_set_font_size(cr, dot_fs * scale);
        cairo_text_extents(cr, dot, &de);
        double sp = gap * scale;
        total = n * de.x_advance + (n > 1 ? (n - 1) * sp : 0);
        double x = inner_l + (inner_w - total) / 2.0;
        cairo_set_source_rgba(cr, cfg.c_fg.r, cfg.c_fg.g, cfg.c_fg.b, 1);
        cairo_font_extents_t fe;
        cairo_font_extents(cr, &fe);
        double ty = mid_y + (fe.ascent - fe.descent) / 2.0;
        for (int i = 0; i < n; ++i) {
            cairo_move_to(cr, x, ty);
            cairo_show_text(cr, dot);
            x += de.x_advance + sp;
        }
        if (!g_busy && !err) {
            cairo_set_source_rgba(cr, cfg.c_fg.r, cfg.c_fg.g, cfg.c_fg.b, 1);
            cairo_rectangle(cr, x - sp / 2.0, fy + 16, 2, fh - 32);
            cairo_fill(cr);
        }
    }

    if (g_fp_ok) {
        cairo_select_font_face(cr, cfg.font.c_str(), CAIRO_FONT_SLANT_NORMAL,
                               CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, field_fs * 1.1);
        cairo_set_source_rgba(cr, cfg.c_fg.r, cfg.c_fg.g, cfg.c_fg.b, 0.66);
        cairo_text_extents_t te;
        cairo_text_extents(cr, fp_glyph, &te);
        vcenter(fp_glyph, fx + fw - outline - 18 - te.x_advance);
    }
}

void draw_one(LSurf& s) {
    // Session-lock surfaces: a commit with a null buffer is a protocol
    // error (ext_session_lock_surface_v1.error 1). Hyprland then kills
    // this client while leaving the session locked — black screen, no
    // password field, TTY switch to recover. Never commit without a
    // buffer that matches the last configure.
    if (!s.surf || !s.ls || !s.configured || s.w <= 0 || s.h <= 0 || !g_bar)
        return;
    int sc = g_bar->scale_of(s.out);
    if (sc < 1) sc = 1;
    int bw = s.w * sc, bh = s.h * sc;
    ensure_bg(s, bw, bh);
    LockBuf* slot = grab_lock_buf(s, bw, bh);
    if (!slot || !slot->wl || !slot->data) {
        fprintf(stderr, "mattbar: lock: shm %dx%d failed; not committing\n",
                bw, bh);
        return;
    }
    cairo_surface_t* cs = cairo_image_surface_create_for_data(
        static_cast<unsigned char*>(slot->data), CAIRO_FORMAT_ARGB32, bw, bh,
        bw * 4);
    cairo_t* cr = cairo_create(cs);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    if (s.bg) {
        cairo_set_source_surface(cr, s.bg, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
        cairo_paint(cr);
    } else {
        cairo_set_source_rgba(cr, cfg.c_bg.r, cfg.c_bg.g, cfg.c_bg.b, 1);
        cairo_paint(cr);
    }
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    cairo_scale(cr, (double)bw / s.w, (double)bh / s.h);
    paint_field(cr, s.w, s.h);
    cairo_destroy(cr);
    cairo_surface_destroy(cs);
    ext_session_lock_surface_v1_ack_configure(s.ls, s.serial);
    if (wl_surface_get_version(s.surf) >= 3)
        wl_surface_set_buffer_scale(s.surf, sc);
    wl_surface_attach(s.surf, slot->wl, 0, 0);
    if (wl_surface_get_version(s.surf) >= 4)
        wl_surface_damage_buffer(s.surf, 0, 0, bw, bh);
    else
        wl_surface_damage(s.surf, 0, 0, s.w, s.h);
    wl_surface_commit(s.surf);
}

void on_surf_configure(void* data, ext_session_lock_surface_v1*,
                       uint32_t serial, uint32_t w, uint32_t h) {
    auto* s = static_cast<LSurf*>(data);
    s->serial = serial;
    if (w) s->w = (int)w;
    if (h) s->h = (int)h;
    s->configured = true;
    draw_one(*s);
}

void on_key(const Bar::KeyEvent& e) {
    if (!g_locked) return;
    note_activity(NoteKey);
    if (g_busy) return;
    if (e.escape()) {
        g_field.clear();
        g_error.clear();
        request_lock_draw();
        return;
    }
    if (e.enter()) {
        if (g_field.text.empty()) return;
        start_pam();
        return;
    }
    if (g_field.handle(e)) {
        g_error.clear();
        request_lock_draw();
    }
}

void make_surf(wl_output* out) {
    if (!g_bar || !g_lock || !out) return;
    auto* s  = new LSurf;
    s->out   = out;
    s->surf  = wl_compositor_create_surface(g_bar->compositor());
    s->ls    = ext_session_lock_v1_get_lock_surface(g_lock, s->surf, out);
    static const ext_session_lock_surface_v1_listener lst = {
        .configure = on_surf_configure,
    };
    ext_session_lock_surface_v1_add_listener(s->ls, &lst, s);
    g_bar->register_surface(s->surf, Bar::SurfaceHooks{
                                         .motion = [](double, double) {
                                             note_activity(NoteMotion);
                                         },
                                         .button = [](int) {
                                             note_activity(NoteButton);
                                         },
                                         .key = [](const Bar::KeyEvent& e) {
                                             on_key(e);
                                         }});
    // Do not commit here. ext-session-lock-v1 sends configure as soon as
    // the lock surface is created. A commit before ack+buffer is
    // "Null buffer attached" on Hyprland (and a protocol error everywhere).
    g_surfs.push_back(s);
}

void destroy_surfs() {
    for (auto* s : g_surfs) {
        if (s->surf && g_bar) g_bar->unregister_surface(s->surf);
        if (s->ls) ext_session_lock_surface_v1_destroy(s->ls);
        if (s->surf) wl_surface_destroy(s->surf);
        free_surf_resources(*s);
        delete s;
    }
    g_surfs.clear();
    g_lock_dirty = false;
}

void on_locked(void*, ext_session_lock_v1*) {
    g_locked = true;
    disarm_reclaim();
    fprintf(stderr, "mattbar: lock: session locked\n");
    start_fingerprint();
    if (!g_sleeping) arm_blank();
}

void on_finished(void*, ext_session_lock_v1*) {
    g_finished = true;
    fprintf(stderr, "mattbar: lock: compositor finished the lock object\n");
    if (!g_locked) {
        destroy_surfs();
        if (g_lock) {
            ext_session_lock_v1_destroy(g_lock);
            g_lock = nullptr;
        }
    }
}

void redraw_lock() {
    g_lock_dirty = false;
    for (auto* s : g_surfs)
        if (s) draw_one(*s);
    if (g_bar && g_bar->display()) wl_display_flush(g_bar->display());
}

void unlock() {
    disarm_blank();
    idle_reset_session();
    wake_display();
    stop_fingerprint();
    if (g_lock) {
        if (g_locked)
            ext_session_lock_v1_unlock_and_destroy(g_lock);
        else
            ext_session_lock_v1_destroy(g_lock);
        g_lock     = nullptr;
        g_locked   = false;
        g_finished = false;
        g_error.clear();
        g_field.clear();
        destroy_surfs();
        if (g_bar && g_bar->display()) wl_display_flush(g_bar->display());
    }
    // After the lock has released the pointer: password keystrokes hid
    // the cursor (hide_on_key_press). Restoring while still locked, or
    // warping to (1,0), left it gone until a workspace switch.
    idle_restore_pointer();
    fprintf(stderr, "mattbar: lock: unlocked\n");
}

} // namespace

void lock_init(Bar& bar) {
    g_bar = &bar;
    // Previous process may have left ttfx up and the cursor hidden.
    idle_reset_session();
    ensure_waiter();
    if (g_blank_fd < 0) {
        g_blank_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
        if (g_blank_fd >= 0)
            bar.add_fd(
                g_blank_fd,
                [](uint32_t) {
                    uint64_t x;
                    while (read(g_blank_fd, &x, sizeof x) > 0) {}
                    if (!g_locked || g_busy || cfg.idle_blank_s <= 0) return;
                    if (g_sleeping) {
                        drain_blank();
                        return;
                    }
                    if (g_blank_reassert) {
                        g_blank_reassert = false;
                        do_blank(true);
                        return;
                    }
                    // Suspend/resume can fire a stale timer; match QS
                    // (Date.now() - armedAt > interval + 2000) → re-arm.
                    uint64_t age = now_ms_lock() - g_blank_armed_ms;
                    uint64_t lim =
                        (uint64_t)std::max(1, cfg.idle_blank_s) * 1000ull +
                        2000ull;
                    if (age > lim) {
                        arm_blank();
                        return;
                    }
                    do_blank(false);
                },
                "lock-blank");
    }
    if (g_fp_retry < 0) {
        g_fp_retry = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
        if (g_fp_retry >= 0)
            bar.add_fd(g_fp_retry, [](uint32_t) {
                uint64_t x;
                while (read(g_fp_retry, &x, sizeof x) > 0) {}
                start_fingerprint();
            }, "lock-fp-retry");
    }
    // Match Quickshell: only arm fingerprint PAM when the dedicated
    // service exists AND fprintd reports an enrolled finger. No sensor
    // and no enrollments stay a no-op.
    watch_sleep();
    // Hibernate resume can restart us after the PrepareForSleep(false)
    // signal has already been missed. Reclaim immediately.
    lock_reclaim();
    if (fingerprint_pam_present())
        g_fp_probe.run(
            bar,
            "command -v fprintd-list >/dev/null 2>&1 && "
            "fprintd-list \"$USER\" 2>/dev/null | grep -qi finger && "
            "echo yes || echo no",
            [](const std::string& out, int) {
                g_fp_ok = out.find("yes") != std::string::npos;
                if (g_fp_ok && g_locked) start_fingerprint();
                for (auto* s : g_surfs) draw_one(*s);
            },
            2500);
}

void lock_destroy() {
    if (g_locked) return;
    unlock();
}

bool lock_is_locked() { return g_locked; }

bool lock_display_asleep() { return g_display_off; }

std::string lock_status_json() {
    char buf[256];
    snprintf(buf, sizeof buf,
             "{\"locked\":%s,\"secure\":%s,\"requested\":%s,"
             "\"authenticating\":%s}",
             g_locked ? "true" : "false", g_locked ? "true" : "false",
             g_lock && !g_locked ? "true" : "false",
             g_busy ? "true" : "false");
    return buf;
}

std::string lock_now() {
    idle_stop_screensaver();
    if (g_locked) return "ok";
    if (access("/etc/pam.d/omarchy-lock-password", R_OK) != 0)
        return "missing-pam";
    if (!g_bar || !g_bar->lock_mgr())
        return "error: compositor has no ext-session-lock-v1";
    if (g_lock) return "ok";
    g_field.clear();
    g_error.clear();
    g_finished = false;
    g_lock = ext_session_lock_manager_v1_lock(g_bar->lock_mgr());
    static const ext_session_lock_v1_listener lst = {
        .locked   = on_locked,
        .finished = on_finished,
    };
    ext_session_lock_v1_add_listener(g_lock, &lst, nullptr);
    for (auto& o : g_bar->output_list()) make_surf(o.wl);
    ensure_waiter();
    // Sleep-lock polls `lock status` for secure=true and only gives the
    // lock IPC 1s. Put the protocol objects on the wire now; wallpaper
    // frost can catch up on the first configure/draw.
    if (g_bar->display()) wl_display_flush(g_bar->display());
    return "ok";
}

void lock_sync_outputs() {
    if (!g_bar || !g_lock) return;
    auto outs = g_bar->output_list();
    for (auto it = g_surfs.begin(); it != g_surfs.end();) {
        bool found = false;
        for (auto& o : outs)
            if (o.wl == (*it)->out) {
                found = true;
                break;
            }
        if (!found) {
            auto* s = *it;
            if (s->surf) g_bar->unregister_surface(s->surf);
            if (s->ls) ext_session_lock_surface_v1_destroy(s->ls);
            if (s->surf) wl_surface_destroy(s->surf);
            free_surf_resources(*s);
            delete s;
            it = g_surfs.erase(it);
        } else {
            ++it;
        }
    }
    for (auto& o : outs) {
        if (!o.wl) continue;
        bool have = false;
        for (auto* s : g_surfs)
            if (s->out == o.wl) {
                have = true;
                break;
            }
        if (!have) make_surf(o.wl);
    }
    if (g_bar->display()) wl_display_flush(g_bar->display());
}

void lock_flush() {
    if (!g_lock_dirty) return;
    redraw_lock();
}

static bool compositor_holds_lock() {
    // logind LockedHint drops when our lock client dies. Hyprland keeps
    // the session locked (solitaryBlockedBy contains LOCK) until a new
    // client takes over — same signal omarchy-hyprland-session-locked uses.
    std::string j = cmd_output("hyprctl -j monitors 2>/dev/null");
    size_t p = 0;
    while ((p = j.find("solitaryBlockedBy", p)) != std::string::npos) {
        size_t e = j.find(']', p);
        if (e == std::string::npos) break;
        if (j.substr(p, e - p).find("LOCK") != std::string::npos) return true;
        p = e + 1;
    }
    return false;
}

static bool session_locked_hint() {
    const char* sid = getenv("XDG_SESSION_ID");
    if (!sid || !*sid) return false;
    sd_bus* b = nullptr;
    if (sd_bus_open_system(&b) < 0) return false;
    char* path = nullptr;
    if (sd_bus_path_encode("/org/freedesktop/login1/session", sid, &path) < 0) {
        sd_bus_unref(b);
        return false;
    }
    int locked = 0;
    sd_bus_error err = SD_BUS_ERROR_NULL;
    int r = sd_bus_get_property_trivial(
        b, "org.freedesktop.login1", path, "org.freedesktop.login1.Session",
        "LockedHint", &err, 'b', &locked);
    sd_bus_error_free(&err);
    free(path);
    sd_bus_unref(b);
    return r >= 0 && locked;
}

void lock_reclaim() {
    if (!cfg.quickshell_shutdown) {
        g_reclaim_n = 0;
        disarm_reclaim();
        return;
    }
    if (g_locked) {
        disarm_reclaim();
        return;
    }
    if (!g_lock && (session_locked_hint() || compositor_holds_lock())) {
        fprintf(stderr,
                "mattbar: lock: session already locked; reclaiming surfaces\n");
        lock_now();
    }
    if (g_locked || g_reclaim_n >= 8 || !g_bar) return;
    if (g_reclaim_fd < 0) {
        g_reclaim_fd =
            timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
        if (g_reclaim_fd < 0) return;
        g_bar->add_fd(
            g_reclaim_fd,
            [](uint32_t) {
                uint64_t x;
                while (read(g_reclaim_fd, &x, sizeof x) > 0) {}
                lock_reclaim();
            },
            "lock-reclaim");
    }
    int ms = std::min(4000, 400 * (1 << std::min(g_reclaim_n, 3)));
    g_reclaim_n++;
    itimerspec ts{};
    ts.it_value.tv_sec  = ms / 1000;
    ts.it_value.tv_nsec = (ms % 1000) * 1000000L;
    timerfd_settime(g_reclaim_fd, 0, &ts, nullptr);
}

void lock_note_activity() { note_activity(NoteKey); }

#include "idle.hpp"
#include "bar.hpp"
#include "config.hpp"
#include "lock.hpp"
#include "modules.hpp"
#include "notify.hpp"
#include "util.hpp"

#include "ext-idle-notify-v1-client-protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <set>
#include <string>

namespace {

Bar* g_bar = nullptr;
ext_idle_notification_v1* notif = nullptr;
int  lock_fd = -1;
int  grace_fd = -1;
int  stale_fd = -1;
int  watch_fd = -1;
int  sock2_fd = -1;
std::string sock2_buf;
bool cycle    = false;
bool saver_on = false;
bool saver_started = false; // launched this cycle; lock only while it stays up
bool ignore_resume = false; // launch grace: mapping looks like activity
int  lock_s = 300, saver_s = 150;
int  armed_first_ms = -1;
uint64_t rearm_until = 0;
bool stay_awake = false;
std::set<std::string> saver_addrs;
constexpr const char* kSaverClass = "org.omarchy.screensaver";

uint64_t idle_now_ms() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

std::string hypr_dir() {
    const char* sig = getenv("HYPRLAND_INSTANCE_SIGNATURE");
    const char* rt  = getenv("XDG_RUNTIME_DIR");
    if (!sig || !*sig) return {};
    if (rt && *rt) {
        std::string p = std::string(rt) + "/hypr/" + sig;
        if (access((p + "/.socket2.sock").c_str(), F_OK) == 0) return p;
    }
    return {};
}

int unix_nb(const std::string& path) {
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return -1;
    sockaddr_un a{};
    a.sun_family = AF_UNIX;
    strncpy(a.sun_path, path.c_str(), sizeof a.sun_path - 1);
    if (connect(fd, (sockaddr*)&a, sizeof a) < 0 && errno != EINPROGRESS) {
        close(fd);
        return -1;
    }
    return fd;
}

bool pid_alive(const char* name) {
    int st = 1;
    cmd_output(std::string("pidof ") + name + " >/dev/null 2>&1", &st);
    return st == 0;
}

bool screensaver_proc() {
    if (pid_alive("ttfx") || pid_alive("omarchy-screensaver")) return true;
    int st = 1;
    cmd_output("pgrep -f '[o]rg.omarchy.screensaver' >/dev/null 2>&1", &st);
    return st == 0;
}

void arm_watch(bool on) {
    if (watch_fd < 0) return;
    itimerspec ts{};
    if (on) {
        ts.it_value.tv_sec    = 1;
        ts.it_interval.tv_sec = 1;
    }
    timerfd_settime(watch_fd, 0, &ts, nullptr);
}

void arm_grace(int ms) {
    if (grace_fd < 0) return;
    itimerspec ts{};
    if (ms > 0) {
        ts.it_value.tv_sec  = ms / 1000;
        ts.it_value.tv_nsec = (ms % 1000) * 1000000L;
    }
    timerfd_settime(grace_fd, 0, &ts, nullptr);
}

void saver_reset_windows() { saver_addrs.clear(); }

bool saver_up() { return !saver_addrs.empty() || screensaver_proc(); }

std::string stay_path() {
    const char* xdg = getenv("XDG_STATE_HOME");
    const char* h   = getenv("HOME");
    std::string base = xdg && *xdg
                           ? std::string(xdg)
                           : std::string(h ? h : ".") + "/.local/state";
    return base + "/omarchy/indicators/stay-awake";
}

std::string shell_json_path() {
    const char* h = getenv("HOME");
    return std::string(h ? h : ".") + "/.config/omarchy/shell.json";
}

bool grab_idle_int(const std::string& js, const char* key, int& out) {
    size_t idle = js.find("\"idle\"");
    std::string pat = std::string("\"") + key + "\"";
    size_t p = js.find(pat, idle == std::string::npos ? 0 : idle);
    if (p == std::string::npos) return false;
    p = js.find(':', p + pat.size());
    if (p == std::string::npos) return false;
    int v = atoi(js.c_str() + p + 1);
    if (v < 0) return false;
    out = v;
    return true;
}

bool replace_idle_int(std::string& js, const char* key, int v) {
    size_t idle = js.find("\"idle\"");
    std::string pat = std::string("\"") + key + "\"";
    size_t p = js.find(pat, idle == std::string::npos ? 0 : idle);
    if (p == std::string::npos) return false;
    p = js.find(':', p + pat.size());
    if (p == std::string::npos) return false;
    size_t n = p + 1;
    while (n < js.size() && isspace((unsigned char)js[n])) ++n;
    size_t e = n;
    if (e < js.size() && js[e] == '-') ++e;
    while (e < js.size() && isdigit((unsigned char)js[e])) ++e;
    if (e == n) return false;
    js.replace(n, e - n, std::to_string(v));
    return true;
}

void read_timeouts() {
    lock_s  = 300;
    saver_s = 150;
    std::string js = slurp(shell_json_path());
    if (js.empty()) return;
    grab_idle_int(js, "lock", lock_s);
    grab_idle_int(js, "screensaver", saver_s);
    if (lock_s < 1) lock_s = 1;
    if (saver_s < 1) saver_s = 1;
}

void write_timeouts() {
    std::string path = shell_json_path();
    std::string js   = slurp(path);
    if (js.empty()) return;
    if (!replace_idle_int(js, "screensaver", saver_s)) return;
    if (!replace_idle_int(js, "lock", lock_s)) return;
    std::ofstream f(path);
    if (f) f << js;
}

void disarm_lock_timer() {
    if (lock_fd < 0) return;
    itimerspec off{};
    timerfd_settime(lock_fd, 0, &off, nullptr);
}

void disarm_stale_timer() {
    if (stale_fd < 0) return;
    itimerspec off{};
    timerfd_settime(stale_fd, 0, &off, nullptr);
}

// The saver is a terminal (--class org.omarchy.screensaver) running
// omarchy-screensaver, which itself loops ttfx. SIGKILL on the script
// skips its trap (which would close the terminal).
//
// Hyprland 0.56: WindowSelector is a window object / address / id, NOT
// `{ class = "..." }`. Passing that table to window.close/kill is the
// same as Super+W / killactive — it hits the FOCUSED client. That is
// what killed Brave on 1.39.23 startup. Only close windows whose class
// actually matches.
const char* kCloseSaverWin =
    "hyprctl eval '"
    "for _, w in ipairs(hl.get_windows() or {}) do "
    "  if w.class == \"org.omarchy.screensaver\" "
    "     or w.initial_class == \"org.omarchy.screensaver\" then "
    "    hl.dispatch(hl.dsp.window.close(w)); "
    "  end; "
    "end; "
    "hl.config({ cursor = { invisible = false } })"
    "' >/dev/null 2>&1 || true";

void stop_screensaver() {
    spawn_detached(
        std::string("pkill -x omarchy-screensaver >/dev/null 2>&1 || true; "
                    "pkill -x ttfx >/dev/null 2>&1 || true; ") +
        kCloseSaverWin +
        "; sleep 0.2; "
        "pkill -KILL -x ttfx >/dev/null 2>&1 || true; "
        "pkill -KILL -x omarchy-screensaver >/dev/null 2>&1 || true; " +
        kCloseSaverWin);
    saver_on = false;
}

void stop_screensaver_now() {
    cmd_output(std::string("pkill -x omarchy-screensaver >/dev/null 2>&1 || true; "
                           "pkill -x ttfx >/dev/null 2>&1 || true; ") +
               kCloseSaverWin);
    usleep(150000);
    cmd_output(std::string("pkill -KILL -x ttfx >/dev/null 2>&1 || true; "
                           "pkill -KILL -x omarchy-screensaver >/dev/null 2>&1 || true; ") +
               kCloseSaverWin);
    saver_on = false;
    saver_reset_windows();
}

void restore_pointer() {
    // hide_on_key_press (Omarchy default) hides the cursor on the
    // password keystrokes. A compositor warp counts as motion and
    // brings it back — but the old `move({ x = 1, y = 0 })` was an
    // absolute jump to the layout origin, so the cursor "disappeared"
    // until warp_on_change_workspace on a workspace switch.
    // Nudge from the current position instead, after a beat so the
    // session-lock surfaces have released the pointer.
    spawn_detached(
        "omarchy-system-wake >/dev/null 2>&1 || true; "
        "( sleep 0.08; "
        "  hyprctl eval 'hl.dsp.dpms(\"on\")' >/dev/null 2>&1 || true; "
        "  hyprctl eval 'hl.dsp.release_input_capture()' "
        ">/dev/null 2>&1 || true; "
        "  hyprctl eval '"
        "local function unhide() "
        "  local p = hl.get_cursor_pos(); "
        "  if not p then return end; "
        "  if p.x <= 1 and p.y <= 1 then "
        "    local m = hl.get_active_monitor(); "
        "    if m then "
        "      hl.dispatch(hl.dsp.cursor.move({ "
        "        x = (m.x or 0) + m.width / 2, "
        "        y = (m.y or 0) + m.height / 2 "
        "      })); "
        "    end; "
        "  else "
        "    hl.dispatch(hl.dsp.cursor.move({ x = p.x + 1, y = p.y })); "
        "    hl.dispatch(hl.dsp.cursor.move({ x = p.x, y = p.y })); "
        "  end; "
        "end; "
        "unhide(); "
        "hl.config({ cursor = { invisible = false } })"
        "' >/dev/null 2>&1 || true; "
        "  sleep 0.2; "
        "  hyprctl eval '"
        "local p = hl.get_cursor_pos(); "
        "if not p then return end; "
        "if p.x <= 1 and p.y <= 1 then "
        "  local m = hl.get_active_monitor(); "
        "  if m then "
        "    hl.dispatch(hl.dsp.cursor.move({ "
        "      x = (m.x or 0) + m.width / 2, "
        "      y = (m.y or 0) + m.height / 2 "
        "    })); "
        "  end; "
        "else "
        "  hl.dispatch(hl.dsp.cursor.move({ x = p.x + 1, y = p.y })); "
        "  hl.dispatch(hl.dsp.cursor.move({ x = p.x, y = p.y })); "
        "end"
        "' >/dev/null 2>&1 || true; "
        ") >/dev/null 2>&1");
}

void cancel_cycle(const char* why) {
    bool had = cycle || saver_on || saver_started;
    disarm_lock_timer();
    disarm_stale_timer();
    arm_watch(false);
    arm_grace(0);
    ignore_resume = false;
    if (!had) {
        saver_started = false;
        saver_reset_windows();
        return;
    }
    fprintf(stderr, "mattbar: idle: cancel (%s)\n", why ? why : "");
    bool saver = saver_on || saver_started;
    stop_screensaver();
    saver_started = false;
    saver_reset_windows();
    if (saver) restore_pointer();
    cycle = false;
    if (saver)
        if (auto* nd = notify_daemon()) nd->refresh_popups();
}

void start_cycle() {
    if (cycle || stay_awake || lock_is_locked()) return;
    cycle = true;
    fprintf(stderr, "mattbar: idle: cycle start saver=%ds lock=%ds\n", saver_s,
            lock_s);
    int first = std::min(saver_s, lock_s);
    int lock_delay  = lock_s - first;
    int saver_delay = saver_s - first;
    // We were already notified after `first` seconds of idle.
    // If lock is due now, never start the screensaver — lock_now kills
    // any leftover and the user would not see the saver anyway.
    if (lock_delay <= 0) {
        stop_screensaver();
        lock_now();
        cycle = false;
        return;
    }
    if (saver_delay <= 0) {
        spawn_detached("omarchy-launch-screensaver");
        saver_on      = true;
        saver_started = true;
        ignore_resume = true;
        saver_reset_windows();
        // Mapping the screensaver looks like seat activity. Keep the
        // lock timer armed during launch and while the window exists;
        // a closewindow (user dismissed it) cancels, matching Omarchy.
        arm_grace(3000);
        arm_watch(true);
        if (auto* nd = notify_daemon()) nd->refresh_popups();
    }
    if (lock_fd >= 0) {
        itimerspec ts{};
        ts.it_value.tv_sec = lock_delay;
        timerfd_settime(lock_fd, 0, &ts, nullptr);
    }
}

void arm_fresh_from_now() {
    int first = std::min(saver_s, lock_s);
    if (first < 1) first = 1;
    if (stale_fd < 0) return;
    itimerspec ts{};
    ts.it_value.tv_sec = first;
    timerfd_settime(stale_fd, 0, &ts, nullptr);
    fprintf(stderr,
            "mattbar: idle: stale idled (restart/rearm); counting a fresh "
            "%ds from now\n",
            first);
}

void on_idled(void*, ext_idle_notification_v1*) {
    if (!cfg.quickshell_shutdown || stay_awake) return;
    if (idle_now_ms() < rearm_until) {
        arm_fresh_from_now();
        return;
    }
    start_cycle();
}

void on_resumed(void*, ext_idle_notification_v1*) {
    // ext-idle-notify fires resumed ONCE when the seat leaves idle.
    // Swallowing that event (screensaver mapping) used to leave the lock
    // timer running with no further resume for real keyboard use.
    // While the saver is launching or on screen, keep the cycle; the
    // window closing is the dismiss signal. Otherwise this is activity.
    if (!cycle) return;
    if (ignore_resume || (saver_started && saver_up())) {
        fprintf(stderr,
                "mattbar: idle: activity while screensaver cycle; lock stays "
                "armed\n");
        return;
    }
    cancel_cycle("activity");
}

void on_saver_opened(const std::string& addr) {
    if (addr.empty()) return;
    saver_addrs.insert(addr);
    ignore_resume = false;
    arm_grace(0);
    fprintf(stderr, "mattbar: idle: screensaver window %s (%zu)\n",
            addr.c_str(), saver_addrs.size());
}

void on_saver_closed(const std::string& addr) {
    if (!saver_addrs.erase(addr)) return;
    fprintf(stderr, "mattbar: idle: screensaver window closed %s (%zu left)\n",
            addr.c_str(), saver_addrs.size());
    if (!cycle || !saver_started || !saver_addrs.empty()) return;
    // User dismissed the saver before the lock deadline.
    cancel_cycle("screensaver-dismissed");
}

void sock2_line(const std::string& l) {
    if (l.rfind("openwindow>>", 0) == 0) {
        // ADDRESS,WORKSPACE,CLASS,TITLE
        std::string rest = l.substr(12);
        size_t a = rest.find(',');
        size_t b = a == std::string::npos ? a : rest.find(',', a + 1);
        size_t c = b == std::string::npos ? b : rest.find(',', b + 1);
        if (c == std::string::npos) return;
        std::string addr = rest.substr(0, a);
        std::string cls  = rest.substr(b + 1, c - b - 1);
        if (cls == kSaverClass) on_saver_opened(addr);
        return;
    }
    if (l.rfind("closewindow>>", 0) == 0)
        on_saver_closed(l.substr(13));
}

void destroy_notif() {
    cancel_cycle("disarm");
    disarm_stale_timer();
    if (notif) {
        ext_idle_notification_v1_destroy(notif);
        notif = nullptr;
    }
    armed_first_ms = -1;
}

void create_notif() {
    destroy_notif();
    if (!g_bar || !g_bar->idle_notifier() || !g_bar->seat()) return;
    if (stay_awake || access(stay_path().c_str(), F_OK) == 0) {
        stay_awake = true;
        return;
    }
    read_timeouts();
    int first_ms = std::min(saver_s, lock_s) * 1000;
    if (first_ms < 1000) first_ms = 1000;
    notif = ext_idle_notifier_v1_get_idle_notification(
        g_bar->idle_notifier(), (uint32_t)first_ms, g_bar->seat());
    static const ext_idle_notification_v1_listener lst = {
        .idled   = on_idled,
        .resumed = on_resumed,
    };
    ext_idle_notification_v1_add_listener(notif, &lst, nullptr);
    armed_first_ms = first_ms;
    // Hyprland sends idled immediately if the seat is already idle
    // (typical after a crash/restart). Do not lock/screensaver on that.
    rearm_until = idle_now_ms() + 2500;
    fprintf(stderr, "mattbar: idle: watching after %dms\n", first_ms);
}

} // namespace

void idle_init(Bar& bar) {
    g_bar = &bar;
    read_timeouts();
    stay_awake = access(stay_path().c_str(), F_OK) == 0;
    idle_reset_session();
    restore_pointer();
    lock_fd    = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    grace_fd   = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    stale_fd   = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    if (lock_fd >= 0)
        bar.add_fd(
            lock_fd,
            [](uint32_t) {
                uint64_t x;
                while (read(lock_fd, &x, sizeof x) > 0) {}
                if (!cycle || stay_awake) return;
                if (saver_started && !saver_up()) {
                    cancel_cycle("screensaver-gone-at-lock");
                    return;
                }
                fprintf(stderr, "mattbar: idle: lock timeout\n");
                stop_screensaver();
                restore_pointer();
                lock_now();
                cycle         = false;
                saver_started = false;
                saver_reset_windows();
                arm_watch(false);
                arm_grace(0);
            },
            "idle-lock");
    if (grace_fd >= 0)
        bar.add_fd(
            grace_fd,
            [](uint32_t) {
                uint64_t x;
                while (read(grace_fd, &x, sizeof x) > 0) {}
                ignore_resume = false;
                if (!cycle || !saver_started) return;
                if (saver_up()) return;
                cancel_cycle("screensaver-not-running");
            },
            "idle-grace");
    if (stale_fd >= 0)
        bar.add_fd(
            stale_fd,
            [](uint32_t) {
                uint64_t x;
                while (read(stale_fd, &x, sizeof x) > 0) {}
                if (stay_awake || lock_is_locked()) return;
                fprintf(stderr, "mattbar: idle: fresh idle elapsed after rearm\n");
                start_cycle();
            },
            "idle-stale");
    watch_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    if (watch_fd >= 0)
        bar.add_fd(
            watch_fd,
            [](uint32_t) {
                uint64_t x;
                while (read(watch_fd, &x, sizeof x) > 0) {}
                if (!cycle || !saver_started || ignore_resume) return;
                if (saver_up()) return;
                cancel_cycle("screensaver-process-gone");
            },
            "idle-saver-watch");
    std::string hd = hypr_dir();
    if (!hd.empty()) {
        sock2_fd = unix_nb(hd + "/.socket2.sock");
        if (sock2_fd >= 0)
            bar.add_fd(
                sock2_fd,
                [](uint32_t) {
                    char    b[2048];
                    ssize_t n;
                    while ((n = read(sock2_fd, b, sizeof b)) > 0)
                        sock2_buf.append(b, n);
                    size_t p;
                    while ((p = sock2_buf.find('\n')) != std::string::npos) {
                        sock2_line(sock2_buf.substr(0, p));
                        sock2_buf.erase(0, p + 1);
                    }
                    if (n == 0 || (n < 0 && errno != EAGAIN &&
                                   errno != EWOULDBLOCK)) {
                        if (g_bar) g_bar->remove_fd(sock2_fd);
                        close(sock2_fd);
                        sock2_fd = -1;
                    }
                },
                "idle-sock2");
        else
            fprintf(stderr, "mattbar: idle: no hyprland socket2; "
                            "screensaver dismiss watched via process only\n");
    }
}

void idle_apply() {
    if (!cfg.quickshell_shutdown) {
        destroy_notif();
        idle_reset_session();
        return;
    }
    stay_awake = access(stay_path().c_str(), F_OK) == 0;
    if (stay_awake) {
        destroy_notif();
        return;
    }
    read_timeouts();
    int first_ms = std::min(saver_s, lock_s) * 1000;
    if (first_ms < 1000) first_ms = 1000;
    // Recreating the notification while the seat is already idle makes
    // Hyprland fire idled immediately. Settings/takeover apply used to
    // do that on every click.
    if (notif && armed_first_ms == first_ms) return;
    create_notif();
}

std::string idle_status_json() {
    char buf[256];
    snprintf(buf, sizeof buf,
             "{\"enabled\":%s,\"stayAwake\":%s,\"screensaver\":%d,"
             "\"lock\":%d,\"blank\":%d,\"inIdleCycle\":%s}",
             (!stay_awake && cfg.quickshell_shutdown) ? "true" : "false",
             stay_awake ? "true" : "false", saver_s, lock_s, cfg.idle_blank_s,
             cycle ? "true" : "false");
    return buf;
}

int idle_screensaver_s() { return saver_s; }
int idle_lock_s() { return lock_s; }

void idle_set_screensaver_s(int s) {
    saver_s = std::clamp(s, 15, 3600);
    write_timeouts();
    if (cfg.quickshell_shutdown && !stay_awake) create_notif();
}

void idle_set_lock_s(int s) {
    lock_s = std::clamp(s, 15, 7200);
    write_timeouts();
    if (cfg.quickshell_shutdown && !stay_awake) create_notif();
}

void idle_stop_screensaver() { stop_screensaver_now(); }

bool idle_screensaver_up() { return saver_on || saver_started; }

void idle_reset_session() {
    stop_screensaver_now();
    cycle         = false;
    saver_started = false;
    ignore_resume = false;
    saver_reset_windows();
    disarm_lock_timer();
    disarm_stale_timer();
    arm_watch(false);
    arm_grace(0);
}

void idle_restore_pointer() { restore_pointer(); }

void idle_shutdown() {
    destroy_notif();
    idle_reset_session();
    restore_pointer();
}

std::string idle_set_enabled(bool on) {
    // Omarchy: enabled idle == !stayAwake
    stay_awake = !on;
    std::string p = stay_path();
    if (stay_awake) {
        auto slash = p.rfind('/');
        if (slash != std::string::npos) {
            std::string dir = p.substr(0, slash);
            auto slash2     = dir.rfind('/');
            if (slash2 != std::string::npos)
                mkdir(dir.substr(0, slash2).c_str(), 0700);
            mkdir(dir.c_str(), 0700);
        }
        int fd = open(p.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        if (fd >= 0) close(fd);
        cancel_cycle("stay-awake");
        destroy_notif();
        return "disabled";
    }
    unlink(p.c_str());
    if (cfg.quickshell_shutdown) create_notif();
    return "enabled";
}

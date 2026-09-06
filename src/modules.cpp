#include "modules.hpp"
#include "bar.hpp"
#include "config.hpp"
#include "nightlight.hpp"
#include "shell.hpp"
#include "idle-inhibit-unstable-v1-client-protocol.h"
#include "notify.hpp"
#include "sdpump.hpp"
#include "audio.hpp"
#include "popup.hpp"
#include "sensors.hpp"
#include "util.hpp"
#include "wallpaper.hpp"

#include <csignal>
#include <dirent.h>
#include <poll.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <sys/inotify.h>
#include <systemd/sd-bus.h>
#include <sys/signalfd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <sys/socket.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/genetlink.h>
#include <linux/nl80211.h>
#include <net/if.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>
#include <map>
#include <memory>
#include <string>
#include <vector>

#ifndef DBG
#define DBG(...)                                                              \
    do {                                                                      \
        if (getenv("MATTBAR_DEBUG")) {                                        \
            fprintf(stderr, "mattbar: " __VA_ARGS__);                         \
            fputc('\n', stderr);                                             \
        }                                                                     \
    } while (0)
#endif

// Launch a command fully detached; never blocks the bar. Double-fork +
// setsid so the child is not in MattBar's systemd cgroup. `system("foo &")`
// left browsers/IDEs in mattbar.service; a restart then SIGTERM'd them
// (Brave/Spotify/Signal dumped core) and hung 90s on jetbrainsd (16.7G
// attributed to the unit).
void spawn_detached(const std::string& c) {
    if (c.empty()) return;
    pid_t pid = fork();
    if (pid < 0) return;
    if (pid > 0) {
        waitpid(pid, nullptr, 0);
        return;
    }
    if (setsid() < 0) _exit(127);
    pid_t g = fork();
    if (g < 0) _exit(127);
    if (g > 0) _exit(0);
    uid_t uid = getuid();
    char cg[160];
    snprintf(cg, sizeof cg,
             "/sys/fs/cgroup/user.slice/user-%u.slice/user@%u.service/"
             "cgroup.procs",
             (unsigned)uid, (unsigned)uid);
    int cfd = open(cg, O_WRONLY | O_CLOEXEC);
    if (cfd >= 0) {
        char b[32];
        int n = snprintf(b, sizeof b, "%d\n", (int)getpid());
        (void)!write(cfd, b, (size_t)n);
        close(cfd);
    }
    int z = open("/dev/null", O_RDWR | O_CLOEXEC);
    if (z >= 0) {
        dup2(z, 0);
        dup2(z, 1);
        dup2(z, 2);
        if (z > 2) close(z);
    }
    execl("/bin/sh", "sh", "-c", c.c_str(), (char*)nullptr);
    _exit(127);
}

static const char* kPowerStateDir =
    "${XDG_STATE_HOME:-$HOME/.local/state}/omarchy/powerprofiles";

void persist_power_profile(const std::string& profile) {
    if (profile.empty()) return;
    // Quote via single quotes after rejecting quotes in the profile name
    // (PPD profiles are power-saver|balanced|performance).
    for (char c : profile)
        if (c == '\'' || c == '/' || c == '\n') return;
    spawn_detached(std::string("d=\"") + kPowerStateDir +
                   "\"; mkdir -p \"$d\"; printf '%s\\n' '" + profile +
                   "' >\"$d/ac\"; printf '%s\\n' '" + profile +
                   "' >\"$d/battery\"; powerprofilesctl set '" + profile +
                   "' >/dev/null 2>&1 || true");
}

std::string with_preserved_power_profile(const std::string& cmd) {
    // Pin first (so QS autodetect reads the right files), run the command,
    // then set again after a delay so a UPower.onBatteryChanged on QS
    // startup cannot leave balanced in place of power-saver.
    return std::string("p=$(powerprofilesctl get 2>/dev/null || true); "
                       "d=\"") +
           kPowerStateDir +
           "\"; "
           "if [ -n \"$p\" ]; then "
           "mkdir -p \"$d\"; printf '%s\\n' \"$p\" >\"$d/ac\"; "
           "printf '%s\\n' \"$p\" >\"$d/battery\"; fi; "
           "(" +
           cmd +
           "); st=$?; "
           "if [ -n \"$p\" ]; then "
           "sleep 1; powerprofilesctl set \"$p\" >/dev/null 2>&1 || true; "
           "fi; exit $st";
}

std::string live_panel_click(const std::string& stored, const char* overlay_id) {
    auto* sh = mattbar_shell();
    // Second click on the same module closes our TUI, even if Quickshell
    // is still the default for opening (same as Omarchy's bar buttons).
    if (sh && overlay_id && *overlay_id && sh->is_open(overlay_id)) {
        sh->hide(overlay_id);
        return {};
    }
    if (!cfg.quickshell_shutdown || !overlay_id || !*overlay_id) return stored;
    const bool stock = stored.find(overlay_id) != std::string::npos ||
                       stored.find("omarchy-launch-") != std::string::npos ||
                       stored.find("omarchy-shell") != std::string::npos ||
                       stored.find("mattbarctl") != std::string::npos;
    if (!stock) return stored;
    return std::string("mattbarctl shell toggle ") + overlay_id;
}

// ---------------------------------------------------------------------------
// AsyncCmd
// ---------------------------------------------------------------------------
void AsyncCmd::run(Bar& bar, const std::string& cmd, Done cb,
                   int timeout_ms, Line on_line) {
    bar_        = &bar;
    cb_         = std::move(cb);
    on_line_    = std::move(on_line);
    timeout_ms_ = timeout_ms;
    if (pid_ > 0) { // coalesce: latest request wins, runs after current
        pending_     = true;
        pending_cmd_ = cmd;
        return;
    }
    start(cmd);
}

void AsyncCmd::start(const std::string& cmd) {
    int p[2];
    if (pipe2(p, O_CLOEXEC) != 0) return;
    pid_t pid = fork();
    if (pid == 0) {
        dup2(p[1], 1);
        dup2(p[1], 2);
        close(p[0]);
        execl("/bin/sh", "sh", "-c", cmd.c_str(), (char*)nullptr);
        _exit(127);
    }
    close(p[1]);
    if (pid < 0) {
        close(p[0]);
        return;
    }
    pid_ = pid;
    fd_  = p[0];
    buf_.clear();
    line_pos_ = 0;
    fcntl(fd_, F_SETFL, O_NONBLOCK);
    bar_->add_fd(fd_, [this](uint32_t ev) {
        char    b[1024];
        ssize_t n;
        while ((n = read(fd_, b, sizeof b)) > 0) {
            buf_.append(b, n);
            drain_lines();
        }
        if (ev & (EPOLLHUP | EPOLLERR)) {
            int st = -1, ws = 0;
            if (waitpid(pid_, &ws, 0) == pid_ && WIFEXITED(ws))
                st = WEXITSTATUS(ws);
            finish(st);
        }
    }, "async-cmd");
    if (timer_fd_ < 0) {
        timer_fd_ = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
        bar_->add_fd(timer_fd_, [this](uint32_t) {
            uint64_t v;
            while (read(timer_fd_, &v, sizeof v) > 0) {}
            if (pid_ > 0) { // overdue: kill it, deliver what we have
                kill(pid_, SIGKILL);
                waitpid(pid_, nullptr, 0);
                finish(-1);
            }
        }, "async-cmd-timeout");
    }
    itimerspec ts{};
    ts.it_value.tv_sec  = timeout_ms_ / 1000;
    ts.it_value.tv_nsec = (timeout_ms_ % 1000) * 1000000L;
    timerfd_settime(timer_fd_, 0, &ts, nullptr);
}

void AsyncCmd::drain_lines() {
    if (!on_line_) return;
    size_t p;
    while ((p = buf_.find('\n', line_pos_)) != std::string::npos) {
        on_line_(buf_.substr(line_pos_, p - line_pos_));
        line_pos_ = p + 1;
    }
}

void AsyncCmd::cancel() {
    pending_ = false;
    if (pid_ <= 0) return;
    kill(pid_, SIGTERM);
}

void AsyncCmd::finish(int status) {
    if (on_line_ && line_pos_ < buf_.size()) {
        on_line_(buf_.substr(line_pos_));
        line_pos_ = buf_.size();
    }
    itimerspec off{};
    if (timer_fd_ >= 0) timerfd_settime(timer_fd_, 0, &off, nullptr);
    if (fd_ >= 0) {
        bar_->remove_fd(fd_);
        close(fd_);
        fd_ = -1;
    }
    pid_ = -1;
    std::string out = std::move(buf_);
    buf_.clear();
    line_pos_ = 0;
    if (cb_) cb_(out, status);
    if (pending_) {
        pending_ = false;
        start(pending_cmd_);
    }
}

AsyncCmd::~AsyncCmd() {
    if (pid_ > 0) {
        kill(pid_, SIGKILL);
        waitpid(pid_, nullptr, 0);
    }
    if (fd_ >= 0) close(fd_);
    if (timer_fd_ >= 0) close(timer_fd_);
}

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------
namespace {

void col(cairo_t* cr, const Color& c, double a = -1) {
    cairo_set_source_rgba(cr, c.r, c.g, c.b, a < 0 ? c.a : a);
}
void set_color(cairo_t* cr, const Color& c) {
    cairo_set_source_rgba(cr, c.r, c.g, c.b, c.a);
}

double text_width(cairo_t* cr, const std::string& s) {
    cairo_text_extents_t ext;
    cairo_text_extents(cr, s.c_str(), &ext);
    return ext.x_advance;
}

// draw text vertically centered in bar of height h, left edge at x
void draw_text(cairo_t* cr, const std::string& s, double x, double h,
               const Color& c) {
    cairo_font_extents_t fe;
    cairo_font_extents(cr, &fe);
    set_color(cr, c);
    cairo_move_to(cr, x, h / 2.0 + (fe.ascent - fe.descent) / 2.0);
    cairo_show_text(cr, s.c_str());
}



// A module that just renders a cached line of text.
class TextModule : public Module {
public:
    // "width" = extent along the bar axis. Vertical bars stack text rows.
    double width(cairo_t* cr) override {
        if (built_vertical_ != cfg_vertical()) { // orientation flipped live
            built_vertical_ = cfg_vertical();
            tick(); // modules compose compact text for vertical bars
        }
        if (text_.empty()) return 0;
        if (!cfg_vertical()) return text_width(cr, text_);
        cairo_font_extents_t fe;
        cairo_font_extents(cr, &fe);
        return fe.ascent + fe.descent + 10;
    }
    void draw(cairo_t* cr, double a, double t) override {
        if (!cfg_vertical()) {
            draw_text(cr, text_, a, t, color_);
            return;
        }
        // vertical: center horizontally in the bar, ellipsize to fit
        std::string s = text_;
        double maxw = t - 8;
        while (s.size() > 1 && text_width(cr, s) > maxw)
            s.resize(s.size() - 1);
        cairo_font_extents_t fe;
        cairo_font_extents(cr, &fe);
        cairo_set_source_rgba(cr, color_.r, color_.g, color_.b, 1.0);
        cairo_move_to(cr, (t - text_width(cr, s)) / 2.0,
                      a + width(cr) / 2.0 + (fe.ascent - fe.descent) / 2.0);
        cairo_show_text(cr, s.c_str());
    }
protected:
    void set_text(Bar& bar, std::string t, Color c = cfg.c_fg) {
        if (t != text_ || c.r != color_.r || c.g != color_.g || c.b != color_.b) {
            text_  = std::move(t);
            color_ = c;
            bar.request_draw();
        }
    }
    std::string text_;
    Color color_ = cfg.c_fg;
    bool built_vertical_ = false;
};

} // namespace

// ---------------------------------------------------------------------------
// Clock
// ---------------------------------------------------------------------------
namespace {
class ClockModule : public TextModule {
public:
    bool enabled() const override { return cfg.show_clock; }
    void init(Bar& bar) override { bar_ = &bar; tick(); }
    // Left-click drops a month calendar below the clock (on the monitor
    // whose clock you clicked); click again to put it away.
    bool on_click(double, int button) override {
        if (button == BTN_RIGHT) {
            clock_cycle_format();
            tick();
            return true;
        }
        if (button == BTN_MIDDLE) {
            auto* sh = mattbar_shell();
            if (sh) sh->toggle("omarchy.clock-timezone", "");
            return true;
        }
        if (button != BTN_LEFT || !cfg.calendar_enabled || !bar_) return false;
        calendar_toggle(*bar_, this);
        return true;
    }
    void tick() override {
        time_t t = time(nullptr);
        tm lt;
        localtime_r(&t, &lt);
        char buf[96];
        const std::string& f =
            cfg_vertical() ? cfg.clock_format_vertical : cfg.clock_format;
        if (strftime(buf, sizeof buf, f.c_str(), &lt) == 0)
            strftime(buf, sizeof buf, "%H:%M", &lt); // bad format: stay sane
        set_text(*bar_, buf);
    }
private:
    Bar* bar_ = nullptr;
};
} // namespace
Module* make_clock() { return new ClockModule; }

void clock_cycle_format() {
    static const char* ring[][2] = {
        {"%a %d %b  %H:%M", "%H:%M"},
        {"%a %d %b  %I:%M %p", "%I:%M"},
        {"%a %d %b  %H:%M:%S", "%H:%M"},
        {"%H:%M", "%H:%M"},
        {"%a %H:%M", "%H:%M"},
    };
    constexpr int n = 5;
    int i = 0;
    for (; i < n; ++i)
        if (cfg.clock_format == ring[i][0]) break;
    i = (i + 1) % n;
    cfg.clock_format          = ring[i][0];
    cfg.clock_format_vertical = ring[i][1];
    cfg.save();
}


// Hyprland IPC helpers defined further down; declared inside the unnamed
// namespace so these names unify with their internal-linkage definitions.
namespace {
std::string hypr_socket_dir();
int         unix_connect(const std::string& path);
std::string hypr_request(const std::string& dir, const std::string& req);
bool        hypr_dispatch2(const std::string& dir, const std::string& legacy,
                           const std::string& lua);
} // namespace

// ---------------------------------------------------------------------------
// AI agent usage — a reader of Omarchy Quattro's agents data contract.
// Collectors (omarchy-agent-usage-*) write one display-ready JSON record per
// agent into $XDG_STATE_HOME/omarchy/agents/usage/; the Quickshell shell
// keeps them fresh on its own timers (it stays running even under the
// null-bar plugin). This module never collects anything: it watches the
// directory and shows the worst rate-limit percentage across ready agents.
// No records (Omarchy 3.x, shell stopped, no agents) => module hides.
// ---------------------------------------------------------------------------
class AgentsModule : public TextModule {
public:
    static AgentsModule* g;
    bool enabled() const override { return cfg.show_agents; }

    AgentsModule() { g = this; }
    ~AgentsModule() override {
        if (g == this) g = nullptr;
        if (ino_fd_ >= 0) close(ino_fd_);
        if (deb_fd_ >= 0) close(deb_fd_);
        if (sock2_fd_ >= 0) close(sock2_fd_);
        if (spawn_fd_ >= 0) close(spawn_fd_);
        if (init_fd_ >= 0) close(init_fd_);
        dismiss_.destroy();
    }

    void init(Bar& bar) override {
        bar_ = &bar;
        const char* sh = getenv("XDG_STATE_HOME");
        std::string st = sh && *sh ? sh
                                   : std::string(getenv("HOME") ? getenv("HOME")
                                                                : ".") +
                                         "/.local/state";
        dir_    = st + "/omarchy/agents/usage";
        ino_fd_ = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        deb_fd_ = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
        if (ino_fd_ >= 0) {
            bar.add_fd(ino_fd_, [this](uint32_t) {
                char buf[4096];
                while (read(ino_fd_, buf, sizeof buf) > 0) {}
                if (deb_fd_ >= 0) { // collectors write several files at once
                    itimerspec ts{};
                    ts.it_value.tv_nsec = 300 * 1000000L;
                    timerfd_settime(deb_fd_, 0, &ts, nullptr);
                }
            }, "agents-usage-dir");
            try_watch();
        }
        if (deb_fd_ >= 0)
            bar.add_fd(deb_fd_, [this](uint32_t) {
                uint64_t x;
                while (read(deb_fd_, &x, sizeof x) > 0) {}
                rescan();
            }, "agents-debounce");
        // Terminal-session popup plumbing: a socket2 stream tells us when
        // our terminal appears (openwindow), dies (closewindow), and when
        // focus leaves it (activewindow) — which, under focus-follows-
        // mouse, IS the mouse-out signal that dismisses the popup.
        hdir_ = hypr_socket_dir();
        DBG("agents: popup term='%s' class='%s' hypr-sockets=%s",
            cfg.agents_term.c_str(), cfg.agents_term_class.c_str(),
            hdir_.empty() ? "MISSING" : "ok");
        if (!hdir_.empty()) {
            sock2_fd_ = unix_connect(hdir_ + "/.socket2.sock");
            if (sock2_fd_ >= 0) {
                // unix_connect() returns a BLOCKING socket (fine for the
                // one-shot request path). A level-triggered drain loop on a
                // blocking fd hangs the event loop on the read after the
                // last byte — which the sd_notify watchdog then correctly
                // "fixes" by having systemd kill the bar, taking the pactl
                // subscribe child with it, in a 10-second loop. Nonblocking
                // is not optional here.
                fcntl(sock2_fd_, F_SETFL, O_NONBLOCK);
                bar.add_fd(sock2_fd_, [this](uint32_t) {
                    char buf[2048];
                    ssize_t n;
                    while ((n = read(sock2_fd_, buf, sizeof buf)) > 0)
                        sock2_buf_.append(buf, n);
                    size_t p;
                    while ((p = sock2_buf_.find('\n')) != std::string::npos) {
                        sock2_line(sock2_buf_.substr(0, p));
                        sock2_buf_.erase(0, p + 1);
                    }
                    if (n == 0) { // EOF: Hyprland restarted. A closed
                        // stream in level-triggered epoll spins forever;
                        // tear down and degrade the popup to panel-only.
                        bar_->remove_fd(sock2_fd_);
                        close(sock2_fd_);
                        sock2_fd_  = -1;
                        term_open_ = shown_ = spawning_ = on_special_ = false;
                        term_cls_.clear();
                        arm_spawn(false);
                        sock2_buf_.clear();
                    }
                }, "agents-socket2");
            }
        }
        // A spawn that never produces a window we recognise must not latch
        // into a silent click. This timer is armed with each spawn dispatch
        // and disarmed on adoption; if it fires, the click falls back to
        // the shell panel so the user always gets SOMETHING.
        spawn_fd_ = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
        if (spawn_fd_ >= 0)
            bar.add_fd(spawn_fd_, [this](uint32_t) {
                uint64_t x;
                while (read(spawn_fd_, &x, sizeof x) > 0) {}
                if (!spawning_) return;
                spawning_ = false;
                DBG("agents: no adoptable window within %d ms; "
                    "panel fallback (check agents_term / agents_term_class)",
                    SPAWN_TIMEOUT_MS);
                open_panel(false);
            }, "agents-spawn-timeout");
        // The compositor chatter below (rules, fallthrough probe,
        // session discovery) is up to ~13 sequential socket requests
        // with second-scale timeouts. Running it synchronously in init —
        // before the event loop starts pinging — let a slow/wedged
        // Hyprland stall past WatchdogSec and produce the 10-second
        // watchdog restart storms seen in the field. Deferred onto the
        // loop via a one-shot timer, with explicit pings between steps.
        init_fd_ = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
        if (init_fd_ >= 0) {
            itimerspec its{};
            its.it_value.tv_nsec = 300 * 1000000L;
            timerfd_settime(init_fd_, 0, &its, nullptr);
            bar.add_fd(init_fd_, [this, &bar](uint32_t) {
                uint64_t x;
                while (read(init_fd_, &x, sizeof x) > 0) {}
                if (init_done_) return;
                init_done_ = true;
                if (!hdir_.empty()) {
                    preinstall_rules();
                    bar.ping_watchdog();
                    if (cfg.agents_click_through) enable_click_through();
                    bar.ping_watchdog();
                }
                if (!hdir_.empty() && sock2_fd_ >= 0 && discover_session())
                    DBG("agents: found existing session window 0x%s from "
                        "a previous run (shown=%d)", term_addr_.c_str(),
                        (int)shown_);
                placed_size_ = popup_size();
                bar.ping_watchdog();
            }, "agents-deferred-init");
        }
        bar.set_click_observer([this](Module* m) {
            if (m == static_cast<Module*>(this)) return;
            if (shown_ && on_special_) {
                DBG("agents: bar interaction elsewhere; hiding the popup");
                show(false);
            }
        });
        rescan();
    }

    void tick() override {
        // usage dir may not exist yet (fresh install, first agent run):
        // cheap re-attempt while revealed, nothing scheduled while hidden
        if (ino_fd_ >= 0 && watch_ < 0) {
            try_watch();
            if (watch_ >= 0) rescan();
        }
        // Settings steppers persist agents_popup_size then apply_config
        // ticks modules: resize the live grok window in place.
        if (init_done_) {
            std::string sz = popup_size();
            if (sz != placed_size_) {
                placed_size_ = sz;
                if (!hdir_.empty()) preinstall_rules();
                sync_shown();
                if (term_open_ && on_special_ && shown_) place_popup();
            }
            if (shown_ && on_special_) {
                evict_strays();
                apply_dismiss_hole();
            }
        }
    }

    bool on_click(double, int button) override {
        if (button == BTN_RIGHT) {
            // Eject a live terminal popup; otherwise the same picker
            // Omarchy's agents widget launches.
            if (term_open_) { eject(); return true; }
            spawn_detached("omarchy-agent --pick");
            return true;
        }
        if (button == BTN_MIDDLE) {
            auto* sh = mattbar_shell();
            if (sh && sh->is_open("omarchy.agents")) {
                sh->call("omarchy.agents", "next", "");
                return true;
            }
        }
        if (button != BTN_LEFT && button != BTN_MIDDLE) return false;
        // Native panel when the shell can MEANINGFULLY represent the
        // default agent; terminal-session popup when it cannot. "Has a
        // usage record" is not enough: a stub or stale record (ready but
        // no limit percentages — e.g. a hand-rolled grok.json) gives the
        // panel nothing to show, while the terminal session is exactly
        // the task view the user wants for such agents. No default
        // chosen => panel, whose selection prompt is the right guidance.
        const std::string def = default_agent();
        bool panelable = def.empty();
        if (!def.empty()) {
            std::string j = slurp(dir_ + "/" + def + ".json");
            if (!j.empty()) {
                bool   rdy = false;
                double pc  = max_percent(j, rdy);
                panelable  = rdy && pc >= 0; // real, displayable limit data
                DBG("agents: click: default='%s' record ready=%d pct=%.0f%% "
                    "-> %s", def.c_str(), rdy, pc * 100.0,
                    panelable ? "panel" : "terminal popup");
            } else {
                DBG("agents: click: default='%s' has no usage record "
                    "-> terminal popup", def.c_str());
            }
        } else {
            DBG("agents: click: no default agent -> panel");
        }
        const bool popup_possible = !hdir_.empty() && sock2_fd_ >= 0;
        if (panelable || !popup_possible) {
            if (!popup_possible)
                DBG("agents: click: hypr sockets unavailable -> panel only");
            open_panel(popup_possible);
            return false;
        }
        // Glyph click is a real toggle. If the compositor already has the
        // popup up, hide it — never spawn a replacement. A mouse-out hide
        // that fires while the pointer travels onto this glyph must not
        // be followed by an immediate re-open (the field "click several
        // times to dismiss" loop).
        if (!term_open_) discover_session();
        suppress_dismiss_until_ = now_ms() + 400;
        sync_shown();
        if (term_open_ && on_special_ &&
            (shown_ || (hid_ms_ && now_ms() - hid_ms_ < 400))) {
            if (shown_) show(false);
            return true;
        }
        return spawn_popup(true);
    }

    double width(cairo_t* cr) override {
        resolve_glyph(cr); // validate against the real font every frame
        return TextModule::width(cr);
    }

private:
    const std::string& glyph() const {
        return resolved_.empty() ? cfg.agents_glyph : resolved_;
    }
    // Same coverage check the bluetooth/brightness glyphs use: ask the
    // scaled font for real glyph indices and fall back down the chain on
    // any .notdef. Configured glyph -> nf-md-robot (Nerd Fonts v3) ->
    // FA robot (v2 fonts) -> plain "AI".
    void resolve_glyph(cairo_t* cr) {
        if (checked_ == cfg.font + cfg.agents_glyph) return;
        checked_ = cfg.font + cfg.agents_glyph;
        auto mapped = [&](const std::string& t) {
            cairo_scaled_font_t* sf = cairo_get_scaled_font(cr);
            cairo_glyph_t*       g  = nullptr;
            int                  n  = 0;
            bool ok = cairo_scaled_font_text_to_glyphs(
                          sf, 0, 0, t.c_str(), (int)t.size(), &g, &n, nullptr,
                          nullptr, nullptr) == CAIRO_STATUS_SUCCESS &&
                      n > 0;
            for (int i = 0; ok && i < n; ++i)
                if (g[i].index == 0) ok = false;
            if (g) cairo_glyph_free(g);
            return ok;
        };
        std::string pick = cfg.agents_glyph;
        if (!mapped(pick)) pick = "\U000F06A9"; // nf-md-robot
        if (!mapped(pick)) pick = "\uf544";     // nf-fa-robot (NF v2)
        if (!mapped(pick)) pick = "AI";
        if (pick != resolved_) {
            resolved_ = pick;
            rescan();
        }
    }

    static constexpr int SPAWN_TIMEOUT_MS = 6000;
    // The panel toggle used to be fire-and-forget — a dead or missing
    // shell made it a perfectly silent click (Quattro shells that died,
    // e.g. to the old notifyd takeover, receive the toggle into the
    // void). Run it observably instead: a clear failure (command missing
    // or nonzero exit) falls back to the terminal popup when one is
    // possible. A timeout kill is NOT treated as failure — a customised
    // long-running agents_click is presumed to be doing its job.
    void open_panel(bool can_fallback) {
        if (auto* sh = mattbar_shell()) {
            if (cfg.quickshell_shutdown || sh->is_open("omarchy.agents")) {
                sh->toggle("omarchy.agents", "");
                return;
            }
        }
        if (cfg.agents_click.empty()) {
            DBG("agents: agents_click is empty; nothing to open");
            if (can_fallback) spawn_popup(false);
            return;
        }
        panel_cmd_.run(*bar_, cfg.agents_click,
                       [this, can_fallback](const std::string&, int status) {
                           if (status == 0 || status < 0) return;
                           DBG("agents: panel command exited %d; %s", status,
                               can_fallback ? "terminal popup fallback"
                                            : "no fallback available");
                           if (can_fallback) spawn_popup(false);
                       },
                       3000);
    }
    // A session that exits within 2 s of mapping is invisible to the
    // user. After the first such exit, the NEXT spawn wraps the stock
    // launcher in a hold-open shim: the popup then stays up displaying
    // omarchy-agent's output and exit status instead of vanishing — the
    // error diagnoses itself. Only the recognisable "... omarchy-agent"
    // stock tail is wrapped; a custom command is never rewritten.
    std::string spawn_cmd() const {
        const std::string& t = cfg.agents_term;
        if (!hold_next_) return t;
        for (const char* tail :
             {" omarchy-agent --inline", " omarchy-agent"}) {
            const size_t n = strlen(tail);
            if (t.size() > n && t.compare(t.size() - n, n, tail) == 0)
                return t.substr(0, t.size() - n) + " sh -c '" +
                       (tail + 1) +
                       "; printf \"\\n[omarchy-agent exited %s - press "
                       "Enter to close]\\n\" \"$?\"; read _'";
        }
        return t; // custom command: never rewritten
    }
    // Place the popup so it visually hangs off the bar instead of
    // floating mid-screen: flush to the bar's edge, aligned toward the
    // right module cluster where the agents glyph lives. Everything is
    // computed in monitor-% so one rule works on any resolution.
    static std::string popup_move() {
        int w = 36, h = 44;
        sscanf(popup_size().c_str(), "%d%% %d%%", &w, &h);
        int x, y;
        if (cfg.position == "bottom") { x = 100 - w - 1; y = 100 - h - 4; }
        else if (cfg.position == "left")  { x = 2;            y = 3; }
        else if (cfg.position == "right") { x = 100 - w - 2;  y = 3; }
        else /* top */                    { x = 100 - w - 1;  y = 3; }
        if (x < 0) x = 0;
        if (y < 0) y = 0;
        return std::to_string(x) + "% " + std::to_string(y) + "%";
    }
    static std::string addr_norm(std::string a) {
        if (a.rfind("0x", 0) == 0 || a.rfind("0X", 0) == 0) a = a.substr(2);
        return a;
    }
    std::string term_sel() const {
        return "address:0x" + addr_norm(term_addr_);
    }
    bool special_is_shown() {
        if (hdir_.empty()) return false;
        std::string mons = hypr_request(hdir_, "j/monitors");
        size_t      p    = 0;
        while ((p = mons.find("\"specialWorkspace\"", p)) !=
               std::string::npos) {
            size_t brace = mons.find('{', p);
            if (brace == std::string::npos) break;
            size_t end = mons.find('}', brace);
            if (end == std::string::npos) break;
            std::string block = mons.substr(brace, end - brace + 1);
            if (block.find("special:mbagent") != std::string::npos)
                return true;
            p = end + 1;
        }
        return false;
    }
    // Visible = the agent window is on a regular workspace. The special
    // is only a holding area while hidden; we never overlay it, because
    // togglespecialworkspace would swallow every other floater that
    // mapped there (file choosers, browsers, …).
    void sync_shown() {
        if (!term_open_ || term_addr_.empty()) {
            shown_ = false;
            return;
        }
        std::string c = client_chunk();
        if (c.empty()) return; // keep last bookkeeping if hypr json lags
        shown_ = c.find("\"name\": \"special:mbagent\"") == std::string::npos;
    }
    static bool is_dialog_class(const std::string& cls) {
        if (cls.empty()) return false;
        if (cls.find("xdg-desktop-portal") != std::string::npos) return true;
        if (cls.find("FileChooser") != std::string::npos) return true;
        return cls == "zenity" || cls == "kdialog" ||
               cls == "org.freedesktop.impl.portal.desktop.gtk" ||
               cls == "org.freedesktop.impl.portal.desktop.gnome" ||
               cls == "org.freedesktop.impl.portal.desktop.kde";
    }
    static bool is_dialog_title(const std::string& title) {
        auto has = [&](const char* s) {
            return title.find(s) != std::string::npos;
        };
        return has("Open File") || has("Save File") || has("Save As") ||
               has("Open Folder") || has("File Chooser") || has("Open Files") ||
               has("Save As…") || has("Save as");
    }
    bool move_term(const std::string& dest, bool silent) {
        if (hdir_.empty() || term_addr_.empty()) return false;
        const std::string sel = term_sel();
        const char* dsp = silent ? "movetoworkspacesilent" : "movetoworkspace";
        std::string legacy =
            std::string("dispatch ") + dsp + " " + dest + "," + sel;
        std::string lua =
            "dispatch hl.dsp.window.move({ window = \"" + sel +
            "\", workspace = \"" + dest + "\", follow = false })";
        return hypr_dispatch2(hdir_, legacy, lua);
    }
    void close_dismiss_catcher() { dismiss_.destroy(); }
    void apply_dismiss_hole() {
        if (!dismiss_.surf || !bar_ || !bar_->compositor()) return;
        int W = dismiss_.w, H = dismiss_.h;
        if (W <= 0 || H <= 0) return;
        int hx = 0, hy = 0, hw = 0, hh = 0;
        std::string c = client_chunk();
        if (!c.empty()) {
            std::string at = json_field(c, "at"), sz = json_field(c, "size");
            int ax = 0, ay = 0, aw = 0, ah = 0;
            if (sscanf(at.c_str(), "[%d ,%d]", &ax, &ay) == 2 &&
                sscanf(sz.c_str(), "[%d ,%d]", &aw, &ah) == 2 &&
                aw > 0 && ah > 0) {
                hx = ax;
                hy = ay;
                hw = aw;
                hh = ah;
            }
        }
        if (hw <= 0 || hh <= 0) {
            PopupPx g = popup_px();
            hx        = g.x;
            hy        = g.y;
            hw        = g.w;
            hh        = g.h;
        }
        if (hx < 0) hx = 0;
        if (hy < 0) hy = 0;
        if (hx + hw > W) hw = W - hx;
        if (hy + hh > H) hh = H - hy;
        if (hw < 1) hw = 1;
        if (hh < 1) hh = 1;
        wl_region* r = wl_compositor_create_region(bar_->compositor());
        if (!r) return;
        if (hy > 0) wl_region_add(r, 0, 0, W, hy);
        if (hy + hh < H) wl_region_add(r, 0, hy + hh, W, H - (hy + hh));
        if (hx > 0) wl_region_add(r, 0, hy, hx, hh);
        if (hx + hw < W) wl_region_add(r, hx + hw, hy, W - (hx + hw), hh);
        wl_surface_set_input_region(dismiss_.surf, r);
        wl_region_destroy(r);
        wl_surface_commit(dismiss_.surf);
    }
    void catcher_maybe_hide(bool from_click) {
        if (!shown_ || !on_special_) return;
        if (now_ms() < suppress_dismiss_until_) return;
        const uint64_t since = now_ms() - reveal_ms_;
        if (!popup_focused_) {
            foreign_seen_ = true;
            if (!from_click) return;
            if (since <= 400) return;
        }
        DBG("agents: dismiss catcher (%s); hiding",
            from_click ? "click" : "mouse-out");
        show(false);
    }
    void open_dismiss_catcher() {
        if (!bar_) return;
        dismiss_.kb_mode  = 0;
        dismiss_.layer    = ZWLR_LAYER_SHELL_V1_LAYER_TOP;
        dismiss_.centered = false;
        dismiss_.paint    = [](cairo_t*) {};
        dismiss_.click    = [this](double, double, int) {
            catcher_maybe_hide(true);
        };
        dismiss_.pmotion = [this](double, double) { catcher_maybe_hide(false); };
        dismiss_.on_configured = [this] { apply_dismiss_hole(); };
        wl_output* out = bar_->input_output();
        if (!out) out = bar_->focused_output();
        if (!out) out = bar_->primary_output();
        uint32_t a = ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
                     ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
                     ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
                     ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
        dismiss_.ensure(*bar_, a, 0, 0, 0, 0, "mattbar-dismiss", 0, 0, out);
    }
    // Hyprland 0.56 Lua `window.resize`/`window.move` take pixel coords.
    // Percent sizes in `resizewindowpixel` are a Lua syntax error there
    // and silently leave the default 800x600 float.
    struct PopupPx {
        int w, h, x, y;
    };
    PopupPx popup_px() {
        int pw = 36, ph = 44;
        sscanf(popup_size().c_str(), "%d%% %d%%", &pw, &ph);
        int         mw = 0, mh = 0;
        std::string mons = hypr_request(hdir_, "j/monitors");
        size_t      f    = mons.find("\"focused\": true");
        if (f == std::string::npos) f = mons.find("\"focused\":true");
        auto jint = [&](const char* k, size_t around) {
            size_t lo = around > 800 ? around - 800 : 0;
            size_t hi = std::min(mons.size(), around + 800);
            std::string slice = mons.substr(lo, hi - lo);
            std::string key   = std::string("\"") + k + "\":";
            size_t      p     = slice.find(key);
            if (p == std::string::npos) return 0;
            p += key.size();
            while (p < slice.size() &&
                   (slice[p] == ' ' || slice[p] == '\t'))
                ++p;
            return atoi(slice.c_str() + p);
        };
        if (f != std::string::npos) {
            mw = jint("width", f);
            mh = jint("height", f);
        }
        if (mw <= 0) mw = 1920;
        if (mh <= 0) mh = 1080;
        int w = mw * pw / 100, h = mh * ph / 100;
        if (w < 200) w = 200;
        if (h < 150) h = 150;
        int x, y;
        if (cfg.position == "bottom") {
            x = mw - w - mw / 100;
            y = mh - h - mh * 4 / 100;
        } else if (cfg.position == "left") {
            x = mw * 2 / 100;
            y = mh * 3 / 100;
        } else if (cfg.position == "right") {
            x = mw - w - mw * 2 / 100;
            y = mh * 3 / 100;
        } else {
            x = mw - w - mw / 100;
            y = mh * 3 / 100;
        }
        if (x < 0) x = 0;
        if (y < 0) y = 0;
        return {w, h, x, y};
    }
    // Focus the popup right after revealing it. This does two jobs: it
    // undoes the reveal-race focus bounce at the source (instead of
    // merely tolerating it through the grace period), and it makes
    // "click anywhere outside" dismiss the popup — with focus ON the
    // popup, any click on another window changes activewindow, which the
    // mouse-out handler already turns into a hide.
    // Right-click discoverability: the eject gesture lives on the bar
    // module (a terminal cannot host bar buttons) and has gone unnoticed
    // for several rounds — say so once per run via a notification.
    void hint_eject() {
        if (hinted_) return;
        hinted_ = true;
        spawn_detached(
            "notify-send MattBar 'Tip: RIGHT-CLICK the robot icon in the "
            "bar to pop this agent session out into a normal window.'");
    }
    void focus_popup() {
        if (term_addr_.empty()) return;
        // Focus the agent window by address. A workspace-wide focus
        // raises every client on special:mbagent — including Brave
        // windows that inherited the special via xdg-activation.
        if (!hypr_dispatch2(hdir_, "dispatch focuswindow " + term_sel(),
                            "dispatch hl.dsp.focus({ window = \"" +
                                term_sel() + "\" })"))
            DBG("agents: focus dispatch failed "
                "(click-outside dismissal degraded to focus-change only)");
        verify_focus(); // informational: arming is the event handler's
    }               // job, and only USER entry may arm
    // Fetch our window's client-object chunk from j/clients ("" if absent).
    std::string client_chunk() {
        std::string j = hypr_request(hdir_, "j/clients");
        std::string key = "\"address\": \"0x" + term_addr_ + "\"";
        size_t a = j.find(key);
        if (a == std::string::npos) return "";
        size_t next = j.find("\"address\": \"", a + key.size());
        return j.substr(a, (next == std::string::npos ? j.size() : next) - a);
    }
    static std::string json_field(const std::string& c, const char* k) {
        std::string key = std::string("\"") + k + "\": ";
        size_t p = c.find(key);
        if (p == std::string::npos) return "?";
        p += key.size();
        size_t e = p;
        if (c[p] == '[') e = c.find(']', p) + 1;
        else while (e < c.size() && c[e] != ',' && c[e] != '}' &&
                    c[e] != '\n') ++e;
        return c.substr(p, e - p);
    }
    // MEASURE what the compositor actually did instead of trusting "ok"
    // replies — this build's whole history is dispatches that report
    // success and change nothing. Returns whether the window is floating.
    bool verify_place() {
        std::string c = client_chunk();
        if (c.empty()) {
            DBG("agents: verify: window 0x%s not present in j/clients",
                term_addr_.c_str());
            return false;
        }
        std::string fl = json_field(c, "floating"),
                    sz = json_field(c, "size"), at = json_field(c, "at");
        DBG("agents: verify geometry: floating=%s size=%s at=%s",
            fl.c_str(), sz.c_str(), at.c_str());
        return fl.rfind("true", 0) == 0;
    }
    bool verify_focus() {
        std::string j  = hypr_request(hdir_, "j/activewindow");
        bool        ok = j.find("\"address\": \"0x" + term_addr_ + "\"") !=
                  std::string::npos;
        DBG("agents: verify focus: popup %s focused", ok ? "IS" : "is NOT");
        return ok;
    }
    // Best-effort compositor-side rules for our class, installed once per
    // run: if they take, every popup window gets its geometry at map time
    // and the per-window dispatches become idempotent no-ops. Replies are
    // logged, not trusted; the dispatch + verify path remains the source
    // of truth.
    // The keyword error text itself said "Use eval." — the Lua-parser
    // socket accepts `eval <code>`. Use it to flip
    // input:special_fallthrough so clicks OUTSIDE the popup pass through
    // to the windows beneath instead of being captured by the special's
    // blocking layer (the field-reported "layer underneath the popup").
    // The setter name is probed and VERIFIED via the documented
    // hl.get_config(); every attempt is logged.
    // eval replies only ok/error — no return values. Smuggle values out
    // through the error message instead: error("MBVAL:"..v) puts v in
    // the reply text.
    std::string eval_get(const std::string& expr) {
        std::string r = hypr_request(
            hdir_, "eval error(\"MBVAL:\" .. tostring(" + expr + "))");
        size_t p = r.rfind("MBVAL:");
        if (p == std::string::npos) return "";
        std::string v = r.substr(p + 6);
        while (!v.empty() &&
               (v.back() == '\n' || v.back() == '\'' || v.back() == ' ' ||
                v.back() == '"' || v.back() == ')'))
            v.pop_back();
        return v;
    }
    void enable_click_through() {
        auto get = [&] {
            return eval_get(
                "hl.get_config(\"input:special_fallthrough\")");
        };
        std::string before = get();
        DBG("agents: special_fallthrough before: '%.30s'", before.c_str());
        if (before.rfind("true", 0) == 0) return;
        // hl.config({...}) replied ok in the field; try it first, then
        // the other spellings.
        const char* setters[] = {
            "eval hl.config({ input = { special_fallthrough = true } })",
            "eval hl.set_config(\"input:special_fallthrough\", true)",
            "eval hl.set(\"input:special_fallthrough\", true)",
        };
        for (const char* st : setters) {
            std::string r   = hypr_request(hdir_, st);
            std::string now = get();
            DBG("agents: fallthrough setter -> reply='%.40s' value='%.20s'",
                r.c_str(), now.c_str());
            if (now.rfind("true", 0) == 0) {
                DBG("agents: special_fallthrough ENABLED (verified): "
                    "outside clicks reach other windows");
                return;
            }
        }
        DBG("agents: could not verify special_fallthrough; outside clicks "
            "may stay captured (bar clicks & foreign windows still "
            "dismiss)");
    }
    // A window mapped before our rule existed keeps its stale geometry
    // forever (rules apply at map). Detect it: not floating, or sitting
    // at Hyprland's default 800x600 float.
    bool stale_geometry() {
        std::string c = client_chunk();
        if (c.empty()) return false;
        return json_field(c, "floating").rfind("true", 0) != 0 ||
               c.find("\"size\": [800, 600]") != std::string::npos;
    }
    void preinstall_rules() {
        const std::string cls = cfg.agents_term_class;
        // The socket evaluates `dispatch <arg>` as `return
        // hl.dispatch(<arg>)` (its own error text revealed the wrapper),
        // so <arg> can be any Lua expression: call the DOCUMENTED
        // hl.window_rule() config API for its side effect, then hand
        // hl.dispatch a harmless dispatcher so the call as a whole
        // succeeds. If the rule takes, every popup window gets
        // float/size/move compositor-side AT MAP TIME — no per-window
        // dispatch games at all.
        // Do NOT pin this class to special:mbagent. Show/hide moves the
        // window by address onto the current workspace and back; a
        // workspace windowrule would yank it back onto the special and
        // take file-chooser transients with it.
        const std::string lua =
            "dispatch (function() hl.window_rule({ enabled = true, "
            "match = { class = \"" + cls + "\" }, float = true, size = "
            "\"" + popup_size() + "\", move = \"" + popup_move() +
            "\", group = \"barred\" }) "
            "return hl.dsp.exec_cmd(\"true\") end)()";
        std::string r1 = hypr_request(hdir_, lua);
        std::string r2 = hypr_request(
            hdir_, "keyword windowrulev2 float,class:^(" + cls + ")$");
        std::string r3 = hypr_request(
            hdir_, "keyword windowrulev2 size " + popup_size() +
                       ",class:^(" + cls + ")$");
        std::string r4 = hypr_request(
            hdir_, "keyword windowrulev2 move " + popup_move() +
                       ",class:^(" + cls + ")$");
        std::string r5 = hypr_request(
            hdir_, "keyword windowrulev2 group barred,class:^(" + cls + ")$");
        DBG("agents: rule preinstall: lua-rule reply='%.120s'", r1.c_str());
        DBG("agents: rule preinstall: legacy keyword replies: "
            "float='%.60s' size='%.60s' move='%.60s' group='%.60s'",
            r2.c_str(), r3.c_str(), r4.c_str(), r5.c_str());
    }
    // Give the freshly adopted (still hidden) popup its real geometry.
    // exec_cmd's Lua rule table silently drops STATIC rules — float,
    // size, move — while honouring workspace; field-verified: the popup
    // arrived TILED, alone on the special, filling the whole monitor and
    // reading as "fullscreen". So geometry is enforced here with plain
    // address-targeted dispatchers, identical on hyprlang and Lua builds
    // (quoted-string dispatch on Lua builds). Order matters: a tiled window ignores resize and
    // move, so float comes first. On builds where the exec rules DID
    // apply, every step is an idempotent no-op.
    void place_popup() {
        const std::string sel = term_sel();
        const std::string win = lua_str(sel);
        PopupPx           g   = popup_px();
        const std::string wx  = std::to_string(g.w), hy = std::to_string(g.h),
                          px  = std::to_string(g.x), py = std::to_string(g.y);
        // Hyprland 0.56 `window.float` is a toggle even with action=set.
        // Calling it on an already-floating leftover tiles the session
        // into a full special workspace. Only float when it isn't.
        if (!verify_place())
            hypr_dispatch2(
                hdir_, "dispatch setfloating " + sel,
                "dispatch hl.dsp.window.float({ window = \"" + win +
                    "\", action = \"toggle\" })");
        // Pixel resize/move: percent args to resizewindowpixel are a Lua
        // syntax error on 0.56 and leave the default 800x600 float.
        hypr_dispatch2(
            hdir_,
            "dispatch resizewindowpixel exact " + wx + " " + hy + "," +
                sel,
            "dispatch hl.dsp.window.resize({ window = \"" + win +
                "\", exact = true, x = " + wx + ", y = " + hy + " })");
        hypr_dispatch2(
            hdir_,
            "dispatch movewindowpixel exact " + px + " " + py + "," + sel,
            "dispatch hl.dsp.window.move({ window = \"" + win +
                "\", exact = true, x = " + px + ", y = " + py + " })");
        if (!verify_place())
            DBG("agents: popup is STILL NOT FLOATING after geometry "
                "dispatches — this Hyprland accepts them without acting; "
                "the preinstalled window rules are the remaining hope");
    }
    // Right-click: hand the live session over to the user as a normal
    // window on their current workspace. The bar stops managing it
    // entirely; the next left-click starts a fresh popup session.
    void eject() {
        // Target = the underlying normal workspace, read from j/monitors
        // (activeWorkspace stays the normal one even while a special is
        // overlaid on top of it).
        std::string ws = normal_ws_id();
        if (ws.empty()) {
            DBG("agents: eject: could not resolve the target workspace; "
                "keeping the popup");
            return;
        }
        // The only documented Lua move acts on the ACTIVE window, so the
        // move is gated on VERIFIED popup focus — it can never relocate
        // some other window.
        if (!shown_) { show(true); }
        focus_popup();
        if (!verify_focus()) {
            DBG("agents: eject: popup focus could not be confirmed; "
                "aborting (an active-window move would hit the wrong "
                "window). Click into the popup once, then right-click "
                "again.");
            return;
        }
        if (!hypr_dispatch2(hdir_,
                            "dispatch movetoworkspace " + ws + "," +
                                term_sel(),
                            "dispatch hl.dsp.window.move({ workspace = \"" +
                                ws + "\" })")) {
            DBG("agents: eject move dispatch failed; keeping the popup");
            return;
        }
        // Measure: did it actually leave the special?
        std::string c = client_chunk();
        if (!c.empty() &&
            c.find("\"name\": \"special:mbagent\"") != std::string::npos) {
            DBG("agents: eject verified FAILED (window still on the "
                "special); keeping the popup");
            return;
        }
        DBG("agents: session ejected to workspace %s (verified); released "
            "from bar management", ws.c_str());
        // Follow it so the user lands on their promoted session.
        hypr_dispatch2(hdir_, "dispatch workspace " + ws,
                       "dispatch hl.dsp.focus({ workspace = " + ws + " })");
        term_open_  = false;
        shown_      = false;
        on_special_ = false;
        hold_next_  = false;
        term_addr_.clear();
        term_cls_.clear();
    }
    static bool is_agent_special(const std::string& name) {
        return name == "special:mbagent" ||
               name.rfind("special:mbagent", 0) == 0;
    }
    bool is_agent_class(const std::string& cls) const {
        return !cls.empty() && cls == cfg.agents_term_class;
    }
    std::string normal_ws_id() {
        std::string mons = hypr_request(hdir_, "j/monitors");
        std::string ws;
        size_t p = mons.find("\"activeWorkspace\"");
        if (p != std::string::npos) p = mons.find("\"id\":", p);
        if (p != std::string::npos) {
            p += 5;
            while (p < mons.size() &&
                   (mons[p] == ' ' || mons[p] == '\n' || mons[p] == '\t'))
                ++p;
            while (p < mons.size() && (isdigit((unsigned char)mons[p]) ||
                                       mons[p] == '-'))
                ws += mons[p++];
        }
        return ws;
    }
    // Agent terminals on special:mbagent open links in Brave; Hyprland
    // then maps those windows onto the same special (xdg-activation /
    // opener workspace). Any other floater that maps while the special
    // is focused lands there too. togglespecialworkspace would
    // raise/dismiss them with the popup. Send every non-agent client
    // back to the real workspace; address-targeted so the agent stays.
    void evict_strays() {
        if (hdir_.empty()) return;
        std::string dest = normal_ws_id();
        if (dest.empty()) dest = "1";
        std::string j   = hypr_request(hdir_, "j/clients");
        size_t      pos = 0;
        while (true) {
            size_t a = j.find("\"address\": \"", pos);
            if (a == std::string::npos) break;
            a += 12;
            size_t e = j.find('"', a);
            if (e == std::string::npos) break;
            size_t next = j.find("\"address\": \"", e);
            std::string chunk =
                j.substr(e, (next == std::string::npos ? j.size() : next) - e);
            pos = e;
            if (chunk.find("\"name\": \"special:mbagent\"") ==
                std::string::npos)
                continue;
            std::string cls;
            size_t c = chunk.find("\"class\": \"");
            if (c != std::string::npos) {
                c += 10;
                size_t ce = chunk.find('"', c);
                if (ce != std::string::npos) cls = chunk.substr(c, ce - c);
            }
            if (is_agent_class(cls)) continue;
            std::string addr = j.substr(a, e - a);
            std::string sel =
                addr.rfind("0x", 0) == 0 ? addr : "0x" + addr;
            hypr_dispatch2(
                hdir_,
                "dispatch movetoworkspacesilent " + dest + ",address:" +
                    sel,
                "dispatch \"movetoworkspacesilent " + dest + ",address:" +
                    sel + "\"");
            DBG("agents: evicted stray class='%s' addr=%s -> workspace %s",
                cls.c_str(), sel.c_str(), dest.c_str());
        }
    }
    // Find a session window that already exists on our special workspace
    // — e.g. from a previous MattBar run — and adopt it instead of ever
    // spawning a duplicate. Only the configured agent class is ours:
    // browsers opened from the agent inherit the special and must not
    // be treated as the session window.
    bool discover_session() {
        if (hdir_.empty()) return false;
        evict_strays();
        std::string j = hypr_request(hdir_, "j/clients");
        size_t      pos = 0;
        int         extras = 0;
        std::string addr, cls;
        while (true) {
            size_t a = j.find("\"address\": \"", pos);
            if (a == std::string::npos) break;
            a += 12;
            size_t e = j.find('"', a);
            if (e == std::string::npos) break;
            size_t next = j.find("\"address\": \"", e);
            std::string chunk =
                j.substr(e, (next == std::string::npos ? j.size() : next) - e);
            if (chunk.find("\"name\": \"special:mbagent\"") !=
                std::string::npos) {
                std::string here;
                size_t c = chunk.find("\"class\": \"");
                if (c != std::string::npos) {
                    c += 10;
                    size_t ce = chunk.find('"', c);
                    if (ce != std::string::npos)
                        here = chunk.substr(c, ce - c);
                }
                if (!is_agent_class(here)) {
                    pos = e;
                    continue;
                }
                if (addr.empty()) {
                    addr = j.substr(a, e - a);
                    cls  = here;
                } else {
                    ++extras;
                }
            }
            pos = e;
        }
        if (addr.empty()) return false;
        term_addr_  = addr_norm(addr);
        term_cls_   = cls;
        term_open_  = true;
        on_special_ = true;
        adopt_ms_   = now_ms();
        // An adopted leftover says nothing about our spawn command: its
        // exit must not feed the fast-exit failure streak.
        spawned_by_us_ = false;
        // Is the special currently revealed? (Bookkeeping must match
        // reality or the first toggle goes the wrong way.)
        sync_shown();
        if (extras)
            DBG("agents: %d additional leftover session(s) on the "
                "special; each will be adopted as the current one exits",
                extras);
        return true;
    }
    // Terminal-session popup: toggle a live session, or spawn one onto
    // the hidden special workspace and let sock2 adoption reveal it.
    // panel_on_fail guards against ping-pong when we arrived here FROM a
    // failed panel attempt.
    bool spawn_popup(bool panel_on_fail) {
        if (hdir_.empty() || sock2_fd_ < 0) {
            DBG("agents: popup unavailable (no hypr sockets)");
            return false;
        }
        if (now_ms() < fail_until_) {
            // Two sessions in a row died within seconds of opening: the
            // COMMAND is broken, not the popup — respawning would just
            // flash invisibly forever. Route to the panel and say so.
            DBG("agents: session command keeps exiting immediately; "
                "run this in a terminal to see why:  %s",
                cfg.agents_term.c_str());
            open_panel(false);
            return false;
        }
        if (!term_open_ && discover_session()) {
            DBG("agents: reusing existing session window 0x%s "
                "(no new process spawned)", term_addr_.c_str());
            place_popup(); // leftovers from old builds may still be tiled
        }
        if (term_open_) { // session lives: reveal (hide is on_click's job)
            if (!on_special_) {
                // Adopted, but stranded on a normal workspace (exec rules
                // were lost and the first repair failed): each click
                // re-attempts the move instead of piling up sessions.
                repair(term_addr_);
                return true;
            }
            // Never closewindow a live session to "fix" 800x600 leftovers.
            // Hyprland 0.56 can resize in place via hl.dsp.window.resize.
            if (!shown_) place_popup();
            show(true);
            if (shown_) {
                focus_popup();
                hint_eject();
            }
            return true;
        }
        if (spawning_)
            return true; // in flight; adoption or the timeout will act
        spawning_ = true;
        const std::string sz  = popup_size();
        const std::string cmd = spawn_cmd();
        if (hold_next_ && cmd != cfg.agents_term)
            DBG("agents: previous session exited fast; spawning with a "
                "hold-open shim so the error stays visible");
        // Hyprlang exec-rules bind to the NEW pid. Lua exec_cmd's
        // workspace option is also pid-matched; when the terminal hands
        // off or the pid misses, Hyprland applies it to the focused
        // window and swallows whatever floater was up onto
        // special:mbagent — which then vanishes with the popup. Class
        // windowrules (preinstall_rules) put OUR terminal on the special;
        // repair() catches a map on the wrong workspace by address.
        if (!hypr_dispatch2(
                hdir_,
                "dispatch exec [float; size " + sz + "; workspace "
                "special:mbagent silent] " + cmd,
                "dispatch hl.dsp.exec_cmd(\"" + lua_str(cmd) +
                    "\", { float = true, size = \"" + sz + "\" })")) {
            // Both dialects rejected the exec (or Hyprland timed out).
            // Never latch a dead state behind a silent click.
            spawning_ = false;
            DBG("agents: terminal spawn dispatch failed%s",
                panel_on_fail ? "; panel fallback" : "");
            if (panel_on_fail) open_panel(false);
            return false;
        }
        DBG("agents: session dispatched; awaiting its window (%d ms budget)",
            SPAWN_TIMEOUT_MS);
        arm_spawn(true);
        return true;
    }
    void arm_spawn(bool arm) {
        if (spawn_fd_ < 0) return;
        itimerspec ts{}; // zeroed = disarm
        if (arm) {
            ts.it_value.tv_sec  = SPAWN_TIMEOUT_MS / 1000;
            ts.it_value.tv_nsec = (SPAWN_TIMEOUT_MS % 1000) * 1000000L;
        }
        timerfd_settime(spawn_fd_, 0, &ts, nullptr);
    }
    // Escape a string for embedding in a double-quoted Lua literal (the
    // command travels inside hl.dsp.exec_cmd("...")): a user-configured
    // agents_term containing quotes must not break out of the string.
    static std::string lua_str(const std::string& s) {
        std::string o;
        o.reserve(s.size());
        for (char ch : s) {
            if (ch == '\\' || ch == '"') o += '\\';
            o += ch;
        }
        return o;
    }
    // The popup size is spliced into dispatch strings for BOTH dialects,
    // so it is validated down to digits/%/space; anything else falls back
    // to the stock footprint rather than risking a malformed dispatch.
    static std::string popup_size() {
        const std::string& v = cfg.agents_popup_size;
        int fields = 0;
        bool ok = !v.empty(), in_num = false;
        for (char ch : v) {
            if (ch >= '0' && ch <= '9') { if (!in_num) { in_num = true; ++fields; } }
            else if (ch == '%' || ch == ' ') in_num = false;
            else { ok = false; break; }
        }
        return (ok && fields == 2) ? v : "36% 44%";
    }
    // Exec rules got lost (Hyprland 0.55 Lua exec_cmd matches rules by the
    // spawned PID; terminals that hand off to a running instance defeat
    // it): the session window is real but sitting on a normal workspace.
    // Move it onto our special by address, then reveal. Event addresses
    // come without the 0x selector prefix.
    void repair(const std::string& addr) {
        // Mapped on a normal workspace already. Keep it there as the
        // visible popup — do not round-trip through special:mbagent
        // (that overlay is what swallowed file choosers).
        term_addr_     = addr_norm(addr);
        on_special_    = true;
        shown_         = true;
        reveal_ms_     = now_ms();
        popup_focused_ = false;
        foreign_seen_  = false;
        hid_ms_        = 0;
        place_popup();
        focus_popup();
        hint_eject();
        DBG("agents: repaired in place on the current workspace "
            "(addr=0x%s)",
            term_addr_.c_str());
    }
    static std::string default_agent() {
        const char* h = getenv("HOME");
        std::string f = slurp((std::string(h ? h : ".") +
                               "/.config/omarchy/defaults/agent").c_str());
        while (!f.empty() && (f.back() == '\n' || f.back() == ' '))
            f.pop_back();
        return f;
    }
    void show(bool want) {
        // Address-targeted move of OUR window only. Overlaying
        // special:mbagent (togglespecialworkspace) made every floater
        // that mapped while the popup was up — GTK file choosers in
        // particular — a member of that special, so dismissing the
        // agent took them with it.
        evict_strays();
        if (term_addr_.empty()) {
            shown_ = false;
            return;
        }
        if (want) {
            std::string dest = normal_ws_id();
            if (dest.empty()) dest = "current";
            DBG("agents: revealing popup onto workspace %s (agent only)",
                dest.c_str());
            move_term(dest, false);
            place_popup();
            if (special_is_shown()) {
                // Older builds left the special overlaid. Agent is
                // already on the real workspace, so toggling it off
                // cannot swallow the popup.
                hypr_dispatch2(hdir_,
                               "dispatch togglespecialworkspace mbagent",
                               "dispatch hl.dsp.workspace.toggle_special("
                               "\"mbagent\")");
            }
            evict_strays();
            shown_         = true;
            reveal_ms_     = now_ms();
            popup_focused_ = false;
            foreign_seen_  = false;
            hid_ms_        = 0;
            open_dismiss_catcher();
        } else {
            DBG("agents: hiding popup onto special:mbagent (agent only)");
            close_dismiss_catcher();
            move_term("special:mbagent", true);
            evict_strays(); // file choosers that followed the parent
            if (special_is_shown()) {
                hypr_dispatch2(hdir_,
                               "dispatch togglespecialworkspace mbagent",
                               "dispatch hl.dsp.workspace.toggle_special("
                               "\"mbagent\")");
            }
            shown_ = false;
            hid_ms_ = now_ms();
        }
    }
    static uint64_t now_ms() {
        timespec ts;
        clock_gettime(CLOCK_BOOTTIME, &ts);
        return ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000ULL;
    }
    void sock2_line(const std::string& l) {
        if (l.rfind("openwindow>>", 0) == 0) {
            // ADDRESS,WORKSPACE,CLASS,TITLE
            std::string rest = l.substr(12);
            size_t a = rest.find(',');
            size_t b = a == std::string::npos ? a : rest.find(',', a + 1);
            size_t c = b == std::string::npos ? b : rest.find(',', b + 1);
            if (c == std::string::npos) return;
            std::string ws  = rest.substr(a + 1, b - a - 1);
            std::string cls = rest.substr(b + 1, c - b - 1);
            if (!spawning_) {
                // A foreign window appearing while the dropdown is up
                // means the user is acting elsewhere (their outside
                // clicks reach the desktop layer and can launch things —
                // observed in the field: Omarchy's background selector).
                std::string title =
                    c + 1 < rest.size() ? rest.substr(c + 1) : "";
                if (is_agent_special(ws) && !is_agent_class(cls)) {
                    DBG("agents: stray '%s' mapped on the special; "
                        "evicting",
                        cls.c_str());
                    evict_strays();
                    return;
                }
                if (is_dialog_class(cls) || is_dialog_title(title)) {
                    // File choosers must stay on the real workspace and
                    // must not dismiss the agent. Drop the outside-click
                    // catcher so the dialog is clickable (it is a normal
                    // window, under our top-layer catcher).
                    evict_strays();
                    close_dismiss_catcher();
                    return;
                }
                if (shown_ && on_special_ && !is_agent_class(cls) &&
                    !is_agent_special(ws)) {
                    DBG("agents: foreign window '%s' opened; hiding the "
                        "popup", cls.c_str());
                    show(false);
                }
                return;
            }
            bool ours_ws  = is_agent_special(ws);
            bool ours_cls = is_agent_class(cls);
            DBG("agents: openwindow addr=%s ws='%s' class='%s' -> %s",
                rest.substr(0, a).c_str(), ws.c_str(), cls.c_str(),
                ours_cls && ours_ws    ? "ADOPT"
                : ours_cls             ? "ADOPT (our class, wrong workspace)"
                : ours_ws              ? "stray on special (evict)"
                                       : "not ours");
            if (ours_ws && !ours_cls) {
                evict_strays();
                return;
            }
            if (!ours_cls) return;
            term_addr_ = addr_norm(rest.substr(0, a));
            term_cls_  = cls;
            term_open_ = true;
            spawning_  = false;
            adopt_ms_  = now_ms();
            spawned_by_us_ = true; // fast-exit tracking applies
            arm_spawn(false);
            if (ours_ws) {
                on_special_ = true;
                place_popup(); // still hidden: position it silently
                show(true);    // reveal at the bar edge
                focus_popup();
                hint_eject();
            } else {
                DBG("agents: exec rules not applied (window on '%s'); "
                    "repairing", ws.c_str());
                repair(term_addr_);
            }
        } else if (l.rfind("closewindow>>", 0) == 0) {
            if (term_open_ &&
                addr_norm(l.substr(13)) == addr_norm(term_addr_)) {
                const uint64_t age = now_ms() - adopt_ms_;
                DBG("agents: session window closed after %llu ms",
                    (unsigned long long)age);
                if (age < 2000 && spawned_by_us_) {
                    hold_next_ = true; // next spawn shows the error
                    if (++fail_streak_ >= 2) {
                        fail_until_ = now_ms() + 60000;
                        fprintf(stderr,
                                "mattbar: agents: the popup session exited "
                                "immediately twice in a row — the terminal "
                                "maps fine, so the command inside it is "
                                "failing. Run `omarchy-agent` in a "
                                "terminal to see its error (note the "
                                "environment: Hyprland exec may have a "
                                "different PATH than your shell).\n"
                                "mattbar: (clicks open the shell panel for "
                                "the next 60 s)\n");
                        spawn_detached(
                            "notify-send -u critical MattBar 'Agent popup: "
                            "the session exits immediately. Run "
                            "omarchy-agent in a terminal to see the "
                            "error.'");
                    }
                } else {
                    fail_streak_ = 0;
                    hold_next_   = false;
                }
                term_open_  = false; // session ended; next click respawns
                shown_      = false;
                on_special_ = false;
                term_addr_.clear();
                term_cls_.clear();
            }
        } else if (l.rfind("activespecial>>", 0) == 0) {
            // Overlaying mbagent is a leftover of older builds. shown_
            // is the agent window's workspace, not the overlay. If the
            // overlay appears, evict everyone else so they don't hide
            // with it.
            std::string rest = l.substr(15);
            size_t      c    = rest.find(',');
            std::string ws   = c == std::string::npos ? rest
                                                      : rest.substr(0, c);
            if (is_agent_special(ws) || ws == "mbagent") evict_strays();
        } else if (l.rfind("activewindow>>", 0) == 0) {
            // CLASS,TITLE — focus-follows-mouse makes this the mouse-out
            // signal. Empty class (focus on nothing) keeps the popup up.
            // Compare against the class the window actually reported, and
            // only auto-dismiss a session we manage on the special (a
            // repaired-in-place session has nowhere to hide to).
            //
            // Reveal race: the popup spawns `silent` (unfocused), and
            // revealing the special while the pointer sits on the BAR
            // makes Hyprland hand focus to the toplevel near the cursor —
            // the user's own terminal. That foreign activewindow arrives
            // one frame after our reveal and is NOT a mouse-out. So
            // dismissal only arms once the popup has actually been
            // focused (the user moused into it), or after a grace period
            // long past any focus-shuffle the reveal itself causes.
            size_t comma = l.find(',', 14);
            std::string cls = l.substr(14, comma - 14);
            std::string title =
                comma == std::string::npos ? "" : l.substr(comma + 1);
            const std::string& ours =
                term_cls_.empty() ? cfg.agents_term_class : term_cls_;
            if (!on_special_) return;
            if (now_ms() < suppress_dismiss_until_) return;
            sync_shown();
            if (!shown_) return;
            const uint64_t since = now_ms() - reveal_ms_;
            if (is_dialog_class(cls) || is_dialog_title(title)) {
                DBG("agents: focus on dialog '%s' (%s); keeping popup, "
                    "evicting strays",
                    cls.c_str(), title.c_str());
                evict_strays();
                close_dismiss_catcher();
                return;
            }
            if (cls == ours) {
                // Hyprland auto-focuses the special's window the moment
                // it is re-revealed — that is NOT the user entering the
                // popup and must not arm dismissal (field bug: instant
                // re-reveals dismissed during bar->popup travel). USER
                // entry is an our-class focus that follows travel (a
                // foreign crossing) or comes well after reveal.
                if (!popup_focused_ && (foreign_seen_ || since > 400)) {
                    popup_focused_ = true;
                    DBG("agents: popup entered "
                        "(mouse-out dismissal armed)");
                    if (!dismiss_.surf) open_dismiss_catcher();
                } else if (!popup_focused_) {
                    DBG("agents: auto-focus at reveal (not arming)");
                }
                return;
            }
            if (!popup_focused_ && since <= 400) {
                // Focus-follows-mouse firing while the pointer crosses
                // windows on its way to the popup: travel, not mouse-out.
                foreign_seen_ = true;
                DBG("agents: ignoring focus '%s' (travel toward the "
                    "popup)", cls.c_str());
                return;
            }
            // Empty class: focus on wallpaper / a layer surface (the
            // bar). After the reveal bounce, that is an outside click.
            DBG("agents: mouse-out (focus moved to '%s'); hiding",
                cls.empty() ? "(none)" : cls.c_str());
            show(false);
        } else if (l.rfind("activewindowv2>>", 0) == 0) {
            std::string addr = addr_norm(l.substr(16));
            while (!addr.empty() &&
                   (addr.back() == '\n' || addr.back() == '\r' ||
                    addr.back() == ' '))
                addr.pop_back();
            if (!on_special_ || !shown_) return;
            if (now_ms() < suppress_dismiss_until_) return;
            if (addr == addr_norm(term_addr_)) {
                const uint64_t since = now_ms() - reveal_ms_;
                if (!popup_focused_ && (foreign_seen_ || since > 400)) {
                    popup_focused_ = true;
                    if (!dismiss_.surf) open_dismiss_catcher();
                }
                return;
            }
            if (!popup_focused_ && now_ms() - reveal_ms_ <= 400) {
                foreign_seen_ = true;
                return;
            }
            DBG("agents: mouse-out (activewindowv2 0x%s); hiding",
                addr.c_str());
            show(false);
        }
    }

    void try_watch() {
        watch_ = inotify_add_watch(ino_fd_, dir_.c_str(),
                                   IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE |
                                       IN_DELETE);
    }

    // Targeted field extraction (house style; records are machine-written,
    // flat, sort_keys=true). "percent" occurs only inside limit entries.
    static double max_percent(const std::string& j, bool& ready) {
        ready    = j.find("\"ready\":true") != std::string::npos;
        double m = -1;
        size_t p = 0;
        while ((p = j.find("\"percent\":", p)) != std::string::npos) {
            p += 10;
            m = std::max(m, atof(j.c_str() + p));
        }
        return m;
    }

    void rescan() {
        int    agents = 0;
        double worst  = -1;
        if (DIR* d = opendir(dir_.c_str())) {
            while (dirent* e = readdir(d)) {
                std::string n = e->d_name;
                if (n.size() < 6 || n.substr(n.size() - 5) != ".json")
                    continue;
                std::string j = slurp(dir_ + "/" + n);
                if (j.empty()) continue;
                bool   rdy = false;
                double pc  = max_percent(j, rdy);
                if (!rdy) continue;
                ++agents;
                worst = std::max(worst, pc);
            }
            closedir(d);
        }
        if (!agents) { // no data contract on this machine: vanish
            set_text(*bar_, "");
            return;
        }
        std::string t = glyph();
        if (worst >= 0) {
            int pct = (int)(worst * 100.0 + 0.5);
            if (pct > 999) pct = 999;
            t += " " + std::to_string(pct) + "%";
            set_text(*bar_, t,
                     pct >= cfg.agents_warn_pct ? cfg.c_urgent : cfg.c_fg);
        } else {
            set_text(*bar_, t, cfg.c_dim); // ready agents, no limit data yet
        }
        DBG("agents: %d ready, worst %.0f%%", agents, worst * 100.0);
    }

    Bar*        bar_ = nullptr;
    std::string dir_, checked_, resolved_;
    int         ino_fd_ = -1, deb_fd_ = -1, watch_ = -1;
    // terminal-session popup state
    std::string hdir_, sock2_buf_, term_addr_, term_cls_;
    AsyncCmd    panel_cmd_; // observable agents_click execution
    int         sock2_fd_  = -1, spawn_fd_ = -1, init_fd_ = -1;
    bool        init_done_ = false;
    uint64_t    adopt_ms_ = 0, fail_until_ = 0; // fast-exit detection
    uint64_t    reveal_ms_ = 0;                 // dismissal grace anchor
    uint64_t    hid_ms_ = 0;                    // last successful hide
    uint64_t    suppress_dismiss_until_ = 0;    // glyph-click race guard
    int         fail_streak_ = 0;
    bool        hold_next_ = false, popup_focused_ = false;
    bool        foreign_seen_ = false;
    bool        spawned_by_us_ = false;
    bool        hinted_ = false;
    bool        term_open_ = false, shown_ = false, spawning_ = false;
    bool        on_special_ = false; // managed popup session (hide/show)
    std::string placed_size_;        // last applied agents_popup_size
    PopupWin    dismiss_;            // outside-click / mouse-out catcher
};


// ---------------------------------------------------------------------------
// Microphone: default-source state beside the volume module. On hardware
// like the MattBook — where the analog mic is a phantom and Bluetooth
// earbuds are the only real capture device — "which source is default and
// is it muted" is load-bearing: the bluez rune means a microphone that
// actually hears you. Event-driven off the shared pactl stream; writes use
// the same accumulate-and-drain pattern as the volume module.
// ---------------------------------------------------------------------------
class MicrophoneModule : public TextModule {
public:
    bool enabled() const override { return cfg.show_microphone; }

    void init(Bar& bar) override {
        bar_ = &bar;
        sub_ = audio_events().subscribe(bar, [this](bool, bool source) {
            if (source) refresh();
        });
        refresh();
    }
    ~MicrophoneModule() override {
        if (sub_) audio_events().unsubscribe(sub_);
    }

    void tick() override {
        if (!audio_events().available()) refresh(); // poll fallback only
    }

    bool on_click(double, int button) override {
        if (button == BTN_LEFT) {
            spawn_detached(live_panel_click(cfg.mic_click, "omarchy.audio"));
            return false;
        }
        if (button == BTN_RIGHT) {
            pend_toggle_ = !pend_toggle_;
            flush_set();
            return true;
        }
        return false;
    }
    bool on_scroll(double, int dir) override {
        pend_delta_ += dir < 0 ? 5 : -5;
        flush_set();
        return true;
    }

private:
    void refresh() {
        cmd_.run(*bar_,
                 "wpctl get-volume @DEFAULT_AUDIO_SOURCE@ 2>/dev/null; "
                 "pactl get-default-source 2>/dev/null",
                 [this](const std::string& out, int) { parse(out); }, 1500);
    }
    void parse(const std::string& out) {
        if (out.find("Volume:") == std::string::npos) {
            set_text(*bar_, ""); // no capture device at all: hide
            return;
        }
        const bool muted = out.find("[MUTED]") != std::string::npos;
        int pct = 0;
        if (auto p = out.find("Volume:"); p != std::string::npos)
            pct = (int)(atof(out.c_str() + p + 7) * 100.0 + 0.5);
        // second line: default source node name -> is this a real (BT) mic?
        const bool bt = out.find("bluez_input") != std::string::npos;
        std::string t = muted ? cfg.mic_muted_glyph : cfg.mic_glyph;
        if (bt) t += " " + cfg.volume_headset_glyph;
        if (!muted && cfg.mic_show_pct) t += " " + std::to_string(pct) + "%";
        set_text(*bar_, t, muted ? cfg.c_dim : cfg.c_fg);
        DBG("microphone: %d%%%s%s", pct, muted ? " muted" : "",
            bt ? " (bt)" : "");
    }
    void flush_set() {
        if (set_cmd_.running()) return;
        std::string cmd;
        if (pend_toggle_) {
            pend_toggle_ = false;
            cmd = "timeout 0.4 wpctl set-mute @DEFAULT_AUDIO_SOURCE@ toggle "
                  "2>/dev/null || timeout 0.4 pactl set-source-mute "
                  "@DEFAULT_SOURCE@ toggle 2>/dev/null";
        } else if (pend_delta_ != 0) {
            std::string n = std::to_string(pend_delta_ < 0 ? -pend_delta_
                                                           : pend_delta_);
            cmd = pend_delta_ > 0
                      ? "timeout 0.4 wpctl set-volume -l 1.0 "
                        "@DEFAULT_AUDIO_SOURCE@ " + n + "%+ 2>/dev/null || "
                        "timeout 0.4 pactl set-source-volume @DEFAULT_SOURCE@ "
                        "+" + n + "% 2>/dev/null"
                      : "timeout 0.4 wpctl set-volume @DEFAULT_AUDIO_SOURCE@ " +
                        n + "%- 2>/dev/null || timeout 0.4 pactl "
                        "set-source-volume @DEFAULT_SOURCE@ -" + n +
                        "% 2>/dev/null";
            pend_delta_ = 0;
        } else {
            return;
        }
        set_cmd_.run(*bar_, cmd, [this](const std::string&, int) {
            refresh();
            flush_set();
        }, 1500);
    }

    Bar*     bar_ = nullptr;
    AsyncCmd cmd_, set_cmd_;
    int      sub_ = 0, pend_delta_ = 0;
    bool     pend_toggle_ = false;
};

// ---------------------------------------------------------------------------
// Screen-recording chip. Same contract as Omarchy's QS ScreenRecording
// indicator: the recorder itself is `omarchy-capture-screenrecording`
// (gpu-screen-recorder). Idle click opens Omarchy's capture submenu
// (audio / mic / webcam options); click while recording stops. Detection
// is an in-process /proc scan, only while a bar is revealed.
// ---------------------------------------------------------------------------
class ScreenRecordModule : public TextModule {
public:
    bool enabled() const override { return cfg.show_screenrecord; }

    void init(Bar& bar) override {
        bar_ = &bar;
        tick();
    }
    void tick() override {
        const bool rec = recorder_running();
        if (rec != rec_ || !init_) {
            rec_  = rec;
            init_ = true;
            set_text(*bar_, cfg.screenrecord_glyph,
                     rec_ ? cfg.c_urgent : cfg.c_dim);
            DBG("screenrecord: %s", rec_ ? "recording" : "idle");
        }
    }
    bool on_click(double, int button) override {
        if (button != BTN_LEFT) return false;
        if (rec_) {
            spawn_detached(cfg.screenrecord_stop);
            return true;
        }
        if (auto* sh = mattbar_shell()) {
            sh->toggle("omarchy.menu",
                       "{\"menu\":\"trigger.capture.screenrecord\"}");
            return true;
        }
        spawn_detached("omarchy-menu toggle trigger.capture.screenrecord");
        return true;
    }

private:
    bool recorder_running() const {
        // comma-separated command prefixes, e.g. gpu-screen-recorder (4.x),
        // wf-recorder (3.x). Prefix-match on /proc/<pid>/cmdline argv[0].
        DIR* d = opendir("/proc");
        if (!d) return false;
        bool found = false;
        while (dirent* e = readdir(d)) {
            if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
            char path[288], buf[256];
            snprintf(path, sizeof path, "/proc/%s/cmdline", e->d_name);
            int fd = open(path, O_RDONLY | O_CLOEXEC);
            if (fd < 0) continue;
            ssize_t n = read(fd, buf, sizeof buf - 1);
            close(fd);
            if (n <= 0) continue;
            buf[n] = 0; // argv[0] is NUL-terminated within buf
            const char* base = strrchr(buf, '/');
            base             = base ? base + 1 : buf;
            size_t p = 0;
            const std::string& procs = cfg.screenrecord_procs;
            while (p < procs.size()) {
                size_t q    = procs.find(',', p);
                auto   name = procs.substr(
                    p, q == std::string::npos ? q : q - p);
                if (!name.empty() &&
                    strncmp(base, name.c_str(), name.size()) == 0) {
                    found = true;
                    break;
                }
                if (q == std::string::npos) break;
                p = q + 1;
            }
            if (found) break;
        }
        closedir(d);
        return found;
    }

    Bar* bar_  = nullptr;
    bool rec_  = false, init_ = false;
};

// ---------------------------------------------------------------------------
// Hyprland workspaces
// ---------------------------------------------------------------------------
namespace {

std::string hypr_socket_dir() {
    const char* sig = getenv("HYPRLAND_INSTANCE_SIGNATURE");
    if (!sig) return {};
    const char* rt = getenv("XDG_RUNTIME_DIR");
    if (rt) {
        std::string p = std::string(rt) + "/hypr/" + sig;
        if (access((p + "/.socket.sock").c_str(), F_OK) == 0) return p;
    }
    std::string p = std::string("/tmp/hypr/") + sig; // older Hyprland
    if (access((p + "/.socket.sock").c_str(), F_OK) == 0) return p;
    return {};
}

int unix_connect(const std::string& path) {
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    // Bounded I/O, always. At session start (or during a config reload)
    // Hyprland can sit on its command socket for a long time; an unbounded
    // connect/read here starves the sd_notify watchdog pings and systemd
    // kills the whole cgroup — the bar AND its pactl subscribe child. Any
    // single stall is now capped well under WatchdogSec. (Streams that get
    // O_NONBLOCK afterwards are unaffected by these timeouts.)
    timeval tv{2, 0};
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

// One-shot request to Hyprland's command socket.
std::string hypr_request(const std::string& dir, const std::string& cmd) {
    int fd = unix_connect(dir + "/.socket.sock");
    if (fd < 0) return {};
    (void)!write(fd, cmd.c_str(), cmd.size());
    std::string out;
    char buf[4096];
    ssize_t n;
    // SO_RCVTIMEO bounds each read; the deadline bounds a slow trickle.
    // Worst case for the whole request stays far below the 10 s watchdog.
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    const long deadline = ts.tv_sec * 1000L + ts.tv_nsec / 1000000L + 3000;
    for (;;) {
        n = read(fd, buf, sizeof buf);
        if (n <= 0) break; // EOF, error, or RCVTIMEO expiry
        out.append(buf, n);
        clock_gettime(CLOCK_MONOTONIC, &ts);
        if (ts.tv_sec * 1000L + ts.tv_nsec / 1000000L > deadline) break;
    }
    close(fd);
    return out;
}

// Hyprland >= 0.55 with a Lua config (what Omarchy Quattro converts every
// install to) evaluates a socket1 `dispatch X` as Lua: `hl.dispatch(X)`.
// The hyprlang form `workspace 3` is a Lua syntax error there — swallowed,
// so clicks silently do nothing. A .conf config still takes the legacy
// dispatcher-table path, and each config type rejects the other's syntax.
// So: send the form that last worked; on a reply that isn't "ok", try the
// other and remember. Costs one extra round-trip per config-type change,
// i.e. approximately never.
bool hypr_dispatch2(const std::string& dir, const std::string& legacy,
                    const std::string& lua) {
    static int mode = 0; // 0 unknown, 1 legacy hyprlang, 2 lua (shared:
                         // one compositor, one config dialect at a time)
    auto ok = [](const std::string& r) { return r.rfind("ok", 0) == 0; };
    if (mode == 2) {
        if (ok(hypr_request(dir, lua))) return true;
        if (ok(hypr_request(dir, legacy))) { mode = 1; return true; }
        mode = 0; // compositor mid-restart? re-learn next dispatch
        return false;
    }
    if (ok(hypr_request(dir, legacy))) { mode = 1; return true; }
    if (ok(hypr_request(dir, lua))) { mode = 2; return true; }
    mode = 0;
    return false;
}

void hypr_dispatch_workspace(const std::string& dir, const std::string& sel) {
    hypr_dispatch2(dir, "dispatch workspace " + sel,
                   "dispatch hl.dsp.focus({ workspace = \"" + sel + "\" })");
}

// Pull every top-level "id": <int> out of Hyprland's JSON without a JSON dep.
std::vector<int> extract_ids(const std::string& json) {
    std::vector<int> ids;
    size_t pos = 0;
    while ((pos = json.find("\"id\":", pos)) != std::string::npos) {
        pos += 5;
        ids.push_back(atoi(json.c_str() + pos));
    }
    return ids;
}

// Pull (workspace id, monitor name) pairs out of Hyprland's j/workspaces.
// Same no-JSON-dependency approach as extract_ids: each workspace object
// carries "id" and "monitor", and they appear in that order.
std::vector<std::pair<int, std::string>> extract_ws_monitors(
    const std::string& json) {
    std::vector<std::pair<int, std::string>> out;
    size_t pos = 0;
    while ((pos = json.find("\"id\":", pos)) != std::string::npos) {
        pos += 5;
        int id = atoi(json.c_str() + pos);
        std::string mon;
        size_t m = json.find("\"monitor\":", pos);
        size_t nxt = json.find("\"id\":", pos);
        if (m != std::string::npos && (nxt == std::string::npos || m < nxt)) {
            size_t q1 = json.find('"', m + 10);
            if (q1 != std::string::npos) {
                size_t q2 = json.find('"', q1 + 1);
                if (q2 != std::string::npos)
                    mon = json.substr(q1 + 1, q2 - q1 - 1);
            }
        }
        out.push_back({id, mon});
    }
    return out;
}

// monitor name -> its active workspace id, from j/monitors. "name" precedes
// "activeWorkspace" in each monitor object.
std::map<std::string, int> extract_active_per_monitor(
    const std::string& json) {
    std::map<std::string, int> out;
    size_t pos = 0;
    while ((pos = json.find("\"activeWorkspace\"", pos)) !=
           std::string::npos) {
        size_t nm = json.rfind("\"name\":", pos);
        std::string mon;
        if (nm != std::string::npos) {
            size_t q1 = json.find('"', nm + 7);
            if (q1 != std::string::npos) {
                size_t q2 = json.find('"', q1 + 1);
                if (q2 != std::string::npos)
                    mon = json.substr(q1 + 1, q2 - q1 - 1);
            }
        }
        size_t idp = json.find("\"id\":", pos);
        if (idp != std::string::npos && !mon.empty())
            out[mon] = atoi(json.c_str() + idp + 5);
        pos += 17;
    }
    return out;
}

class WorkspacesModule : public Module {
public:
    bool enabled() const override { return cfg.show_workspaces; }
    void init(Bar& bar) override {
        bar_ = &bar;
        dir_ = hypr_socket_dir();
        if (dir_.empty()) return;
        refresh();
        // persistent event stream (.socket2)
        ev_fd_ = unix_connect(dir_ + "/.socket2.sock");
        if (ev_fd_ >= 0) {
            fcntl(ev_fd_, F_SETFL, O_NONBLOCK);
            bar.add_fd(ev_fd_, [this](uint32_t) { on_events(); }, "hypr-events");
        }
    }

    void tick() override {
        if (stale_) {
            stale_ = false;
            refresh(); // deferred from events that arrived while hidden
        } else if (ev_fd_ < 0 && !dir_.empty()) {
            refresh(); // no event socket: poll
        }
    }

    // With multi-monitor on, each bar shows the workspaces that live on ITS
    // monitor; the bar being drawn tells us which one that is. With it off
    // (or on a compositor-picked surface with no name) every workspace is
    // shown, exactly as before.
    std::vector<int> visible() const {
        std::string mon = bar_ ? bar_->current_output_name() : std::string();
        if (!cfg.multi_monitor || mon.empty()) return workspaces_;
        std::vector<int> out;
        for (auto& [id, m] : ws_mon_)
            if (m == mon) out.push_back(id);
        std::sort(out.begin(), out.end());
        return out.empty() ? workspaces_ : out;
    }

    int active_for_bar() const {
        std::string mon = bar_ ? bar_->current_output_name() : std::string();
        if (!cfg.multi_monitor || mon.empty()) return active_;
        auto it = active_mon_.find(mon);
        return it == active_mon_.end() ? active_ : it->second;
    }

    double width(cairo_t*) override {
        size_t n = visible().size();
        if (n == 0) return 0.0;
        double per = (cfg_vertical() ? PILL_H : PILL_W) + PILL_GAP;
        return n * per - PILL_GAP;
    }

    void draw(cairo_t* cr, double a, double t) override {
        const bool vert = cfg_vertical();
        cairo_font_extents_t fe;
        cairo_font_extents(cr, &fe);
        const int act = active_for_bar();
        for (int id : visible()) {
            bool active = (id == act);
            double px = vert ? (t - PILL_W) / 2.0 : a;
            double py = vert ? a : (t - PILL_H) / 2.0;
            set_color(cr, active ? cfg.c_accent : cfg.c_ws_bg);
            rounded_rect(cr, px, py, PILL_W, PILL_H, 6);
            cairo_fill(cr);
            std::string label = std::to_string(id);
            double tw = text_width(cr, label);
            const Color tc = active ? contrast_on(cfg.c_accent) : cfg.c_dim;
            cairo_set_source_rgba(cr, tc.r, tc.g, tc.b, 1.0);
            cairo_move_to(cr, px + (PILL_W - tw) / 2.0,
                          py + PILL_H / 2.0 + (fe.ascent - fe.descent) / 2.0);
            cairo_show_text(cr, label.c_str());
            a += (vert ? PILL_H : PILL_W) + PILL_GAP;
        }
    }

    bool on_click(double relx, int button) override {
        auto vis = visible();
        if (button != BTN_LEFT || vis.empty()) return false;
        double per = (cfg_vertical() ? PILL_H : PILL_W) + PILL_GAP;
        int idx = static_cast<int>(relx / per);
        if (idx < 0 || idx >= static_cast<int>(vis.size())) return false;
        hypr_dispatch_workspace(dir_, std::to_string(vis[idx]));
        return true;
    }

    bool on_scroll(double, int dir) override { // cycle workspaces
        hypr_dispatch_workspace(dir_, dir > 0 ? "e+1" : "e-1");
        return true;
    }

private:
    static constexpr double PILL_W = 26, PILL_H = 20, PILL_GAP = 6;

    static void rounded_rect(cairo_t* cr, double x, double y, double w,
                             double h, double r) {
        cairo_new_sub_path(cr);
        cairo_arc(cr, x + w - r, y + r, r, -M_PI_2, 0);
        cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI_2);
        cairo_arc(cr, x + r, y + h - r, r, M_PI_2, M_PI);
        cairo_arc(cr, x + r, y + r, r, M_PI, 1.5 * M_PI);
        cairo_close_path(cr);
    }

    void refresh() {
        std::string json = hypr_request(dir_, "j/workspaces");
        auto pairs = extract_ws_monitors(json);
        std::vector<int> ws;
        std::vector<std::pair<int, std::string>> wsmon;
        for (auto& [id, mon] : pairs) {
            if (id <= 0) continue; // skip special/scratchpad (<0)
            ws.push_back(id);
            wsmon.push_back({id, mon});
        }
        std::sort(ws.begin(), ws.end());
        std::string actj = hypr_request(dir_, "j/activeworkspace");
        auto act = extract_ids(actj);
        int active = act.empty() ? -1 : act.front();
        {
            size_t k = actj.find("\"monitor\"");
            if (k != std::string::npos && bar_) {
                size_t q = actj.find('"', actj.find(':', k));
                if (q != std::string::npos) {
                    size_t e = actj.find('"', q + 1);
                    if (e != std::string::npos)
                        bar_->note_focused_output(
                            actj.substr(q + 1, e - q - 1));
                }
            }
        }
        // Only ask for the per-monitor picture when it can matter; on a
        // single-bar setup this is one IPC round-trip saved per refresh.
        std::map<std::string, int> actmon;
        if (cfg.multi_monitor)
            actmon = extract_active_per_monitor(
                hypr_request(dir_, "j/monitors"));
        if (ws != workspaces_ || active != active_ || wsmon != ws_mon_ ||
            actmon != active_mon_) {
            workspaces_ = std::move(ws);
            ws_mon_     = std::move(wsmon);
            active_mon_ = std::move(actmon);
            active_     = active;
            bar_->request_draw();
        }
    }

    void on_events() {
        char buf[4096];
        ssize_t n;
        bool relevant = false;
        while ((n = read(ev_fd_, buf, sizeof buf)) > 0) {
            // Hyprland's socket2 fires for every focus/window change too;
            // only workspace-affecting events warrant a re-query.
            static const char* keys[] = {"workspace>>",     "workspacev2>>",
                                         "createworkspace", "destroyworkspace",
                                         "moveworkspace",   "renameworkspace",
                                         "focusedmon"};
            buf[n < static_cast<ssize_t>(sizeof buf) ? n : sizeof buf - 1] =
                '\0';
            for (const char* k : keys)
                if (strstr(buf, k)) { relevant = true; break; }
            // focusedmon>>DP-1,3 — keep Bar's overlay target current
            // without forking hyprctl on every hotkey popup.
            const char* p = buf;
            while ((p = strstr(p, "focusedmon>>")) != nullptr) {
                p += 12;
                const char* e = p;
                while (*e && *e != ',' && *e != '\n' && *e != '\r') ++e;
                if (e > p && bar_)
                    bar_->note_focused_output(std::string(p, e - p));
            }
        }
        if (n == 0) { // Hyprland went away
            close(ev_fd_);
            ev_fd_ = -1;
            return;
        }
        if (!relevant) return;
        if (bar_->expanded()) {
            refresh(); // visible: update immediately
        } else {
            stale_ = true; // hidden: no IPC, no redraw; refresh at reveal
        }
    }

    Bar* bar_ = nullptr;
    std::string dir_;
    int ev_fd_ = -1;
    std::vector<int> workspaces_;
    std::vector<std::pair<int, std::string>> ws_mon_; // id -> monitor
    std::map<std::string, int> active_mon_;           // monitor -> active ws
    int active_ = -1;
    bool stale_ = false;
};
} // namespace
Module* make_workspaces() { return new WorkspacesModule; }

// ---------------------------------------------------------------------------
// Battery
// ---------------------------------------------------------------------------
namespace {
class BatteryModule : public TextModule {
public:
    bool enabled() const override { return cfg.show_battery; }
    void init(Bar& bar) override {
        bar_ = &bar;
        DIR* d = opendir("/sys/class/power_supply");
        if (d) {
            while (dirent* e = readdir(d)) {
                std::string name = e->d_name;
                if (name.rfind("BAT", 0) == 0) {
                    path_ = "/sys/class/power_supply/" + name;
                    break;
                }
            }
            closedir(d);
        }
        tick();
    }
    void tick() override {
        if (path_.empty()) return; // desktop: module stays empty
        int cap = atoi(slurp(path_ + "/capacity").c_str());
        std::string status = trim(slurp(path_ + "/status"));
        std::string sym = (status == "Charging")      ? "+"
                          : (status == "Full")        ? "="
                                                      : "";
        Color c = (cap <= 15 && status == "Discharging") ? cfg.c_urgent : cfg.c_fg;
        std::string prefix = cfg_vertical() ? "" : "BAT ";
        std::string t;
        if (cfg.battery_show_time && !cfg_vertical() &&
            (status == "Discharging" || status == "Charging")) {
            // energy_*/power_now (µWh/µW) on most laptops; charge_*/
            // current_now (µAh/µA) on the rest — the ratio is hours either
            // way. Time-to-empty when draining, time-to-full when charging.
            double now  = atof(slurp(path_ + "/energy_now").c_str());
            double full = atof(slurp(path_ + "/energy_full").c_str());
            double rate = atof(slurp(path_ + "/power_now").c_str());
            if (rate <= 0) {
                now  = atof(slurp(path_ + "/charge_now").c_str());
                full = atof(slurp(path_ + "/charge_full").c_str());
                rate = atof(slurp(path_ + "/current_now").c_str());
            }
            if (rate > 0) {
                double hrs = (status == "Charging" ? full - now : now) / rate;
                if (hrs > 0 && hrs < 48) {
                    char b[16];
                    snprintf(b, sizeof b, " %dh%02d", (int)hrs,
                             (int)(hrs * 60) % 60);
                    t = b;
                }
            }
        }
        if (!cfg.power_show_pct && !cfg_vertical())
            set_text(*bar_, (sym.empty() ? std::string("BAT") : sym), c);
        else
            set_text(*bar_, prefix + std::to_string(cap) + "%" + sym + t, c);
        // Alerts: once per threshold crossing, reset when charging resumes.
        if (status == "Discharging") {
            if (cap <= 5 && !warned5_) {
                warned5_ = warned15_ = true;
                if (cfg.notify_battery)
                    notify_post("Battery critical",
                            std::to_string(cap) + "% remaining", 2);
            } else if (cap <= 15 && !warned15_) {
                warned15_ = true;
                if (cfg.notify_battery)
                    notify_post("Battery low",
                            std::to_string(cap) + "% remaining", 1);
            }
        } else {
            warned15_ = warned5_ = false;
        }
    }
    bool on_click(double, int button) override {
        if (path_.empty()) return false;
        if (button == BTN_RIGHT) {
            cfg.power_show_pct = !cfg.power_show_pct;
            cfg.save();
            tick();
            return true;
        }
        auto* sh = mattbar_shell();
        if (sh) sh->toggle("omarchy.power", "");
        return true;
    }
private:
    Bar* bar_ = nullptr;
    std::string path_;
    bool warned15_ = false, warned5_ = false;
};
} // namespace
Module* make_battery() { return new BatteryModule; }

// ---------------------------------------------------------------------------
// Network (default route interface + SSID if wireless)
// ---------------------------------------------------------------------------
namespace {
// --------------------------------------------------------------------------
// nl80211: fetch the SSID of an associated wireless interface without
// spawning iw. Family id resolved once via the genetlink controller, then
// NL80211_CMD_GET_INTERFACE per query; the kernel includes NL80211_ATTR_SSID
// while associated. Returns "" on any failure.
// --------------------------------------------------------------------------
std::string wifi_ssid(const std::string& iface) {
    unsigned idx = if_nametoindex(iface.c_str());
    if (!idx) return {};
    int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_GENERIC);
    if (fd < 0) return {};
    struct Msg {
        nlmsghdr    nl;
        genlmsghdr  ge;
        char        attrs[64];
    };
    auto xchg = [fd](Msg& m, char* rbuf, size_t rlen) -> ssize_t {
        if (send(fd, &m, m.nl.nlmsg_len, 0) < 0) return -1;
        return recv(fd, rbuf, rlen, 0);
    };
    auto put_attr = [](Msg& m, uint16_t type, const void* d, uint16_t len) {
        nlattr* a = (nlattr*)((char*)&m + NLMSG_ALIGN(m.nl.nlmsg_len));
        a->nla_type = type;
        a->nla_len  = (uint16_t)(NLA_HDRLEN + len);
        memcpy((char*)a + NLA_HDRLEN, d, len);
        m.nl.nlmsg_len = NLMSG_ALIGN(m.nl.nlmsg_len) + NLA_ALIGN(a->nla_len);
    };
    char rbuf[4096];

    // resolve the nl80211 family id (cached across calls)
    static uint16_t fam = 0;
    if (!fam) {
        Msg m{};
        m.nl.nlmsg_len   = NLMSG_LENGTH(GENL_HDRLEN);
        m.nl.nlmsg_type  = GENL_ID_CTRL;
        m.nl.nlmsg_flags = NLM_F_REQUEST;
        m.ge.cmd         = CTRL_CMD_GETFAMILY;
        m.ge.version     = 1;
        put_attr(m, CTRL_ATTR_FAMILY_NAME, "nl80211", 8);
        ssize_t n = xchg(m, rbuf, sizeof rbuf);
        if (n <= 0) { close(fd); return {}; }
        for (nlmsghdr* h = (nlmsghdr*)rbuf; NLMSG_OK(h, (size_t)n);
             h = NLMSG_NEXT(h, n)) {
            if (h->nlmsg_type == NLMSG_ERROR) break;
            nlattr* a = (nlattr*)((char*)NLMSG_DATA(h) + GENL_HDRLEN);
            int rem = (int)(h->nlmsg_len - NLMSG_LENGTH(GENL_HDRLEN));
            for (; rem >= NLA_HDRLEN && rem >= a->nla_len;
                 rem -= NLA_ALIGN(a->nla_len),
                 a = (nlattr*)((char*)a + NLA_ALIGN(a->nla_len)))
                if (a->nla_type == CTRL_ATTR_FAMILY_ID)
                    fam = *(uint16_t*)((char*)a + NLA_HDRLEN);
        }
        if (!fam) { close(fd); return {}; }
    }

    Msg m{};
    m.nl.nlmsg_len   = NLMSG_LENGTH(GENL_HDRLEN);
    m.nl.nlmsg_type  = fam;
    m.nl.nlmsg_flags = NLM_F_REQUEST;
    m.ge.cmd         = NL80211_CMD_GET_INTERFACE;
    m.ge.version     = 0;
    uint32_t idx32 = idx;
    put_attr(m, NL80211_ATTR_IFINDEX, &idx32, sizeof idx32);
    ssize_t n = xchg(m, rbuf, sizeof rbuf);
    close(fd);
    if (n <= 0) return {};
    std::string ssid;
    for (nlmsghdr* h = (nlmsghdr*)rbuf; NLMSG_OK(h, (size_t)n);
         h = NLMSG_NEXT(h, n)) {
        if (h->nlmsg_type != fam) continue;
        nlattr* a = (nlattr*)((char*)NLMSG_DATA(h) + GENL_HDRLEN);
        int rem = (int)(h->nlmsg_len - NLMSG_LENGTH(GENL_HDRLEN));
        for (; rem >= NLA_HDRLEN && rem >= a->nla_len;
             rem -= NLA_ALIGN(a->nla_len),
             a = (nlattr*)((char*)a + NLA_ALIGN(a->nla_len)))
            if (a->nla_type == NL80211_ATTR_SSID)
                ssid.assign((char*)a + NLA_HDRLEN, a->nla_len - NLA_HDRLEN);
    }
    return ssid;
}

class NetworkModule : public TextModule {
public:
    ~NetworkModule() override {
        if (nl_fd_ >= 0) close(nl_fd_);
        if (debounce_fd_ >= 0) close(debounce_fd_);
    }
    bool enabled() const override { return cfg.show_network; }
    void init(Bar& bar) override {
        bar_ = &bar;
        // Event-driven: an rtnetlink socket delivers link and route
        // changes — interface up/down, Wi-Fi (re)association, default
        // route moves — so `iw` runs only when the network actually
        // changed, not every 5 seconds of visibility. SSID changes always
        // ride a reassociation, so link events cover them.
        nl_fd_ = socket(AF_NETLINK, SOCK_RAW | SOCK_NONBLOCK | SOCK_CLOEXEC,
                        NETLINK_ROUTE);
        if (nl_fd_ >= 0) {
            sockaddr_nl a{};
            a.nl_family = AF_NETLINK;
            a.nl_groups = RTMGRP_LINK | RTMGRP_IPV4_ROUTE | RTMGRP_IPV6_ROUTE;
            if (bind(nl_fd_, (sockaddr*)&a, sizeof a) < 0) {
                close(nl_fd_);
                nl_fd_ = -1;
            }
        }
        if (nl_fd_ >= 0) {
            // Debounce: a Wi-Fi association is a burst of link + route
            // messages; one query 300 ms after the burst settles.
            debounce_fd_ =
                timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
            bar.add_fd(debounce_fd_, [this](uint32_t) {
                uint64_t n;
                while (read(debounce_fd_, &n, sizeof n) > 0) {}
                if (bar_->expanded()) refresh();
                else stale_ = true; // hidden: no spawn; query at reveal
            }, "net-debounce");
            bar.add_fd(nl_fd_, [this](uint32_t ev) {
                char buf[4096];
                while (recv(nl_fd_, buf, sizeof buf, 0) > 0) {}
                if (ev & (EPOLLHUP | EPOLLERR)) {
                    bar_->remove_fd(nl_fd_);
                    close(nl_fd_);
                    nl_fd_ = -1; // fall back to polling
                    return;
                }
                itimerspec ts{};
                ts.it_value.tv_nsec = 300 * 1000000L;
                timerfd_settime(debounce_fd_, 0, &ts, nullptr);
            }, "rtnetlink");
        }
        counter_ = 0;
        refresh();
    }
    bool on_click(double, int button) override {
        if (button != BTN_LEFT) return false;
        spawn_detached(live_panel_click(cfg.network_click, "omarchy.network"));
        return false;
    }
    void tick() override {
        if (stale_) {
            stale_ = false;
            refresh();
        } else if (nl_fd_ < 0 && counter_++ % 5 == 0) {
            refresh(); // no netlink (containers, odd kernels): old 5 s poll
        }
    }
private:
    void refresh() {
        std::string iface = default_iface();
        if (iface.empty()) {
            set_text(*bar_, "offline", cfg.c_dim);
            return;
        }
        if (access(("/sys/class/net/" + iface + "/wireless").c_str(), F_OK) !=
            0) {
            set_text(*bar_, iface); // wired: nothing to ask
            return;
        }
        // In-process nl80211 query — the last steady-state external binary
        // (iw) is gone. One genetlink round-trip to the local kernel is
        // microseconds, the same latency class as the /proc/net/route read
        // above, so no async machinery: it cannot stall the way a
        // mid-association iw could. Empty answer (not associated, exotic
        // kernel) degrades to the interface name, as before.
        std::string ssid = wifi_ssid(iface);
        set_text(*bar_, ssid.empty() ? iface : ssid);
    }
    static std::string default_iface() {
        std::istringstream rt(slurp("/proc/net/route"));
        std::string line;
        std::getline(rt, line); // header
        while (std::getline(rt, line)) {
            std::istringstream ls(line);
            std::string iface, dest;
            ls >> iface >> dest;
            if (dest == "00000000") return iface;
        }
        return {};
    }
    Bar* bar_ = nullptr;
    unsigned counter_ = 0;
    int nl_fd_ = -1, debounce_fd_ = -1;
    bool stale_ = false;
};
} // namespace
Module* make_network() { return new NetworkModule; }

// ---------------------------------------------------------------------------
// Volume (PipeWire via wpctl, fallback pactl). Scroll to adjust, click mutes.
// ---------------------------------------------------------------------------
namespace {
class VolumeModule : public TextModule {
public:
    bool enabled() const override { return cfg.show_volume; }
    void init(Bar& bar) override {
        bar_ = &bar;
        // Event-driven: the shared pactl-subscribe stream tells us when the
        // sink actually changed; that is the only time the mixer is asked.
        // While hidden nothing is spawned — the change is marked stale and
        // the query happens at the moment of reveal, workspace-style.
        audio_events().subscribe(bar, [this](bool sink, bool) {
            if (!sink) return;
            if (bar_->expanded()) refresh();
            else stale_ = true;
        });
        refresh();
    }
    void tick() override {
        if (stale_) {
            stale_ = false;
            refresh();
        } else if (!audio_events().available() && ++counter_ % 2 == 0) {
            refresh(); // no event stream (no pactl at all): old 2 s poll
        }
    }
    bool on_click(double, int button) override {
        if (button == BTN_LEFT) { // Omarchy convention: open the mixer
            spawn_detached(live_panel_click(cfg.volume_click, "omarchy.audio"));
            return false;
        }
        if (button == BTN_RIGHT) { // mute toggle, async (see flush_set)
            pend_toggle_ = !pend_toggle_; // pair of queued clicks = no-op
            flush_set();
            return true;
        }
        return false;
    }
    bool on_scroll(double, int dir) override {
        pend_delta_ += dir < 0 ? 5 : -5; // notches sum while a setter runs
        flush_set();
        return true;
    }
    // ---- async volume/mute writes ----------------------------------------
    // The old code ran system() per input event: bounded by `timeout 0.4`,
    // but during an audio-stack storm (the exact scenario v1.23.1 exists
    // for) every scroll notch could block the event loop up to 400ms.
    // AsyncCmd's latest-wins coalescing can't be used directly for setters
    // — it would drop scroll notches — so state accumulates here and one
    // in-flight subprocess drains it: a six-notch flick becomes at most two
    // forks (the running one, then a single summed `N%+`), the final volume
    // is identical, and the event loop never waits. The in-shell `timeout
    // 0.4` stays so a hung wpctl still falls through to the pactl fallback;
    // AsyncCmd's own 1500ms kill is the backstop for a hung shell.
    void flush_set() {
        if (set_cmd_.running()) return; // completion callback re-drains
        std::string cmd;
        if (pend_toggle_) {
            pend_toggle_ = false;
            cmd = "timeout 0.4 wpctl set-mute @DEFAULT_AUDIO_SINK@ toggle "
                  "2>/dev/null || timeout 0.4 pactl set-sink-mute "
                  "@DEFAULT_SINK@ toggle 2>/dev/null";
        } else if (pend_delta_ != 0) {
            std::string n = std::to_string(pend_delta_ < 0 ? -pend_delta_
                                                           : pend_delta_);
            cmd = pend_delta_ > 0
                      ? "timeout 0.4 wpctl set-volume -l 1.0 "
                        "@DEFAULT_AUDIO_SINK@ " + n + "%+ 2>/dev/null || "
                        "timeout 0.4 pactl set-sink-volume @DEFAULT_SINK@ +" +
                        n + "% 2>/dev/null"
                      : "timeout 0.4 wpctl set-volume @DEFAULT_AUDIO_SINK@ " +
                        n + "%- 2>/dev/null || timeout 0.4 pactl "
                        "set-sink-volume @DEFAULT_SINK@ -" + n +
                        "% 2>/dev/null";
            pend_delta_ = 0;
        } else {
            return;
        }
        set_cmd_.run(*bar_, cmd, [this](const std::string&, int) {
            refresh();   // read back what we just wrote (covers poll mode;
                         // in event mode the pactl stream coalesces the dup)
            flush_set(); // drain anything queued while the setter ran
        }, 1500);
    }
private:
    // One compound shell invocation gathers everything the module needs —
    // sink properties (headset detection), volume+mute (wpctl, pactl
    // fallback), and the wired-jack active port — separated by markers,
    // parsed when the ASYNC result arrives. One subprocess per refresh,
    // zero milliseconds of event-loop blocking, storms coalesced by
    // AsyncCmd into at most one queued follow-up.
    static std::string prop_of(const std::string& out, const char* key) {
        auto p = out.find(key);
        if (p == std::string::npos) return {};
        auto q1 = out.find('"', p);
        if (q1 == std::string::npos) return {};
        auto q2 = out.find('"', q1 + 1);
        if (q2 == std::string::npos) return {};
        return out.substr(q1 + 1, q2 - q1 - 1);
    }

    bool detect_from(const std::string& inspect, const std::string& port) {
        if (!cfg.volume_headset_indicator) return false;
        std::string ff   = prop_of(inspect, "device.form-factor");
        std::string icon = prop_of(inspect, "device.icon-name");
        std::string node = prop_of(inspect, "node.name");
        if (ff == "headset" || ff == "headphone" || ff == "hands-free")
            return true;
        if (ff == "speaker" || ff == "tv" || ff == "car") return false;
        if (icon.find("headset") != std::string::npos ||
            icon.find("headphon") != std::string::npos)
            return true;
        if (node.rfind("bluez_output", 0) == 0) return true;
        if (node.rfind("alsa_output", 0) == 0 &&
            port.find("headphone") != std::string::npos)
            return true;
        return false;
    }

    void refresh() {
        const char* MARK = "===MB===";
        std::string cmd =
            "wpctl inspect @DEFAULT_AUDIO_SINK@ 2>/dev/null;"
            "printf '\\n===MB===\\n';"
            "wpctl get-volume @DEFAULT_AUDIO_SINK@ 2>/dev/null ||"
            " pactl get-sink-volume @DEFAULT_SINK@ 2>/dev/null | head -1;"
            "printf '\\n===MB===\\n';"
            "pactl list sinks 2>/dev/null |"
            " grep -A40 \"Name: $(pactl get-default-sink 2>/dev/null)\" |"
            " grep 'Active Port'";
        cmd_.run(*bar_, cmd, [this, MARK](const std::string& out, int) {
            auto m1 = out.find(MARK);
            auto m2 = m1 == std::string::npos ? m1
                                              : out.find(MARK, m1 + 8);
            std::string inspect = m1 == std::string::npos
                                      ? std::string()
                                      : out.substr(0, m1);
            std::string vol =
                m1 == std::string::npos || m2 == std::string::npos
                    ? std::string()
                    : out.substr(m1 + 8, m2 - m1 - 8);
            std::string port =
                m2 == std::string::npos ? std::string() : out.substr(m2 + 8);
            // Detection cadence: every refresh in event mode (the refresh
            // IS a device change); every 5th in poll-fallback mode.
            if (audio_events().available() || detect_ctr_++ % 5 == 0)
                headset_ = detect_from(inspect, port);
            apply_volume(vol);
        });
    }

    void apply_volume(const std::string& out) {
        std::string prefix = cfg_vertical() ? "" : "VOL ";
        std::string hs = headset_ && !glyph_.empty() ? " " + glyph_ : "";
        if (out.find("Volume:") != std::string::npos) {
            // wpctl: "Volume: 0.45 [MUTED]"
            double v   = 0;
            bool muted = out.find("MUTED") != std::string::npos;
            auto p     = out.find(':');
            if (p != std::string::npos) v = atof(out.c_str() + p + 1);
            int pct = static_cast<int>(v * 100 + 0.5);
            set_text(*bar_,
                     muted ? prefix + "mute" + hs
                           : prefix + std::to_string(pct) + "%" + hs,
                     muted ? cfg.c_dim : cfg.c_fg);
            return;
        }
        auto p = out.find('%'); // pactl fallback: "... 45% ..."
        if (p != std::string::npos) {
            size_t st = p;
            while (st > 0 && isdigit(out[st - 1])) --st;
            set_text(*bar_, prefix + out.substr(st, p - st) + "%" + hs);
        } else {
            set_text(*bar_, "", cfg.c_dim);
        }
    }
    double width(cairo_t* cr) override {
        // Rune fallback runs at the first paint (needs cairo): configured
        // glyph, then the MD headphones rune, then "HP". A change of font
        // or glyph in settings re-resolves and recomposes the label.
        if (checked_ != cfg.font + cfg.volume_headset_glyph) {
            checked_ = cfg.font + cfg.volume_headset_glyph;
            auto mapped = [&](const std::string& t) {
                cairo_scaled_font_t* sf = cairo_get_scaled_font(cr);
                cairo_glyph_t*       g  = nullptr;
                int                  n  = 0;
                bool ok = cairo_scaled_font_text_to_glyphs(
                              sf, 0, 0, t.c_str(), (int)t.size(), &g, &n,
                              nullptr, nullptr, nullptr) ==
                              CAIRO_STATUS_SUCCESS &&
                          n > 0;
                for (int i = 0; ok && i < n; ++i)
                    if (g[i].index == 0) ok = false;
                if (g) cairo_glyph_free(g);
                return ok;
            };
            std::string pick = cfg.volume_headset_glyph;
            if (!mapped(pick)) pick = "\U000F02CB"; // nf-md-headphones
            if (!mapped(pick)) pick = "HP";
            if (pick != glyph_) {
                glyph_ = pick;
                refresh(); // recompose the label with the resolved rune
            }
        }
        return TextModule::width(cr);
    }

    Bar* bar_ = nullptr;
    unsigned counter_ = 0;
    bool stale_ = false;
    bool headset_ = false;
    unsigned detect_ctr_ = 0;
    AsyncCmd cmd_;
    AsyncCmd set_cmd_;          // volume/mute writes, one in flight
    int      pend_delta_  = 0;  // summed scroll notches awaiting the setter
    bool     pend_toggle_ = false;
    std::string glyph_, checked_;
};
} // namespace
Module* make_volume() { return new VolumeModule; }

// ---------------------------------------------------------------------------
// Bluetooth (BlueZ over the system bus). Fully event-driven, MattBar-style:
// one async GetManagedObjects at startup, then bus-daemon-filtered signals
// (InterfacesAdded/Removed and PropertiesChanged, both sender-scoped to
// org.bluez) keep the state current. No polling, no timers beyond sd-bus's
// own drive-me-at-this-deadline contract — an idle adapter costs zero
// wakeups. Left-click launches cfg.bluetooth_click (Omarchy's bluetui by
// default); right-click toggles adapter power straight through BlueZ.
// ---------------------------------------------------------------------------
namespace {
static uint64_t bt_mono_ms() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000ULL;
}

class BluetoothModule : public TextModule {
public:
    ~BluetoothModule() override {
        if (retry_fd_ >= 0) close(retry_fd_);
        if (grace_fd_ >= 0) close(grace_fd_);
        if (cycle_fd_ >= 0) close(cycle_fd_);
        if (audio_sub_) audio_events().unsubscribe(audio_sub_);
    }
    bool enabled() const override { return cfg.show_bluetooth; }

    void init(Bar& bar) override {
        bar_              = &bar;
        pump_.on_teardown = [this](const char* why) {
            bus_ = nullptr; // the pump owns and has unreffed the connection
            adapters_.clear();
            devs_.clear();
            refresh();
            schedule_retry(why); // reconnect with backoff, not death
        };
        // Recovery timer, armed only while something is wrong: a bar started
        // before bluetoothd is up (login races), a D-Bus restart, or a
        // transient GetManagedObjects failure heals itself with bounded
        // exponential backoff. Healthy state arms nothing: zero idle
        // wakeups, exactly as before.
        retry_fd_ = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
        bar.add_fd(retry_fd_, [this](uint32_t) {
            uint64_t n;
            while (read(retry_fd_, &n, sizeof n) > 0) {}
            if (!bus_) {
                if (setup_bus()) attempts_ = 0;
                else schedule_retry("system bus still unavailable");
            } else if (need_gmo_) {
                need_gmo_ = false;
                sd_bus_call_method_async(bus_, nullptr, "org.bluez", "/",
                                         "org.freedesktop.DBus.ObjectManager",
                                         "GetManagedObjects", on_managed,
                                         this, nullptr);
                pump_.process();
            }
        }, "bluez-retry");
        // transport watcher plumbing: grace timer, reconnect delay timer,
        // and the shared pactl event stream (sink appear/vanish is the
        // authoritative signal; each event triggers one async re-check)
        grace_fd_ = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
        bar.add_fd(grace_fd_, [this](uint32_t) {
            uint64_t n;
            while (read(grace_fd_, &n, sizeof n) > 0) {}
            check_transport();
        }, "bt-transport-grace");
        cycle_fd_ = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
        bar.add_fd(cycle_fd_, [this](uint32_t) {
            uint64_t n;
            while (read(cycle_fd_, &n, sizeof n) > 0) {}
            if (!cycling_ || !bus_ || wedged_dev_.empty()) {
                cycling_ = false;
                return;
            }
            sd_bus_call_method_async(bus_, nullptr, "org.bluez",
                                     wedged_dev_.c_str(), "org.bluez.Device1",
                                     "Connect", on_cycle_conn, this, nullptr);
            pump_.process();
        }, "bt-cycle-reconnect");
        audio_sub_ = audio_events().subscribe(bar, [this](bool sink, bool) {
            if (sink) check_transport();
        });
        if (!setup_bus()) {
            schedule_retry(nullptr);
            refresh();
        }
    }

    void schedule_retry(const char* note) {
        if (++attempts_ > 6) { // about 2 min total, then bus signals only
            fprintf(stderr,
                    "mattbar: bluetooth: still failing after %d attempts; "
                    "will keep listening for org.bluez\n",
                    attempts_ - 1);
            return;
        }
        if (note)
            fprintf(stderr, "mattbar: bluetooth: %s (retry %d in %lds)\n",
                    note, attempts_, 1L << attempts_);
        itimerspec ts{};
        ts.it_value.tv_sec = 1L << attempts_; // 2,4,8,16,32,64s
        timerfd_settime(retry_fd_, 0, &ts, nullptr);
    }

    // No periodic work; tick() only recomposes after live orientation flips
    // (TextModule::width calls it when the bar turns vertical/horizontal).
    void tick() override { refresh(); }

    // The configured glyph is Nerd Font PUA; whether it renders depends on
    // the font cfg.font actually resolves to (Omarchy 3.8 dropped the
    // Cascadia package MattBar's default family comes from, so fontconfig
    // may substitute a glyph-less font). width() runs with the bar font
    // already selected on cr — verify the glyph maps there (unmapped text
    // yields glyph index 0 = .notdef) and fall back to the Material Design
    // rune, then to plain "bt", so the module is never invisible or tofu.
    double width(cairo_t* cr) override {
        resolve_glyph(cr);
        if (glyph_font_.empty() || text_.empty() || cfg_vertical())
            return TextModule::width(cr);
        double gw = 0, rw = 0;
        seg_widths(cr, &gw, &rw);
        return gw + rw;
    }
    void draw(cairo_t* cr, double a, double t) override {
        if (glyph_font_.empty() || text_.empty()) {
            TextModule::draw(cr, a, t);
            return;
        }
        if (cfg_vertical()) {
            // narrow bars: render the whole (compact) text in the glyph-
            // capable family rather than juggling segments in ellipsis code
            select_glyph_font(cr);
            TextModule::draw(cr, a, t);
            select_bar_font(cr);
            return;
        }
        std::string g = glyph(), rest = text_;
        if (text_.rfind(g, 0) == 0) rest = text_.substr(g.size());
        else g.clear();
        double gw = 0, rw = 0;
        seg_widths(cr, &gw, &rw);
        if (!g.empty()) {
            select_glyph_font(cr);
            draw_text(cr, g, a, t, color_);
        }
        select_bar_font(cr);
        if (!rest.empty()) draw_text(cr, rest, a + gw, t, color_);
    }
    void resolve_glyph(cairo_t* cr) {
        if (checked_font_ == cfg.font && checked_glyph_ == cfg.bluetooth_glyph)
            return;
        checked_font_  = cfg.font;
        checked_glyph_ = cfg.bluetooth_glyph;
        auto mapped = [&](const std::string& t) {
            cairo_scaled_font_t* sf = cairo_get_scaled_font(cr);
            cairo_glyph_t*       g  = nullptr;
            int                  n  = 0;
            bool ok = cairo_scaled_font_text_to_glyphs(
                          sf, 0, 0, t.c_str(), (int)t.size(), &g, &n, nullptr,
                          nullptr, nullptr) == CAIRO_STATUS_SUCCESS &&
                      n > 0;
            for (int i = 0; ok && i < n; ++i)
                if (g[i].index == 0) ok = false;
            if (g) cairo_glyph_free(g);
            return ok;
        };
        // Rung 1: the bar font itself (configured rune, then the MD rune).
        std::string pick = cfg.bluetooth_glyph, fam;
        if (!mapped(pick)) pick = "\U000F00AF";
        if (!mapped(pick)) {
            // Rung 2: any Nerd-glyph-capable family installed on the system,
            // used for the rune only (the omarchy module already sets the
            // precedent of a module-local font face). Covers "bar font is
            // not a Nerd Font but one is installed" without new deps.
            static const char* fams[] = {
                "JetBrainsMono Nerd Font",   // Omarchy >= 3.8 / quattro
                "CaskaydiaMono Nerd Font",   // Omarchy 3.1-era package
                "CaskaydiaCove Nerd Font",
                "Cascadia Mono NF",          // Microsoft's own NF build
                "Symbols Nerd Font Mono",    // ttf-nerd-fonts-symbols
                "Symbols Nerd Font",
            };
            pick.clear();
            for (const char* f : fams) {
                cairo_select_font_face(cr, f, CAIRO_FONT_SLANT_NORMAL,
                                       CAIRO_FONT_WEIGHT_NORMAL);
                cairo_set_font_size(cr, cfg.font_size);
                std::string p = cfg.bluetooth_glyph;
                if (!mapped(p)) p = "\U000F00AF";
                if (mapped(p)) {
                    pick = p;
                    fam  = f;
                    break;
                }
            }
            select_bar_font(cr); // leave cr as the bar expects
            if (pick.empty()) pick = "bt"; // Rung 3: always-legible text
            fprintf(stderr,
                    "mattbar: bluetooth glyph not in bar font '%s'; %s\n",
                    cfg.font.c_str(),
                    pick == "bt" ? "falling back to plain text"
                                 : ("using font '" + fam + "'").c_str());
        }
        if (pick != resolved_ || fam != glyph_font_) {
            resolved_   = pick;
            glyph_font_ = fam;
            refresh();
        }
    }

    static void select_bar_font(cairo_t* cr) {
        cairo_select_font_face(cr, cfg.font.c_str(), CAIRO_FONT_SLANT_NORMAL,
                               CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, cfg.font_size);
    }
    void select_glyph_font(cairo_t* cr) const {
        cairo_select_font_face(cr, glyph_font_.c_str(),
                               CAIRO_FONT_SLANT_NORMAL,
                               CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, cfg.font_size);
    }
    // widths of the glyph segment (its own family) and the remainder (bar
    // font); leaves cr on the bar font.
    void seg_widths(cairo_t* cr, double* gw, double* rw) {
        std::string g = glyph(), rest = text_;
        if (text_.rfind(g, 0) == 0) rest = text_.substr(g.size());
        else g.clear();
        select_glyph_font(cr);
        *gw = g.empty() ? 0 : text_width(cr, g);
        select_bar_font(cr);
        *rw = rest.empty() ? 0 : text_width(cr, rest);
    }

    bool on_click(double, int button) override {
        if (button == BTN_LEFT) {
            if (wedged_) { // flagged icon: the obvious intent is "fix it"
                cycle_device();
                return true;
            }
            if (auto* sh = mattbar_shell()) {
                sh->toggle("omarchy.bluetooth", "");
                return true;
            }
            spawn_detached(live_panel_click(cfg.bluetooth_click, "omarchy.bluetooth"));
            return true;
        }
        if (button == BTN_RIGHT && bus_ && !adapters_.empty()) {
            const auto& [path, powered] = *adapters_.begin();
            sd_bus_message* m = nullptr;
            if (sd_bus_message_new_method_call(
                    bus_, &m, "org.bluez", path.c_str(),
                    "org.freedesktop.DBus.Properties", "Set") >= 0) {
                sd_bus_message_append(m, "ss", "org.bluez.Adapter1", "Powered");
                sd_bus_message_open_container(m, 'v', "b");
                sd_bus_message_append(m, "b", powered ? 0 : 1);
                sd_bus_message_close_container(m);
                sd_bus_call_async(bus_, nullptr, m, nullptr, nullptr, 0);
                sd_bus_message_unref(m);
                pump_.process(); // flush the write now, not next wakeup
            }
            return true;
        }
        return false;
    }

private:
    struct Dev {
        bool connected = false;
        bool audio     = false; // Icon "audio-*": headset/headphones/speaker
        std::string name;
        int battery = -1; // org.bluez.Battery1, absent for most devices
    };

    bool setup_bus() {
        int r = sd_bus_open_system(&bus_);
        if (r < 0) {
            fprintf(stderr, "mattbar: bluetooth: system bus unavailable (%s)\n",
                    strerror(-r));
            bus_ = nullptr;
            return false;
        }
        sd_bus_set_method_call_timeout(bus_, 5 * 1000 * 1000ULL); // async; generous for login-storm busses // same cap as tray
        sd_bus_match_signal(bus_, nullptr, "org.bluez", "/",
                            "org.freedesktop.DBus.ObjectManager",
                            "InterfacesAdded", on_added, this);
        sd_bus_match_signal(bus_, nullptr, "org.bluez", "/",
                            "org.freedesktop.DBus.ObjectManager",
                            "InterfacesRemoved", on_removed, this);
        // Sender-scoped: the daemon forwards only BlueZ property traffic, so
        // (as with the tray) unrelated bus chatter never wakes this process.
        sd_bus_match_signal(bus_, nullptr, "org.bluez", nullptr,
                            "org.freedesktop.DBus.Properties",
                            "PropertiesChanged", on_props, this);
        // bluetoothd stop/restart, arg0-filtered daemon-side
        sd_bus_add_match(bus_, nullptr,
                         "type='signal',sender='org.freedesktop.DBus',"
                         "path='/org/freedesktop/DBus',"
                         "interface='org.freedesktop.DBus',"
                         "member='NameOwnerChanged',arg0='org.bluez'",
                         on_owner, this);
        sd_bus_call_method_async(bus_, nullptr, "org.bluez", "/",
                                 "org.freedesktop.DBus.ObjectManager",
                                 "GetManagedObjects", on_managed, this,
                                 nullptr);
        pump_.attach(*bar_, bus_, "bluez");
        pump_.process();
        return true;
    }

    // ---- message walking --------------------------------------------------
    // a{sv} at the current position, applied to `path` for `iface`.
    void read_props(sd_bus_message* m, const std::string& path,
                    const std::string& iface) {
        if (sd_bus_message_enter_container(m, 'a', "{sv}") < 0) return;
        bool ad = iface == "org.bluez.Adapter1";
        bool dv = iface == "org.bluez.Device1";
        bool bt = iface == "org.bluez.Battery1";
        if (ad && !adapters_.count(path)) adapters_[path] = false;
        if ((dv || bt) && !devs_.count(path)) devs_[path];
        bool was = dv && devs_.count(path) ? devs_[path].connected : false;
        while (sd_bus_message_enter_container(m, 'e', "sv") > 0) {
            const char* key = nullptr;
            sd_bus_message_read(m, "s", &key);
            const char* contents = nullptr;
            char        t        = 0;
            sd_bus_message_peek_type(m, &t, &contents);
            std::string k    = key ? key : "";
            bool        used = false;
            if (contents && ad && k == "Powered" && !strcmp(contents, "b")) {
                int b = 0;
                sd_bus_message_enter_container(m, 'v', "b");
                sd_bus_message_read(m, "b", &b);
                sd_bus_message_exit_container(m);
                adapters_[path] = b;
                used            = true;
            } else if (contents && dv && k == "Connected" &&
                       !strcmp(contents, "b")) {
                int b = 0;
                sd_bus_message_enter_container(m, 'v', "b");
                sd_bus_message_read(m, "b", &b);
                sd_bus_message_exit_container(m);
                devs_[path].connected = b;
                used                  = true;
            } else if (contents && dv && (k == "Alias" || k == "Name") &&
                       !strcmp(contents, "s")) {
                const char* v = nullptr;
                sd_bus_message_enter_container(m, 'v', "s");
                sd_bus_message_read(m, "s", &v);
                sd_bus_message_exit_container(m);
                // Alias wins (BlueZ defaults it to Name anyway)
                if (v && (k == "Alias" || devs_[path].name.empty()))
                    devs_[path].name = v;
                used = true;
            } else if (contents && dv && k == "Icon" &&
                       !strcmp(contents, "s")) {
                const char* v = nullptr;
                sd_bus_message_enter_container(m, 'v', "s");
                sd_bus_message_read(m, "s", &v);
                sd_bus_message_exit_container(m);
                if (v) devs_[path].audio = strncmp(v, "audio-", 6) == 0;
                used = true;
            } else if (contents && bt && k == "Percentage" &&
                       !strcmp(contents, "y")) {
                uint8_t p = 0;
                sd_bus_message_enter_container(m, 'v', "y");
                sd_bus_message_read(m, "y", &p);
                sd_bus_message_exit_container(m);
                devs_[path].battery = p;
                used                = true;
            }
            if (!used) sd_bus_message_skip(m, "v");
            sd_bus_message_exit_container(m);
        }
        sd_bus_message_exit_container(m);
        if (dv) {
            Dev& d = devs_[path];
            if (d.connected != was) {
                if (init_done_ && cfg.notify_bluetooth) {
                    std::string who = d.name.empty() ? "Device" : d.name;
                    notify_post("Bluetooth",
                                who + (d.connected ? " connected"
                                                   : " disconnected"), 0);
                }
                // transport watcher: a connect opens the grace window, a
                // disconnect re-evaluates immediately (clears the wedge)
                if (d.connected) arm_grace();
                else check_transport();
            }
        }
    }

    // a{sa{sv}} at the current position.
    void read_ifaces(sd_bus_message* m, const std::string& path) {
        if (sd_bus_message_enter_container(m, 'a', "{sa{sv}}") < 0) return;
        while (sd_bus_message_enter_container(m, 'e', "sa{sv}") > 0) {
            const char* iface = nullptr;
            sd_bus_message_read(m, "s", &iface);
            read_props(m, path, iface ? iface : "");
            sd_bus_message_exit_container(m);
        }
        sd_bus_message_exit_container(m);
    }

    static int on_managed(sd_bus_message* m, void* ud, sd_bus_error*) {
        auto* self = static_cast<BluetoothModule*>(ud);
        if (sd_bus_message_is_method_error(m, nullptr)) {
            const sd_bus_error* e = sd_bus_message_get_error(m);
            self->need_gmo_ = true;
            self->schedule_retry(e && e->name ? e->name
                                              : "GetManagedObjects failed");
            self->refresh();
            return 0;
        }
        if (sd_bus_message_enter_container(m, 'a', "{oa{sa{sv}}}") < 0)
            return 0;
        while (sd_bus_message_enter_container(m, 'e', "oa{sa{sv}}") > 0) {
            const char* path = nullptr;
            sd_bus_message_read(m, "o", &path);
            self->read_ifaces(m, path ? path : "");
            sd_bus_message_exit_container(m);
        }
        sd_bus_message_exit_container(m);
        self->attempts_  = 0;
        self->need_gmo_  = false;
        self->init_done_ = true;
        self->refresh();
        return 0;
    }

    static int on_added(sd_bus_message* m, void* ud, sd_bus_error*) {
        auto*       self = static_cast<BluetoothModule*>(ud);
        const char* path = nullptr;
        if (sd_bus_message_read(m, "o", &path) < 0) return 0;
        self->read_ifaces(m, path ? path : "");
        self->refresh();
        return 0;
    }

    static int on_removed(sd_bus_message* m, void* ud, sd_bus_error*) {
        auto*       self = static_cast<BluetoothModule*>(ud);
        const char* path = nullptr;
        if (sd_bus_message_read(m, "o", &path) < 0 || !path) return 0;
        if (sd_bus_message_enter_container(m, 'a', "s") < 0) return 0;
        const char* iface = nullptr;
        while (sd_bus_message_read(m, "s", &iface) > 0) {
            std::string i = iface ? iface : "";
            if (i == "org.bluez.Adapter1") self->adapters_.erase(path);
            else if (i == "org.bluez.Device1") self->devs_.erase(path);
            else if (i == "org.bluez.Battery1") {
                auto it = self->devs_.find(path);
                if (it != self->devs_.end()) it->second.battery = -1;
            }
        }
        sd_bus_message_exit_container(m);
        self->refresh();
        return 0;
    }

    static int on_props(sd_bus_message* m, void* ud, sd_bus_error*) {
        auto*       self  = static_cast<BluetoothModule*>(ud);
        const char* iface = nullptr;
        const char* path  = sd_bus_message_get_path(m);
        if (sd_bus_message_read(m, "s", &iface) < 0 || !path) return 0;
        self->read_props(m, path, iface ? iface : "");
        self->refresh(); // invalidated-properties tail is irrelevant here
        return 0;
    }

    static int on_owner(sd_bus_message* m, void* ud, sd_bus_error*) {
        auto*       self = static_cast<BluetoothModule*>(ud);
        const char* name = nullptr, *oldo = nullptr, *newo = nullptr;
        if (sd_bus_message_read(m, "sss", &name, &oldo, &newo) < 0) return 0;
        self->adapters_.clear();
        self->devs_.clear();
        if (newo && *newo) { // bluetoothd (re)started: repopulate
            self->attempts_ = 0;
            sd_bus_call_method_async(self->bus_, nullptr, "org.bluez", "/",
                                     "org.freedesktop.DBus.ObjectManager",
                                     "GetManagedObjects", on_managed, self,
                                     nullptr);
        }
        self->refresh();
        return 0;
    }

    // ---- presentation -----------------------------------------------------
    void refresh() {
        if (!bar_) return;
        if (!bus_ || adapters_.empty()) { // no BlueZ / no adapter: hide
            set_text(*bar_, "", cfg.c_dim);
            DBG("bluetooth: <hidden>");
            return;
        }
        bool powered = false;
        for (auto& [p, on] : adapters_) powered |= on;
        if (!powered) {
            set_text(*bar_, glyph() + " off", cfg.c_dim);
            DBG("bluetooth: '%s off'", glyph().c_str());
            return;
        }
        std::string first;
        int         n = 0, batt = -1;
        bool        low = false;
        for (auto& [p, d] : devs_) {
            if (!d.connected) continue;
            if (n++ == 0) {
                first = d.name.empty() ? "device" : d.name;
                batt  = d.battery;
            }
            if (d.battery >= 0 && d.battery <= 15) low = true;
        }
        if (n == 0) {
            set_text(*bar_, glyph(), cfg.c_dim);
            DBG("bluetooth: '%s'", glyph().c_str());
            return;
        }
        // Glyph always leads (module identity next to the network SSID);
        // everything after it is settings-controlled.
        std::string t = glyph();
        if (cfg.bluetooth_show_name) {
            size_t cap = (size_t)cfg.bluetooth_name_len;
            if (first.size() > cap) { // trim on a UTF-8 boundary
                first.resize(cap);
                while (!first.empty() &&
                       (static_cast<unsigned char>(first.back()) & 0xC0) ==
                           0x80)
                    first.pop_back();
                if (!first.empty()) first.pop_back();
                first += "\u2026";
            }
            t += " " + first;
        }
        if (cfg.bluetooth_show_battery && batt >= 0)
            t += " " + std::to_string(batt) + "%";
        if (cfg.bluetooth_show_count && n > 1)
            t += " +" + std::to_string(n - 1);
        if (wedged_) t += cycling_ ? " ..." : " !"; // no transport / healing
        set_text(*bar_, t, wedged_ || low ? cfg.c_urgent : cfg.c_fg);
        DBG("bluetooth: '%s'%s", t.c_str(), wedged_ ? " (wedged)" : "");
    }

    // ---- transport watcher ------------------------------------------------
    // The wedge the MattBook taught us: Device1.Connected=true while
    // PipeWire has no bluez sink — every layer reports healthy and there is
    // no audio. BlueZ state is already in this module; the sink side comes
    // from the shared pactl event stream plus one async query. Zero cost
    // while no audio-class device is connected: nothing is armed, nothing
    // spawns. Recovery = a clean outbound disconnect/connect cycle, which
    // the btmon capture proved never collides (experiment A, productized:
    // click the flagged icon, or bt_auto_heal=true to fire it once
    // automatically per episode).
    bool audio_connected(std::string* path_out = nullptr) const {
        for (auto& [p, d] : devs_)
            if (d.connected && d.audio) {
                if (path_out) *path_out = p;
                return true;
            }
        return false;
    }
    void arm_grace() { // transports legitimately take seconds to appear
        grace_until_ = bt_mono_ms() + 8000;
        itimerspec ts{};
        ts.it_value.tv_sec  = 8;
        ts.it_value.tv_nsec = 200 * 1000000L; // land just past the window
        timerfd_settime(grace_fd_, 0, &ts, nullptr);
        check_transport(); // may clear a stale wedge right away
    }
    void check_transport() {
        std::string dev;
        if (!audio_connected(&dev)) { // nothing to watch: also nothing runs
            set_wedged(false, "");
            return;
        }
        sink_cmd_.run(*bar_, "pactl list short sinks 2>/dev/null",
                      [this, dev](const std::string& out, int) {
                          bool sink = out.find("bluez") != std::string::npos;
                          set_wedged(!sink && bt_mono_ms() >= grace_until_ &&
                                         audio_connected(),
                                     dev);
                      },
                      1500);
    }
    void set_wedged(bool w, const std::string& dev) {
        if (w == wedged_) {
            if (w) wedged_dev_ = dev;
            return;
        }
        wedged_     = w;
        wedged_dev_ = w ? dev : "";
        if (w) {
            DBG("bluetooth: transport wedge on %s", dev.c_str());
            if (!wedge_notified_) {
                wedge_notified_ = true;
                auto it = devs_.find(dev);
                std::string who =
                    it != devs_.end() && !it->second.name.empty()
                        ? it->second.name
                        : "Bluetooth device";
                notify_post("Bluetooth",
                            who + " is connected but has no audio transport" +
                                (cfg.bt_auto_heal
                                     ? " — cycling it"
                                     : " — click the bar icon to cycle it"),
                            1);
            }
            if (cfg.bt_auto_heal && !auto_healed_) {
                auto_healed_ = true;
                cycle_device();
            }
        } else {
            wedge_notified_ = false;
            auto_healed_    = false;
            cycling_        = false;
        }
        refresh();
    }
    void cycle_device() {
        if (cycling_ || !bus_ || wedged_dev_.empty()) return;
        cycling_ = true;
        DBG("bluetooth: cycling %s", wedged_dev_.c_str());
        sd_bus_call_method_async(bus_, nullptr, "org.bluez",
                                 wedged_dev_.c_str(), "org.bluez.Device1",
                                 "Disconnect", on_cycle_disc, this, nullptr);
        pump_.process();
    }
    static int on_cycle_disc(sd_bus_message*, void* ud, sd_bus_error*) {
        // Disconnect settled (either way): reconnect after the same 2s the
        // manual procedure uses, so the device finishes its own teardown.
        auto* self = static_cast<BluetoothModule*>(ud);
        if (!self->cycling_) return 0;
        itimerspec ts{};
        ts.it_value.tv_sec = 2;
        timerfd_settime(self->cycle_fd_, 0, &ts, nullptr);
        return 0;
    }
    static int on_cycle_conn(sd_bus_message* m, void* ud, sd_bus_error*) {
        auto* self     = static_cast<BluetoothModule*>(ud);
        self->cycling_ = false;
        if (sd_bus_message_is_method_error(m, nullptr)) {
            const sd_bus_error* e = sd_bus_message_get_error(m);
            fprintf(stderr, "mattbar: bluetooth: cycle reconnect failed (%s)\n",
                    e && e->name ? e->name : "?");
        }
        self->arm_grace(); // judge the result after transports settle
        return 0;
    }

    const std::string& glyph() const {
        return resolved_.empty() ? cfg.bluetooth_glyph : resolved_;
    }
    std::string checked_font_, checked_glyph_, resolved_, glyph_font_;

    Bar*                        bar_ = nullptr;
    SdPump                      pump_;
    int    retry_fd_ = -1;
    int    attempts_ = 0;
    bool   need_gmo_ = false;
    bool   init_done_ = false;
    // transport watcher state
    AsyncCmd    sink_cmd_;
    int         grace_fd_ = -1, cycle_fd_ = -1;
    uint64_t    grace_until_ = 0;
    bool        wedged_ = false, wedge_notified_ = false;
    bool        auto_healed_ = false, cycling_ = false;
    int         audio_sub_ = 0;
    std::string wedged_dev_;
    sd_bus*                     bus_ = nullptr; // == pump_.bus; nulled on teardown
    std::map<std::string, bool> adapters_;   // path -> Powered
    std::map<std::string, Dev>  devs_;       // path -> device state
};
} // namespace
Module* make_bluetooth() { return new BluetoothModule; }
AgentsModule* AgentsModule::g = nullptr;
Module* make_agents() { return new AgentsModule; }
void agents_hotkey() {
    if (AgentsModule::g) AgentsModule::g->on_click(0, BTN_LEFT);
}
Module* make_microphone() { return new MicrophoneModule; }
Module* make_screenrecord() { return new ScreenRecordModule; }

// ---------------------------------------------------------------------------
// Screen brightness: /sys/class/backlight for state (updated by kernel
// uevents, zero polling), systemd-logind Session.SetBrightness for writes
// (unprivileged for the active session, no helper tools; falls back to a
// direct sysfs write, then to brightnessctl). Left-click opens a slider
// popup on an overlay layer surface; scroll adjusts directly.
// ---------------------------------------------------------------------------
// A sun no font can turn into a cog: solid disc, then eight thin
// round-capped rays with a clear GAP between disc and rays. Gears have
// teeth fused to the rim and a hole in the middle — the gap and the
// filled centre are exactly what make this read as a sun at 13 px.
void draw_sun_icon(cairo_t* cr, double cx, double cy, double size,
                   const Color& c) {
    const double disc = size * 0.30;        // filled centre
    const double r0   = size * 0.42;        // rays start (gap after disc)
    const double r1   = size * 0.58;        // rays end
    cairo_save(cr);
    col(cr, c, 1.0);
    cairo_arc(cr, cx, cy, disc, 0, 2 * M_PI);
    cairo_fill(cr);
    cairo_set_line_width(cr, std::max(1.2, size * 0.09));
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    for (int i = 0; i < 8; ++i) {
        double ang = i * M_PI / 4.0;        // one ray straight up: sun pose,
        double ca = cos(ang), sa = sin(ang); // not the rotated gear pose
        cairo_move_to(cr, cx + ca * r0, cy + sa * r0);
        cairo_line_to(cr, cx + ca * r1, cy + sa * r1);
    }
    cairo_stroke(cr);
    cairo_restore(cr);
}

// "auto" (the new default) draws the vector sun. The OLD default \uf185 is
// treated the same, so every existing conf that merely saved the default
// gets the fix without editing anything; only a deliberately chosen glyph
// keeps font rendering.
bool brightness_vector_icon() {
    return cfg.brightness_glyph == "auto" ||
           cfg.brightness_glyph == "\uf185";
}

void draw_brightness_slider(cairo_t* cr, int w, int h, double frac,
                            const char* label) {
    col(cr, cfg.c_bg, std::min(1.0, cfg.c_bg.a + 0.04));
    cairo_rectangle(cr, 0, 0, w, h);
    cairo_fill(cr);
    col(cr, cfg.c_ws_bg, 1.0);
    cairo_rectangle(cr, 0.5, 0.5, w - 1, h - 1);
    cairo_stroke(cr);
    cairo_font_extents_t fe;
    cairo_font_extents(cr, &fe);
    if (brightness_vector_icon()) {
        draw_sun_icon(cr, 23, h / 2.0, 17, cfg.c_fg);
    } else {
        col(cr, cfg.c_fg, 1.0);
        cairo_move_to(cr, 12, h / 2.0 + (fe.ascent - fe.descent) / 2);
        cairo_show_text(cr, label);
    }
    const double bx = 46, bw = w - bx - 52, cy = h / 2.0;
    col(cr, cfg.c_ws_bg, 1.0); // track
    cairo_rectangle(cr, bx, cy - 2, bw, 4);
    cairo_fill(cr);
    col(cr, cfg.c_accent, 1.0); // fill + knob
    cairo_rectangle(cr, bx, cy - 2, bw * frac, 4);
    cairo_fill(cr);
    cairo_arc(cr, bx + bw * frac, cy, 6, 0, 2 * 3.14159265);
    cairo_fill(cr);
    char pct[8];
    snprintf(pct, sizeof pct, "%d%%", (int)(frac * 100 + 0.5));
    col(cr, cfg.c_fg, 1.0);
    cairo_move_to(cr, bx + bw + 10, h / 2.0 + (fe.ascent - fe.descent) / 2);
    cairo_show_text(cr, pct);
}

namespace {
class BrightnessModule : public TextModule {
public:
    bool enabled() const override { return cfg.show_brightness; }

    ~BrightnessModule() override {
        if (uevent_fd_ >= 0) close(uevent_fd_);
        if (close_fd_ >= 0) close(close_fd_);
    }

    void init(Bar& bar) override {
        bar_ = &bar;
        const char* env = getenv("MATTBAR_BACKLIGHT_DIR");
        dir_ = env ? env : "/sys/class/backlight";
        discover();
        read_state();
        refresh();
        if (dev_.empty())
            fprintf(stderr,
                    "mattbar: brightness: no backlight device in %s; module "
                    "hidden (external monitors use DDC, which the kernel "
                    "does not expose here)\n", dir_.c_str());
        // The uevent watch is bound even when no device exists yet, so a
        // backlight driver that loads after the bar (or comes and goes)
        // shows/hides the module live instead of requiring a restart.
        // kernel uevents on brightness changes, wherever they come from
        uevent_fd_ = socket(AF_NETLINK, SOCK_RAW | SOCK_NONBLOCK | SOCK_CLOEXEC,
                            NETLINK_KOBJECT_UEVENT);
        if (uevent_fd_ >= 0) {
            sockaddr_nl nl{};
            nl.nl_family = AF_NETLINK;
            nl.nl_groups = 1;
            if (bind(uevent_fd_, (sockaddr*)&nl, sizeof nl) < 0) {
                close(uevent_fd_);
                uevent_fd_ = -1;
            } else {
                bar.add_fd(uevent_fd_, [this](uint32_t) {
                    char    buf[2048];
                    ssize_t n;
                    bool    hit = false;
                    while ((n = recv(uevent_fd_, buf, sizeof buf, 0)) > 0)
                        for (ssize_t i = 0; i + 19 < n; ++i)
                            if (!memcmp(buf + i, "SUBSYSTEM=backlight", 19))
                                hit = true;
                    if (hit) {
                        bool had = !dev_.empty();
                        read_state();
                        if (dev_.empty() || cur_ < 0 || max_ <= 0) {
                            discover(); // device appeared or vanished
                            read_state();
                            if (!had && !dev_.empty())
                                DBG("brightness: device %s appeared",
                                    dev_.c_str());
                        }
                        refresh();
                        if (slider_.surf) slider_draw();
                    }
                }, "backlight-uevent");
            }
        }
        // slider auto-close after the pointer leaves it
        close_fd_ = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
        bar.add_fd(close_fd_, [this](uint32_t) {
            uint64_t n;
            while (read(close_fd_, &n, sizeof n) > 0) {}
            slider_.destroy();
        }, "brightness-close");
        pump_.on_teardown = [this](const char*) { wbus_ = nullptr; };
    }

    void tick() override { refresh(); }

    static constexpr double SUN = 14, SUN_GAP = 5;

    double width(cairo_t* cr) override {
        if (!brightness_vector_icon()) {
            resolve_glyph(cr);
            return TextModule::width(cr);
        }
        if (text_.empty()) return 0; // no backlight device
        if (cfg_vertical()) return TextModule::width(cr) + SUN + SUN_GAP;
        return SUN + SUN_GAP + text_width(cr, text_);
    }
    void draw(cairo_t* cr, double a, double t) override {
        last_a_ = a;
        if (!brightness_vector_icon()) {
            TextModule::draw(cr, a, t);
            return;
        }
        if (text_.empty()) return;
        if (!cfg_vertical()) {
            draw_sun_icon(cr, a + SUN / 2.0, t / 2.0, SUN, color_);
            draw_text(cr, text_, a + SUN + SUN_GAP, t, color_);
        } else {
            // vertical: sun above the percentage, both centered in the bar
            draw_sun_icon(cr, t / 2.0, a + SUN / 2.0 + 2, SUN, color_);
            cairo_font_extents_t fe;
            cairo_font_extents(cr, &fe);
            cairo_set_source_rgba(cr, color_.r, color_.g, color_.b, 1.0);
            cairo_move_to(cr, (t - text_width(cr, text_)) / 2.0,
                          a + SUN + SUN_GAP + fe.ascent);
            cairo_show_text(cr, text_.c_str());
        }
    }

    bool on_click(double, int button) override {
        if (button != BTN_LEFT || dev_.empty()) return false;
        if (auto* sh = mattbar_shell(); sh && sh->is_open("omarchy.monitor")) {
            sh->hide("omarchy.monitor");
            return true;
        }
        if (slider_.surf) {
            slider_.destroy();
            return true;
        }
        open_slider();
        return true;
    }
    bool on_scroll(double, int dir) override {
        if (dev_.empty()) return false;
        int pct = (int)(frac() * 100 + 0.5) - dir * cfg.brightness_step;
        set_frac(std::clamp(pct, 1, 100) / 100.0);
        return true;
    }

private:
    double frac() const { return max_ > 0 ? (double)cur_ / max_ : 0; }

    void discover() {
        std::vector<std::string> devs;
        if (DIR* d = opendir(dir_.c_str())) {
            while (dirent* e = readdir(d))
                if (e->d_name[0] != '.') devs.push_back(e->d_name);
            closedir(d);
        }
        std::sort(devs.begin(), devs.end());
        dev_ = devs.empty() ? "" : devs.front();
    }
    int read_int(const std::string& path) const {
        FILE* f = fopen(path.c_str(), "r");
        if (!f) return -1;
        int v = -1;
        if (fscanf(f, "%d", &v) != 1) v = -1;
        fclose(f);
        return v;
    }
    void read_state() {
        if (dev_.empty()) return;
        max_ = read_int(dir_ + "/" + dev_ + "/max_brightness");
        cur_ = read_int(dir_ + "/" + dev_ + "/brightness");
    }

    void refresh() {
        if (!bar_) return;
        if (dev_.empty() || max_ <= 0 || cur_ < 0) {
            set_text(*bar_, "", cfg.c_dim);
            return;
        }
        char t[32];
        if (brightness_vector_icon())
            snprintf(t, sizeof t, "%d%%", (int)(frac() * 100 + 0.5));
        else
            snprintf(t, sizeof t, "%s %d%%", glyph().c_str(),
                     (int)(frac() * 100 + 0.5));
        set_text(*bar_, t, cfg.c_fg);
        DBG("brightness: '%s'", t);
    }

    // ---- writing ---------------------------------------------------------
    void set_frac(double f) {
        f = std::clamp(f, 0.01, 1.0); // never fully dark
        int raw = std::max(1, (int)(f * max_ + 0.5));
        if (raw == cur_) return;
        cur_ = raw; // optimistic; uevent confirms/corrects
        refresh();
        if (slider_.surf) slider_draw();
        DBG("brightness: set %d/%d", raw, max_);
        if (!wbus_) {
            sd_bus* b = nullptr;
            if (sd_bus_open_system(&b) >= 0) {
                wbus_ = b;
                pump_.attach(*bar_, b, "brightness-write");
            }
        }
        if (wbus_) {
            struct Ctx { BrightnessModule* m; int raw; };
            auto* ctx = new Ctx{this, raw};
            sd_bus_call_method_async(
                wbus_, nullptr, "org.freedesktop.login1",
                "/org/freedesktop/login1/session/auto",
                "org.freedesktop.login1.Session", "SetBrightness",
                [](sd_bus_message* m, void* ud, sd_bus_error*) -> int {
                    auto* c = static_cast<Ctx*>(ud);
                    if (sd_bus_message_is_method_error(m, nullptr))
                        c->m->fallback_write(c->raw);
                    delete c;
                    return 0;
                },
                ctx, "ssu", "backlight", dev_.c_str(), (uint32_t)raw);
            pump_.process();
        } else {
            fallback_write(raw);
        }
    }
    void fallback_write(int raw) {
        std::string path = dir_ + "/" + dev_ + "/brightness";
        FILE*       f    = fopen(path.c_str(), "w");
        if (f) {
            fprintf(f, "%d", raw);
            fclose(f);
            DBG("brightness: sysfs write %d", raw);
            return;
        }
        spawn_detached("brightnessctl set " + std::to_string(raw));
        DBG("brightness: brightnessctl fallback %d", raw);
    }

    // ---- slider popup ----------------------------------------------------
    static constexpr int SW = 240, SH = 44;
    void open_slider() {
        // Same placement helper as the calendar/bell/power panels: centered
        // on the module, clamped to stay fully on screen, on the monitor
        // whose bar was clicked.
        double along = bar_->slot_along(this);
        if (along < 0) along = last_a_;
        PopupPlace pl = popup_place(along, SW, bar_->along_length());
        uint32_t anchor = pl.anchor;
        int mt = pl.mt, mr = pl.mr, mb = pl.mb, ml = pl.ml;
        slider_.paint = [this](cairo_t* cr) {
            draw_brightness_slider(cr, SW, SH, frac(), glyph().c_str());
        };
        slider_.click = [this](double x, double, int btn) {
            if (btn != BTN_LEFT) return;
            dragging_ = true;
            disarm_close();
            apply_x(x);
        };
        slider_.pmotion = [this](double x, double) {
            if (dragging_) apply_x(x);
        };
        slider_.prelease = [this](int) { dragging_ = false; };
        slider_.pscroll  = [this](int d) {
            disarm_close();
            on_scroll(0, d);
        };
        slider_.pleave = [this] {
            dragging_ = false;
            arm_close();
        };
        slider_.ensure(*bar_, anchor, mt, mr, mb, ml, "mattbar-brightness",
                       SW, SH, bar_->current_output());
        slider_draw();
    }
    void slider_draw() { slider_.draw(); }
    void apply_x(double x) {
        const double bx = 46, bw = SW - bx - 52;
        set_frac(std::clamp((x - bx) / bw, 0.0, 1.0));
    }
    void arm_close() {
        if (close_fd_ < 0) return;
        itimerspec ts{};
        ts.it_value.tv_sec  = 1;
        ts.it_value.tv_nsec = 500 * 1000000L;
        timerfd_settime(close_fd_, 0, &ts, nullptr);
    }
    void disarm_close() {
        if (close_fd_ < 0) return;
        itimerspec off{};
        timerfd_settime(close_fd_, 0, &off, nullptr);
    }

    // ---- glyph (bar-font check, text fallback) ---------------------------
    const std::string& glyph() const {
        return resolved_.empty() ? cfg.brightness_glyph : resolved_;
    }
    void resolve_glyph(cairo_t* cr) {
        if (checked_ == cfg.font + cfg.brightness_glyph) return;
        checked_ = cfg.font + cfg.brightness_glyph;
        auto mapped = [&](const std::string& t) {
            cairo_scaled_font_t* sf = cairo_get_scaled_font(cr);
            cairo_glyph_t*       g  = nullptr;
            int                  n  = 0;
            bool ok = cairo_scaled_font_text_to_glyphs(
                          sf, 0, 0, t.c_str(), (int)t.size(), &g, &n, nullptr,
                          nullptr, nullptr) == CAIRO_STATUS_SUCCESS &&
                      n > 0;
            for (int i = 0; ok && i < n; ++i)
                if (g[i].index == 0) ok = false;
            if (g) cairo_glyph_free(g);
            return ok;
        };
        std::string pick = cfg.brightness_glyph;
        if (!mapped(pick)) pick = "\U000F00DF"; // nf-md-brightness-6
        if (!mapped(pick)) pick = "bri";
        if (pick != resolved_) {
            resolved_ = pick;
            refresh();
        }
    }

    Bar*        bar_       = nullptr;
    std::string dir_, dev_;
    int         cur_ = -1, max_ = -1;
    int         uevent_fd_ = -1, close_fd_ = -1;
    double      last_a_    = 0;
    bool        dragging_  = false;
    PopupWin    slider_;
    SdPump      pump_;
    sd_bus*     wbus_ = nullptr;
    std::string checked_, resolved_;
};
} // namespace
Module* make_brightness() { return new BrightnessModule; }

// ---------------------------------------------------------------------------
// Media (MPRIS over the session bus), on the same event-driven SdPump: an
// initial ListNames sweep, then NameOwnerChanged (arg0namespace-filtered
// daemon-side) tracks players appearing/vanishing and PropertiesChanged on
// the fixed MPRIS object path tracks title/artist/status. Zero polling.
//   left-click  -> PlayPause     right-click -> Next     scroll -> Next/Prev
// Shows the active player: the most recently changed *playing* player, else
// the most recently changed one. Hidden when no players exist.
// ---------------------------------------------------------------------------
namespace {
static uint64_t mono_ms() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000ULL;
}

class MediaModule : public TextModule {
public:
    ~MediaModule() override {
        close_popup();
        if (art_) cairo_surface_destroy(art_);
        if (retry_fd_ >= 0) close(retry_fd_);
        if (close_fd_ >= 0) close(close_fd_);
    }
    bool enabled() const override { return cfg.show_media; }

    void init(Bar& bar) override {
        bar_              = &bar;
        close_fd_ = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
        if (close_fd_ >= 0)
            bar.add_fd(close_fd_, [this](uint32_t) {
                uint64_t n;
                while (read(close_fd_, &n, sizeof n) > 0) {}
                close_popup();
            }, "media-popup-close");
        pump_.on_teardown = [this](const char* why) {
            bus_ = nullptr;
            players_.clear();
            refresh();
            schedule_retry(why); // reconnect with backoff, not death
        };
        retry_fd_ = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
        bar.add_fd(retry_fd_, [this](uint32_t) {
            uint64_t n;
            while (read(retry_fd_, &n, sizeof n) > 0) {}
            if (!bus_ && setup_bus()) attempts_ = 0;
            else if (!bus_) schedule_retry("session bus still unavailable");
            else if (need_sweep_) {
                need_sweep_ = false;
                sd_bus_call_method_async(bus_, nullptr,
                                         "org.freedesktop.DBus",
                                         "/org/freedesktop/DBus",
                                         "org.freedesktop.DBus", "ListNames",
                                         on_names, this, nullptr);
                pump_.process();
            }
        }, "mpris-retry");
        if (!setup_bus()) {
            schedule_retry("session bus unavailable");
            refresh();
        }
    }

    void schedule_retry(const char* note) {
        if (++attempts_ > 6) {
            fprintf(stderr,
                    "mattbar: media: still failing after %d attempts\n",
                    attempts_ - 1);
            return;
        }
        if (note)
            fprintf(stderr, "mattbar: media: %s (retry %d in %lds)\n", note,
                    attempts_, 1L << attempts_);
        itimerspec ts{};
        ts.it_value.tv_sec = 1L << attempts_;
        timerfd_settime(retry_fd_, 0, &ts, nullptr);
    }

    bool setup_bus() {
        sd_bus* b = nullptr;
        if (sd_bus_open_user(&b) < 0) return false;
        bus_ = b;
        sd_bus_set_method_call_timeout(bus_, 5 * 1000 * 1000ULL); // async; generous for login-storm busses
        if (!getenv("MATTBAR_TEST_NO_OWNER_MATCH")) // test hook only
            sd_bus_add_match(bus_, nullptr,
                             "type='signal',sender='org.freedesktop.DBus',"
                             "path='/org/freedesktop/DBus',"
                             "interface='org.freedesktop.DBus',"
                             "member='NameOwnerChanged',"
                             "arg0namespace='org.mpris.MediaPlayer2'",
                             on_owner, this);
        // One fixed object path for every MPRIS player: daemon-side filter
        // keeps all non-media property traffic away from this process.
        sd_bus_add_match(bus_, nullptr,
                         "type='signal',path='/org/mpris/MediaPlayer2',"
                         "interface='org.freedesktop.DBus.Properties',"
                         "member='PropertiesChanged'",
                         on_props, this);
        sd_bus_call_method_async(bus_, nullptr, "org.freedesktop.DBus",
                                 "/org/freedesktop/DBus",
                                 "org.freedesktop.DBus", "ListNames", on_names,
                                 this, nullptr);
        pump_.attach(*bar_, bus_, "mpris");
        pump_.process();
        return true;
    }

    void tick() override { refresh(); }

    double width(cairo_t* cr) override {
        // Same run-time glyph verification as the bluetooth module: prefer
        // the Nerd Font play/pause runes, degrade to ASCII on any font.
        if (checked_font_ != cfg.font) {
            checked_font_ = cfg.font;
            auto mapped   = [&](const char* t) {
                cairo_scaled_font_t* sf = cairo_get_scaled_font(cr);
                cairo_glyph_t*       g  = nullptr;
                int                  n  = 0;
                bool ok = cairo_scaled_font_text_to_glyphs(
                              sf, 0, 0, t, (int)strlen(t), &g, &n, nullptr,
                              nullptr, nullptr) == CAIRO_STATUS_SUCCESS &&
                          n > 0;
                for (int i = 0; ok && i < n; ++i)
                    if (g[i].index == 0) ok = false;
                if (g) cairo_glyph_free(g);
                return ok;
            };
            bool nf = mapped("\uf04b") && mapped("\uf04c");
            play_   = nf ? "\uf04b" : ">";
            pause_  = nf ? "\uf04c" : "||";
            refresh();
        }
        return TextModule::width(cr);
    }

    bool on_click(double, int button) override {
        if (button == BTN_MIDDLE) {
            cycle_source();
            return true;
        }
        if (button == BTN_RIGHT) {
            toggle_popup();
            return true;
        }
        const Player* p = active();
        if (!p || !bus_) return false;
        if (button != BTN_LEFT) return false;
        sd_bus_call_method_async(bus_, nullptr, p->name.c_str(),
                                 "/org/mpris/MediaPlayer2",
                                 "org.mpris.MediaPlayer2.Player", "PlayPause",
                                 nullptr, nullptr, nullptr);
        pump_.process();
        return true;
    }

    bool on_scroll(double, int dir) override {
        const Player* p = active();
        if (!p || !bus_) return false;
        sd_bus_call_method_async(bus_, nullptr, p->name.c_str(),
                                 "/org/mpris/MediaPlayer2",
                                 "org.mpris.MediaPlayer2.Player",
                                 dir > 0 ? "Next" : "Previous", nullptr,
                                 nullptr, nullptr);
        pump_.process();
        return true;
    }

    static void switch_source() {
        if (g_media) g_media->cycle_source();
    }

private:
    struct Player {
        std::string name;   // well-known org.mpris.MediaPlayer2.*
        std::string status; // Playing / Paused / Stopped
        std::string title, artist, album, art_url;
        uint64_t    seq = 0; // recency of last change
    };

    const Player* active() const {
        if (!prefer_.empty()) {
            for (auto& [u, p] : players_)
                if (p.name == prefer_ &&
                    (p.status == "Playing" || p.status == "Paused"))
                    return &p;
        }
        const Player* best = nullptr;
        for (auto& [u, p] : players_) { // playing beats paused, recent beats old
            if (!best || (p.status == "Playing") > (best->status == "Playing") ||
                ((p.status == "Playing") == (best->status == "Playing") &&
                 p.seq > best->seq))
                best = &p;
        }
        return best;
    }
    void cycle_source() {
        std::vector<std::string> names;
        for (auto& [u, p] : players_)
            if (p.status == "Playing" || p.status == "Paused")
                names.push_back(p.name);
        if (names.empty()) return;
        std::sort(names.begin(), names.end());
        auto it = std::find(names.begin(), names.end(), prefer_);
        if (it == names.end()) prefer_ = names[0];
        else
            prefer_ = names[(it - names.begin() + 1) % names.size()];
        refresh();
    }

    // a{sv} at current position: PlaybackStatus / Metadata for `unique`.
    void read_player_props(sd_bus_message* m, const std::string& unique) {
        if (sd_bus_message_enter_container(m, 'a', "{sv}") < 0) return;
        Player& p = players_[unique];
        while (sd_bus_message_enter_container(m, 'e', "sv") > 0) {
            const char* key = nullptr;
            sd_bus_message_read(m, "s", &key);
            std::string k    = key ? key : "";
            bool        used = false;
            if (k == "PlaybackStatus") {
                const char* v = nullptr;
                if (sd_bus_message_enter_container(m, 'v', "s") >= 0) {
                    sd_bus_message_read(m, "s", &v);
                    sd_bus_message_exit_container(m);
                    if (v) p.status = v;
                    used = true;
                }
            } else if (k == "Metadata") {
                if (sd_bus_message_enter_container(m, 'v', "a{sv}") >= 0) {
                    read_metadata(m, p);
                    sd_bus_message_exit_container(m);
                    used = true;
                }
            }
            if (!used) sd_bus_message_skip(m, "v");
            sd_bus_message_exit_container(m);
        }
        sd_bus_message_exit_container(m);
        p.seq = ++seq_;
    }

    void read_metadata(sd_bus_message* m, Player& p) {
        if (sd_bus_message_enter_container(m, 'a', "{sv}") < 0) return;
        while (sd_bus_message_enter_container(m, 'e', "sv") > 0) {
            const char* key = nullptr;
            sd_bus_message_read(m, "s", &key);
            std::string k    = key ? key : "";
            bool        used = false;
            if (k == "xesam:title") {
                const char* v = nullptr;
                if (sd_bus_message_enter_container(m, 'v', "s") >= 0) {
                    sd_bus_message_read(m, "s", &v);
                    sd_bus_message_exit_container(m);
                    p.title = v ? v : "";
                    used    = true;
                }
            } else if (k == "xesam:artist") {
                if (sd_bus_message_enter_container(m, 'v', "as") >= 0) {
                    if (sd_bus_message_enter_container(m, 'a', "s") >= 0) {
                        const char* v = nullptr;
                        if (sd_bus_message_read(m, "s", &v) > 0 && v)
                            p.artist = v;
                        else
                            p.artist.clear();
                        // drain remaining artists
                        while (sd_bus_message_read(m, "s", &v) > 0) {}
                        sd_bus_message_exit_container(m);
                    }
                    sd_bus_message_exit_container(m);
                    used = true;
                }
            } else if (k == "xesam:album") {
                const char* v = nullptr;
                if (sd_bus_message_enter_container(m, 'v', "s") >= 0) {
                    sd_bus_message_read(m, "s", &v);
                    sd_bus_message_exit_container(m);
                    p.album = v ? v : "";
                    used    = true;
                }
            } else if (k == "mpris:artUrl") {
                const char* v = nullptr;
                if (sd_bus_message_enter_container(m, 'v', "s") >= 0) {
                    sd_bus_message_read(m, "s", &v);
                    sd_bus_message_exit_container(m);
                    p.art_url = v ? v : "";
                    used      = true;
                }
            }
            if (!used) sd_bus_message_skip(m, "v");
            sd_bus_message_exit_container(m);
        }
        sd_bus_message_exit_container(m);
    }

    void query(const std::string& wellknown) {
        sd_bus_message* m = nullptr;
        if (sd_bus_message_new_method_call(
                bus_, &m, wellknown.c_str(), "/org/mpris/MediaPlayer2",
                "org.freedesktop.DBus.Properties", "GetAll") < 0)
            return;
        sd_bus_message_append(m, "s", "org.mpris.MediaPlayer2.Player");
        // remember which well-known name this reply belongs to
        pending_.push_back(wellknown);
        sd_bus_call_async(bus_, nullptr, m, on_getall,
                          new std::string(wellknown), 0);
        sd_bus_message_unref(m);
    }

    static int on_getall(sd_bus_message* m, void* ud, sd_bus_error*) {
        std::unique_ptr<std::string> wk(static_cast<std::string*>(ud));
        MediaModule* self = g_media;
        if (!self || !self->bus_) return 0;
        if (sd_bus_message_is_method_error(m, nullptr)) return 0;
        const char* unique = sd_bus_message_get_sender(m);
        if (!unique) return 0;
        self->players_[unique].name = *wk;
        self->read_player_props(m, unique);
        self->refresh();
        return 0;
    }

    static int on_names(sd_bus_message* m, void* ud, sd_bus_error*) {
        auto* self          = static_cast<MediaModule*>(ud);
        self->sweep_pending_ = false;
        if (sd_bus_message_is_method_error(m, nullptr)) {
            const sd_bus_error* e = sd_bus_message_get_error(m);
            self->need_sweep_     = true;
            self->schedule_retry(e && e->name ? e->name
                                              : "ListNames failed");
            return 0;
        }
        DBG("media: name sweep complete");
        self->attempts_ = 0;
        if (sd_bus_message_enter_container(m, 'a', "s") < 0) return 0;
        const char* n = nullptr;
        while (sd_bus_message_read(m, "s", &n) > 0)
            if (n && strncmp(n, "org.mpris.MediaPlayer2.", 23) == 0) {
                DBG("media: found %s", n);
                self->query(n);
            }
        sd_bus_message_exit_container(m);
        return 0;
    }

    static int on_owner(sd_bus_message* m, void* ud, sd_bus_error*) {
        auto*       self = static_cast<MediaModule*>(ud);
        const char *name = nullptr, *oldo = nullptr, *newo = nullptr;
        if (sd_bus_message_read(m, "sss", &name, &oldo, &newo) < 0) return 0;
        if (oldo && *oldo) self->players_.erase(oldo);
        if (newo && *newo && name) self->query(name);
        self->refresh();
        return 0;
    }

    static int on_props(sd_bus_message* m, void* ud, sd_bus_error*) {
        auto*       self   = static_cast<MediaModule*>(ud);
        const char* iface  = nullptr;
        const char* sender = sd_bus_message_get_sender(m);
        if (sd_bus_message_read(m, "s", &iface) < 0 || !sender) return 0;
        if (!iface || strcmp(iface, "org.mpris.MediaPlayer2.Player")) return 0;
        if (!self->players_.count(sender)) {
            // A live MPRIS player we never discovered: NameOwnerChanged can
            // be missed (startup races; broker match quirks on driver
            // signals). The event itself is proof a player exists - resweep.
            uint64_t now = mono_ms();
            if (!self->sweep_pending_ &&
                now - self->last_sweep_ms_ > 1500) {
                self->sweep_pending_ = true;
                self->last_sweep_ms_ = now;
                DBG("media: props from unknown %s; resweeping names", sender);
                sd_bus_call_method_async(self->bus_, nullptr,
                                         "org.freedesktop.DBus",
                                         "/org/freedesktop/DBus",
                                         "org.freedesktop.DBus", "ListNames",
                                         on_names, self, nullptr);
            }
            return 0;
        }
        self->read_player_props(m, sender);
        self->refresh();
        return 0;
    }

    // "org.mpris.MediaPlayer2.chromium.instance42" -> "chromium"
    static std::string short_name(const std::string& wk) {
        std::string n =
            wk.rfind("org.mpris.MediaPlayer2.", 0) == 0 ? wk.substr(23) : wk;
        auto dot = n.find('.');
        if (dot != std::string::npos) n.resize(dot);
        return n.empty() ? std::string("media") : n;
    }

    static std::string percent_decode(const std::string& s) {
        std::string o;
        o.reserve(s.size());
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '%' && i + 2 < s.size()) {
                char hex[3] = {s[i + 1], s[i + 2], 0};
                o += (char)strtol(hex, nullptr, 16);
                i += 2;
            } else
                o += s[i];
        }
        return o;
    }

    static std::string file_from_url(const std::string& url) {
        std::string u = url;
        if (u.rfind("file://", 0) == 0) {
            u = u.substr(7);
            if (u.rfind("localhost", 0) == 0) u = u.substr(9);
        }
        return percent_decode(u);
    }

    static std::string shell_quote(const std::string& s) {
        std::string o = "'";
        for (char c : s) {
            if (c == '\'') o += "'\\''";
            else o += c;
        }
        o += "'";
        return o;
    }

    std::string art_cache_path() const {
        const char* r = getenv("XDG_RUNTIME_DIR");
        if (r && *r) return std::string(r) + "/mattbar-art";
        return "/tmp/mattbar-art";
    }

    void load_art(const Player* p) {
        std::string url = p ? p->art_url : "";
        if (url == art_url_) return;
        art_url_ = url;
        if (art_) {
            cairo_surface_destroy(art_);
            art_ = nullptr;
        }
        if (url.empty()) return;
        if (url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0) {
            if (!bar_) return;
            std::string cache = art_cache_path();
            art_fetch_.run(
                *bar_,
                "curl -fsL --max-time 4 -o " + shell_quote(cache) + " " +
                    shell_quote(url),
                [this, url, cache](const std::string&, int st) {
                    if (st != 0 || art_url_ != url) return;
                    if (art_) cairo_surface_destroy(art_);
                    art_ = image_load_file(cache);
                    if (popup_.surf) popup_.draw();
                },
                5000);
            return;
        }
        art_ = image_load_file(file_from_url(url));
    }

    void hold_popup(bool on) {
        if (!bar_ || on == popup_hold_) return;
        popup_hold_ = on;
        bar_->hold_open(on);
    }

    void arm_popup_close(int ms = 1400) {
        if (close_fd_ < 0) return;
        itimerspec ts{};
        ts.it_value.tv_sec  = ms / 1000;
        ts.it_value.tv_nsec = (ms % 1000) * 1000000L;
        timerfd_settime(close_fd_, 0, &ts, nullptr);
    }
    void disarm_popup_close() {
        if (close_fd_ < 0) return;
        itimerspec off{};
        timerfd_settime(close_fd_, 0, &off, nullptr);
    }

    void close_popup() {
        disarm_popup_close();
        hold_popup(false);
        popup_.destroy();
        popup_hits_.clear();
    }

    int popup_w() const { return 320; }
    int popup_h() const {
        int n = 0;
        for (auto& [u, p] : players_)
            if (p.status == "Playing" || p.status == "Paused") ++n;
        int h = 12 + 64 + 12 + 36 + 12;
        if (n > 1) h += 10 + n * 34;
        return h;
    }

    void rrect(cairo_t* cr, double x, double y, double w, double h, double r) {
        cairo_new_sub_path(cr);
        cairo_arc(cr, x + w - r, y + r, r, -M_PI_2, 0);
        cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI_2);
        cairo_arc(cr, x + r, y + h - r, r, M_PI_2, M_PI);
        cairo_arc(cr, x + r, y + r, r, M_PI, 1.5 * M_PI);
        cairo_close_path(cr);
    }

    void paint_popup(cairo_t* cr) {
        const int W = popup_w(), H = popup_h();
        cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
        cairo_set_source_rgba(cr, 0, 0, 0, 0);
        cairo_paint(cr);
        cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
        cairo_set_source_rgba(cr, cfg.c_bg.r, cfg.c_bg.g, cfg.c_bg.b,
                              std::max(cfg.c_bg.a, 0.96));
        rrect(cr, 0.5, 0.5, W - 1, H - 1, 10);
        cairo_fill_preserve(cr);
        cairo_set_source_rgba(cr, cfg.c_ws_bg.r, cfg.c_ws_bg.g, cfg.c_ws_bg.b, 1);
        cairo_set_line_width(cr, 1);
        cairo_stroke(cr);

        cairo_select_font_face(cr, cfg.font.c_str(), CAIRO_FONT_SLANT_NORMAL,
                               CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, cfg.font_size);
        popup_hits_.clear();

        const Player* p = active();
        const double ax = 12, ay = 12, asz = 64;
        cairo_save(cr);
        rrect(cr, ax, ay, asz, asz, 8);
        cairo_clip(cr);
        if (art_) {
            int sw = cairo_image_surface_get_width(art_);
            int sh = cairo_image_surface_get_height(art_);
            if (sw > 0 && sh > 0) {
                double s = std::max(asz / sw, asz / sh);
                cairo_translate(cr, ax + (asz - sw * s) / 2.0,
                                ay + (asz - sh * s) / 2.0);
                cairo_scale(cr, s, s);
                cairo_set_source_surface(cr, art_, 0, 0);
                cairo_paint(cr);
            }
        } else {
            cairo_set_source_rgba(cr, cfg.c_ws_bg.r, cfg.c_ws_bg.g,
                                  cfg.c_ws_bg.b, 1);
            cairo_paint(cr);
        }
        cairo_restore(cr);

        auto say = [&](double x, double y, const std::string& s, const Color& c) {
            cairo_set_source_rgba(cr, c.r, c.g, c.b, 1);
            cairo_font_extents_t fe;
            cairo_font_extents(cr, &fe);
            cairo_move_to(cr, x, y + (fe.ascent - fe.descent) / 2.0);
            cairo_show_text(cr, s.c_str());
        };
        double tx = ax + asz + 12;
        double tw = W - tx - 12;
        auto trunc = [&](std::string s, double maxw) {
            cairo_text_extents_t e;
            cairo_text_extents(cr, s.c_str(), &e);
            if (e.x_advance <= maxw) return s;
            while (s.size() > 1) {
                s.pop_back();
                while (!s.empty() &&
                       (static_cast<unsigned char>(s.back()) & 0xC0) == 0x80)
                    s.pop_back();
                std::string t = s + "\u2026";
                cairo_text_extents(cr, t.c_str(), &e);
                if (e.x_advance <= maxw) return t;
            }
            return s;
        };
        cairo_set_font_size(cr, cfg.font_size + 1);
        say(tx, ay + 14, trunc(p && !p->title.empty() ? p->title
                                                      : "Nothing playing",
                               tw),
            cfg.c_fg);
        cairo_set_font_size(cr, std::max(10.0, cfg.font_size - 1));
        if (p && !p->artist.empty())
            say(tx, ay + 34, trunc(p->artist, tw), cfg.c_dim);
        if (p && !p->album.empty())
            say(tx, ay + 50, trunc(p->album, tw), cfg.c_dim);

        // transport
        double by = ay + asz + 14;
        auto btn = [&](double x, const char* label, int kind) {
            cairo_set_source_rgba(cr, cfg.c_ws_bg.r, cfg.c_ws_bg.g,
                                  cfg.c_ws_bg.b, 1);
            rrect(cr, x, by, 44, 28, 6);
            cairo_fill(cr);
            cairo_text_extents_t e;
            cairo_set_font_size(cr, cfg.font_size);
            cairo_text_extents(cr, label, &e);
            say(x + (44 - e.x_advance) / 2.0, by + 14, label, cfg.c_fg);
            popup_hits_.push_back({x, by, 44, 28, kind, -1});
        };
        bool playing = p && p->status == "Playing";
        btn(W / 2.0 - 22 - 52, "\u23ee", 0); // prev
        btn(W / 2.0 - 22, playing ? "\u23f8" : "\u25b6", 1);
        btn(W / 2.0 + 22 + 8, "\u23ed", 2); // next

        std::vector<std::string> names;
        for (auto& [u, pl] : players_)
            if (pl.status == "Playing" || pl.status == "Paused")
                names.push_back(pl.name);
        std::sort(names.begin(), names.end());
        if (names.size() > 1) {
            double y = by + 40;
            cairo_set_font_size(cr, std::max(9.0, cfg.font_size - 2));
            say(12, y, "SOURCES", cfg.c_accent);
            y += 16;
            cairo_set_font_size(cr, cfg.font_size);
            for (int i = 0; i < (int)names.size(); ++i) {
                bool sel = p && p->name == names[i];
                if (sel) {
                    cairo_set_source_rgba(cr, cfg.c_accent.r, cfg.c_accent.g,
                                          cfg.c_accent.b, 0.28);
                    rrect(cr, 10, y - 4, W - 20, 30, 6);
                    cairo_fill(cr);
                }
                const Player* sp = nullptr;
                for (auto& [u, pl] : players_)
                    if (pl.name == names[i]) {
                        sp = &pl;
                        break;
                    }
                std::string label =
                    sp && !sp->title.empty() ? sp->title : short_name(names[i]);
                say(18, y + 11, trunc(label, W - 40), cfg.c_fg);
                popup_hits_.push_back({10, y - 4, (double)W - 20, 30, 3, i});
                y += 34;
            }
            source_names_ = names;
        } else
            source_names_.clear();
        (void)H;
    }

    void popup_click(double x, double y, int btn) {
        disarm_popup_close();
        if (btn != BTN_LEFT) return;
        const Player* p = active();
        for (auto& h : popup_hits_) {
            if (x < h.x || y < h.y || x >= h.x + h.w || y >= h.y + h.h)
                continue;
            if (h.kind == 0 || h.kind == 1 || h.kind == 2) {
                if (!p || !bus_) return;
                const char* method = h.kind == 0   ? "Previous"
                                     : h.kind == 2 ? "Next"
                                                   : "PlayPause";
                sd_bus_call_method_async(bus_, nullptr, p->name.c_str(),
                                         "/org/mpris/MediaPlayer2",
                                         "org.mpris.MediaPlayer2.Player",
                                         method, nullptr, nullptr, nullptr);
                pump_.process();
                return;
            }
            if (h.kind == 3 && h.idx >= 0 &&
                h.idx < (int)source_names_.size()) {
                prefer_ = source_names_[h.idx];
                refresh();
                return;
            }
        }
    }

    void toggle_popup() {
        if (!bar_) return;
        if (popup_.surf) {
            close_popup();
            return;
        }
        const Player* p = active();
        load_art(p);
        popup_.kb_mode = 2;
        popup_.paint   = [this](cairo_t* cr) { paint_popup(cr); };
        popup_.click   = [this](double x, double y, int b) { popup_click(x, y, b); };
        popup_.pmotion = [this](double, double) { disarm_popup_close(); };
        popup_.pleave  = [this] { arm_popup_close(); };
        popup_.pkey    = [this](const Bar::KeyEvent& e) {
            if (e.escape()) close_popup();
        };
        double along = bar_->slot_along(this);
        if (along < 0) along = bar_->pointer_along();
        pop_place_ = popup_place(along, popup_w(), bar_->along_length());
        pop_out_   = bar_->current_output();
        hold_popup(true);
        popup_.ensure(*bar_, pop_place_.anchor, pop_place_.mt, pop_place_.mr,
                      pop_place_.mb, pop_place_.ml, "mattbar-media",
                      popup_w(), popup_h(), pop_out_);
        popup_.draw();
    }

    void refresh() {
        if (!bar_) return;
        const Player* p = active();
        // Only genuinely transporting players count; players with sparse
        // metadata (web radio, mpv, players mid-load) must still show -
        // an empty title fell through to <hidden> before, which made the
        // module invisible on machines whose player never sets one.
        if (!p || (p->status != "Playing" && p->status != "Paused")) {
            set_text(*bar_, "", cfg.c_dim);
            DBG("media: <hidden>");
            return;
        }
        std::string label =
            p->title.empty()
                ? short_name(p->name)
                : (p->artist.empty() || !cfg.media_show_artist
                       ? p->title
                       : p->artist + " - " + p->title);
        size_t cap = (size_t)cfg.media_len;
        if (label.size() > cap) { // UTF-8-safe truncation
            label.resize(cap);
            while (!label.empty() &&
                   (static_cast<unsigned char>(label.back()) & 0xC0) == 0x80)
                label.pop_back();
            if (!label.empty()) label.pop_back();
            label += "\u2026";
        }
        bool        playing = p->status == "Playing";
        std::string t = (playing ? play_ : pause_) + " " + label;
        set_text(*bar_, t, playing ? cfg.c_fg : cfg.c_dim);
        DBG("media: '%s'", t.c_str());
        load_art(p);
        if (popup_.surf) {
            if (!p) {
                close_popup();
            } else {
                popup_.ensure(*bar_, pop_place_.anchor, pop_place_.mt,
                              pop_place_.mr, pop_place_.mb, pop_place_.ml,
                              "mattbar-media", popup_w(), popup_h(),
                              pop_out_);
                popup_.draw();
            }
        }
    }

    struct PopHit {
        double x, y, w, h;
        int    kind = 0, idx = 0;
    };

    static MediaModule*                g_media; // for detached async replies
    int                                retry_fd_ = -1;
    int                                close_fd_ = -1;
    PopupWin                           popup_;
    bool                               popup_hold_ = false;
    cairo_surface_t*                   art_ = nullptr;
    std::string                        art_url_;
    AsyncCmd                           art_fetch_;
    std::vector<PopHit>                popup_hits_;
    std::vector<std::string>           source_names_;
    PopupPlace                         pop_place_{};
    wl_output*                         pop_out_ = nullptr;
    bool                               sweep_pending_ = false;
    bool                               need_sweep_    = false;
    uint64_t                           last_sweep_ms_ = 0;
    int                                attempts_ = 0;
    Bar*                               bar_ = nullptr;
    SdPump                             pump_;
    sd_bus*                            bus_ = nullptr;
    std::map<std::string, Player>      players_; // unique name -> state
    std::string                        prefer_;
    std::vector<std::string>           pending_;
    uint64_t                           seq_ = 0;
    std::string play_ = ">", pause_ = "||", checked_font_;

public:
    MediaModule() { g_media = this; }
};
MediaModule* MediaModule::g_media = nullptr;
} // namespace
Module* make_media() { return new MediaModule; }
void media_source_switch() { MediaModule::switch_source(); }

// Displays chip: same overlay as Super+Ctrl+D (omarchy.monitor).
// ---------------------------------------------------------------------------
namespace {
class DisplayModule : public TextModule {
public:
    bool enabled() const override { return cfg.show_display; }
    void init(Bar& bar) override {
        bar_ = &bar;
        set_text(*bar_, glyph_, cfg.c_fg);
    }
    double width(cairo_t* cr) override {
        resolve(cr);
        return TextModule::width(cr);
    }
    void draw(cairo_t* cr, double a, double t) override {
        resolve(cr);
        bool on = false;
        if (auto* sh = mattbar_shell()) on = sh->is_open("omarchy.monitor");
        set_text(*bar_, glyph_, on ? cfg.c_accent : cfg.c_fg);
        TextModule::draw(cr, a, t);
    }
    bool on_click(double, int button) override {
        if (button != BTN_LEFT) return false;
        if (auto* sh = mattbar_shell()) {
            sh->toggle("omarchy.monitor", "{}");
            if (bar_) bar_->request_draw();
            return true;
        }
        spawn_detached("mattbarctl shell toggle omarchy.monitor");
        return true;
    }

private:
    void resolve(cairo_t* cr) {
        if (checked_ == cfg.font) return;
        checked_ = cfg.font;
        auto mapped = [&](const char* t) {
            cairo_scaled_font_t* sf = cairo_get_scaled_font(cr);
            cairo_glyph_t*       g  = nullptr;
            int                  n  = 0;
            bool ok = cairo_scaled_font_text_to_glyphs(
                          sf, 0, 0, t, (int)strlen(t), &g, &n, nullptr,
                          nullptr, nullptr) == CAIRO_STATUS_SUCCESS &&
                      n > 0;
            for (int i = 0; ok && i < n; ++i)
                if (g[i].index == 0) ok = false;
            if (g) cairo_glyph_free(g);
            return ok;
        };
        const char* pick = "\uf108"; // nf-fa-desktop
        if (!mapped(pick)) pick = "\U000F0379"; // nf-md-monitor
        if (!mapped(pick)) pick = "DSP";
        glyph_ = pick;
        if (bar_) set_text(*bar_, glyph_, cfg.c_fg);
    }
    Bar*        bar_ = nullptr;
    std::string glyph_ = "\uf108", checked_;
};
} // namespace
Module* make_display() { return new DisplayModule; }

// ---------------------------------------------------------------------------
// Stay-awake toggle: holds a zwp_idle_inhibitor_v1 on the bar's own surface
// while active, so the compositor suspends idle actions (lock, dpms).
// Runtime state only — like Waybar's idle_inhibitor, it resets on restart.
// Coffee glyph with the usual run-time font fallback.
// ---------------------------------------------------------------------------
namespace {
class CaffeineModule : public TextModule {
public:
    bool enabled() const override { return cfg.show_caffeine; }
    void init(Bar& bar) override {
        bar_ = &bar;
        refresh();
    }
    void tick() override { refresh(); }
    double width(cairo_t* cr) override {
        if (checked_font_ != cfg.font) {
            checked_font_ = cfg.font;
            cairo_scaled_font_t* sf = cairo_get_scaled_font(cr);
            cairo_glyph_t*       g  = nullptr;
            int                  n  = 0;
            const char*          t  = "\uf0f4"; // nf-fa-coffee
            bool ok = cairo_scaled_font_text_to_glyphs(
                          sf, 0, 0, t, (int)strlen(t), &g, &n, nullptr,
                          nullptr, nullptr) == CAIRO_STATUS_SUCCESS &&
                      n > 0 && g[0].index != 0;
            if (g) cairo_glyph_free(g);
            glyph_ = ok ? t : "awake";
            refresh();
        }
        return TextModule::width(cr);
    }
    bool on_click(double, int button) override {
        if (button != BTN_LEFT) return false;
        if (inhibitor_) {
            zwp_idle_inhibitor_v1_destroy(inhibitor_);
            inhibitor_ = nullptr;
        } else if (bar_->idle_inhibit_manager() && bar_->bar_surface()) {
            inhibitor_ = zwp_idle_inhibit_manager_v1_create_inhibitor(
                bar_->idle_inhibit_manager(), bar_->bar_surface());
        } else {
            fprintf(stderr,
                    "mattbar: compositor lacks idle-inhibit; toggle is inert\n");
        }
        DBG("caffeine: %s", inhibitor_ ? "on" : "off");
        refresh();
        return true;
    }

private:
    void refresh() {
        if (!bar_) return;
        set_text(*bar_, glyph_, inhibitor_ ? cfg.c_accent : cfg.c_dim);
    }
    Bar*                    bar_       = nullptr;
    zwp_idle_inhibitor_v1*  inhibitor_ = nullptr;
    std::string             glyph_ = "awake", checked_font_;
};
} // namespace
Module* make_caffeine() { return new CaffeineModule; }

// ---------------------------------------------------------------------------
// Night light: same hyprsunset temperatures Omarchy uses (4000 / 6500 K).
// Accent when the filter is on; dim in daytime. Click toggles.
// ---------------------------------------------------------------------------
namespace {
class NightlightModule : public TextModule {
public:
    bool enabled() const override { return cfg.show_nightlight; }
    void init(Bar& bar) override {
        bar_ = &bar;
        nightlight_init(bar);
        nightlight_refresh();
        refresh();
    }
    void tick() override {
        if (++ticks_ >= 5) {
            ticks_ = 0;
            nightlight_refresh();
        }
        refresh();
    }
    double width(cairo_t* cr) override {
        if (checked_font_ != cfg.font) {
            checked_font_ = cfg.font;
            cairo_scaled_font_t* sf = cairo_get_scaled_font(cr);
            cairo_glyph_t*       g  = nullptr;
            int                  n  = 0;
            const char*          t  = "\U000F050E"; // nf-md-weather-night
            bool ok = cairo_scaled_font_text_to_glyphs(
                          sf, 0, 0, t, (int)strlen(t), &g, &n, nullptr,
                          nullptr, nullptr) == CAIRO_STATUS_SUCCESS &&
                      n > 0 && g[0].index != 0;
            if (g) cairo_glyph_free(g);
            glyph_ = ok ? t : "moon";
            refresh();
        }
        return TextModule::width(cr);
    }
    bool on_click(double, int button) override {
        if (button != BTN_LEFT) return false;
        nightlight_toggle();
        refresh();
        return true;
    }

private:
    void refresh() {
        if (!bar_) return;
        set_text(*bar_, glyph_,
                 nightlight_enabled() ? cfg.c_accent : cfg.c_dim);
    }
    Bar*        bar_          = nullptr;
    int         ticks_        = 0;
    std::string glyph_        = "moon";
    std::string checked_font_;
};
} // namespace
Module* make_nightlight() { return new NightlightModule; }

// ---------------------------------------------------------------------------
// Pin: click to keep the bar revealed (auto-hide paused); click again to
// resume. Never touches the exclusive zone, so windows are unaffected.
// ---------------------------------------------------------------------------
namespace {
class PinModule : public Module {
public:
    bool enabled() const override { return cfg.show_pin; }
    void init(Bar& bar) override { bar_ = &bar; }
    double width(cairo_t*) override { return 16; }

    void draw(cairo_t* cr, double a, double t) override {
        const bool pinned = bar_->pinned();
        const Color& c = pinned ? cfg.c_accent : cfg.c_dim;
        cairo_set_source_rgba(cr, c.r, c.g, c.b, 1.0);
        double cx = cfg_vertical() ? t / 2.0 : a + 8;
        double cy = cfg_vertical() ? a + 8 : t / 2.0;
        // pushpin: head circle + stem
        cairo_new_path(cr);
        cairo_arc(cr, cx, cy - 3, 3.5, 0, 2 * M_PI);
        if (pinned) {
            cairo_fill(cr);
        } else {
            cairo_set_line_width(cr, 1.2);
            cairo_stroke(cr);
        }
        cairo_set_line_width(cr, 1.6);
        cairo_move_to(cr, cx, cy + 1);
        cairo_line_to(cr, cx, cy + 7);
        cairo_stroke(cr);
    }

    bool on_click(double, int button) override {
        if (button != BTN_LEFT) return false;
        bar_->toggle_pinned();
        return true;
    }

private:
    Bar* bar_ = nullptr;
};
} // namespace
Module* make_pin() { return new PinModule; }

// ---------------------------------------------------------------------------
// More: overflow group. Modules assigned to layout_more are not drawn on
// the bar; they appear in a wrapping popup behind this ⋯ control.
// ---------------------------------------------------------------------------
namespace {
class MoreModule : public Module {
public:
    static MoreModule* g;
    MoreModule() { g = this; }
    ~MoreModule() override {
        g = nullptr;
        close_now();
        if (close_fd_ >= 0) close(close_fd_);
    }

    void init(Bar& bar) override {
        bar_ = &bar;
        close_fd_ = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
        if (close_fd_ >= 0)
            bar.add_fd(close_fd_, [this](uint32_t) {
                uint64_t n;
                while (read(close_fd_, &n, sizeof n) > 0) {}
                close_now();
            }, "more-close");
    }

    bool enabled() const override {
        return bar_ && has_items();
    }

    double width(cairo_t*) override { return 18; }

    void draw(cairo_t* cr, double a, double t) override {
        last_a_ = a;
        const bool on = popup_.surf != nullptr;
        const Color& c = on ? cfg.c_accent : cfg.c_fg;
        cairo_set_source_rgba(cr, c.r, c.g, c.b, 1.0);
        double cx = cfg_vertical() ? t / 2.0 : a + 9;
        double cy = cfg_vertical() ? a + 9 : t / 2.0;
        for (int i = -1; i <= 1; ++i) {
            cairo_arc(cr, cx + i * 5.0, cy, 1.6, 0, 2 * M_PI);
            cairo_fill(cr);
        }
    }

    bool on_click(double, int button) override {
        if (button != BTN_LEFT || !bar_) return false;
        if (popup_.surf) close_now();
        else open();
        return true;
    }

    void tick() override {
        if (!has_items()) {
            close_now();
            return;
        }
        if (popup_.surf) {
            relayout();
            popup_.draw();
        }
    }

    bool is_open() const { return popup_.surf != nullptr; }

    void close_now() {
        disarm_close();
        hold(false);
        popup_.destroy();
        slots_.clear();
        if (bar_) bar_->request_draw();
    }

private:
    struct Slot {
        Module* m = nullptr;
        double  x = 0, y = 0, w = 0, h = 0;
    };

    bool has_items() const {
        if (!bar_) return false;
        for (auto* m : bar_->more) {
            if (!m || !m->enabled()) continue;
            if (m->primary_only() && !bar_->current_is_primary()) continue;
            return true;
        }
        return false;
    }

    void hold(bool on) {
        if (!bar_ || on == holding_) return;
        holding_ = on;
        bar_->hold_open(on);
    }
    void arm_close(int ms = 1400) {
        if (close_fd_ < 0) return;
        itimerspec ts{};
        ts.it_value.tv_sec  = ms / 1000;
        ts.it_value.tv_nsec = (ms % 1000) * 1000000L;
        timerfd_settime(close_fd_, 0, &ts, nullptr);
    }
    void disarm_close() {
        if (close_fd_ < 0) return;
        itimerspec off{};
        timerfd_settime(close_fd_, 0, &off, nullptr);
    }

    double max_popup_w() const {
        double along = bar_ ? bar_->along_length() : 520;
        return std::clamp(along - 24.0, 180.0, 520.0);
    }
    double row_h() const { return (double)cfg_thickness(); }

    void setfont(cairo_t* cr) {
        cairo_select_font_face(cr, cfg.font.c_str(), CAIRO_FONT_SLANT_NORMAL,
                               CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, cfg.font_size);
    }

    void measure(cairo_t* cr) {
        slots_.clear();
        const double pad = 10, gap = 10;
        const double rh  = row_h();
        if (cfg_vertical()) {
            double y = pad;
            double thick = rh;
            for (auto* m : bar_->more) {
                if (!m || !m->enabled()) continue;
                if (m->primary_only() && !bar_->current_is_primary()) continue;
                setfont(cr);
                double mw = m->width(cr);
                if (mw <= 0.5) continue;
                slots_.push_back({m, pad, y, thick, mw});
                y += mw + gap;
            }
            pop_w_ = (int)std::lround(thick + pad * 2);
            pop_h_ = (int)std::lround(std::max(48.0, y + pad - gap));
        } else {
            const double maxw = max_popup_w();
            double x = pad, y = pad;
            double used_w = pad;
            for (auto* m : bar_->more) {
                if (!m || !m->enabled()) continue;
                if (m->primary_only() && !bar_->current_is_primary()) continue;
                setfont(cr);
                double mw = m->width(cr);
                if (mw <= 0.5) continue;
                if (x > pad && x + mw + pad > maxw) {
                    x = pad;
                    y += rh + gap;
                }
                slots_.push_back({m, x, y, mw, rh});
                x += mw + gap;
                used_w = std::max(used_w, x);
            }
            pop_w_ = (int)std::lround(std::max(120.0, used_w + pad - gap));
            pop_h_ = (int)std::lround(y + rh + pad);
        }
        if (slots_.empty()) {
            pop_w_ = 220;
            pop_h_ = 48;
        }
    }

    void relayout() {
        if (!bar_ || !popup_.surf) return;
        cairo_surface_t* dummy =
            cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 8, 8);
        cairo_t* cr = cairo_create(dummy);
        measure(cr);
        cairo_destroy(cr);
        cairo_surface_destroy(dummy);
        popup_.ensure(*bar_, place_.anchor, place_.mt, place_.mr, place_.mb,
                      place_.ml, "mattbar-more", pop_w_, pop_h_, out_);
    }

    void paint(cairo_t* cr) {
        cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
        cairo_set_source_rgba(cr, 0, 0, 0, 0);
        cairo_paint(cr);
        cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
        cairo_set_source_rgba(cr, cfg.c_bg.r, cfg.c_bg.g, cfg.c_bg.b,
                              std::max(cfg.c_bg.a, 0.96));
        double r = 10;
        cairo_new_sub_path(cr);
        cairo_arc(cr, pop_w_ - r - 0.5, r + 0.5, r, -M_PI_2, 0);
        cairo_arc(cr, pop_w_ - r - 0.5, pop_h_ - r - 0.5, r, 0, M_PI_2);
        cairo_arc(cr, r + 0.5, pop_h_ - r - 0.5, r, M_PI_2, M_PI);
        cairo_arc(cr, r + 0.5, r + 0.5, r, M_PI, 1.5 * M_PI);
        cairo_close_path(cr);
        cairo_fill_preserve(cr);
        cairo_set_source_rgba(cr, cfg.c_ws_bg.r, cfg.c_ws_bg.g, cfg.c_ws_bg.b, 1);
        cairo_set_line_width(cr, 1);
        cairo_stroke(cr);

        measure(cr);
        if (slots_.empty()) {
            setfont(cr);
            cairo_set_source_rgba(cr, cfg.c_dim.r, cfg.c_dim.g, cfg.c_dim.b, 1);
            cairo_font_extents_t fe;
            cairo_font_extents(cr, &fe);
            const char* msg = "Assign modules to More";
            cairo_text_extents_t e;
            cairo_text_extents(cr, msg, &e);
            cairo_move_to(cr, (pop_w_ - e.x_advance) / 2.0,
                          pop_h_ / 2.0 + (fe.ascent - fe.descent) / 2.0);
            cairo_show_text(cr, msg);
            return;
        }
        for (auto& s : slots_) {
            setfont(cr);
            // Horizontal bar: along = x, thickness = row height.
            // Vertical bar: along = y, thickness = popup/bar width.
            if (cfg_vertical()) s.m->draw(cr, s.y, s.w);
            else s.m->draw(cr, s.x, s.h);
        }
    }

    Slot* hit(double x, double y) {
        for (auto& s : slots_)
            if (x >= s.x && x < s.x + s.w && y >= s.y && y < s.y + s.h)
                return &s;
        return nullptr;
    }

    void open() {
        if (!bar_ || !has_items()) return;
        cairo_surface_t* dummy =
            cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 8, 8);
        cairo_t* cr = cairo_create(dummy);
        measure(cr);
        cairo_destroy(cr);
        cairo_surface_destroy(dummy);

        double along = bar_->slot_along(this);
        if (along < 0) along = last_a_;
        place_ = popup_place(along, pop_w_, bar_->along_length());
        out_   = bar_->current_output();
        popup_.kb_mode = 2;
        popup_.paint   = [this](cairo_t* c) { paint(c); };
        popup_.click   = [this](double x, double y, int b) {
            disarm_close();
            if (Slot* s = hit(x, y)) {
                double rel = cfg_vertical() ? y - s->y : x - s->x;
                if (s->m->on_click(rel, b)) {
                    if (bar_) bar_->request_draw();
                    if (popup_.surf) popup_.draw();
                }
            }
        };
        popup_.pscroll = [this](int d) {
            disarm_close();
            if (Slot* s = hit(popup_.mx, popup_.my)) {
                double rel = cfg_vertical() ? popup_.my - s->y : popup_.mx - s->x;
                if (s->m->on_scroll(rel, d)) {
                    if (bar_) bar_->request_draw();
                    if (popup_.surf) popup_.draw();
                }
            }
        };
        popup_.pmotion = [this](double x, double y) {
            disarm_close();
            if (Slot* s = hit(x, y))
                s->m->on_hover(cfg_vertical() ? y - s->y : x - s->x);
        };
        popup_.pleave = [this] { arm_close(); };
        popup_.pkey   = [this](const Bar::KeyEvent& e) {
            if (e.escape()) close_now();
        };
        hold(true);
        popup_.ensure(*bar_, place_.anchor, place_.mt, place_.mr, place_.mb,
                      place_.ml, "mattbar-more", pop_w_, pop_h_, out_);
        popup_.draw();
        if (bar_) bar_->request_draw();
    }

    Bar*     bar_      = nullptr;
    PopupWin popup_;
    PopupPlace place_{};
    wl_output* out_ = nullptr;
    std::vector<Slot> slots_;
    int close_fd_ = -1;
    bool holding_ = false;
    double last_a_ = 0;
    int pop_w_ = 220, pop_h_ = 48;
};
MoreModule* MoreModule::g = nullptr;
} // namespace
Module* make_more() { return new MoreModule; }
void more_close() {
    if (MoreModule::g) MoreModule::g->close_now();
}
bool more_is_open() {
    return MoreModule::g && MoreModule::g->is_open();
}


// ---------------------------------------------------------------------------
// Custom button (Waybar "custom/*" equivalent): a glyph that runs commands on
// click. With a check command, the glyph only shows while the command prints
// output (re-run every interval, and on SIGRTMIN+<n> like Waybar's "signal").
// Powers the Omarchy menu button and update indicator.
// ---------------------------------------------------------------------------
namespace {
class CustomModule;
CustomModule* g_update = nullptr;

class CustomModule : public TextModule {
public:
    CustomModule(const bool* flag, std::string glyph, std::string font,
                 std::string click, std::string rclick, std::string check,
                 int interval_s, int rtsig)
        : flag_(flag), glyph_(std::move(glyph)), font_(std::move(font)),
          click_(std::move(click)), rclick_(std::move(rclick)),
          check_(std::move(check)), interval_s_(interval_s), rtsig_(rtsig) {}
    ~CustomModule() {
        if (g_update == this) g_update = nullptr;
    }
    void refresh_check() {
        if (!check_.empty()) run_check();
    }
    void clear_indicator() {
        check_cmd_.cancel();
        if (bar_) set_text(*bar_, "");
    }

    bool enabled() const override { return !flag_ || *flag_; }

    void init(Bar& bar) override {
        bar_ = &bar;
        if (check_.empty()) {
            set_text(*bar_, glyph_);
            return;
        }
        run_check();
        // watch our launched child so the check re-runs on its exit
        {
            sigset_t cs;
            sigemptyset(&cs);
            sigaddset(&cs, SIGCHLD);
            sigprocmask(SIG_BLOCK, &cs, nullptr);
            chld_fd_ = signalfd(-1, &cs, SFD_NONBLOCK | SFD_CLOEXEC);
            if (chld_fd_ >= 0)
                bar.add_fd(chld_fd_, [this](uint32_t) {
                    signalfd_siginfo si;
                    while (read(chld_fd_, &si, sizeof si) > 0) {}
                    if (child_ > 0 &&
                        waitpid(child_, nullptr, WNOHANG) > 0) {
                        child_ = -1;
                        run_check();
                    }
                }, "update-child");
        }
        if (rtsig_ > 0) {
            sigset_t ss;
            sigemptyset(&ss);
            sigaddset(&ss, SIGRTMIN + rtsig_);
            sigprocmask(SIG_BLOCK, &ss, nullptr);
            sig_fd_ = signalfd(-1, &ss, SFD_NONBLOCK | SFD_CLOEXEC);
            if (sig_fd_ >= 0)
                bar.add_fd(sig_fd_, [this](uint32_t) {
                    signalfd_siginfo si;
                    while (read(sig_fd_, &si, sizeof si) > 0) {}
                    run_check();
                }, "update-signal");
        }
        // Omarchy 3.x announced finished updates by signalling the bar
        // (SIGRTMIN+7, handled above); Quattro dropped that convention and
        // talks only to its own shell. Watch pacman's log instead: any
        // transaction — bar-launched, keybind, terminal, AUR helper, either
        // Omarchy generation — touches it, and one debounced re-check later
        // the icon tells the truth. Directory watch, so log rotation
        // cannot orphan it. Zero cost between transactions.
        ino_fd_ = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        if (ino_fd_ >= 0 &&
            inotify_add_watch(ino_fd_, "/var/log",
                              IN_MODIFY | IN_CREATE | IN_MOVED_TO) >= 0) {
            deb_fd_ =
                timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
            bar.add_fd(ino_fd_, [this](uint32_t) {
                char buf[4096];
                ssize_t n;
                bool hit = false;
                while ((n = read(ino_fd_, buf, sizeof buf)) > 0) {
                    for (char* p = buf; p < buf + n;) {
                        auto* e = reinterpret_cast<inotify_event*>(p);
                        if (e->len && !strcmp(e->name, "pacman.log"))
                            hit = true;
                        p += sizeof(inotify_event) + e->len;
                    }
                }
                if (hit && deb_fd_ >= 0) { // (re)arm: 60 s after last write
                    itimerspec ts{};
                    ts.it_value.tv_sec = 60;
                    timerfd_settime(deb_fd_, 0, &ts, nullptr);
                }
            }, "update-pacman-log");
            if (deb_fd_ >= 0)
                bar.add_fd(deb_fd_, [this](uint32_t) {
                    uint64_t x;
                    while (read(deb_fd_, &x, sizeof x) > 0) {}
                    run_check();
                }, "update-pacman-debounce");
        } else if (ino_fd_ >= 0) {
            close(ino_fd_);
            ino_fd_ = -1;
        }
    }

    void tick() override {
        if (check_.empty()) return;
        // After the user launches the action (e.g. the updater), re-check
        // every 10 s for a while so the icon clears promptly when the
        // condition ends — Omarchy's own refresh signal targets waybar by
        // name, so MattBar can't rely on it.
        long iv = now_s() < fast_until_ ? 10 : interval_s_;
        if (now_s() - last_check_ >= iv) run_check();
    }

    // override to render with the module's own font (e.g. the omarchy logo
    // font); the bar re-selects the default font before every module.
    double width(cairo_t* cr) override {
        if (!font_.empty())
            cairo_select_font_face(cr, font_.c_str(),
                                   CAIRO_FONT_SLANT_NORMAL,
                                   CAIRO_FONT_WEIGHT_NORMAL);
        return TextModule::width(cr);
    }
    void draw(cairo_t* cr, double x, double h) override {
        if (!font_.empty())
            cairo_select_font_face(cr, font_.c_str(),
                                   CAIRO_FONT_SLANT_NORMAL,
                                   CAIRO_FONT_WEIGHT_NORMAL);
        TextModule::draw(cr, x, h);
    }

    bool on_click(double, int button) override {
        if (button == BTN_LEFT) {
            if (check_.empty()) {
                spawn(live_click(click_));
            } else {
                launch_tracked(); // re-check the moment it exits
                fast_until_ = now_s() + 1800; // plus a 30 min fast window
            }
        } else if (button == BTN_RIGHT) {
            spawn(rclick_);
        }
        return false;
    }

private:
    static long now_s() {
        // BOOTTIME, not MONOTONIC: monotonic freezes during suspend, so on
        // a machine that sleeps a lot "every 6 h" silently became "every
        // 6 h of awake time" — the update icon could lag days behind
        // Waybar's. Boottime counts sleep, so the first tick after any
        // resume that crossed the interval fires the check. (Ticks only
        // run while a bar is revealed — the zero-wakeup idle property is
        // untouched; a hidden bar still schedules nothing.)
        timespec ts;
        clock_gettime(CLOCK_BOOTTIME, &ts);
        return ts.tv_sec;
    }
    // The Omarchy-logo click is stock `omarchy-menu` (Quickshell). Only
    // when the user has asked MattBar to shut Quickshell down do we open
    // our own menu instead. Resolved at click time so a live settings
    // toggle does not require reconstructing the module.
    static std::string live_click(const std::string& stored) {
        if (auto* sh = mattbar_shell(); sh && sh->is_open("omarchy.menu")) {
            sh->hide("omarchy.menu");
            return {};
        }
        if (stored == "omarchy-menu" ||
            stored == "mattbarctl shell toggle omarchy.menu")
            return cfg.quickshell_shutdown
                       ? "mattbarctl shell toggle omarchy.menu"
                       : "omarchy-menu";
        return stored;
    }
    static void spawn(const std::string& c) { spawn_detached(c); }
    void run_check() {
        last_check_ = now_s();
        // Waybar custom-module semantics, which Omarchy's scripts rely on:
        // the module is VISIBLE iff the check exits 0. Output text is
        // informational only — omarchy-update-available prints a message in
        // BOTH states ("update available" / "is up to date") and signals
        // via exit code alone.
        // Async with a 10 s budget: this used to be a synchronous popen
        // that froze the ENTIRE BAR for up to 10 seconds whenever the
        // check script was slow (network hiccup at exactly check time).
        check_cmd_.run(*bar_, check_ + " 2>/dev/null",
                       [this](const std::string& out, int st) {
                           bool visible = (st == 0);
                           if (getenv("MATTBAR_DEBUG"))
                               fprintf(stderr,
                                       "mattbar: update check -> exit=%d "
                                       "(%s) \"%.60s\"\n",
                                       st, visible ? "icon shown"
                                                   : "icon hidden",
                                       trim(out).c_str());
                           set_text(*bar_, visible ? glyph_ : "");
                       },
                       10000);
    }

    void launch_tracked() {
        if (click_.empty() || child_ > 0) return; // one at a time
        pid_t pid = fork();
        if (pid == 0) {
            execl("/bin/sh", "sh", "-c", click_.c_str(), nullptr);
            _exit(127);
        }
        if (pid > 0) child_ = pid;
    }

    Bar* bar_ = nullptr;
    const bool* flag_;
    AsyncCmd    check_cmd_;
    std::string glyph_, font_, click_, rclick_, check_;
    int interval_s_, rtsig_;
    int sig_fd_ = -1;
    int chld_fd_ = -1;
    int ino_fd_ = -1; // pacman.log watch (see init)
    int deb_fd_ = -1; // its debounce timer
    pid_t child_ = -1;
    long fast_until_ = 0;
    long last_check_ = 0;
};
} // namespace

Module* make_omarchy_button() {
    return new CustomModule(&cfg.show_omarchy, cfg.omarchy_glyph,
                            cfg.omarchy_font, cfg.omarchy_click,
                            cfg.omarchy_right_click, "", 0, 0);
}

Module* make_update_button() {
    auto* m = new CustomModule(&cfg.show_update, cfg.update_glyph, "",
                               cfg.update_click, "", cfg.update_check,
                               cfg.update_interval_s, cfg.update_signal);
    g_update = m;
    return m;
}

void update_refresh() {
    if (g_update) g_update->refresh_check();
}
void update_clear() {
    if (g_update) g_update->clear_indicator();
}


// ---------------------------------------------------------------------------
// Temperature: any hwmon sensor, defaulting to the CPU package sensor.
// Click or scroll the module to cycle through discovered sensors; the pick
// is also selectable in settings and persisted as cfg.temp_sensor.
// Pure sysfs reads: no process spawns.
// ---------------------------------------------------------------------------
namespace {
class TempModule : public TextModule {
public:
    bool enabled() const override { return cfg.show_temp; }

    void init(Bar& bar) override {
        bar_ = &bar;
        refresh();
    }

    void tick() override { refresh(); }

    // The settings window can change cfg.temp_sensor between ticks; width()
    // runs on every bar draw, so re-resolve there for instant feedback.
    double width(cairo_t* cr) override {
        if (resolved_for_ != cfg.temp_sensor) refresh();
        return TextModule::width(cr);
    }

    bool on_click(double, int button) override {
        if (button != BTN_LEFT) return false;
        temp_cycle_sensor(1);
        refresh();
        return true;
    }
    bool on_scroll(double, int dir) override {
        temp_cycle_sensor(dir);
        refresh();
        return true;
    }

private:
    void resolve() {
        resolved_for_ = cfg.temp_sensor;
        path_.clear();
        auto list = temp_available_sensors();
        const TempSensorInfo* pick = nullptr;
        if (cfg.temp_sensor == "auto") {
            pick = temp_pick_auto(list);
            display_ = "CPU";
            if (pick && pick->display != "CPU") display_ = pick->display;
        } else {
            for (auto& s : list)
                if (s.id == cfg.temp_sensor) { pick = &s; break; }
            if (!pick) pick = temp_pick_auto(list); // stale id: fall back
            if (pick) display_ = pick->display;
        }
        if (pick) path_ = pick->path;
    }

    void refresh() {
        if (resolved_for_ != cfg.temp_sensor || path_.empty()) resolve();
        if (path_.empty()) { // no sensors at all: module hides itself
            set_text(*bar_, "");
            return;
        }
        std::string v = trim(slurp(path_));
        if (v.empty()) { // sensor vanished (e.g. USB device unplugged)
            resolve();
            v = path_.empty() ? "" : trim(slurp(path_));
            if (v.empty()) { set_text(*bar_, ""); return; }
        }
        int c = atoi(v.c_str()) / 1000;
        std::string label = cfg_vertical() ? "" : display_ + " ";
        set_text(*bar_, label + std::to_string(c) + "\u00b0C",
                 c >= cfg.temp_warn ? cfg.c_urgent : cfg.c_fg);
    }

    Bar* bar_ = nullptr;
    std::string resolved_for_, display_, path_;
};
} // namespace
Module* make_temp() { return new TempModule; }

#include "modules.hpp"
#include "bar.hpp"
#include "config.hpp"
#include "idle-inhibit-unstable-v1-client-protocol.h"
#include "notify.hpp"
#include "sdpump.hpp"
#include "audio.hpp"
#include "popup.hpp"
#include "sensors.hpp"
#include "util.hpp"

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

// Launch a command fully detached; never blocks the bar.
void spawn_detached(const std::string& c) {
    if (c.empty()) return;
    (void)!system((c + " >/dev/null 2>&1 &").c_str());
}

// --------------------------------------------------------------------------- AsyncCmd --------------------------------...
void AsyncCmd::run(Bar& bar, const std::string& cmd, Done cb,
                   int timeout_ms) {
    bar_        = &bar;
    cb_         = std::move(cb);
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
    fcntl(fd_, F_SETFL, O_NONBLOCK);
    bar_->add_fd(fd_, [this](uint32_t ev) {
        char    b[1024];
        ssize_t n;
        while ((n = read(fd_, b, sizeof b)) > 0) buf_.append(b, n);
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

void AsyncCmd::finish(int status) {
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

// --------------------------------------------------------------------------- helpers ---------------------------------...
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

// --------------------------------------------------------------------------- Clock -----------------------------------...
namespace {
class ClockModule : public TextModule {
public:
    bool enabled() const override { return cfg.show_clock; }
    void init(Bar& bar) override { bar_ = &bar; tick(); }
    // Left-click drops a month calendar below the clock (on the monitor whose clock you clicked); click again to put it away.
    bool on_click(double, int button) override {
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


// Hyprland IPC helpers defined further down; declared inside the unnamed namespace so these names unify with their inte...
namespace {
std::string hypr_socket_dir();
int         unix_connect(const std::string& path);
std::string hypr_request(const std::string& dir, const std::string& req);
bool        hypr_dispatch2(const std::string& dir, const std::string& legacy,
                           const std::string& lua);
} // namespace

// --------------------------------------------------------------------------- AI agent usage — a reader of Omarchy Quattro's agents data contract.
class AgentsModule : public TextModule {
public:
    bool enabled() const override { return cfg.show_agents; }

    ~AgentsModule() override {
        if (ino_fd_ >= 0) close(ino_fd_);
        if (deb_fd_ >= 0) close(deb_fd_);
        if (sock2_fd_ >= 0) close(sock2_fd_);
        if (spawn_fd_ >= 0) close(spawn_fd_);
        if (init_fd_ >= 0) close(init_fd_);
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
        // Terminal-session popup plumbing: a socket2 stream tells us when our terminal appears (openwindow), dies (closewindow)...
        hdir_ = hypr_socket_dir();
        DBG("agents: popup term='%s' class='%s' hypr-sockets=%s",
            cfg.agents_term.c_str(), cfg.agents_term_class.c_str(),
            hdir_.empty() ? "MISSING" : "ok");
        if (!hdir_.empty()) {
            sock2_fd_ = unix_connect(hdir_ + "/.socket2.sock");
            if (sock2_fd_ >= 0) {
                // unix_connect() returns a BLOCKING socket (fine for the one-shot request path).
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
                        // stream in level-triggered epoll spins forever; tear down and degrade the popup to panel-only.
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
        // A spawn that never produces a window we recognise must not latch into a silent click.
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
        // The compositor chatter below (rules, fallthrough probe, session discovery) is up to ~13 sequential socket requests with second-scale timeouts.
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
                bar.ping_watchdog();
            }, "agents-deferred-init");
        }
        bar.set_click_observer([this](Module* m) {
            // The popup lives on a special workspace: while it is up, outside clicks land on the desktop layer (no fallthrough) and never change focus, so the focus-based dismissal can't see them.
            if (m != static_cast<Module*>(this) && shown_ && on_special_) {
                DBG("agents: bar interaction elsewhere; hiding the popup");
                show(false);
            }
        });
        rescan();
    }

    void tick() override {
        // usage dir may not exist yet (fresh install, first agent run): cheap re-attempt while revealed, nothing scheduled whil...
        if (ino_fd_ >= 0 && watch_ < 0) {
            try_watch();
            if (watch_ >= 0) rescan();
        }
    }

    bool on_click(double, int button) override {
        if (button == BTN_RIGHT) {
            // Eject: promote the popup session to a normal window on the current workspace and release it from bar management.
            if (term_open_) { eject(); return true; }
            DBG("agents: right-click ignored (eject needs a live popup "
                "session)");
            return false;
        }
        if (button != BTN_LEFT) return false;
        // Native panel when the shell can MEANINGFULLY represent the default agent; terminal-session popup when it cannot.
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
    // Same coverage check the bluetooth/brightness glyphs use: ask the scaled font for real glyph indices and fall back down the chain on any .notdef.
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
    // The panel toggle used to be fire-and-forget — a dead or missing shell made it a perfectly silent click (Quattro shells that died, e.g.
    void open_panel(bool can_fallback) {
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
    // A session that exits within 2 s of mapping is invisible to the user.
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
    // Place the popup so it visually hangs off the bar instead of floating mid-screen: flush to the bar's edge, aligned toward the right module cluster where the agents glyph lives.
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
    std::string term_sel() const {
        return "address:" + (term_addr_.rfind("0x", 0) == 0
                                 ? term_addr_
                                 : "0x" + term_addr_);
    }
    // Focus the popup right after revealing it.
    void hint_eject() {
        if (hinted_) return;
        hinted_ = true;
        spawn_detached(
            "notify-send MattBar 'Tip: RIGHT-CLICK the robot icon in the "
            "bar to pop this agent session out into a normal window.'");
    }
    void focus_popup() {
        if (term_addr_.empty()) return;
        if (!hypr_dispatch2(hdir_, "dispatch focuswindow " + term_sel(),
                            "dispatch hl.dsp.focus({ workspace = "
                            "\"special:mbagent\" })"))
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
    // MEASURE what the compositor actually did instead of trusting "ok" replies — this build's whole history is dispatches that report success and change nothing.
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
    // Best-effort compositor-side rules for our class, installed once per run: if they take, every popup window gets its geometry at map time and the per-window dispatches become idempotent no-ops.
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
        // hl.config({...}) replied ok in the field; try it first, then the other spellings.
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
    // A window mapped before our rule existed keeps its stale geometry forever (rules apply at map).
    bool stale_geometry() {
        std::string c = client_chunk();
        if (c.empty()) return false;
        return json_field(c, "floating").rfind("true", 0) != 0 ||
               c.find("\"size\": [800, 600]") != std::string::npos;
    }
    void preinstall_rules() {
        const std::string cls = cfg.agents_term_class;
        // The socket evaluates `dispatch <arg>` as `return hl.dispatch(<arg>)` (its own error text revealed the wrapper), so <arg> can be any Lua expression: call the DOCUMENTED hl.window_rule() config API for its side effect, then hand hl.dispatch a harmless dispatcher so the call as a whole succeeds.
        const std::string lua =
            "dispatch (function() hl.window_rule({ enabled = true, "
            "match = { class = \"" + cls + "\" }, float = true, size = "
            "\"" + popup_size() + "\", move = \"" + popup_move() +
            "\" }) return hl.dsp.exec_cmd(\"true\") end)()";
        std::string r1 = hypr_request(hdir_, lua);
        std::string r2 = hypr_request(
            hdir_, "keyword windowrulev2 float,class:^(" + cls + ")$");
        std::string r3 = hypr_request(
            hdir_, "keyword windowrulev2 size " + popup_size() +
                       ",class:^(" + cls + ")$");
        std::string r4 = hypr_request(
            hdir_, "keyword windowrulev2 move " + popup_move() +
                       ",class:^(" + cls + ")$");
        DBG("agents: rule preinstall: lua-rule reply='%.120s'", r1.c_str());
        DBG("agents: rule preinstall: legacy keyword replies: "
            "float='%.60s' size='%.60s' move='%.60s'",
            r2.c_str(), r3.c_str(), r4.c_str());
    }
    // Give the freshly adopted (still hidden) popup its real geometry.
    void place_popup() {
        const std::string           sel = term_sel();
        const struct { const char* what; std::string arg; } steps[] = {
            {"float", "setfloating " + sel},
            {"size", "resizewindowpixel exact " + popup_size() + "," + sel},
            {"move", "movewindowpixel exact " + popup_move() + "," + sel},
        };
        for (const auto& st : steps)
            if (!hypr_dispatch2(hdir_, "dispatch " + st.arg,
                                "dispatch \"" + st.arg + "\""))
                DBG("agents: popup %s dispatch rejected", st.what);
        if (!verify_place())
            DBG("agents: popup is STILL NOT FLOATING after geometry "
                "dispatches — this Hyprland accepts them without acting; "
                "the preinstalled window rules are the remaining hope");
    }
    // Right-click: hand the live session over to the user as a normal window on their current workspace.
    void eject() {
        // Target = the underlying normal workspace, read from j/monitors (activeWorkspace stays the normal one even while a spe...
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
        if (ws.empty()) {
            DBG("agents: eject: could not resolve the target workspace; "
                "keeping the popup");
            return;
        }
        // The only documented Lua move acts on the ACTIVE window, so the move is gated on VERIFIED popup focus — it can never r...
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
    // Find a session window that already exists on our special workspace — e.g.
    bool discover_session() {
        if (hdir_.empty()) return false;
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
                if (addr.empty()) {
                    addr     = j.substr(a, e - a);
                    size_t c = chunk.find("\"class\": \"");
                    if (c != std::string::npos) {
                        c += 10;
                        size_t ce = chunk.find('"', c);
                        if (ce != std::string::npos)
                            cls = chunk.substr(c, ce - c);
                    }
                } else {
                    ++extras;
                }
            }
            pos = e;
        }
        if (addr.empty()) return false;
        if (addr.rfind("0x", 0) == 0) addr = addr.substr(2);
        term_addr_  = addr;
        term_cls_   = cls;
        term_open_  = true;
        on_special_ = true;
        adopt_ms_   = now_ms();
        // An adopted leftover says nothing about our spawn command: its exit must not feed the fast-exit failure streak.
        spawned_by_us_ = false;
        // Is the special currently revealed? (Bookkeeping must match reality or the first toggle goes the wrong way.)
        std::string mons = hypr_request(hdir_, "j/monitors");
        shown_ = mons.find("\"name\": \"special:mbagent\"") !=
                 std::string::npos;
        if (extras)
            DBG("agents: %d additional leftover session(s) on the "
                "special; each will be adopted as the current one exits",
                extras);
        return true;
    }
    // Terminal-session popup: toggle a live session, or spawn one onto the hidden special workspace and let sock2 adoption reveal it.
    bool spawn_popup(bool panel_on_fail) {
        if (hdir_.empty() || sock2_fd_ < 0) {
            DBG("agents: popup unavailable (no hypr sockets)");
            return false;
        }
        if (now_ms() < fail_until_) {
            // Two sessions in a row died within seconds of opening: the COMMAND is broken, not the popup — respawning would just flash invisibly forever.
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
        if (term_open_) { // session lives: toggle the dropdown
            if (!on_special_) {
                // Adopted, but stranded on a normal workspace (exec rules were lost and the first repair failed): each click re-attempt...
                repair(term_addr_);
                return true;
            }
            if (!shown_ && !spawned_by_us_ && !migrated_ &&
                stale_geometry()) {
                // A pre-rule leftover can't be resized in place on this build: reveal it, confirm focus, close it, and respawn a rule-g...
                migrated_ = true;
                show(true);
                focus_popup();
                if (verify_focus()) {
                    DBG("agents: pre-rule session has stale geometry; "
                        "migrating (close + rule-governed respawn)");
                    migrate_respawn_ = true;
                    hypr_dispatch2(hdir_,
                                   "dispatch closewindow " + term_sel(),
                                   "dispatch hl.dsp.window.close()");
                    return true;
                }
                DBG("agents: migration skipped (focus unconfirmed)");
                return true;
            }
            if (!shown_) place_popup(); // idempotent; also fixes
            show(!shown_);              // init-discovered leftovers
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
        if (!hypr_dispatch2(
                hdir_,
                "dispatch exec [float; size " + sz + "; workspace "
                "special:mbagent silent] " + cmd,
                "dispatch hl.dsp.exec_cmd(\"" + lua_str(cmd) +
                    "\", { float = true, size = \"" + sz +
                    "\", workspace = \"special:mbagent silent\" })")) {
            // Both dialects rejected the exec (or Hyprland timed out). Never latch a dead state behind a silent click.
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
    // Escape a string for embedding in a double-quoted Lua literal (the command travels inside hl.dsp.exec_cmd("...")): a u...
    static std::string lua_str(const std::string& s) {
        std::string o;
        o.reserve(s.size());
        for (char ch : s) {
            if (ch == '\\' || ch == '"') o += '\\';
            o += ch;
        }
        return o;
    }
    // The popup size is spliced into dispatch strings for BOTH dialects, so it is validated down to digits/%/space; anythin...
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
    // Exec rules got lost (Hyprland 0.55 Lua exec_cmd matches rules by the spawned PID; terminals that hand off to a running instance defeat it): the session window is real but sitting on a normal workspace.
    void repair(const std::string& addr) {
        std::string sel = "address:" +
                          (addr.rfind("0x", 0) == 0 ? addr : "0x" + addr);
        if (hypr_dispatch2(
                hdir_,
                "dispatch movetoworkspacesilent special:mbagent," + sel,
                "dispatch \"movetoworkspacesilent special:mbagent," + sel +
                    "\"")) {
            on_special_ = true;
            shown_      = false; // it just vanished onto the special
            place_popup();
            show(true);          // ...and drops back down managed
            focus_popup();
        } else {
            // Can't re-home it. The window is at least VISIBLE where it is — degrade to that honestly instead of hiding a live sess...
            on_special_ = false;
            shown_      = true;
            DBG("agents: repair move failed; session left on the current "
                "workspace");
        }
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
        if (want == shown_) return;
        DBG("agents: %s the popup (togglespecialworkspace mbagent)",
            want ? "revealing" : "hiding");
        hypr_dispatch2(hdir_, "dispatch togglespecialworkspace mbagent",
                       "dispatch hl.dsp.workspace.toggle_special("
                       "\"mbagent\")");
        shown_ = want;
        if (want) {
            reveal_ms_     = now_ms();
            popup_focused_ = false;
            foreign_seen_  = false;
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
                // A foreign window appearing while the dropdown is up means the user is acting elsewhere (their outside clicks reach th...
                const std::string& ours = term_cls_.empty()
                                              ? cfg.agents_term_class
                                              : term_cls_;
                if (shown_ && on_special_ && cls != ours &&
                    ws != "special:mbagent") {
                    DBG("agents: foreign window '%s' opened; hiding the "
                        "popup", cls.c_str());
                    show(false);
                }
                return;
            }
            bool ours_ws  = ws == "special:mbagent";
            bool ours_cls = cls == cfg.agents_term_class;
            DBG("agents: openwindow addr=%s ws='%s' class='%s' -> %s",
                rest.substr(0, a).c_str(), ws.c_str(), cls.c_str(),
                ours_ws   ? "ADOPT (our workspace)"
                : ours_cls ? "ADOPT (our class, wrong workspace)"
                           : "not ours");
            if (!ours_ws && !ours_cls) return;
            term_addr_ = rest.substr(0, a);
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
            if (term_open_ && l.substr(13) == term_addr_) {
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
                if (migrate_respawn_) {
                    migrate_respawn_ = false;
                    DBG("agents: spawning the rule-governed replacement");
                    spawn_popup(false);
                }
            }
        } else if (l.rfind("activewindow>>", 0) == 0) {
            // CLASS,TITLE — focus-follows-mouse makes this the mouse-out signal.
            std::string cls = l.substr(14, l.find(',', 14) - 14);
            const std::string& ours =
                term_cls_.empty() ? cfg.agents_term_class : term_cls_;
            if (!shown_ || !on_special_ || cls.empty()) return;
            const uint64_t since = now_ms() - reveal_ms_;
            if (cls == ours) {
                // Hyprland auto-focuses the special's window the moment it is re-revealed — that is NOT the user entering the popup and must not arm dismissal (field bug: instant re-reveals dismissed during bar->popup travel).
                if (!popup_focused_ && (foreign_seen_ || since > 800)) {
                    popup_focused_ = true;
                    DBG("agents: popup entered "
                        "(mouse-out dismissal armed)");
                } else if (!popup_focused_) {
                    DBG("agents: auto-focus at reveal (not arming)");
                }
                return;
            }
            if (!popup_focused_ && since <= 1500) {
                // Focus-follows-mouse firing while the pointer crosses windows on its way to the popup: travel, not mouse-out.
                foreign_seen_ = true;
                DBG("agents: ignoring focus '%s' (travel toward the "
                    "popup)", cls.c_str());
                return;
            }
            DBG("agents: mouse-out (focus moved to '%s'); hiding",
                cls.c_str());
            show(false);
        }
    }

    void try_watch() {
        watch_ = inotify_add_watch(ino_fd_, dir_.c_str(),
                                   IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE |
                                       IN_DELETE);
    }

    // Targeted field extraction (house style; records are machine-written, flat, sort_keys=true).
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
    int         fail_streak_ = 0;
    bool        hold_next_ = false, popup_focused_ = false;
    bool        foreign_seen_ = false;
    bool        spawned_by_us_ = false;
    bool        migrated_ = false, migrate_respawn_ = false;
    bool        hinted_ = false;
    bool        term_open_ = false, shown_ = false, spawning_ = false;
    bool        on_special_ = false; // window actually lives on the special
};


// --------------------------------------------------------------------------- Microphone: default-source state beside the volume module.
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
            spawn_detached(cfg.mic_click);
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

// --------------------------------------------------------------------------- Screen-recording indicator: a red light while a recorder process runs, invisible otherwise.
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
            set_text(*bar_, rec_ ? cfg.screenrecord_glyph : "", cfg.c_urgent);
            DBG("screenrecord: %s", rec_ ? "recording" : "idle");
        }
    }
    bool on_click(double, int button) override {
        if (button == BTN_LEFT && rec_) {
            spawn_detached(cfg.screenrecord_stop);
            return true;
        }
        return false;
    }

private:
    bool recorder_running() const {
        // comma-separated command prefixes, e.g.
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

// --------------------------------------------------------------------------- Hyprland workspaces ---------------------...
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
    // Bounded I/O, always. At session start (or during a config reload) Hyprland can sit on its command socket for a long t...
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

// Hyprland >= 0.55 with a Lua config (what Omarchy Quattro converts every install to) evaluates a socket1 `dispatch X` as Lua: `hl.dispatch(X)`.
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

// monitor name -> its active workspace id, from j/monitors. "name" precedes "activeWorkspace" in each monitor object.
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

    // With multi-monitor on, each bar shows the workspaces that live on ITS monitor; the bar being drawn tells us which one that is.
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
        auto act = extract_ids(hypr_request(dir_, "j/activeworkspace"));
        int active = act.empty() ? -1 : act.front();
        // Only ask for the per-monitor picture when it can matter; on a single-bar setup this is one IPC round-trip saved per r...
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
            // Hyprland's socket2 fires for every focus/window change too; only workspace-affecting events warrant a re-query.
            static const char* keys[] = {"workspace>>",     "workspacev2>>",
                                         "createworkspace", "destroyworkspace",
                                         "moveworkspace",   "renameworkspace",
                                         "focusedmon"};
            buf[n < static_cast<ssize_t>(sizeof buf) ? n : sizeof buf - 1] =
                '\0';
            for (const char* k : keys)
                if (strstr(buf, k)) { relevant = true; break; }
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

// --------------------------------------------------------------------------- Battery ---------------------------------...
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
            // energy_*/power_now (µWh/µW) on most laptops; charge_*/ current_now (µAh/µA) on the rest — the ratio is hours either way.
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
private:
    Bar* bar_ = nullptr;
    std::string path_;
    bool warned15_ = false, warned5_ = false;
};
} // namespace
Module* make_battery() { return new BatteryModule; }

// --------------------------------------------------------------------------- Network (default route interface + SSID i...
namespace {
// -------------------------------------------------------------------------- nl80211: fetch the SSID of an associated wireless interface without spawning iw.
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
        // Event-driven: an rtnetlink socket delivers link and route changes — interface up/down, Wi-Fi (re)association, default route moves — so `iw` runs only when the network actually changed, not every 5 seconds of visibility.
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
            // Debounce: a Wi-Fi association is a burst of link + route messages; one query 300 ms after the burst settles.
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
        spawn_detached(cfg.network_click); // e.g. omarchy-launch-wifi
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
        // In-process nl80211 query — the last steady-state external binary (iw) is gone.
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

// --------------------------------------------------------------------------- Volume (PipeWire via wpctl, fallback pactl).
namespace {
class VolumeModule : public TextModule {
public:
    bool enabled() const override { return cfg.show_volume; }
    void init(Bar& bar) override {
        bar_ = &bar;
        // Event-driven: the shared pactl-subscribe stream tells us when the sink actually changed; that is the only time the mixer is asked.
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
            spawn_detached(cfg.volume_click);
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
    // ---- async volume/mute writes ---------------------------------------- The old code ran system() per input event: bounded by `timeout 0.4`, but during an audio-stack storm (the exact scenario v1.23.1 exists for) every scroll notch could block the event loop up to 400ms.
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
    // One compound shell invocation gathers everything the module needs — sink properties (headset detection), volume+mute (wpctl, pactl fallback), and the wired-jack active port — separated by markers, parsed when the ASYNC result arrives.
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
            // Detection cadence: every refresh in event mode (the refresh IS a device change); every 5th in poll-fallback mode.
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
        // Rune fallback runs at the first paint (needs cairo): configured glyph, then the MD headphones rune, then "HP".
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

// --------------------------------------------------------------------------- Bluetooth (BlueZ over the system bus).
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
        // Recovery timer, armed only while something is wrong: a bar started before bluetoothd is up (login races), a D-Bus restart, or a transient GetManagedObjects failure heals itself with bounded exponential backoff.
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
        // transport watcher plumbing: grace timer, reconnect delay timer, and the shared pactl event stream (sink appear/vanish...
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

    // No periodic work; tick() only recomposes after live orientation flips (TextModule::width calls it when the bar turns ...
    void tick() override { refresh(); }

    // The configured glyph is Nerd Font PUA; whether it renders depends on the font cfg.font actually resolves to (Omarchy 3.8 dropped the Cascadia package MattBar's default family comes from, so fontconfig may substitute a glyph-less font).
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
            // narrow bars: render the whole (compact) text in the glyph- capable family rather than juggling segments in ellipsis code
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
            // Rung 2: any Nerd-glyph-capable family installed on the system, used for the rune only (the omarchy module already sets the precedent of a module-local font face).
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
    // widths of the glyph segment (its own family) and the remainder (bar font); leaves cr on the bar font.
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
            spawn_detached(cfg.bluetooth_click);
            return false;
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
        // Sender-scoped: the daemon forwards only BlueZ property traffic, so (as with the tray) unrelated bus chatter never wak...
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

    // ---- message walking -------------------------------------------------- a{sv} at the current position, applied to `pa...
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
                // transport watcher: a connect opens the grace window, a disconnect re-evaluates immediately (clears the wedge)
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
        // Glyph always leads (module identity next to the network SSID); everything after it is settings-controlled.
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

    // ---- transport watcher ------------------------------------------------ The wedge the MattBook taught us: Device1.Connected=true while PipeWire has no bluez sink — every layer reports healthy and there is no audio.
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
        // Disconnect settled (either way): reconnect after the same 2s the manual procedure uses, so the device finishes its ow...
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
Module* make_agents() { return new AgentsModule; }
Module* make_microphone() { return new MicrophoneModule; }
Module* make_screenrecord() { return new ScreenRecordModule; }

// --------------------------------------------------------------------------- Screen brightness: /sys/class/backlight for state (updated by kernel uevents, zero polling), systemd-logind Session.SetBrightness for writes (unprivileged for the active session, no helper tools; falls back to a direct sysfs write, then to brightnessctl).
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

// "auto" (the new default) draws the vector sun.
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
        // The uevent watch is bound even when no device exists yet, so a backlight driver that loads after the bar (or comes and goes) shows/hides the module live instead of requiring a restart.
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
        // Same placement helper as the calendar/bell/power panels: centered on the module, clamped to stay fully on screen, on ...
        PopupPlace pl = popup_place(last_a_, SW, bar_->along_length());
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

// --------------------------------------------------------------------------- Media (MPRIS over the session bus), on the same event-driven SdPump: an initial ListNames sweep, then NameOwnerChanged (arg0namespace-filtered daemon-side) tracks players appearing/vanishing and PropertiesChanged on the fixed MPRIS object path tracks title/artist/status.
namespace {
static uint64_t mono_ms() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000ULL;
}

class MediaModule : public TextModule {
public:
    ~MediaModule() override {
        if (retry_fd_ >= 0) close(retry_fd_);
    }
    bool enabled() const override { return cfg.show_media; }

    void init(Bar& bar) override {
        bar_              = &bar;
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
        // One fixed object path for every MPRIS player: daemon-side filter keeps all non-media property traffic away from this ...
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
        // Same run-time glyph verification as the bluetooth module: prefer the Nerd Font play/pause runes, degrade to ASCII on ...
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
        const Player* p = active();
        if (!p || !bus_) return false;
        const char* method = button == BTN_LEFT    ? "PlayPause"
                             : button == BTN_RIGHT ? "Next"
                                                   : nullptr;
        if (!method) return false;
        sd_bus_call_method_async(bus_, nullptr, p->name.c_str(),
                                 "/org/mpris/MediaPlayer2",
                                 "org.mpris.MediaPlayer2.Player", method,
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

private:
    struct Player {
        std::string name;   // well-known org.mpris.MediaPlayer2.*
        std::string status; // Playing / Paused / Stopped
        std::string title, artist;
        uint64_t    seq = 0; // recency of last change
    };

    const Player* active() const {
        const Player* best = nullptr;
        for (auto& [u, p] : players_) { // playing beats paused, recent beats old
            if (!best || (p.status == "Playing") > (best->status == "Playing") ||
                ((p.status == "Playing") == (best->status == "Playing") &&
                 p.seq > best->seq))
                best = &p;
        }
        return best;
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
            // A live MPRIS player we never discovered: NameOwnerChanged can be missed (startup races; broker match quirks on driver signals).
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

    void refresh() {
        if (!bar_) return;
        const Player* p = active();
        // Only genuinely transporting players count; players with sparse metadata (web radio, mpv, players mid-load) must still...
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
    }

    static MediaModule*                g_media; // for detached async replies
    int                                retry_fd_ = -1;
    bool                               sweep_pending_ = false;
    bool                               need_sweep_    = false;
    uint64_t                           last_sweep_ms_ = 0;
    int                                attempts_ = 0;
    Bar*                               bar_ = nullptr;
    SdPump                             pump_;
    sd_bus*                            bus_ = nullptr;
    std::map<std::string, Player>      players_; // unique name -> state
    std::vector<std::string>           pending_;
    uint64_t                           seq_ = 0;
    std::string play_ = ">", pause_ = "||", checked_font_;

public:
    MediaModule() { g_media = this; }
};
MediaModule* MediaModule::g_media = nullptr;
} // namespace
Module* make_media() { return new MediaModule; }

// --------------------------------------------------------------------------- Stay-awake toggle: holds a zwp_idle_inhibitor_v1 on the bar's own surface while active, so the compositor suspends idle actions (lock, dpms).
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

// --------------------------------------------------------------------------- Pin: click to keep the bar revealed (auto-hide paused); click again to resume.
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


// --------------------------------------------------------------------------- Custom button (Waybar "custom/*" equivalent): a glyph that runs commands on click.
namespace {
class CustomModule : public TextModule {
public:
    CustomModule(const bool* flag, std::string glyph, std::string font,
                 std::string click, std::string rclick, std::string check,
                 int interval_s, int rtsig)
        : flag_(flag), glyph_(std::move(glyph)), font_(std::move(font)),
          click_(std::move(click)), rclick_(std::move(rclick)),
          check_(std::move(check)), interval_s_(interval_s), rtsig_(rtsig) {}

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
        // Omarchy 3.x announced finished updates by signalling the bar (SIGRTMIN+7, handled above); Quattro dropped that convention and talks only to its own shell.
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
        // After the user launches the action (e.g.
        long iv = now_s() < fast_until_ ? 10 : interval_s_;
        if (now_s() - last_check_ >= iv) run_check();
    }

    // override to render with the module's own font (e.g.
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
                spawn(click_);
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
        // BOOTTIME, not MONOTONIC: monotonic freezes during suspend, so on a machine that sleeps a lot "every 6 h" silently became "every 6 h of awake time" — the update icon could lag days behind Waybar's.
        timespec ts;
        clock_gettime(CLOCK_BOOTTIME, &ts);
        return ts.tv_sec;
    }
    static void spawn(const std::string& c) {
        if (!c.empty())
            (void)!system((c + " >/dev/null 2>&1 &").c_str());
    }
    void run_check() {
        last_check_ = now_s();
        // Waybar custom-module semantics, which Omarchy's scripts rely on: the module is VISIBLE iff the check exits 0.
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
    return new CustomModule(&cfg.show_update, cfg.update_glyph, "",
                            cfg.update_click, "", cfg.update_check,
                            cfg.update_interval_s, cfg.update_signal);
}


// --------------------------------------------------------------------------- Temperature: any hwmon sensor, defaulting to the CPU package sensor.
namespace {
class TempModule : public TextModule {
public:
    bool enabled() const override { return cfg.show_temp; }

    void init(Bar& bar) override {
        bar_ = &bar;
        refresh();
    }

    void tick() override { refresh(); }

    // The settings window can change cfg.temp_sensor between ticks; width() runs on every bar draw, so re-resolve there for...
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

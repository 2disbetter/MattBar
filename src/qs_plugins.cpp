#include "qs_plugins.hpp"
#include "config.hpp"
#include "modules.hpp"
#include "notify.hpp"
#include "overlay.hpp"
#include "popup.hpp"
#include "ui.hpp"
#include "util.hpp"

#include <cairo/cairo.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
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

namespace {

std::vector<QsPlugin> g_catalog;
pid_t                 g_pid       = -1;
Bar*                  g_bar       = nullptr;
bool                  g_json_ours = false;
uint64_t              g_restart_ms = 0;
int                   g_fail_streak = 0;

const char* kSkipIds[] = {"mattbar.null-bar", nullptr};
const char* kDisableFirstParty[] = {
    "omarchy.notifications", "omarchy.lock",     "omarchy.polkit",
    "omarchy.idle",          "omarchy.battery",  "omarchy.osd",
    "omarchy.nightlight",    "omarchy.media",    "omarchy.clipboard",
    "omarchy.emojis",        "omarchy.image-picker", "omarchy.reminders",
    "omarchy.menu",          "omarchy.background", nullptr};

std::string home_dir() {
    const char* h = getenv("HOME");
    return h && *h ? h : ".";
}
std::string plugins_dir() { return home_dir() + "/.config/omarchy/plugins"; }
std::string shell_json_path() {
    return home_dir() + "/.config/omarchy/shell.json";
}
std::string bak_path() {
    return home_dir() + "/.config/omarchy/shell.json.mattbar-sidecar-bak";
}
std::string marker_path() {
    const char* st = getenv("XDG_STATE_HOME");
    std::string d  = st && *st ? st : home_dir() + "/.local/state";
    return d + "/omarchy/mattbar-sidecar-on";
}
std::string omarchy_shell_path() {
    const char* p = getenv("OMARCHY_PATH");
    return std::string(p && *p ? p : "/usr/share/omarchy") + "/shell";
}

bool skip_id(const std::string& id) {
    for (int i = 0; kSkipIds[i]; ++i)
        if (id == kSkipIds[i]) return true;
    if (id.rfind("omarchy.", 0) == 0) return true;
    return false;
}

std::string json_str(const std::string& j, const char* key) {
    std::string pat = std::string("\"") + key + "\"";
    size_t      p   = j.find(pat);
    if (p == std::string::npos) return {};
    p = j.find(':', p + pat.size());
    if (p == std::string::npos) return {};
    p = j.find_first_not_of(" \t\n\r", p + 1);
    if (p == std::string::npos || j[p] != '"') return {};
    ++p;
    std::string o;
    for (; p < j.size() && j[p] != '"'; ++p) {
        if (j[p] == '\\' && p + 1 < j.size()) {
            o += j[++p];
            continue;
        }
        o += j[p];
    }
    return o;
}

std::vector<std::string> json_str_array(const std::string& j, const char* key) {
    std::vector<std::string> out;
    std::string              pat = std::string("\"") + key + "\"";
    size_t                   p   = j.find(pat);
    if (p == std::string::npos) return out;
    p = j.find('[', p + pat.size());
    if (p == std::string::npos) return out;
    size_t e = j.find(']', p);
    if (e == std::string::npos) return out;
    std::string block = j.substr(p, e - p);
    size_t      q     = 0;
    while ((q = block.find('"', q)) != std::string::npos) {
        ++q;
        size_t r = block.find('"', q);
        if (r == std::string::npos) break;
        out.push_back(block.substr(q, r - q));
        q = r + 1;
    }
    return out;
}

bool has_kind(const std::vector<std::string>& k, const char* s) {
    return std::find(k.begin(), k.end(), s) != k.end();
}

bool write_file(const std::string& path, const std::string& body) {
    std::ofstream f(path.c_str(), std::ios::trunc);
    if (!f) return false;
    f << body;
    return (bool)f;
}

bool mkdir_parents(const std::string& file) {
    auto slash = file.rfind('/');
    if (slash == std::string::npos) return true;
    std::string dir = file.substr(0, slash);
    std::string cmd = "mkdir -p '" + dir + "'";
    return system(cmd.c_str()) == 0;
}

uint64_t now_ms() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000ULL;
}

} // namespace

std::string qs_module_id(const std::string& plugin_id) {
    return "qs:" + plugin_id;
}
bool qs_is_module_id(const std::string& module_id) {
    return module_id.rfind("qs:", 0) == 0;
}
std::string qs_plugin_id_of(const std::string& module_id) {
    return qs_is_module_id(module_id) ? module_id.substr(3) : module_id;
}

void qs_plugins_scan() {
    g_catalog.clear();
    std::string dir = plugins_dir();
    DIR*        d   = opendir(dir.c_str());
    if (!d) return;
    while (dirent* e = readdir(d)) {
        if (e->d_name[0] == '.') continue;
        std::string man = dir + "/" + e->d_name + "/manifest.json";
        std::string j   = slurp(man);
        if (j.empty()) continue;
        QsPlugin p;
        p.id          = json_str(j, "id");
        p.name        = json_str(j, "name");
        p.description = json_str(j, "description");
        p.kinds       = json_str_array(j, "kinds");
        if (p.id.empty() || skip_id(p.id)) continue;
        if (p.name.empty()) p.name = p.id;
        bool is_bar = has_kind(p.kinds, "bar");
        p.placeable = has_kind(p.kinds, "bar-widget") ||
                      has_kind(p.kinds, "overlay") ||
                      has_kind(p.kinds, "panel") || has_kind(p.kinds, "menu");
        p.service_only = has_kind(p.kinds, "service") && !p.placeable && !is_bar;
        if (is_bar && !p.placeable && !p.service_only) continue;
        if (!p.placeable && !p.service_only) continue;
        g_catalog.push_back(std::move(p));
    }
    closedir(d);
    std::sort(g_catalog.begin(), g_catalog.end(),
              [](const QsPlugin& a, const QsPlugin& b) { return a.id < b.id; });
}

const std::vector<QsPlugin>& qs_plugins_catalog() {
    if (g_catalog.empty()) qs_plugins_scan();
    return g_catalog;
}
const QsPlugin* qs_plugin_find(const std::string& plugin_id) {
    for (auto& p : qs_plugins_catalog())
        if (p.id == plugin_id) return &p;
    return nullptr;
}
bool qs_plugin_known(const std::string& plugin_id) {
    return qs_plugin_find(plugin_id) != nullptr;
}

bool qs_plugin_service_on(const std::string& plugin_id) {
    auto v = cfg.csv_split(cfg.qs_plugin_services);
    return std::find(v.begin(), v.end(), plugin_id) != v.end();
}
void qs_plugin_set_service(const std::string& plugin_id, bool on) {
    auto v   = cfg.csv_split(cfg.qs_plugin_services);
    auto it  = std::find(v.begin(), v.end(), plugin_id);
    if (on && it == v.end()) v.push_back(plugin_id);
    if (!on && it != v.end()) v.erase(it);
    cfg.qs_plugin_services = cfg.csv_join(v);
}

std::vector<std::string> qs_plugin_layout_ids() {
    auto v = cfg.csv_split(cfg.qs_plugin_layout);
    std::vector<std::string> out;
    for (auto& id : v) {
        const QsPlugin* p = qs_plugin_find(id);
        if (p && p->placeable) out.push_back(id);
    }
    return out;
}

bool qs_plugin_shown(const std::string& plugin_id) {
    auto v = qs_plugin_layout_ids();
    return std::find(v.begin(), v.end(), plugin_id) != v.end();
}

void qs_plugin_set_shown(const std::string& plugin_id, bool on) {
    auto v  = cfg.csv_split(cfg.qs_plugin_layout);
    auto it = std::find(v.begin(), v.end(), plugin_id);
    if (on && it == v.end()) v.push_back(plugin_id);
    if (!on && it != v.end()) v.erase(it);
    cfg.qs_plugin_layout = cfg.csv_join(v);
}

void qs_plugin_move(const std::string& plugin_id, int delta) {
    auto v  = cfg.csv_split(cfg.qs_plugin_layout);
    auto it = std::find(v.begin(), v.end(), plugin_id);
    if (it == v.end()) return;
    int i  = (int)(it - v.begin());
    int to = i + delta;
    if (to < 0 || to >= (int)v.size()) return;
    std::swap(v[i], v[to]);
    cfg.qs_plugin_layout = cfg.csv_join(v);
}

bool qs_plugins_want_runtime() {
    if (!cfg.quickshell_shutdown || !cfg.qs_plugins) return false;
    qs_plugins_scan();
    for (auto& p : g_catalog) {
        if (p.placeable && qs_plugin_shown(p.id)) return true;
        if (p.service_only && qs_plugin_service_on(p.id)) return true;
    }
    return false;
}
bool qs_plugins_running() { return g_pid > 0; }

void qs_plugins_sync_modules(Bar& bar) {
    g_bar = &bar;
    qs_plugins_scan();
}

static bool null_bar_installed() {
    return access((plugins_dir() + "/mattbar.null-bar/manifest.json").c_str(),
                  R_OK) == 0;
}

static std::string sidecar_json() {
    std::string o = "{\n  \"version\": 1,\n  \"bar\": {\n"
                    "    \"id\": \"mattbar.null-bar\",\n"
                    "    \"position\": \"top\",\n"
                    "    \"transparent\": true,\n"
                    "    \"layout\": { \"left\": [], \"center\": [], "
                    "\"right\": [] }\n  },\n  \"plugins\": [";
    bool first = true;
    auto add   = [&](const std::string& id) {
        if (!first) o += ",";
        first = false;
        o += "\n    { \"id\": \"" + id + "\" }";
    };
    for (auto& p : g_catalog) {
        if (p.placeable && qs_plugin_shown(p.id)) add(p.id);
        else if (p.service_only && qs_plugin_service_on(p.id)) add(p.id);
    }
    o += "\n  ],\n  \"disabledPlugins\": [";
    first = true;
    for (int i = 0; kDisableFirstParty[i]; ++i) {
        if (!first) o += ", ";
        first = false;
        o += std::string("\"") + kDisableFirstParty[i] + "\"";
    }
    o += "]\n}\n";
    return o;
}

static void restore_user_json() {
    if (!g_json_ours && access(marker_path().c_str(), F_OK) != 0) return;
    std::string bak = slurp(bak_path());
    if (!bak.empty()) write_file(shell_json_path(), bak);
    unlink(bak_path().c_str());
    unlink(marker_path().c_str());
    g_json_ours = false;
    fprintf(stderr, "mattbar: qs-plugins: restored user shell.json\n");
}

static bool install_sidecar_json() {
    if (!null_bar_installed()) {
        fprintf(stderr,
                "mattbar: qs-plugins: mattbar.null-bar is not installed "
                "under ~/.config/omarchy/plugins; cannot start sidecar\n");
        return false;
    }
    if (!g_json_ours && access(marker_path().c_str(), F_OK) != 0) {
        std::string cur = slurp(shell_json_path());
        if (cur.empty()) {
            fprintf(stderr, "mattbar: qs-plugins: no user shell.json\n");
            return false;
        }
        mkdir_parents(bak_path());
        mkdir_parents(marker_path());
        if (!write_file(bak_path(), cur)) return false;
        write_file(marker_path(), "1\n");
    } else if (access(bak_path().c_str(), R_OK) != 0) {
        fprintf(stderr,
                "mattbar: qs-plugins: sidecar marker set but backup "
                "missing; refusing to overwrite shell.json\n");
        return false;
    }
    if (!write_file(shell_json_path(), sidecar_json())) return false;
    g_json_ours = true;
    return true;
}

static void kill_pid() {
    if (g_pid <= 0) return;
    kill(g_pid, SIGTERM);
    for (int i = 0; i < 20; ++i) {
        int   st  = 0;
        pid_t w = waitpid(g_pid, &st, WNOHANG);
        if (w == g_pid || (w < 0 && errno == ECHILD)) {
            g_pid = -1;
            return;
        }
        usleep(50000);
    }
    kill(g_pid, SIGKILL);
    waitpid(g_pid, nullptr, 0);
    g_pid = -1;
}

static void kill_any_qs() {
    kill_pid();
    std::string cmd =
        with_preserved_power_profile("quickshell kill -p '" +
                                     omarchy_shell_path() +
                                     "' --any-display >/dev/null 2>&1 || true");
    int st = 0;
    cmd_output(cmd, &st);
}

static bool spawn_sidecar() {
    if (g_pid > 0) return true;
    const char* qs = "/usr/bin/quickshell";
    if (access(qs, X_OK) != 0) qs = "quickshell";
    pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {
        prctl(PR_SET_PDEATHSIG, SIGTERM);
        if (getppid() == 1) _exit(1);
        const char* om = getenv("OMARCHY_PATH");
        if (!om || !*om) setenv("OMARCHY_PATH", "/usr/share/omarchy", 1);
        execl(qs, "quickshell", "-n", "-p", omarchy_shell_path().c_str(),
              (char*)nullptr);
        execlp("quickshell", "quickshell", "-n", "-p",
               omarchy_shell_path().c_str(), (char*)nullptr);
        _exit(127);
    }
    g_pid = pid;
    fprintf(stderr, "mattbar: qs-plugins: sidecar started pid=%d\n", (int)pid);
    return true;
}

void qs_plugins_reap() {
    if (g_pid <= 0) return;
    int   st = 0;
    pid_t w  = waitpid(g_pid, &st, WNOHANG);
    if (w == 0) return;
    fprintf(stderr, "mattbar: qs-plugins: sidecar pid %d exited\n", (int)g_pid);
    g_pid = -1;
    if (!qs_plugins_want_runtime()) return;
    uint64_t now = now_ms();
    if (now < g_restart_ms) return;
    if (++g_fail_streak > 4) {
        fprintf(stderr,
                "mattbar: qs-plugins: sidecar died repeatedly; leaving it "
                "down until the next settings apply\n");
        return;
    }
    g_restart_ms = now + 1500;
    if (install_sidecar_json()) spawn_sidecar();
}

void qs_plugins_stop() {
    kill_any_qs();
    restore_user_json();
    g_fail_streak = 0;
}

void qs_plugins_shutdown() {
    plugins_close();
    if (cfg.qs_plugins) {
        cfg.qs_plugins = false;
        cfg.save();
    }
    if (g_pid > 0 || g_json_ours ||
        access(marker_path().c_str(), F_OK) == 0) {
        fprintf(stderr, "mattbar: qs-plugins: user stopped sidecar\n");
        qs_plugins_stop();
    }
    if (g_bar) {
        g_bar->refresh_settings();
        g_bar->request_draw();
    }
}

void qs_plugins_apply(Bar& bar) {
    g_bar = &bar;
    qs_plugins_sync_modules(bar);
    // Crash leftover: a previous run swapped shell.json and died.
    if (access(marker_path().c_str(), F_OK) == 0 && !g_json_ours &&
        g_pid <= 0 && !qs_plugins_want_runtime())
        restore_user_json();
    if (!qs_plugins_want_runtime()) {
        if (g_pid > 0 || g_json_ours) {
            fprintf(stderr, "mattbar: qs-plugins: stopping sidecar\n");
            qs_plugins_stop();
        }
        return;
    }
    if (!install_sidecar_json()) return;
    if (g_pid > 0) {
        // Already ours; rewrite of shell.json is picked up if qs watches
        // the file. Omarchy disables the file watcher, so restart.
        kill_pid();
    } else {
        kill_any_qs();
    }
    g_fail_streak = 0;
    spawn_sidecar();
}

static std::string qs_ipc_cmd(const std::string& target,
                              const std::string& method,
                              const std::string& arg) {
    std::string cmd = "timeout --kill-after=1s 2s /usr/bin/qs ipc -n -p " +
                      ov::shell_quote(omarchy_shell_path()) +
                      " call -- shell " + method + " " + target;
    if (!arg.empty()) {
        std::string a = arg;
        for (char& c : a)
            if (c == '\'') c = '"';
        cmd += " " + ov::shell_quote(a);
    }
    return cmd;
}

std::string qs_plugins_ipc(const std::string& target, const std::string& method,
                           const std::string& arg) {
    if (g_pid <= 0) return {};
    int         st  = 0;
    std::string out = trim(cmd_output(qs_ipc_cmd(target, method, arg), &st));
    DBG("qs-plugins ipc %s %s -> '%s' status=%d", method.c_str(),
        target.c_str(), out.c_str(), st);
    return out;
}

void qs_plugins_activate(const std::string& plugin_id, const std::string& method,
                         const std::string& arg) {
    if (!cfg.quickshell_shutdown) {
        notify_post("Plugins",
                    "Turn on takeover (shut down Quickshell) to run plugins",
                    1);
        return;
    }
    const QsPlugin* p = qs_plugin_find(plugin_id);
    if (!p) {
        notify_post("Plugins", "Unknown plugin " + plugin_id, 1);
        return;
    }
    bool dirty = false;
    if (p->placeable && !qs_plugin_shown(plugin_id)) {
        qs_plugin_set_shown(plugin_id, true);
        dirty = true;
    }
    if (!cfg.qs_plugins) {
        cfg.qs_plugins = true;
        dirty          = true;
    }
    if (dirty) {
        cfg.save();
        if (g_bar) g_bar->refresh_settings();
    }
    // Don't restart a live sidecar just to summon — that drops in-progress
    // overlay state (Neon Cadet's current ball, etc.).
    if (g_pid <= 0 && g_bar) qs_plugins_apply(*g_bar);

    std::string name = p->name.empty() ? plugin_id : p->name;
    std::string verb = method.empty() ? "summon" : method;
    std::string payload = arg.empty() ? "{}" : arg;
    // Short per-try timeout: the 2s sync helper would stall ~40s if QS
    // never comes up. Sidecar is usually answering within 1–3s.
    std::string ipc =
        "timeout --kill-after=0.2s 0.5s /usr/bin/qs ipc -n -p " +
        ov::shell_quote(omarchy_shell_path()) + " call -- shell " + verb + " " +
        plugin_id + " " + ov::shell_quote(payload);
    std::string cmd =
        "ok=0; "
        "for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do "
        "  if " +
        ipc +
        " >/dev/null 2>&1; then ok=1; break; fi; "
        "  sleep 0.35; "
        "done; "
        "if [ \"$ok\" -ne 1 ]; then notify-send -u normal -a MattBar "
        "Plugins " +
        ov::shell_quote("Could not open " + name) + "; fi";
    spawn_detached(cmd);
    fprintf(stderr, "mattbar: qs-plugins: activate %s %s\n", verb.c_str(),
            plugin_id.c_str());
}

namespace {

class PluginsModule : public Module {
public:
    static PluginsModule* g;
    PluginsModule() { g = this; }
    ~PluginsModule() override {
        if (g == this) g = nullptr;
        if (close_fd_ >= 0) close(close_fd_);
    }
    bool enabled() const override {
        return cfg.quickshell_shutdown && cfg.show_plugins;
    }
    void init(Bar& bar) override {
        bar_      = &bar;
        close_fd_ = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
        if (close_fd_ >= 0)
            bar.add_fd(close_fd_, [this](uint32_t) {
                uint64_t n;
                while (read(close_fd_, &n, sizeof n) > 0) {}
                close_now();
            }, "plugins-close");
    }
    double width(cairo_t*) override { return 22; }
    void   draw(cairo_t* cr, double a, double t) override {
        last_a_    = a;
        const bool on = popup_.surf != nullptr;
        const Color& c = on ? cfg.c_accent : cfg.c_fg;
        cairo_set_source_rgba(cr, c.r, c.g, c.b, 1.0);
        cairo_select_font_face(cr, cfg.font.c_str(), CAIRO_FONT_SLANT_NORMAL,
                               CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, cfg.font_size);
        resolve_glyph(cr);
        cairo_text_extents_t e;
        cairo_text_extents(cr, glyph_.c_str(), &e);
        cairo_font_extents_t fe;
        cairo_font_extents(cr, &fe);
        double cx = cfg_vertical() ? t / 2.0 : a + 11;
        double cy = cfg_vertical() ? a + 11 : t / 2.0;
        cairo_move_to(cr, cx - e.x_advance / 2.0,
                      cy + (fe.ascent - fe.descent) / 2.0);
        cairo_show_text(cr, glyph_.c_str());
    }
    bool on_click(double, int button) override {
        if (!bar_) return false;
        if (button == BTN_RIGHT) {
            qs_plugins_shutdown();
            return true;
        }
        if (button != BTN_LEFT) return false;
        if (popup_.surf) close_now();
        else open();
        return true;
    }
    void tick() override {
        if (popup_.surf) {
            relayout();
            popup_.draw();
        }
    }
    bool is_open() const { return popup_.surf != nullptr; }
    void close_now() {
        disarm();
        hold(false);
        popup_.destroy();
        rows_.clear();
        if (bar_) bar_->request_draw();
    }

private:
    struct Row {
        std::string id, label;
        double      x = 0, y = 0, w = 0, h = 0;
    };
    void hold(bool on) {
        if (!bar_ || on == holding_) return;
        holding_ = on;
        bar_->hold_open(on);
    }
    void arm(int ms = 1400) {
        if (close_fd_ < 0) return;
        itimerspec ts{};
        ts.it_value.tv_sec  = ms / 1000;
        ts.it_value.tv_nsec = (ms % 1000) * 1000000L;
        timerfd_settime(close_fd_, 0, &ts, nullptr);
    }
    void disarm() {
        if (close_fd_ < 0) return;
        itimerspec off{};
        timerfd_settime(close_fd_, 0, &off, nullptr);
    }
    double row_h() const { return std::max(26.0, (double)cfg_thickness()); }
    void   setfont(cairo_t* cr) {
        cairo_select_font_face(cr, cfg.font.c_str(), CAIRO_FONT_SLANT_NORMAL,
                               CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, cfg.font_size);
    }
    bool show_stop() const {
        return cfg.qs_plugins || qs_plugins_running();
    }
    void measure(cairo_t* cr) {
        rows_.clear();
        const double pad = 10, gap = 4;
        const double rh  = row_h();
        setfont(cr);
        double maxlab = 120;
        auto   ids    = qs_plugin_layout_ids();
        const char* stop_lab = "Stop Quickshell";
        auto widen = [&](const std::string& s) {
            cairo_text_extents_t e;
            cairo_text_extents(cr, s.c_str(), &e);
            maxlab = std::max(maxlab, e.x_advance);
        };
        for (auto& id : ids) {
            const QsPlugin* p = qs_plugin_find(id);
            widen(p ? p->name : id);
        }
        if (show_stop()) widen(stop_lab);
        double y = pad;
        double w = maxlab + pad * 2 + 8;
        if (ids.empty() && !show_stop()) {
            pop_w_ = 260;
            pop_h_ = 56;
            return;
        }
        for (auto& id : ids) {
            const QsPlugin* p = qs_plugin_find(id);
            std::string     lab = p ? p->name : id;
            rows_.push_back({id, lab, pad, y, w - pad * 2, rh});
            y += rh + gap;
        }
        if (show_stop()) {
            if (!ids.empty()) y += 4;
            rows_.push_back({"", stop_lab, pad, y, w - pad * 2, rh});
            y += rh + gap;
        }
        pop_w_ = (int)std::lround(w);
        pop_h_ = (int)std::lround(y + pad - gap);
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
                      place_.ml, "mattbar-plugins", pop_w_, pop_h_, out_);
    }
    Row* hit(double x, double y) {
        for (auto& r : rows_)
            if (x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h)
                return &r;
        return nullptr;
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
        cairo_set_source_rgba(cr, cfg.c_ws_bg.r, cfg.c_ws_bg.g, cfg.c_ws_bg.b,
                              1);
        cairo_set_line_width(cr, 1);
        cairo_stroke(cr);
        setfont(cr);
        cairo_font_extents_t fe;
        cairo_font_extents(cr, &fe);
        measure(cr);
        if (rows_.empty()) {
            cairo_set_source_rgba(cr, cfg.c_dim.r, cfg.c_dim.g, cfg.c_dim.b, 1);
            const char* msg = cfg.qs_plugins
                                  ? "Check plugins under Shell"
                                  : "Enable Quickshell plugins on Shell";
            cairo_text_extents_t e;
            cairo_text_extents(cr, msg, &e);
            cairo_move_to(cr, (pop_w_ - e.x_advance) / 2.0,
                          pop_h_ / 2.0 + (fe.ascent - fe.descent) / 2.0);
            cairo_show_text(cr, msg);
            return;
        }
        for (auto& row : rows_) {
            bool stop = row.id.empty();
            bool hot = popup_.mx >= row.x && popup_.mx < row.x + row.w &&
                       popup_.my >= row.y && popup_.my < row.y + row.h;
            if (hot) {
                cairo_set_source_rgba(cr, cfg.c_ws_bg.r, cfg.c_ws_bg.g,
                                      cfg.c_ws_bg.b, 1);
                cairo_rectangle(cr, row.x, row.y, row.w, row.h);
                cairo_fill(cr);
            }
            if (stop) {
                cairo_set_source_rgba(cr, cfg.c_urgent.r, cfg.c_urgent.g,
                                      cfg.c_urgent.b, 1);
            } else {
                cairo_set_source_rgba(cr, cfg.c_fg.r, cfg.c_fg.g, cfg.c_fg.b,
                                      1);
            }
            cairo_move_to(cr, row.x + 6,
                          row.y + row.h / 2.0 +
                              (fe.ascent - fe.descent) / 2.0);
            cairo_show_text(cr, row.label.c_str());
        }
    }
    void open() {
        if (!bar_) return;
        more_close();
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
            disarm();
            if (b != BTN_LEFT && b != BTN_MIDDLE && b != BTN_RIGHT) return;
            if (Row* row = hit(x, y)) {
                std::string id = row->id;
                close_now();
                if (id.empty()) qs_plugins_shutdown();
                else if (b == BTN_RIGHT) qs_plugins_ipc(id, "hide", "");
                else qs_plugins_activate(id, "summon", "{}");
            }
        };
        popup_.pmotion = [this](double, double) {
            disarm();
            if (popup_.surf) popup_.draw();
        };
        popup_.pleave = [this] { arm(); };
        popup_.pkey   = [this](const Bar::KeyEvent& e) {
            if (e.escape()) close_now();
        };
        hold(true);
        popup_.ensure(*bar_, place_.anchor, place_.mt, place_.mr, place_.mb,
                      place_.ml, "mattbar-plugins", pop_w_, pop_h_, out_);
        popup_.draw();
        if (bar_) bar_->request_draw();
    }

    void resolve_glyph(cairo_t* cr) {
        if (checked_ == cfg.font) return;
        checked_ = cfg.font;
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
        // Puzzle piece, not FA plug (\uf1e6) which reads as a wall-wart.
        std::string pick = "\uf12e";      // nf-fa-puzzle-piece
        if (!mapped(pick)) pick = "\U000F0431"; // nf-md-puzzle-outline
        if (!mapped(pick)) pick = "\U000F040C"; // nf-md-puzzle
        if (!mapped(pick)) pick = "\ueb26";     // nf-cod-extensions
        if (!mapped(pick)) pick = "P";
        glyph_ = pick;
    }

    Bar*       bar_      = nullptr;
    PopupWin   popup_;
    PopupPlace place_{};
    wl_output* out_      = nullptr;
    std::vector<Row> rows_;
    int        close_fd_ = -1;
    bool       holding_  = false;
    double     last_a_   = 0;
    int        pop_w_ = 220, pop_h_ = 48;
    std::string glyph_   = "\uf12e";
    std::string checked_;
};
PluginsModule* PluginsModule::g = nullptr;
} // namespace

Module* make_plugins() { return new PluginsModule; }
void    plugins_close() {
    if (PluginsModule::g) PluginsModule::g->close_now();
}
bool plugins_is_open() {
    return PluginsModule::g && PluginsModule::g->is_open();
}

namespace {

using ov::Host;
using ov::col;
using ov::panel_bg;
using ov::rrect;
using ov::say;
using ov::select_shell_font;
using ov::shell_quote;
using ov::tw;

bool looks_git_url(const std::string& u) {
    if (u.empty() || u.size() > 400) return false;
    for (char c : u)
        if (c == ' ' || c == ';' || c == '|' || c == '&' || c == '$' ||
            c == '`' || c == '\n' || c == '\r' || c == '"' || c == '\'')
            return false;
    return u.rfind("https://", 0) == 0 || u.rfind("http://", 0) == 0 ||
           u.rfind("git@", 0) == 0 || u.rfind("git://", 0) == 0 ||
           u.rfind("ssh://", 0) == 0;
}

void after_plugin_disk_change() {
    qs_plugins_scan();
    auto* sh = mattbar_shell();
    if (!sh || !sh->bar()) return;
    sh->bar()->apply_config();
    sh->bar()->refresh_settings();
}

class PluginAddOverlay : public Overlay {
public:
    const char* id() const override { return "mattbar.plugin-add"; }
    void summon(const std::string&) override { open(); }
    void hide() override { host_.close(); }
    bool is_open() const override { return host_.is_open(); }
    ~PluginAddOverlay() override { host_.close(); }

private:
    Host      host_;
    TextField field_;
    AsyncCmd  cmd_;
    std::string status_;
    bool        busy_ = false;
    int W() const { return 420; }
    int H() const { return 240; }
    double fs() const { return cfg.font_size > 0 ? cfg.font_size : 13.0; }
    void open() {
        field_.clear();
        field_.focused = true;
        status_.clear();
        busy_ = false;
        host_.win.paint = [this](cairo_t* cr) { paint(cr); };
        host_.win.pkey  = [this](const Bar::KeyEvent& e) { on_key(e); };
        host_.win.click = [this](double x, double y, int b) {
            if (b != BTN_LEFT) return;
            // Add
            if (y >= H() - 52 && y < H() - 28 && x >= 18 && x < 90) go();
            // Cancel
            if (y >= H() - 52 && y < H() - 28 && x >= 100 && x < 180)
                host_.dismiss();
        };
        host_.open(W(), H(), "mattbar-plugin-add", id(), true);
    }
    void go() {
        if (busy_) return;
        std::string url = trim(field_.text);
        if (!looks_git_url(url)) {
            status_ = "Need an https:// or git@ URL";
            host_.redraw();
            return;
        }
        auto* sh = mattbar_shell();
        if (!sh || !sh->bar()) return;
        busy_   = true;
        status_ = "Cloning… this can take a few seconds";
        host_.redraw();
        cmd_.run(
            *sh->bar(),
            std::string("/usr/bin/omarchy plugin add --yes ") +
                shell_quote(url),
            [this](const std::string& out, int st) {
                busy_ = false;
                std::string t = trim(out);
                if (st == 0) {
                    status_ = t.empty() ? "Added. Check it below to load it."
                                        : t;
                    after_plugin_disk_change();
                } else {
                    auto nl = t.rfind('\n');
                    status_ = nl == std::string::npos ? t : t.substr(nl + 1);
                    if (status_.empty()) status_ = "Add failed";
                }
                if (host_.is_open()) host_.redraw();
            },
            60000);
    }
    void on_key(const Bar::KeyEvent& e) {
        if (!e.pressed) return;
        if (e.escape()) {
            host_.dismiss();
            return;
        }
        if (e.enter()) {
            go();
            return;
        }
        if (!busy_ && field_.handle(e)) host_.redraw();
    }
    void paint(cairo_t* cr) {
        panel_bg(cr, W(), H());
        select_shell_font(cr, fs());
        say(cr, 18, 24, "Add plugin from git", cfg.c_fg);
        cairo_set_font_size(cr, std::max(10.0, fs() - 1));
        say(cr, 18, 46, "Plugins run unsandboxed in the Quickshell sidecar.",
            cfg.c_accent);
        say(cr, 18, 64, "Only add a repo whose code you are willing to run.",
            cfg.c_dim);
        cairo_set_font_size(cr, fs());
        field_.draw(cr, 16, 84, W() - 32, 28, "https://github.com/org/plugin.git");
        cairo_set_font_size(cr, std::max(10.0, fs() - 1));
        if (!status_.empty())
            say(cr, 18, 132, status_.substr(0, 72),
                busy_ ? cfg.c_dim : cfg.c_fg);
        say(cr, 18, 154, "Lands disabled. Check it in the list to load.",
            cfg.c_dim);
        cairo_set_font_size(cr, fs());
        col(cr, cfg.c_accent, 1);
        rrect(cr, 18, H() - 52, 70, 24, 6);
        cairo_fill(cr);
        say(cr, 36, H() - 40, "Add", contrast_on(cfg.c_accent));
        col(cr, cfg.c_ws_bg, 1);
        rrect(cr, 100, H() - 52, 78, 24, 6);
        cairo_fill(cr);
        say(cr, 112, H() - 40, "Cancel", cfg.c_fg);
        cairo_set_font_size(cr, std::max(10.0, fs() - 1));
        say(cr, 18, H() - 16,
            "Enter to add  \u00b7  Esc to cancel  \u00b7  Ctrl+V paste",
            cfg.c_dim);
    }
};

class PluginRemoveOverlay : public Overlay {
public:
    const char* id() const override { return "mattbar.plugin-remove"; }
    void summon(const std::string&) override { open(); }
    void hide() override { host_.close(); }
    bool is_open() const override { return host_.is_open(); }
    ~PluginRemoveOverlay() override { host_.close(); }

private:
    Host host_;
    AsyncCmd cmd_;
    std::vector<QsPlugin> items_;
    int  sel_   = 0;
    bool conf_  = false;
    bool busy_  = false;
    std::string status_;
    int W() const { return 400; }
    int H() const { return 280; }
    double fs() const { return cfg.font_size > 0 ? cfg.font_size : 13.0; }
    void refresh_items() {
        items_.clear();
        for (auto& p : qs_plugins_catalog())
            if (p.id != "mattbar.null-bar") items_.push_back(p);
        if (sel_ >= (int)items_.size()) sel_ = (int)items_.size() - 1;
        if (sel_ < 0) sel_ = 0;
    }
    void open() {
        qs_plugins_scan();
        refresh_items();
        conf_  = false;
        busy_  = false;
        status_.clear();
        host_.win.paint = [this](cairo_t* cr) { paint(cr); };
        host_.win.pkey  = [this](const Bar::KeyEvent& e) { on_key(e); };
        host_.win.click = [this](double, double y, int b) {
            if (b != BTN_LEFT || busy_ || items_.empty()) return;
            int row = (int)((y - 52) / 28);
            if (row >= 0 && row < (int)items_.size()) {
                sel_  = row;
                conf_ = true;
                host_.redraw();
            }
        };
        host_.open(W(), H(), "mattbar-plugin-remove", id(), true);
    }
    void do_remove() {
        if (busy_ || items_.empty() || sel_ < 0 ||
            sel_ >= (int)items_.size())
            return;
        auto* sh = mattbar_shell();
        if (!sh || !sh->bar()) return;
        std::string id = items_[sel_].id;
        busy_   = true;
        status_ = "Removing " + id;
        host_.redraw();
        qs_plugin_set_shown(id, false);
        qs_plugin_set_service(id, false);
        cmd_.run(
            *sh->bar(),
            std::string("/usr/bin/omarchy plugin remove --yes ") +
                shell_quote(id),
            [this, id](const std::string& out, int st) {
                busy_ = false;
                conf_ = false;
                std::string t = trim(out);
                if (st == 0) {
                    status_ = t.empty() ? ("Removed " + id) : t;
                    after_plugin_disk_change();
                    refresh_items();
                } else {
                    status_ = t.empty() ? "Remove failed" : t;
                }
                if (host_.is_open()) host_.redraw();
            },
            15000);
    }
    void on_key(const Bar::KeyEvent& e) {
        if (!e.pressed || busy_) return;
        if (e.escape()) {
            if (conf_) {
                conf_ = false;
                host_.redraw();
            } else
                host_.dismiss();
            return;
        }
        if (e.up() && sel_ > 0) {
            --sel_;
            conf_ = false;
            host_.redraw();
            return;
        }
        if (e.down() && sel_ + 1 < (int)items_.size()) {
            ++sel_;
            conf_ = false;
            host_.redraw();
            return;
        }
        if (e.enter()) {
            if (items_.empty()) return;
            if (!conf_) {
                conf_ = true;
                host_.redraw();
            } else
                do_remove();
        }
    }
    void paint(cairo_t* cr) {
        panel_bg(cr, W(), H());
        select_shell_font(cr, fs());
        say(cr, 18, 24, "Remove plugin", cfg.c_fg);
        cairo_set_font_size(cr, std::max(10.0, fs() - 1));
        if (items_.empty()) {
            say(cr, 18, 80, "No third-party plugins installed.", cfg.c_dim);
            say(cr, 18, H() - 16, "Esc to close", cfg.c_dim);
            return;
        }
        cairo_set_font_size(cr, fs());
        double y = 56;
        for (int i = 0; i < (int)items_.size(); ++i) {
            if (y > H() - 70) break;
            bool on = i == sel_;
            if (on) {
                col(cr, cfg.c_accent, 0.25);
                rrect(cr, 12, y - 12, W() - 24, 26, 6);
                cairo_fill(cr);
            }
            say(cr, 18, y, items_[i].name, cfg.c_fg);
            y += 28;
        }
        cairo_set_font_size(cr, std::max(10.0, fs() - 1));
        if (conf_ && sel_ >= 0 && sel_ < (int)items_.size())
            say(cr, 18, H() - 36,
                "Remove " + items_[sel_].id + "? Enter again to confirm",
                cfg.c_accent);
        else if (!status_.empty())
            say(cr, 18, H() - 36, status_.substr(0, 58), cfg.c_dim);
        say(cr, 18, H() - 16, "Enter select  \u00b7  Esc cancel", cfg.c_dim);
    }
};

} // namespace

Overlay* make_plugin_add_overlay() { return new PluginAddOverlay; }
Overlay* make_plugin_remove_overlay() { return new PluginRemoveOverlay; }

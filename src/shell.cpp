#include "shell.hpp"
#include "bar.hpp"
#include "config.hpp"
#include "idle.hpp"
#include "lock.hpp"
#include "modules.hpp"
#include "nightlight.hpp"
#include "notify.hpp"
#include "omarchy_theme.hpp"
#include "overlay.hpp"
#include "polkit.hpp"
#include "qs_plugins.hpp"
#include "wallpaper.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace {

Shell* g_shell = nullptr;

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\n' ||
                          s.back() == '\r'))
        s.pop_back();
    size_t i = 0;
    while (i < s.size() && s[i] == ' ') ++i;
    return s.substr(i);
}

// Split "method id payload..." — payload may contain spaces.
void split_rest(const std::string& rest, std::string& method, std::string& id,
                std::string& payload) {
    std::istringstream ss(rest);
    ss >> method >> id;
    std::string tail;
    std::getline(ss, tail);
    payload = trim(tail);
}

class ClockOverlay : public Overlay {
public:
    explicit ClockOverlay(Bar** bar) : bar_(bar) {}
    const char* id() const override { return "omarchy.clock"; }
    void summon(const std::string&) override {
        if (*bar_) calendar_open(**bar_, nullptr);
    }
    void hide() override { calendar_close(); }
    bool is_open() const override { return calendar_is_open(); }

private:
    Bar** bar_;
};

std::string handle_notifications(const std::string& method) {
    auto* nd = notify_daemon();
    if (!nd) return "error: no notification daemon";
    if (method == "dismissOne" || method == "dismiss") {
        nd->dismiss_last();
        return "ok";
    }
    if (method == "dismissAll") {
        nd->dismiss_all();
        return "ok";
    }
    if (method == "invokeLast" || method == "invoke") {
        nd->invoke_last();
        return "ok";
    }
    if (method == "restore") {
        nd->restore_last();
        return "ok";
    }
    if (method == "toggleDnd" || method == "toggle") {
        nd->toggle_dnd();
        return nd->dnd() ? "on" : "off";
    }
    if (method == "dndState" || method == "isDnd" || method == "status")
        return nd->dnd() ? "on" : "off";
    if (method == "showHistory") return "ok"; // history lives on the bell
    if (method == "clear") {
        nd->clear_history();
        return "ok";
    }
    return "unknown";
}

std::string json_string(const std::string& json, const char* key) {
    std::string pat = std::string("\"") + key + "\"";
    auto p = json.find(pat);
    if (p == std::string::npos) return {};
    p = json.find(':', p + pat.size());
    if (p == std::string::npos) return {};
    p = json.find_first_not_of(" \t", p + 1);
    if (p == std::string::npos) return {};
    if (json[p] == '"') {
        ++p;
        auto e = json.find('"', p);
        if (e == std::string::npos) return {};
        return json.substr(p, e - p);
    }
    auto e = json.find_first_of(",} \t", p);
    return json.substr(p, e - p);
}

std::string handle_osd(const std::string& method, const std::string& arg) {
    auto* nd = notify_daemon();
    if (!nd) return "error: no notification daemon";
    if (method == "close") return "ok";
    if (method == "state") return "closed";
    if (method == "ping") return "ok";
    if (method != "show") return "unknown";
    std::string icon = json_string(arg, "icon");
    std::string msg  = json_string(arg, "message");
    std::string val  = json_string(arg, "value");
    double frac = 0;
    if (!val.empty()) {
        frac = atof(val.c_str());
        if (frac > 1.0) frac /= 100.0;
    }
    std::string label = !msg.empty() ? msg : (icon.empty() ? "osd" : icon);
    nd->osd_show(label, frac, false);
    return "ok";
}

std::string handle_media(const std::string& method) {
    const char* cmd = nullptr;
    if (method == "playPause") cmd = "playerctl play-pause";
    else if (method == "next") cmd = "playerctl next";
    else if (method == "previous") cmd = "playerctl previous";
    else if (method == "play") cmd = "playerctl play";
    else if (method == "pause") cmd = "playerctl pause";
    else if (method == "sourceSwitch") {
        media_source_switch();
        return "ok";
    }
    else if (method == "ping") return "ok";
    else if (method == "status") return "ok";
    else return "unhandled";
    spawn_detached(cmd);
    return "ok";
}

} // namespace

void Shell::init(Bar& bar) {
    bar_    = &bar;
    g_shell = this;
    add(make_menu_overlay());
    add(new ClockOverlay(&bar_));
    register_shell_panels(*this);
    register_shell_overlays(*this);
    add(make_timezone_overlay());
    add(make_power_overlay());
    add(make_reminders_overlay());
    add(make_tailscale_overlay());
    add(make_dropbox_overlay());
    add(make_plugin_add_overlay());
    add(make_plugin_remove_overlay());
    apply_takeover();
    // After notifyd's first apply_enabled (main inits daemon before
    // Shell), reconcile sidecar state. apply_config does this too.
    qs_plugins_apply(bar);
}

static std::string self_exe() {
    char buf[512];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n <= 0) return "/usr/local/bin/mattbar";
    buf[n] = 0;
    return buf;
}

static std::string runtime_dir() {
    const char* rt = getenv("XDG_RUNTIME_DIR");
    return std::string(rt && *rt ? rt : "/tmp") + "/mattbar";
}

static std::string shim_dir() { return runtime_dir() + "/bin"; }

static bool mkdir_p(const std::string& path) {
    std::string cur;
    for (size_t i = 0; i <= path.size(); ++i) {
        if (i == path.size() || path[i] == '/') {
            if (cur.empty()) {
                cur = "/";
                continue;
            }
            mkdir(cur.c_str(), 0700);
        }
        if (i < path.size()) cur += path[i];
    }
    return true;
}

static std::string path_without_dir(const std::string& path,
                                    const std::string& dir) {
    std::string out, cur;
    auto flush = [&]() {
        if (!cur.empty() && cur != dir) {
            if (!out.empty()) out += ':';
            out += cur;
        }
        cur.clear();
    };
    for (char c : path) {
        if (c == ':') flush();
        else cur += c;
    }
    flush();
    return out.empty() ? "/usr/bin" : out;
}

static std::string lua_dquote(const std::string& s) {
    std::string o = "\"";
    for (char c : s) {
        if (c == '\\' || c == '"') o += '\\';
        o += c;
    }
    o += '"';
    return o;
}

static bool write_text(const std::string& path, const std::string& body) {
    FILE* f = fopen(path.c_str(), "w");
    if (!f) {
        fprintf(stderr, "mattbar: shell: cannot write %s\n", path.c_str());
        return false;
    }
    fwrite(body.data(), 1, body.size(), f);
    fclose(f);
    return true;
}

// Hyprland 0.55+ Lua: `hyprctl keyword env` is rejected ("use eval"), and
// inline `hyprctl eval 'lua'` dies if the payload contains a single quote
// (the apps/system menu JSON). Write a file and dofile it instead.
static void hypr_dofile(const std::string& path) {
    for (char c : path)
        if (c == '"' || c == '\'' || c == '\n') return;
    std::string cmd = "hyprctl eval 'dofile(\"" + path + "\")'";
    FILE* p = popen((cmd + " 2>&1").c_str(), "r");
    if (!p) {
        spawn_detached(cmd);
        return;
    }
    char        buf[512];
    std::string out;
    while (fgets(buf, sizeof buf, p)) out += buf;
    pclose(p);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r'))
        out.pop_back();
    if (out != "ok" && !out.empty())
        fprintf(stderr, "mattbar: hypr eval: %s\n", out.c_str());
}

// Super+Space and Super+Ctrl+A/B/W/D/… are Lua binds (hl.bind). A PATH
// shim never reaches hl.dsp.exec_cmd; unbind+rebind is required. On
// takeover, reload the installer module so Super+Space hits mattbarctl.
static void rebind_shell_keys(bool ours, const std::string& hypr_path) {
    std::string dir = runtime_dir();
    mkdir_p(dir);
    std::string file = dir + "/hypr-rebind.lua";
    std::string lua;
    if (ours) {
        lua = "package.loaded['hypr.mattbar-shell-keys'] = nil\n"
              "local ok = pcall(require, 'hypr.mattbar-shell-keys')\n"
              "if not ok then\n"
              "  hl.unbind('SUPER + SPACE')\n"
              "  o.bind('SUPER + SPACE', 'Omarchy menu',\n"
              "         'mattbarctl shell toggle omarchy.menu')\n"
              "end\n"
              "hl.env('PATH', " +
              lua_dquote(hypr_path) + ")\n";
    } else {
        lua = "local function b(keys, desc, cmd)\n"
              "  hl.unbind(keys)\n"
              "  o.bind(keys, desc, cmd)\n"
              "end\n"
              "b('SUPER + SPACE', 'Omarchy menu', 'omarchy-menu toggle')\n"
              "b('SUPER + ALT + SPACE', 'Apps menu', 'omarchy-menu toggle apps')\n"
              "b('SUPER + ESCAPE', 'System menu', 'omarchy-menu toggle system')\n"
              "b('SUPER + CTRL + A', 'Audio', "
              "'omarchy-shell shell toggle omarchy.audio')\n"
              "b('SUPER + CTRL + B', 'Bluetooth', "
              "'omarchy-shell shell toggle omarchy.bluetooth')\n"
              "b('SUPER + CTRL + W', 'Network', "
              "'omarchy-shell shell toggle omarchy.network')\n"
              "b('SUPER + CTRL + L', 'Lock system', "
              "'omarchy-system-lock')\n"
              "b('SUPER + CTRL + D', 'Display', "
              "'omarchy-shell shell toggle omarchy.monitor')\n"
              "b('SUPER + CTRL + E', 'Emojis', "
              "'omarchy-shell shell toggle omarchy.emojis')\n"
              "b('SUPER + CTRL + V', 'Clipboard manager', "
              "'omarchy-shell shell toggle omarchy.clipboard')\n"
              "hl.env('PATH', " +
              lua_dquote(hypr_path) + ")\n";
    }
    if (write_text(file, lua)) hypr_dofile(file);
}

void Shell::apply_takeover() {
    std::string dir = shim_dir();
    static std::string orig_path;
    if (orig_path.empty()) {
        const char* p = getenv("PATH");
        orig_path = path_without_dir(p && *p ? p : "/usr/bin", dir);
    }
    std::string exe = self_exe();
    auto link_one = [&](const char* name) {
        std::string dest = dir + "/" + name;
        unlink(dest.c_str());
        if (symlink(exe.c_str(), dest.c_str()) != 0)
            fprintf(stderr, "mattbar: shell: cannot symlink %s -> %s\n",
                    dest.c_str(), exe.c_str());
    };
    auto unlink_one = [&](const char* name) {
        std::string dest = dir + "/" + name;
        char buf[512];
        ssize_t n = readlink(dest.c_str(), buf, sizeof buf - 1);
        if (n > 0) {
            buf[n] = 0;
            if (exe == buf || strstr(buf, "mattbar")) unlink(dest.c_str());
        }
    };
    static bool systemd_shimmed = false;
    auto sync_sleep_lock_path = [&](bool on) {
        if (on == systemd_shimmed) return;
        systemd_shimmed = on;
        // omarchy-sleep-lock.service does not inherit Hyprland PATH, so
        // it was still calling /usr/bin/omarchy-shell (qs ipc) and
        // timing out: "Screen did not lock before suspend".
        spawn_detached("systemctl --user import-environment PATH; "
                       "systemctl --user try-restart omarchy-sleep-lock.service");
    };
    if (cfg.quickshell_shutdown) {
        mkdir_p(dir);
        link_one("omarchy-shell");
        link_one("omarchy-menu");
        std::string np = dir + ":" + orig_path;
        setenv("PATH", np.c_str(), 1);
        sync_sleep_lock_path(true);
        rebind_shell_keys(true, np);
        fprintf(stderr,
                "mattbar: shell: Quickshell takeover on; "
                "omarchy-shell shim at %s\n",
                dir.c_str());
    } else {
        unlink_one("omarchy-shell");
        unlink_one("omarchy-menu");
        setenv("PATH", orig_path.c_str(), 1);
        sync_sleep_lock_path(false);
        hide("omarchy.menu");
        rebind_shell_keys(false, orig_path);
    }
    wallpaper_apply();
    polkit_apply();
    idle_apply();
    lock_reclaim();
}

Shell::~Shell() {
    for (auto& [_, o] : overlays_) delete o;
    overlays_.clear();
    if (g_shell == this) g_shell = nullptr;
}

void Shell::add(Overlay* o) {
    if (!o) return;
    overlays_[o->id()] = o;
}

Overlay* Shell::find(const std::string& id) const {
    auto it = overlays_.find(id);
    return it == overlays_.end() ? nullptr : it->second;
}

std::string Shell::toggle(const std::string& id, const std::string& payload) {
    Overlay* o = find(id);
    if (!o) {
        if (qs_plugin_known(id)) {
            qs_plugins_activate(id, "toggle",
                                payload.empty() ? "{}" : payload);
            return "ok";
        }
        return "unknown";
    }
    if (o->is_open()) o->hide();
    else o->summon(payload);
    return "ok";
}

std::string Shell::summon(const std::string& id, const std::string& payload) {
    Overlay* o = find(id);
    if (!o) {
        if (qs_plugin_known(id)) {
            qs_plugins_activate(id, "summon",
                                payload.empty() ? "{}" : payload);
            return "ok";
        }
        return "unknown";
    }
    o->summon(payload);
    return "ok";
}

std::string Shell::hide(const std::string& id) {
    Overlay* o = find(id);
    if (!o) {
        if (qs_plugins_running() && qs_plugin_known(id)) {
            qs_plugins_ipc(id, "hide", "");
            return "ok";
        }
        return "unknown";
    }
    o->hide();
    return "ok";
}

bool Shell::is_open(const std::string& id) const {
    Overlay* o = find(id);
    return o && o->is_open();
}

std::string Shell::call(const std::string& id, const std::string& method,
                        const std::string& arg) {
    Overlay* o = find(id);
    if (!o) return "unknown";
    return o->call(method, arg);
}

std::string Shell::handle(const std::string& target, const std::string& rest) {
    std::string method, id, payload;
    split_rest(rest, method, id, payload);

    if (target == "shell") {
        if (method == "ping" || method.empty()) return ping();
        if (method == "toggle") return toggle(id, payload);
        if (method == "summon") return summon(id, payload);
        if (method == "hide") {
            hide(id);
            return "ok";
        }
        if (method == "call") {
            std::string m2, a2;
            std::istringstream ps(payload);
            ps >> m2;
            std::getline(ps, a2);
            while (!a2.empty() && a2[0] == ' ') a2.erase(a2.begin());
            return call(id, m2, a2);
        }
        if (method == "listPlugins") {
            std::string out;
            for (auto& [k, _] : overlays_) {
                if (!out.empty()) out += "\n";
                out += k;
            }
            return out.empty() ? "ok" : out;
        }
        if (method == "setPluginEnabled" || method == "rescanPlugins" ||
            method == "reloadConfig" || method == "applyTheme")
            return "ok"; // no QML plugin host
        return "unknown";
    }
    if (target == "notifications") return handle_notifications(method);
    if (target == "osd") return handle_osd(method, trim(id + " " + payload));
    if (target == "media") return handle_media(method);
    if (target == "lock") {
        if (method == "ping") return "ok";
        if (method == "isLocked") return lock_is_locked() ? "true" : "false";
        if (method == "status") return lock_status_json();
        if (method == "lock") return lock_now();
        return "unknown";
    }
    if (target == "background") {
        auto point_link = [](const std::string& path) {
            if (path.empty()) return;
            const char* h = getenv("HOME");
            const char* xdg = getenv("XDG_STATE_HOME");
            std::string link = xdg && *xdg
                                   ? std::string(xdg) + "/omarchy/current/background"
                                   : std::string(h ? h : ".") +
                                         "/.local/state/omarchy/current/background";
            spawn_detached("mkdir -p \"$(dirname " + ov::shell_quote(link) +
                           ")\" && ln -sfn " + ov::shell_quote(path) + " " +
                           ov::shell_quote(link));
        };
        if (method == "refresh" || method.empty()) {
            wallpaper_refresh();
            return "ok";
        }
        if (method == "set" && !id.empty()) {
            point_link(id);
            wallpaper_set(id, false);
            return "ok";
        }
        if (method == "setInstant" && !id.empty()) {
            point_link(id);
            wallpaper_set(id, true);
            return "ok";
        }
        if (method == "transition") {
            std::string to = payload.empty() ? id : payload;
            if (to.empty()) {
                wallpaper_refresh();
                return "ok";
            }
            point_link(to);
            wallpaper_transition(payload.empty() ? std::string() : id, to);
            return "ok";
        }
        if (method == "themeTransition") {
            // fromPath path [finalPath colorsB64 shellB64]
            std::string from = id, to, final_path;
            std::istringstream ps(payload);
            ps >> to >> final_path;
            if (to.empty()) to = from;
            if (!final_path.empty()) point_link(final_path);
            else point_link(to);
            wallpaper_transition(from, to);
            if (cfg.follow_omarchy_theme) {
                omarchy_theme_apply(cfg);
                if (bar_) bar_->request_draw();
            }
            return "ok";
        }
        wallpaper_refresh();
        return "ok";
    }
    if (target == "idle") {
        if (method == "ping") return "ok";
        if (method == "status") return idle_status_json();
        if (method == "enable") return idle_set_enabled(true);
        if (method == "disable") return idle_set_enabled(false);
        if (method == "toggle") {
            bool on = idle_status_json().find("\"enabled\":true") !=
                      std::string::npos;
            return idle_set_enabled(!on);
        }
        return "ok";
    }
    if (target == "nightlight") {
        if (method == "ping") return "ok";
        if (method == "status") return nightlight_status_json();
        if (method == "refresh") {
            nightlight_refresh();
            return "ok";
        }
        if (method == "enable") return nightlight_set(true);
        if (method == "disable") return nightlight_set(false);
        if (method == "toggle") return nightlight_toggle();
        return "unknown";
    }
    if (target == "image-selector") return image_selector_dispatch(rest);
    // `omarchy-shell omarchy.audio toggle` — target is the overlay id
    if (find(target)) {
        if (method == "toggle" || method.empty())
            return toggle(target, payload.empty() ? id : payload);
        if (method == "summon" || method == "open" || method == "show")
            return summon(target, payload.empty() ? id : payload);
        if (method == "hide" || method == "close") {
            hide(target);
            return "ok";
        }
        return call(target, method, payload.empty() ? id : payload);
    }
    if (qs_plugin_known(target)) {
        std::string m = method.empty() ? "toggle" : method;
        std::string a = payload.empty() ? id : payload;
        qs_plugins_activate(target, m, a.empty() ? "{}" : a);
        return "ok";
    }
    return "error: unknown target '" + target + "'";
}

Shell* mattbar_shell() { return g_shell; }

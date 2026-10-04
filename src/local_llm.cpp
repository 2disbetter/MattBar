// Local LLM bar chip. Recovered from mattbar 1.43.0 (local_llm.cpp in
// /usr/local/bin/mattbar): LocalLlmModule, health via /health + /api/tags,
// structure_serve / Brave chat float, mattbarctl localllm [toggle|stop].
// This is a behavioral reconstruction from the unstripped binary, not the
// original compiler output.

#include "hyprev.hpp"
#include "modules.hpp"
#include "bar.hpp"
#include "config.hpp"
#include "popup.hpp"
#include "util.hpp"

#include <cairo/cairo.h>
#include <dirent.h>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <time.h>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>
#include <utility>
#include <cstdio>

void draw_llm_icon(cairo_t* cr, double cx, double cy, double size,
                   const Color& c);

#ifndef DBG
#define DBG(...)                                                              \
    do {                                                                      \
        if (getenv("MATTBAR_DEBUG")) {                                        \
            fprintf(stderr, "mattbar: " __VA_ARGS__);                         \
            fputc('\n', stderr);                                              \
        }                                                                     \
    } while (0)
#endif

namespace {

void col(cairo_t* cr, const Color& c, double a = -1) {
    cairo_set_source_rgba(cr, c.r, c.g, c.b, a < 0 ? c.a : a);
}


std::string json_field(const std::string& json, const char* key) {
    std::string pat = std::string("\"") + key + "\":";
    size_t      p   = json.find(pat);
    if (p == std::string::npos) return {};
    p += pat.size();
    while (p < json.size() && (json[p] == ' ' || json[p] == '\t')) ++p;
    if (p < json.size() && json[p] == '"') {
        ++p;
        size_t e = json.find('"', p);
        if (e == std::string::npos) return {};
        return json.substr(p, e - p);
    }
    if (p < json.size() && json[p] == '[') {
        size_t e = json.find(']', p);
        if (e == std::string::npos) return {};
        return json.substr(p, e - p + 1);
    }
    size_t e = p;
    while (e < json.size() &&
           (isalnum((unsigned char)json[e]) || json[e] == 'x' || json[e] == '-'))
        ++e;
    return json.substr(p, e - p);
}

std::string shell_quote(const std::string& s) {
    std::string o = "'";
    for (char c : s) {
        if (c == '\'') o += "'\\''";
        else o += c;
    }
    o += "'";
    return o;
}

std::string engine_url() {
    std::string u = trim(cfg.local_llm_url);
    if (u.empty()) u = "http://127.0.0.1:11434";
    while (u.size() > 1 && u.back() == '/') u.pop_back();
    return u;
}

std::string win_class() {
    return cfg.local_llm_class.empty() ? "com.mattbar.llm" : cfg.local_llm_class;
}

std::string data_dir() {
    const char* rt = getenv("XDG_RUNTIME_DIR");
    std::string base = (rt && *rt) ? rt : "/tmp";
    return base + "/mattbar-llm";
}

// Dedicated static origin for Structure Chat. Chromium will not persist
// Notification permission on file:// (it re-prompts every click).
constexpr int kUiPort = 18765;

std::string ui_dir() { return data_dir() + "/ui"; }

std::string ui_origin() {
    return "http://127.0.0.1:" + std::to_string(kUiPort);
}

void patch_file(const std::string& path, const std::string& from,
                const std::string& to) {
    std::string s = slurp(path);
    if (s.empty()) return;
    bool ch = false;
    for (size_t p = 0; (p = s.find(from, p)) != std::string::npos;) {
        s.replace(p, from.size(), to);
        p += to.size();
        ch = true;
    }
    if (!ch) return;
    std::ofstream out(path, std::ios::trunc);
    if (out) out << s;
}

void write_if_absent(const std::string& path, const std::string& body) {
    if (access(path.c_str(), F_OK) == 0) return;
    std::ofstream out(path, std::ios::trunc);
    if (out) out << body;
}

// Brave treats a dedicated --user-data-dir as a new profile: first-run
// welcome, crash-restore, "send debugging info", and the yellow
// --disable-web-security infobar. Seed the profile so those stay gone.
void prime_brave_profile(const std::string& dir) {
    mkdir(dir.c_str(), 0700);
    mkdir((dir + "/Default").c_str(), 0700);
    // Sentinel: Chromium skips FRE when this file exists.
    write_if_absent(dir + "/First Run", "");
    write_if_absent(
        dir + "/Local State",
        "{\n"
        "  \"browser\": { \"has_seen_welcome_page\": true },\n"
        "  \"brave\": {\n"
        "    \"p3a\": { \"enabled\": false, \"notice_acknowledged\": true },\n"
        "    \"stats\": { \"first_check_made\": true }\n"
        "  },\n"
        "  \"user_experience_metrics\": {\n"
        "    \"reporting_enabled\": false,\n"
        "    \"stability\": { \"exited_cleanly\": true }\n"
        "  }\n"
        "}\n");
    write_if_absent(
        dir + "/Default/Preferences",
        "{\n"
        "  \"browser\": {\n"
        "    \"has_seen_welcome_page\": true,\n"
        "    \"check_default_browser\": false\n"
        "  },\n"
        "  \"profile\": {\n"
        "    \"exit_type\": \"Normal\",\n"
        "    \"exited_cleanly\": true\n"
        "  },\n"
        "  \"enable_do_not_track\": true,\n"
        "  \"safebrowsing\": { \"enabled\": false },\n"
        "  \"user_experience_metrics\": { \"reporting_enabled\": false }\n"
        "}\n");
    // Hide-to-special or a killed popup looks like a crash next launch.
    patch_file(dir + "/Default/Preferences", "\"exit_type\":\"Crashed\"",
               "\"exit_type\":\"Normal\"");
    patch_file(dir + "/Default/Preferences", "\"exit_type\": \"Crashed\"",
               "\"exit_type\": \"Normal\"");
    patch_file(dir + "/Default/Preferences", "\"exited_cleanly\": false",
               "\"exited_cleanly\": true");
    patch_file(dir + "/Local State", "\"exited_cleanly\": false",
               "\"exited_cleanly\": true");
    patch_file(dir + "/Local State", "\"reporting_enabled\": true",
               "\"reporting_enabled\": false");
    // Persist "Allow notifications" for the local HTTP UI origin.
    std::string py =
        "import json, pathlib, time\n"
        "p = pathlib.Path(" +
        shell_quote(dir + "/Default/Preferences") +
        ")\n"
        "try: d = json.loads(p.read_text())\n"
        "except Exception: d = {}\n"
        "ex = d.setdefault('profile', {}).setdefault('content_settings', {})"
        ".setdefault('exceptions', {}).setdefault('notifications', {})\n"
        "key = '" +
        ui_origin() +
        ",*'\n"
        "ex[key] = {'expiration': '0', 'last_modified': str(int(time.time()*1e6)),"
        " 'setting': 1}\n"
        "life = d.setdefault('permission_lifetime', {}).setdefault('expirations', {})\n"
        "life.pop('notifications', None)\n"
        "p.parent.mkdir(parents=True, exist_ok=True)\n"
        "p.write_text(json.dumps(d))\n";
    cmd_output("python3 -c " + shell_quote(py));
}

std::string start_bin() {
    std::string c = trim(cfg.local_llm_start_cmd);
    if (c.empty()) return {};
    // first whitespace-delimited token, skip env assignments
    size_t i = 0;
    while (i < c.size()) {
        while (i < c.size() && isspace((unsigned char)c[i])) ++i;
        size_t j = i;
        while (j < c.size() && !isspace((unsigned char)c[j])) ++j;
        std::string tok = c.substr(i, j - i);
        i               = j;
        if (tok.find('=') != std::string::npos && tok[0] != '/') continue;
        size_t sl = tok.rfind('/');
        return sl == std::string::npos ? tok : tok.substr(sl + 1);
    }
    return {};
}

std::string cmdline_of(const char* pid) {
    std::ifstream f(std::string("/proc/") + pid + "/cmdline");
    if (!f) return {};
    std::string s((std::istreambuf_iterator<char>(f)),
                  std::istreambuf_iterator<char>());
    for (char& c : s)
        if (c == '\0') c = ' ';
    return s;
}

bool is_our_brave_cmd(const std::string& cmd) {
    if (cmd.find("brave") == std::string::npos) return false;
    if (cmd.find("--type=") != std::string::npos) return false; // helper proc
    return cmd.find("--user-data-dir=" + data_dir()) != std::string::npos;
}

std::string json_object_at(const std::string& s, size_t open) {
    if (open >= s.size() || s[open] != '{') return {};
    int  depth  = 0;
    bool in_str = false;
    for (size_t i = open; i < s.size(); ++i) {
        char c = s[i];
        if (in_str) {
            if (c == '\\' && i + 1 < s.size()) {
                ++i;
                continue;
            }
            if (c == '"') in_str = false;
            continue;
        }
        if (c == '"') {
            in_str = true;
            continue;
        }
        if (c == '{') ++depth;
        else if (c == '}') {
            if (--depth == 0) return s.substr(open, i - open + 1);
        }
    }
    return {};
}

std::string lower_copy(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

bool title_is_ours(const std::string& title) {
    std::string t = lower_copy(title);
    return t.find("structure chat") != std::string::npos ||
           t.find("structure-chat") != std::string::npos;
}

bool tcp_listening(int port) {
    if (port <= 0) return false;
    char needle[16];
    snprintf(needle, sizeof needle, ":%04X", port);
    auto scan = [&](const char* path) {
        std::ifstream f(path);
        std::string   line;
        if (f) std::getline(f, line); // header
        while (f && std::getline(f, line)) {
            // local_address is field 2; state 0A = LISTEN
            std::istringstream iss(line);
            std::string sl, local, rem, st;
            iss >> sl >> local >> rem >> st;
            if (st == "0A" && local.find(needle) != std::string::npos)
                return true;
        }
        return false;
    };
    return scan("/proc/net/tcp") || scan("/proc/net/tcp6");
}

int url_port(const std::string& url) {
    size_t colon = url.rfind(':');
    if (colon == std::string::npos || colon + 1 >= url.size()) return 11434;
    // ignore : in http://
    size_t scheme = url.find("://");
    if (scheme != std::string::npos && colon < scheme + 3) return 11434;
    char* end = nullptr;
    long  p   = strtol(url.c_str() + colon + 1, &end, 10);
    if (p > 0 && p < 65536) return (int)p;
    return 11434;
}

bool engine_process_running() {
    std::string bin = start_bin();
    if (bin.empty()) return false;
    DIR* d = opendir("/proc");
    if (!d) return false;
    bool found = false;
    while (dirent* e = readdir(d)) {
        if (e->d_name[0] < '1' || e->d_name[0] > '9') continue;
        std::string cmd = cmdline_of(e->d_name);
        if (cmd.empty()) continue;
        if (cmd.find("--type=") != std::string::npos) continue;
        if (cmd.find(bin) != std::string::npos) {
            found = true;
            break;
        }
    }
    closedir(d);
    return found;
}

bool engine_already_up() {
    if (tcp_listening(url_port(engine_url()))) return true;
    return engine_process_running();
}

void kill_client() {
    std::string dir = data_dir();
    spawn_helper("pkill -f -- " + shell_quote("--user-data-dir=" + dir) +
                   " >/dev/null 2>&1 || true");
}

void stop_ui_server() {
    spawn_helper("pkill -f -- " +
                   shell_quote("python3 -m http.server " +
                               std::to_string(kUiPort)) +
                   " >/dev/null 2>&1 || true");
}

void ensure_ui_server() {
    mkdir(data_dir().c_str(), 0700);
    mkdir(ui_dir().c_str(), 0700);
    if (tcp_listening(kUiPort)) return;
    spawn_detached("python3 -m http.server " + std::to_string(kUiPort) +
                   " --bind 127.0.0.1 --directory " + shell_quote(ui_dir()) +
                   " >/dev/null 2>&1");
    for (int i = 0; i < 20 && !tcp_listening(kUiPort); ++i)
        usleep(30000);
}

void stop_engine() {
    if (!cfg.local_llm_stop_cmd.empty()) {
        spawn_detached(cfg.local_llm_stop_cmd);
    } else {
        std::string u = engine_url();
        if (!u.empty())
            spawn_helper("curl -fsS -m 3 -X POST " + shell_quote(u + "/shutdown") +
                           " >/dev/null 2>&1 || true");
    }
    std::string bin = start_bin();
    if (!bin.empty())
        spawn_helper("pkill -f -- " + shell_quote(bin) +
                       " >/dev/null 2>&1 || true");
}

std::string find_structure_chat() {
    std::vector<std::string> cands;
    auto add = [&](std::string p) {
        if (!p.empty()) cands.push_back(std::move(p));
    };
    if (const char* home = getenv("HOME")) {
        add(std::string(home) + "/Applications/structure-chat.html");
        add(std::string(home) +
            "/Projects/StructureCPP/structure-cpp/structure-chat.html");
    }
    add("/usr/local/share/structure-cpp/structure-chat.html");
    add("/usr/share/structure-cpp/structure-chat.html");
    // next to structure_serve
    std::string which = cmd_output("command -v structure_serve 2>/dev/null");
    which             = trim(which);
    if (!which.empty()) {
        size_t sl = which.rfind('/');
        std::string dir = sl == std::string::npos ? "." : which.substr(0, sl);
        add(dir + "/structure-chat.html");
        add(dir + "/../share/structure-cpp/structure-chat.html");
    }
    for (auto& p : cands)
        if (access(p.c_str(), R_OK) == 0) return p;
    return {};
}

std::string patch_chat_html(const std::string& src) {
    std::string html = slurp(src);
    if (html.empty()) return {};
    const std::string needle = "value=\"http://localhost:11434\"";
    std::string       want   = "value=\"" + engine_url() + "\"";
    size_t            p      = html.find(needle);
    if (p != std::string::npos) html.replace(p, needle.size(), want);
    mkdir(data_dir().c_str(), 0700);
    mkdir(ui_dir().c_str(), 0700);
    std::string dst = ui_dir() + "/chat.html";
    std::ofstream out(dst, std::ios::trunc);
    if (!out) return {};
    out << html;
    return dst;
}

std::string prepare_chat_url() {
    std::string given = trim(cfg.local_llm_chat_url);
    if (given.rfind("http://", 0) == 0 || given.rfind("https://", 0) == 0)
        return given;
    std::string file = given;
    if (file.empty()) file = find_structure_chat();
    if (!file.empty() && access(file.c_str(), R_OK) == 0) {
        patch_chat_html(file);
        ensure_ui_server();
        return ui_origin() + "/chat.html";
    }
    if (given.empty()) return engine_url() + "/chat.html";
    return given;
}

class LocalLlmModule : public Module {
public:
    static LocalLlmModule* g;

    LocalLlmModule() { g = this; }
    ~LocalLlmModule() override {
        if (g == this) g = nullptr;
        close_dismiss_catcher();
        teardown_fds();
    }

    bool enabled() const override { return cfg.show_local_llm; }

    void init(Bar& bar) override {
        bar_  = &bar;
        hdir_ = hypr_instance_dir();
        if (!hdir_.empty()) {
            // Shared event stream (hyprev.hpp), not a private connection.
            sub_ = hyprev::subscribe(
                bar,
                {"openwindow", "closewindow", "activewindow",
                 "activewindowv2"},
                [this](const HyprEvent& ev) { on_line(std::string(ev.line)); });
            preinstall_rules();
        }
        mkdir(data_dir().c_str(), 0700);
        color_ = cfg.c_dim;
        placed_geom_ = geom_key();
        bar.add_click_observer([this](Module* m) {
            if (m == static_cast<Module*>(this)) return;
            if (spawning_ || now_ms() < suppress_dismiss_until_) return;
            if (shown_ || window_is_visible()) show(false);
        });
        refresh_health();
    }

    void tick() override {
        if (!bar_) return;
        ++ticks_;
        if (ticks_ % 5 == 0) refresh_health();
        std::string g = geom_key();
        if (g != placed_geom_) {
            placed_geom_ = g;
            if (shown_ && window_alive()) {
                place_popup();
                apply_dismiss_hole();
            }
        }
        if (spawning_) {
            if (window_alive()) {
                spawning_ = false;
                show(true);
            } else if (now_ms() - spawn_ms_ > 8000) {
                spawning_ = false;
            }
        }
        if (shown_) apply_dismiss_hole();
        if (shown_ && !window_alive()) {
            shown_ = false;
            on_special_ = false;
            addr_.clear();
            close_dismiss_catcher();
            paint();
        }
    }

    bool on_click(double, int button) override {
        if (button == BTN_RIGHT) {
            local_llm_shutdown();
            return true;
        }
        if (button != BTN_LEFT) return false;
        suppress_dismiss_until_ = now_ms() + 400;
        if (window_alive()) {
            if (window_is_visible()) show(false);
            else show(true);
            return true;
        }
        if (spawning_ && now_ms() - spawn_ms_ < 8000) return true;
        ensure_engine();
        mkdir(data_dir().c_str(), 0700);
        std::string url = prepare_chat_url();
        if (url.empty()) url = "http://127.0.0.1:11434";
        spawn_client(url);
        return true;
    }

    double width(cairo_t* cr) override {
        if (local_llm_vector_icon()) return 22;
        resolve_glyph(cr);
        cairo_text_extents_t ext;
        cairo_text_extents(cr, glyph_.c_str(), &ext);
        return ext.x_advance + 8;
    }

    void draw(cairo_t* cr, double x, double h) override {
        if (local_llm_vector_icon()) {
            draw_llm_icon(cr, x + 11, h * 0.5, std::min(20.0, h - 6), color_);
            return;
        }
        resolve_glyph(cr);
        cairo_font_extents_t fe;
        cairo_font_extents(cr, &fe);
        col(cr, color_, 1.0);
        cairo_move_to(cr, x,
                      h * 0.5 + (fe.ascent - fe.descent) / 2.0);
        cairo_show_text(cr, glyph_.c_str());
    }

    void show(bool on) {
        if (addr_.empty() && !window_alive()) {
            shown_ = false;
            close_dismiss_catcher();
            return;
        }
        if (on) {
            std::string dest = normal_ws_id();
            if (dest.empty()) dest = "current";
            move_win(dest, false);
            place_popup();
            shown_         = true;
            on_special_    = true;
            reveal_ms_     = now_ms();
            popup_focused_ = false;
            foreign_seen_  = false;
            suppress_dismiss_until_ = now_ms() + 1500;
            open_dismiss_catcher();
        } else {
            close_dismiss_catcher();
            prime_brave_profile(data_dir());
            if (!move_win("special:mbllm", true)) {
                DBG("llm: hide move failed; closing the popup window");
                hypr_dispatch2(hdir_,
                               "dispatch closewindow " + term_sel(),
                               "dispatch hl.dsp.window.close({ window = \"" +
                                   term_sel() + "\" })");
                addr_.clear();
            }
            shown_      = false;
            on_special_ = false;
            hid_ms_     = now_ms();
        }
        paint();
    }

private:
    void teardown_fds() {
        hyprev::unsubscribe(sub_);
        sub_ = 0;
    }

    void paint() {
        color_ = healthy_ ? cfg.c_fg : cfg.c_dim;
        if (bar_) bar_->request_draw();
    }

    void resolve_glyph(cairo_t* cr) {
        std::string want = cfg.local_llm_glyph.empty() ||
                                   cfg.local_llm_glyph == "auto"
                               ? "LLM"
                               : cfg.local_llm_glyph;
        if (checked_ == cfg.font + want) return;
        checked_          = cfg.font + want;
        auto mapped       = [&](const std::string& t) {
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
        std::string pick = want;
        if (!mapped(pick)) pick = "LLM";
        glyph_ = pick;
    }

    static uint64_t now_ms() {
        timespec ts;
        clock_gettime(CLOCK_BOOTTIME, &ts);
        return ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000ULL;
    }
    static std::string lua_str(const std::string& s) {
        std::string o;
        o.reserve(s.size());
        for (char ch : s) {
            if (ch == '\\' || ch == '"') o += '\\';
            o += ch;
        }
        return o;
    }
    static std::string addr_norm(std::string a) {
        if (a.rfind("0x", 0) == 0 || a.rfind("0X", 0) == 0) a = a.substr(2);
        return a;
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
    bool verify_place() {
        std::string c = client_chunk();
        if (c.empty()) return false;
        return json_field(c, "floating").rfind("true", 0) == 0;
    }
    void preinstall_rules() {
        if (hdir_.empty() || !bar_) return;
        const std::string cls = win_class();
        const std::string sz  = popup_size();
        const std::string mv  = popup_move();
        const char*       tag = "localllm: rule preinstall";
        auto fire_match = [&](const std::string& match_lua,
                              const std::string& match_v2) {
            const std::string lua =
                "dispatch (function() hl.window_rule({ enabled = true, "
                "match = { " +
                match_lua +
                " }, float = true, size = \"" + sz + "\", move = \"" + mv +
                "\", group = \"barred\" }) "
                "return hl.dsp.exec_cmd(\"true\") end)()";
            hypr_fire(*bar_, lua, tag);
            hypr_fire(*bar_, "keyword windowrulev2 float," + match_v2, tag);
            hypr_fire(*bar_,
                      "keyword windowrulev2 size " + sz + "," + match_v2, tag);
            hypr_fire(*bar_,
                      "keyword windowrulev2 move " + mv + "," + match_v2, tag);
            hypr_fire(*bar_, "keyword windowrulev2 group barred," + match_v2,
                      tag);
        };
        // Title rules only apply once the HTML sets <title>; class from
        // CHROME_DESKTOP is the map-time match. Pid-based place_popup is
        // the fallback when Brave still reports brave-browser.
        // Never match title "Structure Chat" or class brave-browser:
        // those hit the daily Brave profile. Class rules only apply if
        // Chromium honors com.mattbar.llm; pid matching is the real gate.
        fire_match("class = \"" + cls + "\"", "class:^(" + cls + ")$");
        fire_match("class = \"brave-com.mattbar.llm\"",
                   "class:^(brave-com\\.mattbar\\.llm)");
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
                sscanf(sz.c_str(), "[%d ,%d]", &aw, &ah) == 2 && aw > 0 &&
                ah > 0) {
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
        if (!shown_) return;
        if (now_ms() < suppress_dismiss_until_) return;
        const uint64_t since = now_ms() - reveal_ms_;
        if (!popup_focused_) {
            foreign_seen_ = true;
            if (!from_click) return;
            if (since <= 400) return;
        }
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
        dismiss_.ensure(*bar_, a, 0, 0, 0, 0, "mattbar-llm-dismiss", 0, 0, out);
        dismiss_.set_input_empty();
    }

    std::string term_sel() const { return "address:0x" + addr_norm(addr_); }

    std::string client_chunk() {
        if (hdir_.empty() || addr_.empty()) return {};
        return client_by_addr(hypr_request(hdir_, "j/clients"), addr_);
    }

    std::string client_by_addr(const std::string& clients,
                               const std::string& addr) {
        std::string want = addr_norm(addr);
        size_t      p    = 0;
        while ((p = clients.find("\"address\":", p)) != std::string::npos) {
            size_t b = clients.rfind('{', p);
            if (b == std::string::npos) {
                p += 10;
                continue;
            }
            std::string chunk = json_object_at(clients, b);
            if (chunk.empty()) break;
            if (addr_norm(json_field(chunk, "address")) == want) return chunk;
            p = b + chunk.size();
        }
        return {};
    }

    static bool class_is_ours(const std::string& cls) {
        if (cls.empty()) return false;
        if (cls == win_class()) return true;
        return cls.find("mattbar.llm") != std::string::npos ||
               cls.find("com.mattbar.llm") != std::string::npos;
    }

    // Daily Brave and the LLM popup are both class brave-browser when
    // --class is ignored. Title "Structure Chat" also appears if --app=
    // attached to the existing profile. The isolated --user-data-dir is
    // the only safe identity.
    bool is_our_window(const std::string& chunk) const {
        if (chunk.empty()) return false;
        std::string pid = json_field(chunk, "pid");
        if (pid.empty()) return false;
        return is_our_brave_cmd(cmdline_of(pid.c_str()));
    }

    bool window_alive() {
        if (hdir_.empty()) return false;
        std::string clients = hypr_request(hdir_, "j/clients");
        std::string title_keep, pid_keep;
        std::vector<std::string> extras;
        size_t      p = 0;
        while ((p = clients.find("\"address\":", p)) != std::string::npos) {
            size_t b = clients.rfind('{', p);
            if (b == std::string::npos) {
                p += 10;
                continue;
            }
            std::string chunk = json_object_at(clients, b);
            if (chunk.empty()) break;
            p = b + chunk.size();
            if (!is_our_window(chunk)) continue;
            std::string a = addr_norm(json_field(chunk, "address"));
            if (a.empty()) continue;
            if (title_is_ours(json_field(chunk, "title"))) {
                if (title_keep.empty()) title_keep = a;
                else extras.push_back(a);
            } else if (pid_keep.empty()) {
                pid_keep = a;
            } else {
                extras.push_back(a);
            }
        }
        std::string keep = !title_keep.empty() ? title_keep : pid_keep;
        if (keep.empty()) {
            addr_.clear();
            return false;
        }
        addr_ = keep;
        // During first spawn Brave maps extra profile windows; closing
        // them here kills the chat. Only reap leftovers once we are idle.
        if (!spawning_) {
            for (const auto& a : extras) {
                if (a == keep) continue;
                hypr_dispatch2(hdir_,
                               "dispatch closewindow address:0x" + a,
                               "dispatch hl.dsp.window.close({ window = "
                               "\"address:0x" +
                                   a + "\" })");
            }
        }
        return true;
    }

    bool window_is_visible() {
        if (addr_.empty()) return false;
        std::string c = client_chunk();
        if (c.empty()) return false;
        return c.find("special:mbllm") == std::string::npos;
    }

    bool move_win(const std::string& ws, bool silent) {
        if (hdir_.empty() || addr_.empty()) return false;
        const std::string sel = term_sel();
        const char* dsp = silent ? "movetoworkspacesilent" : "movetoworkspace";
        std::string legacy =
            std::string("dispatch ") + dsp + " " + ws + "," + sel;
        // Hyprland 0.56 Lua: same dispatcher the agents popup uses.
        std::string lua =
            "dispatch hl.dsp.window.move({ window = \"" + sel +
            "\", workspace = \"" + ws + "\", follow = false })";
        bool ok = hypr_dispatch2(hdir_, legacy, lua);
        DBG("llm: move %s -> %s ok=%d", sel.c_str(), ws.c_str(), (int)ok);
        return ok;
    }

    static std::string popup_size() {
        const std::string& v = cfg.local_llm_popup_size;
        int fields = 0;
        bool ok = !v.empty(), in_num = false;
        for (char ch : v) {
            if (ch >= '0' && ch <= '9') {
                if (!in_num) {
                    in_num = true;
                    ++fields;
                }
            } else if (ch == '%' || ch == ' ')
                in_num = false;
            else {
                ok = false;
                break;
            }
        }
        return (ok && fields == 2) ? v : "70% 80%";
    }
    static std::string popup_move() {
        auto g = cfg.popup_place(popup_size(), cfg.local_llm_popup_anchor, 70, 80);
        return std::to_string(g.x_pct) + "% " + std::to_string(g.y_pct) + "%";
    }
    static std::string geom_key() {
        return popup_size() + "|" + cfg.local_llm_popup_anchor + "|" +
               cfg.position;
    }
    struct PopupPx {
        int w, h, x, y;
    };
    PopupPx popup_px() {
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
            while (p < slice.size() && (slice[p] == ' ' || slice[p] == '\t'))
                ++p;
            return atoi(slice.c_str() + p);
        };
        int ox = 0, oy = 0;
        if (f != std::string::npos) {
            mw = jint("width", f);
            mh = jint("height", f);
            ox = jint("x", f);
            oy = jint("y", f);
        }
        if (mw <= 0) mw = 1920;
        if (mh <= 0) mh = 1080;
        auto g = cfg.popup_place(popup_size(), cfg.local_llm_popup_anchor, 70, 80);
        int w = mw * g.w_pct / 100, h = mh * g.h_pct / 100;
        int x = ox + mw * g.x_pct / 100, y = oy + mh * g.y_pct / 100;
        if (w < 200) w = 200;
        if (h < 150) h = 150;
        if (x < 0) x = 0;
        if (y < 0) y = 0;
        if (x + w > mw) x = std::max(0, mw - w);
        if (y + h > mh) y = std::max(0, mh - h);
        return {w, h, x, y};
    }

    void place_popup() {
        if (hdir_.empty() || addr_.empty()) return;
        std::string sel = term_sel();
        PopupPx     g   = popup_px();
        const std::string wx = std::to_string(g.w), hy = std::to_string(g.h),
                          px = std::to_string(g.x), py = std::to_string(g.y);
        const std::string win = lua_str(sel);
        // First map often lands tiled in a dwindle/tab group because
        // Brave ignores --class and Lua exec_cmd drops static float/size.
        hypr_dispatch2(
            hdir_, "dispatch moveoutofgroup " + sel,
            "dispatch hl.dsp.window.move({ window = \"" + win +
                "\", out_of_group = true })");
        hypr_dispatch2(
            hdir_, "dispatch fullscreenstate 0 0," + sel,
            "dispatch hl.dsp.window.fullscreen({ window = \"" + win +
                "\", action = \"unset\" })");
        // Hyprland 0.56 window.float is a toggle. Only float when tiled.
        if (!verify_place())
            hypr_dispatch2(hdir_, "dispatch setfloating " + sel,
                           "dispatch hl.dsp.window.float({ window = \"" + win +
                               "\", action = \"toggle\" })");
        hypr_dispatch2(hdir_,
                       "dispatch resizewindowpixel exact " + wx + " " + hy +
                           "," + sel,
                       "dispatch hl.dsp.window.resize({ window = \"" + win +
                           "\", exact = true, x = " + wx + ", y = " + hy +
                           " })");
        hypr_dispatch2(
            hdir_, "dispatch movewindowpixel exact " + px + " " + py + "," + sel,
            "dispatch hl.dsp.window.move({ window = \"" + win +
                "\", exact = true, x = " + px + ", y = " + py + " })");
        if (!verify_place())
            hypr_dispatch2(hdir_, "dispatch setfloating " + sel,
                           "dispatch hl.dsp.window.float({ window = \"" + win +
                               "\", action = \"toggle\" })");
        hypr_dispatch2(hdir_, "dispatch focuswindow " + sel,
                       "dispatch hl.dsp.focus({ window = \"" + win + "\" })");
        placed_geom_ = geom_key();
        DBG("llm: place %s float=%d %sx%s @ %s,%s", sel.c_str(),
            (int)verify_place(), wx.c_str(), hy.c_str(), px.c_str(),
            py.c_str());
    }

    void ensure_engine() {
        if (engine_already_up()) {
            healthy_ = true;
            paint();
            return;
        }
        std::string cmd = trim(cfg.local_llm_start_cmd);
        if (cmd.empty()) return;
        mkdir(data_dir().c_str(), 0700);
        std::string lock = data_dir() + "/engine.lock";
        spawn_detached("flock -n " + shell_quote(lock) + " -c " +
                       shell_quote(cmd + " >/dev/null 2>&1") + " &");
        healthy_ = false;
        paint();
    }

    void spawn_client(const std::string& url) {
        std::string dir = data_dir();
        std::string cls = win_class();
        prime_brave_profile(dir);
        const char* home = getenv("HOME");
        if (home && *home) {
            std::string apps = std::string(home) + "/.local/share/applications";
            mkdir(apps.c_str(), 0755);
            write_if_absent(
                apps + "/com.mattbar.llm.desktop",
                "[Desktop Entry]\n"
                "Type=Application\n"
                "Name=Structure Chat\n"
                "StartupWMClass=com.mattbar.llm\n"
                "NoDisplay=true\n"
                "Exec=brave --app=%u\n");
        }
        // --test-type suppresses the "unsupported command-line flag"
        // infobar from --disable-web-security. Crash-restore / "send
        // debugging info" come from a Crashed exit_type in the profile
        // (hide-to-special looks like a crash); prime_brave_profile
        // clears that, and these flags cover the rest.
        // CHROME_DESKTOP is what Chromium uses for the Wayland app_id
        // when --class is ignored.
        // Do not hypr exec_cmd with float/size: when the pid misses
        // (Brave hands off to the already-running daily instance), those
        // rules hit the FOCUSED window — which swallowed the user's
        // main Brave onto special:mbllm as a 50% popup.
        // env+spawn_detached keeps --user-data-dir on a real new process.
        std::string cmd =
            "env CHROME_DESKTOP=com.mattbar.llm.desktop "
            "brave --ozone-platform=wayland --ozone-platform-hint=wayland "
            "--user-data-dir=" +
            shell_quote(dir) +
            " --no-first-run --no-default-browser-check --disable-fre "
            "--disable-sync --disable-infobars --hide-crash-restore-bubble "
            "--disable-session-crashed-bubble --disable-breakpad "
            "--disable-crash-reporter --noerrdialogs --test-type "
            "--disable-web-security --disable-site-isolation-trials "
            "--allow-file-access-from-files "
            "--disable-features=BlockInsecurePrivateNetworkRequests,"
            "InfiniteSessionRestore,TranslateUI,PrivacySandboxSettings4 "
            "--class=" +
            cls + " --name=" + cls + " --app=" + shell_quote(url);
        preinstall_rules();
        spawn_detached(cmd + " >/dev/null 2>&1 &");
        DBG("llm: spawned isolated brave user-data-dir=%s", dir.c_str());
        spawning_ = true;
        spawn_ms_ = now_ms();
        suppress_dismiss_until_ = now_ms() + 2000;
    }

    void on_line(std::string line) {
        // The module inits (and used to query Hyprland on every window
        // map) even when it is switched off; with nothing of ours on screen
        // or on the way, there is nothing to adopt or dismiss.
        if (!cfg.show_local_llm && !shown_ && !spawning_ && addr_.empty())
            return;
        auto eat = [&](const char* ev) {
            if (line.rfind(ev, 0) != 0) return false;
            line = line.substr(strlen(ev));
            return true;
        };
        if (eat("openwindow>>")) {
            size_t c1 = line.find(',');
            if (c1 == std::string::npos) return;
            std::string addr = line.substr(0, c1);
            size_t      c2   = line.find(',', c1 + 1);
            if (c2 == std::string::npos) return;
            std::string ws = line.substr(c1 + 1, c2 - c1 - 1);
            size_t      c3 = line.find(',', c2 + 1);
            std::string cls =
                c3 == std::string::npos
                    ? line.substr(c2 + 1)
                    : line.substr(c2 + 1, c3 - c2 - 1);
            std::string title =
                c3 == std::string::npos ? std::string()
                                        : line.substr(c3 + 1);
            bool spec = ws.find("special:mbllm") != std::string::npos;
            std::string chunk =
                client_by_addr(hypr_request(hdir_, "j/clients"), addr);
            bool ours = is_our_window(chunk);
            if (!ours) {
                // Brave's first launch maps extra windows; that is
                // not "click elsewhere". Only dismiss on a foreign
                // map after the popup has settled.
                if (shown_ && !spawning_ && !spec &&
                    now_ms() >= suppress_dismiss_until_)
                    show(false);
                return;
            }
            std::string a = addr_norm(addr);
            if (!addr_.empty() && a != addr_norm(addr_)) {
                if (spawning_) return; // extra profile window during spawn
                hypr_dispatch2(
                    hdir_, "dispatch closewindow address:0x" + a,
                    "dispatch hl.dsp.window.close({ window = "
                    "\"address:0x" +
                        a + "\" })");
                return;
            }
            addr_ = a;
            spawning_ = false;
            DBG("mattbar: llm: adopted window 0x%s pid=%s",
                addr_.c_str(), json_field(chunk, "pid").c_str());
            show(true);
        } else if (eat("closewindow>>")) {
            if (addr_norm(line) == addr_norm(addr_)) {
                addr_.clear();
                shown_ = false;
                on_special_ = false;
                close_dismiss_catcher();
                paint();
            }
        } else if (eat("activewindow>>")) {
            if (!shown_) return;
            if (now_ms() < suppress_dismiss_until_) return;
            std::string aw = hypr_request(hdir_, "j/activewindow");
            const uint64_t since = now_ms() - reveal_ms_;
            if (is_our_window(aw) ||
                (!addr_.empty() &&
                 aw.find(addr_norm(addr_)) != std::string::npos)) {
                if (!popup_focused_ && (foreign_seen_ || since > 400))
                    popup_focused_ = true;
                return;
            }
            if (!popup_focused_ && since <= 400) {
                foreign_seen_ = true;
                return;
            }
            show(false);
        } else if (eat("activewindowv2>>")) {
            if (!shown_) return;
            if (now_ms() < suppress_dismiss_until_) return;
            std::string a = addr_norm(line);
            while (!a.empty() && (a.back() == '\n' || a.back() == ' '))
                a.pop_back();
            if (a == addr_norm(addr_)) {
                const uint64_t since = now_ms() - reveal_ms_;
                if (!popup_focused_ && (foreign_seen_ || since > 400))
                    popup_focused_ = true;
                return;
            }
            if (!popup_focused_ && now_ms() - reveal_ms_ <= 400) {
                foreign_seen_ = true;
                return;
            }
            show(false);
        }
    }

    void refresh_health() {
        if (!bar_) return;
        std::string u = engine_url();
        std::string cmd =
            "curl -fsS -m 2 " + shell_quote(u + "/health") +
            " || curl -fsS -m 2 " + shell_quote(u + "/api/tags");
        health_cmd_.run(*bar_, cmd,
                        [this](const std::string&, int status) {
                            bool up = (status == 0) || engine_already_up();
                            if (up != healthy_) {
                                healthy_ = up;
                                paint();
                            } else {
                                healthy_ = up;
                            }
                        },
                        3000);
    }

    Bar*        bar_      = nullptr;
    int         sub_ = 0;
    std::string hdir_;
    std::string addr_;
    std::string glyph_   = "LLM";
    std::string checked_;
    Color       color_   = {0.5, 0.5, 0.5, 1};
    AsyncCmd    health_cmd_;
    bool        shown_    = false;
    bool        healthy_  = false;
    bool        spawning_ = false;
    bool        on_special_    = false;
    bool        popup_focused_ = false;
    bool        foreign_seen_  = false;
    uint64_t    reveal_ms_ = 0;
    uint64_t    hid_ms_    = 0;
    uint64_t    spawn_ms_  = 0;
    uint64_t    suppress_dismiss_until_ = 0;
    int         ticks_    = 0;
    std::string placed_geom_;
    PopupWin    dismiss_;
};

LocalLlmModule* LocalLlmModule::g = nullptr;

} // namespace

void draw_llm_icon(cairo_t* cr, double cx, double cy, double size,
                   const Color& c) {
    // Structure Chat columns (structure-chat.html favicon) without the
    // square plate, so it sits on the bar like other module glyphs.
    auto rr = [&](double x, double y, double w, double h, double rad) {
        if (rad > w / 2) rad = w / 2;
        if (rad > h / 2) rad = h / 2;
        if (rad < 0.15) {
            cairo_rectangle(cr, x, y, w, h);
            cairo_fill(cr);
            return;
        }
        cairo_new_sub_path(cr);
        cairo_arc(cr, x + w - rad, y + rad, rad, -M_PI / 2, 0);
        cairo_arc(cr, x + w - rad, y + h - rad, rad, 0, M_PI / 2);
        cairo_arc(cr, x + rad, y + h - rad, rad, M_PI / 2, M_PI);
        cairo_arc(cr, x + rad, y + rad, rad, M_PI, 3 * M_PI / 2);
        cairo_close_path(cr);
        cairo_fill(cr);
    };
    cairo_save(cr);
    const double u = size / 32.0;
    const double x = cx - size / 2.0, y = cy - size / 2.0;
    cairo_set_source_rgba(cr, c.r, c.g, c.b, 1.0);
    rr(x + 7 * u, y + 4 * u, 18 * u, 3 * u, 1 * u);
    rr(x + 9 * u, y + 7 * u, 14 * u, 2 * u, 0);
    rr(x + 10 * u, y + 9 * u, 3 * u, 14 * u, 0.5 * u);
    rr(x + 14.5 * u, y + 9 * u, 3 * u, 14 * u, 0.5 * u);
    rr(x + 19 * u, y + 9 * u, 3 * u, 14 * u, 0.5 * u);
    rr(x + 9 * u, y + 23 * u, 14 * u, 2 * u, 0);
    rr(x + 7 * u, y + 25 * u, 18 * u, 3 * u, 1 * u);
    cairo_restore(cr);
}

bool local_llm_vector_icon() {
    return cfg.local_llm_glyph.empty() || cfg.local_llm_glyph == "auto";
}

void local_llm_shutdown() {
    if (LocalLlmModule::g) LocalLlmModule::g->show(false);
    kill_client();
    stop_engine();
    stop_ui_server();
}

void local_llm_hotkey() {
    if (LocalLlmModule::g) LocalLlmModule::g->on_click(0, BTN_LEFT);
}

Module* make_local_llm() { return new LocalLlmModule; }

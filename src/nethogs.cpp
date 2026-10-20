// Bandwidth bar chip. Click opens a terminal popup running mattnhogs or
// nethogs from /usr/local/bin or /usr/bin. Hide/show keeps that process.
// q quits the tool and the window closes with it.
#include "hyprev.hpp"
#include "modules.hpp"
#include "bar.hpp"
#include "config.hpp"
#include "popup.hpp"
#include "spawn.hpp"

#include <cairo/cairo.h>
#include <linux/input-event-codes.h>
#include <unistd.h>
#include <fcntl.h>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <ctime>

namespace {

constexpr const char* kClass = "com.mattbar.nethogs";
constexpr const char* kSpecial = "special:mbnethogs";
constexpr const char* kSize = "58% 50%";
constexpr const char* kAnchor = "center";

std::string addr_norm(std::string a) {
    if (a.rfind("0x", 0) == 0 || a.rfind("0X", 0) == 0) a = a.substr(2);
    while (!a.empty() && (a.back() == '\n' || a.back() == ' ' || a.back() == '\r'))
        a.pop_back();
    return a;
}

std::string lua_str(const std::string& s) {
    std::string o;
    o.reserve(s.size());
    for (char ch : s) {
        if (ch == '\\' || ch == '"') o += '\\';
        o += ch;
    }
    return o;
}

std::string json_field(const std::string& c, const char* k) {
    std::string key = std::string("\"") + k + "\": ";
    size_t p = c.find(key);
    if (p == std::string::npos) {
        key = std::string("\"") + k + "\":";
        p = c.find(key);
        if (p == std::string::npos) return {};
    }
    p += key.size();
    while (p < c.size() && (c[p] == ' ' || c[p] == '\t')) ++p;
    if (p >= c.size()) return {};
    if (c[p] == '"') {
        size_t e = c.find('"', p + 1);
        if (e == std::string::npos) return {};
        return c.substr(p + 1, e - p - 1);
    }
    size_t e = p;
    if (c[p] == '[') {
        e = c.find(']', p);
        if (e == std::string::npos) return {};
        return c.substr(p, e - p + 1);
    }
    while (e < c.size() && c[e] != ',' && c[e] != '}' && c[e] != '\n') ++e;
    return c.substr(p, e - p);
}

std::string json_object_at(const std::string& s, size_t b) {
    if (b >= s.size() || s[b] != '{') return {};
    int depth = 0;
    for (size_t i = b; i < s.size(); ++i) {
        if (s[i] == '{') ++depth;
        else if (s[i] == '}') {
            --depth;
            if (depth == 0) return s.substr(b, i - b + 1);
        }
    }
    return {};
}

uint64_t now_ms() {
    timespec ts;
    clock_gettime(CLOCK_BOOTTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

bool bin_exists(const char* name) {
    std::string p = std::string("/usr/bin/") + name;
    return access(p.c_str(), X_OK) == 0;
}

std::string tool_name() {
    return cfg.bandwidth_tool == "nethogs" ? "nethogs" : "mattnhogs";
}

std::string tool_path() {
    std::string name = tool_name();
    const char* dirs[] = {"/usr/local/bin/", "/usr/bin/"};
    for (const char* d : dirs) {
        std::string p = std::string(d) + name;
        if (access(p.c_str(), X_OK) == 0) return p;
    }
    return {};
}

bool cmdline_has_tool(pid_t pid) {
    char path[64];
    snprintf(path, sizeof path, "/proc/%d/cmdline", (int)pid);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    char buf[512];
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0) return false;
    for (ssize_t i = 0; i < n; ++i)
        if (buf[i] == '\0') buf[i] = ' ';
    buf[n] = '\0';
    std::string want = tool_name();
    std::string cur;
    for (ssize_t i = 0; i <= n; ++i) {
        if (i == n || buf[i] == ' ') {
            auto slash = cur.find_last_of('/');
            std::string base = slash == std::string::npos ? cur : cur.substr(slash + 1);
            if (base == want) return true;
            cur.clear();
        } else {
            cur.push_back(buf[i]);
        }
    }
    return false;
}

void collect_children(pid_t pid, std::vector<pid_t>& out, int depth) {
    if (depth > 6 || pid <= 1) return;
    char path[96];
    snprintf(path, sizeof path, "/proc/%d/task/%d/children", (int)pid, (int)pid);
    FILE* f = fopen(path, "r");
    if (!f) return;
    int c = 0;
    while (fscanf(f, "%d", &c) == 1) {
        out.push_back((pid_t)c);
        collect_children((pid_t)c, out, depth + 1);
    }
    fclose(f);
}

bool tree_has_tool(pid_t root) {
    if (root <= 1) return false;
    if (cmdline_has_tool(root)) return true;
    std::vector<pid_t> kids;
    collect_children(root, kids, 0);
    for (pid_t c : kids)
        if (cmdline_has_tool(c)) return true;
    return false;
}

std::string term_command() {
    // Match the terminal the agents popup already uses when we can, so
    // this chip does not open a different emulator than the rest of the bar.
    std::string bin;
    auto sp = cfg.agents_term.find(' ');
    if (sp != std::string::npos) bin = cfg.agents_term.substr(0, sp);
    if (bin != "ghostty" && bin != "alacritty" && bin != "kitty" &&
        bin != "foot")
        bin.clear();
    if (bin.empty() || !bin_exists(bin.c_str())) {
        if (bin_exists("ghostty")) bin = "ghostty";
        else if (bin_exists("alacritty")) bin = "alacritty";
        else if (bin_exists("kitty")) bin = "kitty";
        else bin = "foot";
    }
    std::string nh = tool_path();
    if (nh.empty()) nh = tool_name();
    if (bin == "ghostty")
        return "ghostty --class=com.mattbar.nethogs --window-width=120 "
               "--window-height=32 -e " +
               std::string(nh);
    if (bin == "alacritty")
        return "alacritty --class com.mattbar.nethogs "
               "-o window.dimensions.columns=120 -o window.dimensions.lines=32 "
               "-e " +
               std::string(nh);
    if (bin == "kitty")
        return "kitty --class com.mattbar.nethogs -o initial_window_width=120c "
               "-o initial_window_height=32c " +
               std::string(nh);
    return "foot -a com.mattbar.nethogs -W 120x32 " + std::string(nh);
}

class NethogsModule : public Module {
public:
    static NethogsModule* g;
    NethogsModule() { g = this; }
    ~NethogsModule() override {
        if (g == this) g = nullptr;
        close_dismiss_catcher();
        hyprev::unsubscribe(sub_);
    }

    bool enabled() const override {
        return cfg.show_bandwidth && !tool_path().empty();
    }

    void init(Bar& bar) override {
        bar_  = &bar;
        hdir_ = hypr_instance_dir();
        if (!hdir_.empty()) {
            sub_ = hyprev::subscribe(
                bar,
                {"openwindow", "closewindow", "activewindow", "activewindowv2"},
                [this](const HyprEvent& ev) { on_line(std::string(ev.line)); });
        }
        bar.add_click_observer([this](Module* m) {
            if (m == static_cast<Module*>(this)) return;
            if (spawning_ || now_ms() < suppress_until_) return;
            if (shown_) show(false);
        });
        if (find_window()) {
            shown_ = visible();
            if (shown_) open_dismiss_catcher();
            paint();
        }
    }

    void tick() override {
        if (!bar_) return;
        if (spawning_ && addr_.empty() && now_ms() - spawn_ms_ > 8000) {
            spawning_ = false;
            fprintf(stderr, "mattbar: bandwidth: no window after spawn\n");
        }
        if (addr_.empty()) return;
        if (now_ms() < adopt_ms_ + 800) return;
        if (term_pid_ > 1 && tree_has_tool(term_pid_)) {
            saw_nethogs_ = true;
            return;
        }
        // q quit nethogs. Close the terminal if it is still mapped.
        if (saw_nethogs_) {
            fprintf(stderr, "mattbar: bandwidth: quit; closing popup\n");
            close_window();
            clear_session();
            paint();
        }
    }

    bool on_click(double, int button) override {
        if (button != BTN_LEFT) return false;
        suppress_until_ = now_ms() + 400;
        if (find_window()) {
            if (term_pid_ > 1 && !tree_has_tool(term_pid_)) {
                close_window();
                clear_session();
                spawn();
                return true;
            }
            if (visible()) show(false);
            else show(true);
            return true;
        }
        if (spawning_ && now_ms() - spawn_ms_ < 8000) return true;
        spawn();
        return true;
    }

    double width(cairo_t*) override { return 22; }

    void draw(cairo_t* cr, double x, double h) override {
        Color c = shown_ ? cfg.c_accent : (addr_.empty() ? cfg.c_dim : cfg.c_fg);
        double sz = h - 6;
        if (sz > 18) sz = 18;
        draw_bandwidth_icon(cr, x + 11, h * 0.5, sz, c);
    }

    void toggle() { on_click(0, BTN_LEFT); }

private:
    void paint() {
        if (bar_) bar_->request_draw();
    }

    void clear_session() {
        addr_.clear();
        term_pid_    = -1;
        shown_       = false;
        spawning_    = false;
        saw_nethogs_ = false;
        close_dismiss_catcher();
    }

    std::string sel() const { return "address:0x" + addr_norm(addr_); }

    std::string client_by_addr(const std::string& clients, const std::string& addr) {
        std::string want = addr_norm(addr);
        size_t p = 0;
        while ((p = clients.find("\"address\"", p)) != std::string::npos) {
            size_t b = clients.rfind('{', p);
            if (b == std::string::npos) {
                p += 9;
                continue;
            }
            std::string chunk = json_object_at(clients, b);
            if (chunk.empty()) break;
            if (addr_norm(json_field(chunk, "address")) == want) return chunk;
            p = b + chunk.size();
        }
        return {};
    }

    bool class_ours(const std::string& cls) const {
        return cls.find("mattbar.nethogs") != std::string::npos;
    }

    bool find_window() {
        if (hdir_.empty()) return false;
        std::string clients = hypr_request(hdir_, "j/clients");
        size_t p = 0;
        while ((p = clients.find("\"address\"", p)) != std::string::npos) {
            size_t b = clients.rfind('{', p);
            if (b == std::string::npos) {
                p += 9;
                continue;
            }
            std::string chunk = json_object_at(clients, b);
            if (chunk.empty()) break;
            p = b + chunk.size();
            std::string cls = json_field(chunk, "class");
            std::string ic  = json_field(chunk, "initialClass");
            if (!class_ours(cls) && !class_ours(ic)) continue;
            std::string a = addr_norm(json_field(chunk, "address"));
            if (a.empty()) continue;
            addr_ = a;
            int pid = atoi(json_field(chunk, "pid").c_str());
            if (pid > 1) term_pid_ = (pid_t)pid;
            spawning_ = false;
            return true;
        }
        if (!addr_.empty() && !client_by_addr(clients, addr_).empty())
            return true;
        addr_.clear();
        term_pid_ = -1;
        return false;
    }

    bool visible() {
        if (addr_.empty() || hdir_.empty()) return false;
        std::string c = client_by_addr(hypr_request(hdir_, "j/clients"), addr_);
        if (c.empty()) return false;
        std::string ws = json_field(c, "workspace");
        if (ws.find("mbnethogs") != std::string::npos) return false;
        // workspace may be an object; the name is a sibling field.
        return c.find(kSpecial) == std::string::npos;
    }

    std::string normal_ws() {
        std::string mons = hypr_request(hdir_, "j/monitors");
        size_t p = mons.find("\"activeWorkspace\"");
        if (p != std::string::npos) p = mons.find("\"id\":", p);
        if (p == std::string::npos) return {};
        p += 5;
        while (p < mons.size() && (mons[p] == ' ' || mons[p] == '\n')) ++p;
        std::string ws;
        while (p < mons.size() && (isdigit((unsigned char)mons[p]) || mons[p] == '-'))
            ws += mons[p++];
        return ws;
    }

    bool move_win(const std::string& ws, bool silent) {
        if (hdir_.empty() || addr_.empty()) return false;
        const std::string s = sel();
        const char* dsp = silent ? "movetoworkspacesilent" : "movetoworkspace";
        std::string legacy = std::string("dispatch ") + dsp + " " + ws + "," + s;
        std::string lua =
            "dispatch hl.dsp.window.move({ window = \"" + s +
            "\", workspace = \"" + ws + "\", follow = false })";
        return hypr_dispatch2(hdir_, legacy, lua);
    }

    void focus_win() {
        if (hdir_.empty() || addr_.empty()) return;
        const std::string s = sel();
        hypr_dispatch2(hdir_, "dispatch focuswindow " + s,
                       "dispatch hl.dsp.focus({ window = \"" + lua_str(s) + "\" })");
    }

    struct Px { int w, h, x, y; };
    Px popup_px() {
        int mw = 1920, mh = 1080, ox = 0, oy = 0;
        std::string mons = hypr_request(hdir_, "j/monitors");
        size_t f = mons.find("\"focused\": true");
        if (f == std::string::npos) f = mons.find("\"focused\":true");
        auto jint = [&](const char* k) {
            if (f == std::string::npos) return 0;
            size_t lo = f > 900 ? f - 900 : 0;
            std::string slice = mons.substr(lo, 1800);
            std::string key = std::string("\"") + k + "\":";
            size_t p = slice.find(key);
            if (p == std::string::npos) return 0;
            return atoi(slice.c_str() + p + key.size());
        };
        if (f != std::string::npos) {
            mw = jint("width");
            mh = jint("height");
            ox = jint("x");
            oy = jint("y");
        }
        if (mw <= 0) mw = 1920;
        if (mh <= 0) mh = 1080;
        auto g = cfg.popup_place(kSize, kAnchor, 58, 50);
        int w = mw * g.w_pct / 100, h = mh * g.h_pct / 100;
        int x = ox + mw * g.x_pct / 100, y = oy + mh * g.y_pct / 100;
        if (w < 480) w = 480;
        if (h < 280) h = 280;
        if (x < 0) x = 0;
        if (y < 0) y = 0;
        return {w, h, x, y};
    }

    bool floating() {
        std::string c = client_by_addr(hypr_request(hdir_, "j/clients"), addr_);
        return json_field(c, "floating").rfind("true", 0) == 0;
    }

    void place_popup() {
        if (hdir_.empty() || addr_.empty()) return;
        std::string s = sel();
        std::string win = lua_str(s);
        Px g = popup_px();
        std::string wx = std::to_string(g.w), hy = std::to_string(g.h);
        std::string px = std::to_string(g.x), py = std::to_string(g.y);
        hypr_dispatch2(hdir_, "dispatch moveoutofgroup " + s,
                       "dispatch hl.dsp.window.move({ window = \"" + win +
                           "\", out_of_group = true })");
        if (!floating())
            hypr_dispatch2(hdir_, "dispatch setfloating " + s,
                           "dispatch hl.dsp.window.float({ window = \"" + win +
                               "\", action = \"toggle\" })");
        hypr_dispatch2(hdir_,
                       "dispatch resizewindowpixel exact " + wx + " " + hy + "," + s,
                       "dispatch hl.dsp.window.resize({ window = \"" + win +
                           "\", exact = true, x = " + wx + ", y = " + hy + " })");
        hypr_dispatch2(hdir_,
                       "dispatch movewindowpixel exact " + px + " " + py + "," + s,
                       "dispatch hl.dsp.window.move({ window = \"" + win +
                           "\", exact = true, x = " + px + ", y = " + py + " })");
        focus_win();
    }

    void close_window() {
        if (hdir_.empty() || addr_.empty()) return;
        const std::string s = sel();
        hypr_dispatch2(hdir_, "dispatch closewindow " + s,
                       "dispatch hl.dsp.window.close({ window = \"" +
                           lua_str(s) + "\" })");
    }

    void show(bool on) {
        if (addr_.empty() && !find_window()) {
            shown_ = false;
            close_dismiss_catcher();
            paint();
            return;
        }
        if (on) {
            std::string dest = normal_ws();
            if (dest.empty()) dest = "current";
            move_win(dest, false);
            place_popup();
            shown_ = true;
            reveal_ms_ = now_ms();
            popup_focused_ = false;
            foreign_seen_ = false;
            suppress_until_ = now_ms() + 600;
            open_dismiss_catcher();
            fprintf(stderr, "mattbar: nethogs: showing 0x%s\n", addr_.c_str());
        } else {
            close_dismiss_catcher();
            move_win(kSpecial, true);
            shown_ = false;
            fprintf(stderr, "mattbar: nethogs: hidden (still running)\n");
        }
        paint();
    }

    void spawn() {
        if (find_window()) {
            show(true);
            return;
        }
        spawning_ = true;
        spawn_ms_ = now_ms();
        saw_nethogs_ = false;
        std::string cmd = term_command();
        fprintf(stderr, "mattbar: nethogs: starting (%s)\n", cmd.c_str());
        spawn_detached(cmd);
    }

    void adopt(const std::string& addr, const std::string& chunk) {
        addr_ = addr_norm(addr);
        int pid = atoi(json_field(chunk, "pid").c_str());
        if (pid > 1) term_pid_ = (pid_t)pid;
        spawning_ = false;
        adopt_ms_ = now_ms();
        fprintf(stderr, "mattbar: nethogs: adopted 0x%s pid=%d\n",
                addr_.c_str(), pid);
        show(true);
    }

    void on_line(std::string line) {
        if (!cfg.show_bandwidth && !shown_ && !spawning_ && addr_.empty()) return;
        if (line.rfind("openwindow>>", 0) == 0) {
            std::string rest = line.substr(12);
            size_t a = rest.find(',');
            size_t b = a == std::string::npos ? a : rest.find(',', a + 1);
            size_t c = b == std::string::npos ? b : rest.find(',', b + 1);
            if (c == std::string::npos) return;
            std::string cls = rest.substr(b + 1, c - b - 1);
            if (!class_ours(cls)) {
                if (shown_ && !spawning_ && now_ms() >= suppress_until_)
                    show(false);
                return;
            }
            std::string ad = rest.substr(0, a);
            if (!addr_.empty() && addr_norm(ad) != addr_norm(addr_)) {
                // One nethogs. Do not start a second window.
                const std::string s = "address:0x" + addr_norm(ad);
                hypr_dispatch2(hdir_, "dispatch closewindow " + s,
                               "dispatch hl.dsp.window.close({ window = \"" +
                                   s + "\" })");
                return;
            }
            std::string chunk =
                client_by_addr(hypr_request(hdir_, "j/clients"), ad);
            adopt(ad, chunk);
        } else if (line.rfind("closewindow>>", 0) == 0) {
            if (addr_norm(line.substr(13)) == addr_norm(addr_)) {
                fprintf(stderr, "mattbar: nethogs: popup closed\n");
                clear_session();
                paint();
            }
        } else if (line.rfind("activewindow>>", 0) == 0 ||
                   line.rfind("activewindowv2>>", 0) == 0) {
            if (!shown_ || now_ms() < suppress_until_) return;
            bool ours = false;
            if (line.rfind("activewindowv2>>", 0) == 0)
                ours = addr_norm(line.substr(16)) == addr_norm(addr_);
            else
                ours = line.find(kClass) != std::string::npos;
            if (ours) {
                if (!popup_focused_ &&
                    (foreign_seen_ || now_ms() - reveal_ms_ > 400))
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

    void close_dismiss_catcher() { dismiss_.destroy(); }

    void apply_dismiss_hole() {
        if (!dismiss_.surf || !bar_ || !bar_->compositor()) return;
        int W = dismiss_.w, H = dismiss_.h;
        if (W <= 0 || H <= 0) return;
        Px g = popup_px();
        int hx = g.x, hy = g.y, hw = g.w, hh = g.h;
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

    void catcher_hide(bool from_click) {
        if (!shown_ || now_ms() < suppress_until_) return;
        if (!popup_focused_) {
            foreign_seen_ = true;
            if (!from_click) return;
            if (now_ms() - reveal_ms_ <= 400) return;
        }
        show(false);
    }

    void open_dismiss_catcher() {
        if (!bar_) return;
        dismiss_.kb_mode  = 0;
        dismiss_.layer    = ZWLR_LAYER_SHELL_V1_LAYER_TOP;
        dismiss_.centered = false;
        dismiss_.paint    = [](cairo_t*) {};
        dismiss_.click    = [this](double, double, int) { catcher_hide(true); };
        dismiss_.pmotion  = [this](double, double) { catcher_hide(false); };
        dismiss_.on_configured = [this] { apply_dismiss_hole(); };
        wl_output* out = bar_->input_output();
        if (!out) out = bar_->focused_output();
        if (!out) out = bar_->primary_output();
        uint32_t a = ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
                     ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
                     ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
                     ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
        dismiss_.ensure(*bar_, a, 0, 0, 0, 0, "mattbar-nethogs-dismiss", 0, 0,
                        out);
        dismiss_.set_input_empty();
    }

    Bar* bar_ = nullptr;
    std::string hdir_, addr_;
    pid_t term_pid_ = -1;
    int sub_ = 0;
    bool shown_ = false, spawning_ = false, saw_nethogs_ = false;
    bool popup_focused_ = false, foreign_seen_ = false;
    uint64_t spawn_ms_ = 0, adopt_ms_ = 0, reveal_ms_ = 0, suppress_until_ = 0;
    PopupWin dismiss_;
};

NethogsModule* NethogsModule::g = nullptr;

} // namespace

Module* make_nethogs() { return new NethogsModule; }

void nethogs_hotkey() {
    if (NethogsModule::g) NethogsModule::g->toggle();
}

void draw_bandwidth_icon(cairo_t* cr, double cx, double cy, double size,
                         const Color& c) {
    cairo_save(cr);
    cairo_set_source_rgba(cr, c.r, c.g, c.b, 1);
    cairo_set_line_width(cr, size < 14 ? 1.4 : size * 0.11);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    const double lx = cx - size * 0.22;
    const double rx = cx + size * 0.22;
    const double top = cy - size * 0.40;
    const double bot = cy + size * 0.40;
    const double wing = size * 0.16;
    const double head = size * 0.20;
    cairo_new_path(cr);
    cairo_move_to(cr, lx, bot);
    cairo_line_to(cr, lx, top);
    cairo_move_to(cr, lx - wing, top + head);
    cairo_line_to(cr, lx, top);
    cairo_line_to(cr, lx + wing, top + head);
    cairo_move_to(cr, rx, top);
    cairo_line_to(cr, rx, bot);
    cairo_move_to(cr, rx - wing, bot - head);
    cairo_line_to(cr, rx, bot);
    cairo_line_to(cr, rx + wing, bot - head);
    cairo_stroke(cr);
    cairo_restore(cr);
}

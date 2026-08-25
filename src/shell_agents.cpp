// Agents usage panel (`omarchy.agents`). Reads the JSON records
// omarchy-agent-usage-update writes to ~/.local/state/omarchy/agents/usage/
// and matches Omarchy's TUI: hero, subscription switch, limits/balance,
// tokens by day, tokens by model.
#include "overlay.hpp"
#include "util.hpp"

#include <dirent.h>
#include <linux/input-event-codes.h>
#include <time.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using ov::Host;
using ov::col;
using ov::say;
using ov::tw;
using ov::rrect;
using ov::panel_bg;
using ov::select_shell_font;
using ov::utf8_trunc;
using ov::json_str;
using ov::parse_json_string;

namespace {

struct Limit {
    std::string title;
    double      percent = -1; // 0..1
    std::string resets;
};
struct Day {
    std::string date;
    double      tokens = 0;
};
struct Model {
    std::string name;
    double      total = 0, in = 0, out = 0, cr = 0, cw = 0;
};
struct Prov {
    std::string id, name, tier, status, help;
    bool        ready = false;
    std::vector<Limit> limits;
    std::vector<Day>   days;
    std::vector<Model> models;
    bool   has_balance = false, bal_est = false;
    double bal_rem = 0, bal_funded = 0, bal_spent = 0;
    std::string currency = "USD";
    int    today_prompts = 0, today_sessions = 0;
    bool   has_prompt_stats = true;
};

std::string usage_dir() {
    const char* xdg = getenv("XDG_STATE_HOME");
    const char* h   = getenv("HOME");
    std::string base = xdg && *xdg
                           ? std::string(xdg)
                           : std::string(h ? h : ".") + "/.local/state";
    return base + "/omarchy/agents/usage";
}

size_t skip_ws(const std::string& s, size_t i) {
    while (i < s.size() && isspace((unsigned char)s[i])) ++i;
    return i;
}

double jnum(const std::string& s, const char* key, size_t from = 0) {
    std::string pat = std::string("\"") + key + "\"";
    auto p = s.find(pat, from);
    if (p == std::string::npos) return NAN;
    p = s.find(':', p + pat.size());
    if (p == std::string::npos) return NAN;
    p = skip_ws(s, p + 1);
    char* end = nullptr;
    double v  = strtod(s.c_str() + p, &end);
    if (end == s.c_str() + (long)p) return NAN;
    return v;
}

int jnumi(const std::string& s, const char* key, int fb = 0) {
    double v = jnum(s, key);
    return std::isfinite(v) ? (int)std::lround(v) : fb;
}

std::vector<std::string> object_array(const std::string& s, const char* key) {
    std::vector<std::string> out;
    std::string pat = std::string("\"") + key + "\"";
    auto p = s.find(pat);
    if (p == std::string::npos) return out;
    p = s.find('[', p + pat.size());
    if (p == std::string::npos) return out;
    int depth = 0;
    size_t start = 0;
    for (size_t i = p; i < s.size(); ++i) {
        if (s[i] == '{') {
            if (depth == 0) start = i;
            ++depth;
        } else if (s[i] == '}') {
            --depth;
            if (depth == 0) out.push_back(s.substr(start, i - start + 1));
        } else if (s[i] == ']' && depth == 0)
            break;
    }
    return out;
}

std::string window_title(const std::string& label, const std::string& title) {
    if (!title.empty()) return title;
    std::string t = label;
    for (char& c : t) c = (char)tolower((unsigned char)c);
    if (t.find("month") != std::string::npos || t.find("30-day") != std::string::npos)
        return "Monthly";
    if (t.find("week") != std::string::npos || t.find("7-day") != std::string::npos)
        return "Weekly";
    if (t.find("session") != std::string::npos) return "Session";
    auto paren = label.find('(');
    std::string plain = paren == std::string::npos ? label : label.substr(0, paren);
    while (!plain.empty() && plain.back() == ' ') plain.pop_back();
    return plain.empty() ? "Limit" : plain;
}

std::string friendly_model(std::string id) {
    if (id.empty()) return "Unknown";
    if (id.rfind("claude-", 0) == 0) id = id.substr(7);
    if (id.size() > 9 && id[id.size() - 9] == '-') {
        bool digits = true;
        for (size_t i = id.size() - 8; i < id.size(); ++i)
            if (!isdigit((unsigned char)id[i])) digits = false;
        if (digits) id.resize(id.size() - 9);
    }
    std::string out;
    std::string version;
    std::string part;
    auto flush_word = [&](const std::string& w) {
        if (w.empty()) return;
        std::string t = w;
        if (t == "gpt") t = "GPT";
        else if (t == "deepseek") t = "DeepSeek";
        else if (!t.empty()) t[0] = (char)toupper((unsigned char)t[0]);
        if (!out.empty()) out += " ";
        out += t;
    };
    auto flush_ver = [&] {
        if (version.empty()) return;
        if (!out.empty()) out += " ";
        out += version;
        version.clear();
    };
    for (size_t i = 0; i <= id.size(); ++i) {
        char c = i < id.size() ? id[i] : '-';
        if (c == '-' || i == id.size()) {
            if (part.empty()) continue;
            if (isdigit((unsigned char)part[0])) {
                if (!version.empty()) version += ".";
                version += part;
            } else {
                flush_ver();
                flush_word(part);
            }
            part.clear();
        } else
            part += c;
    }
    flush_ver();
    return out.empty() ? "Unknown" : out;
}

std::string fmt_tokens(double n) {
    char b[32];
    if (n >= 1e9) snprintf(b, sizeof b, "%.1fB", n / 1e9);
    else if (n >= 1e6) snprintf(b, sizeof b, "%.1fM", n / 1e6);
    else if (n >= 1e3) snprintf(b, sizeof b, "%.1fK", n / 1e3);
    else snprintf(b, sizeof b, "%.0f", n);
    return b;
}

std::string money(double v, const std::string& cur) {
    char b[48];
    const char* p = "$";
    if (cur == "EUR") p = "\u20AC";
    else if (cur == "GBP") p = "\u00A3";
    else if (cur != "USD" && !cur.empty()) {
        snprintf(b, sizeof b, "%s %.2f", cur.c_str(), v);
        return b;
    }
    snprintf(b, sizeof b, "%s%.2f", p, v);
    return b;
}

std::string today_ymd() {
    time_t t = time(nullptr);
    tm     tm{};
    localtime_r(&t, &tm);
    char b[16];
    strftime(b, sizeof b, "%Y-%m-%d", &tm);
    return b;
}

std::string day_label(const std::string& ymd, const std::string& today) {
    if (ymd == today) return "Today";
    int y = 0, m = 0, d = 0;
    if (sscanf(ymd.c_str(), "%d-%d-%d", &y, &m, &d) != 3) return ymd;
    tm t{};
    t.tm_year = y - 1900;
    t.tm_mon  = m - 1;
    t.tm_mday = d;
    t.tm_hour = 12;
    mktime(&t);
    static const char* n[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    if (t.tm_wday < 0 || t.tm_wday > 6) return ymd;
    return n[t.tm_wday];
}

time_t parse_iso(const std::string& s) {
    int Y = 0, M = 0, D = 0, h = 0, m = 0, sec = 0;
    if (sscanf(s.c_str(), "%d-%d-%dT%d:%d:%d", &Y, &M, &D, &h, &m, &sec) < 6)
        return 0;
    tm t{};
    t.tm_year = Y - 1900;
    t.tm_mon  = M - 1;
    t.tm_mday = D;
    t.tm_hour = h;
    t.tm_min  = m;
    t.tm_sec  = sec;
    return timegm(&t);
}

std::string fmt_dur(time_t reset) {
    time_t now = time(nullptr);
    long   ms  = (long)(reset - now);
    if (ms <= 0) return {};
    long minutes = ms / 60;
    long hours   = minutes / 60;
    long days    = hours / 24;
    char b[32];
    if (days > 0) snprintf(b, sizeof b, "Resets in %ldd %ldh", days, hours % 24);
    else if (hours > 0)
        snprintf(b, sizeof b, "Resets in %ldh %ldm", hours, minutes % 60);
    else
        snprintf(b, sizeof b, "Resets in %ldm", std::max(1L, minutes));
    return b;
}

void parse_models(const std::string& raw, std::vector<Model>& out) {
    auto p = raw.find("\"modelUsage\"");
    if (p == std::string::npos) return;
    p = raw.find('{', p);
    if (p == std::string::npos) return;
    size_t i = p + 1;
    int depth = 1;
    while (i < raw.size() && depth > 0) {
        i = skip_ws(raw, i);
        if (i >= raw.size()) break;
        if (raw[i] == '}') {
            --depth;
            ++i;
            continue;
        }
        if (raw[i] != '"') {
            ++i;
            continue;
        }
        std::string id = parse_json_string(raw, i);
        i = skip_ws(raw, i);
        if (i < raw.size() && raw[i] == ':') ++i;
        i = skip_ws(raw, i);
        if (i >= raw.size() || raw[i] != '{') continue;
        size_t start = i;
        int d = 0;
        do {
            if (raw[i] == '{') ++d;
            else if (raw[i] == '}') --d;
            ++i;
        } while (i < raw.size() && d > 0);
        std::string obj = raw.substr(start, i - start);
        Model m;
        m.name  = friendly_model(id);
        m.in    = std::isfinite(jnum(obj, "inputTokens")) ? jnum(obj, "inputTokens") : 0;
        m.out   = std::isfinite(jnum(obj, "outputTokens")) ? jnum(obj, "outputTokens") : 0;
        m.cr    = std::isfinite(jnum(obj, "cacheReadInputTokens"))
                   ? jnum(obj, "cacheReadInputTokens")
                   : 0;
        m.cw    = std::isfinite(jnum(obj, "cacheCreationInputTokens"))
                   ? jnum(obj, "cacheCreationInputTokens")
                   : 0;
        m.total = m.in + m.out + m.cr + m.cw;
        if (m.total > 0) out.push_back(m);
    }
    std::sort(out.begin(), out.end(),
              [](const Model& a, const Model& b) { return a.total > b.total; });
    if (out.size() > 4) out.resize(4);
}

Prov parse_record(const std::string& raw) {
    Prov p;
    p.id     = json_str(raw, "id");
    p.name   = json_str(raw, "name");
    if (p.name.empty()) p.name = p.id;
    p.tier   = json_str(raw, "tierLabel");
    p.status = json_str(raw, "usageStatusText");
    p.help   = json_str(raw, "authHelpText");
    p.ready  = raw.find("\"ready\":true") != std::string::npos;
    p.today_prompts  = jnumi(raw, "todayPrompts");
    p.today_sessions = jnumi(raw, "todaySessions");
    std::string hps  = json_str(raw, "hasPromptStats");
    if (raw.find("\"hasPromptStats\":false") != std::string::npos)
        p.has_prompt_stats = false;
    for (auto& obj : object_array(raw, "limits")) {
        Limit L;
        L.title   = window_title(json_str(obj, "label"), json_str(obj, "title"));
        double pc = jnum(obj, "percent");
        if (!std::isfinite(pc)) continue;
        L.percent = pc > 1.0 ? pc / 100.0 : pc;
        L.resets  = json_str(obj, "resetsAt");
        p.limits.push_back(L);
    }
    auto bal = raw.find("\"balance\"");
    if (bal != std::string::npos) {
        double rem = jnum(raw, "remaining", bal);
        if (std::isfinite(rem) && rem >= 0) {
            p.has_balance = true;
            p.bal_rem     = rem;
            double f      = jnum(raw, "funded", bal);
            p.bal_funded  = std::isfinite(f) && f > 0 ? f : 0;
            double sp     = jnum(raw, "spent", bal);
            p.bal_spent   = std::isfinite(sp) ? std::max(0.0, sp) : 0;
            std::string c = json_str(raw.substr(bal, 400), "currency");
            if (!c.empty()) p.currency = c;
            p.bal_est = raw.find("\"estimated\":true", bal) != std::string::npos;
        }
    }
    for (auto& obj : object_array(raw, "recentDays")) {
        Day d;
        d.date   = json_str(obj, "date");
        double t = jnum(obj, "messageCount");
        d.tokens = std::isfinite(t) ? t : 0;
        if (!d.date.empty()) p.days.push_back(d);
    }
    parse_models(raw, p.models);
    return p;
}

bool has_data(const Prov& p) {
    if (!p.limits.empty() || p.has_balance) return true;
    if (p.today_prompts > 0 || p.today_sessions > 0) return true;
    for (auto& d : p.days)
        if (d.tokens > 0) return true;
    if (!p.models.empty()) return true;
    if (!p.status.empty()) return true;
    return p.ready;
}

std::vector<Prov> load_providers() {
    std::vector<Prov> out;
    std::string dir = usage_dir();
    DIR* d = opendir(dir.c_str());
    if (!d) return out;
    std::vector<std::string> files;
    while (dirent* e = readdir(d)) {
        std::string n = e->d_name;
        if (n.size() > 5 && n.substr(n.size() - 5) == ".json") files.push_back(n);
    }
    closedir(d);
    std::sort(files.begin(), files.end());
    for (auto& n : files) {
        std::string raw = slurp(dir + "/" + n);
        if (raw.empty()) continue;
        Prov p = parse_record(raw);
        if (p.id.empty()) p.id = n.substr(0, n.size() - 5);
        if (p.name.empty()) p.name = p.id;
        if (has_data(p) || !p.status.empty()) out.push_back(std::move(p));
    }
    return out;
}

void meter(cairo_t* cr, double x, double y, double w, double h, double frac,
           bool alarm) {
    frac = std::clamp(frac, 0.0, 1.0);
    col(cr, cfg.c_ws_bg, 1);
    rrect(cr, x, y, w, h, h / 2);
    cairo_fill(cr);
    if (frac <= 0) return;
    col(cr, alarm ? cfg.c_urgent : cfg.c_fg, 1);
    rrect(cr, x, y, std::max(h, w * frac), h, h / 2);
    cairo_fill(cr);
}

struct Hit {
    double x, y, w, h;
    int    kind = 0; // 0 provider pill
    int    idx  = 0;
};

class AgentsOverlay : public Overlay {
public:
    const char* id() const override { return "omarchy.agents"; }
    void summon(const std::string&) override { open(); }
    void hide() override { host_.close(); }
    bool is_open() const override { return host_.is_open(); }
    std::string call(const std::string& method, const std::string&) override {
        if (method == "refresh") {
            kick_update(true);
            return "ok";
        }
        if (method == "next") {
            if (!provs_.empty()) {
                sel_ = (sel_ + 1) % (int)provs_.size();
                host_.redraw();
            }
            return "ok";
        }
        if (method == "redraw" && host_.is_open()) host_.redraw();
        return "ok";
    }
    ~AgentsOverlay() override { host_.close(); }

private:
    Host host_;
    AsyncCmd cmd_;
    std::vector<Prov> provs_;
    std::vector<Hit> hits_;
    int sel_ = 0, scroll_ = 0;

    int W() const { return 380; }
    int H() const { return std::max(cfg.shell_panel_height, 520); }
    double fs() const { return cfg.shell_agents_font_size; }

    void kick_update(bool force) {
        auto* sh = mattbar_shell();
        if (!sh || !sh->bar()) return;
        std::string c = force ? "omarchy-agent-usage-update --force"
                              : "omarchy-agent-usage-update --limits-only";
        cmd_.run(*sh->bar(), c,
                 [this](const std::string&, int) {
                     load();
                     host_.redraw();
                 },
                 20000);
    }

    void load() {
        std::string keep = sel_ < (int)provs_.size() && sel_ >= 0
                               ? provs_[sel_].id
                               : std::string();
        provs_ = load_providers();
        sel_   = 0;
        for (int i = 0; i < (int)provs_.size(); ++i)
            if (provs_[i].id == keep) sel_ = i;
    }

    void open() {
        load();
        scroll_ = 0;
        host_.win.paint   = [this](cairo_t* cr) { paint(cr); };
        host_.win.click   = [this](double x, double y, int b) { on_click(x, y, b); };
        host_.win.pscroll = [this](int d) {
            scroll_ = std::max(0, scroll_ + d * 28);
            host_.redraw();
        };
        host_.win.pkey = [this](const Bar::KeyEvent& e) { on_key(e); };
        host_.open(W(), H(), "mattbar-agents", id(), true, Host::Place::BarEnd);
        kick_update(false);
    }

    const Prov* cur() const {
        if (sel_ < 0 || sel_ >= (int)provs_.size()) return nullptr;
        return &provs_[sel_];
    }

    void on_key(const Bar::KeyEvent& e) {
        if (!e.pressed) return;
        if (e.escape()) {
            host_.dismiss();
            return;
        }
        if (e.left() || (e.utf8 == "h" || e.utf8 == "H")) {
            if (!provs_.empty()) {
                sel_ = (sel_ + (int)provs_.size() - 1) % (int)provs_.size();
                scroll_ = 0;
                host_.redraw();
            }
            return;
        }
        if (e.right() || (e.utf8 == "l" || e.utf8 == "L")) {
            if (!provs_.empty()) {
                sel_ = (sel_ + 1) % (int)provs_.size();
                scroll_ = 0;
                host_.redraw();
            }
            return;
        }
        if (e.down() || e.utf8 == "j" || e.utf8 == "J") {
            scroll_ += 28;
            host_.redraw();
            return;
        }
        if (e.up() || e.utf8 == "k" || e.utf8 == "K") {
            scroll_ = std::max(0, scroll_ - 28);
            host_.redraw();
            return;
        }
        if (e.enter() || e.utf8 == "r" || e.utf8 == "R") {
            kick_update(true);
            return;
        }
    }

    void on_click(double x, double y, int b) {
        if (b != BTN_LEFT) return;
        for (int i = (int)hits_.size() - 1; i >= 0; --i) {
            const Hit& h = hits_[i];
            if (x < h.x || y < h.y || x >= h.x + h.w || y >= h.y + h.h) continue;
            if (h.kind == 0) {
                sel_    = h.idx;
                scroll_ = 0;
                host_.redraw();
                return;
            }
        }
    }

    void paint(cairo_t* cr) {
        panel_bg(cr, W(), H());
        select_shell_font(cr, fs());
        hits_.clear();
        cairo_save(cr);
        cairo_rectangle(cr, 0, 0, W(), H());
        cairo_clip(cr);
        cairo_translate(cr, 0, -scroll_);

        double y = 16;
        const Prov* p = cur();
        if (provs_.empty()) {
            cairo_set_font_size(cr, fs());
            say(cr, 24, H() / 2.0 - 10, "No AI coding subscriptions found.",
                cfg.c_dim);
            say(cr, 24, H() / 2.0 + 12, "Agents show up here once you've used them.",
                cfg.c_dim);
            cairo_restore(cr);
            return;
        }

        cairo_set_font_size(cr, fs() + 6);
        say(cr, 18, y + 10, p ? p->name : "", cfg.c_fg);
        cairo_set_font_size(cr, fs());
        std::string meta = p && !p->status.empty()
                               ? p->status
                               : (p && !p->tier.empty() ? p->tier : "Subscription");
        if (!meta.empty()) {
            meta[0] = (char)toupper((unsigned char)meta[0]);
            say(cr, 18, y + 28, utf8_trunc(meta, 36), cfg.c_dim);
        }
        y += 48;

        if (provs_.size() > 1) {
            double x = 16;
            double cw =
                (W() - 32.0 - 8.0 * (provs_.size() - 1)) / (double)provs_.size();
            for (int i = 0; i < (int)provs_.size(); ++i) {
                bool on = i == sel_;
                col(cr, on ? cfg.c_accent : cfg.c_ws_bg, 1);
                rrect(cr, x, y, cw, 24, 6);
                cairo_fill(cr);
                cairo_set_font_size(cr, std::max(9.0, fs() - 1));
                std::string lab = utf8_trunc(provs_[i].name, 12);
                double lw = tw(cr, lab);
                say(cr, x + (cw - lw) / 2, y + 12, lab,
                    on ? contrast_on(cfg.c_accent) : cfg.c_fg);
                hits_.push_back({x, y - (double)scroll_, cw, 24, 0, i});
                x += cw + 8;
            }
            y += 36;
        }

        if (p && !p->help.empty() && !p->status.empty()) {
            col(cr, cfg.c_urgent, 0.12);
            rrect(cr, 14, y, W() - 28, 40, 8);
            cairo_fill(cr);
            cairo_set_font_size(cr, std::max(9.0, fs() - 1));
            say(cr, 22, y + 20, utf8_trunc(p->help, 48), cfg.c_dim);
            y += 50;
        }

        auto header = [&](const char* t) {
            cairo_set_font_size(cr, std::max(9.0, fs() - 1));
            say(cr, 18, y + 8, t, cfg.c_accent);
            y += 22;
        };

        if (p && p->has_balance) {
            header("BALANCE");
            cairo_set_font_size(cr, fs());
            say(cr, 18, y + 8, "Prepaid credits", cfg.c_fg);
            bool alarm = p->bal_funded > 0 && p->bal_rem / p->bal_funded <= 0.1;
            std::string v = money(p->bal_rem, p->currency);
            say(cr, W() - 18 - tw(cr, v), y + 8, v,
                alarm ? cfg.c_urgent : cfg.c_fg);
            y += 20;
            if (p->bal_funded > 0) {
                meter(cr, 18, y, W() - 36, 6, p->bal_rem / p->bal_funded, alarm);
                y += 14;
                cairo_set_font_size(cr, std::max(9.0, fs() - 1));
                std::string d = money(p->bal_spent, p->currency) + " spent of " +
                                money(p->bal_funded, p->currency) + " funded";
                if (p->bal_est) d += " \u00b7 estimated";
                say(cr, 18, y + 6, d, cfg.c_dim);
                y += 20;
            }
        }

        if (p && !p->limits.empty()) {
            header("LIMITS");
            for (auto& L : p->limits) {
                cairo_set_font_size(cr, fs());
                bool alarm = L.percent >= 0.9;
                say(cr, 18, y + 8, utf8_trunc(L.title, 22), cfg.c_fg);
                char pct[16];
                snprintf(pct, sizeof pct, "%d%%",
                         (int)std::lround(std::clamp(L.percent, 0.0, 1.0) * 100));
                say(cr, W() - 18 - tw(cr, pct), y + 8, pct,
                    alarm ? cfg.c_urgent : cfg.c_fg);
                y += 18;
                meter(cr, 18, y, W() - 36, 6, L.percent, alarm);
                y += 12;
                time_t rt = parse_iso(L.resets);
                std::string rs = rt ? fmt_dur(rt) : std::string();
                if (!rs.empty()) {
                    cairo_set_font_size(cr, std::max(9.0, fs() - 1));
                    say(cr, 18, y + 6, rs, cfg.c_dim);
                    y += 16;
                }
            }
        }

        if (p && !p->days.empty()) {
            header("TOKENS BY DAY");
            double peak = 1;
            for (auto& d : p->days) peak = std::max(peak, d.tokens);
            std::string today = today_ymd();
            for (auto& d : p->days) {
                bool today_row = d.date == today;
                cairo_set_font_size(cr, fs());
                Color fg = today_row ? cfg.c_fg : cfg.c_dim;
                say(cr, 18, y + 8, day_label(d.date, today), fg);
                meter(cr, 80, y + 5, W() - 160, 6, d.tokens / peak, false);
                std::string tv = fmt_tokens(d.tokens);
                say(cr, W() - 18 - tw(cr, tv), y + 8, tv, fg);
                y += 22;
            }
        }

        if (p && !p->models.empty()) {
            header("TOKENS BY MODEL");
            double peak = std::max(1.0, p->models[0].total);
            for (auto& m : p->models) {
                cairo_set_font_size(cr, fs());
                say(cr, 18, y + 8, utf8_trunc(m.name, 18), cfg.c_fg);
                meter(cr, 18, y + 18, W() - 36, 5, m.total / peak, false);
                std::string tv = fmt_tokens(m.total);
                say(cr, W() - 18 - tw(cr, tv), y + 8, tv, cfg.c_dim);
                y += 32;
            }
        }

        cairo_restore(cr);
        (void)y;
    }
};

} // namespace

Overlay* make_agents_overlay() { return new AgentsOverlay; }

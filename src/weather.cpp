// Weather pill + panel matching Omarchy's omarchy.weather TUI: Open-Meteo
// (and wttr.in for IP auto-detect), location file owned by
// omarchy-weather-location, hero + 3-day forecast, click-to-edit city.
#include "weather.hpp"
#include "overlay.hpp"
#include "ui.hpp"
#include "util.hpp"

#include <linux/input-event-codes.h>

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <vector>

using ov::Host;
using ov::col;
using ov::say;
using ov::tw;
using ov::rrect;
using ov::panel_bg;
using ov::select_shell_font;
using ov::utf8_trunc;
using ov::shell_quote;
using ov::json_str;

namespace {

Bar* g_bar = nullptr;

struct Loc {
    std::string name;
    double      lat = NAN, lon = NAN;
    bool has_coords() const { return std::isfinite(lat) && std::isfinite(lon); }
};

struct Day {
    std::string date, name, icon;
    int         max_c = 0, min_c = 0;
};

struct Now {
    std::string icon = "\uE33D"; // nf-weather-cloud
    std::string location;
    int         temp_c = -999, feels_c = -999, wind_kmh = -1, humidity = -1;
    bool        is_day = true, ok = false;
    std::vector<Day> days;
};

struct Suggest {
    std::string name, desc;
    double      lat = NAN, lon = NAN;
};

Now          g_now;
Loc          g_loc;
AsyncCmd     g_fetch, g_geo, g_wttr;
uint64_t     g_last_ms = 0;
bool         g_fetching = false;

std::string loc_path() {
    const char* xdg = getenv("XDG_STATE_HOME");
    const char* h   = getenv("HOME");
    std::string base = xdg && *xdg
                           ? std::string(xdg)
                           : std::string(h ? h : ".") + "/.local/state";
    return base + "/omarchy/settings/weather.json";
}

int rnd(double v) { return (int)std::lround(v); }

bool use_imperial() {
    std::string u = cfg.weather_unit;
    for (char& c : u) c = (char)tolower((unsigned char)c);
    if (u == "imperial") return true;
    if (u == "metric") return false;
    const char* lang = getenv("LC_ALL");
    if (!lang || !*lang) lang = getenv("LC_MEASUREMENT");
    if (!lang || !*lang) lang = getenv("LANG");
    std::string l = lang ? lang : "";
    return l.find("en_US") != std::string::npos ||
           l.find("en_LR") != std::string::npos;
}

int to_f(int c) { return rnd(c * 9.0 / 5.0 + 32.0); }
int to_mph(int kmh) { return rnd(kmh * 0.621371); }

const char* icon_wttr(int code, bool night) {
    switch (code) {
    case 113: return night ? "\uE32B" : "\uE30D";
    case 116: return night ? "\uE32E" : "\uE302";
    case 119:
    case 122: return "\uE33D";
    case 143:
    case 248:
    case 260: return night ? "\uE346" : "\uE313";
    case 176:
    case 263:
    case 353: return night ? "\uE333" : "\uE308";
    case 179:
    case 227:
    case 230:
    case 323:
    case 326:
    case 368: return night ? "\uE327" : "\uE30A";
    case 182:
    case 185:
    case 281:
    case 284:
    case 311:
    case 314:
    case 317:
    case 320:
    case 350:
    case 362:
    case 365:
    case 374:
    case 377: return "\uE3AD";
    case 200:
    case 386:
    case 389:
    case 392:
    case 395: return "\uE31D";
    case 266:
    case 293:
    case 296:
    case 299:
    case 302:
    case 305:
    case 308:
    case 356:
    case 359: return "\uE318";
    case 329:
    case 332:
    case 335:
    case 338:
    case 371: return "\uE31A";
    default:  return "\uE33D";
    }
}

const char* icon_wmo(int code, bool night) {
    if (code == 0) return icon_wttr(113, night);
    if (code == 1 || code == 2) return icon_wttr(116, night);
    if (code == 3) return icon_wttr(119, night);
    if (code == 45 || code == 48) return icon_wttr(143, night);
    if (code == 51 || code == 53 || code == 55 || code == 56 || code == 57 ||
        code == 61)
        return icon_wttr(266, night);
    if (code == 63 || code == 65 || code == 66 || code == 67 || code == 80 ||
        code == 81 || code == 82)
        return icon_wttr(308, night);
    if (code == 71 || code == 73 || code == 75 || code == 77 || code == 85 ||
        code == 86)
        return icon_wttr(338, night);
    if (code == 95 || code == 96 || code == 99) return icon_wttr(389, night);
    return icon_wttr(119, night);
}

uint64_t now_ms() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

std::string today_ymd() {
    time_t t = time(nullptr);
    tm     tm{};
    localtime_r(&t, &tm);
    char b[16];
    strftime(b, sizeof b, "%Y-%m-%d", &tm);
    return b;
}

std::string weekday(const std::string& ymd) {
    int y = 0, m = 0, d = 0;
    if (sscanf(ymd.c_str(), "%d-%d-%d", &y, &m, &d) != 3) return ymd;
    tm t{};
    t.tm_year = y - 1900;
    t.tm_mon  = m - 1;
    t.tm_mday = d;
    t.tm_hour = 12;
    mktime(&t);
    static const char* names[] = {"Sunday",   "Monday", "Tuesday", "Wednesday",
                                  "Thursday", "Friday", "Saturday"};
    if (t.tm_wday < 0 || t.tm_wday > 6) return ymd;
    return names[t.tm_wday];
}

size_t skip_ws(const std::string& s, size_t i) {
    while (i < s.size() && isspace((unsigned char)s[i])) ++i;
    return i;
}

bool find_key(const std::string& s, const char* key, size_t from, size_t& i) {
    std::string pat = std::string("\"") + key + "\"";
    auto p = s.find(pat, from);
    if (p == std::string::npos) return false;
    i = s.find(':', p + pat.size());
    if (i == std::string::npos) return false;
    i = skip_ws(s, i + 1);
    return true;
}

std::vector<double> num_array(const std::string& s, const char* key,
                              size_t from = 0) {
    std::vector<double> out;
    size_t i = 0;
    if (!find_key(s, key, from, i) || i >= s.size() || s[i] != '[') return out;
    ++i;
    while (i < s.size() && s[i] != ']') {
        i = skip_ws(s, i);
        if (s[i] == ']') break;
        if (s[i] == ',') {
            ++i;
            continue;
        }
        char* end = nullptr;
        double v = strtod(s.c_str() + i, &end);
        if (end == s.c_str() + i) break;
        out.push_back(v);
        i = (size_t)(end - s.c_str());
    }
    return out;
}

std::vector<std::string> str_array(const std::string& s, const char* key,
                                   size_t from = 0) {
    std::vector<std::string> out;
    size_t i = 0;
    if (!find_key(s, key, from, i) || i >= s.size() || s[i] != '[') return out;
    ++i;
    while (i < s.size() && s[i] != ']') {
        i = skip_ws(s, i);
        if (s[i] == ']') break;
        if (s[i] == ',') {
            ++i;
            continue;
        }
        if (s[i] == '"') {
            out.push_back(ov::parse_json_string(s, i));
            continue;
        }
        break;
    }
    return out;
}

double json_num(const std::string& s, const char* key, size_t from = 0) {
    size_t i = 0;
    if (!find_key(s, key, from, i)) return NAN;
    char* end = nullptr;
    double v  = strtod(s.c_str() + i, &end);
    if (end == s.c_str() + i) return NAN;
    return v;
}

void load_loc() {
    std::string raw = slurp(loc_path());
    g_loc = {};
    if (raw.empty()) return;
    g_loc.name = json_str(raw, "name");
    double la = json_num(raw, "latitude"), lo = json_num(raw, "longitude");
    if (std::isfinite(la) && std::isfinite(lo)) {
        g_loc.lat = la;
        g_loc.lon = lo;
    }
}

void apply_open_meteo(const std::string& raw) {
    size_t cur = 0, daily = 0;
    find_key(raw, "current", 0, cur);
    find_key(raw, "daily", 0, daily);
    double t = json_num(raw, "temperature_2m", cur);
    if (!std::isfinite(t)) return;
    Now n;
    n.ok       = true;
    n.temp_c   = rnd(t);
    n.feels_c  = rnd(json_num(raw, "apparent_temperature", cur));
    n.wind_kmh = rnd(json_num(raw, "wind_speed_10m", cur));
    n.humidity = rnd(json_num(raw, "relative_humidity_2m", cur));
    n.is_day   = json_num(raw, "is_day", cur) > 0.5;
    int code   = (int)json_num(raw, "weather_code", cur);
    n.icon     = icon_wmo(code, !n.is_day);
    n.location = g_loc.name;
    auto times = str_array(raw, "time", daily);
    auto codes = num_array(raw, "weather_code", daily);
    auto maxs  = num_array(raw, "temperature_2m_max", daily);
    auto mins  = num_array(raw, "temperature_2m_min", daily);
    std::string today = today_ymd();
    for (size_t i = 0; i < times.size() && n.days.size() < 3; ++i) {
        if (times[i].substr(0, 10) <= today) continue;
        Day d;
        d.date  = times[i].substr(0, 10);
        d.name  = weekday(d.date);
        d.max_c = i < maxs.size() ? rnd(maxs[i]) : 0;
        d.min_c = i < mins.size() ? rnd(mins[i]) : 0;
        d.icon  = icon_wmo(i < codes.size() ? (int)codes[i] : 3, false);
        n.days.push_back(d);
    }
    g_now = std::move(n);
}

void start_open_meteo(double lat, double lon);

int inum(const std::string& s, const char* key, size_t from, int fallback = 0) {
    double v = json_num(s, key, from);
    return std::isfinite(v) ? rnd(v) : fallback;
}

void apply_wttr(const std::string& raw) {
    auto p = raw.find("\"current_condition\"");
    if (p == std::string::npos) return;
    if (raw.find("\"temp_C\"", p) == std::string::npos) return;
    Now n = g_now;
    n.ok       = true;
    n.temp_c   = inum(raw, "temp_C", p);
    n.feels_c  = inum(raw, "FeelsLikeC", p);
    n.wind_kmh = inum(raw, "windspeedKmph", p);
    n.humidity = inum(raw, "humidity", p);
    n.icon     = icon_wttr(inum(raw, "weatherCode", p), false);
    auto area  = raw.find("\"nearest_area\"");
    if (area != std::string::npos) {
        std::string city = json_str(raw.substr(area, 800), "value");
        if (!city.empty() && g_loc.name.empty()) n.location = city;
        else n.location = g_loc.name.empty() ? city : g_loc.name;
        double la = json_num(raw, "latitude", area);
        double lo = json_num(raw, "longitude", area);
        if (std::isfinite(la) && std::isfinite(lo) && !g_loc.has_coords()) {
            g_loc.lat = la;
            g_loc.lon = lo;
            if (g_loc.name.empty()) g_loc.name = city;
            start_open_meteo(la, lo);
        }
    }
    if (n.location.empty()) n.location = g_loc.name;
    g_now = std::move(n);
}

void start_open_meteo(double lat, double lon) {
    if (!g_bar) return;
    g_fetching = true;
    char url[512];
    snprintf(url, sizeof url,
             "curl -fsS --max-time 8 "
             "'https://api.open-meteo.com/v1/forecast"
             "?latitude=%.4f&longitude=%.4f"
             "&daily=weather_code,temperature_2m_max,temperature_2m_min"
             "&current=temperature_2m,apparent_temperature,relative_humidity_2m,"
             "wind_speed_10m,weather_code,is_day"
             "&forecast_days=4&timezone=auto'",
             lat, lon);
    g_fetch.run(*g_bar, url,
                [](const std::string& out, int st) {
                    g_fetching = false;
                    if (st == 0 && !out.empty()) apply_open_meteo(out);
                    g_last_ms = now_ms();
                    if (g_bar) g_bar->request_draw();
                    auto* sh = mattbar_shell();
                    if (sh && sh->is_open("omarchy.weather"))
                        sh->call("omarchy.weather", "redraw", "");
                },
                9000);
}

void start_wttr() {
    if (!g_bar) return;
    g_fetching = true;
    g_wttr.run(*g_bar, "curl -fsS --max-time 10 'https://wttr.in/?format=j1'",
               [](const std::string& out, int st) {
                   g_fetching = false;
                   if (st == 0 && !out.empty()) apply_wttr(out);
                   g_last_ms = now_ms();
                   if (g_bar) g_bar->request_draw();
               },
               11000);
}

void persist_loc() {
    if (g_loc.name.empty()) {
        spawn_detached("omarchy-weather-location --clear");
        return;
    }
    if (g_loc.has_coords()) {
        char cmd[512];
        snprintf(cmd, sizeof cmd, "omarchy-weather-location --set %s %.5f,%.5f",
                 shell_quote(g_loc.name).c_str(), g_loc.lat, g_loc.lon);
        spawn_detached(cmd);
    } else {
        spawn_detached(std::string("omarchy-weather-location --set ") +
                       shell_quote(g_loc.name));
    }
}

} // namespace

void weather_init(Bar& bar) {
    g_bar = &bar;
    load_loc();
}

void weather_refresh(bool force) {
    if (!g_bar) return;
    uint64_t age = now_ms() - g_last_ms;
    int span = std::max(1, cfg.weather_refresh_min) * 60 * 1000;
    if (!force && g_last_ms && age < (uint64_t)span) return;
    load_loc();
    if (g_loc.has_coords()) start_open_meteo(g_loc.lat, g_loc.lon);
    else start_wttr();
}

std::string weather_bar_label() {
    if (!g_now.ok) return g_fetching ? g_now.icon + " \u2026" : "";
    if (!cfg.weather_show_temp) return g_now.icon;
    int t = use_imperial() ? to_f(g_now.temp_c) : g_now.temp_c;
    return g_now.icon + " " + std::to_string(t) + "\u00B0";
}

namespace {

struct Hit {
    double x, y, w, h;
    int    kind = 0; // 0 location, 1 suggestion, 2 clear
    int    idx  = -1;
};

class WeatherOverlay : public Overlay {
public:
    const char* id() const override { return "omarchy.weather"; }
    void summon(const std::string&) override { open(); }
    void hide() override { host_.close(); }
    bool is_open() const override { return host_.is_open(); }
    std::string call(const std::string& method, const std::string&) override {
        if (method == "redraw" && host_.is_open()) host_.redraw();
        if (method == "edit") {
            if (!host_.is_open()) open();
            start_edit();
        }
        return "ok";
    }
    ~WeatherOverlay() override { host_.close(); }

private:
    Host host_;
    std::vector<Hit> hits_;
    bool editing_ = false;
    TextField field_;
    std::vector<Suggest> sugg_;
    int sug_ = 0;

    int W() const { return std::max(cfg.shell_panel_width, 460); }
    int H() const { return 300; }
    double fs() const { return cfg.shell_weather_font_size; }

    void open() {
        weather_refresh(true);
        editing_ = false;
        sugg_.clear();
        host_.win.paint = [this](cairo_t* cr) { paint(cr); };
        host_.win.click = [this](double x, double y, int b) { on_click(x, y, b); };
        host_.win.pkey  = [this](const Bar::KeyEvent& e) { on_key(e); };
        host_.open(W(), H(), "mattbar-weather", id(), true, Host::Place::BarEnd);
    }

    void start_edit() {
        editing_     = true;
        field_.text  = g_loc.name;
        field_.cursor = field_.text.size();
        field_.focused = true;
        sugg_.clear();
        sug_ = 0;
        host_.redraw();
    }

    void cancel_edit() {
        editing_ = false;
        sugg_.clear();
        host_.redraw();
    }

    void geocode() {
        auto* sh = mattbar_shell();
        if (!sh || !sh->bar() || field_.text.size() < 2) {
            sugg_.clear();
            host_.redraw();
            return;
        }
        std::string q = field_.text;
        for (char& c : q)
            if (c == ' ') c = '+';
        std::string cmd =
            "curl -fsS --max-time 5 'https://geocoding-api.open-meteo.com/v1/"
            "search?name=" +
            q + "&count=5&language=en&format=json'";
        g_geo.run(*sh->bar(), cmd,
                  [this](const std::string& out, int st) {
                      sugg_.clear();
                      if (st != 0) {
                          host_.redraw();
                          return;
                      }
                      auto results = out.find("\"results\"");
                      size_t i = results == std::string::npos ? 0 : results;
                      for (int n = 0; n < 5; ++n) {
                          auto np = out.find("\"name\"", i);
                          if (np == std::string::npos) break;
                          size_t k = np;
                          Suggest s;
                          s.name = json_str(out.substr(np, 400), "name");
                          s.lat  = json_num(out, "latitude", np);
                          s.lon  = json_num(out, "longitude", np);
                          std::string a1 = json_str(out.substr(np, 500), "admin1");
                          std::string co = json_str(out.substr(np, 500), "country");
                          if (!a1.empty() && !co.empty()) s.desc = a1 + ", " + co;
                          else s.desc = co.empty() ? a1 : co;
                          if (!s.name.empty() && std::isfinite(s.lat))
                              sugg_.push_back(s);
                          i = np + 8;
                          (void)k;
                      }
                      sug_ = 0;
                      host_.redraw();
                  },
                  6000);
    }

    void commit(const Suggest* s) {
        if (s) {
            g_loc.name = s->name;
            g_loc.lat  = s->lat;
            g_loc.lon  = s->lon;
        } else if (field_.text.empty()) {
            g_loc = {};
        } else {
            g_loc.name = field_.text;
            if (!sugg_.empty()) {
                g_loc.lat = sugg_[sug_].lat;
                g_loc.lon = sugg_[sug_].lon;
                g_loc.name = sugg_[sug_].name;
            }
        }
        persist_loc();
        editing_ = false;
        sugg_.clear();
        weather_refresh(true);
        host_.redraw();
    }

    void on_key(const Bar::KeyEvent& e) {
        if (!e.pressed) return;
        if (editing_) {
            if (e.escape()) {
                cancel_edit();
                return;
            }
            if (e.enter()) {
                commit(sugg_.empty() ? nullptr : &sugg_[sug_]);
                return;
            }
            if (e.down() && !sugg_.empty()) {
                sug_ = std::min(sug_ + 1, (int)sugg_.size() - 1);
                host_.redraw();
                return;
            }
            if (e.up() && !sugg_.empty()) {
                sug_ = std::max(sug_ - 1, 0);
                host_.redraw();
                return;
            }
            if (field_.handle(e)) {
                geocode();
                host_.redraw();
            }
            return;
        }
        if (e.escape()) {
            host_.dismiss();
            return;
        }
        if (e.enter()) start_edit();
    }

    void on_click(double x, double y, int b) {
        if (b != BTN_LEFT) return;
        for (int i = (int)hits_.size() - 1; i >= 0; --i) {
            const Hit& h = hits_[i];
            if (x < h.x || y < h.y || x >= h.x + h.w || y >= h.y + h.h) continue;
            if (h.kind == 0) {
                start_edit();
                return;
            }
            if (h.kind == 2) {
                g_loc = {};
                persist_loc();
                cancel_edit();
                weather_refresh(true);
                return;
            }
            if (h.kind == 1 && h.idx >= 0 && h.idx < (int)sugg_.size()) {
                commit(&sugg_[h.idx]);
                return;
            }
        }
        if (editing_ && y > 90) cancel_edit();
    }

    void paint(cairo_t* cr) {
        panel_bg(cr, W(), H());
        select_shell_font(cr, fs());
        hits_.clear();
        const bool imp = use_imperial();
        const Now& n   = g_now;

        // Hero icon + temp
        cairo_set_font_size(cr, 42);
        say(cr, 18, 48, n.ok ? n.icon : "\uE33D", cfg.c_fg);
        cairo_set_font_size(cr, 36);
        std::string tnum = n.ok ? std::to_string(imp ? to_f(n.temp_c) : n.temp_c)
                                : "\u2014";
        say(cr, 78, 42, tnum, cfg.c_fg);
        cairo_set_font_size(cr, fs() + 2);
        say(cr, 78 + tw(cr, tnum) + 4, 28, n.ok ? (imp ? "\u00B0F" : "\u00B0C") : "",
            cfg.c_dim);

        cairo_set_font_size(cr, fs());
        if (editing_) {
            field_.draw(cr, W() - 210, 16, 168, 26, "Search city");
            hits_.push_back({W() - 210.0, 16, 168, 26, 0, -1});
            say(cr, W() - 34, 29, "\u2715", cfg.c_dim);
            hits_.push_back({W() - 40.0, 16, 24, 26, 2, -1});
        } else {
            std::string loc = n.location.empty() ? g_loc.name : n.location;
            for (char& c : loc)
                if (c >= 'a' && c <= 'z') c = (char)(c - 32);
            std::string pin = "\uf041  " + utf8_trunc(loc, 22);
            double pw = tw(cr, pin);
            say(cr, W() - 18 - pw, 28, pin, cfg.c_dim);
            hits_.push_back({W() - 18.0 - pw, 14, pw + 8, 24, 0, -1});
        }

        if (n.ok) {
            auto stat = [&](double x, const char* k, const std::string& v) {
                cairo_set_font_size(cr, std::max(9.0, fs() - 2));
                say(cr, x, 78, k, cfg.c_dim);
                cairo_set_font_size(cr, fs() + 1);
                say(cr, x, 98, v, cfg.c_fg);
            };
            stat(W() - 210, "FEELS",
                 std::to_string(imp ? to_f(n.feels_c) : n.feels_c) +
                     (imp ? "\u00B0F" : "\u00B0C"));
            stat(W() - 140, "WIND",
                 imp ? std::to_string(to_mph(n.wind_kmh)) + " mph"
                     : std::to_string(n.wind_kmh) + " km/h");
            stat(W() - 62, "HUMID", std::to_string(n.humidity) + "%");
        } else {
            cairo_set_font_size(cr, fs());
            say(cr, 18, 96, g_fetching ? "Fetching forecast\u2026" : "No weather yet",
                cfg.c_dim);
        }

        double y = 118;
        if (editing_ && !sugg_.empty()) {
            cairo_set_font_size(cr, fs());
            for (int i = 0; i < (int)sugg_.size(); ++i) {
                if (i == sug_) {
                    col(cr, cfg.c_accent, 0.28);
                    rrect(cr, 12, y, W() - 24, 26, 6);
                    cairo_fill(cr);
                }
                Color fg = i == sug_ ? contrast_on(cfg.c_accent) : cfg.c_fg;
                say(cr, 20, y + 13, sugg_[i].name, fg);
                say(cr, 20 + tw(cr, sugg_[i].name) + 8, y + 13, sugg_[i].desc,
                    cfg.c_dim);
                hits_.push_back({12, y, W() - 24.0, 26, 1, i});
                y += 28;
            }
        } else if (!n.days.empty()) {
            col(cr, cfg.c_fg, 0.12);
            cairo_rectangle(cr, 16, y, W() - 32, 1);
            cairo_fill(cr);
            y += 16;
            int nd = (int)n.days.size();
            double cell = (W() - 36.0) / std::max(1, nd);
            cairo_set_font_size(cr, fs() + 6);
            for (int i = 0; i < nd; ++i) {
                double x = 18 + i * cell;
                say(cr, x, y + 18, n.days[i].icon, cfg.c_fg);
                cairo_set_font_size(cr, std::max(9.0, fs() - 1));
                std::string dn = n.days[i].name;
                for (char& c : dn)
                    if (c >= 'a' && c <= 'z') c = (char)(c - 32);
                say(cr, x + 28, y + 8, utf8_trunc(dn, 10), cfg.c_dim);
                cairo_set_font_size(cr, fs());
                int hi = imp ? to_f(n.days[i].max_c) : n.days[i].max_c;
                int lo = imp ? to_f(n.days[i].min_c) : n.days[i].min_c;
                say(cr, x + 28, y + 26,
                    std::to_string(hi) + "\u00B0  " + std::to_string(lo) + "\u00B0",
                    cfg.c_fg);
                cairo_set_font_size(cr, fs() + 6);
            }
        }
    }
};

class WeatherModule : public Module {
public:
    bool enabled() const override { return cfg.show_weather; }
    void init(Bar& bar) override {
        bar_ = &bar;
        weather_init(bar);
        weather_refresh(true);
    }
    void tick() override {
        weather_refresh(false);
        std::string n = weather_bar_label();
        if (n != label_ && bar_) bar_->request_draw();
    }
    double width(cairo_t* cr) override {
        label_ = weather_bar_label();
        if (label_.empty()) return 0;
        cairo_text_extents_t e;
        cairo_text_extents(cr, label_.c_str(), &e);
        return e.x_advance;
    }
    void draw(cairo_t* cr, double x, double h) override {
        if (label_.empty()) return;
        cairo_font_extents_t fe;
        cairo_font_extents(cr, &fe);
        cairo_set_source_rgba(cr, cfg.c_fg.r, cfg.c_fg.g, cfg.c_fg.b, 1);
        cairo_move_to(cr, x, h / 2.0 + (fe.ascent - fe.descent) / 2.0);
        cairo_show_text(cr, label_.c_str());
    }
    bool on_click(double, int button) override {
        if (button == BTN_MIDDLE) {
            weather_refresh(true);
            return true;
        }
        if (button == BTN_RIGHT) {
            spawn_detached(
                "omarchy-notification-send \"$(omarchy-weather-status)\"");
            return true;
        }
        if (button != BTN_LEFT) return false;
        spawn_detached(live_panel_click(
            "omarchy-shell shell toggle omarchy.weather", "omarchy.weather"));
        return true;
    }

private:
    Bar*        bar_ = nullptr;
    std::string label_;
};

} // namespace

Module*  make_weather() { return new WeatherModule; }
Overlay* make_weather_overlay() { return new WeatherOverlay; }

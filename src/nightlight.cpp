#include "nightlight.hpp"
#include "bar.hpp"
#include "config.hpp"
#include "modules.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

Bar*     g_bar  = nullptr;
int      g_temp = -1;
bool     g_known = false;
AsyncCmd g_probe;

constexpr int kIdentityK = 6000;

int on_k() { return cfg.nightlight_on_k; }
int off_k() { return cfg.nightlight_off_k; }

bool is_night(int temp) { return temp > 0 && temp < kIdentityK; }

void apply_temp(int temp) {
    g_temp  = temp;
    g_known = true;
    if (g_bar) g_bar->request_draw();
    char cmd[640];
    snprintf(cmd, sizeof cmd,
             "pgrep -x hyprsunset >/dev/null || { "
             "setsid uwsm-app -- hyprsunset >/dev/null 2>&1 & sleep 0.4; }; "
             "for _ in 1 2 3 4 5 6 7 8; do "
             "hyprctl hyprsunset temperature %d >/dev/null 2>&1; "
             "t=$(hyprctl hyprsunset temperature 2>/dev/null | "
             "grep -oE '[0-9]+' | head -n1); "
             "[ \"$t\" = \"%d\" ] && break; sleep 0.2; done",
             temp, temp);
    spawn_detached(cmd);
}

void ingest(const std::string& out) {
    int t = -1;
    for (size_t i = 0; i < out.size(); ++i) {
        if (out[i] >= '0' && out[i] <= '9') {
            t = atoi(out.c_str() + i);
            break;
        }
    }
    bool was = nightlight_enabled();
    g_temp   = t;
    g_known  = true;
    if (g_bar && was != nightlight_enabled()) g_bar->request_draw();
}

} // namespace

void nightlight_init(Bar& bar) { g_bar = &bar; }

void nightlight_refresh() {
    if (!g_bar) return;
    g_probe.run(*g_bar, "hyprctl hyprsunset temperature 2>/dev/null",
                [](const std::string& out, int st) {
                    if (st == 0) ingest(out);
                    else {
                        g_temp  = -1;
                        g_known = true;
                    }
                    if (g_bar) g_bar->request_draw();
                },
                1500);
}

std::string nightlight_status_json() {
    char buf[96];
    if (!g_known || g_temp < 0)
        snprintf(buf, sizeof buf, "{\"enabled\":false,\"temperature\":null}");
    else
        snprintf(buf, sizeof buf, "{\"enabled\":%s,\"temperature\":%d}",
                 is_night(g_temp) ? "true" : "false", g_temp);
    return buf;
}

std::string nightlight_set(bool on) {
    apply_temp(on ? on_k() : off_k());
    return on ? "enabled" : "disabled";
}

std::string nightlight_toggle() { return nightlight_set(!nightlight_enabled()); }

bool nightlight_enabled() { return g_known && is_night(g_temp); }

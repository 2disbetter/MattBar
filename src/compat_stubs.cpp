// Symbols the restored 1.43 core expects that this tree's modules.cpp
// (still the 1.29 subset) does not define. Real implementations live in
// more/display/nightlight modules on upstream; these keep the link whole
// and the idle/lock/LLM resume path compiling.
#include "modules.hpp"
#include "bar.hpp"
#include "shell.hpp"
#include "spawn.hpp"

#include <linux/input-event-codes.h>

namespace {
class HiddenModule : public Module {
public:
    bool   enabled() const override { return false; }
    double width(cairo_t*) override { return 0; }
    void   draw(cairo_t*, double, double) override {}
};
} // namespace

Module* make_more() { return new HiddenModule; }
Module* make_display() { return new HiddenModule; }
Module* make_nightlight() { return new HiddenModule; }
void    more_close() {}
bool    more_is_open() { return false; }

void agents_hotkey() { spawn_detached(cfg.agents_click); }
void agents_pick() { spawn_detached("omarchy-setup-agent"); }

void update_refresh() {}
void update_clear() {}

void calendar_open(Bar& bar, Module* owner) { calendar_toggle(bar, owner); }
void calendar_close() {}
bool calendar_is_open() { return false; }

void media_source_switch() {}
void media_play_pause() {
    spawn_detached("playerctl play-pause >/dev/null 2>&1 || true");
}
void media_next() {
    spawn_detached("playerctl next >/dev/null 2>&1 || true");
}
void media_previous() {
    spawn_detached("playerctl previous >/dev/null 2>&1 || true");
}
void clock_cycle_format() {
    static const char* ring[][2] = {
        {"%a %d %b  %H:%M", "%H:%M"},
        {"%a %d %b  %I:%M %p", "%I:%M"},
        {"%H:%M", "%H:%M"},
    };
    constexpr int n = 3;
    int i = 0;
    for (; i < n; ++i)
        if (cfg.clock_format == ring[i][0]) break;
    i = (i + 1) % n;
    cfg.clock_format          = ring[i][0];
    cfg.clock_format_vertical = ring[i][1];
    cfg.save();
}

void persist_power_profile(const std::string& profile) {
    if (profile.empty()) return;
    spawn_detached("powerprofilesctl set " + sh_quote(profile) +
                   " >/dev/null 2>&1 || true");
}

std::string with_preserved_power_profile(const std::string& cmd) {
    return cmd;
}

std::string live_panel_click(const std::string& stored, const char* overlay_id) {
    auto* sh = mattbar_shell();
    if (sh && overlay_id && *overlay_id && sh->is_open(overlay_id)) {
        sh->hide(overlay_id);
        return {};
    }
    if (!cfg.quickshell_shutdown || !overlay_id || !*overlay_id) return stored;
    return std::string("mattbarctl shell toggle ") + overlay_id + " {}";
}

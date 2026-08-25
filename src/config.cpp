#include "config.hpp"
#include "omarchy_theme.hpp"
#include "qs_plugins.hpp"
#include "util.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <fstream>
#include <map>
#include <sstream>

Config cfg;

// --- color <-> "#RRGGBBAA" -------------------------------------------------
static bool parse_color(const std::string& s, Color& out) {
    if (s.size() < 7 || s[0] != '#') return false;
    auto hex2 = [&](size_t i) {
        return static_cast<int>(strtol(s.substr(i, 2).c_str(), nullptr, 16));
    };
    out.r = hex2(1) / 255.0;
    out.g = hex2(3) / 255.0;
    out.b = hex2(5) / 255.0;
    out.a = s.size() >= 9 ? hex2(7) / 255.0 : 1.0;
    return true;
}

static std::string fmt_color(const Color& c) {
    char buf[16];
    snprintf(buf, sizeof buf, "#%02x%02x%02x%02x",
             static_cast<int>(c.r * 255 + 0.5), static_cast<int>(c.g * 255 + 0.5),
             static_cast<int>(c.b * 255 + 0.5), static_cast<int>(c.a * 255 + 0.5));
    return buf;
}

std::string Config::path() {
    const char* xdg = getenv("XDG_CONFIG_HOME");
    std::string base = xdg && *xdg ? xdg
                                   : std::string(getenv("HOME") ? getenv("HOME")
                                                                : ".") +
                                         "/.config";
    return base + "/mattbar/mattbar.conf";
}

// ---------------------------------------------------------------------------
// Quattro-aware click defaults. Omarchy 4 deleted the omarchy-launch-{wifi,
// audio,bluetooth} wrappers (impala/wiremix/bluetui TUIs) in favour of
// Quickshell panels toggled via `omarchy-shell shell toggle omarchy.<name>`.
// Stock defaults resolve at load time against what is actually installed,
// in BOTH directions (an OS rollback re-resolves back to the TUI wrapper).
// A value the user customised is never touched.
// ---------------------------------------------------------------------------
static bool cmd_exists(const std::string& cmdline) {
    std::string c = cmdline.substr(0, cmdline.find(' '));
    if (c.empty()) return false;
    if (c[0] == '/') return access(c.c_str(), X_OK) == 0;
    const char* path = getenv("PATH");
    if (!path) return false;
    const std::string p(path);
    for (size_t s = 0;;) {
        size_t e   = p.find(':', s);
        auto   dir = p.substr(s, e == std::string::npos ? e : e - s);
        if (!dir.empty() && access((dir + "/" + c).c_str(), X_OK) == 0)
            return true;
        if (e == std::string::npos) return false;
        s = e + 1;
    }
}

static void resolve_click(std::string& v, const char* legacy,
                          const char* quattro) {
    const bool stock = v == legacy || v == quattro || v == "auto";
    if (!stock) return;                       // user's own command: hands off
    if (v != "auto" && cmd_exists(v)) return; // current stock value works
    if (cmd_exists("omarchy-shell")) v = quattro;      // Omarchy 4
    else if (cmd_exists(legacy))     v = legacy;       // Omarchy 3.x
    else if (v == "auto")            v = legacy;       // neither: keep a
    // stable value; the click no-ops exactly as it always did off-Omarchy
}

void Config::resolve_click_defaults() {
    resolve_click(network_click, "omarchy-launch-wifi",
                  "omarchy-shell shell toggle omarchy.network");
    resolve_click(volume_click, "omarchy-launch-audio",
                  "omarchy-shell shell toggle omarchy.audio");
    resolve_click(bluetooth_click, "omarchy-launch-bluetooth",
                  "omarchy-shell shell toggle omarchy.bluetooth");
    resolve_click(mic_click, "omarchy-launch-audio",
                  "omarchy-shell shell toggle omarchy.audio");
    // 1.30 briefly saved the MattBar menu command as the default; treat
    // that as stock so a saved conf does not stick after the toggle is off.
    if (omarchy_click == "auto" ||
        omarchy_click == "mattbarctl shell toggle omarchy.menu")
        omarchy_click = "omarchy-menu";
    // Agent-popup terminal: any RECOGNISED stock value — current or from
    // an older MattBar that saved it to disk — is normalised to the
    // up-to-date form for whichever terminal is installed. A customised
    // value is never touched.
    //
    // Two hard-won constraints live in these strings. (1) --inline:
    // without it omarchy-agent is a launcher that execs
    // omarchy-launch-tui and exits immediately — the popup dies ~60 ms
    // after mapping. (2) ghostty's --class must be a dotted GTK app ID
    // or it is silently ignored (window maps as com.mitchellh.ghostty
    // and the bar never recognises it); the dotted class also makes
    // ghostty run a separate instance whose PID Hyprland 0.55's
    // exec_cmd rules can actually match. kitty and foot take the
    // command positionally (no -e).
    struct TermForm { const char* bin; const char* cmd; const char* cls; };
    // Window size is set with the TERMINAL'S OWN cell flags (110x30
    // cells ~ 920x640 px): Hyprland's Lua builds ignore size in both
    // exec_cmd rule tables and (field-verified) hl.window_rule effects,
    // so the client sizing itself is the one mechanism that cannot be
    // dropped.
    static const TermForm forms[] = {
        {"alacritty",
         "alacritty --class mattbar-agent -o window.dimensions.columns=110 "
         "-o window.dimensions.lines=30 -e omarchy-agent --inline",
         "mattbar-agent"},
        {"ghostty",
         "ghostty --class=com.mattbar.agent --window-width=110 "
         "--window-height=30 -e omarchy-agent --inline",
         "com.mattbar.agent"},
        {"kitty",
         "kitty --class mattbar-agent -o initial_window_width=110c "
         "-o initial_window_height=30c omarchy-agent --inline",
         "mattbar-agent"},
        {"foot", "foot -a mattbar-agent -W 110x30 omarchy-agent --inline",
         "mattbar-agent"},
    };
    static const char* stock_legacy[] = {
        "alacritty --class mattbar-agent -e omarchy-agent",
        "alacritty --class mattbar-agent -e omarchy-agent --inline",
        "ghostty --class=com.mattbar.agent -e omarchy-agent",
        "ghostty --class=com.mattbar.agent -e omarchy-agent --inline",
        "ghostty --class=mattbar-agent -e omarchy-agent",
        "kitty --class mattbar-agent -e omarchy-agent",
        "kitty --class mattbar-agent omarchy-agent --inline",
        "foot -a mattbar-agent -e omarchy-agent",
        "foot -a mattbar-agent omarchy-agent --inline",
    };
    bool term_stock = false;
    for (const auto& f : forms) term_stock |= (agents_term == f.cmd);
    for (const char* s : stock_legacy) term_stock |= (agents_term == s);
    if (term_stock) {
        // Prefer the terminal the current value already names (if it is
        // installed), else the first installed one in preference order.
        const TermForm* pick = nullptr;
        std::string bin = agents_term.substr(0, agents_term.find(' '));
        for (const auto& f : forms)
            if (bin == f.bin && cmd_exists(f.bin)) pick = &f;
        if (!pick)
            for (const auto& f : forms)
                if (cmd_exists(f.bin)) { pick = &f; break; }
        if (pick) {
            agents_term = pick->cmd;
            // Keep the stock class in lockstep (only ever touches the
            // two values we ship; anything else is the user's).
            if (agents_term_class == "mattbar-agent" ||
                agents_term_class == "com.mattbar.agent")
                agents_term_class = pick->cls;
        }
    }
}

void Config::load() {
    std::ifstream f(path());
    if (!f) { // keep defaults; file appears on first Save
        resolve_click_defaults();
        return;
    }
    std::string line;
    while (std::getline(f, line)) {
        // '#' starts a comment only before the '='; color VALUES also begin
        // with '#' and must survive (e.g. strip_color = #ff336680).
        auto hash = line.find('#');
        auto eq0  = line.find('=');
        if (hash != std::string::npos &&
            (eq0 == std::string::npos || hash < eq0))
            line = line.substr(0, hash);
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = trim(line.substr(0, eq));
        std::string v = trim(line.substr(eq + 1));
        if (!v.empty() && v[0] != '#') { // color values start with '#'
            auto c = v.find(" #");
            if (c != std::string::npos) v = trim(v.substr(0, c));
        }
        auto b = [&] { return v == "true" || v == "1" || v == "yes"; };
        auto i = [&] { return atoi(v.c_str()); };

        if      (k == "bar_height")        bar_height = i();
        else if (k == "position")          position = v;
        else if (k == "vertical_width")    vertical_width = i();
        else if (k == "strip_hit_height")  strip_hit_height = i();
        else if (k == "output")            output = v;
        else if (k == "strip_height")      strip_height = i();
        else if (k == "hide_delay_ms")     hide_delay_ms = i();
        else if (k == "reveal_delay_ms")   reveal_delay_ms = i();
        else if (k == "tick_ms")           tick_ms = i();
        else if (k == "tray_collapse_ms")  tray_collapse_ms = i();
        else if (k == "tray_start_expanded") tray_start_expanded = b();
        else if (k == "font")              font = v;
        else if (k == "font_size")         font_size = atof(v.c_str());
        else if (k == "bg_color")          parse_color(v, c_bg);
        else if (k == "strip_color")       parse_color(v, c_strip);
        else if (k == "fg_color")          parse_color(v, c_fg);
        else if (k == "dim_color")         parse_color(v, c_dim);
        else if (k == "accent_color")      parse_color(v, c_accent);
        else if (k == "urgent_color")      parse_color(v, c_urgent);
        else if (k == "pill_color")        parse_color(v, c_ws_bg);
        else if (k == "follow_omarchy_theme") follow_omarchy_theme = b();
        else if (k == "show_workspaces")   show_workspaces = b();
        else if (k == "show_clock")        show_clock = b();
        else if (k == "show_pin")          show_pin = b();
        else if (k == "show_tray")         show_tray = b();
        else if (k == "show_network")      show_network = b();
        else if (k == "show_bluetooth")    show_bluetooth = b();
        else if (k == "bt_auto_heal")      bt_auto_heal = b();
        else if (k == "show_agents")       show_agents = b();
        else if (k == "agents_warn_pct")   agents_warn_pct = atoi(v.c_str());
        else if (k == "agents_glyph")      agents_glyph = v;
        else if (k == "agents_click")      agents_click = v;
        else if (k == "agents_term")       agents_term = v;
        else if (k == "agents_term_class") agents_term_class = v;
        else if (k == "agents_popup_size") {
            agents_popup_size = v;
            set_agents_popup_pct(agents_popup_w_pct(), agents_popup_h_pct());
        }
        else if (k == "agents_click_through")
            agents_click_through = (v == "true" || v == "1");
        else if (k == "show_microphone")   show_microphone = b();
        else if (k == "mic_show_pct")      mic_show_pct = b();
        else if (k == "mic_glyph")         mic_glyph = v;
        else if (k == "mic_muted_glyph")   mic_muted_glyph = v;
        else if (k == "mic_click")         mic_click = v;
        else if (k == "show_screenrecord") show_screenrecord = b();
        else if (k == "screenrecord_glyph") screenrecord_glyph = v;
        else if (k == "screenrecord_procs") screenrecord_procs = v;
        else if (k == "screenrecord_stop")  screenrecord_stop = v;
        else if (k == "show_brightness")   show_brightness = b();
        else if (k == "show_display")      show_display = b();
        else if (k == "show_media")        show_media = b();
        else if (k == "show_caffeine")     show_caffeine = b();
        else if (k == "show_nightlight")   show_nightlight = b();
        else if (k == "nightlight_on_k")
            nightlight_on_k = std::clamp(atoi(v.c_str()), 1000, 5999);
        else if (k == "nightlight_off_k")
            nightlight_off_k = std::clamp(atoi(v.c_str()), 6000, 10000);
        else if (k == "show_weather")      show_weather = b();
        else if (k == "show_active_window") show_active_window = b();
        else if (k == "active_window_max")
            active_window_max = std::clamp(atoi(v.c_str()), 8, 80);
        else if (k == "show_kblayout")     show_kblayout = b();
        else if (k == "show_reminder")     show_reminder = b();
        else if (k == "show_dictation")    show_dictation = b();
        else if (k == "power_show_pct")    power_show_pct = b();
        else if (k == "show_tailscale")    show_tailscale = b();
        else if (k == "show_dropbox")      show_dropbox = b();
        else if (k == "weather_show_temp") weather_show_temp = b();
        else if (k == "weather_unit")      weather_unit = v;
        else if (k == "weather_refresh_min")
            weather_refresh_min = std::clamp(atoi(v.c_str()), 1, 120);
        else if (k == "shell_weather_font_size")
            shell_weather_font_size = std::clamp(atof(v.c_str()), 9.0, 22.0);
        else if (k == "shell_agents_font_size")
            shell_agents_font_size = std::clamp(atof(v.c_str()), 9.0, 22.0);
        else if (k == "enable_notifications") enable_notifications = b();
        else if (k == "enable_osd")        enable_osd = b();
        else if (k == "notification_timeout_s")
            notification_timeout_s = std::clamp(atoi(v.c_str()), 0, 300);
        else if (k == "notifications_takeover") notifications_takeover = b();
        else if (k == "quickshell_shutdown")    quickshell_shutdown = b();
        else if (k == "qs_plugins")             qs_plugins = b();
        else if (k == "show_plugins")           show_plugins = b();
        else if (k == "qs_plugin_layout")       qs_plugin_layout = v;
        else if (k == "qs_plugin_services")     qs_plugin_services = v;
        else if (k == "idle_blank_s")
            idle_blank_s = std::clamp(atoi(v.c_str()), 0, 120);
        else if (k == "shell_font_size")
            shell_font_size = std::clamp(atof(v.c_str()), 9.0, 28.0);
        else if (k == "shell_audio_font_size")
            shell_audio_font_size = std::clamp(atof(v.c_str()), 9.0, 22.0);
        else if (k == "shell_network_font_size")
            shell_network_font_size = std::clamp(atof(v.c_str()), 9.0, 22.0);
        else if (k == "shell_bluetooth_font_size")
            shell_bluetooth_font_size = std::clamp(atof(v.c_str()), 9.0, 22.0);
        else if (k == "shell_display_font_size")
            shell_display_font_size = std::clamp(atof(v.c_str()), 9.0, 22.0);
        else if (k == "shell_clipboard_font_size")
            shell_clipboard_font_size = std::clamp(atof(v.c_str()), 9.0, 22.0);
        else if (k == "shell_emoji_font_size")
            shell_emoji_font_size = std::clamp(atof(v.c_str()), 9.0, 22.0);
        else if (k == "shell_image_font_size")
            shell_image_font_size = std::clamp(atof(v.c_str()), 9.0, 22.0);
        else if (k == "shell_panel_width")
            shell_panel_width = std::clamp(atoi(v.c_str()), 280, 720);
        else if (k == "shell_panel_height")
            shell_panel_height = std::clamp(atoi(v.c_str()), 300, 900);
        else if (k == "shell_overlay_width")
            shell_overlay_width = std::clamp(atoi(v.c_str()), 360, 960);
        else if (k == "shell_overlay_height")
            shell_overlay_height = std::clamp(atoi(v.c_str()), 300, 900);
        else if (k == "shell_audio_step")
            shell_audio_step = std::clamp(atoi(v.c_str()), 1, 20);
        else if (k == "shell_audio_show_apps") shell_audio_show_apps = b();
        else if (k == "shell_audio_show_pct")  shell_audio_show_pct = b();
        else if (k == "shell_wifi_scan_on_open") shell_wifi_scan_on_open = b();
        else if (k == "shell_bt_scan_on_open")   shell_bt_scan_on_open = b();
        else if (k == "shell_clipboard_limit")
            shell_clipboard_limit = std::clamp(atoi(v.c_str()), 20, 1000);
        else if (k == "shell_clipboard_paste") shell_clipboard_paste = b();
        else if (k == "shell_emoji_insert")    shell_emoji_insert = b();
        else if (k == "shell_image_show_labels") shell_image_show_labels = b();
        else if (k == "osd_volume")        osd_volume = b();
        else if (k == "osd_mic")           osd_mic = b();
        else if (k == "osd_brightness")    osd_brightness = b();
        else if (k == "notify_battery")    notify_battery = b();
        else if (k == "notify_bluetooth")  notify_bluetooth = b();
        else if (k == "osd_timeout_ms")
            osd_timeout_ms = std::clamp(atoi(v.c_str()), 250, 10000);
        else if (k == "notification_max_shown")
            notification_max_shown = std::clamp(atoi(v.c_str()), 1, 10);
        else if (k == "notification_emoji_font") notification_emoji_font = v;
        else if (k == "show_volume")       show_volume = b();
        else if (k == "show_battery")      show_battery = b();
        else if (k == "show_omarchy")      show_omarchy = b();
        else if (k == "show_update")       show_update = b();
        else if (k == "network_click")     network_click = v;
        else if (k == "volume_click")      volume_click = v;
        else if (k == "bluetooth_click")   bluetooth_click = v;
        else if (k == "bluetooth_glyph")   bluetooth_glyph = v;
        else if (k == "brightness_glyph")  brightness_glyph = v;
        else if (k == "brightness_step")
            brightness_step = std::clamp(atoi(v.c_str()), 1, 25);
        else if (k == "media_show_artist") media_show_artist = b();
        else if (k == "media_len")
            media_len = std::clamp(atoi(v.c_str()), 8, 80);
        else if (k == "bluetooth_show_name")    bluetooth_show_name = b();
        else if (k == "bluetooth_show_battery") bluetooth_show_battery = b();
        else if (k == "bluetooth_show_count")   bluetooth_show_count = b();
        else if (k == "bluetooth_name_len")
            bluetooth_name_len = std::clamp(atoi(v.c_str()), 4, 64);
        else if (k == "omarchy_glyph")     omarchy_glyph = v;
        else if (k == "omarchy_font")      omarchy_font = v;
        else if (k == "omarchy_click")     omarchy_click = v;
        else if (k == "omarchy_right_click") omarchy_right_click = v;
        else if (k == "update_glyph")      update_glyph = v;
        else if (k == "update_check")      update_check = v;
        else if (k == "update_click")      update_click = v;
        else if (k == "update_interval_s") update_interval_s = i();
        else if (k == "update_signal")     update_signal = i();
        else if (k == "show_temp")         show_temp = b();
        else if (k == "temp_sensor")       temp_sensor = v;
        else if (k == "temp_warn")         temp_warn = i();
        else if (k == "multi_monitor")     multi_monitor = b();
        else if (k == "monitors")          monitors = v;
        else if (k == "primary_output")    primary_output = v;
        else if (k == "show_notifications") show_notifications = b();
        else if (k == "show_power")        show_power = b();
        else if (k == "clock_format")      clock_format = v;
        else if (k == "clock_format_vertical") clock_format_vertical = v;
        else if (k == "battery_show_time") battery_show_time = b();
        else if (k == "volume_headset_indicator") volume_headset_indicator = b();
        else if (k == "volume_headset_glyph") volume_headset_glyph = v;
        else if (k == "reveal_all_monitors") reveal_all_monitors = b();
        else if (k == "calendar_enabled")  calendar_enabled = b();
        else if (k == "calendar_monday_first") calendar_monday_first = b();
        else if (k == "calendar_week_numbers") calendar_week_numbers = b();
        else if (k == "notify_glyph")      notify_glyph = v;
        else if (k == "bell_show_count")   bell_show_count = b();
        else if (k == "bell_dim_when_empty") bell_dim_when_empty = b();
        else if (k == "history_max")
            history_max = std::clamp(atoi(v.c_str()), 5, 100);
        else if (k == "muted_apps")        muted_apps = v;
        else if (k == "known_apps")        known_apps = v;
        else if (k == "power_show_label")  power_show_label = b();
        else if (k == "power_show_actions") power_show_actions = b();
        else if (k == "power_cmd_suspend")  power_cmd_suspend = v;
        else if (k == "power_cmd_hibernate") power_cmd_hibernate = v;
        else if (k == "power_cmd_restart")  power_cmd_restart = v;
        else if (k == "power_cmd_shutdown") power_cmd_shutdown = v;
        else if (k == "power_glyph_saver") power_glyph_saver = v;
        else if (k == "power_glyph_balanced") power_glyph_balanced = v;
        else if (k == "power_glyph_perf")  power_glyph_perf = v;
        else if (k == "layout_left")       layout_left = v;
        else if (k == "layout_center")     layout_center = v;
        else if (k == "layout_right")      layout_right = v;
        else if (k == "layout_more")       layout_more = v;
    }
    if (position != "top" && position != "bottom" && position != "left" &&
        position != "right")
        position = "top";
    layout_normalize();
    // The conf file always holds the USER's palette. Snapshot it, then, if
    // theme-following is on, overlay the active Omarchy theme onto the
    // effective colors (c_*) only.
    colors_snapshot();
    if (follow_omarchy_theme) omarchy_theme_apply(*this);
    resolve_click_defaults();
}

void Config::colors_snapshot() {
    u_bg = c_bg; u_strip = c_strip; u_fg = c_fg; u_dim = c_dim;
    u_accent = c_accent; u_urgent = c_urgent; u_ws_bg = c_ws_bg;
}

void Config::colors_restore() {
    c_bg = u_bg; c_strip = u_strip; c_fg = u_fg; c_dim = u_dim;
    c_accent = u_accent; c_urgent = u_urgent; c_ws_bg = u_ws_bg;
}

void Config::save() const {
    std::string p = path();
    auto slash = p.rfind('/');
    if (slash != std::string::npos) {
        std::string dir = p.substr(0, slash);
        // mkdir -p (two levels: .config/mattbar)
        auto s2 = dir.rfind('/');
        if (s2 != std::string::npos) mkdir(dir.substr(0, s2).c_str(), 0755);
        mkdir(dir.c_str(), 0755);
    }
    std::ofstream f(p);
    if (!f) return;
    f << "# MattBar configuration (written by the settings window)\n"
      << "# position: top | bottom | left | right\n"
      << "position = " << position << "\n"
      << "vertical_width = " << vertical_width << "\n"
      << "bar_height = " << bar_height << "\n"
      << "strip_height = " << strip_height << "\n"
      << "strip_hit_height = " << strip_hit_height << "\n"
      << "# output: monitor name like DP-2; empty = compositor picks\n"
      << "output = " << output << "\n"
      << "# multi_monitor: one bar per output. monitors = CSV of names\n"
      << "# (empty = every output). primary_output hosts the tray, the\n"
      << "# settings window and notification/OSD popups.\n"
      << "multi_monitor = " << (multi_monitor ? "true" : "false") << "\n"
      << "monitors = " << monitors << "\n"
      << "primary_output = " << primary_output << "\n"
      << "hide_delay_ms = " << hide_delay_ms << "\n"
      << "reveal_delay_ms = " << reveal_delay_ms << "\n"
      << "tick_ms = " << tick_ms << "\n"
      << "# tray_collapse_ms: 0 = never\n"
      << "tray_collapse_ms = " << tray_collapse_ms << "\n"
      << "tray_start_expanded = " << (tray_start_expanded ? "true" : "false")
      << "\n"
      << "font = " << font << "\n"
      << "font_size = " << font_size << "\n"
      // colors: always the user's own palette — when Omarchy theme
      // following is active, c_* hold the theme overlay and u_* the user's
      // colors (kept fresh: following can only turn on via a snapshot).
      << "bg_color = " << fmt_color(follow_omarchy_theme ? u_bg : c_bg) << "\n"
      << "strip_color = " << fmt_color(follow_omarchy_theme ? u_strip : c_strip) << "\n"
      << "fg_color = " << fmt_color(follow_omarchy_theme ? u_fg : c_fg) << "\n"
      << "dim_color = " << fmt_color(follow_omarchy_theme ? u_dim : c_dim) << "\n"
      << "accent_color = " << fmt_color(follow_omarchy_theme ? u_accent : c_accent) << "\n"
      << "urgent_color = " << fmt_color(follow_omarchy_theme ? u_urgent : c_urgent) << "\n"
      << "pill_color = " << fmt_color(follow_omarchy_theme ? u_ws_bg : c_ws_bg) << "\n"
      << "follow_omarchy_theme = " << (follow_omarchy_theme ? "true" : "false") << "\n"
      << "show_workspaces = " << (show_workspaces ? "true" : "false") << "\n"
      << "show_clock = " << (show_clock ? "true" : "false") << "\n"
      << "show_pin = " << (show_pin ? "true" : "false") << "\n"
      << "show_tray = " << (show_tray ? "true" : "false") << "\n"
      << "show_network = " << (show_network ? "true" : "false") << "\n"
      << "show_bluetooth = " << (show_bluetooth ? "true" : "false") << "\n"
      << "# bt_auto_heal: cycle a device once when it is connected but has\n"
      << "# no audio transport (wedge auto-recovery)\n"
      << "bt_auto_heal = " << (bt_auto_heal ? "true" : "false") << "\n"
      << "show_agents = " << (show_agents ? "true" : "false") << "\n"
      << "agents_warn_pct = " << agents_warn_pct << "\n"
      << "agents_glyph = " << agents_glyph << "\n"
      << "agents_click = " << agents_click << "\n"
      << "agents_term = " << agents_term << "\n"
      << "agents_term_class = " << agents_term_class << "\n"
      << "# agents_popup_size: W% H% of the monitor (Settings: Agents)\n"
      << "agents_popup_size = " << agents_popup_size << "\n"
      << "agents_click_through = "
      << (agents_click_through ? "true" : "false") << "\n"
      << "show_microphone = " << (show_microphone ? "true" : "false") << "\n"
      << "mic_show_pct = " << (mic_show_pct ? "true" : "false") << "\n"
      << "mic_glyph = " << mic_glyph << "\n"
      << "mic_muted_glyph = " << mic_muted_glyph << "\n"
      << "mic_click = " << mic_click << "\n"
      << "show_screenrecord = " << (show_screenrecord ? "true" : "false") << "\n"
      << "screenrecord_glyph = " << screenrecord_glyph << "\n"
      << "screenrecord_procs = " << screenrecord_procs << "\n"
      << "screenrecord_stop = " << screenrecord_stop << "\n"
      << "show_brightness = " << (show_brightness ? "true" : "false") << "\n"
      << "show_display = " << (show_display ? "true" : "false") << "\n"
      << "show_media = " << (show_media ? "true" : "false") << "\n"
      << "show_caffeine = " << (show_caffeine ? "true" : "false") << "\n"
      << "show_nightlight = " << (show_nightlight ? "true" : "false") << "\n"
      << "nightlight_on_k = " << nightlight_on_k << "\n"
      << "nightlight_off_k = " << nightlight_off_k << "\n"
      << "show_weather = " << (show_weather ? "true" : "false") << "\n"
      << "show_active_window = " << (show_active_window ? "true" : "false")
      << "\n"
      << "active_window_max = " << active_window_max << "\n"
      << "show_kblayout = " << (show_kblayout ? "true" : "false") << "\n"
      << "show_reminder = " << (show_reminder ? "true" : "false") << "\n"
      << "show_dictation = " << (show_dictation ? "true" : "false") << "\n"
      << "power_show_pct = " << (power_show_pct ? "true" : "false") << "\n"
      << "show_tailscale = " << (show_tailscale ? "true" : "false") << "\n"
      << "show_dropbox = " << (show_dropbox ? "true" : "false") << "\n"
      << "weather_show_temp = " << (weather_show_temp ? "true" : "false") << "\n"
      << "weather_unit = " << weather_unit << "\n"
      << "weather_refresh_min = " << weather_refresh_min << "\n"
      << "shell_weather_font_size = "
      << static_cast<int>(shell_weather_font_size) << "\n"
      << "shell_agents_font_size = "
      << static_cast<int>(shell_agents_font_size) << "\n"
      << "enable_notifications = " << (enable_notifications ? "true" : "false") << "\n"
      << "enable_osd = " << (enable_osd ? "true" : "false") << "\n"
      << "notification_timeout_s = " << notification_timeout_s << "\n"
      << "notifications_takeover = " << (notifications_takeover ? "true" : "false") << "\n"
      << "quickshell_shutdown = " << (quickshell_shutdown ? "true" : "false") << "\n"
      << "# qs_plugins: optional Quickshell sidecar for user plugins while\n"
      << "# takeover is on. Off keeps qs dead. Sidecar starts only with at\n"
      << "# least one plugin on the bar or in qs_plugin_services.\n"
      << "qs_plugins = " << (qs_plugins ? "true" : "false") << "\n"
      << "show_plugins = " << (show_plugins ? "true" : "false") << "\n"
      << "qs_plugin_layout = " << qs_plugin_layout << "\n"
      << "qs_plugin_services = " << qs_plugin_services << "\n"
      << "idle_blank_s = " << idle_blank_s << "\n"
      << "shell_font_size = " << static_cast<int>(shell_font_size) << "\n"
      << "shell_audio_font_size = " << static_cast<int>(shell_audio_font_size) << "\n"
      << "shell_network_font_size = " << static_cast<int>(shell_network_font_size) << "\n"
      << "shell_bluetooth_font_size = " << static_cast<int>(shell_bluetooth_font_size) << "\n"
      << "shell_display_font_size = " << static_cast<int>(shell_display_font_size) << "\n"
      << "shell_clipboard_font_size = " << static_cast<int>(shell_clipboard_font_size) << "\n"
      << "shell_emoji_font_size = " << static_cast<int>(shell_emoji_font_size) << "\n"
      << "shell_image_font_size = " << static_cast<int>(shell_image_font_size) << "\n"
      << "shell_panel_width = " << shell_panel_width << "\n"
      << "shell_panel_height = " << shell_panel_height << "\n"
      << "shell_overlay_width = " << shell_overlay_width << "\n"
      << "shell_overlay_height = " << shell_overlay_height << "\n"
      << "shell_audio_step = " << shell_audio_step << "\n"
      << "shell_audio_show_apps = " << (shell_audio_show_apps ? "true" : "false") << "\n"
      << "shell_audio_show_pct = " << (shell_audio_show_pct ? "true" : "false") << "\n"
      << "shell_wifi_scan_on_open = " << (shell_wifi_scan_on_open ? "true" : "false") << "\n"
      << "shell_bt_scan_on_open = " << (shell_bt_scan_on_open ? "true" : "false") << "\n"
      << "shell_clipboard_limit = " << shell_clipboard_limit << "\n"
      << "shell_clipboard_paste = " << (shell_clipboard_paste ? "true" : "false") << "\n"
      << "shell_emoji_insert = " << (shell_emoji_insert ? "true" : "false") << "\n"
      << "shell_image_show_labels = " << (shell_image_show_labels ? "true" : "false") << "\n"
      << "osd_volume = " << (osd_volume ? "true" : "false") << "\n"
      << "osd_mic = " << (osd_mic ? "true" : "false") << "\n"
      << "osd_brightness = " << (osd_brightness ? "true" : "false") << "\n"
      << "notify_battery = " << (notify_battery ? "true" : "false") << "\n"
      << "notify_bluetooth = " << (notify_bluetooth ? "true" : "false") << "\n"
      << "osd_timeout_ms = " << osd_timeout_ms << "\n"
      << "notification_max_shown = " << notification_max_shown << "\n"
      << "notification_emoji_font = " << notification_emoji_font << "\n"
      << "show_volume = " << (show_volume ? "true" : "false") << "\n"
      << "show_battery = " << (show_battery ? "true" : "false") << "\n"
      << "show_omarchy = " << (show_omarchy ? "true" : "false") << "\n"
      << "show_update = " << (show_update ? "true" : "false") << "\n"
      << "show_temp = " << (show_temp ? "true" : "false") << "\n"
      << "# temp_sensor: auto or chip:label\n"
      << "temp_sensor = " << temp_sensor << "\n"
      << "temp_warn = " << temp_warn << "\n"
      << "show_notifications = " << (show_notifications ? "true" : "false")
      << "\n"
      << "show_power = " << (show_power ? "true" : "false") << "\n"
      << "# clock_format: any strftime string\n"
      << "clock_format = " << clock_format << "\n"
      << "clock_format_vertical = " << clock_format_vertical << "\n"
      << "battery_show_time = " << (battery_show_time ? "true" : "false")
      << "\n"
      << "volume_headset_indicator = "
      << (volume_headset_indicator ? "true" : "false") << "\n"
      << "volume_headset_glyph = " << volume_headset_glyph << "\n"
      << "reveal_all_monitors = " << (reveal_all_monitors ? "true" : "false")
      << "\n"
      << "calendar_enabled = " << (calendar_enabled ? "true" : "false") << "\n"
      << "calendar_monday_first = "
      << (calendar_monday_first ? "true" : "false") << "\n"
      << "calendar_week_numbers = "
      << (calendar_week_numbers ? "true" : "false") << "\n"
      << "notify_glyph = " << notify_glyph << "\n"
      << "bell_show_count = " << (bell_show_count ? "true" : "false") << "\n"
      << "bell_dim_when_empty = " << (bell_dim_when_empty ? "true" : "false")
      << "\n"
      << "history_max = " << history_max << "\n"
      << "# muted_apps: notifications from these never pop up (still logged)\n"
      << "muted_apps = " << muted_apps << "\n"
      << "known_apps = " << known_apps << "\n"
      << "power_show_label = " << (power_show_label ? "true" : "false") << "\n"
      << "power_show_actions = " << (power_show_actions ? "true" : "false")
      << "\n"
      << "power_cmd_suspend = " << power_cmd_suspend << "\n"
      << "power_cmd_hibernate = " << power_cmd_hibernate << "\n"
      << "power_cmd_restart = " << power_cmd_restart << "\n"
      << "power_cmd_shutdown = " << power_cmd_shutdown << "\n"
      << "power_glyph_saver = " << power_glyph_saver << "\n"
      << "power_glyph_balanced = " << power_glyph_balanced << "\n"
      << "power_glyph_perf = " << power_glyph_perf << "\n"
      << "layout_left = " << layout_left << "\n"
      << "layout_center = " << layout_center << "\n"
      << "layout_right = " << layout_right << "\n"
      << "layout_more = " << layout_more << "\n"
      << "network_click = " << network_click << "\n"
      << "volume_click = " << volume_click << "\n"
      << "bluetooth_click = " << bluetooth_click << "\n"
      << "bluetooth_glyph = " << bluetooth_glyph << "\n"
      << "brightness_glyph = " << brightness_glyph << "\n"
      << "brightness_step = " << brightness_step << "\n"
      << "media_show_artist = " << (media_show_artist ? "true" : "false") << "\n"
      << "media_len = " << media_len << "\n"
      << "bluetooth_show_name = " << (bluetooth_show_name ? "true" : "false") << "\n"
      << "bluetooth_show_battery = " << (bluetooth_show_battery ? "true" : "false") << "\n"
      << "bluetooth_show_count = " << (bluetooth_show_count ? "true" : "false") << "\n"
      << "bluetooth_name_len = " << bluetooth_name_len << "\n"
      << "omarchy_glyph = " << omarchy_glyph << "\n"
      << "omarchy_font = " << omarchy_font << "\n"
      << "omarchy_click = " << omarchy_click << "\n"
      << "omarchy_right_click = " << omarchy_right_click << "\n"
      << "update_glyph = " << update_glyph << "\n"
      << "update_check = " << update_check << "\n"
      << "update_click = " << update_click << "\n"
      << "update_interval_s = " << update_interval_s << "\n"
      << "update_signal = " << update_signal << "\n";
}


// ---------------------------------------------------------------------------
// Module layout operations
// ---------------------------------------------------------------------------
static const char* KNOWN_MODULES[] = {"omarchy",   "workspaces", "clock",
                                      "pin",       "tray",       "more",
                                      "update",
                                      "temp",      "media",      "network",    "bluetooth",
                                      "display", "brightness",
                                      "caffeine",  "nightlight", "weather",
                                      "notifications",
                                      "power",
                                      "volume",    "battery",
                                      "agents",    "microphone",
                                      "screenrecord",
                                      "kblayout", "activewindow",
                                      "reminder", "dictation",
                                      "tailscale", "dropbox",
                                      "plugins"};

static std::vector<std::string> split_list(const std::string& s) {
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string tok;
    while (std::getline(ss, tok, ',')) {
        tok = trim(tok);
        if (!tok.empty()) out.push_back(tok);
    }
    return out;
}

static std::string join_list(const std::vector<std::string>& v) {
    std::string out;
    for (auto& s : v) {
        if (!out.empty()) out += ",";
        out += s;
    }
    return out;
}

// --- generic CSV lists (monitors, muted_apps, known_apps) ------------------
std::vector<std::string> Config::csv_split(const std::string& s) {
    return split_list(s);
}
std::string Config::csv_join(const std::vector<std::string>& v) {
    return join_list(v);
}

// Should this output carry a bar? Single-monitor mode keeps the old rule
// (cfg.output, or "wherever the compositor puts it" when empty).
bool Config::wants_monitor(const std::string& name) const {
    if (!multi_monitor) return output.empty() || name == output;
    if (monitors.empty()) return true; // every connected output
    for (auto& m : csv_split(monitors))
        if (m == name) return true;
    return false;
}

bool Config::app_muted(const std::string& app) const {
    if (app.empty()) return false;
    for (auto& a : csv_split(muted_apps))
        if (a == app) return true;
    return false;
}

void Config::set_app_muted(const std::string& app, bool muted) {
    if (app.empty()) return;
    auto v = csv_split(muted_apps);
    auto it = std::find(v.begin(), v.end(), app);
    if (muted && it == v.end()) v.push_back(app);
    else if (!muted && it != v.end()) v.erase(it);
    else return;
    muted_apps = join_list(v);
    save();
}

// Remember every app that has ever notified us, so the settings window can
// offer a mute checkbox for it even when it isn't currently running.
void Config::note_app(const std::string& app) {
    if (app.empty() || app.find(',') != std::string::npos) return;
    auto v = csv_split(known_apps);
    if (std::find(v.begin(), v.end(), app) != v.end()) return;
    v.push_back(app);
    std::sort(v.begin(), v.end());
    if (v.size() > 40) v.resize(40);
    known_apps = join_list(v);
    save();
}

std::vector<std::string> Config::layout_get(int zone) const {
    if (zone == 0) return split_list(layout_left);
    if (zone == 1) return split_list(layout_center);
    if (zone == 2) return split_list(layout_right);
    return split_list(layout_more);
}

void Config::layout_set(int zone, const std::vector<std::string>& v) {
    std::string joined = join_list(v);
    if (zone == 0) layout_left = joined;
    else if (zone == 1) layout_center = joined;
    else if (zone == 2) layout_right = joined;
    else layout_more = joined;
}

int Config::layout_zone_of(const std::string& id, int* idx) const {
    for (int z = 0; z < 4; ++z) {
        auto v = layout_get(z);
        for (size_t i = 0; i < v.size(); ++i)
            if (v[i] == id) {
                if (idx) *idx = static_cast<int>(i);
                return z;
            }
    }
    return -1;
}

void Config::layout_move(const std::string& id, int delta) {
    int idx = -1;
    int z = layout_zone_of(id, &idx);
    if (z < 0) return;
    auto v = layout_get(z);
    int to = idx + delta;
    if (to < 0 || to >= static_cast<int>(v.size())) return;
    std::swap(v[idx], v[to]);
    layout_set(z, v);
}

void Config::layout_set_zone(const std::string& id, int zone) {
    int cur = layout_zone_of(id);
    if (cur == zone || zone < 0 || zone > 3) return;
    // More cannot nest in itself. Plugins is a different accordion and
    // is allowed in More, same as volume/clock/etc.
    if (id == "more" && zone == 3) return;
    if (cur >= 0) {
        auto v = layout_get(cur);
        v.erase(std::remove(v.begin(), v.end(), id), v.end());
        layout_set(cur, v);
    }
    auto v = layout_get(zone);
    v.push_back(id);
    layout_set(zone, v);
}

void Config::layout_normalize() {
    // Older builds placed each qs:<id> on the bar. Fold those into the
    // Plugins accordion list so the bar only hosts one Plugins chip.
    for (int z = 0; z < 4; ++z) {
        auto v = layout_get(z);
        std::vector<std::string> keep;
        bool ch = false;
        for (auto& id : v) {
            if (qs_is_module_id(id)) {
                qs_plugin_set_shown(qs_plugin_id_of(id), true);
                ch = true;
                continue;
            }
            keep.push_back(id);
        }
        if (ch) layout_set(z, keep);
    }
    auto known = [](const std::string& id) {
        for (auto* k : KNOWN_MODULES)
            if (id == k) return true;
        return false;
    };
    std::vector<std::string> seen;
    for (int z = 0; z < 4; ++z) {
        std::vector<std::string> clean;
        for (auto& id : layout_get(z)) {
            if (!known(id)) continue; // typo or removed module
            if (id == "more" && z == 3) continue; // host cannot nest
            if (std::find(seen.begin(), seen.end(), id) != seen.end())
                continue; // duplicate
            seen.push_back(id);
            clean.push_back(id);
        }
        layout_set(z, clean);
    }
    // anything missing lands at the end of the right zone
    auto right = layout_get(2);
    for (auto* k : KNOWN_MODULES)
        if (std::find(seen.begin(), seen.end(), k) == seen.end())
            right.push_back(k);
    layout_set(2, right);
}

static void parse_agents_popup_pct(const std::string& s, int& w, int& h) {
    w = 36;
    h = 44;
    int a = 0, b = 0;
    if (sscanf(s.c_str(), "%d%% %d%%", &a, &b) == 2) {
        w = a;
        h = b;
    }
}

int Config::agents_popup_w_pct() const {
    int w, h;
    parse_agents_popup_pct(agents_popup_size, w, h);
    return w;
}

int Config::agents_popup_h_pct() const {
    int w, h;
    parse_agents_popup_pct(agents_popup_size, w, h);
    return h;
}

void Config::set_agents_popup_pct(int w, int h) {
    w = std::clamp(w, 20, 80);
    h = std::clamp(h, 20, 90);
    agents_popup_size = std::to_string(w) + "% " + std::to_string(h) + "%";
}

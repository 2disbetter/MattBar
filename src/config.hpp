#pragma once
// --------------------------------------------------------------------------- MattBar runtime configuration, loaded from ~/.config/mattbar/mattbar.conf and editable live through the built-in settings window (tray gear icon).
#include <string>

inline constexpr const char* MATTBAR_VERSION = "1.28.0";
#include <vector>

struct Color { double r, g, b, a; };

struct Config {
    // Geometry & timing
    std::string position = "top"; // top | bottom | left | right
    int bar_height     = 34;  // px, bar thickness when horizontal
    int vertical_width = 72;  // px, bar thickness when vertical
    int strip_height = 2;    // px, VISIBLE strip line when hidden
    int strip_hit_height = 8; // px, invisible hover zone that reveals the bar
    std::string output;       // monitor name ("DP-2"); empty = compositor picks

    // --- Multi-monitor ------------------------------------------------------ Off by default: exactly the old behaviour (one bar, on `output` or wherever the compositor puts it).
    bool        multi_monitor  = false;
    std::string monitors;        // CSV of output names; empty = all outputs
    std::string primary_output;  // which one hosts tray/settings/popups
    int hide_delay_ms    = 500;   // pointer-leave -> hide
    int reveal_delay_ms  = 0;     // dwell time in the hot zone before reveal
                                  // (0 = instant); prevents edge-brush misfires
    int tick_ms          = 1000;  // module refresh while visible
    int tray_collapse_ms = 4000;  // tray auto-collapse; 0 = never
    bool tray_start_expanded = false;

    // Font (CaskaydiaMono Nerd Font is the Omarchy default; glyphs below assume a Nerd Font.
    std::string font = "JetBrainsMono Nerd Font";
    double font_size = 13.0;

    // Colors. c_* are the EFFECTIVE colors all rendering reads. Normally they equal the user's own palette (u_* below); whe...
    Color c_bg     {0.071, 0.075, 0.094, 0.94}; // bar background
    Color c_strip  {0.286, 0.560, 0.960, 0.35}; // hidden hot strip
    Color c_fg     {0.870, 0.880, 0.910, 1.00}; // normal text
    Color c_dim    {0.520, 0.545, 0.600, 1.00}; // inactive text
    Color c_accent {0.286, 0.560, 0.960, 1.00}; // active workspace / pin
    Color c_urgent {0.940, 0.380, 0.360, 1.00}; // low battery
    Color c_ws_bg  {0.150, 0.160, 0.200, 1.00}; // pills / menu hover

    bool follow_omarchy_theme = false; // colors track the Omarchy theme
    Color u_bg = c_bg, u_strip = c_strip, u_fg = c_fg, u_dim = c_dim,
          u_accent = c_accent, u_urgent = c_urgent, u_ws_bg = c_ws_bg;
    void colors_snapshot();
    void resolve_click_defaults(); // Omarchy 3.x vs Quattro click adaptation
    // u_* = c_*  (before an Omarchy overlay)
    void colors_restore();  // c_* = u_*  (when the toggle turns off)

    // Module toggles (live)
    bool show_workspaces = true, show_clock = true, show_pin = true,
         show_tray = true, show_network = true, show_volume = true,
         show_battery = true, show_omarchy = true, show_update = true,
         show_temp = true, show_bluetooth = true, show_media = true,
         show_notifications = true, show_power = true;

    // Temperature module: "auto" (CPU package) or a "chip:label" sensor id
    std::string temp_sensor = "auto";
    int temp_warn = 85; // degrees C at which the reading turns urgent-red

    // Omarchy integration (mirrors Omarchy's Waybar custom modules)
    std::string omarchy_glyph = "\ue900";       // Omarchy logo (private font)
    std::string omarchy_font  = "omarchy";
    std::string omarchy_click = "omarchy-menu";
    std::string omarchy_right_click = "xdg-terminal-exec";
    // Click commands: stock values auto-adapt to Omarchy 3.x wrappers or Omarchy 4 (Quattro) shell panels, whichever is installed.
    std::string network_click = "omarchy-launch-wifi";  // left-click on WiFi
    std::string volume_click  = "omarchy-launch-audio"; // left-click on volume
    std::string bluetooth_click = "omarchy-launch-bluetooth"; // left-click on bt
    // Bluetooth rune from the Nerd Font range, same assumption the update glyph already makes (Omarchy's bar fonts are Nerd Fonts).
    std::string bluetooth_glyph = "\uf294";
    // What the bluetooth module shows beyond the icon (settings-controlled)
    bool bluetooth_show_name    = true;  // connected device's name
    bool bluetooth_show_battery = true;  // its battery %, when reported
    bool bluetooth_show_count   = true;  // "+N" for extra connections
    int  bluetooth_name_len     = 18;    // name truncation, in characters
    bool        show_brightness  = true;      // hidden without a backlight
    // "auto" = MattBar draws its own vector sun (distinct from the tray's gear at any size/font).
    std::string brightness_glyph = "auto";
    int         brightness_step  = 5;         // scroll step in percent
    // Media module display (settings: Media)
    bool media_show_artist = true; // "Artist - Title" vs title only
    int  media_len         = 30;   // label truncation, in characters
    bool enable_notifications = false; // own org.freedesktop.Notifications
    bool enable_osd           = false; // volume/brightness flash
    int  notification_timeout_s = 5;
    // When enabling the daemon, terminate the current owner of org.freedesktop.Notifications (mako etc.) once, so MattBar's queued claim promotes.
    bool notifications_takeover  = true;
    // Per-type feedback toggles (settings: Notifications & OSD)
    bool osd_volume       = true;  // sink volume/mute OSD
    bool osd_mic          = true;  // microphone volume/mute OSD
    bool osd_brightness   = true;  // backlight OSD
    bool notify_battery   = true;  // low/critical battery alerts
    bool notify_bluetooth = true;  // device connect/disconnect alerts
    // Transport watcher: when a BlueZ-connected audio device has no PipeWire sink after a grace period (the AVDTP-collision...
    bool bt_auto_heal = false;
    // Agents module (Omarchy Quattro AI-usage records; hides without them)
    bool        show_agents    = true;
    int         agents_warn_pct = 90;         // urgent color at/above this
    std::string agents_glyph   = "\U000F06A9"; // nf-md-robot (v3); falls
                                                // back per-font if unmapped
    std::string agents_click   = "omarchy-shell shell toggle omarchy.agents";
    // Terminal-session popup for agents the shell's usage panel cannot represent (e.g.
    std::string agents_term =
        "alacritty --class mattbar-agent -o window.dimensions.columns=110 "
        "-o window.dimensions.lines=30 -e omarchy-agent --inline";
    std::string agents_term_class = "mattbar-agent";
    // Popup footprint (Hyprland float size, "W% H%" of the monitor).
    std::string agents_popup_size = "36% 44%";
    // Flip Hyprland's input:special_fallthrough so clicks outside the popup reach the windows beneath (and dismiss it) inst...
    bool agents_click_through = true;
    // Microphone module (default-source mute/volume; bluez rune = real mic)
    bool        show_microphone = true;
    bool        mic_show_pct    = false;
    std::string mic_glyph       = "\uf130";  // Nerd Font microphone
    std::string mic_muted_glyph = "\uf131";  // slashed variant
    std::string mic_click       = "omarchy-launch-audio"; // adaptive, see
                                                          // resolve_click Screen-recording indicator (red light while a recorder runs)
    bool        show_screenrecord = true;
    std::string screenrecord_glyph = "\uf192"; // record dot
    std::string screenrecord_procs =
        "gpu-screen-recorder,wf-recorder"; // argv[0] prefixes to match
    std::string screenrecord_stop =
        "omarchy-capture-screenrecording --stop-recording";
    // Timing (settings: Notifications & OSD).
    int osd_timeout_ms         = 1500; // volume/mic/brightness OSD linger
    int notification_max_shown = 5;    // stacked popups before overflow
    // Font used for codepoints the bar font lacks in notification text (emoji).
    std::string notification_emoji_font = "Noto Color Emoji";
    bool show_caffeine        = true;  // idle-inhibit toggle module
    std::string update_glyph = "\uf021";        // Nerd Font refresh icon
    std::string update_check = "omarchy-update-available";
    std::string update_click =
        "omarchy-launch-floating-terminal-with-presentation omarchy-update";
    int update_interval_s = 21600;              // 6 h, like Omarchy's Waybar
    int update_signal     = 7;                  // refresh on SIGRTMIN+7

    // Module layout: comma-separated module ids per zone (0=L, 1=C, 2=R).
    std::string layout_left   = "omarchy,workspaces";
    std::string layout_center = "clock,pin";
    std::string layout_right  = "tray,update,agents,screenrecord,notifications,temp,media,network,bluetooth,brightness,microphone,volume,power,battery,caffeine";

    // --- Clock / calendar --------------------------------------------------- strftime format strings; settings offers pre...
    std::string clock_format          = "%a %d %b  %H:%M";
    std::string clock_format_vertical = "%H:%M";
    bool calendar_enabled      = true; // left-click the clock opens a month
    bool calendar_monday_first = true; // ISO week; false = Sunday first
    bool calendar_week_numbers = false;

    // --- Notification centre (bell module) ---------------------------------- Deliberately undemanding: the bell never gro...
    std::string notify_glyph      = "\uf0f3"; // nf-fa-bell
    bool        bell_show_count   = false;    // "3" next to the bell
    bool        bell_dim_when_empty = true;   // the only ambient signal
    int         history_max       = 30;       // notifications remembered
    // Per-app muting: a muted app's notifications never pop up, but are still recorded in history, so muting loses nothing.
    std::string muted_apps;  // CSV of app names
    std::string known_apps;  // CSV of apps seen so far (populates settings)

    // Battery: append a time estimate ("2h05") while charging/discharging, computed from the kernel's energy/power (or char...
    bool battery_show_time = false;

    // Volume: a headphone rune to the right of the percentage while the DEFAULT sink is a headset-class device (BT earbuds, wired headphones), Omarchy-Waybar-style.
    bool        volume_headset_indicator = true;
    std::string volume_headset_glyph     = "\uf025"; // nf-fa-headphones

    // Multi-monitor: hovering ANY bar reveals ALL of them (for people who treat their displays as one surface); they hide t...
    bool reveal_all_monitors = false;

    // --- Power profiles ----------------------------------------------------- power-profiles-daemon over D-Bus; the module...
    bool        power_show_label   = false;    // "Balanced" beside the glyph
    // Session actions under the profiles in the power popup.
    bool        power_show_actions = true;
    std::string power_cmd_suspend   = "systemctl suspend";
    std::string power_cmd_hibernate = "systemctl hibernate";
    std::string power_cmd_restart   = "systemctl reboot";
    std::string power_cmd_shutdown  = "systemctl poweroff";
    std::string power_glyph_saver  = "\uf06c"; // nf-fa-leaf
    std::string power_glyph_balanced = "\uf0e7"; // nf-fa-bolt
    std::string power_glyph_perf   = "\uf135"; // nf-fa-rocket

    // CSV list helpers (monitors, muted_apps, known_apps)
    static std::vector<std::string> csv_split(const std::string&);
    static std::string csv_join(const std::vector<std::string>&);
    bool wants_monitor(const std::string& name) const;
    bool app_muted(const std::string& app) const;
    void set_app_muted(const std::string& app, bool muted);
    void note_app(const std::string& app); // remember an app we've seen

    std::vector<std::string> layout_get(int zone) const;
    void layout_set(int zone, const std::vector<std::string>&);
    int  layout_zone_of(const std::string& id, int* idx = nullptr) const;
    void layout_move(const std::string& id, int delta);    // -1 up / +1 down
    void layout_set_zone(const std::string& id, int zone); // appends to zone
    void layout_normalize(); // every known id exactly once; junk removed

    void load();
    void save() const;
    static std::string path();
};

extern Config cfg;

// Readable label color for text drawn ON a colored fill (e.g.
inline Color contrast_on(const Color& c) {
    double lum = 0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b;
    return lum > 0.65 ? Color{0.09, 0.10, 0.13, 1.0}
                      : Color{1.0, 1.0, 1.0, 1.0};
}

// Orientation helpers usable by every module
inline bool cfg_vertical() {
    return cfg.position == "left" || cfg.position == "right";
}
inline int cfg_thickness() {
    return cfg_vertical() ? cfg.vertical_width : cfg.bar_height;
}

// Fixed layout constants (not user-facing)
inline constexpr int SIDE_PADDING   = 10;
inline constexpr int MODULE_GAP     = 18;
inline constexpr int TRAY_ICON_SIZE = 20;
inline constexpr int TRAY_ICON_GAP  = 8;

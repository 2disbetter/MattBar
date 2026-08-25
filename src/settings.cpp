#include "settings.hpp"
#include "bar.hpp"
#include "config.hpp"
#include "idle.hpp"
#include "modules.hpp"
#include "omarchy_theme.hpp"
#include "qs_plugins.hpp"
#include "sensors.hpp"
#include "shell.hpp"
#include "shm.hpp"

#include <cairo/cairo.h>
#include <linux/input-event-codes.h>

#include <algorithm>
#include <string>

namespace {
void col(cairo_t* cr, const Color& c, double a = -1) {
    cairo_set_source_rgba(cr, c.r, c.g, c.b, a < 0 ? c.a : a);
}
constexpr double HEADER_H   = 52;
constexpr double FOOTER_H   = 58;
constexpr double SCROLL_STEP = 40;
constexpr int TabBar     = 0;
constexpr int TabLayout  = 1;
constexpr int TabModules = 2;
constexpr int TabNotify  = 3;
constexpr int TabLook    = 4;
constexpr int TabShell   = 5;
constexpr int ShellIdle    = 0;
constexpr int ShellMenu    = 1;
constexpr int ShellPanels  = 2;
constexpr int ShellPickers = 3;
constexpr int ShellDesktop = 4;
constexpr int ShellPlugins = 5;
} // namespace

SettingsWindow::SettingsWindow(Bar& bar) : bar_(bar) {
    surf_ = wl_compositor_create_surface(bar_.compositor());
    frac_.on_change = [this] { draw(); };
    frac_.attach(bar_.frac_mgr(), bar_.viewporter(), surf_);
    // The settings window is a singleton: it opens on the primary bar's
    // monitor, so it can't appear twice or land on a monitor you aren't
    // looking at.
    ls_ = zwlr_layer_shell_v1_get_layer_surface(
        bar_.layer_shell(), surf_, bar_.primary_output(),
        ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "mattbar-settings");
    static const zwlr_layer_surface_v1_listener ls_listener = {
        .configure = on_configure,
        .closed    = on_closed,
    };
    zwlr_layer_surface_v1_add_listener(ls_, &ls_listener, this);
    // no anchors -> compositor centers the surface on the output
    zwlr_layer_surface_v1_set_size(ls_, w_, h_);
    zwlr_layer_surface_v1_set_exclusive_zone(ls_, 0);
    zwlr_layer_surface_v1_set_keyboard_interactivity(ls_, 0);

    bar_.register_surface(
        surf_, Bar::SurfaceHooks{
                   .motion = [this](double x, double y) { on_motion(x, y); },
                   .button = [this](int b) { on_button(b); },
                   .leave  = [this] { mx_ = my_ = -1; },
                   .scroll = [this](int d) { on_scroll(d); },
               });
    wl_surface_commit(surf_);
}

SettingsWindow::~SettingsWindow() {
    bar_.unregister_surface(surf_);
    if (ls_) zwlr_layer_surface_v1_destroy(ls_);
    frac_.destroy();
    if (surf_) wl_surface_destroy(surf_);
}

void SettingsWindow::on_configure(void* data, zwlr_layer_surface_v1* ls,
                                  uint32_t serial, uint32_t, uint32_t) {
    auto* self = static_cast<SettingsWindow*>(data);
    zwlr_layer_surface_v1_ack_configure(ls, serial);
    self->mapped_ = true;
    self->draw();
}

void SettingsWindow::on_closed(void* data, zwlr_layer_surface_v1*) {
    static_cast<SettingsWindow*>(data)->bar_.close_settings_later();
}

void SettingsWindow::on_motion(double x, double y) {
    mx_ = x;
    my_ = y;
}

void SettingsWindow::on_scroll(int dir) {
    double next = std::clamp(scroll_ + dir * SCROLL_STEP, 0.0, scroll_max_);
    if (next != scroll_) {
        scroll_ = next;
        draw();
    }
}

void SettingsWindow::on_button(int button) {
    if (button != BTN_LEFT) return;
    for (size_t i = 0; i < widgets_.size(); ++i) {
        Widget wgt = widgets_[i]; // copy: callbacks rebuild widgets_
        if (mx_ < wgt.x || mx_ >= wgt.x + wgt.w || my_ < wgt.y ||
            my_ >= wgt.y + wgt.h)
            continue;
        if (wgt.set_frac) {
            double f = std::clamp((mx_ - wgt.x) / wgt.w, 0.0, 1.0);
            wgt.set_frac(f);
        } else if (wgt.click) {
            wgt.click();
        }
        return;
    }
}

void SettingsWindow::apply() {
    bar_.apply_config();
    cfg.save(); // settings persist automatically; no Save button needed
    draw();
}

void SettingsWindow::draw() {
    if (!mapped_) return;
    if (tab_ == TabShell && !cfg.quickshell_shutdown) tab_ = TabBar;
    if (tab_ < TabBar || tab_ > TabShell) tab_ = TabBar;
    void* data = nullptr;
    // HiDPI: buffer at the primary output's scale (that is where this
    // window opens); widget hit rects and all layout stay logical.
    const int sc = wl_surface_get_version(surf_) >= 3
                       ? bar_.scale_of(bar_.primary_output())
                       : 1;
    const int bw = frac_.active() ? frac_.px(w_) : w_ * sc;
    const int bh = frac_.active() ? frac_.px(h_) : h_ * sc;
    wl_buffer* buffer = create_argb_buffer(bar_.shm(), bw, bh, &data);
    if (!buffer) return;
    widgets_.clear();

    cairo_surface_t* cs = cairo_image_surface_create_for_data(
        static_cast<unsigned char*>(data), CAIRO_FORMAT_ARGB32, bw, bh,
        bw * 4);
    cairo_t* cr = cairo_create(cs);
    cairo_scale(cr, (double)bw / w_, (double)bh / h_);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    col(cr, cfg.c_bg, 0.98);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    cairo_select_font_face(cr, cfg.font.c_str(), CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, cfg.font_size);
    cairo_font_extents_t fe;
    cairo_font_extents(cr, &fe);

    struct Tab {
        const char* lbl;
        int         id;
    };
    Tab tabs[6];
    int ntabs = 0;
    auto add_tab = [&](const char* l, int id) { tabs[ntabs++] = {l, id}; };
    add_tab("Bar", TabBar);
    add_tab("Layout", TabLayout);
    add_tab("Modules", TabModules);
    add_tab("Notify", TabNotify);
    add_tab("Look", TabLook);
    if (cfg.quickshell_shutdown) add_tab("Shell", TabShell);
    Tab shells[6];
    int nshell = 0;
    auto add_shell = [&](const char* l, int id) { shells[nshell++] = {l, id}; };
    if (tab_ == TabShell && cfg.quickshell_shutdown) {
        add_shell("Idle", ShellIdle);
        add_shell("Menu", ShellMenu);
        add_shell("Panels", ShellPanels);
        add_shell("Pickers", ShellPickers);
        add_shell("Desktop", ShellDesktop);
        add_shell("Plugins", ShellPlugins);
        if (shell_tab_ < 0 || shell_tab_ >= nshell) shell_tab_ = 0;
    }
    auto tab_label_w = [&](const char* s) {
        cairo_text_extents_t ext;
        cairo_text_extents(cr, s, &ext);
        return ext.x_advance + 16;
    };
    auto count_rows = [&](Tab* ts, int n) {
        int    rows = 1;
        double tx   = 24;
        for (int i = 0; i < n; ++i) {
            double twid = tab_label_w(ts[i].lbl);
            if (tx + twid > w_ - 12 && tx > 24) {
                ++rows;
                tx = 24;
            }
            tx += twid + 6;
        }
        return rows;
    };
    int    tab_rows   = count_rows(tabs, ntabs);
    int    shell_rows = nshell ? count_rows(shells, nshell) : 0;
    const double TAB_H = 22;
    const double hdr   = 44 + tab_rows * (TAB_H + 4) +
                       shell_rows * (TAB_H + 4) + 8;

    // Widget hit rects are recorded in SURFACE coordinates. woff_ maps
    // content-local y -> surface y during the scrolled pass; widgets fully
    // outside the visible band are not clickable.
    auto add_widget = [&](double x, double y, double ww, double wh,
                          std::function<void()> click,
                          std::function<void(double)> frac) {
        double sy = y + woff_;
        if (sy + wh < hdr - 4 || sy > h_ - FOOTER_H + 4) return;
        widgets_.push_back({x, sy, ww, wh, std::move(click), std::move(frac)});
    };

    auto text = [&](double x, double y, const std::string& s, const Color& c) {
        col(cr, c, 1.0);
        cairo_move_to(cr, x, y + (fe.ascent - fe.descent) / 2.0);
        cairo_show_text(cr, s.c_str());
    };
    auto text_w = [&](const std::string& s) {
        cairo_text_extents_t ext;
        cairo_text_extents(cr, s.c_str(), &ext);
        return ext.x_advance;
    };
    auto small_button = [&](double x, double y, const std::string& label,
                            std::function<void()> cb) {
        double bw = 22, bh = 22;
        col(cr, cfg.c_ws_bg);
        cairo_rectangle(cr, x, y - bh / 2, bw, bh);
        cairo_fill(cr);
        text(x + (bw - text_w(label)) / 2.0, y, label, cfg.c_fg);
        add_widget(x, y - bh / 2, bw, bh, std::move(cb), nullptr);
    };
    auto stepper = [&](double y, const std::string& label,
                       const std::string& value, std::function<void(int)> adj) {
        text(24, y, label, cfg.c_fg);
        text(310 - 12 - text_w(value), y, value, cfg.c_dim);
        small_button(318, y, "-", [adj, this] { adj(-1); apply(); });
        small_button(318 + 30, y, "+", [adj, this] { adj(+1); apply(); });
    };
    auto checkbox = [&](double x, double y, const std::string& label,
                        bool* flag) {
        double bs = 15;
        col(cr, cfg.c_ws_bg);
        cairo_rectangle(cr, x, y - bs / 2, bs, bs);
        cairo_fill(cr);
        if (*flag) {
            col(cr, cfg.c_accent, 1.0);
            cairo_rectangle(cr, x + 3, y - bs / 2 + 3, bs - 6, bs - 6);
            cairo_fill(cr);
        }
        text(x + bs + 8, y, label, cfg.c_fg);
        add_widget(x, y - bs / 2, bs + 8 + text_w(label), bs,
                   [flag, this] {
                       *flag = !*flag;
                       apply();
                   },
                   nullptr);
    };
    auto slider = [&](double y, const std::string& label, double v,
                      std::function<void(double)> set) {
        text(24, y, label, cfg.c_fg);
        double sx = 90, sw = 250, sh = 6;
        col(cr, cfg.c_ws_bg);
        cairo_rectangle(cr, sx, y - sh / 2, sw, sh);
        cairo_fill(cr);
        col(cr, cfg.c_accent, 1.0);
        cairo_rectangle(cr, sx, y - sh / 2, sw * v, sh);
        cairo_fill(cr);
        cairo_arc(cr, sx + sw * v, y, 6, 0, 2 * M_PI);
        cairo_fill(cr);
        add_widget(sx, y - 12, sw, 24, nullptr, [set, this](double f) {
            set(f);
            apply();
        });
        char pct[8];
        snprintf(pct, sizeof pct, "%d", static_cast<int>(v * 255 + 0.5));
        text(sx + sw + 14, y, pct, cfg.c_dim);
    };
    auto section = [&](double y, const std::string& s) {
        text(24, y, s, cfg.c_accent);
        col(cr, cfg.c_ws_bg);
        cairo_move_to(cr, 24 + text_w(s) + 10, y);
        cairo_line_to(cr, w_ - 24, y);
        cairo_stroke(cr);
    };
    // Dim helper lines, word-wrapped to the window. Returns the y of the
    // LAST line drawn, so single-line hints advance exactly as `text` did
    // and longer ones (or bigger fonts) just push what follows down —
    // scroll_max_ derives from the final y, so the scrollbar keeps up.
    auto hint = [&](double y, const std::string& s,
                    double x = 24.0) -> double {
        const double maxw = w_ - x - 24;
        std::string line, word;
        auto flushword = [&] {
            if (word.empty()) return;
            std::string cand = line.empty() ? word : line + " " + word;
            if (!line.empty() && text_w(cand) > maxw) {
                text(x, y, line, cfg.c_dim);
                y += 20;
                line = word;
            } else {
                line = cand;
            }
            word.clear();
        };
        for (char c : s) {
            if (c == ' ') flushword();
            else word += c;
        }
        flushword();
        if (!line.empty()) text(x, y, line, cfg.c_dim);
        return y;
    };

    // ---- scrolled content pass -------------------------------------------
    const double visible_h = h_ - hdr - FOOTER_H;
    woff_ = hdr - scroll_;
    cairo_save(cr);
    cairo_rectangle(cr, 0, hdr, w_, visible_h);
    cairo_clip(cr);
    cairo_translate(cr, 0, woff_);

    double y = 18; // content-local coordinates
    if (tab_ == TabShell && cfg.quickshell_shutdown) {
        auto fmt_idle = [](int s) -> std::string {
            if (s <= 0) return "off";
            if (s < 60) return std::to_string(s) + " s";
            int m = s / 60, r = s % 60;
            if (r == 0) return std::to_string(m) + " min";
            return std::to_string(m) + " min " + std::to_string(r) + " s";
        };

        if (shell_tab_ == ShellIdle) {
        section(y, "Idle");
        y += 30;
        stepper(y, "Screensaver", fmt_idle(idle_screensaver_s()), [](int d) {
            idle_set_screensaver_s(idle_screensaver_s() + d * 15);
        });
        y += 30;
        stepper(y, "Lock", fmt_idle(idle_lock_s()), [](int d) {
            idle_set_lock_s(idle_lock_s() + d * 15);
        });
        y += 30;
        stepper(y, "Display off after lock", fmt_idle(cfg.idle_blank_s),
                [](int d) {
                    cfg.idle_blank_s =
                        std::clamp(cfg.idle_blank_s + d, 0, 60);
                });
        y += 24;
        y = hint(y, "from last input: screensaver, then lock, then backlight off");
        y += 26;
        y = hint(y, "screensaver and lock are Omarchy's idle.screensaver / idle.lock");
        y += 26;
        y = hint(y, "display off matches Quickshell's 5 s blank; 0 keeps the panel lit");
        y += 12;
        }

        if (shell_tab_ == ShellMenu) {
        section(y, "Launcher");
        y += 30;
        stepper(y, "Font size",
                std::to_string(static_cast<int>(cfg.shell_font_size)),
                [](int d) {
                    cfg.shell_font_size =
                        std::clamp(cfg.shell_font_size + d, 9.0, 28.0);
                });
        y += 24;
        y = hint(y, "Super+Space menu labels; icons scale with this");
        y += 26;
        y = hint(y, "the bar font is still used; this is size only");
        y += 26;
        y = hint(y, "each panel below has its own size — dense TUIs default smaller");
        y += 12;
        }

        if (shell_tab_ == ShellPanels) {
        section(y, "Panels");
        y += 30;
        stepper(y, "Panel width", std::to_string(cfg.shell_panel_width),
                [](int d) {
                    cfg.shell_panel_width =
                        std::clamp(cfg.shell_panel_width + d * 20, 280, 720);
                });
        y += 30;
        stepper(y, "Panel height", std::to_string(cfg.shell_panel_height),
                [](int d) {
                    cfg.shell_panel_height =
                        std::clamp(cfg.shell_panel_height + d * 20, 300, 900);
                });
        y += 24;
        y = hint(y, "audio, network, bluetooth, display");
        y += 30;
        stepper(y, "Overlay width", std::to_string(cfg.shell_overlay_width),
                [](int d) {
                    cfg.shell_overlay_width =
                        std::clamp(cfg.shell_overlay_width + d * 20, 360, 960);
                });
        y += 30;
        stepper(y, "Overlay height", std::to_string(cfg.shell_overlay_height),
                [](int d) {
                    cfg.shell_overlay_height =
                        std::clamp(cfg.shell_overlay_height + d * 20, 300, 900);
                });
        y += 24;
        y = hint(y, "clipboard, emoji, image picker");
        y += 36;

        section(y, "Audio");
        y += 30;
        stepper(y, "Font size",
                std::to_string(static_cast<int>(cfg.shell_audio_font_size)),
                [](int d) {
                    cfg.shell_audio_font_size =
                        std::clamp(cfg.shell_audio_font_size + d, 9.0, 22.0);
                });
        y += 30;
        stepper(y, "Volume step %", std::to_string(cfg.shell_audio_step),
                [](int d) {
                    cfg.shell_audio_step =
                        std::clamp(cfg.shell_audio_step + d, 1, 20);
                });
        y += 30;
        checkbox(24, y, "Per-app mixer", &cfg.shell_audio_show_apps);
        y += 26;
        checkbox(24, y, "Show percentages", &cfg.shell_audio_show_pct);
        y += 36;

        section(y, "Network");
        y += 30;
        stepper(y, "Font size",
                std::to_string(static_cast<int>(cfg.shell_network_font_size)),
                [](int d) {
                    cfg.shell_network_font_size =
                        std::clamp(cfg.shell_network_font_size + d, 9.0, 22.0);
                });
        y += 30;
        checkbox(24, y, "Scan on open", &cfg.shell_wifi_scan_on_open);
        y += 24;
        y = hint(y, "left-click a network to connect; right-click forgets");
        y += 36;

        section(y, "Bluetooth");
        y += 30;
        stepper(y, "Font size",
                std::to_string(static_cast<int>(cfg.shell_bluetooth_font_size)),
                [](int d) {
                    cfg.shell_bluetooth_font_size =
                        std::clamp(cfg.shell_bluetooth_font_size + d, 9.0, 22.0);
                });
        y += 30;
        checkbox(24, y, "Scan on open", &cfg.shell_bt_scan_on_open);
        y += 24;
        y = hint(y, "left-click connect/disconnect; right-click forgets");
        y += 36;

        section(y, "Display");
        y += 30;
        stepper(y, "Font size",
                std::to_string(static_cast<int>(cfg.shell_display_font_size)),
                [](int d) {
                    cfg.shell_display_font_size =
                        std::clamp(cfg.shell_display_font_size + d, 9.0, 22.0);
                });
        y += 24;
        y = hint(y, "the panel chrome; TEXT SIZE in the panel still drives the desktop");
        y += 12;
        }

        if (shell_tab_ == ShellPickers) {
        section(y, "Clipboard");
        y += 30;
        stepper(y, "Font size",
                std::to_string(static_cast<int>(cfg.shell_clipboard_font_size)),
                [](int d) {
                    cfg.shell_clipboard_font_size =
                        std::clamp(cfg.shell_clipboard_font_size + d, 9.0, 22.0);
                });
        y += 30;
        stepper(y, "History limit", std::to_string(cfg.shell_clipboard_limit),
                [](int d) {
                    cfg.shell_clipboard_limit =
                        std::clamp(cfg.shell_clipboard_limit + d * 10, 20, 1000);
                });
        y += 30;
        checkbox(24, y, "Paste into focused window", &cfg.shell_clipboard_paste);
        y += 24;
        y = hint(y, "off: copy only. Ctrl+L or right-click to clear");
        y += 36;

        section(y, "Emoji");
        y += 30;
        stepper(y, "Font size",
                std::to_string(static_cast<int>(cfg.shell_emoji_font_size)),
                [](int d) {
                    cfg.shell_emoji_font_size =
                        std::clamp(cfg.shell_emoji_font_size + d, 9.0, 22.0);
                });
        y += 30;
        checkbox(24, y, "Insert into focused window", &cfg.shell_emoji_insert);
        y += 24;
        y = hint(y, "off: copy to clipboard only");
        y += 36;

        section(y, "Image picker");
        y += 30;
        stepper(y, "Font size",
                std::to_string(static_cast<int>(cfg.shell_image_font_size)),
                [](int d) {
                    cfg.shell_image_font_size =
                        std::clamp(cfg.shell_image_font_size + d, 9.0, 22.0);
                });
        y += 30;
        checkbox(24, y, "Show filenames", &cfg.shell_image_show_labels);
        y += 24;
        y = hint(y, "used by wallpaper and theme tools");
        y += 12;
        }

        if (shell_tab_ == ShellDesktop) {
        section(y, "Night light");
        y += 30;
        stepper(y, "Night K", std::to_string(cfg.nightlight_on_k),
                [](int d) {
                    cfg.nightlight_on_k =
                        std::clamp(cfg.nightlight_on_k + d * 100, 1000, 5999);
                });
        y += 30;
        stepper(y, "Day K", std::to_string(cfg.nightlight_off_k),
                [](int d) {
                    cfg.nightlight_off_k =
                        std::clamp(cfg.nightlight_off_k + d * 100, 6000, 10000);
                });
        y += 24;
        y = hint(y, "hyprsunset; Super+Ctrl+N and the bar moon toggle these");
        y += 24;
        y = hint(y, "below 6000 K counts as on, same as omarchy toggle nightlight");
        y += 36;

        section(y, "Weather");
        y += 30;
        stepper(y, "Font size",
                std::to_string(static_cast<int>(cfg.shell_weather_font_size)),
                [](int d) {
                    cfg.shell_weather_font_size =
                        std::clamp(cfg.shell_weather_font_size + d, 9.0, 22.0);
                });
        y += 30;
        stepper(y, "Refresh (min)", std::to_string(cfg.weather_refresh_min),
                [](int d) {
                    cfg.weather_refresh_min =
                        std::clamp(cfg.weather_refresh_min + d, 1, 120);
                });
        y += 30;
        {
            text(24, y, "Units", cfg.c_fg);
            struct {
                const char* lbl;
                const char* val;
            } us[] = {{"Auto", "auto"}, {"\u00B0C", "metric"}, {"\u00B0F", "imperial"}};
            double px = 170;
            for (auto& u : us) {
                bool on = cfg.weather_unit == u.val;
                double bw = 56, bh = 20;
                if (on) col(cr, cfg.c_accent, 1.0);
                else col(cr, cfg.c_ws_bg);
                cairo_rectangle(cr, px, y - bh / 2, bw, bh);
                cairo_fill(cr);
                text(px + (bw - text_w(u.lbl)) / 2.0, y, u.lbl,
                     on ? contrast_on(cfg.c_accent) : cfg.c_dim);
                std::string val = u.val;
                add_widget(px, y - bh / 2, bw, bh,
                           [val] { cfg.weather_unit = val; }, nullptr);
                px += bw + 6;
            }
        }
        y += 26;
        checkbox(24, y, "Show temperature on the bar", &cfg.weather_show_temp);
        y += 24;
        y = hint(y, "click the location to search; empty search returns to IP auto");
        y += 36;

        section(y, "Agents");
        y += 30;
        stepper(y, "Font size",
                std::to_string(static_cast<int>(cfg.shell_agents_font_size)),
                [](int d) {
                    cfg.shell_agents_font_size =
                        std::clamp(cfg.shell_agents_font_size + d, 9.0, 22.0);
                });
        y += 24;
        y = hint(y, "usage dashboard: h/l switch subscription, r refresh, Esc close");
        y += 30;
        stepper(y, "Popup width",
                std::to_string(cfg.agents_popup_w_pct()) + "%", [](int d) {
                    cfg.set_agents_popup_pct(cfg.agents_popup_w_pct() + d * 2,
                                             cfg.agents_popup_h_pct());
                });
        y += 30;
        stepper(y, "Popup height",
                std::to_string(cfg.agents_popup_h_pct()) + "%", [](int d) {
                    cfg.set_agents_popup_pct(cfg.agents_popup_w_pct(),
                                             cfg.agents_popup_h_pct() + d * 2);
                });
        y += 24;
        y = hint(y, "percent of the monitor; grok session hangs off the bar");
        y += 24;
        y = hint(y, "right-click the bar icon launches omarchy-agent --pick");
        y += 12;
        }

        if (shell_tab_ == ShellPlugins) {
        section(y, "Quickshell plugins");
        y += 26;
        checkbox(24, y, "Run user Quickshell plugins", &cfg.qs_plugins);
        y += 24;
        y = hint(y, cfg.qs_plugins
                        ? (qs_plugins_want_runtime()
                               ? (qs_plugins_running()
                                      ? "sidecar is up. Escape hides an overlay; "
                                        "it stays in memory until you Stop"
                                      : "on: starting a stripped Quickshell "
                                        "for the plugins checked below")
                               : "on, but idle: check a plugin below to "
                                 "put it in the Plugins chip")
                        : "off: Quickshell stays dead. Check plugins below "
                          "to choose them; click one to start the sidecar");
        y += 26;
        y = hint(y, "Stop: right-click the Plugins chip, Stop Quickshell in "
                    "the accordion, or the button below. The chip can live "
                    "in More like any other module");
        y += 30;
        {
            auto text_btn = [&](double x, double y2, const std::string& label,
                                std::function<void()> cb) {
                double bw = text_w(label) + 16, bh = 22;
                col(cr, cfg.c_ws_bg);
                cairo_rectangle(cr, x, y2 - bh / 2, bw, bh);
                cairo_fill(cr);
                text(x + 8, y2, label, cfg.c_fg);
                add_widget(x, y2 - bh / 2, bw, bh, std::move(cb), nullptr);
                return bw;
            };
            double bx = 24;
            bx += text_btn(bx, y, "Add URL", [] {
                      if (auto* sh = mattbar_shell())
                          sh->summon("mattbar.plugin-add", "");
                  }) +
                  8;
            bx += text_btn(bx, y, "Browse", [] {
                      spawn_detached("xdg-open https://omarchyplugins.com");
                  }) +
                  8;
            bx += text_btn(bx, y, "Remove", [] {
                      if (auto* sh = mattbar_shell())
                          sh->summon("mattbar.plugin-remove", "");
                  }) +
                  8;
            if (cfg.qs_plugins || qs_plugins_running())
                text_btn(bx, y, "Stop sidecar", [] { qs_plugins_shutdown(); });
        }
        y += 30;
        {
            qs_plugins_scan();
            auto plugs = qs_plugins_catalog();
            if (plugs.empty()) {
                y = hint(y, "none installed yet. Add URL or Browse the "
                            "catalog, then check a plugin to load it");
                y += 26;
            } else {
                auto tickbox = [&](double x, double y2, const std::string& label,
                                   bool on, std::function<void()> cb) {
                    double bs = 15;
                    col(cr, cfg.c_ws_bg);
                    cairo_rectangle(cr, x, y2 - bs / 2, bs, bs);
                    cairo_fill(cr);
                    if (on) {
                        col(cr, cfg.c_accent, 1.0);
                        cairo_rectangle(cr, x + 3, y2 - bs / 2 + 3, bs - 6,
                                        bs - 6);
                        cairo_fill(cr);
                    }
                    text(x + bs + 8, y2, label, cfg.c_fg);
                    add_widget(x, y2 - bs / 2, bs + 8 + text_w(label), bs,
                               std::move(cb), nullptr);
                };
                for (auto& p : plugs) {
                    std::string id = p.id;
                    if (p.placeable) {
                        bool on = qs_plugin_shown(id);
                        tickbox(24, y, p.name, on, [id, on, this] {
                            qs_plugin_set_shown(id, !on);
                            apply();
                        });
                        if (on) {
                            auto order = qs_plugin_layout_ids();
                            size_t idx = 0;
                            for (; idx < order.size(); ++idx)
                                if (order[idx] == id) break;
                            if (idx > 0)
                                small_button(292, y, "^", [id, this] {
                                    qs_plugin_move(id, -1);
                                    apply();
                                });
                            if (idx + 1 < order.size())
                                small_button(322, y, "v", [id, this] {
                                    qs_plugin_move(id, +1);
                                    apply();
                                });
                        }
                        y += 22;
                        y = hint(y, p.description.empty() ? p.id
                                                          : p.description);
                        y += 24;
                    } else if (p.service_only) {
                        bool on = qs_plugin_service_on(id);
                        tickbox(24, y, p.name + " (service)", on,
                                [id, on, this] {
                                    qs_plugin_set_service(id, !on);
                                    apply();
                                });
                        y += 22;
                        if (!p.description.empty()) {
                            y = hint(y, p.description);
                            y += 22;
                        }
                    }
                }
            }
        }
        y += 12;
        }
    } else if (tab_ == TabBar) {
    section(y, "General");
    y += 30;
    {
        text(24, y, "Position", cfg.c_fg);
        struct { const char* lbl; const char* val; } ps[] = {
            {"Top", "top"}, {"Bot", "bottom"}, {"Left", "left"},
            {"Right", "right"}};
        double px = 170;
        for (auto& p : ps) {
            bool active = cfg.position == p.val;
            double bw = 48, bh = 20;
            if (active) col(cr, cfg.c_accent, 1.0);
            else col(cr, cfg.c_ws_bg);
            cairo_rectangle(cr, px, y - bh / 2, bw, bh);
            cairo_fill(cr);
            text(px + (bw - text_w(p.lbl)) / 2.0, y, p.lbl,
                 active ? contrast_on(cfg.c_accent) : cfg.c_dim);
            std::string val = p.val;
            add_widget(px, y - bh / 2, bw, bh,
                       [val, this] {
                           cfg.position = val;
                           apply();
                       },
                       nullptr);
            px += bw + 6;
        }
    }
    y += 30;
    stepper(y, "Vertical bar width", std::to_string(cfg.vertical_width),
            [](int d) {
                cfg.vertical_width =
                    std::clamp(cfg.vertical_width + d * 4, 44, 140);
            });
    y += 30;
    stepper(y, "Bar height", std::to_string(cfg.bar_height), [](int d) {
        cfg.bar_height = std::clamp(cfg.bar_height + d * 2, 24, 64);
    });
    y += 30;
    stepper(y, "Hide delay (ms)", std::to_string(cfg.hide_delay_ms),
            [](int d) {
                cfg.hide_delay_ms =
                    std::clamp(cfg.hide_delay_ms + d * 100, 0, 3000);
            });
    y += 30;
    stepper(y, "Reveal delay (ms)",
            cfg.reveal_delay_ms == 0 ? "instant"
                                     : std::to_string(cfg.reveal_delay_ms),
            [](int d) {
                cfg.reveal_delay_ms =
                    std::clamp(cfg.reveal_delay_ms + d * 50, 0, 1000);
            });
    y += 30;
    stepper(y, "Tray auto-collapse (ms)",
            cfg.tray_collapse_ms == 0 ? "off"
                                      : std::to_string(cfg.tray_collapse_ms),
            [](int d) {
                // Graded steps so 60 s is reachable without 120 clicks:
                // 500 ms below 5 s, 1 s to 15 s, 5 s above. The up/down
                // thresholds differ so +1 followed by -1 always returns
                // to the same value.
                int v = cfg.tray_collapse_ms;
                int step =
                    d > 0 ? (v >= 15000 ? 5000 : v >= 5000 ? 1000 : 500)
                          : (v > 15000 ? 5000 : v > 5000 ? 1000 : 500);
                cfg.tray_collapse_ms = std::clamp(v + d * step, 0, 60000);
            });
    y += 30;
    stepper(y, "Font size",
            std::to_string(static_cast<int>(cfg.font_size)), [](int d) {
                cfg.font_size = std::clamp(cfg.font_size + d, 9.0, 22.0);
            });

    y += 36;
    section(y, "Monitors");
    y += 26;
    checkbox(24, y, "One bar per monitor", &cfg.multi_monitor);
    y += 24;
    y = hint(y, cfg.multi_monitor
                    ? "each bar hides and reveals on its own"
                    : "off: one bar only — monitors plugged in later don't "
                      "get one (pin below to choose which)");
    y += 26;
    {
        // Live list of connected outputs. With multi-monitor off this is a
        // single-choice pin (the old `output` key); with it on, each monitor
        // gets its own checkbox and one of them is elected primary.
        auto outs = bar_.output_names();
        if (outs.empty()) {
            y = hint(y, "no named outputs reported by the compositor");
            y += 26;
        } else if (!cfg.multi_monitor) {
            text(24, y, "Show the bar on", cfg.c_fg);
            y += 26;
            for (auto& name : outs) {
                bool on = cfg.output == name;
                double bw = 74, bh = 20;
                if (on) col(cr, cfg.c_accent, 1.0);
                else col(cr, cfg.c_ws_bg);
                cairo_rectangle(cr, 34, y - bh / 2, bw, bh);
                cairo_fill(cr);
                text(34 + (bw - text_w(name)) / 2.0, y, name,
                     on ? contrast_on(cfg.c_accent) : cfg.c_dim);
                std::string n = name;
                add_widget(34, y - bh / 2, bw, bh,
                           [n, this] {
                               // clicking the active one un-pins it again
                               cfg.output = (cfg.output == n) ? "" : n;
                               apply();
                           },
                           nullptr);
                text(34 + bw + 12, y,
                     cfg.output == name ? "pinned here" : "", cfg.c_dim);
                y += 26;
            }
            y = hint(y, cfg.output.empty()
                            ? "none pinned: the compositor decides where the "
                              "single bar lives"
                            : "click again to unpin");
            y += 26;
        } else {
            text(24, y, "Bars on", cfg.c_fg);
            y += 26;
            auto wanted = Config::csv_split(cfg.monitors);
            for (auto& name : outs) {
                bool on = cfg.monitors.empty() ||
                          std::find(wanted.begin(), wanted.end(), name) !=
                              wanted.end();
                // checkbox drawn by hand: the flag lives inside a CSV
                double bs = 15;
                col(cr, cfg.c_ws_bg);
                cairo_rectangle(cr, 34, y - bs / 2, bs, bs);
                cairo_fill(cr);
                if (on) {
                    col(cr, cfg.c_accent, 1.0);
                    cairo_rectangle(cr, 37, y - bs / 2 + 3, bs - 6, bs - 6);
                    cairo_fill(cr);
                }
                text(34 + bs + 8, y, name, cfg.c_fg);
                std::string n = name;
                std::vector<std::string> all = outs;
                add_widget(34, y - bs / 2, bs + 8 + text_w(name), bs,
                           [n, all, on, this] {
                               // "empty means all" is convenient in the conf
                               // file but confusing to toggle, so the first
                               // click materialises the full list.
                               auto v = Config::csv_split(cfg.monitors);
                               if (v.empty()) v = all;
                               auto it = std::find(v.begin(), v.end(), n);
                               if (on && it != v.end()) {
                                   // Refuse to uncheck the last one: an
                                   // empty list means "all monitors" in the
                                   // conf, so allowing it would silently
                                   // re-check everything — and zero bars
                                   // would leave no way back in anyway.
                                   if (v.size() <= 1) return;
                                   v.erase(it);
                               } else if (!on && it == v.end()) {
                                   v.push_back(n);
                               }
                               cfg.monitors = Config::csv_join(v);
                               apply();
                           },
                           nullptr);
                // primary selector, only meaningful for monitors that have
                // a bar at all
                if (on) {
                    // Highlight what the bar ELECTED, not what the conf
                    // asks for: with primary_output unset (or naming a
                    // monitor that no longer carries a bar) the two can
                    // differ, and the highlight must not lie about where
                    // the tray actually is.
                    bool prim = bar_.primary_name() == name;
                    double bw = 66, bh = 18;
                    if (prim) col(cr, cfg.c_accent, 1.0);
                    else col(cr, cfg.c_ws_bg);
                    cairo_rectangle(cr, 250, y - bh / 2, bw, bh);
                    cairo_fill(cr);
                    text(250 + (bw - text_w("primary")) / 2.0, y, "primary",
                         prim ? contrast_on(cfg.c_accent) : cfg.c_dim);
                    add_widget(250, y - bh / 2, bw, bh,
                               [n, this] {
                                   cfg.primary_output = n;
                                   apply();
                               },
                               nullptr);
                }
                y += 26;
            }
            y = hint(y, "primary carries the tray, settings and popups");
            y += 26;
            checkbox(24, y, "Hovering one bar reveals all",
                     &cfg.reveal_all_monitors);
            y += 24;
            y = hint(y, "they hide together too, once the pointer has left "
                        "every bar");
            y += 26;
        }
    }

    y += 10;
    section(y, "Hot strip (hidden-state trigger line)");
    y += 30;
    stepper(y, "Visible line height", std::to_string(cfg.strip_height),
            [](int d) {
                cfg.strip_height = std::clamp(cfg.strip_height + d, 1, 8);
            });
    y += 30;
    stepper(y, "Hover zone height", std::to_string(cfg.strip_hit_height),
            [](int d) {
                cfg.strip_hit_height =
                    std::clamp(cfg.strip_hit_height + d, 1, 24);
            });
    y += 24;
    y = hint(y, "reveal target; also catches clicks at the top edge");
    y += 12;
    } else if (tab_ == TabLook) {
    section(y, "Colors");
    y += 24;

    // Every themable color. Members mirror the *_color conf keys; defaults
    // come from a default-constructed Config so they live in exactly one
    // place (config.hpp).
    struct ColorMeta { const char* label; Color Config::*member; };
    static const ColorMeta cmetas[] = {
        {"Background",    &Config::c_bg},
        {"Text",          &Config::c_fg},
        {"Dim text",      &Config::c_dim},
        {"Accent",        &Config::c_accent},
        {"Urgent",        &Config::c_urgent},
        {"Pills & hover", &Config::c_ws_bg},
        {"Hot strip",     &Config::c_strip},
    };
    constexpr int NCOLORS =
        static_cast<int>(sizeof cmetas / sizeof cmetas[0]);
    color_sel_ = std::clamp(color_sel_, 0, NCOLORS - 1);

    // Checkerboard backing so translucent colors read correctly.
    auto checker = [&](double x, double y2, double cw, double ch) {
        cairo_save(cr);
        cairo_rectangle(cr, x, y2, cw, ch);
        cairo_clip(cr);
        cairo_set_source_rgb(cr, 0.60, 0.60, 0.60);
        cairo_paint(cr);
        cairo_set_source_rgb(cr, 0.36, 0.36, 0.36);
        for (int cy = 0; cy < ch; cy += 5)
            for (int cx = ((cy / 5) % 2) * 5; cx < cw; cx += 10)
                cairo_rectangle(cr, x + cx, y2 + cy, 5, 5);
        cairo_fill(cr);
        cairo_restore(cr);
    };
    auto swatch = [&](double x, double y2, const Color& c, double s,
                      bool selected) {
        checker(x, y2, s, s);
        col(cr, c);
        cairo_rectangle(cr, x, y2, s, s);
        cairo_fill(cr);
        col(cr, selected ? cfg.c_accent : cfg.c_dim,
            selected ? 1.0 : 0.5);
        cairo_set_line_width(cr, selected ? 2 : 1);
        cairo_rectangle(cr, x - 0.5, y2 - 0.5, s + 1, s + 1);
        cairo_stroke(cr);
        cairo_set_line_width(cr, 1);
    };
    // Live preview: a miniature bar using every themable color in context.
    auto preview = [&]() {
        text(24, y, "Preview", cfg.c_dim);
        y += 12;
        double px = 24, pw = w_ - 48, ph = 30, py0 = y, pc = py0 + ph / 2.0;
        checker(px, py0, pw, ph);
        col(cr, cfg.c_bg);
        cairo_rectangle(cr, px, py0, pw, ph);
        cairo_fill(cr);
        col(cr, cfg.c_strip); // strip line along the preview's top edge
        cairo_rectangle(cr, px, py0, pw, 2);
        cairo_fill(cr);
        col(cr, cfg.c_accent); // active workspace pill
        cairo_rectangle(cr, px + 8, pc - 8, 22, 16);
        cairo_fill(cr);
        text(px + 8 + (22 - text_w("1")) / 2.0, pc, "1",
             contrast_on(cfg.c_accent));
        col(cr, cfg.c_ws_bg); // inactive pill (dim label)
        cairo_rectangle(cr, px + 34, pc - 8, 22, 16);
        cairo_fill(cr);
        text(px + 34 + (22 - text_w("2")) / 2.0, pc, "2", cfg.c_dim);
        text(px + (pw - text_w("12:34")) / 2.0, pc, "12:34", cfg.c_fg);
        double rx = px + pw - 8 - text_w("97C");
        text(rx, pc, "97C", cfg.c_urgent);
        rx -= 10 + text_w("eth0");
        text(rx, pc, "eth0", cfg.c_dim);
        y += ph;
    };

    // ---- Follow Omarchy theme toggle -------------------------------------
    // Not the stock checkbox: turning it on must snapshot the user's palette
    // before the theme overlays it, and turning it off must restore.
    {
        double bs = 15;
        col(cr, cfg.c_ws_bg);
        cairo_rectangle(cr, 24, y - bs / 2, bs, bs);
        cairo_fill(cr);
        if (cfg.follow_omarchy_theme) {
            col(cr, cfg.c_accent, 1.0);
            cairo_rectangle(cr, 24 + 3, y - bs / 2 + 3, bs - 6, bs - 6);
            cairo_fill(cr);
        }
        text(24 + bs + 8, y, "Follow Omarchy theme", cfg.c_fg);
        if (cfg.follow_omarchy_theme) {
            const std::string& st = omarchy_theme_status();
            text(w_ - 24 - text_w(st), y, st, cfg.c_dim);
        }
        add_widget(24, y - bs / 2, bs + 8 + text_w("Follow Omarchy theme"),
                   bs,
                   [this] {
                       if (!cfg.follow_omarchy_theme) {
                           cfg.colors_snapshot(); // keep the user's palette
                           cfg.follow_omarchy_theme = true;
                           omarchy_theme_apply(cfg); // status explains a miss
                       } else {
                           cfg.follow_omarchy_theme = false;
                           cfg.colors_restore();
                       }
                       apply();
                   },
                   nullptr);
    }
    y += 26;

    if (cfg.follow_omarchy_theme) {
        // Theme drives the palette: show it read-only. Manual editing would
        // just be clobbered on the next theme swap, so it is hidden — the
        // user's own colors are kept and come back when the toggle is off.
        y = hint(y, "colors follow the Omarchy theme; toggle off to edit");
        y += 26;
        for (int i = 0; i < NCOLORS; ++i) {
            double sx = 24 + (i % 2) * 200;
            double sy = y + (i / 2) * 26;
            swatch(sx, sy - 8, cfg.*(cmetas[i].member), 16, false);
            text(sx + 26, sy, cmetas[i].label, cfg.c_dim);
        }
        y += ((NCOLORS + 1) / 2) * 26 + 6;
        preview();
    } else {

    // Preset themes: baked values for all NCOLORS colors, in cmetas order
    // (bg, fg, dim, accent, urgent, pills, strip). Selecting one overwrites
    // every color at once; the sliders below still fine-tune from there.
    struct Theme { const char* name; Color c[NCOLORS]; };
    static Theme themes[] = {
        {"Default", {}}, // filled from Config's defaults below
        {"Light",
         {{0.949, 0.953, 0.969, 0.94}, {0.141, 0.149, 0.180, 1.00},
          {0.420, 0.447, 0.502, 1.00}, {0.145, 0.388, 0.922, 1.00},
          {0.831, 0.235, 0.204, 1.00}, {0.867, 0.878, 0.910, 1.00},
          {0.145, 0.388, 0.922, 0.35}}},
        {"Nord",
         {{0.180, 0.204, 0.251, 0.94}, {0.925, 0.937, 0.957, 1.00},
          {0.482, 0.533, 0.631, 1.00}, {0.533, 0.753, 0.816, 1.00},
          {0.749, 0.380, 0.416, 1.00}, {0.231, 0.259, 0.322, 1.00},
          {0.533, 0.753, 0.816, 0.35}}},
        {"Gruvbox",
         {{0.157, 0.157, 0.157, 0.94}, {0.922, 0.859, 0.698, 1.00},
          {0.573, 0.514, 0.455, 1.00}, {0.271, 0.522, 0.533, 1.00},
          {0.800, 0.141, 0.114, 1.00}, {0.235, 0.220, 0.212, 1.00},
          {0.271, 0.522, 0.533, 0.35}}},
        {"Dracula",
         {{0.157, 0.165, 0.212, 0.94}, {0.973, 0.973, 0.949, 1.00},
          {0.384, 0.447, 0.643, 1.00}, {0.741, 0.576, 0.976, 1.00},
          {1.000, 0.333, 0.333, 1.00}, {0.267, 0.278, 0.353, 1.00},
          {0.741, 0.576, 0.976, 0.35}}},
        {"Solarized Light",
         {{0.992, 0.965, 0.890, 0.94}, {0.027, 0.212, 0.259, 1.00},
          {0.576, 0.631, 0.631, 1.00}, {0.149, 0.545, 0.824, 1.00},
          {0.863, 0.196, 0.184, 1.00}, {0.933, 0.910, 0.835, 1.00},
          {0.149, 0.545, 0.824, 0.35}}},
    };
    static bool themes_init = false;
    if (!themes_init) {
        const Config d; // "Default" is literally the stock config
        for (int i = 0; i < NCOLORS; ++i) themes[0].c[i] = d.*(cmetas[i].member);
        themes_init = true;
    }

    // Highlight the preset that exactly matches the current colors (after
    // 8-bit quantization, since values round-trip through #RRGGBBAA); if
    // the user has tweaked sliders, no preset lights up.
    auto quant = [](double v) { return static_cast<int>(v * 255 + 0.5); };
    auto theme_active = [&](const Theme& t) {
        for (int i = 0; i < NCOLORS; ++i) {
            const Color& a = cfg.*(cmetas[i].member);
            const Color& b = t.c[i];
            if (quant(a.r) != quant(b.r) || quant(a.g) != quant(b.g) ||
                quant(a.b) != quant(b.b) || quant(a.a) != quant(b.a))
                return false;
        }
        return true;
    };
    text(24, y, "Theme", cfg.c_fg);
    {
        double tx = 90;
        for (auto& t : themes) {
            double bw = text_w(t.name) + 14, bh = 20;
            if (tx + bw > w_ - 24) { tx = 90; y += 26; } // wrap
            bool active = theme_active(t);
            if (active) col(cr, cfg.c_accent, 1.0);
            else col(cr, cfg.c_ws_bg);
            cairo_rectangle(cr, tx, y - bh / 2, bw, bh);
            cairo_fill(cr);
            text(tx + 7, y, t.name,
                 active ? contrast_on(cfg.c_accent) : cfg.c_fg);
            const Theme* tp = &t;
            add_widget(tx, y - bh / 2, bw, bh,
                       [tp, this] {
                           for (int i = 0; i < NCOLORS; ++i)
                               cfg.*(cmetas[i].member) = tp->c[i];
                           apply();
                       },
                       nullptr);
            tx += bw + 6;
        }
    }
    y += 28;
    y = hint(y, "click a swatch to select, then adjust below");
    y += 26;

    // Two-column swatch table (rows of 2 keeps the window compact).
    for (int i = 0; i < NCOLORS; ++i) {
        double sx = 24 + (i % 2) * 200;
        double sy = y + (i / 2) * 26;
        swatch(sx, sy - 8, cfg.*(cmetas[i].member), 16, i == color_sel_);
        text(sx + 26, sy, cmetas[i].label,
             i == color_sel_ ? cfg.c_fg : cfg.c_dim);
        add_widget(sx, sy - 11, 26 + text_w(cmetas[i].label) + 6, 22,
                   [i, this] {
                       color_sel_ = i;
                       draw(); // selection only; nothing to apply/save
                   },
                   nullptr);
    }
    y += ((NCOLORS + 1) / 2) * 26 + 6;

    {
        Color* sel = &(cfg.*(cmetas[color_sel_].member));
        text(24, y, std::string("Editing: ") + cmetas[color_sel_].label,
             cfg.c_fg);

        // Right-aligned reset buttons on the same row.
        auto text_button = [&](double right_x, const std::string& label,
                               std::function<void()> cb) {
            double bw = text_w(label) + 16, bh = 20;
            col(cr, cfg.c_ws_bg);
            cairo_rectangle(cr, right_x - bw, y - bh / 2, bw, bh);
            cairo_fill(cr);
            text(right_x - bw + 8, y, label, cfg.c_fg);
            add_widget(right_x - bw, y - bh / 2, bw, bh, std::move(cb),
                       nullptr);
            return bw;
        };
        double bw = text_button(w_ - 24, "Reset all", [this] {
            const Config d;
            for (auto& m : cmetas) cfg.*(m.member) = d.*(m.member);
            apply();
        });
        text_button(w_ - 24 - bw - 8, "Reset", [this] {
            const Config d;
            cfg.*(cmetas[color_sel_].member) = d.*(cmetas[color_sel_].member);
            apply();
        });

        y += 30;
        slider(y, "Red", sel->r, [sel](double f) { sel->r = f; });
        y += 28;
        slider(y, "Green", sel->g, [sel](double f) { sel->g = f; });
        y += 28;
        slider(y, "Blue", sel->b, [sel](double f) { sel->b = f; });
        y += 28;
        slider(y, "Opacity", sel->a, [sel](double f) { sel->a = f; });
    }

    y += 28;
    preview();
    } // !follow_omarchy_theme

    } else if (tab_ == TabModules || tab_ == TabNotify) {
    if (tab_ == TabModules) {
    section(y, "Temperature");
    y += 30;
    stepper(y, "Sensor", temp_current_display(),
            [](int d) { temp_cycle_sensor(d); });
    y += 30;
    stepper(y, "Warn at (\u00b0C)", std::to_string(cfg.temp_warn), [](int d) {
        cfg.temp_warn = std::clamp(cfg.temp_warn + d * 5, 50, 110);
    });

    y += 36;
    section(y, "Bluetooth");
    y += 26;
    text(24, y, "shown next to the icon:", cfg.c_dim);
    y += 24;
    checkbox(24, y, "Device name", &cfg.bluetooth_show_name);
    checkbox(165, y, "Battery %", &cfg.bluetooth_show_battery);
    checkbox(290, y, "+N count", &cfg.bluetooth_show_count);
    y += 30;
    stepper(y, "Name length", std::to_string(cfg.bluetooth_name_len),
            [](int d) {
                cfg.bluetooth_name_len =
                    std::clamp(cfg.bluetooth_name_len + d * 2, 4, 64);
            });

    y += 36;
    section(y, "Media");
    y += 26;
    checkbox(24, y, "Show artist", &cfg.media_show_artist);
    y += 30;
    stepper(y, "Max length", std::to_string(cfg.media_len) + " chars",
            [](int d) {
                cfg.media_len = std::clamp(cfg.media_len + d * 2, 8, 80);
            });
    } // TabModules (bar chips, first half)

    if (tab_ == TabNotify) {
    section(y, "Notifications & OSD");
    y += 26;
    checkbox(24, y, "Notification daemon (own org.freedesktop.Notifications)",
             &cfg.enable_notifications);
    y += 24;
    checkbox(24, y, "Volume / brightness OSD", &cfg.enable_osd);
    y += 24;
    checkbox(24, y, "Take over from a running daemon (mako / Omarchy shell)",
             &cfg.notifications_takeover);
    y += 24;
    checkbox(24, y, "Shut down the Quickshell instance completely",
             &cfg.quickshell_shutdown);
    y += 24;
    y = hint(y, cfg.quickshell_shutdown
                    ? "Quickshell is stopped. MattBar provides the menu, "
                      "lock, wallpaper and any panels it has; Omarchy's "
                      "own UI will not appear"
                    : "off: Quickshell stays up for wifi / bluetooth / "
                      "volume panels and Super+Space. MattBar only takes "
                      "notifications if takeover is on");
    y += 30;
    text(24, y, "on-screen display for:", cfg.c_dim);
    y += 24;
    checkbox(24, y, "Volume", &cfg.osd_volume);
    checkbox(130, y, "Microphone", &cfg.osd_mic);
    checkbox(280, y, "Brightness", &cfg.osd_brightness);
    y += 30;
    text(24, y, "alerts:", cfg.c_dim);
    y += 24;
    checkbox(24, y, "Battery low", &cfg.notify_battery);
    checkbox(160, y, "Bluetooth connect / disconnect",
             &cfg.notify_bluetooth);
    y += 30;
    stepper(y, "Notification timeout",
            cfg.notification_timeout_s <= 0
                ? std::string("never")
                : std::to_string(cfg.notification_timeout_s) + "s",
            [](int d) {
                cfg.notification_timeout_s =
                    std::clamp(cfg.notification_timeout_s + d, 0, 300);
            });
    y += 30;
    stepper(y, "OSD duration",
            std::to_string(cfg.osd_timeout_ms) + "ms", [](int d) {
                cfg.osd_timeout_ms =
                    std::clamp(cfg.osd_timeout_ms + d * 250, 250, 10000);
            });
    y += 30;
    stepper(y, "Popups shown at once",
            std::to_string(cfg.notification_max_shown), [](int d) {
                cfg.notification_max_shown =
                    std::clamp(cfg.notification_max_shown + d, 1, 10);
            });
    y += 24;
    y = hint(y, "off by default: leaves mako / the Omarchy shell in charge");

    y += 36;
    section(y, "Notification centre");
    y += 26;
    checkbox(24, y, "Show count on the bell", &cfg.bell_show_count);
    y += 24;
    y = hint(y, "off: the bell never nags you to go and read it");
    y += 26;
    checkbox(24, y, "Dim the bell when there's nothing", &cfg.bell_dim_when_empty);
    y += 30;
    stepper(y, "Remember", std::to_string(cfg.history_max), [](int d) {
        cfg.history_max = std::clamp(cfg.history_max + d * 5, 5, 100);
    });
    y += 24;
    y = hint(y, "click the bell for history; right-click an entry to mute");
    y += 30;
    {
        // Per-app mute list, built from every app that has notified us.
        auto apps = Config::csv_split(cfg.known_apps);
        // An app muted before we ever recorded it (hand-edited conf) must
        // still be listed, or it could never be un-muted from here.
        for (auto& m : Config::csv_split(cfg.muted_apps))
            if (std::find(apps.begin(), apps.end(), m) == apps.end())
                apps.push_back(m);
        std::sort(apps.begin(), apps.end());
        if (apps.empty()) {
            y = hint(y, "no apps have sent a notification yet");
            y += 26;
        } else {
            text(24, y, "Muted apps", cfg.c_fg);
            y += 26;
            for (auto& app : apps) {
                bool muted = cfg.app_muted(app);
                double bs = 15;
                col(cr, cfg.c_ws_bg);
                cairo_rectangle(cr, 34, y - bs / 2, bs, bs);
                cairo_fill(cr);
                if (muted) {
                    col(cr, cfg.c_accent, 1.0);
                    cairo_rectangle(cr, 37, y - bs / 2 + 3, bs - 6, bs - 6);
                    cairo_fill(cr);
                }
                text(34 + bs + 8, y, app, muted ? cfg.c_dim : cfg.c_fg);
                std::string a = app;
                add_widget(34, y - bs / 2, bs + 8 + text_w(app), bs,
                           [a, muted, this] {
                               cfg.set_app_muted(a, !muted);
                               apply();
                           },
                           nullptr);
                y += 24;
            }
            y = hint(y,
                     "muted apps stay out of your way but still get logged");
            y += 26;
        }
    }
    } // TabNotify

    if (tab_ == TabModules) {
    section(y, "Clock & calendar");
    y += 26;
    {
        text(24, y, "Clock format", cfg.c_fg);
        struct { const char* lbl; const char* fmt; const char* vfmt; } ps[] = {
            {"24h", "%a %d %b  %H:%M", "%H:%M"},
            {"12h", "%a %d %b  %I:%M %p", "%I:%M"},
            {"24h+s", "%a %d %b  %H:%M:%S", "%H:%M"},
            {"bare", "%H:%M", "%H:%M"},
        };
        double px = 170;
        for (auto& p : ps) {
            bool active = cfg.clock_format == p.fmt;
            double bw = 52, bh = 20;
            if (active) col(cr, cfg.c_accent, 1.0);
            else col(cr, cfg.c_ws_bg);
            cairo_rectangle(cr, px, y - bh / 2, bw, bh);
            cairo_fill(cr);
            text(px + (bw - text_w(p.lbl)) / 2.0, y, p.lbl,
                 active ? contrast_on(cfg.c_accent) : cfg.c_dim);
            std::string f = p.fmt, vf = p.vfmt;
            add_widget(px, y - bh / 2, bw, bh,
                       [f, vf, this] {
                           cfg.clock_format          = f;
                           cfg.clock_format_vertical = vf;
                           apply();
                       },
                       nullptr);
            px += bw + 6;
        }
    }
    y += 24;
    y = hint(y, "any strftime string works via clock_format in the conf "
                "file; presets just set the common ones");
    y += 26;
    checkbox(24, y, "Volume: headset indicator", &cfg.volume_headset_indicator);
    y += 24;
    y = hint(y, "a headphone rune beside the volume while sound goes to "
                "earbuds or headphones");
    y += 26;
    checkbox(24, y, "Battery: show time estimate", &cfg.battery_show_time);
    y += 24;
    y = hint(y, "\"2h05\" to empty while discharging, to full while "
                "charging");
    y += 26;
    checkbox(24, y, "Click the clock for a calendar", &cfg.calendar_enabled);
    y += 26;
    checkbox(24, y, "Week starts on Monday", &cfg.calendar_monday_first);
    y += 26;
    checkbox(24, y, "Show week numbers", &cfg.calendar_week_numbers);

    y += 36;
    section(y, "Agents");
    y += 30;
    stepper(y, "Popup width",
            std::to_string(cfg.agents_popup_w_pct()) + "%", [](int d) {
                cfg.set_agents_popup_pct(cfg.agents_popup_w_pct() + d * 2,
                                         cfg.agents_popup_h_pct());
            });
    y += 30;
    stepper(y, "Popup height",
            std::to_string(cfg.agents_popup_h_pct()) + "%", [](int d) {
                cfg.set_agents_popup_pct(cfg.agents_popup_w_pct(),
                                         cfg.agents_popup_h_pct() + d * 2);
            });
    y += 24;
    y = hint(y, "percent of the monitor; grok / terminal session at the bar edge");

    y += 36;
    section(y, "Power profile");
    y += 26;
    checkbox(24, y, "Show the profile name", &cfg.power_show_label);
    y += 26;
    checkbox(24, y, "Session actions in the popup", &cfg.power_show_actions);
    y += 24;
    y = hint(y, "suspend, hibernate, restart and shut down under the "
                "profiles; restart and shut down ask for a second click. "
                "Commands via power_cmd_* in the conf file");

    y += 36;
    section(y, "Night light");
    y += 30;
    stepper(y, "Night K", std::to_string(cfg.nightlight_on_k),
            [](int d) {
                cfg.nightlight_on_k =
                    std::clamp(cfg.nightlight_on_k + d * 100, 1000, 5999);
            });
    y += 30;
    stepper(y, "Day K", std::to_string(cfg.nightlight_off_k),
            [](int d) {
                cfg.nightlight_off_k =
                    std::clamp(cfg.nightlight_off_k + d * 100, 6000, 10000);
            });
    y += 24;
    y = hint(y, "hyprsunset temperatures; click the bar moon or Super+Ctrl+N");
    y += 36;

    section(y, "Weather");
    y += 30;
    stepper(y, "Refresh (min)", std::to_string(cfg.weather_refresh_min),
            [](int d) {
                cfg.weather_refresh_min =
                    std::clamp(cfg.weather_refresh_min + d, 1, 120);
            });
    y += 30;
    {
        text(24, y, "Units", cfg.c_fg);
        struct {
            const char* lbl;
            const char* val;
        } us[] = {{"Auto", "auto"}, {"\u00B0C", "metric"}, {"\u00B0F", "imperial"}};
        double px = 170;
        for (auto& u : us) {
            bool on = cfg.weather_unit == u.val;
            double bw = 56, bh = 20;
            if (on) col(cr, cfg.c_accent, 1.0);
            else col(cr, cfg.c_ws_bg);
            cairo_rectangle(cr, px, y - bh / 2, bw, bh);
            cairo_fill(cr);
            text(px + (bw - text_w(u.lbl)) / 2.0, y, u.lbl,
                 on ? contrast_on(cfg.c_accent) : cfg.c_dim);
            std::string val = u.val;
            add_widget(px, y - bh / 2, bw, bh,
                       [val] { cfg.weather_unit = val; }, nullptr);
            px += bw + 6;
        }
    }
    y += 26;
    checkbox(24, y, "Show temperature on the bar", &cfg.weather_show_temp);
    y += 26;
    checkbox(24, y, "Battery percentage on the bar", &cfg.power_show_pct);
    y += 12;
    } // TabModules (clock / power / weather)
    } else if (tab_ == TabLayout) {
    section(y, "Modules & layout");
    y += 24;
    y = hint(y, "check = show   L/C/R/M = zone   arrows = order");
    y += 18;
    y = hint(y, "M = More  (click ⋯ on the bar to show those modules)");
    y += 26;

    struct ModMeta { const char* id; const char* label; bool* flag; };
    static const ModMeta metas[] = {
        {"more", "More", nullptr},
        {"plugins", "Plugins", &cfg.show_plugins},
        {"omarchy", "Omarchy", &cfg.show_omarchy},
        {"workspaces", "Workspaces", &cfg.show_workspaces},
        {"clock", "Clock", &cfg.show_clock},
        {"pin", "Pin", &cfg.show_pin},
        {"tray", "Tray", &cfg.show_tray},
        {"update", "Update", &cfg.show_update},
        {"agents", "AI agents", &cfg.show_agents},
        {"screenrecord", "Rec light", &cfg.show_screenrecord},
        {"temp", "Temp", &cfg.show_temp},
        {"network", "Network", &cfg.show_network},
        {"bluetooth", "Bluetooth", &cfg.show_bluetooth},
        {"display", "Displays", &cfg.show_display},
        {"brightness", "Brightness", &cfg.show_brightness},
        {"media", "Media (MPRIS)", &cfg.show_media},
        {"caffeine", "Stay awake", &cfg.show_caffeine},
        {"nightlight", "Night light", &cfg.show_nightlight},
        {"weather", "Weather", &cfg.show_weather},
        {"kblayout", "Keyboard layout", &cfg.show_kblayout},
        {"activewindow", "Active window", &cfg.show_active_window},
        {"reminder", "Reminders", &cfg.show_reminder},
        {"dictation", "Dictation", &cfg.show_dictation},
        {"tailscale", "Tailscale", &cfg.show_tailscale},
        {"dropbox", "Dropbox", &cfg.show_dropbox},
        {"notifications", "Notifications", &cfg.show_notifications},
        {"power", "Power profile", &cfg.show_power},
        {"microphone", "Microphone", &cfg.show_microphone},
        {"volume", "Volume", &cfg.show_volume},
        {"battery", "Battery", &cfg.show_battery},
    };
    auto meta_for = [&](const std::string& id) -> const ModMeta* {
        for (auto& m : metas)
            if (id == m.id) return &m;
        return nullptr;
    };
    auto zone_box = [&](double x, double y2, const char* lbl, bool active,
                        std::function<void()> cb) {
        double bw = 20, bh = 17;
        if (active) col(cr, cfg.c_accent, 1.0);
        else col(cr, cfg.c_ws_bg);
        cairo_rectangle(cr, x, y2 - bh / 2, bw, bh);
        cairo_fill(cr);
        text(x + (bw - text_w(lbl)) / 2.0, y2, lbl,
             active ? contrast_on(cfg.c_accent) : cfg.c_dim);
        add_widget(x, y2 - bh / 2, bw, bh, std::move(cb), nullptr);
    };

    // rows in on-bar order, grouped by zone (with a small gap between zones)
    static const char* zone_names[] = {"Left", "Center", "Right", "More"};
    static const char* zl[]         = {"L", "C", "R", "M"};
    auto draw_zone_row = [&](const ModMeta* m, const std::string& id, int z,
                             size_t i, size_t n) {
        if (m->flag) checkbox(40, y, m->label, m->flag);
        else text(40, y, m->label, cfg.c_fg);
        int tmax = (id == "more") ? 3 : 4; // More cannot nest in itself
        for (int tz = 0; tz < tmax; ++tz) {
            zone_box(198 + tz * 22, y, zl[tz], tz == z, [id, tz, this] {
                cfg.layout_set_zone(id, tz);
                apply();
            });
        }
        bool can_up = i > 0, can_dn = i + 1 < n;
        if (can_up)
            small_button(292, y, "^", [id, this] {
                cfg.layout_move(id, -1);
                apply();
            });
        if (can_dn)
            small_button(322, y, "v", [id, this] {
                cfg.layout_move(id, +1);
                apply();
            });
        y += 26;
    };

    for (int z = 0; z < 4; ++z) {
        text(24, y, zone_names[z], cfg.c_dim);
        y += 24;
        auto ids = cfg.layout_get(z);
        if (z == 3 && ids.empty()) {
            text(40, y, "nothing assigned", cfg.c_dim);
            y += 26;
        }
        for (size_t i = 0; i < ids.size(); ++i) {
            const ModMeta* m = meta_for(ids[i]);
            std::string    id = ids[i];
            if (!m) continue;
            draw_zone_row(m, id, z, i, ids.size());
        }
        y += 8;
    }

    // ---- modules not currently on the bar ---------------------------------
    // A module absent from every zone would otherwise be invisible here
    // too: newly added modules would be undiscoverable without hand-editing
    // the config. L/C/R places one; the checkbox still governs visibility
    // once placed.
    {
        std::vector<const ModMeta*> unplaced;
        for (auto& m : metas)
            if (cfg.layout_zone_of(m.id) < 0) unplaced.push_back(&m);
        if (!unplaced.empty()) {
            text(24, y, "Not on bar", cfg.c_dim);
            y += 24;
            for (auto* m : unplaced) {
                std::string id = m->id;
                if (m->flag) checkbox(40, y, m->label, m->flag);
                else text(40, y, m->label, cfg.c_fg);
                int tmax = (id == "more") ? 3 : 4;
                for (int tz = 0; tz < tmax; ++tz) {
                    zone_box(198 + tz * 22, y, zl[tz], false, [id, tz, this] {
                        cfg.layout_set_zone(id, tz);
                        apply();
                    });
                }
                y += 26;
            }
            y += 8;
        }
    }
    y += 12;
    } // tab content

    const double content_h = y;
    cairo_restore(cr);
    scroll_max_ = std::max(0.0, content_h - visible_h);
    scroll_     = std::clamp(scroll_, 0.0, scroll_max_);

    // ---- fixed header -----------------------------------------------------
    woff_ = 0;
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    col(cr, cfg.c_bg, 0.98);
    cairo_rectangle(cr, 0, 0, w_, hdr);
    cairo_fill(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    text(24, 22, "MattBar Settings", cfg.c_fg);
    {
        auto paint_tabs = [&](Tab* ts, int n, double ty, int current,
                              std::function<void(int)> on_pick) {
            double tx = 24;
            for (int i = 0; i < n; ++i) {
                double twid = tab_label_w(ts[i].lbl);
                if (tx + twid > w_ - 12 && tx > 24) {
                    tx = 24;
                    ty += TAB_H + 4;
                }
                bool on = current == ts[i].id;
                if (on) col(cr, cfg.c_accent, 1.0);
                else col(cr, cfg.c_ws_bg);
                cairo_rectangle(cr, tx, ty, twid, TAB_H);
                cairo_fill(cr);
                text(tx + 8, ty + TAB_H / 2.0, ts[i].lbl,
                     on ? contrast_on(cfg.c_accent) : cfg.c_dim);
                int id = ts[i].id;
                widgets_.push_back({tx, ty, twid, TAB_H,
                                    [id, on_pick] { on_pick(id); }, nullptr});
                tx += twid + 6;
            }
            return ty + TAB_H + 4;
        };
        double ty = paint_tabs(tabs, ntabs, 42, tab_, [this](int id) {
            tab_    = id;
            scroll_ = 0;
            draw();
        });
        if (nshell)
            paint_tabs(shells, nshell, ty, shell_tab_, [this](int id) {
                shell_tab_ = id;
                scroll_    = 0;
                draw();
            });
    }
    col(cr, cfg.c_ws_bg);
    cairo_move_to(cr, 0, hdr - 0.5);
    cairo_line_to(cr, w_, hdr - 0.5);
    cairo_stroke(cr);

    // ---- fixed footer -----------------------------------------------------
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    col(cr, cfg.c_bg, 0.98);
    cairo_rectangle(cr, 0, h_ - FOOTER_H, w_, FOOTER_H);
    cairo_fill(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    col(cr, cfg.c_ws_bg);
    cairo_move_to(cr, 0, h_ - FOOTER_H + 0.5);
    cairo_line_to(cr, w_, h_ - FOOTER_H + 0.5);
    cairo_stroke(cr);

    double by = h_ - FOOTER_H / 2.0;
    auto big_button = [&](double x, const std::string& label, bool accent,
                          std::function<void()> cb) {
        double bw = 130, bh = 30;
        if (accent) col(cr, cfg.c_accent, 1.0);
        else col(cr, cfg.c_ws_bg);
        cairo_rectangle(cr, x, by - bh / 2, bw, bh);
        cairo_fill(cr);
        text(x + (bw - text_w(label)) / 2.0, by, label,
             accent ? contrast_on(cfg.c_accent) : cfg.c_fg);
        widgets_.push_back({x, by - bh / 2, bw, bh, std::move(cb), nullptr});
    };
    text(24, by, "settings persist automatically", cfg.c_dim);
    big_button(w_ - 24 - 130, "Close", true,
               [this] { bar_.close_settings_later(); });

    // ---- scrollbar --------------------------------------------------------
    if (scroll_max_ > 0) {
        double track_y = hdr + 2, track_h = visible_h - 4;
        double thumb_h =
            std::max(24.0, track_h * visible_h / (visible_h + scroll_max_));
        double thumb_y =
            track_y + (track_h - thumb_h) * (scroll_ / scroll_max_);
        col(cr, cfg.c_ws_bg);
        cairo_rectangle(cr, w_ - 6, thumb_y, 3, thumb_h);
        cairo_fill(cr);
    }

    // outer border last, over everything
    col(cr, cfg.c_ws_bg);
    cairo_set_line_width(cr, 1);
    cairo_rectangle(cr, 0.5, 0.5, w_ - 1, h_ - 1);
    cairo_stroke(cr);

    cairo_destroy(cr);
    cairo_surface_destroy(cs);

    frac_.apply(surf_, w_, h_, sc);
    wl_surface_attach(surf_, buffer, 0, 0);
    if (wl_surface_get_version(surf_) >= 4)
        wl_surface_damage_buffer(surf_, 0, 0, bw, bh);
    else
        wl_surface_damage(surf_, 0, 0, w_, h_);
    wl_surface_commit(surf_);
}

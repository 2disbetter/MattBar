#include "bar.hpp"
#include "config.hpp"
#include "ctl.hpp"
#include "modules.hpp"
#include "idle.hpp"
#include "lock.hpp"
#include "nightlight.hpp"
#include "weather.hpp"
#include "notify.hpp"
#include "polkit.hpp"
#include "qs_plugins.hpp"
#include "shell.hpp"
#include "wallpaper.hpp"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <string>

static Bar* g_bar = nullptr;

int main(int argc, char** argv) {
    // mattbarctl mode: as the `mattbarctl` symlink, or `mattbar ctl ...`
    {
        std::string self = argv[0];
        auto slash = self.rfind('/');
        if (slash != std::string::npos) self = self.substr(slash + 1);
        if (self == "mattbarctl") return ctl_client(argc - 1, argv + 1);
        if (self == "omarchy-shell")
            return omarchy_shell_client(argc - 1, argv + 1);
        if (self == "omarchy-menu")
            return omarchy_menu_client(argc - 1, argv + 1);
        if (argc > 1 && std::string(argv[1]) == "ctl")
            return ctl_client(argc - 2, argv + 2);
    }
    if (argc > 1 && std::string(argv[1]) == "--version") {
        printf("mattbar %s (built %s %s)\n", MATTBAR_VERSION, __DATE__,
               __TIME__);
        return 0;
    }
    if (getenv("MATTBAR_DEBUG"))
        fprintf(stderr, "mattbar %s (built %s %s) — debug on\n",
                MATTBAR_VERSION, __DATE__, __TIME__);
    // Graceful stop: `_exit` skipped destructors, left pactl orphans, and
    // if we were the lock client the session stayed locked with no UI.
    auto on_stop = [](int) { if (g_bar) g_bar->request_stop(); };
    struct sigaction sa {};
    sa.sa_handler = on_stop;
    sigemptyset(&sa.sa_mask);
    // no SA_RESTART: epoll_wait must return so the loop can notice stop
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    signal(SIGPIPE, SIG_IGN);

    cfg.load();

    Bar bar;
    g_bar = &bar;

    // Placement and order come from cfg.layout_* (editable in settings).
    bar.add_module("omarchy",    make_omarchy_button());
    bar.add_module("workspaces", make_workspaces());
    bar.add_module("clock",      make_clock());
    bar.add_module("pin",        make_pin());
    bar.add_module("more",       make_more());
    bar.add_module("plugins",    make_plugins());
    bar.add_module("tray",       make_tray());
    bar.add_module("update",     make_update_button());
    bar.add_module("agents",     make_agents());
    bar.add_module("microphone", make_microphone());
    bar.add_module("screenrecord", make_screenrecord());
    bar.add_module("temp",       make_temp());
    bar.add_module("network",    make_network());
    bar.add_module("volume",     make_volume());
    bar.add_module("bluetooth",  make_bluetooth());
    bar.add_module("display",    make_display());
    bar.add_module("brightness", make_brightness());
    bar.add_module("media",      make_media());
    bar.add_module("caffeine",   make_caffeine());
    bar.add_module("nightlight", make_nightlight());
    bar.add_module("weather",    make_weather());
    bar.add_module("kblayout",   make_kblayout());
    bar.add_module("activewindow", make_active_window());
    bar.add_module("reminder",   make_reminder());
    bar.add_module("dictation",  make_dictation());
    bar.add_module("tailscale",  make_tailscale());
    bar.add_module("dropbox",    make_dropbox());
    bar.add_module("battery",    make_battery());
    bar.add_module("notifications", make_notifications());
    bar.add_module("power",      make_power());
    qs_plugins_sync_modules(bar);

    nightlight_init(bar);
    weather_init(bar);
    if (!bar.init()) return 1;
    wallpaper_init(bar);
    lock_init(bar);
    polkit_init(bar);
    idle_init(bar);
    NotifyDaemon daemon;
    daemon.init(bar); // dormant unless cfg.enable_notifications/enable_osd
    Shell shell;
    shell.init(bar);  // named overlays (menu, calendar, …)
    CtlServer ctl;
    ctl.init(bar);    // $XDG_RUNTIME_DIR/mattbar.sock (mattbarctl)
    bar.run();
    bar.shutdown();
    return bar.exit_code();
}

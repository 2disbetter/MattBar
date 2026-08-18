#include "bar.hpp"
#include "config.hpp"
#include "ctl.hpp"
#include "modules.hpp"
#include "notify.hpp"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <string>

static Bar* g_bar = nullptr;

int main(int argc, char** argv) {
    // mattbarctl mode (symlink or `mattbar ctl ...`)
    {
        std::string self = argv[0];
        auto slash = self.rfind('/');
        if (slash != std::string::npos) self = self.substr(slash + 1);
        if (self == "mattbarctl") return ctl_client(argc - 1, argv + 1);
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
    signal(SIGINT,  [](int) { if (g_bar) g_bar->request_draw(); _exit(0); });
    signal(SIGTERM, [](int) { _exit(0); });
    signal(SIGPIPE, SIG_IGN);

    cfg.load();

    Bar bar;
    g_bar = &bar;

    // Placement/order from cfg.layout_* (editable in settings).
    bar.add_module("omarchy",    make_omarchy_button());
    bar.add_module("workspaces", make_workspaces());
    bar.add_module("clock",      make_clock());
    bar.add_module("pin",        make_pin());
    bar.add_module("tray",       make_tray());
    bar.add_module("update",     make_update_button());
    bar.add_module("agents",     make_agents());
    bar.add_module("microphone", make_microphone());
    bar.add_module("screenrecord", make_screenrecord());
    bar.add_module("temp",       make_temp());
    bar.add_module("network",    make_network());
    bar.add_module("volume",     make_volume());
    bar.add_module("bluetooth",  make_bluetooth());
    bar.add_module("brightness", make_brightness());
    bar.add_module("media",      make_media());
    bar.add_module("caffeine",   make_caffeine());
    bar.add_module("battery",    make_battery());
    bar.add_module("notifications", make_notifications());
    bar.add_module("power",      make_power());

    if (!bar.init()) return 1;
    NotifyDaemon daemon;
    daemon.init(bar); // dormant unless cfg.enable_notifications/enable_osd
    CtlServer ctl;
    ctl.init(bar);    // $XDG_RUNTIME_DIR/mattbar.sock (mattbarctl)
    bar.run();
    bar.shutdown();
    return bar.exit_code();
}

#include "polkit.hpp"
#include "bar.hpp"
#include "config.hpp"
#include "popup.hpp"
#include "sdpump.hpp"
#include "ui.hpp"

#include <cairo/cairo.h>
#include <dirent.h>
#include <linux/input-event-codes.h>
#include <pwd.h>
#include <csignal>
#include <sys/timerfd.h>
#include <sys/wait.h>
#include <systemd/sd-bus.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

constexpr const char* OBJ =
    "/org/mattbar/PolicyKit1/AuthenticationAgent";
constexpr const char* IFACE =
    "org.freedesktop.PolicyKit1.AuthenticationAgent";
constexpr const char* HELPER = "/usr/lib/polkit-1/polkit-agent-helper-1";
constexpr int DLG_W = 380, DLG_H = 168;

Bar*    g_bar = nullptr;
SdPump  pump;
sd_bus* sys  = nullptr;
bool    registered = false;
sd_bus_message* pending = nullptr;
std::string cookie, message, identity_user;
TextField field;
std::string err;
PopupWin win;
bool holding = false;
pid_t helper_pid = -1;
int   helper_in  = -1;
int   wait_fd    = -1;
bool  busy       = false;
bool  fp_ok      = false;

bool fingerprint_polkit() {
    auto has = [](const char* p) {
        FILE* f = fopen(p, "r");
        if (!f) return false;
        char buf[256];
        bool hit = false;
        while (fgets(buf, sizeof buf, f))
            if (strstr(buf, "pam_fprintd")) {
                hit = true;
                break;
            }
        fclose(f);
        return hit;
    };
    if (!has("/etc/pam.d/polkit-1") && !has("/etc/pam.d/polkit")) return false;
    return access("/usr/bin/fprintd-list", X_OK) == 0 ||
           access("/usr/bin/fprintd-verify", X_OK) == 0;
}

bool lid_closed() {
    DIR* d = opendir("/proc/acpi/button/lid");
    if (!d) return false;
    bool closed = false;
    while (dirent* e = readdir(d)) {
        if (e->d_name[0] == '.') continue;
        std::string p =
            std::string("/proc/acpi/button/lid/") + e->d_name + "/state";
        FILE* f = fopen(p.c_str(), "r");
        if (!f) continue;
        char buf[64];
        if (fgets(buf, sizeof buf, f) && strstr(buf, "closed")) closed = true;
        fclose(f);
    }
    closedir(d);
    return closed;
}

void hold(bool on) {
    if (!g_bar || on == holding) return;
    holding = on;
    g_bar->hold_open(on);
}

void close_dialog();

void reply_ok() {
    if (pending && sys) {
        sd_bus_reply_method_return(pending, "");
        pump.process();
    }
    if (pending) {
        sd_bus_message_unref(pending);
        pending = nullptr;
    }
}

void reply_cancel() {
    if (pending && sys) {
        sd_bus_error e = SD_BUS_ERROR_NULL;
        sd_bus_error_set(&e, "org.freedesktop.PolicyKit1.Error.Cancelled",
                         "Authentication cancelled");
        sd_bus_reply_method_error(pending, &e);
        sd_bus_error_free(&e);
        pump.process();
    }
    if (pending) {
        sd_bus_message_unref(pending);
        pending = nullptr;
    }
}

void paint(cairo_t* cr) {
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    cairo_set_source_rgba(cr, cfg.c_bg.r, cfg.c_bg.g, cfg.c_bg.b,
                          std::max(cfg.c_bg.a, 0.96));
    cairo_rectangle(cr, 0.5, 0.5, DLG_W - 1, DLG_H - 1);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, cfg.c_ws_bg.r, cfg.c_ws_bg.g, cfg.c_ws_bg.b, 1);
    cairo_set_line_width(cr, 1);
    cairo_stroke(cr);
    cairo_select_font_face(cr, cfg.font.c_str(), CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, cfg.shell_font_size > 0 ? cfg.shell_font_size
                                                    : cfg.font_size);
    cairo_font_extents_t fe;
    cairo_font_extents(cr, &fe);
    auto say = [&](double x, double y, const std::string& s, const Color& c) {
        cairo_set_source_rgba(cr, c.r, c.g, c.b, 1);
        cairo_move_to(cr, x, y + (fe.ascent - fe.descent) / 2.0);
        cairo_show_text(cr, s.c_str());
    };
    std::string title = message.empty() ? "Authentication is required" : message;
    if (title.size() > 54) title = title.substr(0, 51) + "...";
    say(16, 22, title, cfg.c_fg);
    field.password = true;
    field.focused  = true;
    const char* ph = busy ? "\u2026"
                     : (fp_ok && !lid_closed() ? "password or fingerprint"
                                               : "password");
    field.draw(cr, 16, 48, DLG_W - 32, 34, ph);
    if (!err.empty()) say(16, 96, err, cfg.c_urgent);
    else if (fp_ok && !lid_closed() && helper_pid > 0)
        say(16, 96, "scan fingerprint", cfg.c_dim);
    // buttons
    cairo_set_source_rgba(cr, cfg.c_ws_bg.r, cfg.c_ws_bg.g, cfg.c_ws_bg.b, 1);
    cairo_rectangle(cr, 16, DLG_H - 44, 90, 28);
    cairo_fill(cr);
    say(34, DLG_H - 30, "Cancel", cfg.c_fg);
    cairo_set_source_rgba(cr, cfg.c_accent.r, cfg.c_accent.g, cfg.c_accent.b, 1);
    cairo_rectangle(cr, DLG_W - 16 - 120, DLG_H - 44, 120, 28);
    cairo_fill(cr);
    Color on = contrast_on(cfg.c_accent);
    say(DLG_W - 16 - 108, DLG_H - 30, "Authenticate", on);
}

void start_helper(const std::string& pw) {
    int p[2];
    if (pipe(p) != 0) {
        busy = false;
        err  = "helper failed";
        win.draw();
        return;
    }
    pid_t pid = fork();
    if (pid == 0) {
        close(p[1]);
        dup2(p[0], 0);
        close(p[0]);
        execl(HELPER, "polkit-agent-helper-1", identity_user.c_str(),
              cookie.c_str(), (char*)nullptr);
        _exit(127);
    }
    close(p[0]);
    if (pid < 0) {
        close(p[1]);
        busy = false;
        err  = "fork failed";
        win.draw();
        return;
    }
    if (!pw.empty()) {
        std::string line = pw + "\n";
        (void)!write(p[1], line.c_str(), line.size());
        close(p[1]);
        helper_in = -1;
    } else {
        helper_in = p[1];
    }
    helper_pid = pid;
    if (wait_fd < 0 && g_bar) {
        wait_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
        if (wait_fd >= 0)
            g_bar->add_fd(
                wait_fd,
                [](uint32_t) {
                    uint64_t x;
                    while (read(wait_fd, &x, sizeof x) > 0) {}
                    if (helper_pid <= 0) return;
                    int st = 0;
                    pid_t r = waitpid(helper_pid, &st, WNOHANG);
                    if (r != helper_pid) return;
                    helper_pid = -1;
                    busy       = false;
                    if (helper_in >= 0) {
                        close(helper_in);
                        helper_in = -1;
                    }
                    bool ok = WIFEXITED(st) && WEXITSTATUS(st) == 0;
                    if (ok) {
                        reply_ok();
                        close_dialog();
                    } else {
                        err = "incorrect password";
                        if (fp_ok && !lid_closed()) start_helper("");
                        if (win.surf) win.draw();
                    }
                },
                "polkit-helper");
    }
    if (wait_fd >= 0) {
        itimerspec ts{};
        ts.it_interval.tv_nsec = 50 * 1000000L;
        ts.it_value.tv_nsec    = 50 * 1000000L;
        timerfd_settime(wait_fd, 0, &ts, nullptr);
    }
}

void show_dialog() {
    if (!g_bar) return;
    field.clear();
    err.clear();
    busy = false;
    fp_ok = fingerprint_polkit();
    win.centered = true;
    win.kb_mode  = 1;
    win.paint    = [](cairo_t* cr) { paint(cr); };
    win.click    = [](double x, double y, int btn) {
        if (btn != BTN_LEFT || busy) return;
        if (y >= DLG_H - 48 && x < 120) {
            reply_cancel();
            close_dialog();
        } else if (y >= DLG_H - 48 && x > DLG_W - 140) {
            // submit via key path
            Bar::KeyEvent e;
            e.pressed = true;
            e.keysym  = 0xff0d;
            win.pkey(e);
        }
    };
    win.pkey = [](const Bar::KeyEvent& e) {
        if (!e.pressed || busy) return;
        if (e.escape()) {
            reply_cancel();
            close_dialog();
            return;
        }
        if (e.enter()) {
            if (cookie.empty() || identity_user.empty()) return;
            if (field.text.empty() && !(fp_ok && helper_pid > 0)) return;
            busy = true;
            err.clear();
            win.draw();
            if (helper_pid > 0 && helper_in >= 0 && !field.text.empty()) {
                std::string pw = field.text + "\n";
                (void)!write(helper_in, pw.c_str(), pw.size());
                close(helper_in);
                helper_in = -1;
                field.clear();
                return;
            }
            start_helper(field.text);
            field.clear();
            return;
        }
        if (field.handle(e)) {
            err.clear();
            win.draw();
        }
    };
    hold(true);
    win.ensure(*g_bar, 0, 0, 0, 0, 0, "mattbar-polkit", DLG_W, DLG_H,
               g_bar->primary_output());
    win.draw();
    if (fp_ok && !lid_closed() && !cookie.empty() && !identity_user.empty())
        start_helper("");
}

void close_dialog() {
    if (helper_pid > 0) {
        kill(helper_pid, SIGTERM);
        waitpid(helper_pid, nullptr, 0);
        helper_pid = -1;
    }
    if (helper_in >= 0) {
        close(helper_in);
        helper_in = -1;
    }
    hold(false);
    win.destroy();
    field.clear();
    cookie.clear();
    busy = false;
}

static int m_begin(sd_bus_message* m, void*, sd_bus_error*) {
    if (pending) {
        reply_cancel();
        close_dialog();
    }
    const char *action = nullptr, *msg = nullptr, *icon = nullptr;
    if (sd_bus_message_read(m, "sss", &action, &msg, &icon) < 0) return -EINVAL;
    message = msg ? msg : "Authentication is required";
    if (sd_bus_message_enter_container(m, 'a', "{ss}") < 0) return -EINVAL;
    while (sd_bus_message_enter_container(m, 'e', "ss") > 0)
        sd_bus_message_skip(m, "ss"), sd_bus_message_exit_container(m);
    sd_bus_message_exit_container(m);
    const char* ck = nullptr;
    if (sd_bus_message_read(m, "s", &ck) < 0) return -EINVAL;
    cookie = ck ? ck : "";
    identity_user.clear();
    uid_t me = getuid();
    if (sd_bus_message_enter_container(m, 'a', "(sa{sv})") >= 0) {
        while (sd_bus_message_enter_container(m, 'r', "sa{sv}") > 0) {
            const char* kind = nullptr;
            sd_bus_message_read(m, "s", &kind);
            if (sd_bus_message_enter_container(m, 'a', "{sv}") >= 0) {
                const char* key = nullptr;
                while (sd_bus_message_enter_container(m, 'e', "sv") > 0) {
                    sd_bus_message_read(m, "s", &key);
                    if (kind && !strcmp(kind, "unix-user") && key &&
                        !strcmp(key, "uid")) {
                        uint32_t uid = 0;
                        sd_bus_message_read(m, "v", "u", &uid);
                        if (uid == me) {
                            if (passwd* pw = getpwuid(me))
                                identity_user = pw->pw_name;
                        }
                    } else {
                        sd_bus_message_skip(m, "v");
                    }
                    sd_bus_message_exit_container(m);
                }
                sd_bus_message_exit_container(m);
            }
            sd_bus_message_exit_container(m);
        }
        sd_bus_message_exit_container(m);
    }
    if (identity_user.empty()) {
        if (passwd* pw = getpwuid(me)) identity_user = pw->pw_name;
    }
    pending = sd_bus_message_ref(m);
    fprintf(stderr, "mattbar: polkit: auth '%s' (%s)\n",
            action ? action : "?", identity_user.c_str());
    show_dialog();
    return 1; // reply later
}

static int m_cancel(sd_bus_message* m, void*, sd_bus_error*) {
    const char* ck = nullptr;
    sd_bus_message_read(m, "s", &ck);
    if (ck && cookie == ck) {
        reply_cancel();
        close_dialog();
    }
    return sd_bus_reply_method_return(m, "");
}

bool register_agent() {
    if (!sys || registered) return registered;
    const char* sid = getenv("XDG_SESSION_ID");
    if (!sid || !*sid) {
        fprintf(stderr, "mattbar: polkit: no XDG_SESSION_ID\n");
        return false;
    }
    const char* loc = getenv("LANG");
    if (!loc || !*loc) loc = "C";
    sd_bus_error err = SD_BUS_ERROR_NULL;
    int r = sd_bus_call_method(
        sys, "org.freedesktop.PolicyKit1",
        "/org/freedesktop/PolicyKit1/Authority",
        "org.freedesktop.PolicyKit1.Authority", "RegisterAuthenticationAgent",
        &err, nullptr, "(sa{sv})ss", "unix-session", 1, "session-id", "s", sid,
        loc, OBJ);
    if (r < 0) {
        fprintf(stderr, "mattbar: polkit: register failed (%s)\n",
                err.message ? err.message : strerror(-r));
        sd_bus_error_free(&err);
        return false;
    }
    sd_bus_error_free(&err);
    registered = true;
    fprintf(stderr, "mattbar: polkit: authentication agent registered\n");
    return true;
}

void unregister_agent() {
    if (pending) {
        reply_cancel();
        close_dialog();
    }
    if (!sys || !registered) return;
    const char* sid = getenv("XDG_SESSION_ID");
    if (sid && *sid) {
        sd_bus_error err = SD_BUS_ERROR_NULL;
        sd_bus_call_method(sys, "org.freedesktop.PolicyKit1",
                           "/org/freedesktop/PolicyKit1/Authority",
                           "org.freedesktop.PolicyKit1.Authority",
                           "UnregisterAuthenticationAgent", &err, nullptr,
                           "(sa{sv})s", "unix-session", 1, "session-id", "s",
                           sid, OBJ);
        sd_bus_error_free(&err);
    }
    registered = false;
    fprintf(stderr, "mattbar: polkit: authentication agent unregistered\n");
}

int retry_fd = -1;
int retry_n  = 0;

bool ensure_bus();

void arm_register_retry() {
    if (!g_bar) return;
    if (retry_n >= 8) {
        fprintf(stderr,
                "mattbar: polkit: giving up after %d register attempts "
                "(another agent may still own the session)\n",
                retry_n);
        return;
    }
    if (retry_fd < 0) {
        retry_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
        if (retry_fd < 0) return;
        g_bar->add_fd(
            retry_fd,
            [](uint32_t) {
                uint64_t x;
                while (read(retry_fd, &x, sizeof x) > 0) {}
                if (!cfg.quickshell_shutdown || registered) return;
                if (!ensure_bus()) {
                    arm_register_retry();
                    return;
                }
                if (register_agent()) {
                    retry_n = 0;
                    return;
                }
                arm_register_retry();
            },
            "polkit-retry");
    }
    int ms = std::min(4000, 400 * (1 << std::min(retry_n, 3)));
    retry_n++;
    itimerspec ts{};
    ts.it_value.tv_sec  = ms / 1000;
    ts.it_value.tv_nsec = (ms % 1000) * 1000000L;
    timerfd_settime(retry_fd, 0, &ts, nullptr);
}

bool ensure_bus() {
    if (sys) return true;
    if (!g_bar) return false;
    sd_bus* b = nullptr;
    if (sd_bus_open_system(&b) < 0) {
        fprintf(stderr, "mattbar: polkit: system bus unavailable\n");
        return false;
    }
    sys = b;
    static const sd_bus_vtable vt[] = {
        SD_BUS_VTABLE_START(0),
        SD_BUS_METHOD("BeginAuthentication", "sssa{ss}sa(sa{sv})", "", m_begin,
                      0),
        SD_BUS_METHOD("CancelAuthentication", "s", "", m_cancel, 0),
        SD_BUS_VTABLE_END,
    };
    sd_bus_add_object_vtable(sys, nullptr, OBJ, IFACE, vt, nullptr);
    pump.attach(*g_bar, sys, "polkit");
    pump.process();
    return true;
}

} // namespace

void polkit_init(Bar& bar) { g_bar = &bar; }

void polkit_apply() {
    if (!g_bar) return;
    if (!cfg.quickshell_shutdown) {
        retry_n = 0;
        if (retry_fd >= 0) {
            itimerspec off{};
            timerfd_settime(retry_fd, 0, &off, nullptr);
        }
        unregister_agent();
        return;
    }
    if (registered) return;
    if (!ensure_bus() || !register_agent())
        arm_register_retry();
    else
        retry_n = 0;
}

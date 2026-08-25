#include "ctl.hpp"
#include "bar.hpp"
#include "config.hpp"
#include "modules.hpp"
#include "notify.hpp"
#include "qs_plugins.hpp"
#include "shell.hpp"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <vector>

static std::string trimline(const char* s);

std::string ctl_socket_path() {
    const char* rt = getenv("XDG_RUNTIME_DIR");
    return std::string(rt && *rt ? rt : "/tmp") + "/mattbar.sock";
}

// ---------------------------------------------------------------------------
// Server
// ---------------------------------------------------------------------------
void CtlServer::init(Bar& bar) {
    bar_  = &bar;
    path_ = ctl_socket_path();
    listen_fd_ =
        socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (listen_fd_ < 0) return;
    sockaddr_un a{};
    a.sun_family = AF_UNIX;
    strncpy(a.sun_path, path_.c_str(), sizeof(a.sun_path) - 1);
    // Only reclaim the socket path if it is STALE. Unconditionally
    // unlinking stole the path from a LIVE instance — the exact
    // situation of a manual debug run (./mattbar) alongside the enabled
    // systemd unit — leaving the service copy silently uncontactable.
    // A live owner answers a connect; a stale path refuses it.
    int probe = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (probe >= 0) {
        if (connect(probe, (sockaddr*)&a, sizeof a) == 0) {
            close(probe);
            fprintf(stderr,
                    "mattbar: ANOTHER INSTANCE is already running (it "
                    "answered on %s).\n"
                    "mattbar: this copy continues WITHOUT the control "
                    "socket. If the other copy is the systemd unit, stop "
                    "it first for a clean debug run:\n"
                    "mattbar:     systemctl --user stop mattbar\n",
                    path_.c_str());
            close(listen_fd_);
            listen_fd_ = -1;
            return;
        }
        close(probe);
    }
    unlink(path_.c_str()); // stale socket from a previous run
    if (bind(listen_fd_, (sockaddr*)&a, sizeof a) < 0 ||
        listen(listen_fd_, 4) < 0) {
        // An unwritable runtime dir: the bar works fine without the
        // socket, so just say so once and move on.
        fprintf(stderr, "mattbar: ctl socket unavailable at %s\n",
                path_.c_str());
        close(listen_fd_);
        listen_fd_ = -1;
        return;
    }
    bar.add_fd(listen_fd_, [this](uint32_t) {
        // Requests are tiny and local; one read/one write per connection
        // keeps this a leaf in the event loop, never a stall.
        int c = accept4(listen_fd_, nullptr, nullptr,
                        SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (c < 0) return;
        // Image-selector rows travel as base64 on this socket; 4k was
        // not enough for a theme's wallpaper set.
        char        buf[8192];
        std::string req;
        for (int spin = 0; spin < 400; ++spin) { // ~40ms budget
            ssize_t n = read(c, buf, sizeof buf);
            if (n > 0) {
                req.append(buf, n);
                if (req.find('\n') != std::string::npos ||
                    req.size() > 1024 * 1024)
                    break;
                continue;
            }
            if (n == 0 || (n < 0 && errno != EAGAIN)) break;
            usleep(100);
        }
        if (!req.empty()) {
            std::string reply = handle(trimline(req.c_str()));
            reply += "\n";
            (void)!write(c, reply.c_str(), reply.size());
        }
        close(c);
    }, "ctl-socket");
}

CtlServer::~CtlServer() {
    if (listen_fd_ >= 0) close(listen_fd_);
    if (!path_.empty()) unlink(path_.c_str());
}

static std::string trimline(const char* s) {
    std::string out = s;
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' ||
                            out.back() == ' '))
        out.pop_back();
    return out;
}

std::string CtlServer::handle(const std::string& line) {
    std::istringstream ss(line);
    std::string        cmd, arg;
    ss >> cmd;
    std::string rest;
    std::getline(ss, rest);
    // trim leading space on rest; arg is the first token of rest for
    // the original one-argument commands.
    while (!rest.empty() && rest[0] == ' ') rest.erase(rest.begin());
    {
        std::istringstream a(rest);
        a >> arg;
    }
    auto* nd = notify_daemon();
    if (auto* sh = mattbar_shell()) {
        if (cmd == "shell" || cmd == "lock" || cmd == "osd" || cmd == "media" ||
            cmd == "idle" || cmd == "nightlight" || cmd == "background" ||
            cmd == "notifications" || cmd == "image-selector" ||
            cmd.rfind("omarchy.", 0) == 0)
            return sh->handle(cmd, rest);
    }

    if (cmd == "dnd") {
        if (!nd) return "error: no notification daemon";
        bool cur = nd->dnd();
        if (arg == "on" && !cur) nd->toggle_dnd();
        else if (arg == "off" && cur) nd->toggle_dnd();
        else if (arg == "toggle" || arg.empty()) nd->toggle_dnd();
        else if (arg != "status") return "usage: dnd [on|off|toggle|status]";
        bar_->request_draw(); // the bell's slash tracks this immediately
        return nd->dnd() ? "dnd on" : "dnd off";
    }
    if (cmd == "dismiss") { if (nd) nd->dismiss_last(); return "ok"; }
    if (cmd == "dismiss-all") { if (nd) nd->dismiss_all(); return "ok"; }
    if (cmd == "invoke") { if (nd) nd->invoke_last(); return "ok"; }
    if (cmd == "restore") { if (nd) nd->restore_last(); return "ok"; }

    if (cmd == "profile") {
        std::string act = power_ctl_active();
        if (act.empty())
            return "error: power-profiles-daemon not available";
        if (arg.empty() || arg == "status") {
            std::string out = "active: " + act + "\navailable:";
            for (auto& p : power_ctl_profiles()) out += " " + p;
            return out;
        }
        if (!power_ctl_set(arg)) {
            std::string out = "error: unknown profile '" + arg +
                              "' (available:";
            for (auto& p : power_ctl_profiles()) out += " " + p;
            return out + ")";
        }
        return "profile " + arg;
    }

    if (cmd == "pin") {
        bool cur = bar_->pinned();
        if (arg == "on" && !cur) bar_->toggle_pinned();
        else if (arg == "off" && cur) bar_->toggle_pinned();
        else if (arg == "toggle" || arg.empty()) bar_->toggle_pinned();
        else if (arg != "status") return "usage: pin [on|off|toggle|status]";
        return bar_->pinned() ? "pinned" : "unpinned";
    }
    if (cmd == "reveal") {
        // Show every bar; without a pointer inside they collapse again
        // after the normal hide delay, exactly like a hover would.
        bar_->ctl_reveal();
        return "revealed";
    }
    if (cmd == "hide") {
        bar_->ctl_hide();
        return "hidden";
    }
    if (cmd == "status") {
        std::string out = "mattbar " + std::string(MATTBAR_VERSION);
        out += bar_->pinned() ? " pinned" : "";
        if (nd) {
            out += nd->dnd() ? " dnd" : "";
            out += " notifications=" + std::to_string(nd->history_count());
        }
        std::string act = power_ctl_active();
        if (!act.empty()) out += " profile=" + act;
        out += qs_plugins_running() ? " sidecar=up" : " sidecar=down";
        return out;
    }
    if (cmd == "plugins") {
        if (arg == "stop" || arg == "off" || arg == "kill") {
            qs_plugins_shutdown();
            return "plugins stopped";
        }
        if (arg == "status" || arg.empty())
            return qs_plugins_running() ? "sidecar up" : "sidecar down";
        return "usage: plugins [status|stop]";
    }
    if (cmd == "agents") {
        if (arg == "pick") {
            spawn_detached("omarchy-agent --pick");
            return "ok";
        }
        if (arg == "toggle" || arg == "click" || arg.empty()) {
            agents_hotkey();
            if (bar_) bar_->request_draw();
            return "ok";
        }
        return "usage: agents [toggle|pick]";
    }
    if (cmd == "help" || cmd.empty())
        return "commands: dnd [on|off|toggle|status], dismiss, dismiss-all,\n"
               "invoke, restore, profile [name], pin [on|off|toggle|status],\n"
               "reveal, hide, status, plugins [status|stop],\n"
               "agents [toggle|pick],\n"
               "shell ping|toggle|summon|hide <id> [payload],\n"
               "notifications dismissOne|dismissAll|invokeLast|toggleDnd,\n"
               "osd show <json>, media playPause|next|previous,\n"
               "nightlight toggle|enable|disable|status|refresh";
    return "error: unknown command '" + cmd + "' (try help)";
}

// ---------------------------------------------------------------------------
// Client
// ---------------------------------------------------------------------------
int ctl_client(int argc, char** argv) {
    std::string line;
    for (int i = 0; i < argc; ++i) {
        if (i) line += " ";
        line += argv[i];
    }
    if (line.empty()) line = "help";
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) { perror("mattbarctl: socket"); return 1; }
    sockaddr_un a{};
    a.sun_family = AF_UNIX;
    std::string p = ctl_socket_path();
    strncpy(a.sun_path, p.c_str(), sizeof(a.sun_path) - 1);
    if (connect(fd, (sockaddr*)&a, sizeof a) < 0) {
        fprintf(stderr, "mattbarctl: cannot reach %s (is mattbar running?)\n",
                p.c_str());
        close(fd);
        return 1;
    }
    line += "\n";
    (void)!write(fd, line.c_str(), line.size());
    char    buf[2048];
    ssize_t n;
    std::string reply;
    while ((n = read(fd, buf, sizeof buf)) > 0) reply.append(buf, n);
    close(fd);
    fputs(reply.c_str(), stdout);
    return reply.rfind("error", 0) == 0 ? 1 : 0;
}

int omarchy_shell_client(int argc, char** argv) {
    int i = 0;
    if (i < argc && argv[i] && std::string(argv[i]) == "-q") ++i;
    if (i >= argc) return ctl_client(0, nullptr);
    std::vector<char*> toks;
    for (; i < argc; ++i) toks.push_back(argv[i]);
    // `omarchy-shell shell toggle omarchy.menu` with no payload: add {}
    if (toks.size() == 3 && std::string(toks[0]) == "shell" &&
        (std::string(toks[1]) == "toggle" || std::string(toks[1]) == "summon")) {
        static char empty[] = "{}";
        toks.push_back(empty);
    }
    return ctl_client((int)toks.size(), toks.data());
}

int omarchy_menu_client(int argc, char** argv) {
    std::string verb = "toggle";
    std::string route = "root";
    if (argc >= 1 && argv[0] && *argv[0]) verb = argv[0];
    if (argc >= 2 && argv[1] && *argv[1]) route = argv[1];
    if (verb == "close" || verb == "hide") {
        char t[] = "shell", m[] = "hide", id[] = "omarchy.menu";
        char* a[] = {t, m, id};
        return ctl_client(3, a);
    }
    std::string payload = std::string("{\"menu\":\"") + route + "\"}";
    char t[] = "shell", m[] = "toggle", id[] = "omarchy.menu";
    std::vector<char> p(payload.begin(), payload.end());
    p.push_back(0);
    if (verb == "summon") {
        char sm[] = "summon";
        char* a[] = {t, sm, id, p.data()};
        return ctl_client(4, a);
    }
    char* a[] = {t, m, id, p.data()};
    return ctl_client(4, a);
}

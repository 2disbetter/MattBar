#include "ctl.hpp"
#include "bar.hpp"
#include "config.hpp"
#include "modules.hpp"
#include "notify.hpp"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <sstream>
#include <vector>

static std::string trimline(const char* s);

std::string ctl_socket_path() {
    const char* rt = getenv("XDG_RUNTIME_DIR");
    return std::string(rt && *rt ? rt : "/tmp") + "/mattbar.sock";
}

// Server
void CtlServer::init(Bar& bar) {
    bar_  = &bar;
    path_ = ctl_socket_path();
    listen_fd_ =
        socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (listen_fd_ < 0) return;
    sockaddr_un a{};
    a.sun_family = AF_UNIX;
    strncpy(a.sun_path, path_.c_str(), sizeof(a.sun_path) - 1);
    // Only reclaim path if stale.
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
    unlink(path_.c_str()); // stale socket from previous run
    if (bind(listen_fd_, (sockaddr*)&a, sizeof a) < 0 ||
        listen(listen_fd_, 4) < 0) {
        // Unwritable runtime dir: bar works without socket.
        fprintf(stderr, "mattbar: ctl socket unavailable at %s\n",
                path_.c_str());
        close(listen_fd_);
        listen_fd_ = -1;
        return;
    }
    bar.add_fd(listen_fd_, [this](uint32_t) {
        // Requests are tiny/local; one read/write per connection.
        int c = accept4(listen_fd_, nullptr, nullptr,
                        SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (c < 0) return;
        char    buf[512];
        ssize_t n = -1;
        for (int spin = 0; spin < 200; ++spin) { // ~20ms budget
            n = read(c, buf, sizeof buf - 1);
            if (n >= 0 || errno != EAGAIN) break;
            usleep(100);
        }
        if (n > 0) {
            buf[n] = 0;
            std::string reply = handle(trimline(buf));
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
    ss >> cmd >> arg;
    auto* nd = notify_daemon();

    if (cmd == "dnd") {
        if (!nd) return "error: no notification daemon";
        bool cur = nd->dnd();
        if (arg == "on" && !cur) nd->toggle_dnd();
        else if (arg == "off" && cur) nd->toggle_dnd();
        else if (arg == "toggle" || arg.empty()) nd->toggle_dnd();
        else if (arg != "status") return "usage: dnd [on|off|toggle|status]";
        bar_->request_draw(); // bell slash tracks this immediately
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
        // Show every bar; collapse after normal hide delay if no pointer.
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
        return out;
    }
    if (cmd == "help" || cmd.empty())
        return "commands: dnd [on|off|toggle|status], dismiss, dismiss-all,\n"
               "invoke, restore, profile [name], pin [on|off|toggle|status],\n"
               "reveal, hide, status";
    return "error: unknown command '" + cmd + "' (try help)";
}

// Client
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

#include "spawn.hpp"
#include "bar.hpp"
#include "config.hpp"

#include <fcntl.h>
#include <poll.h>
#include <time.h>
#include <signal.h>
#include <spawn.h>
#include <sys/syscall.h>
#include <sys/timerfd.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <utility>
#include <vector>
#include <cstdint>
#include <string>
#include <functional>

#ifndef SYS_pidfd_open
#define SYS_pidfd_open 434 // same number on every architecture
#endif

extern char** environ;

namespace {

Bar* g_bar     = nullptr;
int  g_watched = 0; // pidfd-watched children in flight

struct Polled {
    pid_t                    pid;
    std::function<void(int)> cb;
};
std::vector<Polled> g_polled; // no-pidfd fallback + pre-init queue
int                 g_poll_fd = -1;

int status_of(int ws) { return WIFEXITED(ws) ? WEXITSTATUS(ws) : -1; }

void arm_poll(bool on) {
    if (g_poll_fd < 0) return;
    itimerspec ts{};
    if (on) {
        ts.it_value.tv_nsec    = 250 * 1000000L;
        ts.it_interval.tv_nsec = 250 * 1000000L;
    }
    timerfd_settime(g_poll_fd, 0, &ts, nullptr);
}

void poll_children() {
    std::vector<std::pair<std::function<void(int)>, int>> done;
    for (auto it = g_polled.begin(); it != g_polled.end();) {
        int   ws = 0;
        pid_t r  = waitpid(it->pid, &ws, WNOHANG);
        if (r == 0) { ++it; continue; } // still running
        done.emplace_back(std::move(it->cb), r == it->pid ? status_of(ws) : -1);
        it = g_polled.erase(it);
    }
    if (g_polled.empty()) arm_poll(false);
    for (auto& [cb, st] : done) // after erasing: a callback may spawn again
        if (cb) cb(st);
}

// Signals a child must NOT inherit in their MattBar state. SIG_IGN and a
// blocked mask both survive exec. Start at SIGRTMIN: 32/33 are
// glibc-internal.
void clean_signals(posix_spawnattr_t* a, short* flags) {
    sigset_t empty;
    sigemptyset(&empty);
    posix_spawnattr_setsigmask(a, &empty);
    sigset_t def;
    sigemptyset(&def);
    for (int s : {SIGPIPE, SIGCHLD, SIGHUP, SIGINT, SIGQUIT, SIGTERM, SIGUSR1,
                  SIGUSR2, SIGALRM})
        sigaddset(&def, s);
    for (int s = SIGRTMIN; s <= SIGRTMAX; ++s) sigaddset(&def, s);
    posix_spawnattr_setsigdefault(a, &def);
    *flags |= POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF;
}

pid_t do_spawn(const char* path, const char* const argv[], bool use_path,
               const SpawnOpts& o) {
    posix_spawnattr_t          attr;
    posix_spawn_file_actions_t fa;
    posix_spawnattr_init(&attr);
    posix_spawn_file_actions_init(&fa);
    short flags = 0;
    clean_signals(&attr, &flags);
    if (o.new_session) {
        flags |= POSIX_SPAWN_SETSID; // implies its own process group
    } else if (o.new_pgroup) {
        posix_spawnattr_setpgroup(&attr, 0);
        flags |= POSIX_SPAWN_SETPGROUP;
    }
    posix_spawnattr_setflags(&attr, flags);
    if (o.devnull_io) {
        posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
        posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
        posix_spawn_file_actions_adddup2(&fa, 1, 2);
    }
    if (o.stdin_fd >= 0) posix_spawn_file_actions_adddup2(&fa, o.stdin_fd, 0);
    if (o.stdout_fd >= 0) // dup2 clears O_CLOEXEC on the new fd 1
        posix_spawn_file_actions_adddup2(&fa, o.stdout_fd, 1);
    if (o.merge_stderr && (o.stdout_fd >= 0 || o.devnull_io))
        posix_spawn_file_actions_adddup2(&fa, 1, 2);
#if defined(__GLIBC__) && \
    (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 34))
    // Children get stdin/stdout/stderr and nothing else. Everything MattBar
    // opens itself is close-on-exec, but not everything it links is: stdio
    // FILEs and std::fstreams, and files the image worker thread has open
    // while the main loop spawns. Without this, a launched app or the
    // long-lived Quickshell sidecar could keep such an fd for its lifetime.
    posix_spawn_file_actions_addclosefrom_np(&fa, 3);
#endif

    pid_t pid = -1;
    int   rc  = use_path
                    ? posix_spawnp(&pid, path, &fa, &attr,
                                   const_cast<char* const*>(argv), environ)
                    : posix_spawn(&pid, path, &fa, &attr,
                                  const_cast<char* const*>(argv), environ);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&attr);
    return rc == 0 ? pid : -1;
}

bool in_path(const char* name) {
    const char* path = getenv("PATH");
    if (!path) return false;
    std::string p = path;
    size_t      i = 0;
    for (;;) {
        size_t      j   = p.find(':', i);
        std::string dir = p.substr(i, j == std::string::npos ? j : j - i);
        if (!dir.empty() && access((dir + "/" + name).c_str(), X_OK) == 0)
            return true;
        if (j == std::string::npos) return false;
        i = j + 1;
    }
}

} // namespace

void spawn_init(Bar& bar) {
    g_bar     = &bar;
    g_poll_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    if (g_poll_fd >= 0) {
        bar.add_fd(g_poll_fd, [](uint32_t) {
            uint64_t n;
            while (read(g_poll_fd, &n, sizeof n) > 0) {}
            poll_children();
        }, "child-poll");
        if (!g_polled.empty()) arm_poll(true);
    }
}

pid_t spawn_sh(const std::string& cmd, const SpawnOpts& o) {
    const char* argv[] = {"sh", "-c", cmd.c_str(), nullptr};
    return do_spawn("/bin/sh", argv, false, o);
}

pid_t spawn_argv(const char* const argv[], const SpawnOpts& o) {
    return do_spawn(argv[0], argv, true, o);
}

int pidfd_for(pid_t pid) {
    return static_cast<int>(syscall(SYS_pidfd_open, pid, 0)); // O_CLOEXEC set
}

int children_pending() {
    return g_watched + static_cast<int>(g_polled.size());
}

void watch_child(pid_t pid, std::function<void(int)> cb, int pidfd) {
    if (pid <= 0) return;
    if (pidfd < 0 && g_bar) pidfd = pidfd_for(pid);
    if (pidfd >= 0 && g_bar) {
        ++g_watched;
        // The bar copies a callback before invoking it, so remove_fd() from
        // inside is safe.
        g_bar->add_fd(pidfd, [pid, pidfd, cb = std::move(cb)](uint32_t) {
            int   ws = 0;
            pid_t r  = waitpid(pid, &ws, WNOHANG);
            if (r == 0) return; // not exited yet (spurious wake)
            g_bar->remove_fd(pidfd);
            close(pidfd);
            --g_watched;
            if (cb) cb(r == pid ? status_of(ws) : -1);
        }, "child-exit");
        return;
    }
    if (pidfd >= 0) close(pidfd);
    g_polled.push_back({pid, std::move(cb)});
    arm_poll(true); // no-op before spawn_init; armed there instead
}

std::string launch_prefix() {
    const std::string& v = cfg.launch_wrapper;
    if (v == "none") return "";
    if (!v.empty() && v != "auto") return v + " ";
    // Only a process managed by systemd shares a cgroup that a unit
    // restart kills; INVOCATION_ID is set for exactly those (and inherited
    // by children of e.g. a uwsm-managed compositor, where a scope is
    // equally right).
    if (!getenv("INVOCATION_ID")) return "";
    static const bool have = in_path("systemd-run");
    return have ? "systemd-run --user --scope --collect --quiet "
                  "--slice=app-graphical.slice -- "
                : "";
}

std::string sh_quote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    return out + "'";
}

// ---------------------------------------------------------------------------
void child_reset_signals() {
    struct sigaction dfl {};
    dfl.sa_handler = SIG_DFL;
    sigemptyset(&dfl.sa_mask);
    for (int s : {SIGPIPE, SIGCHLD, SIGHUP, SIGINT, SIGQUIT, SIGTERM, SIGUSR1,
                  SIGUSR2, SIGALRM})
        sigaction(s, &dfl, nullptr);
    for (int s = SIGRTMIN; s <= SIGRTMAX; ++s) sigaction(s, &dfl, nullptr);
    sigset_t empty;
    sigemptyset(&empty);
    sigprocmask(SIG_SETMASK, &empty, nullptr);
}

void spawn_detached(const std::string& c) {
    if (c.empty()) return;
    SpawnOpts o;
    o.devnull_io  = true;
    o.new_session = true;
    // `a; b` / `a && b` must run inside the wrapper as one unit, so the
    // command is re-quoted into a single `sh -c` argument behind it.
    const std::string pre = launch_prefix();
    pid_t pid = pre.empty() ? spawn_sh(c, o)
                            : spawn_sh(pre + "sh -c " + sh_quote(c), o);
    if (pid > 0) watch_child(pid);
}

void spawn_helper(const std::string& c) {
    if (c.empty()) return;
    SpawnOpts o;
    o.devnull_io = true;
    o.new_pgroup = true;
    pid_t pid = spawn_sh(c, o);
    if (pid > 0) watch_child(pid);
}

namespace {
long mono_ms_now() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}
} // namespace

std::string run_capture_bounded(const std::string& cmd, int timeout_ms,
                                int* status, const std::string* input) {
    if (status) *status = -1;
    int out[2], in[2] = {-1, -1};
    if (pipe2(out, O_CLOEXEC) != 0) return {};
    if (input && pipe2(in, O_CLOEXEC) != 0) {
        close(out[0]);
        close(out[1]);
        return {};
    }
    SpawnOpts o;
    o.stdout_fd  = out[1];
    o.stdin_fd   = input ? in[0] : -1;
    o.new_pgroup = true;
    pid_t pid = spawn_sh(cmd, o);
    close(out[1]);
    if (in[0] >= 0) close(in[0]);
    if (pid < 0) {
        close(out[0]);
        if (in[1] >= 0) close(in[1]);
        return {};
    }
    fcntl(out[0], F_SETFL, O_NONBLOCK);
    if (in[1] >= 0) fcntl(in[1], F_SETFL, O_NONBLOCK);
    const long  deadline = mono_ms_now() + (timeout_ms > 0 ? timeout_ms : 0);
    std::string res;
    size_t      in_off = 0;
    int         pfd    = pidfd_for(pid);
    bool        out_open = true, exited = false;
    int         ws = 0;
    while (true) {
        if (!out_open && in[1] < 0) break;
        long left = deadline - mono_ms_now();
        if (left <= 0) break;
        pollfd fds[2];
        int    nf = 0, io = -1, ii = -1;
        if (out_open) { io = nf; fds[nf++] = {out[0], POLLIN, 0}; }
        if (in[1] >= 0) { ii = nf; fds[nf++] = {in[1], POLLOUT, 0}; }
        int r = poll(fds, nf, (int)left);
        if (r < 0 && errno != EINTR) break;
        if (r <= 0) continue;
        if (ii >= 0 && fds[ii].revents) {
            if (fds[ii].revents & POLLOUT) {
                ssize_t n = write(in[1], input->data() + in_off,
                                  input->size() - in_off);
                if (n > 0) in_off += (size_t)n;
                if (n < 0 && errno != EAGAIN) in_off = input->size();
            } else {
                in_off = input->size(); // POLLERR/POLLHUP: reader gone
            }
            if (in_off >= input->size()) {
                close(in[1]);
                in[1] = -1;
            }
        }
        if (io >= 0 && fds[io].revents) {
            char    buf[4096];
            ssize_t n;
            while ((n = read(out[0], buf, sizeof buf)) > 0) res.append(buf, n);
            if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR))
                out_open = false;
        }
    }
    if (in[1] >= 0) close(in[1]);
    close(out[0]);
    // Output is done (or we gave up); give the process the rest of the
    // budget to exit so we can report its status.
    while (!exited) {
        pid_t w = waitpid(pid, &ws, WNOHANG);
        if (w == pid) { exited = true; break; }
        long left = deadline - mono_ms_now();
        if (left <= 0 || w < 0) break;
        if (pfd >= 0) {
            pollfd p{pfd, POLLIN, 0};
            poll(&p, 1, (int)left);
        } else {
            poll(nullptr, 0, (int)std::min(left, 10L));
        }
    }
    if (exited) {
        if (pfd >= 0) close(pfd);
        if (status && WIFEXITED(ws)) *status = WEXITSTATUS(ws);
    } else {
        kill(-pid, SIGKILL); // the whole pipeline, not just sh
        watch_child(pid, nullptr, pfd); // reaped later, off the hot path
        fprintf(stderr, "mattbar: command timed out after %d ms: %.80s\n",
                timeout_ms, cmd.c_str());
    }
    return res;
}

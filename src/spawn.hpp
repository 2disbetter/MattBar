#pragma once
// ---------------------------------------------------------------------------
// Process spawning and reaping — the one place MattBar creates children.
//
//  * posix_spawn instead of fork(): glibc implements it with
//    clone(CLONE_VM|CLONE_VFORK) — no page-table copy, no copy-on-write
//    fault storm in the bar after every spawn.
//  * Clean child state: MattBar blocks SIGCHLD/SIGRTMIN+n for its
//    signalfds and ignores SIGPIPE (as systemd does for every service).
//    Blocked masks and SIG_IGN dispositions SURVIVE exec, so apps launched
//    from the bar used to run with SIGPIPE ignored and signals blocked —
//    unlike the same app launched from a keybind. Children now start with
//    an empty mask and default dispositions.
//  * Reaping via pidfd: exit is an epoll event, so nothing ever blocks in
//    waitpid() on the event loop.
//  * App launches escape the bar's cgroup (see launch_prefix()).
// ---------------------------------------------------------------------------
#include <sys/types.h>

#include <functional>
#include <string>

class Bar;

// Called from Bar::init so reaping can use the event loop. Anything
// watched earlier is queued and picked up then.
void spawn_init(Bar& bar);

struct SpawnOpts {
    int  stdin_fd     = -1;    // dup2 onto fd 0 in the child (-1: inherit)
    int  stdout_fd    = -1;    // dup2 onto fd 1 in the child (-1: inherit)
    bool merge_stderr = false; // fd 2 -> wherever fd 1 goes
    bool devnull_io   = false; // stdin/stdout/stderr -> /dev/null
    bool new_pgroup   = false; // own process group: kill(-pid) reaches the tree
    bool new_session  = false; // setsid(): own session and process group
};

// posix_spawn `/bin/sh -c cmd` with clean signal state; pid or -1.
pid_t spawn_sh(const std::string& cmd, const SpawnOpts& o = {});
// Same for a direct argv (no shell, no quoting); PATH lookup like execvp.
pid_t spawn_argv(const char* const argv[], const SpawnOpts& o = {});

// pidfd for a child we spawned, or -1 (kernel < 5.3).
int pidfd_for(pid_t pid);

// Reap `pid` asynchronously; cb(exit status, or -1) runs on the event
// loop once it has exited. Pass an existing pidfd to hand over ownership,
// or -1 to have one opened. Without pidfd support a 250 ms WNOHANG poll —
// armed only while something is pending — does the job instead.
void watch_child(pid_t pid, std::function<void(int)> cb = nullptr,
                 int pidfd = -1);

// Number of children currently awaiting reap (diagnostics/tests).
int children_pending();

// Prefix for user-facing app launches, from cfg.launch_wrapper:
//  "auto" (default): if MattBar runs under systemd (INVOCATION_ID set) and
//    systemd-run exists, each launch gets its own transient scope in
//    app-graphical.slice — what Omarchy's uwsm-app does. Otherwise launched
//    apps sit in the bar's cgroup and die on every bar restart (watchdog,
//    crash, upgrade), including a power-menu screen locker.
//  "none": no wrapper.  Anything else: used verbatim as the prefix.
std::string launch_prefix();

// POSIX single-quote a string for sh.
std::string sh_quote(const std::string& s);

// User-facing launch (apps, terminals, the updater): fully detached, own
// session, wrapped with launch_prefix() so it survives a bar restart.
// Reaped asynchronously; never blocks.
void spawn_detached(const std::string& cmd);
// Internal fire-and-forget helper (hyprctl, pkill, playerctl, ...): no
// scope wrapper, stdio to /dev/null, own process group, reaped async.
void spawn_helper(const std::string& cmd);

// For children created with a bare fork() that do NOT exec (the PAM
// workers in lock.cpp): restore default dispositions and an empty mask.
// Otherwise such a child keeps MattBar's SIGTERM handler — and that
// handler writes the parent's stop eventfd, so killing the child stopped
// the bar. Async-signal-safe; call first thing after fork().
void child_reset_signals();

#include "audio.hpp"
#include "bar.hpp"

#include <fcntl.h>
#include <signal.h>
#include <sys/epoll.h>
#include <sys/prctl.h>
#include <sys/timerfd.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <string>

// ---------------------------------------------------------------------------
// One `pactl subscribe` for the whole process. pactl talks the PulseAudio
// client protocol, which PipeWire's pipewire-pulse serves natively, so this
// covers both stacks with zero new library dependencies. The process sleeps
// in its socket between events: an idle audio stack costs zero wakeups.
//
// Event filtering matches the OSD's hard-won rule: only "on sink #" /
// "on source #" lines are device state changes; sink-input/source-output
// lines are app streams starting and stopping, and reacting to those would
// mean a query burst every time any app opens audio.
// ---------------------------------------------------------------------------

AudioEvents& audio_events() {
    static AudioEvents a;
    return a;
}

int AudioEvents::subscribe(Bar& bar, Callback cb) {
    bar_ = &bar;
    if (fd_ < 0 && retry_fd_ < 0) start(bar);
    subs_.push_back({next_id_, std::move(cb)});
    return next_id_++;
}

void AudioEvents::unsubscribe(int id) {
    for (auto it = subs_.begin(); it != subs_.end(); ++it)
        if (it->id == id) {
            subs_.erase(it);
            break;
        }
    // The stream stays up even with zero subscribers: a sleeping pactl is
    // free, and the OSD toggling off/on shouldn't churn processes.
}

static void reap_orphan_pactl() {
    // KillMode=process does not signal leftover children. Older mattbar
    // runs left `pactl subscribe` in this cgroup; they only write on
    // audio events, so SIGPIPE never arrives and they sit forever.
    FILE* f = fopen("/proc/self/cgroup", "r");
    if (!f) return;
    char line[256] = {};
    std::string cg;
    while (fgets(line, sizeof line, f)) {
        char* p = strchr(line, ':');
        if (!p) continue;
        p = strchr(p + 1, ':');
        if (!p) continue;
        ++p;
        size_t n = strlen(p);
        while (n && (p[n - 1] == '\n' || p[n - 1] == '\r')) p[--n] = 0;
        if (n) {
            cg = p;
            break;
        }
    }
    fclose(f);
    if (cg.empty()) return;
    std::string path = "/sys/fs/cgroup" + cg + "/cgroup.procs";
    FILE* procs = fopen(path.c_str(), "r");
    if (!procs) return;
    pid_t me = getpid();
    int p = 0;
    while (fscanf(procs, "%d", &p) == 1) {
        if (p <= 1 || p == me) continue;
        char cmdp[64];
        snprintf(cmdp, sizeof cmdp, "/proc/%d/cmdline", p);
        FILE* c = fopen(cmdp, "r");
        if (!c) continue;
        char buf[128] = {};
        size_t n = fread(buf, 1, sizeof buf - 1, c);
        fclose(c);
        if (n < 6) continue;
        bool pactl = strncmp(buf, "pactl", 5) == 0 && buf[5] == 0;
        bool sub   = false;
        for (size_t i = 0; i + 1 < n; ++i)
            if (buf[i] == 0 && strncmp(buf + i + 1, "subscribe", 9) == 0)
                sub = true;
        if (pactl && sub) kill(p, SIGTERM);
    }
    fclose(procs);
}

void AudioEvents::start(Bar& bar) {
    reap_orphan_pactl();
    int p[2];
    if (pipe2(p, O_CLOEXEC) != 0) return;
    pid_t pid = fork();
    if (pid == 0) {
        prctl(PR_SET_PDEATHSIG, SIGTERM);
        if (getppid() == 1) _exit(0);
        dup2(p[1], 1);
        close(p[0]);
        close(p[1]);
        execlp("pactl", "pactl", "subscribe", (char*)nullptr);
        _exit(127);
    }
    close(p[1]);
    if (pid < 0) {
        close(p[0]);
        return;
    }
    pid_ = pid;
    fd_  = p[0];
    fcntl(fd_, F_SETFL, O_NONBLOCK);

    if (query_fd_ < 0) {
        query_fd_ = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
        bar.add_fd(query_fd_, [this](uint32_t) {
            uint64_t n;
            while (read(query_fd_, &n, sizeof n) > 0) {}
            bool sink = q_sink_, src = q_source_;
            q_sink_ = q_source_ = false;
            if (!sink && !src) return;
            for (auto& s : subs_) s.cb(sink, src);
        }, "audio-events");
    }

    bar.add_fd(fd_, [this](uint32_t ev) {
        char    buf[512];
        ssize_t n;
        bool    hit = false;
        while ((n = read(fd_, buf, sizeof buf - 1)) > 0) {
            buf[n] = 0;
            if (strstr(buf, "on sink #")) q_sink_ = hit = true;
            if (strstr(buf, "on source #")) q_source_ = hit = true;
        }
        if (ev & (EPOLLHUP | EPOLLERR)) {
            // pactl died (audio daemon restart, or it was never there).
            stop();
            schedule_restart();
            return;
        }
        if (hit) {
            attempts_ = 0; // a working stream clears the failure budget
            // Debounce: one callback per burst — a volume-key repeat or a
            // slider drag is dozens of events and must be one query.
            itimerspec ts{};
            ts.it_value.tv_nsec = 60 * 1000000L;
            timerfd_settime(query_fd_, 0, &ts, nullptr);
        }
    }, "pactl-sub");
}

void AudioEvents::stop() {
    if (bar_ && fd_ >= 0) bar_->remove_fd(fd_);
    if (fd_ >= 0) close(fd_);
    fd_ = -1;
    if (pid_ > 0) {
        kill(pid_, SIGTERM);
        waitpid(pid_, nullptr, WNOHANG); // reap; ECHILD is fine if too early
        pid_ = -1;
    }
}

void AudioEvents::schedule_restart() {
    if (!bar_) return;
    if (++attempts_ > 5) {
        fprintf(stderr,
                "mattbar: audio: event stream failed %d times; consumers "
                "fall back to polling\n",
                attempts_ - 1);
        return; // available() stays false -> consumers poll
    }
    if (retry_fd_ < 0) {
        retry_fd_ = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
        bar_->add_fd(retry_fd_, [this](uint32_t) {
            uint64_t n;
            while (read(retry_fd_, &n, sizeof n) > 0) {}
            if (fd_ < 0) start(*bar_);
        }, "audio-retry");
    }
    itimerspec ts{};
    ts.it_value.tv_sec = 1 << (attempts_ < 5 ? attempts_ : 5); // 2..32 s
    timerfd_settime(retry_fd_, 0, &ts, nullptr);
}

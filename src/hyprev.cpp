// Shared Hyprland IPC: one line-buffered .socket2 stream fanned out to
// subscribers, plus non-blocking command-socket requests. See hyprev.hpp.
#include "hyprev.hpp"
#include "bar.hpp"

#include <poll.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <memory>
#include <vector>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <initializer_list>

#ifndef DBG
#define DBG(...)                                                              \
    do {                                                                      \
        if (getenv("MATTBAR_DEBUG")) {                                        \
            fprintf(stderr, "mattbar: " __VA_ARGS__);                         \
            fputc('\n', stderr);                                             \
        }                                                                     \
    } while (0)
#endif

std::string hypr_instance_dir() {
    const char* sig = getenv("HYPRLAND_INSTANCE_SIGNATURE");
    if (!sig || !*sig) return {};
    const char* rt = getenv("XDG_RUNTIME_DIR");
    if (rt && *rt) {
        std::string p = std::string(rt) + "/hypr/" + sig;
        if (access((p + "/.socket.sock").c_str(), F_OK) == 0) return p;
    }
    std::string p = std::string("/tmp/hypr/") + sig; // older Hyprland
    if (access((p + "/.socket.sock").c_str(), F_OK) == 0) return p;
    return {};
}

namespace {

// Non-blocking, close-on-exec AF_UNIX stream connect. A unix connect
// either completes immediately or fails (EAGAIN = listen backlog full),
// so there is no EINPROGRESS dance.
int unix_connect_nb(const std::string& path) {
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return -1;
    sockaddr_un a{};
    a.sun_family = AF_UNIX;
    if (path.size() >= sizeof a.sun_path) {
        close(fd);
        return -1;
    }
    memcpy(a.sun_path, path.c_str(), path.size() + 1);
    if (connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

void timer_in_ms(int tfd, long ms) {
    itimerspec ts{};
    if (ms <= 0) ts.it_value.tv_nsec = 1; // all-zero would DISARM
    else {
        ts.it_value.tv_sec  = ms / 1000;
        ts.it_value.tv_nsec = (ms % 1000) * 1000000L;
    }
    timerfd_settime(tfd, 0, &ts, nullptr);
}

void drain_timer(int tfd) {
    uint64_t n;
    while (read(tfd, &n, sizeof n) > 0) {}
}

// ---------------------------------------------------------------------------
// event stream
// ---------------------------------------------------------------------------
struct Sub {
    int                      id = 0;
    std::vector<std::string> names; // empty: everything
    hyprev::Handler          handler;
    hyprev::StateFn          state;
    bool                     dead = false;
};

struct Stream {
    Bar*             bar = nullptr;
    bool             started = false, active = false, up = false;
    bool             want_state_up = false; // announce the next connect
    int              fd = -1, retry_fd = -1;
    long             backoff_ms = 0;
    std::string      buf;
    std::vector<Sub> subs;
    int              next_id = 1, depth = 0;
    hyprev::Stats    st;
} S;

// Upper bound for one unterminated line. Hyprland lines are short (a
// window title is the longest field); anything this big is garbage.
constexpr size_t kMaxLine = 256 * 1024;

void compact_subs() {
    if (S.depth) return;
    S.subs.erase(std::remove_if(S.subs.begin(), S.subs.end(),
                                [](const Sub& s) { return s.dead; }),
                 S.subs.end());
}

void notify_state(bool up) {
    ++S.depth;
    const size_t n = S.subs.size();
    for (size_t i = 0; i < n; ++i) {
        if (S.subs[i].dead || !S.subs[i].state) continue;
        auto fn = S.subs[i].state; // the vector may grow under the call
        fn(up);
    }
    --S.depth;
    compact_subs();
}

void dispatch_line(std::string_view line) {
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    if (line.empty()) return;
    ++S.st.lines;
    size_t sep = line.find(">>");
    HyprEvent ev;
    ev.line = line;
    if (sep == std::string_view::npos) {
        ev.name = line;
    } else {
        ev.name = line.substr(0, sep);
        ev.data = line.substr(sep + 2);
    }
    ++S.depth;
    // Subscribers added by a handler start with the NEXT event.
    const size_t n = S.subs.size();
    for (size_t i = 0; i < n; ++i) {
        const Sub& s = S.subs[i];
        if (s.dead) continue;
        bool want = s.names.empty();
        for (const auto& n : s.names)
            if (n == ev.name) { want = true; break; }
        if (!want) continue;
        ++S.st.delivered;
        auto fn = s.handler; // copy: a handler may subscribe (realloc)
        fn(ev);
    }
    --S.depth;
    compact_subs();
}

void schedule_retry();
void try_connect();

void drop_stream(const char* why) {
    Bar::close_fd(S.fd);
    S.buf.clear();
    const bool was_up = S.up;
    S.up = false;
    if (was_up) {
        ++S.st.drops;
        fprintf(stderr,
                "mattbar: hyprland event stream closed (%s); reconnecting\n",
                why);
        S.want_state_up = true;
        notify_state(false);
    }
    schedule_retry();
}

void on_readable(uint32_t events) {
    ++S.st.wakeups;
    // Read what is there now, bounded so one flood cannot starve the rest
    // of the loop (level-triggered epoll brings us straight back).
    char    chunk[16384];
    size_t  got = 0;
    ssize_t n   = 0;
    bool    eof = false;
    while (got < 256 * 1024) {
        n = read(S.fd, chunk, sizeof chunk);
        if (n > 0) {
            S.buf.append(chunk, static_cast<size_t>(n));
            got += static_cast<size_t>(n);
            continue;
        }
        if (n == 0) eof = true;
        else if (errno == EINTR) continue;
        else if (errno != EAGAIN && errno != EWOULDBLOCK) eof = true;
        break;
    }
    // Deliver every complete line; keep a partial tail for the next read.
    size_t start = 0;
    for (;;) {
        size_t nl = S.buf.find('\n', start);
        if (nl == std::string::npos) break;
        dispatch_line(std::string_view(S.buf).substr(start, nl - start));
        if (S.fd < 0) return; // a handler cannot close us, but be safe
        start = nl + 1;
    }
    if (start) S.buf.erase(0, start);
    if (S.buf.size() > kMaxLine) {
        fprintf(stderr, "mattbar: hyprland event stream: %zu bytes without "
                        "a newline; discarding\n", S.buf.size());
        S.buf.clear();
    }
    if (eof || (events & (EPOLLHUP | EPOLLERR)))
        drop_stream(n == 0 ? "end of stream" : strerror(errno));
}

void schedule_retry() {
    if (!S.bar || S.retry_fd < 0) return;
    // 0.5 s, 1, 2, 4 … capped at 30 s: Hyprland restarts come back within
    // a second; a session without Hyprland costs one connect per 30 s.
    S.backoff_ms = S.backoff_ms <= 0 ? 500 : std::min(S.backoff_ms * 2, 30000L);
    timer_in_ms(S.retry_fd, S.backoff_ms);
}

void try_connect() {
    if (S.fd >= 0) return;
    std::string dir = hypr_instance_dir();
    int fd = dir.empty() ? -1 : unix_connect_nb(dir + "/.socket2.sock");
    if (fd < 0) {
        if (!S.want_state_up) {
            // First attempt failed: subscribers learn about the stream
            // when it eventually comes up.
            S.want_state_up = true;
            DBG("hyprland event stream not available yet; retrying");
        }
        schedule_retry();
        return;
    }
    S.fd = fd;
    S.up = true;
    S.backoff_ms = 0;
    ++S.st.connects;
    S.bar->add_fd(fd, on_readable, "hypr-events");
    if (S.st.connects > 1)
        fprintf(stderr, "mattbar: hyprland event stream reconnected\n");
    if (S.want_state_up) {
        S.want_state_up = false;
        notify_state(true);
    }
}

void start(Bar& bar) {
    if (S.started) return;
    S.started = true;
    S.bar     = &bar;
    const char* sig = getenv("HYPRLAND_INSTANCE_SIGNATURE");
    if (!sig || !*sig) return; // not Hyprland: stay inactive forever
    S.active   = true;
    S.retry_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    if (S.retry_fd >= 0)
        bar.add_fd(S.retry_fd, [](uint32_t) {
            drain_timer(S.retry_fd);
            try_connect();
        }, "hypr-events-retry");
    try_connect();
}

// ---------------------------------------------------------------------------
// async requests
// ---------------------------------------------------------------------------
struct Req {
    uint64_t    id = 0;
    int         fd = -1, tfd = -1;
    std::string req, out;
    size_t      woff = 0;
    bool        failed = false; // deliver !ok on the next timer tick
    HyprReply   done;
};
std::map<uint64_t, std::unique_ptr<Req>> g_reqs;
uint64_t g_next_req = 1, g_req_total = 0, g_req_failed = 0;
Bar*     g_req_bar = nullptr;

void req_release(Req& r) {
    Bar::close_fd(r.fd);
    Bar::close_fd(r.tfd);
}

void req_finish(uint64_t id, bool ok) {
    auto it = g_reqs.find(id);
    if (it == g_reqs.end()) return;
    std::unique_ptr<Req> r = std::move(it->second);
    g_reqs.erase(it);
    req_release(*r);
    if (!ok) {
        ++g_req_failed;
        r->out.clear();
    }
    // Erased first: the callback may issue new requests freely.
    if (r->done) r->done(ok, std::move(r->out));
}

void req_io(uint64_t id, uint32_t events) {
    auto it = g_reqs.find(id);
    if (it == g_reqs.end()) return;
    Req& r = *it->second;
    if (r.woff < r.req.size()) {
        while (r.woff < r.req.size()) {
            ssize_t n = send(r.fd, r.req.data() + r.woff,
                             r.req.size() - r.woff, MSG_NOSIGNAL);
            if (n > 0) { r.woff += static_cast<size_t>(n); continue; }
            if (n < 0 && errno == EINTR) continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
            req_finish(id, false);
            return;
        }
        g_req_bar->mod_fd(r.fd, EPOLLIN); // written: wait for the reply
        return;
    }
    char buf[16384];
    for (;;) {
        ssize_t n = read(r.fd, buf, sizeof buf);
        if (n > 0) {
            r.out.append(buf, static_cast<size_t>(n));
            if (r.out.size() > 64u * 1024 * 1024) { // absurd: give up
                req_finish(id, false);
                return;
            }
            continue;
        }
        if (n == 0) { req_finish(id, true); return; } // Hyprland closed: done
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        req_finish(id, false);
        return;
    }
    if (events & (EPOLLHUP | EPOLLERR)) req_finish(id, !r.out.empty());
}

} // namespace

namespace hyprev {

int subscribe(Bar& bar, std::initializer_list<const char*> names, Handler h,
              StateFn on_state) {
    start(bar);
    if (!S.active) return 0;
    Sub s;
    s.id = S.next_id++;
    for (const char* n : names) s.names.emplace_back(n);
    s.handler = std::move(h);
    s.state   = std::move(on_state);
    S.subs.push_back(std::move(s));
    return S.subs.back().id;
}

void unsubscribe(int id) {
    if (id <= 0) return;
    for (auto& s : S.subs)
        if (s.id == id) s.dead = true;
    compact_subs();
}

bool connected() { return S.up; }

Stats stats() {
    Stats st = S.st;
    st.subscribers = static_cast<int>(
        std::count_if(S.subs.begin(), S.subs.end(),
                      [](const Sub& s) { return !s.dead; }));
    return st;
}

} // namespace hyprev

uint64_t hypr_async(Bar& bar, std::string req, HyprReply done,
                    int timeout_ms) {
    g_req_bar = &bar;
    auto r    = std::make_unique<Req>();
    r->id     = g_next_req++;
    r->req    = std::move(req);
    r->done   = std::move(done);
    ++g_req_total;
    const uint64_t id = r->id;
    r->tfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    if (r->tfd < 0) {
        // Out of file descriptors: nothing can be deferred, so this is the
        // one case where done() runs before hypr_async returns.
        ++g_req_failed;
        if (r->done) r->done(false, std::string());
        return id;
    }
    std::string dir = hypr_instance_dir();
    r->fd = dir.empty() ? -1 : unix_connect_nb(dir + "/.socket.sock");
    if (r->fd < 0) {
        r->failed = true;
        timer_in_ms(r->tfd, 0); // report on the next loop pass
    } else {
        timer_in_ms(r->tfd, timeout_ms > 0 ? timeout_ms : 3000);
    }
    bar.add_fd(r->tfd, [id](uint32_t) {
        auto it = g_reqs.find(id);
        if (it == g_reqs.end()) return;
        drain_timer(it->second->tfd);
        req_finish(id, false); // connect failure or timeout
    }, "hypr-req-timer");
    if (r->fd >= 0) {
        bar.add_fd(r->fd, [id](uint32_t ev) { req_io(id, ev); }, "hypr-req");
        bar.mod_fd(r->fd, EPOLLOUT); // write first; EPOLLIN once sent
    }
    g_reqs[id] = std::move(r);
    return id;
}

void hypr_async_cancel(uint64_t ticket) {
    auto it = g_reqs.find(ticket);
    if (it == g_reqs.end()) return;
    req_release(*it->second);
    g_reqs.erase(it);
}

uint64_t hypr_async_total() { return g_req_total; }
uint64_t hypr_async_failed() { return g_req_failed; }

// ---------------------------------------------------------------------------
// Blocking requests (package F). One implementation for the whole bar; the
// agents and local-LLM popups used to carry private copies, each able to
// block ~9 s per request (2 s connect + 2 s write + a read deadline that
// was only checked after each successful read, each read allowed another
// 2 s), and some call sites chain five or six of them.
// ---------------------------------------------------------------------------
namespace {

uint64_t mono_now_ms() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000ULL;
}

HyprSyncStats g_sync;
uint64_t      g_sync_cool_until = 0;

// After a timeout, Hyprland is wedged (config reload, session start,
// suspend thaw): further blocking requests fail at once for this long, so
// a chain of N requests costs one budget instead of N.
constexpr uint64_t kSyncCooldownMs = 1000;

} // namespace

std::string hypr_request(const std::string& dir, const std::string& req,
                         int budget_ms) {
    if (dir.empty() || req.empty()) return {};
    ++g_sync.total;
    const uint64_t t0 = mono_now_ms();
    if (t0 < g_sync_cool_until) {
        ++g_sync.fast_fails;
        return {};
    }
    // Non-blocking: a unix connect completes at once or fails (a full
    // listen backlog is EAGAIN), so nothing here can wait on connect().
    int fd = unix_connect_nb(dir + "/.socket.sock");
    if (fd < 0) {
        ++g_sync.failed;
        return {};
    }
    const uint64_t deadline = t0 + static_cast<uint64_t>(
                                       budget_ms > 0 ? budget_ms : 1);
    std::string out;
    size_t      woff = 0;
    bool        done = false, timed_out = false, failed = false;
    char        buf[16384];
    while (!done) {
        const uint64_t now = mono_now_ms();
        if (now >= deadline) { timed_out = true; break; }
        pollfd p{fd, static_cast<short>(woff < req.size() ? POLLOUT : POLLIN),
                 0};
        int pr = poll(&p, 1, static_cast<int>(deadline - now));
        if (pr < 0) {
            if (errno == EINTR) continue;
            failed = true;
            break;
        }
        if (pr == 0) { timed_out = true; break; }
        if (woff < req.size()) {
            ssize_t n = write(fd, req.data() + woff, req.size() - woff);
            if (n > 0) woff += static_cast<size_t>(n);
            else if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            else { failed = true; break; }
            continue;
        }
        for (;;) {
            ssize_t n = read(fd, buf, sizeof buf);
            if (n > 0) {
                out.append(buf, static_cast<size_t>(n));
                if (out.size() > 64u * 1024 * 1024) { failed = true; break; }
                continue;
            }
            if (n == 0) { done = true; break; } // Hyprland closed: complete
            if (errno == EINTR) continue;
            if (errno != EAGAIN) failed = true;
            break;
        }
        if (failed) break;
    }
    close(fd);
    const uint64_t took = mono_now_ms() - t0;
    g_sync.worst_ms = std::max(g_sync.worst_ms, took);
    if (timed_out) {
        ++g_sync.timeouts;
        g_sync_cool_until = mono_now_ms() + kSyncCooldownMs;
        fprintf(stderr, "mattbar: hyprland: no reply to '%.40s' within %d ms; "
                "blocking requests fail fast for %llu ms\n", req.c_str(),
                budget_ms, (unsigned long long)kSyncCooldownMs);
        return {};
    }
    if (failed) {
        ++g_sync.failed;
        return {};
    }
    return out; // complete or nothing, like hypr_async
}

std::string hypr_query(const std::string& req) {
    return hypr_request(hypr_instance_dir(), req);
}

HyprSyncStats hypr_sync_stats() { return g_sync; }

// Hyprland >= 0.55 with a Lua config (what Omarchy Quattro converts every
// install to) evaluates a socket1 `dispatch X` as Lua: `hl.dispatch(X)`.
// The hyprlang form `workspace 3` is a Lua syntax error there — swallowed,
// so clicks silently do nothing. A .conf config still takes the legacy
// dispatcher-table path, and each config type rejects the other's syntax.
// So: send the form that last worked; on a reply that isn't "ok", try the
// other and remember. One cache for the whole bar: one compositor, one
// config dialect at a time. (The local-LLM popup used to keep its own.)
namespace {
int g_dispatch_mode = 0; // 0 unknown, 1 legacy hyprlang, 2 lua
bool reply_ok(const std::string& r) { return r.rfind("ok", 0) == 0; }
} // namespace

bool hypr_dispatch2(const std::string& dir, const std::string& legacy,
                    const std::string& lua) {
    if (g_dispatch_mode == 2) {
        if (reply_ok(hypr_request(dir, lua))) return true;
        if (reply_ok(hypr_request(dir, legacy))) {
            g_dispatch_mode = 1;
            return true;
        }
        g_dispatch_mode = 0; // compositor mid-restart? re-learn next time
        return false;
    }
    if (reply_ok(hypr_request(dir, legacy))) {
        g_dispatch_mode = 1;
        return true;
    }
    if (reply_ok(hypr_request(dir, lua))) {
        g_dispatch_mode = 2;
        return true;
    }
    g_dispatch_mode = 0;
    return false;
}

void hypr_dispatch2_async(Bar& bar, const std::string& legacy,
                          const std::string& lua) {
    const bool        lua_first = g_dispatch_mode == 2;
    const std::string first     = lua_first ? lua : legacy;
    const std::string second    = lua_first ? legacy : lua;
    Bar*              b         = &bar;
    hypr_async(bar, first, [b, lua_first, second](bool ok, std::string r) {
        if (ok && reply_ok(r)) {
            g_dispatch_mode = lua_first ? 2 : 1;
            return;
        }
        hypr_async(*b, second, [lua_first](bool ok2, std::string r2) {
            g_dispatch_mode =
                ok2 && reply_ok(r2) ? (lua_first ? 1 : 2) : 0;
        });
    });
}

void hypr_fire(Bar& bar, const std::string& req, const char* what) {
    std::string tag = what ? what : "";
    hypr_async(bar, req, [tag](bool ok, std::string r) {
        DBG("%s: %s reply='%.120s'", tag.c_str(), ok ? "ok" : "FAILED",
            r.c_str());
    });
}

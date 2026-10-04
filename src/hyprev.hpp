#pragma once
// Hyprland IPC shared by every module (package E).
//
// Event stream: ONE connection to .socket2 for the whole bar, line
// buffered, fanned out to subscribers by event name. Before this, the
// workspaces, agents, idle, active-window and local-LLM code each held
// their own socket2 connection, so every Hyprland event (every focus
// change, every title update) woke five parsers, and each had its own
// flavour of line and end-of-stream handling: the workspaces one missed
// events split across 4 KB reads and never reconnected; the active-window
// one ignored EOF altogether (a closed stream stays readable, so it would
// spin). Here the stream reconnects with backoff and tells subscribers
// when it drops and when it comes back, so they can resync.
//
// Requests: hypr_async() is the non-blocking twin of hypr_request(): the
// socket is connected, written and read from the main loop and the reply
// is delivered to a callback, so a slow compositor costs latency on one
// widget instead of freezing the bar.
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <string>
#include <string_view>

class Bar;

// Hyprland's instance directory ($XDG_RUNTIME_DIR/hypr/$SIG, or the older
// /tmp/hypr/$SIG); empty when not running under Hyprland or when its
// command socket is not there (yet).
std::string hypr_instance_dir();

struct HyprEvent {
    std::string_view name; // "openwindow"
    std::string_view data; // everything after ">>"
    std::string_view line; // the whole line, "openwindow>>..."
};

namespace hyprev {
using Handler = std::function<void(const HyprEvent&)>;
// false when the stream drops, true when it is back after a drop or after
// a failed first connect. Events in between are lost: resync on true.
using StateFn = std::function<void(bool connected)>;

// Subscribe to events by exact name ("workspacev2"); an empty list means
// every event. The first subscription starts the stream. Handlers run on
// the main loop and may subscribe or unsubscribe (themselves included).
// Returns an id (> 0), or 0 when not under Hyprland at all.
int  subscribe(Bar& bar, std::initializer_list<const char*> names,
               Handler h, StateFn on_state = {});
void unsubscribe(int id);
bool connected();

struct Stats {
    uint64_t wakeups = 0, lines = 0, delivered = 0, connects = 0,
             drops = 0;
    int      subscribers = 0;
};
Stats stats();
} // namespace hyprev

// One request on Hyprland's command socket ("j/workspaces", "dispatch …")
// without blocking. done(ok, reply) runs on the main loop, not from
// inside hypr_async itself (the one exception: fd exhaustion, reported
// synchronously so callers never wait on a request that cannot exist). ok is false when the socket is missing, the
// connection fails, or no complete reply arrived within timeout_ms (the
// partial reply is then discarded). Returns a ticket (never 0).
using HyprReply = std::function<void(bool ok, std::string reply)>;
uint64_t hypr_async(Bar& bar, std::string req, HyprReply done,
                    int timeout_ms = 3000);
// Drop a request; its callback will not run. Safe with 0 or a finished
// ticket.
void hypr_async_cancel(uint64_t ticket);
uint64_t hypr_async_total();   // requests issued (ctl ipc-stats)
uint64_t hypr_async_failed();  // of those, failed or timed out

// Blocking request, for the call sites whose logic needs the reply before
// it can continue (the agents and local-LLM popup choreography). Prefer
// hypr_async. One deadline covers connect, write and read; the reply is
// returned only if complete (Hyprland closes the socket after replying),
// otherwise empty. After a timeout every blocking request fails at once
// for a second, so a chain of them against a wedged compositor costs one
// budget, not one per request. The main loop's worst stall is therefore
// ~budget_ms, far inside the 30 s watchdog.
constexpr int kHyprSyncBudgetMs = 1500;
std::string hypr_request(const std::string& dir, const std::string& req,
                         int budget_ms = kHyprSyncBudgetMs);
// hypr_request against hypr_instance_dir().
std::string hypr_query(const std::string& req);

struct HyprSyncStats {
    uint64_t total = 0, timeouts = 0, fast_fails = 0, failed = 0,
             worst_ms = 0;
};
HyprSyncStats hypr_sync_stats(); // ctl ipc-stats

// `dispatch` in whichever syntax the running config accepts (hyprlang or
// Lua), learning which one works; see hyprev.cpp.
bool hypr_dispatch2(const std::string& dir, const std::string& legacy,
                    const std::string& lua);
// Same, without blocking: for fire-and-forget dispatches (workspace
// clicks and scrolls), where nothing waits on the outcome.
void hypr_dispatch2_async(Bar& bar, const std::string& legacy,
                          const std::string& lua);
// Fire-and-forget request whose reply only matters for the debug log.
void hypr_fire(Bar& bar, const std::string& req, const char* what);

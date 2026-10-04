#include "idle.hpp"
#include "bar.hpp"
#include "config.hpp"
#include "lock.hpp"
#include "modules.hpp"
#include "notify.hpp"
#include "spawn.hpp"
#include "util.hpp"
#include "hyprev.hpp"

#include "ext-idle-notify-v1-client-protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/timerfd.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <dirent.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <set>
#include <string>
#include <cstdint>

namespace {

Bar* g_bar = nullptr;
ext_idle_notification_v1* notif = nullptr;
int  lock_fd = -1;
int  grace_fd = -1;
int  stale_fd = -1;
int  watch_fd = -1;
int  motion_fd = -1;
int  relaunch_fd = -1;
bool cycle    = false;
bool saver_on = false;
bool saver_started = false; // launched this cycle; lock only while it stays up
bool ignore_resume = false; // launch grace: mapping looks like activity
bool saver_stopping = false; // MattBar is tearing the saver down on purpose
bool lock_pending   = false; // lock deadline hit; saver stays until key/mouse
int  lock_s = 300, saver_s = 150;
int  armed_first_ms = -1;
uint64_t rearm_until = 0;
uint64_t last_launch_ms = 0;
bool stay_awake = false;
std::set<std::string> saver_addrs;
constexpr const char* kSaverClass = "org.omarchy.screensaver";
// Dismiss the saver after this much *moving* time (gaps reset the count).
constexpr int kMoveNeedMs   = 1500;
constexpr int kMoveTickMs   = 50;
constexpr int kMovePx       = 4;   // ignore trackpad/touchscreen jitter
constexpr int kMoveStillMs  = 250; // stillness resets the accumulator
constexpr int kRelaunchGapMs = 2000;
constexpr int kRelaunchMax   = 3; // rapid deaths: stop looping
int  relaunch_hits = 0;
int  move_ms     = 0;
int  still_ms    = 0;
int  last_cx     = 0, last_cy = 0;
bool have_cursor = false;
uint64_t saver_move_after = 0;

uint64_t idle_now_ms() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

std::string hypr_dir() {
    const char* sig = getenv("HYPRLAND_INSTANCE_SIGNATURE");
    const char* rt  = getenv("XDG_RUNTIME_DIR");
    if (!sig || !*sig) return {};
    if (rt && *rt) {
        std::string p = std::string(rt) + "/hypr/" + sig;
        if (access((p + "/.socket2.sock").c_str(), F_OK) == 0) return p;
    }
    return {};
}

// One in-process pass over /proc replaces `pidof ttfx`, `pidof
// omarchy-screensaver` and `pgrep -f org.omarchy.screensaver` — three
// fork+execs (pgrep itself reads every cmdline) once a second while the
// saver runs.
bool cmdline_is_screensaver(char* buf, ssize_t n) {
    if (n <= 0) return false;
    buf[n] = '\0';
    const char* a0 = buf;
    const char* a1 =
        (size_t)(strlen(a0) + 1) < (size_t)n ? a0 + strlen(a0) + 1 : "";
    for (const char* a : {a0, a1}) {
        const char* b = strrchr(a, '/');
        b = b ? b + 1 : a;
        if (!strcmp(b, "ttfx") || !strcmp(b, "omarchy-screensaver") ||
            !strcmp(b, "omarchy-launch-screensaver"))
            return true;
    }
    for (ssize_t i = 0; i < n; ++i)
        if (buf[i] == '\0') buf[i] = ' ';
    return strstr(buf, "org.omarchy.screensaver") != nullptr;
}

bool screensaver_proc() {
    DIR* d = opendir("/proc");
    if (!d) return false;
    bool found = false;
    char path[300], buf[4096];
    while (!found) {
        dirent* e = readdir(d);
        if (!e) break;
        if (e->d_name[0] < '1' || e->d_name[0] > '9') continue;
        snprintf(path, sizeof path, "/proc/%s/cmdline", e->d_name);
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) continue;
        ssize_t n = read(fd, buf, sizeof buf - 1);
        close(fd);
        if (cmdline_is_screensaver(buf, n)) found = true;
    }
    closedir(d);
    return found;
}

void arm_watch(bool on) {
    if (watch_fd < 0) return;
    itimerspec ts{};
    if (on) {
        ts.it_value.tv_sec    = 1;
        ts.it_interval.tv_sec = 1;
    }
    timerfd_settime(watch_fd, 0, &ts, nullptr);
}

void arm_move_watch(bool on) {
    if (motion_fd < 0) return;
    itimerspec ts{};
    if (on) {
        ts.it_value.tv_nsec    = kMoveTickMs * 1000000L;
        ts.it_interval.tv_nsec = kMoveTickMs * 1000000L;
    }
    timerfd_settime(motion_fd, 0, &ts, nullptr);
    if (!on) {
        move_ms      = 0;
        still_ms     = 0;
        have_cursor  = false;
    }
}

std::string hypr_cmd(const std::string& cmd) {
    std::string dir = hypr_dir();
    if (dir.empty()) return {};
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return {};
    timeval tv{0, 40000};
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    sockaddr_un a{};
    a.sun_family = AF_UNIX;
    strncpy(a.sun_path, (dir + "/.socket.sock").c_str(), sizeof a.sun_path - 1);
    if (connect(fd, (sockaddr*)&a, sizeof a) < 0) {
        close(fd);
        return {};
    }
    (void)!write(fd, cmd.c_str(), cmd.size());
    shutdown(fd, SHUT_WR);
    std::string out;
    char        buf[256];
    ssize_t     n;
    while ((n = read(fd, buf, sizeof buf)) > 0) out.append(buf, (size_t)n);
    close(fd);
    return out;
}

bool cursor_xy(int& x, int& y) {
    std::string r = hypr_cmd("cursorpos");
    int a = 0, b = 0;
    if (sscanf(r.c_str(), "%d ,%d", &a, &b) == 2 ||
        sscanf(r.c_str(), "%d,%d", &a, &b) == 2) {
        x = a;
        y = b;
        return true;
    }
    return false;
}

void arm_grace(int ms) {
    if (grace_fd < 0) return;
    itimerspec ts{};
    if (ms > 0) {
        ts.it_value.tv_sec  = ms / 1000;
        ts.it_value.tv_nsec = (ms % 1000) * 1000000L;
    }
    timerfd_settime(grace_fd, 0, &ts, nullptr);
}

void saver_reset_windows() { saver_addrs.clear(); }

bool saver_up() { return !saver_addrs.empty() || screensaver_proc(); }

std::string stay_path() {
    const char* xdg = getenv("XDG_STATE_HOME");
    const char* h   = getenv("HOME");
    std::string base = xdg && *xdg
                           ? std::string(xdg)
                           : std::string(h ? h : ".") + "/.local/state";
    return base + "/omarchy/indicators/stay-awake";
}

std::string shell_json_path() {
    const char* h = getenv("HOME");
    return std::string(h ? h : ".") + "/.config/omarchy/shell.json";
}

bool grab_idle_int(const std::string& js, const char* key, int& out) {
    size_t idle = js.find("\"idle\"");
    std::string pat = std::string("\"") + key + "\"";
    size_t p = js.find(pat, idle == std::string::npos ? 0 : idle);
    if (p == std::string::npos) return false;
    p = js.find(':', p + pat.size());
    if (p == std::string::npos) return false;
    int v = atoi(js.c_str() + p + 1);
    if (v < 0) return false;
    out = v;
    return true;
}

bool replace_idle_int(std::string& js, const char* key, int v) {
    size_t idle = js.find("\"idle\"");
    std::string pat = std::string("\"") + key + "\"";
    size_t p = js.find(pat, idle == std::string::npos ? 0 : idle);
    if (p == std::string::npos) return false;
    p = js.find(':', p + pat.size());
    if (p == std::string::npos) return false;
    size_t n = p + 1;
    while (n < js.size() && isspace((unsigned char)js[n])) ++n;
    size_t e = n;
    if (e < js.size() && js[e] == '-') ++e;
    while (e < js.size() && isdigit((unsigned char)js[e])) ++e;
    if (e == n) return false;
    js.replace(n, e - n, std::to_string(v));
    return true;
}

void read_timeouts() {
    lock_s  = 300;
    saver_s = 150;
    std::string js = slurp(shell_json_path());
    if (js.empty()) return;
    grab_idle_int(js, "lock", lock_s);
    grab_idle_int(js, "screensaver", saver_s);
    if (lock_s < 1) lock_s = 1;
    if (saver_s < 1) saver_s = 1;
}

void write_timeouts() {
    std::string path = shell_json_path();
    std::string js   = slurp(path);
    if (js.empty()) return;
    if (!replace_idle_int(js, "screensaver", saver_s)) return;
    if (!replace_idle_int(js, "lock", lock_s)) return;
    // Omarchy's own config: an interrupted in-place rewrite left it
    // truncated and the shell unable to parse it.
    if (!atomic_write(path, js))
        fprintf(stderr, "mattbar: idle: could not write %s\n", path.c_str());
}

void disarm_lock_timer() {
    if (lock_fd < 0) return;
    itimerspec off{};
    timerfd_settime(lock_fd, 0, &off, nullptr);
}

void disarm_stale_timer() {
    if (stale_fd < 0) return;
    itimerspec off{};
    timerfd_settime(stale_fd, 0, &off, nullptr);
}

void disarm_relaunch() {
    if (relaunch_fd < 0) return;
    itimerspec off{};
    timerfd_settime(relaunch_fd, 0, &off, nullptr);
}

std::string idle_wrap_dir() {
    const char* rt = getenv("XDG_RUNTIME_DIR");
    if (rt && *rt) return std::string(rt) + "/mattbar-idle";
    return "/tmp/mattbar-idle-" + std::to_string((long)getuid());
}

std::string idle_exe_dir() {
    char    buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n <= 0) return {};
    buf[n] = '\0';
    std::string p = buf;
    auto        sl = p.rfind('/');
    if (sl == std::string::npos) return {};
    return p.substr(0, sl);
}

bool screensaver_toggled_off() {
    const char* h = getenv("HOME");
    if (!h || !*h) return false;
    return access((std::string(h) +
                   "/.local/state/omarchy/toggles/screensaver-off")
                      .c_str(),
                  F_OK) == 0;
}

// Packaged omarchy-screensaver exits on ANY stdin byte (`read -n1`) and
// whenever the saver window is not focused. OSD, focus-in CSI, OSC
// replies, hyprctl glitches, and compositor pointer warps all tear it
// down. Hyprland exec_cmd does not inherit MattBar's PATH, so a PATH
// wrap never reaches Ghostty's `-e omarchy-screensaver`. Launch with an
// absolute `-e` of this stand-in instead. It only treats a real key as
// dismiss; MattBar itself dismisses on sustained pointer movement.
constexpr const char kSaverWrapper[] = R"MBSAVER(#!/bin/bash
dismiss_key() {
  mattbarctl idle screensaver-key >/dev/null 2>&1 || true
  hyprctl eval 'hl.config({ cursor = { invisible = false } })' >/dev/null 2>&1 || true
  exit 0
}
trap dismiss_key SIGINT
trap 'exit 0' SIGTERM SIGHUP SIGQUIT

printf '\033[?1000l\033[?1002l\033[?1003l\033[?1006l\033[?1015l\033[?1004l\033[?2004l'
printf '\033]11;rgb:00/00/00\007'
hyprctl eval 'hl.config({ cursor = { invisible = true } })' >/dev/null 2>&1 || true
stty -echo -icanon min 0 time 0 2>/dev/null || true
while IFS= read -r -n1 -t 0.05 _; do :; done

esc_is_key() {
  local c
  IFS= read -r -n1 -t 0.08 c || return 0
  case $c in
    ']')
      while IFS= read -r -n1 -t 0.2 c; do
        [[ $c == $'\a' ]] && break
        if [[ $c == $'\e' ]]; then IFS= read -r -n1 -t 0.05 _; break; fi
      done
      return 1
      ;;
    'O')
      IFS= read -r -n1 -t 0.08 _
      return 0
      ;;
    '[') ;;
    *) return 0 ;;
  esac
  IFS= read -r -n1 -t 0.08 c || return 1
  case $c in
    I|O) return 1 ;;
    '?')
      while IFS= read -r -n1 -t 0.08 c; do
        case $c in [@-~]) break ;; esac
      done
      return 1
      ;;
    '<')
      while IFS= read -r -n1 -t 0.08 c; do
        [[ $c == 'M' || $c == 'm' ]] && break
      done
      return 1
      ;;
    M)
      IFS= read -r -n1 -t 0.08 _; IFS= read -r -n1 -t 0.08 _
      return 1
      ;;
  esac
  while true; do
    case $c in [@-~]) break ;; esac
    IFS= read -r -n1 -t 0.08 c || return 1
  done
  case $c in c|n|R|t) return 1 ;; esac
  return 0
}

byte_is_key() {
  local c=$1
  [[ $c == $'\e' ]] && { esc_is_key; return $?; }
  case $c in
    $'\n'|$'\r'|$'\t'|$'\x7f'|$'\b') return 0 ;;
    [[:cntrl:]]) return 1 ;;
  esac
  return 0
}

tty=$(tty 2>/dev/null)
deadline=$((SECONDS + 2))
while ((SECONDS < deadline)) && [[ $(stty size 2>/dev/null) == "24 80" ]]; do
  sleep 0.02
done
while IFS= read -r -n1 -t 0.05 _; do :; done

while true; do
  ttfx -i "${HOME}/.config/omarchy/branding/screensaver.txt" \
    --frame-rate 120 --canvas-width 0 --canvas-height 0 --reuse-canvas \
    --anchor-canvas c --anchor-text c --random-effect --no-eol --no-restore-cursor &
  while pgrep -t "${tty#/dev/}" -x ttfx >/dev/null; do
    if IFS= read -r -n1 -t 1 c && byte_is_key "$c"; then
      dismiss_key
    fi
  done
  wait 2>/dev/null || true
done
)MBSAVER";

// Same multi-monitor launch as Omarchy, but `-e` is this directory's
// wrapper (absolute). Also SIGKILLs a leftover packaged saver first:
// that script's exit trap `pkill -f org.omarchy.screensaver` is what
// turns one stale window into a map/kill loop across a bar restart.
constexpr const char kSaverLaunch[] = R"MBLAUNCH(#!/bin/bash
DIR=$(cd "$(dirname "$0")" && pwd)
SAVER="$DIR/omarchy-screensaver"
OMARCHY_PATH="${OMARCHY_PATH:-/usr/share/omarchy}"
if ! command -v ttfx >/dev/null 2>&1; then exit 1; fi
if [[ -f "${HOME}/.local/state/omarchy/toggles/screensaver-off" && ${1:-} != force ]]; then
  exit 1
fi
pkill -KILL -x ttfx >/dev/null 2>&1 || true
pkill -KILL -x omarchy-screensaver >/dev/null 2>&1 || true
if command -v pgrep >/dev/null 2>&1; then
  pgrep -f '[o]rg.omarchy.screensaver' | while read -r p; do
    [[ -n $p ]] && kill -KILL "$p" >/dev/null 2>&1 || true
  done
fi
sleep 0.05
focused=$(omarchy-hyprland-monitor-focused 2>/dev/null || true)
terminal=$(xdg-terminal-exec --print-id 2>/dev/null || echo ghostty)
hypr_focus_monitor() {
  hyprctl dispatch "hl.dsp.focus({ monitor = \"$1\" })" >/dev/null 2>&1 || hyprctl dispatch focusmonitor "$1" >/dev/null
}
hypr_exec() {
  local command
  printf -v command '%q ' "$@"
  hyprctl dispatch "hl.dsp.exec_cmd([[$command]])" >/dev/null 2>&1 || hyprctl dispatch exec -- bash -lc "$command" >/dev/null
}
SOCKET="$XDG_RUNTIME_DIR/hypr/$HYPRLAND_INSTANCE_SIGNATURE/.socket2.sock"
exec {events}< <(socat -U - "UNIX-CONNECT:$SOCKET")
wait_for_screensaver_window() {
  local line deadline=$((SECONDS + 5))
  while ((SECONDS < deadline)) && IFS= read -r -t $((deadline - SECONDS)) -u "$events" line; do
    [[ $line == openwindow\>\>*,org.omarchy.screensaver,* ]] && return 0
  done
}
for m in $(hyprctl monitors -j | jq -r '.[] | .name'); do
  hypr_focus_monitor "$m"
  case $terminal in
  *Alacritty*) hypr_exec alacritty --class=org.omarchy.screensaver --config-file "$OMARCHY_PATH/default/alacritty/screensaver.toml" -e "$SAVER" ;;
  *foot*)      hypr_exec foot --app-id=org.omarchy.screensaver --config="$OMARCHY_PATH/default/foot/screensaver.ini" -e "$SAVER" ;;
  *kitty*)     hypr_exec kitty --class=org.omarchy.screensaver --override font_size=18 --override window_padding_width=0 -e "$SAVER" ;;
  *)           hypr_exec ghostty --class=org.omarchy.screensaver --config-file="$OMARCHY_PATH/default/ghostty/screensaver" --font-size=18 -e "$SAVER" ;;
  esac
  wait_for_screensaver_window
done
[[ -n $focused ]] && hypr_focus_monitor "$focused"
)MBLAUNCH";

bool install_saver_wrapper() {
    std::string dir = idle_wrap_dir();
    mkdir(dir.c_str(), 0700);
    std::string saver  = dir + "/omarchy-screensaver";
    std::string launch = dir + "/omarchy-launch-screensaver";
    if (!atomic_write(saver, kSaverWrapper) || chmod(saver.c_str(), 0755) != 0)
        return false;
    if (!atomic_write(launch, kSaverLaunch) || chmod(launch.c_str(), 0755) != 0)
        return false;
    return true;
}

void launch_screensaver() {
    std::string dir = idle_wrap_dir();
    std::string cmd;
    if (install_saver_wrapper()) {
        std::string exe = idle_exe_dir();
        cmd = "PATH=" + sh_quote(dir);
        if (!exe.empty()) cmd += ":" + sh_quote(exe);
        cmd += ":$PATH " + sh_quote(dir + "/omarchy-launch-screensaver");
    } else {
        fprintf(stderr,
                "mattbar: idle: could not write screensaver wrapper; "
                "using packaged omarchy-screensaver\n");
        cmd = "omarchy-launch-screensaver";
    }
    last_launch_ms = idle_now_ms();
    spawn_detached(cmd);
}

void request_relaunch(const char* why);
void stop_screensaver();
void restore_pointer();
void arm_fresh_from_now();
void cancel_cycle(const char* why);
void kill_screensaver_procs();

void finish_lock_from_idle(const char* why) {
    fprintf(stderr, "mattbar: idle: locking (%s)\n", why ? why : "");
    saver_stopping = true;
    lock_pending   = false;
    disarm_relaunch();
    stop_screensaver();
    restore_pointer();
    lock_now();
    cycle         = false;
    saver_started = false;
    saver_reset_windows();
    arm_watch(false);
    arm_move_watch(false);
    arm_grace(0);
}

void do_relaunch(const char* why) {
    disarm_relaunch();
    if (saver_stopping || !cycle || !saver_started || stay_awake ||
        lock_is_locked() || screensaver_toggled_off())
        return;
    if (saver_up()) return;
    fprintf(stderr, "mattbar: idle: screensaver died (%s); relaunching\n",
            why ? why : "");
    ignore_resume = true;
    // Procs only: window.kill here races Hyprland and takes down the
    // window we are about to map (the map/kill loop).
    kill_screensaver_procs();
    saver_reset_windows();
    launch_screensaver();
    saver_on         = true;
    saver_move_after = idle_now_ms() + 2000;
    arm_grace(4000);
    arm_watch(true);
    arm_move_watch(true);
}

void arm_relaunch_in(int ms) {
    if (relaunch_fd < 0) return;
    itimerspec ts{};
    if (ms < 50) ms = 50;
    ts.it_value.tv_sec  = ms / 1000;
    ts.it_value.tv_nsec = (ms % 1000) * 1000000L;
    timerfd_settime(relaunch_fd, 0, &ts, nullptr);
}

void request_relaunch(const char* why) {
    if (saver_stopping || !cycle || !saver_started || stay_awake ||
        lock_is_locked() || screensaver_toggled_off())
        return;
    if (saver_up()) return;
    if (lock_pending) {
        finish_lock_from_idle(why);
        return;
    }
    uint64_t now = idle_now_ms();
    if (last_launch_ms && now - last_launch_ms < 8000)
        ++relaunch_hits;
    else
        relaunch_hits = 1;
    if (relaunch_hits >= kRelaunchMax) {
        fprintf(stderr,
                "mattbar: idle: screensaver unstable (%s); backing off\n",
                why ? why : "");
        cancel_cycle("screensaver-unstable");
        arm_fresh_from_now();
        return;
    }
    uint64_t due = last_launch_ms + (uint64_t)kRelaunchGapMs;
    if (now < due) {
        fprintf(stderr,
                "mattbar: idle: screensaver gone (%s); retry in %llums\n",
                why ? why : "", (unsigned long long)(due - now));
        arm_relaunch_in((int)(due - now));
        return;
    }
    do_relaunch(why);
}

// The saver is a terminal (--class org.omarchy.screensaver) running
// omarchy-screensaver, which itself loops ttfx.
//
// omarchy-screensaver traps SIGTERM and `pkill -f org.omarchy.screensaver`,
// which signals Ghostty from inside Ghostty's child. That deadlock is
// what Hyprland's wait/terminate dialog is. SIGKILL skips the trap.
// Kill the script and ttfx first so Ghostty does not SIGTERM its child
// on the way down.
//
// Hyprland 0.56: WindowSelector is a window object / address / id, NOT
// `{ class = "..." }`. Passing that table to window.close/kill is the
// same as Super+W / killactive — it hits the FOCUSED client. Only
// touch windows whose class actually matches. Use kill, not close:
// close is an xdg request and is what arms the ANR dialog.
void kill_screensaver_windows() {
    hypr_cmd(
        "eval "
        "for _, w in ipairs(hl.get_windows() or {}) do "
        "  if w.class == \"org.omarchy.screensaver\" "
        "     or w.initial_class == \"org.omarchy.screensaver\" then "
        "    hl.dispatch(hl.dsp.window.kill(w)); "
        "  end; "
        "end; "
        "hl.config({ cursor = { invisible = false } })");
}

void kill_screensaver_procs() {
    DIR* d = opendir("/proc");
    if (!d) return;
    pid_t self = getpid();
    char  path[300], buf[4096];
    // Two passes: scripts/ttfx first, then the terminal (cmdline class).
    for (int pass = 0; pass < 2; ++pass) {
        rewinddir(d);
        while (dirent* e = readdir(d)) {
            if (e->d_name[0] < '1' || e->d_name[0] > '9') continue;
            pid_t pid = (pid_t)atoi(e->d_name);
            if (pid <= 1 || pid == self) continue;
            snprintf(path, sizeof path, "/proc/%s/cmdline", e->d_name);
            int fd = open(path, O_RDONLY | O_CLOEXEC);
            if (fd < 0) continue;
            ssize_t n = read(fd, buf, sizeof buf - 1);
            close(fd);
            if (n <= 0) continue;
            char copy[4096];
            if ((size_t)n >= sizeof copy) n = (ssize_t)sizeof copy - 1;
            memcpy(copy, buf, (size_t)n);
            if (!cmdline_is_screensaver(copy, n)) continue;
            bool terminal = strstr(copy, "org.omarchy.screensaver") != nullptr;
            if (pass == 0 && terminal) continue;
            if (pass == 1 && !terminal) continue;
            kill(pid, SIGKILL);
        }
    }
    closedir(d);
}

void wait_screensaver_gone() {
    for (int i = 0; i < 8; ++i) {
        if (!screensaver_proc()) return;
        if (g_bar) g_bar->ping_watchdog();
        poll(nullptr, 0, 50);
        kill_screensaver_procs();
        if (i == 1 || i == 4) kill_screensaver_windows();
    }
}

void stop_screensaver() {
    // SIGKILL in-process so lock/unlock do not proceed with a live
    // Ghostty still mapped (spawn_helper ran the old SIGTERM after
    // lock surfaces were already up).
    kill_screensaver_procs();
    kill_screensaver_windows();
    wait_screensaver_gone();
    saver_on = false;
}

void stop_screensaver_now() {
    stop_screensaver();
    saver_reset_windows();
}

void restore_pointer() {
    // hide_on_key_press (Omarchy default) hides the cursor on the
    // password keystrokes. A compositor warp counts as motion and
    // brings it back — but the old `move({ x = 1, y = 0 })` was an
    // absolute jump to the layout origin, so the cursor "disappeared"
    // until warp_on_change_workspace on a workspace switch.
    // Nudge from the current position instead, after a beat so the
    // session-lock surfaces have released the pointer.
    spawn_helper(
        "omarchy-system-wake >/dev/null 2>&1 || true; "
        "( sleep 0.08; "
        "  hyprctl eval 'hl.dsp.dpms(\"on\")' >/dev/null 2>&1 || true; "
        "  hyprctl eval 'hl.dsp.release_input_capture()' "
        ">/dev/null 2>&1 || true; "
        "  hyprctl eval '"
        "local function unhide() "
        "  local p = hl.get_cursor_pos(); "
        "  if not p then return end; "
        "  if p.x <= 1 and p.y <= 1 then "
        "    local m = hl.get_active_monitor(); "
        "    if m then "
        "      hl.dispatch(hl.dsp.cursor.move({ "
        "        x = (m.x or 0) + m.width / 2, "
        "        y = (m.y or 0) + m.height / 2 "
        "      })); "
        "    end; "
        "  else "
        "    hl.dispatch(hl.dsp.cursor.move({ x = p.x + 1, y = p.y })); "
        "    hl.dispatch(hl.dsp.cursor.move({ x = p.x, y = p.y })); "
        "  end; "
        "end; "
        "unhide(); "
        "hl.config({ cursor = { invisible = false } })"
        "' >/dev/null 2>&1 || true; "
        "  sleep 0.2; "
        "  hyprctl eval '"
        "local p = hl.get_cursor_pos(); "
        "if not p then return end; "
        "if p.x <= 1 and p.y <= 1 then "
        "  local m = hl.get_active_monitor(); "
        "  if m then "
        "    hl.dispatch(hl.dsp.cursor.move({ "
        "      x = (m.x or 0) + m.width / 2, "
        "      y = (m.y or 0) + m.height / 2 "
        "    })); "
        "  end; "
        "else "
        "  hl.dispatch(hl.dsp.cursor.move({ x = p.x + 1, y = p.y })); "
        "  hl.dispatch(hl.dsp.cursor.move({ x = p.x, y = p.y })); "
        "end"
        "' >/dev/null 2>&1 || true; "
        ") >/dev/null 2>&1");
}

void cancel_cycle(const char* why);
void on_move_tick();

void dismiss_screensaver(const char* why);

void cancel_cycle(const char* why) {
    bool had = cycle || saver_on || saver_started;
    saver_stopping = true;
    lock_pending   = false;
    disarm_lock_timer();
    disarm_stale_timer();
    disarm_relaunch();
    arm_watch(false);
    arm_move_watch(false);
    arm_grace(0);
    ignore_resume = false;
    if (!had) {
        saver_started = false;
        saver_reset_windows();
        return;
    }
    fprintf(stderr, "mattbar: idle: cancel (%s)\n", why ? why : "");
    bool saver = saver_on || saver_started;
    stop_screensaver();
    saver_started = false;
    saver_reset_windows();
    if (saver) restore_pointer();
    cycle = false;
    if (saver)
        if (auto* nd = notify_daemon()) nd->refresh_popups();
}

void dismiss_screensaver(const char* why) {
    bool pending = lock_pending;
    cancel_cycle(why);
    if (pending && !stay_awake && !lock_is_locked()) {
        fprintf(stderr, "mattbar: idle: lock after screensaver dismiss\n");
        lock_now();
    }
}

void on_move_tick() {
    if (!cycle || !saver_started || !saver_up() ||
        idle_now_ms() < saver_move_after) {
        move_ms     = 0;
        still_ms    = 0;
        have_cursor = false;
        return;
    }
    int x = 0, y = 0;
    if (!cursor_xy(x, y)) return;
    if (!have_cursor) {
        last_cx     = x;
        last_cy     = y;
        have_cursor = true;
        return;
    }
    int dx = x - last_cx, dy = y - last_cy;
    last_cx = x;
    last_cy = y;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    if (dx + dy >= kMovePx) {
        move_ms += kMoveTickMs;
        still_ms = 0;
        if (move_ms >= kMoveNeedMs) {
            fprintf(stderr, "mattbar: idle: screensaver dismissed by mouse "
                            "(%dms of movement)\n",
                    move_ms);
            dismiss_screensaver("screensaver-mouse-move");
        }
    } else {
        still_ms += kMoveTickMs;
        if (still_ms >= kMoveStillMs) move_ms = 0;
    }
}

void start_cycle() {
    if (cycle || stay_awake || lock_is_locked()) return;
    saver_stopping = false;
    lock_pending   = false;
    relaunch_hits  = 0;
    cycle = true;
    fprintf(stderr, "mattbar: idle: cycle start saver=%ds lock=%ds\n", saver_s,
            lock_s);
    int first = std::min(saver_s, lock_s);
    int lock_delay  = lock_s - first;
    int saver_delay = saver_s - first;
    // We were already notified after `first` seconds of idle.
    // If lock is due now, never start the screensaver — lock_now kills
    // any leftover and the user would not see the saver anyway.
    if (lock_delay <= 0) {
        stop_screensaver();
        lock_now();
        cycle = false;
        return;
    }
    if (saver_delay <= 0 && !screensaver_toggled_off()) {
        saver_stopping = false;
        launch_screensaver();
        saver_on      = true;
        saver_started = true;
        ignore_resume = true;
        saver_move_after = idle_now_ms() + 2000; // settle before counting motion
        saver_reset_windows();
        // Mapping the screensaver looks like seat activity. Keep the
        // lock timer armed during launch and while the window exists.
        // Window death that is not a key or sustained mouse move is
        // relaunched; packaged omarchy-screensaver used to treat focus
        // loss and any stdin byte as a dismiss.
        arm_grace(3000);
        arm_watch(true);
        arm_move_watch(true);
        if (auto* nd = notify_daemon()) nd->refresh_popups();
    } else if (saver_delay <= 0) {
        fprintf(stderr, "mattbar: idle: screensaver toggled off; lock only\n");
    }
    if (lock_fd >= 0) {
        itimerspec ts{};
        ts.it_value.tv_sec = lock_delay;
        timerfd_settime(lock_fd, 0, &ts, nullptr);
    }
}

void arm_fresh_from_now() {
    int first = std::min(saver_s, lock_s);
    if (first < 1) first = 1;
    if (stale_fd < 0) return;
    itimerspec ts{};
    ts.it_value.tv_sec = first;
    timerfd_settime(stale_fd, 0, &ts, nullptr);
    fprintf(stderr,
            "mattbar: idle: stale idled (restart/rearm); counting a fresh "
            "%ds from now\n",
            first);
}

void on_idled(void*, ext_idle_notification_v1*) {
    if (!cfg.quickshell_shutdown || stay_awake) return;
    if (idle_now_ms() < rearm_until) {
        arm_fresh_from_now();
        return;
    }
    start_cycle();
}

void on_resumed(void*, ext_idle_notification_v1*) {
    // ext-idle-notify fires resumed ONCE when the seat leaves idle.
    // Mapping the saver, OSD, DPMS, and synthetic pointer events all
    // look like seat activity. While this cycle owns a screensaver,
    // only a real key (mattbarctl idle screensaver-key) or sustained
    // mouse movement dismisses it.
    if (!cycle) return;
    if (ignore_resume || saver_started) {
        fprintf(stderr,
                "mattbar: idle: activity while screensaver cycle; lock stays "
                "armed\n");
        return;
    }
    cancel_cycle("activity");
}

void on_saver_opened(const std::string& addr) {
    if (addr.empty()) return;
    saver_addrs.insert(addr);
    ignore_resume = false;
    arm_grace(0);
    disarm_relaunch();
    fprintf(stderr, "mattbar: idle: screensaver window %s (%zu)\n",
            addr.c_str(), saver_addrs.size());
}

void on_saver_closed(const std::string& addr) {
    if (!saver_addrs.erase(addr)) return;
    fprintf(stderr, "mattbar: idle: screensaver window closed %s (%zu left)\n",
            addr.c_str(), saver_addrs.size());
    if (!cycle || !saver_started || !saver_addrs.empty()) return;
    if (saver_stopping) return;
    // Packaged omarchy-screensaver exits on focus loss and on any stdin
    // byte. Treat a vanished window as a crash, not a user dismiss.
    request_relaunch("window-closed");
}

void sock2_line(const std::string& l) {
    if (l.rfind("openwindow>>", 0) == 0) {
        // ADDRESS,WORKSPACE,CLASS,TITLE
        std::string rest = l.substr(12);
        size_t a = rest.find(',');
        size_t b = a == std::string::npos ? a : rest.find(',', a + 1);
        size_t c = b == std::string::npos ? b : rest.find(',', b + 1);
        if (c == std::string::npos) return;
        std::string addr = rest.substr(0, a);
        std::string cls  = rest.substr(b + 1, c - b - 1);
        if (cls == kSaverClass) on_saver_opened(addr);
        return;
    }
    if (l.rfind("closewindow>>", 0) == 0)
        on_saver_closed(l.substr(13));
}

void destroy_notif() {
    cancel_cycle("disarm");
    disarm_stale_timer();
    if (notif) {
        ext_idle_notification_v1_destroy(notif);
        notif = nullptr;
    }
    armed_first_ms = -1;
}

void create_notif() {
    destroy_notif();
    if (!g_bar || !g_bar->idle_notifier() || !g_bar->seat()) return;
    if (stay_awake || access(stay_path().c_str(), F_OK) == 0) {
        stay_awake = true;
        return;
    }
    read_timeouts();
    int first_ms = std::min(saver_s, lock_s) * 1000;
    if (first_ms < 1000) first_ms = 1000;
    notif = ext_idle_notifier_v1_get_idle_notification(
        g_bar->idle_notifier(), (uint32_t)first_ms, g_bar->seat());
    static const ext_idle_notification_v1_listener lst = {
        .idled   = on_idled,
        .resumed = on_resumed,
    };
    ext_idle_notification_v1_add_listener(notif, &lst, nullptr);
    armed_first_ms = first_ms;
    // Hyprland sends idled immediately if the seat is already idle
    // (typical after a crash/restart). Do not lock/screensaver on that.
    rearm_until = idle_now_ms() + 2500;
    fprintf(stderr, "mattbar: idle: watching after %dms\n", first_ms);
}

} // namespace

void idle_init(Bar& bar) {
    g_bar = &bar;
    read_timeouts();
    stay_awake = access(stay_path().c_str(), F_OK) == 0;
    idle_reset_session();
    restore_pointer();
    lock_fd    = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    grace_fd   = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    stale_fd   = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    if (lock_fd >= 0)
        bar.add_fd(
            lock_fd,
            [](uint32_t) {
                uint64_t x;
                while (read(lock_fd, &x, sizeof x) > 0) {}
                if (!cycle || stay_awake) return;
                // Lock is due, but the saver is the thing the user is
                // looking at. Killing it here is what felt like a random
                // interrupt (idle.lock is only 30s after the saver on
                // this machine). Keep the animation until a real key or
                // sustained mouse move, then lock.
                if (saver_started) {
                    lock_pending = true;
                    fprintf(stderr,
                            "mattbar: idle: lock due; screensaver stays "
                            "until key or mouse\n");
                    return;
                }
                fprintf(stderr, "mattbar: idle: lock timeout\n");
                saver_stopping = true;
                disarm_relaunch();
                stop_screensaver();
                restore_pointer();
                lock_now();
                cycle         = false;
                saver_started = false;
                saver_reset_windows();
                arm_watch(false);
                arm_move_watch(false);
                arm_grace(0);
            },
            "idle-lock");
    if (grace_fd >= 0)
        bar.add_fd(
            grace_fd,
            [](uint32_t) {
                uint64_t x;
                while (read(grace_fd, &x, sizeof x) > 0) {}
                ignore_resume = false;
                if (!cycle || !saver_started || saver_stopping) return;
                if (saver_up()) return;
                request_relaunch("screensaver-not-running");
            },
            "idle-grace");
    if (stale_fd >= 0)
        bar.add_fd(
            stale_fd,
            [](uint32_t) {
                uint64_t x;
                while (read(stale_fd, &x, sizeof x) > 0) {}
                if (stay_awake || lock_is_locked()) return;
                fprintf(stderr, "mattbar: idle: fresh idle elapsed after rearm\n");
                start_cycle();
            },
            "idle-stale");
    motion_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    if (motion_fd >= 0)
        bar.add_fd(
            motion_fd,
            [](uint32_t) {
                uint64_t x;
                while (read(motion_fd, &x, sizeof x) > 0) {}
                on_move_tick();
            },
            "idle-mouse-move");
    relaunch_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    if (relaunch_fd >= 0)
        bar.add_fd(
            relaunch_fd,
            [](uint32_t) {
                uint64_t x;
                while (read(relaunch_fd, &x, sizeof x) > 0) {}
                do_relaunch("retry");
            },
            "idle-saver-relaunch");
    watch_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    if (watch_fd >= 0)
        bar.add_fd(
            watch_fd,
            [](uint32_t) {
                uint64_t x;
                while (read(watch_fd, &x, sizeof x) > 0) {}
                if (!cycle || !saver_started || ignore_resume ||
                    saver_stopping)
                    return;
                if (saver_up()) {
                    disarm_relaunch();
                    return;
                }
                request_relaunch("screensaver-process-gone");
            },
            "idle-saver-watch");
    // Screensaver window tracking rides the shared Hyprland event stream
    // (hyprev.hpp); while it is down, the process watch still catches a
    // dismissed screensaver.
    hyprev::subscribe(bar, {"openwindow", "closewindow"},
                      [](const HyprEvent& ev) {
                          sock2_line(std::string(ev.line));
                      });
}

void idle_apply() {
    if (!cfg.quickshell_shutdown) {
        destroy_notif();
        idle_reset_session();
        return;
    }
    stay_awake = access(stay_path().c_str(), F_OK) == 0;
    if (stay_awake) {
        destroy_notif();
        return;
    }
    read_timeouts();
    int first_ms = std::min(saver_s, lock_s) * 1000;
    if (first_ms < 1000) first_ms = 1000;
    // Recreating the notification while the seat is already idle makes
    // Hyprland fire idled immediately. Settings/takeover apply used to
    // do that on every click.
    if (notif && armed_first_ms == first_ms) return;
    create_notif();
}

std::string idle_status_json() {
    char buf[256];
    snprintf(buf, sizeof buf,
             "{\"enabled\":%s,\"stayAwake\":%s,\"screensaver\":%d,"
             "\"lock\":%d,\"blank\":%d,\"inIdleCycle\":%s}",
             (!stay_awake && cfg.quickshell_shutdown) ? "true" : "false",
             stay_awake ? "true" : "false", saver_s, lock_s, cfg.idle_blank_s,
             cycle ? "true" : "false");
    return buf;
}

int idle_screensaver_s() { return saver_s; }
int idle_lock_s() { return lock_s; }

void idle_set_screensaver_s(int s) {
    saver_s = std::clamp(s, 15, 3600);
    write_timeouts();
    if (cfg.quickshell_shutdown && !stay_awake) create_notif();
}

void idle_set_lock_s(int s) {
    lock_s = std::clamp(s, 15, 7200);
    write_timeouts();
    if (cfg.quickshell_shutdown && !stay_awake) create_notif();
}

void idle_stop_screensaver() { stop_screensaver_now(); }

bool idle_screensaver_up() { return saver_on || saver_started; }

void idle_screensaver_key() {
    if (!cycle || !saver_started) return;
    fprintf(stderr, "mattbar: idle: screensaver dismissed by keyboard\n");
    dismiss_screensaver("screensaver-keyboard");
}

void idle_reset_session() {
    saver_stopping = true;
    lock_pending   = false;
    stop_screensaver_now();
    cycle         = false;
    saver_started = false;
    ignore_resume = false;
    saver_reset_windows();
    disarm_lock_timer();
    disarm_stale_timer();
    disarm_relaunch();
    arm_watch(false);
    arm_move_watch(false);
    arm_grace(0);
}

void idle_restore_pointer() { restore_pointer(); }

void idle_shutdown() {
    destroy_notif();
    idle_reset_session();
    restore_pointer();
}

std::string idle_set_enabled(bool on) {
    // Omarchy: enabled idle == !stayAwake
    stay_awake = !on;
    std::string p = stay_path();
    if (stay_awake) {
        auto slash = p.rfind('/');
        if (slash != std::string::npos) {
            std::string dir = p.substr(0, slash);
            auto slash2     = dir.rfind('/');
            if (slash2 != std::string::npos)
                mkdir(dir.substr(0, slash2).c_str(), 0700);
            mkdir(dir.c_str(), 0700);
        }
        int fd = open(p.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        if (fd >= 0) close(fd);
        cancel_cycle("stay-awake");
        destroy_notif();
        return "disabled";
    }
    unlink(p.c_str());
    if (cfg.quickshell_shutdown) create_notif();
    return "enabled";
}

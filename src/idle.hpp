#pragma once
// Screensaver + auto-lock via ext-idle-notify-v1. Active only while
// cfg.quickshell_shutdown is on. Timeouts come from Omarchy's shell.json
// idle.lock / idle.screensaver (defaults 300s / 150s).
#include <string>

class Bar;

void        idle_init(Bar&);
void        idle_apply(); // arm/disarm to match the takeover toggle
// Kill leftover screensaver and cancel an in-flight idle cycle. Call on
// start, crash-reclaim, unlock, and shutdown so a restart does not
// inherit a running ttfx from the previous process.
void        idle_reset_session();
// Wake DPMS and unhide the cursor without warping it off-screen. Call
// after the session lock is actually torn down, not while it still owns
// the pointer.
void        idle_restore_pointer();
void        idle_shutdown();
std::string idle_status_json();
std::string idle_set_enabled(bool on); // stay-awake inverted, like Omarchy IPC
// Kill ttfx / org.omarchy.screensaver. Safe if none is running. Call
// before showing the lock so the saver is not still burning CPU.
void        idle_stop_screensaver();
int         idle_screensaver_s();
int         idle_lock_s();
void        idle_set_screensaver_s(int s); // persist to shell.json + re-arm
void        idle_set_lock_s(int s);
// Screensaver is covering the session. Mapping a notification overlay
// on top is treated as activity and brings the panel back.
bool        idle_screensaver_up();

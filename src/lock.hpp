#pragma once
// Session lock (ext-session-lock-v1 + PAM). Used when Quickshell is
// shut down; otherwise Omarchy's lock plugin remains in charge.
#include <string>

class Bar;

void        lock_init(Bar&);
std::string lock_now(); // "ok" / error string
bool        lock_is_locked();
std::string lock_status_json();
void        lock_destroy(); // if compositor finished without a lock
// If logind still reports LockedHint (previous lock client died, or
// Quickshell was killed mid-lock), take the lock so the password field
// comes back instead of a blank locked session.
void        lock_reclaim();
// Pointer/key on the lock surface: turn the backlight back on and
// restart the post-lock blank timer (Omarchy LockView.onWakeRequested).
void        lock_note_activity();
// Recreate lock surfaces for current outputs (hotplug / resume).
void        lock_sync_outputs();
// Paint lock surfaces that were marked dirty (password keystrokes
// coalesce here so one full-screen commit is not done per key).
void        lock_flush();
// True after the post-lock blank (brightness/DPMS off). Toast popups
// must not map a layer surface while this is set — that wakes the panel.
bool        lock_display_asleep();
// Same DPMS/brightness off the lock screen uses. Idle calls this when
// the post-lock-due blank fires, then takes the lock in the dark so a
// later wake shows the password field instead of the screensaver.
void        lock_blank_now();
// Undo lock_blank_now / post-lock blank. Cancels a pending reassert so
// a screensaver dismiss cannot be blanked again a beat later.
void        lock_wake_now();
// Stay lit for lock_wake_blank_s (Keep display on after wake). Idle
// calls this after a screensaver dismiss so the 5s post-lock dim does
// not black the panel under someone who just came back.
void        lock_hold_after_wake();

MattBar is a C++ based ultra efficient and powerful drop in replacement for waybar and specifically the Omarchy Quickshell bar. It is configurable through a GUI, ultra light weight (19 Meg RAM) and incredible light CPU utilization. It supports Omarchy theming, as well as most of the Omarchy bar's modules. However, this does not mean that it is Quattro plugin compatible. 

<img width="1600" height="720" alt="image" src="https://github.com/user-attachments/assets/79846e09-ef13-4e43-8f5d-b0cb0bd544ea" />

MattBar is a minimal, auto-hiding status bar for Hyprland and other wlroots compositors, written in C++ against raw wayland-client + Cairo — no GTK, no Qt. It lives on the layer-shell overlay layer with a zero exclusive zone, so it takes no screen space and slides in over your windows when you touch the hot strip at the screen edge (with a pin module to keep it out). It docks to any edge — vertical bars get their own thickness and compact text layouts — and handles multi-monitor and HiDPI scaling natively.

Modules: workspaces, clock, pin, system tray (with auto-collapse), Omarchy menu, update indicator, temperature, network, volume (headset-aware, with mixer/mute/scroll bindings), Bluetooth (device name, battery, multi-connection count), brightness (vector-drawn sun, slider popup), media/MPRIS, stay-awake, notifications, power profile, and battery. Nearly everything is event-driven rather than polled — PipeWire events for audio, BlueZ D-Bus signals, kernel uevents for backlight, inotify for themes — so an idle bar costs essentially zero wakeups.

Notifications & OSD (opt-in): MattBar can act as the desktop notification daemon (org.freedesktop.Notifications), taking over from mako cleanly, with do-not-disturb, history/restore, actions, per-type toggles, and emoji rendering. Its volume/mic/brightness OSD shows a bar plus the numeric percentage, and because it reacts to the actual PipeWire/backlight change rather than keypresses, it works no matter what triggered the change. Ready-made drop-ins under contrib/ rebind Omarchy 3.x (swayosd) and 4.x/Quattro (Quickshell) media keys so MattBar draws the only overlay — on Quattro without losing the mic-mute LED or DDC external-display brightness.

Configuration: a plain key = value file at ~/.config/mattbar/mattbar.conf, plus a built-in click-only settings window (gear icon in the tray) — colors, sizes, delays, and per-module visibility all apply live, no save button. An optional Follow Omarchy theme toggle tracks Omarchy's active theme instantly via inotify (both the 4.x colors.toml palette and older waybar.css themes), preserving your transparency preferences. Runtime control goes through mattbar ctl or plain signals (pkill -RTMIN+N) for dismissing notifications, DND, and more.

You will need to move the mattbar.service file, which makes sure the bar is running, etc.: 
```
cp contrib/mattbar.service ~/.config/systemd/user/
systemctl --user daemon-reload
systemctl --user enable --now mattbar
```

If you want your notification handled by Mattbar, you will need to select that in settings, and also add the following things do your bindings.lua in .config/hypr/bindings.lua: 
```
hl.unbind("SUPER + comma")
hl.unbind("SUPER + SHIFT + comma")
hl.unbind("SUPER + CTRL + comma")
hl.unbind("SUPER + ALT + comma")
o.bind("SUPER + comma", "Dismiss last notification", "pkill -RTMIN+2 mattbar")
o.bind("SUPER + SHIFT + comma", "Dismiss all notifications", "pkill -RTMIN+3 mattbar")
o.bind("SUPER + CTRL + comma", "Toggle do-not-disturb", "pkill -RTMIN+5 mattbar")
o.bind("SUPER + ALT + comma", "Invoke last notification", "pkill -RTMIN+4 mattbar")
```

There are a few more things that you can do if you want to go further in your dedication to MattBar use, but these are optional. The bar is pretty flexible. You can just hide the Quattro bar with Super + Shift + Space. You don't have to terminate it, etc. 
I just REALLY prefer a taskbar that auto hides, and doesn't constantly changes the size of my open application windows. 

MattBar is a minimal, auto-hiding status bar primarily for Omarchy, but can be used with Hyprland and other wlroots compositors in genenral.
<img width="1600" height="720" alt="image" src="https://github.com/user-attachments/assets/383eeeb0-846b-419d-82f0-d1b61d34f5b2" />
MattBar consolidates all things to one thead, using only 21-38 Mb RAM. It can support plugins, but then you have to have quickshell running. Still you can use the plugin when you need it and shut down the quickshell instance as soon as you are done. 

MattBar replaces all components of the Omarchy shell, using glorious C++ and as few dependencies as possible. 

# Positioning
Dock it to any edge — top, bottom, left, or right — via the Position setting in General. Changes apply instantly. On vertical edges, modules stack compactly (time-only clock, shorter labels, stacked tray icons). 

# Auto-hide behavior
Windows always use the full screen — the bar never takes up space or causes reflow. 
When hidden, only a thin 2 px strip is visible at the screen edge.
Hover to reveal: move your mouse to that edge and the bar slides out over your windows. Move away and it hides again after ~500 ms.
Reveal delay (optional): set a dwell time so casually brushing the screen edge doesn't accidentally trigger the bar. 

# Full Quickshell replacement for Omarchy
MattBar can replace all of the quickshell interface, and you loose zero functionality. MattBar even includes full plugin support, with a dedicated bar just plugins. (all completely optional)

# Installation
Simply grab the zip in releases and follow the instructions there to run install.sh. 

#pragma once
// ---------------------------------------------------------------------------
// mattbarctl: a line-based control socket at $XDG_RUNTIME_DIR/mattbar.sock
// (0600, same-uid SO_PEERCRED). There is no /tmp fallback — a missing
// runtime dir means no socket, not a world-writable one.
// The RTMIN+N signal surface is nearly full and signals can't carry
// arguments or answer questions; the socket can do both:
//
//   mattbarctl dnd on|off|toggle|status
//   mattbarctl dismiss | dismiss-all | invoke | restore
//   mattbarctl profile [name]        (no name: prints active + available)
//   mattbarctl pin on|off|toggle|status
//   mattbarctl reveal | hide
//   mattbarctl hold on|off|toggle|status
//   mattbarctl settings [open|toggle|close|status]
//   mattbarctl status                (one-line summary)
//   mattbarctl shell ping|toggle|summon|hide <id> [payload]
//   mattbarctl notifications dismissOne|dismissAll|invokeLast|toggleDnd
//   mattbarctl osd show <json>
//   mattbarctl media playPause|next|previous
//   (omarchy-shell shim in contrib/mattbar-shell/)
//
// `mattbarctl` is the same binary (argv[0] symlink or `mattbar ctl ...`).
// ---------------------------------------------------------------------------
#include <string>

class Bar;

class CtlServer {
public:
    void init(Bar&);
    ~CtlServer();

private:
    std::string handle(const std::string& line);
    Bar* bar_       = nullptr;
    int  listen_fd_ = -1;
    std::string path_;
};

// Client side (used by main.cpp): send one command line, print the reply.
// Returns the process exit code.
int ctl_client(int argc, char** argv, bool quiet = false);
// argv[0] dispatch when this binary is invoked as omarchy-shell / omarchy-menu
// (the PATH shim used only while quickshell_shutdown is on).
int omarchy_shell_client(int argc, char** argv);
int omarchy_menu_client(int argc, char** argv);

std::string ctl_socket_path();

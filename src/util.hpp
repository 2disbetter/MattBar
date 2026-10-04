#pragma once
#include <cstdio>
#include <cstdlib>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <climits>
#include <fstream>
#include <sstream>
#include <string>

// Read a whole (small) file, e.g. sysfs attributes. Empty string on failure.
inline std::string slurp(const std::string& path) {
    std::ifstream f(path);
    if (!f) return {};
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Write a file atomically: temp file in the same directory, then rename.
// A crash, watchdog kill or full disk leaves either the old or the new
// content, never a truncated file. Symlinks are followed (the target is
// replaced, the link survives) and an existing file keeps its mode.
inline bool atomic_write(const std::string& path, const std::string& data) {
    std::string target = path;
    if (char* real = realpath(path.c_str(), nullptr)) {
        target = real;
        free(real);
    }
    mode_t      mode = 0644;
    struct stat st {};
    if (stat(target.c_str(), &st) == 0) mode = st.st_mode & 07777;
    std::string tmp = target + ".tmp." + std::to_string(getpid());
    int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
    if (fd < 0) return false;
    fchmod(fd, mode); // umask must not narrow an existing file's mode
    const char* p    = data.data();
    size_t      left = data.size();
    while (left > 0) {
        ssize_t n = write(fd, p, left);
        if (n < 0) {
            if (errno == EINTR) continue;
            close(fd);
            unlink(tmp.c_str());
            return false;
        }
        p += n;
        left -= (size_t)n;
    }
    // No fsync: btrfs and ext4 (auto_da_alloc) both flush data before a
    // rename-over-existing commits, and an fsync here would put disk
    // latency on the event loop.
    if (close(fd) != 0 || rename(tmp.c_str(), target.c_str()) != 0) {
        unlink(tmp.c_str());
        return false;
    }
    return true;
}

// Capture stdout of a shell command; optionally its exit status
// (-1 if it did not terminate normally or timed out). Bounded: after
// timeout_ms the command's whole process group is killed and whatever it
// printed so far is returned — a hung hyprctl/nmcli/quickshell can no
// longer stall the event loop until the watchdog fires. Children start
// with clean signal state (see spawn.hpp). Prefer AsyncCmd for anything
// not needed synchronously.
std::string run_capture_bounded(const std::string& cmd, int timeout_ms,
                                int* status, const std::string* input);
inline std::string cmd_output(const std::string& cmd, int* status = nullptr,
                              int timeout_ms = 3000) {
    return run_capture_bounded(cmd, timeout_ms, status, nullptr);
}

inline bool path_has_dir(const std::string& path, const std::string& dir) {
    if (dir.empty()) return false;
    for (size_t i = 0; i <= path.size();) {
        size_t j = path.find(':', i);
        if (j == std::string::npos) j = path.size();
        if (path.compare(i, j - i, dir) == 0) return true;
        i = j + 1;
    }
    return false;
}

// systemd unit PATH is often just /usr/bin. User tools (grok, mise, pipx)
// live in ~/.local/bin. Prepend those so Hyprland exec and omarchy-agent
// see the same commands as an interactive shell.
inline std::string enrich_user_path(std::string path) {
    if (path.empty()) path = "/usr/local/bin:/usr/bin";
    auto prepend = [&](const std::string& d) {
        if (d.empty() || path_has_dir(path, d)) return;
        path = d + ":" + path;
    };
    if (const char* home = getenv("HOME")) {
        prepend(std::string(home) + "/.local/bin");
        prepend(std::string(home) + "/.local/share/mise/shims");
    }
    return path;
}

inline void apply_enriched_path() {
    const char* p = getenv("PATH");
    std::string np = enrich_user_path(p ? p : "");
    if (p && np == p) return;
    setenv("PATH", np.c_str(), 1);
}

inline std::string trim(std::string s) {
    const char* ws = " \t\r\n";
    auto b = s.find_first_not_of(ws);
    if (b == std::string::npos) return {};
    auto e = s.find_last_not_of(ws);
    return s.substr(b, e - b + 1);
}

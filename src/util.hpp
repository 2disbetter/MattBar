#pragma once
#include <cstdio>
#include <sys/wait.h>
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

// Capture stdout of a shell command; optionally its exit status
// (-1 if it did not terminate normally). Empty string on failure.
inline std::string cmd_output(const std::string& cmd, int* status = nullptr) {
    if (status) *status = -1;
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return {};
    std::string out;
    char buf[512];
    while (fgets(buf, sizeof buf, p)) out += buf;
    int rc = pclose(p);
    if (status && rc >= 0 && WIFEXITED(rc)) *status = WEXITSTATUS(rc);
    return out;
}

inline std::string trim(std::string s) {
    const char* ws = " \t\r\n";
    auto b = s.find_first_not_of(ws);
    if (b == std::string::npos) return {};
    auto e = s.find_last_not_of(ws);
    return s.substr(b, e - b + 1);
}

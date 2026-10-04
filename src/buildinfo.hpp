#pragma once
// Which build is this process? (settings > Bar > About, `mattbar
// --version`, `mattbarctl version`)
//
// The version string only changes per release; the build id changes with
// every link that changes the code. It is the linker's GNU build-id (a
// SHA-1 over the linked binary, forced on in CMakeLists.txt), read from
// this process's own ELF notes, so it names the code that is actually
// running — not whatever file happens to sit at the install path now.
// __DATE__/__TIME__ were not good enough: they only change when main.cpp
// itself is recompiled.
#include <string>

struct BuildInfo {
    std::string version;  // MATTBAR_VERSION, e.g. "1.42.8"
    std::string id;       // full build id, lowercase hex
    std::string short_id; // first 7 hex digits, the one people compare
    std::string linked;   // "YYYY-MM-DD HH:MM", mtime of the running binary
    std::string path;     // where the running binary was started from
};

// Computed on first use, then cached (the running image never changes).
const BuildInfo& build_info();

// "1.42.8 · build 3f9a1c2 · 2026-09-28 14:32"
std::string build_line();

// Is the file at build_info().path still the binary this process runs?
enum class ExeState {
    Same,     // yes
    Replaced, // a different file is there now (rebuilt / reinstalled)
    Gone,     // nothing is there any more
};
// Cheap (readlink + stat): fine to call on every settings redraw. For
// Replaced, *disk_linked gets the new file's mtime in the same format.
ExeState exe_state(std::string* disk_linked = nullptr);

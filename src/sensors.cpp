#include "sensors.hpp"
#include "config.hpp"
#include "util.hpp"

#include <dirent.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

static std::string hwmon_base() {
    const char* env = getenv("MATTBAR_HWMON");
    return env && *env ? env : "/sys/class/hwmon";
}

// Map raw hwmon labels to short bar text.
static std::string display_for(const std::string& name,
                               const std::string& label) {
    if (label == "Tctl" || label == "Tdie" || label.rfind("Package", 0) == 0 ||
        label == "CPU")
        return "CPU";
    if (label == "Composite" || label.rfind("temp", 0) == 0 || label.empty())
        return name; // generic: chip name is better (nvme, acpitz)
    return label;    // "Core 3", "edge", "junction", ...
}

std::vector<TempSensorInfo> temp_available_sensors() {
    std::vector<TempSensorInfo> out;
    std::string base = hwmon_base();
    DIR* d = opendir(base.c_str());
    if (!d) return out;

    struct Raw { std::string name; int idx; TempSensorInfo info; };
    std::vector<Raw> raws;

    while (dirent* e = readdir(d)) {
        if (strncmp(e->d_name, "hwmon", 5) != 0) continue;
        std::string dir  = base + "/" + e->d_name;
        std::string name = trim(slurp(dir + "/name"));
        if (name.empty()) name = e->d_name;

        DIR* hd = opendir(dir.c_str());
        if (!hd) continue;
        while (dirent* f = readdir(hd)) {
            int idx = 0;
            // match temp<N>_input
            if (sscanf(f->d_name, "temp%d_input", &idx) != 1) continue;
            if (!strstr(f->d_name, "_input")) continue;
            std::string input = dir + "/" + f->d_name;
            std::string label = trim(
                slurp(dir + "/temp" + std::to_string(idx) + "_label"));
            std::string label_key =
                label.empty() ? "temp" + std::to_string(idx) : label;
            raws.push_back(
                {name, idx,
                 {name + ":" + label_key, display_for(name, label), input}});
        }
        closedir(hd);
    }
    closedir(d);

    std::sort(raws.begin(), raws.end(), [](const Raw& a, const Raw& b) {
        return a.name != b.name ? a.name < b.name : a.idx < b.idx;
    });
    for (auto& r : raws) out.push_back(std::move(r.info));
    return out;
}

const TempSensorInfo* temp_pick_auto(
    const std::vector<TempSensorInfo>& list) {
    // (chip prefix, label prefix) in descending preference
    static const std::pair<const char*, const char*> prefs[] = {
        {"k10temp", "Tctl"},    {"k10temp", "Tdie"}, {"zenpower", "Tdie"},
        {"zenpower", "Tctl"},   {"coretemp", "Package"},
        {"cpu_thermal", ""},    {"coretemp", ""},    {"acpitz", ""},
    };
    for (auto& [chip, lbl] : prefs)
        for (auto& s : list)
            if (s.id.rfind(std::string(chip) + ":", 0) == 0 &&
                (!*lbl || s.id.find(std::string(":") + lbl) !=
                              std::string::npos))
                return &s;
    return list.empty() ? nullptr : &list.front();
}

void temp_cycle_sensor(int dir) {
    auto list = temp_available_sensors();
    std::vector<std::string> ids = {"auto"};
    for (auto& s : list) ids.push_back(s.id);
    int n = static_cast<int>(ids.size());
    int cur = 0;
    for (int i = 0; i < n; ++i)
        if (ids[i] == cfg.temp_sensor) { cur = i; break; }
    cfg.temp_sensor = ids[((cur + dir) % n + n) % n];
}

std::string temp_current_display() {
    auto list = temp_available_sensors();
    if (cfg.temp_sensor == "auto") {
        const TempSensorInfo* p = temp_pick_auto(list);
        return "auto (" + (p ? p->display : std::string("none")) + ")";
    }
    for (auto& s : list)
        if (s.id == cfg.temp_sensor) return s.display;
    return cfg.temp_sensor + " (missing)";
}

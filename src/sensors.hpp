#pragma once
// hwmon temperature sensor discovery (no Wayland deps; unit-testable).
#include <string>
#include <vector>

struct TempSensorInfo {
    std::string id;      // "name:label" — stored in cfg.temp_sensor
    std::string display; // short bar label ("CPU", "Core 3", ...)
    std::string path;    // .../tempN_input (millidegrees C)
};

std::vector<TempSensorInfo> temp_available_sensors();

// CPU-priority pick for cfg.temp_sensor == "auto"; null if none.
const TempSensorInfo* temp_pick_auto(const std::vector<TempSensorInfo>&);

// Cycle cfg.temp_sensor through ["auto", ...ids]; dir = +1 / -1.
void temp_cycle_sensor(int dir);

// Human-readable name of current selection (settings UI).
std::string temp_current_display();

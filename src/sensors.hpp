#pragma once
// hwmon temperature sensor discovery (no Wayland deps; unit-testable).
// Sensor ids are "hwmon_name:label", e.g. "k10temp:Tctl", "nvme:Composite".
// Set MATTBAR_HWMON to override /sys/class/hwmon (used by tests/debugging).
#include <string>
#include <vector>

struct TempSensorInfo {
    std::string id;      // "name:label" — stored in cfg.temp_sensor
    std::string display; // short label shown on the bar ("CPU", "Core 3", ...)
    std::string path;    // .../tempN_input (millidegrees C)
};

std::vector<TempSensorInfo> temp_available_sensors();

// CPU-priority pick used for cfg.temp_sensor == "auto"; null if none found.
const TempSensorInfo* temp_pick_auto(const std::vector<TempSensorInfo>&);

// Cycle cfg.temp_sensor through ["auto", ...sensor ids]; dir = +1 / -1.
void temp_cycle_sensor(int dir);

// Human-readable name of the current selection, for the settings window.
std::string temp_current_display();

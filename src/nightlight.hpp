#pragma once
// Owns hyprsunset temperature the way Omarchy's omarchy.nightlight service
// does: 4000 K = night, 6500 K = day, below 6000 K counts as on.
#include <string>

class Bar;

void        nightlight_init(Bar&);
void        nightlight_refresh();
std::string nightlight_status_json();
std::string nightlight_set(bool on);
std::string nightlight_toggle();
bool        nightlight_enabled();

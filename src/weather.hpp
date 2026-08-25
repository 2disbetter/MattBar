#pragma once
#include <string>

class Bar;
class Module;
struct Overlay;

void        weather_init(Bar&);
void        weather_refresh(bool force);
std::string weather_bar_label();
Module*     make_weather();
Overlay*    make_weather_overlay();

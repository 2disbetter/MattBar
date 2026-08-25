#pragma once
// Opt-in Quickshell sidecar for user Omarchy plugins while MattBar
// takeover is on. Off (default): no qs process. On, with at least one
// plugin placed or a service enabled: a stripped omarchy-shell child
// (null-bar, first-party session plugins disabled) that dies with us.
#include "bar.hpp"
#include <string>
#include <vector>
class Module;
class Overlay;

struct QsPlugin {
    std::string id, name, description;
    std::vector<std::string> kinds;
    bool placeable    = false; // bar-widget / overlay / panel / menu
    bool service_only = false;
};

std::string              qs_module_id(const std::string& plugin_id);
bool                     qs_is_module_id(const std::string& module_id);
std::string              qs_plugin_id_of(const std::string& module_id);
const std::vector<QsPlugin>& qs_plugins_catalog();
const QsPlugin*          qs_plugin_find(const std::string& plugin_id);
bool                     qs_plugin_known(const std::string& plugin_id);
bool                     qs_plugin_service_on(const std::string& plugin_id);
void                     qs_plugin_set_service(const std::string& plugin_id,
                                               bool on);
bool qs_plugin_shown(const std::string& plugin_id);
void qs_plugin_set_shown(const std::string& plugin_id, bool on);
void qs_plugin_move(const std::string& plugin_id, int delta);
std::vector<std::string> qs_plugin_layout_ids();

Module* make_plugins();
void    plugins_close();
bool    plugins_is_open();
Overlay* make_plugin_add_overlay();
Overlay* make_plugin_remove_overlay();

bool qs_plugins_want_runtime();
bool qs_plugins_running();
void qs_plugins_scan();
void qs_plugins_sync_modules(Bar& bar);
void qs_plugins_apply(Bar& bar);
void qs_plugins_reap();
void qs_plugins_stop();
// User-facing shutdown: hide overlays, flip qs_plugins off, kill the
// sidecar and restore the user's shell.json. Plugins stay in the chip
// so the next click can start Quickshell again.
void qs_plugins_shutdown();
// Forward an IPC call to the live sidecar (`qs ipc`), or empty if down.
std::string qs_plugins_ipc(const std::string& target, const std::string& method,
                           const std::string& arg);
// Bring the sidecar up if needed and summon/toggle a placed plugin.
// Accordion clicks and `omarchy-shell shell summon <id>` both land here
// so a checked plugin is never a silent no-op when the sidecar is down.
void qs_plugins_activate(const std::string& plugin_id,
                         const std::string& method = "summon",
                         const std::string& arg    = "{}");

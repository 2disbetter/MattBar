#pragma once
// PolicyKit authentication agent. Registered only while
// cfg.quickshell_shutdown is on (otherwise Quickshell owns the agent).
class Bar;

void polkit_init(Bar&);
void polkit_apply(); // register/unregister to match the takeover toggle

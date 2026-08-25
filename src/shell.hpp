#pragma once
// ---------------------------------------------------------------------------
// MattBar Shell host: named overlays + Omarchy-compatible IPC dispatch.
// See SHELL.md. Overlays register by id (`omarchy.menu`, `omarchy.audio`,
// …). mattbarctl (and the omarchy-shell shim) talk to this, not to qs ipc.
// ---------------------------------------------------------------------------
#include <functional>
#include <map>
#include <string>

class Bar;

struct Overlay {
    virtual ~Overlay() = default;
    virtual const char* id() const = 0;
    virtual void        summon(const std::string& payload) = 0;
    virtual void        hide() = 0;
    virtual bool        is_open() const = 0;
    virtual std::string call(const std::string& /*method*/,
                             const std::string& /*arg*/) {
        return "unknown";
    }
};

class Shell {
public:
    void init(Bar&);
    ~Shell();
    Bar* bar() const { return bar_; }
    // Install or remove the omarchy-shell / omarchy-menu PATH shim
    // according to cfg.quickshell_shutdown. Safe to call on every
    // settings apply.
    void apply_takeover();

    void add(Overlay*); // takes ownership

    std::string ping() const { return "ok"; }
    std::string toggle(const std::string& id, const std::string& payload);
    std::string summon(const std::string& id, const std::string& payload);
    std::string hide(const std::string& id);
    bool        is_open(const std::string& id) const;
    std::string call(const std::string& id, const std::string& method,
                     const std::string& arg);

    // Full ctl line after the first token: "toggle omarchy.menu {…}"
    // or a whole-line dispatch from mattbarctl ("shell ping", "lock lock",
    // "notifications dismissOne", "osd show …", "media playPause").
    std::string handle(const std::string& target, const std::string& rest);

private:
    Overlay* find(const std::string& id) const;
    Bar* bar_ = nullptr;
    std::map<std::string, Overlay*> overlays_;
};

Shell* mattbar_shell();

// Overlays implemented in their own TUs and registered from Shell::init.
Overlay* make_menu_overlay();

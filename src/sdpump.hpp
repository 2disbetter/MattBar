#pragma once
#include "bar.hpp"
#include <systemd/sd-bus.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <poll.h>
#include <unistd.h>
#include <cstring>
#include <functional>

// --------------------------------------------------------------------------- Shared sd-bus pump for event-driven bus modules (bluetooth: system bus, media: session bus).
struct SdPump {
    sd_bus*  bus      = nullptr;
    int      fd       = -1;
    int      timer_fd = -1;
    uint32_t events   = 0;
    int      barren   = 0;
    Bar*     bar      = nullptr;
    std::function<void(const char*)> on_teardown;

    ~SdPump() {
        if (bus) sd_bus_unref(bus);
        if (timer_fd >= 0) close(timer_fd);
    }
    // `opened` must be freshly opened; the pump takes ownership.
    void attach(Bar& b, sd_bus* opened, const char* label) {
        bar      = &b;
        bus      = opened;
        timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
        bar->add_fd(timer_fd, [this](uint32_t) {
            uint64_t n;
            while (read(timer_fd, &n, sizeof n) > 0) {}
            process();
        }, label);
        fd     = sd_bus_get_fd(bus);
        events = EPOLLIN;
        bar->add_fd(fd, [this](uint32_t ev) {
            if (ev & (EPOLLHUP | EPOLLERR)) {
                teardown("connection lost");
                return;
            }
            process();
        }, label);
    }
    void teardown(const char* why) {
        if (bar && fd >= 0) bar->remove_fd(fd); // drop stale callback entry
        if (bus) sd_bus_unref(bus);             // closes fd
        bus = nullptr;
        fd  = -1;
        if (timer_fd >= 0) {
            if (bar) bar->remove_fd(timer_fd);
            close(timer_fd);
            timer_fd = -1;
        }
        events = 0;
        barren = 0;
        if (on_teardown) on_teardown(why); // may re-attach with a fresh bus
    }
    void process() {
        if (!bus) return;
        int r, progress = 0;
        while ((r = sd_bus_process(bus, nullptr)) > 0) ++progress;
        if (r < 0) {
            teardown(strerror(-r));
            return;
        }
        if (progress == 0) {
            if (++barren > 2000) {
                teardown("wakeup storm with no messages");
                return;
            }
        } else {
            barren = 0;
        }
        int      ev = sd_bus_get_events(bus);
        uint32_t e  = 0;
        if (ev > 0) {
            if (ev & POLLIN) e |= EPOLLIN;
            if (ev & POLLOUT) e |= EPOLLOUT;
        } else if (ev < 0) {
            e = EPOLLIN;
        }
        if (e != events) {
            events = e;
            bar->mod_fd(fd, e);
        }
        uint64_t to = UINT64_MAX;
        sd_bus_get_timeout(bus, &to);
        itimerspec ts{};
        int        flags = 0;
        if (to == 0) {
            ts.it_value.tv_nsec = 1;
        } else if (to != UINT64_MAX) {
            ts.it_value.tv_sec  = to / 1000000ULL;
            ts.it_value.tv_nsec = (to % 1000000ULL) * 1000;
            flags               = TFD_TIMER_ABSTIME;
        } // UINT64_MAX: ts stays zero -> disarm
        timerfd_settime(timer_fd, flags, &ts, nullptr);
    }
};

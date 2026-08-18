#pragma once
// Shared audio-change event stream.
#include <sys/types.h>

#include <functional>
#include <vector>

class Bar;

class AudioEvents {
public:
    // sink = output changed, source = input (mic) changed
    using Callback = std::function<void(bool sink, bool source)>;

    // Lazily starts the event stream on first subscriber.
    int  subscribe(Bar& bar, Callback cb);
    void unsubscribe(int id);

    // True while event stream is healthy (else fall back to polling).
    bool available() const { return fd_ >= 0; }

private:
    void start(Bar& bar);
    void stop();
    void schedule_restart();

    struct Sub {
        int      id;
        Callback cb;
    };
    std::vector<Sub> subs_;
    Bar*  bar_       = nullptr;
    int   next_id_   = 1;
    pid_t pid_       = -1;
    int   fd_        = -1;   // pactl stdout read end
    int   query_fd_  = -1;   // debounce timer
    int   retry_fd_  = -1;   // restart-after-death timer
    int   attempts_  = 0;
    bool  q_sink_    = false, q_source_ = false;
};

AudioEvents& audio_events();

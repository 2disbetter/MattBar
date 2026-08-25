#pragma once
// ---------------------------------------------------------------------------
// Shared audio-change event stream. One persistent `pactl subscribe`
// process feeds every consumer (volume module, OSD); subscribers are
// called once per debounced burst with which side changed. This is what
// lets the volume module stop polling: it queries the mixer only when the
// mixer actually changed.
// ---------------------------------------------------------------------------
#include <sys/types.h>

#include <functional>
#include <vector>

class Bar;

class AudioEvents {
public:
    // sink = output (speakers) changed, source = input (mic) changed
    using Callback = std::function<void(bool sink, bool source)>;

    // Lazily starts the event stream on the first subscriber.
    int  subscribe(Bar& bar, Callback cb);
    void unsubscribe(int id);

    // True while the underlying event stream is healthy. Consumers use
    // this to decide whether to fall back to polling.
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
    int   fd_        = -1;   // read end of pactl's stdout
    int   query_fd_  = -1;   // debounce timer
    int   retry_fd_  = -1;   // restart-after-death timer
    int   attempts_  = 0;
    bool  q_sink_    = false, q_source_ = false;
};

AudioEvents& audio_events();

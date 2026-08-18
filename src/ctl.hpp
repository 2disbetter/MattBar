#pragma once
// mattbarctl: line-based control socket at $XDG_RUNTIME_DIR/mattbar.sock.
#include <string>

class Bar;

class CtlServer {
public:
    void init(Bar&);
    ~CtlServer();

private:
    std::string handle(const std::string& line);
    Bar* bar_       = nullptr;
    int  listen_fd_ = -1;
    std::string path_;
};

// Client side (main.cpp): send one command line, print reply. Returns exit code.
int ctl_client(int argc, char** argv);

std::string ctl_socket_path();

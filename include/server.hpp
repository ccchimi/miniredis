#pragma once

#include <cstdint>

#include "store.hpp"

// A TCP server that speaks RESP and serves a single shared Store.
//
// The model is one thread per connected client. Each client thread parses
// commands from its own socket and runs them against the shared store, whose
// own locking makes concurrent access safe. This is easy to reason about and
// comfortably handles hundreds of clients; a later phase can swap in an event
// loop if we want to push to tens of thousands.
class Server {
public:
    explicit Server(uint16_t port);

    // Binds, listens, and accepts connections forever. Returns only on a fatal
    // setup error (in which case it has already logged the reason).
    int run();

private:
    uint16_t port_;
    Store store_;
};

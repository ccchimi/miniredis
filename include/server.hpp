#pragma once

#include <cstdint>
#include <string>

#include "store.hpp"

// A TCP server that speaks RESP and serves a single shared Store, with the
// keyspace persisted to a snapshot file.
//
// The model is one thread per connected client. Each client thread parses
// commands from its own socket and runs them against the shared store, whose
// own locking makes concurrent access safe. Two background threads handle
// maintenance: one evicts expired keys, one periodically saves a snapshot.
// This is easy to reason about and comfortably handles hundreds of clients; a
// later phase can swap in an event loop if we want to push to tens of thousands.
class Server {
public:
    // `snapshotPath` is the file the keyspace is loaded from at startup and
    // saved to periodically.
    Server(uint16_t port, std::string snapshotPath = "dump.mrdb");

    // Loads any existing snapshot, then binds, listens, and accepts connections
    // forever. Returns only on a fatal setup error (already logged).
    int run();

private:
    uint16_t port_;
    std::string snapshotPath_;
    Store store_;
};

#pragma once

#include <string>
#include <vector>

#include "store.hpp"

// Executes one already-parsed command against the store and returns the RESP
// bytes to send back to the client.
//
// `args[0]` is the command name (case-insensitive); the rest are its
// arguments. This layer knows nothing about sockets, which keeps it easy to
// unit-test: give it a Store and an argument vector, check the reply string.
std::string executeCommand(Store& store, const std::vector<std::string>& args);

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "server.hpp"

// Entry point. Usage: miniredis [port]   (default port 6379, same as Redis)
int main(int argc, char** argv) {
    uint16_t port = 6379;

    if (argc > 1) {
        int parsed = std::atoi(argv[1]);
        if (parsed < 1 || parsed > 65535) {
            std::fprintf(stderr, "invalid port '%s' (expected 1-65535)\n",
                         argv[1]);
            return 1;
        }
        port = static_cast<uint16_t>(parsed);
    }

    Server server(port);
    return server.run();
}

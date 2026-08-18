#include "server.hpp"

// Winsock headers must come before <windows.h>; WIN32_LEAN_AND_MEAN trims it.
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "commands.hpp"
#include "resp.hpp"

namespace {

// Reads and serves commands on one client socket until it closes or errors.
// Runs on its own thread; `store` is shared and does its own locking.
void handleClient(SOCKET clientSock, Store& store) {
    std::string inbuf;   // accumulates raw bytes across recv() calls
    char chunk[4096];

    while (true) {
        int n = recv(clientSock, chunk, sizeof(chunk), 0);
        if (n <= 0) {
            break;  // 0 = client closed cleanly, <0 = error
        }
        inbuf.append(chunk, static_cast<size_t>(n));

        // Drain every complete command currently sitting in the buffer.
        std::string outbuf;
        size_t pos = 0;
        bool protocolError = false;

        while (pos < inbuf.size()) {
            std::vector<std::string> args;
            resp::ParseStatus status = resp::parseCommand(inbuf, pos, args);
            if (status == resp::ParseStatus::Incomplete) {
                break;  // wait for more bytes
            }
            if (status == resp::ParseStatus::Error) {
                outbuf += resp::encodeError("ERR protocol error");
                protocolError = true;
                break;
            }
            outbuf += executeCommand(store, args);
        }

        // Drop the bytes we consumed, keeping any partial command.
        inbuf.erase(0, pos);

        if (!outbuf.empty()) {
            send(clientSock, outbuf.data(), static_cast<int>(outbuf.size()), 0);
        }
        if (protocolError) {
            break;
        }
    }

    closesocket(clientSock);
}

}  // namespace

Server::Server(uint16_t port) : port_(port) {}

int Server::run() {
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::fprintf(stderr, "WSAStartup failed\n");
        return 1;
    }

    SOCKET listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSock == INVALID_SOCKET) {
        std::fprintf(stderr, "socket() failed: %d\n", WSAGetLastError());
        WSACleanup();
        return 1;
    }

    // Let us re-bind the port immediately after a restart.
    int yes = 1;
    setsockopt(listenSock, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&yes), sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port_);

    if (bind(listenSock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) ==
        SOCKET_ERROR) {
        std::fprintf(stderr, "bind() failed on port %u: %d\n", port_,
                     WSAGetLastError());
        closesocket(listenSock);
        WSACleanup();
        return 1;
    }

    if (listen(listenSock, SOMAXCONN) == SOCKET_ERROR) {
        std::fprintf(stderr, "listen() failed: %d\n", WSAGetLastError());
        closesocket(listenSock);
        WSACleanup();
        return 1;
    }

    std::printf("miniredis listening on port %u\n", port_);
    std::fflush(stdout);

    // Active expiration: a background thread periodically evicts expired keys
    // so that memory is reclaimed even for keys no client ever touches again.
    // Passive expiration (on access) still handles the common case instantly.
    std::thread([this] {
        using namespace std::chrono_literals;
        while (true) {
            std::this_thread::sleep_for(100ms);
            store_.purgeExpired();
        }
    }).detach();

    while (true) {
        SOCKET clientSock = accept(listenSock, nullptr, nullptr);
        if (clientSock == INVALID_SOCKET) {
            std::fprintf(stderr, "accept() failed: %d\n", WSAGetLastError());
            continue;
        }
        // Hand the connection to a detached worker thread and go back to
        // accepting. store_ outlives every client, so passing it by reference
        // is safe.
        std::thread(handleClient, clientSock, std::ref(store_)).detach();
    }

    // Unreachable in this phase (the accept loop never returns), but kept for
    // when we add graceful shutdown.
    closesocket(listenSock);
    WSACleanup();
    return 0;
}

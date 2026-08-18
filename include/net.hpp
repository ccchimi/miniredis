#pragma once

// Thin cross-platform socket shim.
//
// Windows (Winsock) and POSIX sockets are almost the same API with a handful of
// spelling differences: the socket handle type, the "invalid" and "error"
// sentinels, how you close a socket, how you read the last error, and a
// per-process startup/teardown that only Windows needs. This header papers over
// exactly those differences so server.cpp can be written once for both.

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace net {

#ifdef _WIN32
using socket_t = SOCKET;
inline constexpr socket_t kInvalidSocket = INVALID_SOCKET;
inline constexpr int kSocketError = SOCKET_ERROR;
#else
using socket_t = int;
inline constexpr socket_t kInvalidSocket = -1;
inline constexpr int kSocketError = -1;
#endif

// Initializes the sockets subsystem (Winsock needs this once per process).
// Returns true on success; a no-op that always succeeds on POSIX.
inline bool init() {
#ifdef _WIN32
    WSADATA wsaData;
    return WSAStartup(MAKEWORD(2, 2), &wsaData) == 0;
#else
    return true;
#endif
}

// Tears the sockets subsystem down (Winsock only).
inline void cleanup() {
#ifdef _WIN32
    WSACleanup();
#endif
}

// Closes a socket handle.
inline void closeSocket(socket_t s) {
#ifdef _WIN32
    closesocket(s);
#else
    ::close(s);
#endif
}

// Returns the last socket error code, for logging.
inline int lastError() {
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

}  // namespace net

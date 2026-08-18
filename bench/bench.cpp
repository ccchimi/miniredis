// bench.cpp — a small load generator for miniredis.
//
// Opens N concurrent connections, each firing a stream of synchronous
// (send one, wait for reply) commands, and reports aggregate throughput in
// operations per second. First it runs a SET workload, then a GET workload
// over the keys it just wrote.
//
// Usage: miniredis_bench [port] [clients] [total-requests]
//        defaults:        6400   50        200000

#include "net.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

namespace {

using clock_type = std::chrono::steady_clock;

// One client connection with a small receive buffer, plus just enough of a
// RESP reply reader to know when a full reply has arrived.
class Conn {
public:
    bool connect(uint16_t port) {
        sock_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sock_ == net::kInvalidSocket) {
            return false;
        }
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(port);
        return ::connect(sock_, reinterpret_cast<sockaddr*>(&addr),
                         sizeof(addr)) != net::kSocketError;
    }

    void close() {
        if (sock_ != net::kInvalidSocket) {
            net::closeSocket(sock_);
            sock_ = net::kInvalidSocket;
        }
    }

    bool sendAll(const std::string& data) {
        size_t sent = 0;
        while (sent < data.size()) {
            int n = send(sock_, data.data() + sent,
                         static_cast<int>(data.size() - sent), 0);
            if (n <= 0) {
                return false;
            }
            sent += static_cast<size_t>(n);
        }
        return true;
    }

    // Reads and discards exactly one RESP reply. Handles +, -, :, $, and *.
    bool readReply() {
        if (!ensure(1)) {
            return false;
        }
        char type = buf_[0];
        if (type == '+' || type == '-' || type == ':') {
            size_t end = findLine(0);
            if (end == std::string::npos) {
                return false;
            }
            buf_.erase(0, end + 2);
            return true;
        }
        if (type == '$') {
            size_t end = findLine(1);
            if (end == std::string::npos) {
                return false;
            }
            long len = std::atol(buf_.substr(1, end - 1).c_str());
            buf_.erase(0, end + 2);
            if (len < 0) {
                return true;  // nil
            }
            if (!ensure(static_cast<size_t>(len) + 2)) {
                return false;
            }
            buf_.erase(0, static_cast<size_t>(len) + 2);
            return true;
        }
        if (type == '*') {
            size_t end = findLine(1);
            if (end == std::string::npos) {
                return false;
            }
            long count = std::atol(buf_.substr(1, end - 1).c_str());
            buf_.erase(0, end + 2);
            for (long i = 0; i < count; ++i) {
                if (!readReply()) {
                    return false;
                }
            }
            return true;
        }
        return false;
    }

private:
    // Ensures the buffer holds at least `n` bytes, reading more if needed.
    bool ensure(size_t n) {
        char tmp[4096];
        while (buf_.size() < n) {
            int r = recv(sock_, tmp, sizeof(tmp), 0);
            if (r <= 0) {
                return false;
            }
            buf_.append(tmp, static_cast<size_t>(r));
        }
        return true;
    }

    // Returns the index of the CRLF that ends the line starting at `from`,
    // reading more bytes as necessary.
    size_t findLine(size_t from) {
        while (true) {
            size_t pos = buf_.find("\r\n", from);
            if (pos != std::string::npos) {
                return pos;
            }
            if (!ensure(buf_.size() + 1)) {
                return std::string::npos;
            }
        }
    }

    net::socket_t sock_ = net::kInvalidSocket;
    std::string buf_;
};

std::string bulk(const std::string& s) {
    return "$" + std::to_string(s.size()) + "\r\n" + s + "\r\n";
}

std::string setCmd(const std::string& key, const std::string& value) {
    return "*3\r\n" + bulk("SET") + bulk(key) + bulk(value);
}

std::string getCmd(const std::string& key) {
    return "*2\r\n" + bulk("GET") + bulk(key);
}

// Runs `opsPerClient` operations on each of `clients` connections and returns
// the elapsed seconds. `fail` is set if any connection errors out.
double runWorkload(uint16_t port, int clients, int opsPerClient, bool doSet,
                   std::atomic<bool>& fail) {
    auto work = [&](int) {
        Conn conn;
        if (!conn.connect(port)) {
            fail.store(true);
            return;
        }
        for (int i = 0; i < opsPerClient; ++i) {
            std::string key = "key:" + std::to_string(i);
            std::string cmd = doSet ? setCmd(key, "value") : getCmd(key);
            if (!conn.sendAll(cmd) || !conn.readReply()) {
                fail.store(true);
                break;
            }
        }
        conn.close();
    };

    auto start = clock_type::now();
    std::vector<std::thread> threads;
    threads.reserve(clients);
    for (int c = 0; c < clients; ++c) {
        threads.emplace_back(work, c);
    }
    for (auto& t : threads) {
        t.join();
    }
    auto end = clock_type::now();
    return std::chrono::duration<double>(end - start).count();
}

void report(const char* name, int ops, double seconds, int clients) {
    double perSec = seconds > 0 ? ops / seconds : 0;
    std::printf("%-4s %8d ops in %6.2f s  =>  %10.0f ops/sec  (%d clients)\n",
                name, ops, seconds, perSec, clients);
}

}  // namespace

int main(int argc, char** argv) {
    uint16_t port = 6400;
    int clients = 50;
    int totalRequests = 200000;
    if (argc > 1) port = static_cast<uint16_t>(std::atoi(argv[1]));
    if (argc > 2) clients = std::atoi(argv[2]);
    if (argc > 3) totalRequests = std::atoi(argv[3]);
    if (clients < 1) clients = 1;

    if (!net::init()) {
        std::fprintf(stderr, "socket init failed\n");
        return 1;
    }

    int opsPerClient = totalRequests / clients;
    int actualOps = opsPerClient * clients;
    std::printf("miniredis benchmark: port %u, %d clients, %d requests each\n\n",
                port, clients, opsPerClient);

    std::atomic<bool> fail{false};
    double setSecs = runWorkload(port, clients, opsPerClient, true, fail);
    double getSecs = runWorkload(port, clients, opsPerClient, false, fail);

    if (fail.load()) {
        std::fprintf(stderr,
                     "\nerror: a connection failed. Is the server running on "
                     "port %u?\n",
                     port);
        net::cleanup();
        return 1;
    }

    report("SET", actualOps, setSecs, clients);
    report("GET", actualOps, getSecs, clients);
    net::cleanup();
    return 0;
}

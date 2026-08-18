#include "commands.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <optional>

#include "resp.hpp"

namespace {

std::string toUpper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::toupper(c); });
    return s;
}

// Standard Redis-style arity error, e.g. for `SET` with no value.
std::string wrongArgs(const std::string& name) {
    return resp::encodeError("ERR wrong number of arguments for '" + name +
                             "' command");
}

// Strictly parses a base-10 signed integer. Rejects empty strings, stray
// characters, and overflow.
bool parseLongLong(const std::string& s, long long& out) {
    if (s.empty()) {
        return false;
    }
    size_t i = 0;
    bool negative = false;
    if (s[i] == '+' || s[i] == '-') {
        negative = (s[i] == '-');
        ++i;
        if (i == s.size()) {
            return false;
        }
    }
    long long value = 0;
    for (; i < s.size(); ++i) {
        if (s[i] < '0' || s[i] > '9') {
            return false;
        }
        value = value * 10 + (s[i] - '0');  // fine for the ranges we handle
    }
    out = negative ? -value : value;
    return true;
}

// Parses optional trailing SET options: [EX seconds | PX milliseconds].
// Returns false (and sets `err`) on a malformed option.
bool parseSetTtl(const std::vector<std::string>& args, size_t start,
                 std::optional<std::chrono::milliseconds>& ttl, std::string& err) {
    size_t i = start;
    while (i < args.size()) {
        const std::string opt = toUpper(args[i]);
        if (opt == "EX" || opt == "PX") {
            if (i + 1 >= args.size()) {
                err = resp::encodeError("ERR syntax error");
                return false;
            }
            long long n = 0;
            if (!parseLongLong(args[i + 1], n) || n <= 0) {
                err = resp::encodeError(
                    "ERR invalid expire time in 'set' command");
                return false;
            }
            ttl = (opt == "EX") ? std::chrono::milliseconds(n * 1000)
                                : std::chrono::milliseconds(n);
            i += 2;
        } else {
            err = resp::encodeError("ERR syntax error");
            return false;
        }
    }
    return true;
}

}  // namespace

std::string executeCommand(Store& store, const std::vector<std::string>& args) {
    if (args.empty()) {
        return resp::encodeError("ERR empty command");
    }

    const std::string name = toUpper(args[0]);

    if (name == "PING") {
        if (args.size() == 1) {
            return resp::encodeSimpleString("PONG");
        }
        if (args.size() == 2) {
            return resp::encodeBulkString(args[1]);
        }
        return wrongArgs("ping");
    }

    if (name == "ECHO") {
        if (args.size() != 2) {
            return wrongArgs("echo");
        }
        return resp::encodeBulkString(args[1]);
    }

    if (name == "SET") {
        // SET key value [EX seconds | PX milliseconds]
        if (args.size() < 3) {
            return wrongArgs("set");
        }
        std::optional<std::chrono::milliseconds> ttl;
        std::string err;
        if (!parseSetTtl(args, 3, ttl, err)) {
            return err;
        }
        store.set(args[1], args[2], ttl);
        return resp::encodeSimpleString("OK");
    }

    if (name == "GET") {
        if (args.size() != 2) {
            return wrongArgs("get");
        }
        auto value = store.get(args[1]);
        if (!value) {
            return resp::encodeNil();
        }
        return resp::encodeBulkString(*value);
    }

    if (name == "DEL") {
        if (args.size() < 2) {
            return wrongArgs("del");
        }
        std::vector<std::string> keys(args.begin() + 1, args.end());
        return resp::encodeInteger(store.del(keys));
    }

    if (name == "EXISTS") {
        if (args.size() < 2) {
            return wrongArgs("exists");
        }
        std::vector<std::string> keys(args.begin() + 1, args.end());
        return resp::encodeInteger(store.exists(keys));
    }

    // EXPIRE key seconds  /  PEXPIRE key milliseconds
    if (name == "EXPIRE" || name == "PEXPIRE") {
        if (args.size() != 3) {
            return wrongArgs(name == "EXPIRE" ? "expire" : "pexpire");
        }
        long long n = 0;
        if (!parseLongLong(args[2], n)) {
            return resp::encodeError("ERR value is not an integer or out of range");
        }
        auto ttl = (name == "EXPIRE") ? std::chrono::milliseconds(n * 1000)
                                      : std::chrono::milliseconds(n);
        return resp::encodeInteger(store.expire(args[1], ttl) ? 1 : 0);
    }

    // TTL key -> seconds remaining;  PTTL key -> milliseconds remaining.
    if (name == "TTL" || name == "PTTL") {
        if (args.size() != 2) {
            return wrongArgs(name == "TTL" ? "ttl" : "pttl");
        }
        long long ms = store.pttlMillis(args[1]);
        if (ms < 0) {
            return resp::encodeInteger(ms);  // -1 (no expiry) or -2 (no key)
        }
        if (name == "PTTL") {
            return resp::encodeInteger(ms);
        }
        return resp::encodeInteger((ms + 999) / 1000);  // round up to seconds
    }

    if (name == "PERSIST") {
        if (args.size() != 2) {
            return wrongArgs("persist");
        }
        return resp::encodeInteger(store.persist(args[1]) ? 1 : 0);
    }

    if (name == "DBSIZE") {
        return resp::encodeInteger(static_cast<long long>(store.size()));
    }

    // redis-cli sends `COMMAND DOCS` right after connecting to learn about the
    // server. We do not implement command introspection, so we reply with an
    // empty array to keep the client happy.
    if (name == "COMMAND") {
        return "*0\r\n";
    }

    return resp::encodeError("ERR unknown command '" + args[0] + "'");
}

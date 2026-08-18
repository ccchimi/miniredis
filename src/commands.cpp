#include "commands.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <optional>
#include <utility>

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

// Standard Redis error for operating on a key of the wrong type.
std::string wrongTypeErr() {
    return resp::encodeError(
        "WRONGTYPE Operation against a key holding the wrong kind of value");
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

    if (name == "TYPE") {
        if (args.size() != 2) {
            return wrongArgs("type");
        }
        return resp::encodeSimpleString(store.typeName(args[1]));
    }

    // --- Lists ---
    if (name == "LPUSH" || name == "RPUSH") {
        if (args.size() < 3) {
            return wrongArgs(name == "LPUSH" ? "lpush" : "rpush");
        }
        std::vector<std::string> values(args.begin() + 2, args.end());
        auto len = (name == "LPUSH") ? store.lpush(args[1], values)
                                     : store.rpush(args[1], values);
        if (!len) {
            return wrongTypeErr();
        }
        return resp::encodeInteger(*len);
    }

    if (name == "LLEN") {
        if (args.size() != 2) {
            return wrongArgs("llen");
        }
        auto len = store.llen(args[1]);
        if (!len) {
            return wrongTypeErr();
        }
        return resp::encodeInteger(*len);
    }

    if (name == "LRANGE") {
        if (args.size() != 4) {
            return wrongArgs("lrange");
        }
        long long start = 0, stop = 0;
        if (!parseLongLong(args[2], start) || !parseLongLong(args[3], stop)) {
            return resp::encodeError(
                "ERR value is not an integer or out of range");
        }
        auto result = store.lrange(args[1], start, stop);
        if (result.wrongType) {
            return wrongTypeErr();
        }
        return resp::encodeArray(result.items);
    }

    if (name == "LPOP" || name == "RPOP") {
        // LPOP key [count]
        if (args.size() < 2 || args.size() > 3) {
            return wrongArgs(name == "LPOP" ? "lpop" : "rpop");
        }
        bool hasCount = (args.size() == 3);
        long long count = 1;
        if (hasCount) {
            if (!parseLongLong(args[2], count) || count < 0) {
                return resp::encodeError(
                    "ERR value is out of range, must be positive");
            }
        }
        auto result = (name == "LPOP") ? store.lpop(args[1], count)
                                       : store.rpop(args[1], count);
        if (result.wrongType) {
            return wrongTypeErr();
        }
        if (!hasCount) {
            // No count: reply with a single bulk string, or nil.
            if (!result.values || result.values->empty()) {
                return resp::encodeNil();
            }
            return resp::encodeBulkString((*result.values)[0]);
        }
        // With count: reply with an array, or a nil array if the key is absent.
        if (!result.values) {
            return resp::encodeNilArray();
        }
        return resp::encodeArray(*result.values);
    }

    // --- Hashes ---
    if (name == "HSET") {
        // HSET key field value [field value ...]
        if (args.size() < 4 || (args.size() % 2) != 0) {
            return wrongArgs("hset");
        }
        std::vector<std::pair<std::string, std::string>> fieldValues;
        for (size_t i = 2; i + 1 < args.size(); i += 2) {
            fieldValues.emplace_back(args[i], args[i + 1]);
        }
        auto added = store.hset(args[1], fieldValues);
        if (!added) {
            return wrongTypeErr();
        }
        return resp::encodeInteger(*added);
    }

    if (name == "HGET") {
        if (args.size() != 3) {
            return wrongArgs("hget");
        }
        auto result = store.hget(args[1], args[2]);
        if (result.wrongType) {
            return wrongTypeErr();
        }
        if (!result.value) {
            return resp::encodeNil();
        }
        return resp::encodeBulkString(*result.value);
    }

    if (name == "HDEL") {
        if (args.size() < 3) {
            return wrongArgs("hdel");
        }
        std::vector<std::string> fields(args.begin() + 2, args.end());
        auto removed = store.hdel(args[1], fields);
        if (!removed) {
            return wrongTypeErr();
        }
        return resp::encodeInteger(*removed);
    }

    if (name == "HEXISTS") {
        if (args.size() != 3) {
            return wrongArgs("hexists");
        }
        auto r = store.hexists(args[1], args[2]);
        if (!r) {
            return wrongTypeErr();
        }
        return resp::encodeInteger(*r);
    }

    if (name == "HLEN") {
        if (args.size() != 2) {
            return wrongArgs("hlen");
        }
        auto len = store.hlen(args[1]);
        if (!len) {
            return wrongTypeErr();
        }
        return resp::encodeInteger(*len);
    }

    if (name == "HGETALL") {
        if (args.size() != 2) {
            return wrongArgs("hgetall");
        }
        auto result = store.hgetall(args[1]);
        if (result.wrongType) {
            return wrongTypeErr();
        }
        return resp::encodeArray(result.items);
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

#include "commands.hpp"

#include <algorithm>
#include <cctype>

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

}  // namespace

std::string executeCommand(Store& store, const std::vector<std::string>& args) {
    if (args.empty()) {
        return resp::encodeError("ERR empty command");
    }

    const std::string name = toUpper(args[0]);

    if (name == "PING") {
        // PING           -> +PONG
        // PING <message> -> bulk echo of <message>
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
        if (args.size() != 3) {
            return wrongArgs("set");
        }
        store.set(args[1], args[2]);
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

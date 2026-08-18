#include "commands.hpp"

#include <string>
#include <vector>

#include "framework.hpp"
#include "resp.hpp"
#include "store.hpp"

namespace {

// Convenience: run a command against a store and return the raw reply.
std::string run(Store& s, const std::vector<std::string>& args) {
    return executeCommand(s, args);
}

}  // namespace

void test_commands() {
    // --- PING / ECHO ---
    {
        Store s;
        CHECK_EQ(run(s, {"PING"}), resp::encodeSimpleString("PONG"));
        CHECK_EQ(run(s, {"ping"}), resp::encodeSimpleString("PONG"));  // case
        CHECK_EQ(run(s, {"PING", "hi"}), resp::encodeBulkString("hi"));
        CHECK_EQ(run(s, {"ECHO", "hi"}), resp::encodeBulkString("hi"));
    }

    // --- SET / GET roundtrip and nil ---
    {
        Store s;
        CHECK_EQ(run(s, {"SET", "k", "v"}), resp::encodeSimpleString("OK"));
        CHECK_EQ(run(s, {"GET", "k"}), resp::encodeBulkString("v"));
        CHECK_EQ(run(s, {"GET", "missing"}), resp::encodeNil());
    }

    // --- DEL / EXISTS ---
    {
        Store s;
        run(s, {"SET", "a", "1"});
        run(s, {"SET", "b", "2"});
        CHECK_EQ(run(s, {"EXISTS", "a", "b", "c"}), resp::encodeInteger(2));
        CHECK_EQ(run(s, {"DEL", "a", "c"}), resp::encodeInteger(1));
    }

    // --- SET with EX sets a TTL; TTL reads it back ---
    {
        Store s;
        CHECK_EQ(run(s, {"SET", "k", "v", "EX", "100"}),
                 resp::encodeSimpleString("OK"));
        // TTL should be 100 (or 99 if a tick elapsed); accept the exact value.
        CHECK_EQ(run(s, {"TTL", "k"}), resp::encodeInteger(100));
        CHECK_EQ(run(s, {"TTL", "missing"}), resp::encodeInteger(-2));

        run(s, {"SET", "p", "v"});
        CHECK_EQ(run(s, {"TTL", "p"}), resp::encodeInteger(-1));
        CHECK_EQ(run(s, {"PERSIST", "k"}), resp::encodeInteger(1));
        CHECK_EQ(run(s, {"TTL", "k"}), resp::encodeInteger(-1));
    }

    // --- error handling ---
    {
        Store s;
        // wrong arity
        CHECK(run(s, {"SET", "k"}).rfind("-ERR", 0) == 0);
        CHECK(run(s, {"GET"}).rfind("-ERR", 0) == 0);
        // unknown command
        CHECK(run(s, {"FLARGLE"}).rfind("-ERR", 0) == 0);
        // bad SET option
        CHECK(run(s, {"SET", "k", "v", "EX", "notanumber"}).rfind("-ERR", 0) ==
              0);
        // bad expire time
        CHECK(run(s, {"SET", "k", "v", "EX", "0"}).rfind("-ERR", 0) == 0);
    }
}

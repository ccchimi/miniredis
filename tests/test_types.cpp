#include <string>
#include <vector>

#include "commands.hpp"
#include "framework.hpp"
#include "resp.hpp"
#include "store.hpp"

namespace {

std::string run(Store& s, const std::vector<std::string>& args) {
    return executeCommand(s, args);
}

bool isWrongType(const std::string& reply) {
    return reply.rfind("-WRONGTYPE", 0) == 0;
}

}  // namespace

void test_types() {
    // --- lists ---
    {
        Store s;
        CHECK_EQ(run(s, {"RPUSH", "L", "a", "b", "c"}), resp::encodeInteger(3));
        CHECK_EQ(run(s, {"LLEN", "L"}), resp::encodeInteger(3));
        CHECK_EQ(run(s, {"LRANGE", "L", "0", "-1"}),
                 resp::encodeArray({"a", "b", "c"}));

        // LPUSH prepends, so the last pushed ends up first.
        CHECK_EQ(run(s, {"LPUSH", "L", "z"}), resp::encodeInteger(4));
        CHECK_EQ(run(s, {"LRANGE", "L", "0", "0"}), resp::encodeArray({"z"}));

        // pops from each end
        CHECK_EQ(run(s, {"LPOP", "L"}), resp::encodeBulkString("z"));
        CHECK_EQ(run(s, {"RPOP", "L"}), resp::encodeBulkString("c"));
        CHECK_EQ(run(s, {"LRANGE", "L", "0", "-1"}),
                 resp::encodeArray({"a", "b"}));

        // LPOP with a count returns an array
        CHECK_EQ(run(s, {"LPOP", "L", "5"}), resp::encodeArray({"a", "b"}));
        // list is now empty and the key is gone
        CHECK_EQ(run(s, {"TYPE", "L"}), resp::encodeSimpleString("none"));
        CHECK_EQ(run(s, {"LPOP", "L"}), resp::encodeNil());
    }

    // --- hashes ---
    {
        Store s;
        CHECK_EQ(run(s, {"HSET", "H", "f1", "v1", "f2", "v2"}),
                 resp::encodeInteger(2));
        CHECK_EQ(run(s, {"HGET", "H", "f1"}), resp::encodeBulkString("v1"));
        CHECK_EQ(run(s, {"HLEN", "H"}), resp::encodeInteger(2));
        CHECK_EQ(run(s, {"HEXISTS", "H", "f1"}), resp::encodeInteger(1));
        CHECK_EQ(run(s, {"HEXISTS", "H", "nope"}), resp::encodeInteger(0));

        // updating an existing field adds 0 new fields
        CHECK_EQ(run(s, {"HSET", "H", "f1", "v1b"}), resp::encodeInteger(0));
        CHECK_EQ(run(s, {"HGET", "H", "f1"}), resp::encodeBulkString("v1b"));

        CHECK_EQ(run(s, {"HDEL", "H", "f1"}), resp::encodeInteger(1));
        CHECK_EQ(run(s, {"HGET", "H", "f1"}), resp::encodeNil());
        CHECK_EQ(run(s, {"HLEN", "H"}), resp::encodeInteger(1));

        // HGETALL order is unspecified for a hash; check it has the one pair.
        std::string all = run(s, {"HGETALL", "H"});
        CHECK(all.rfind("*2\r\n", 0) == 0);
        CHECK(all.find("f2") != std::string::npos);
        CHECK(all.find("v2") != std::string::npos);
    }

    // --- TYPE ---
    {
        Store s;
        run(s, {"SET", "str", "x"});
        run(s, {"RPUSH", "lst", "x"});
        run(s, {"HSET", "hsh", "f", "v"});
        CHECK_EQ(run(s, {"TYPE", "str"}), resp::encodeSimpleString("string"));
        CHECK_EQ(run(s, {"TYPE", "lst"}), resp::encodeSimpleString("list"));
        CHECK_EQ(run(s, {"TYPE", "hsh"}), resp::encodeSimpleString("hash"));
        CHECK_EQ(run(s, {"TYPE", "missing"}),
                 resp::encodeSimpleString("none"));
    }

    // --- WRONGTYPE: mixing types is rejected ---
    {
        Store s;
        run(s, {"SET", "str", "x"});
        run(s, {"RPUSH", "lst", "x"});
        CHECK(isWrongType(run(s, {"LPUSH", "str", "y"})));   // list op on string
        CHECK(isWrongType(run(s, {"HGET", "lst", "f"})));    // hash op on list
        CHECK(isWrongType(run(s, {"LLEN", "str"})));
        // GET on a non-string returns nil (a deliberate simplification)
        CHECK_EQ(run(s, {"GET", "lst"}), resp::encodeNil());
    }

    // --- sorted sets ---
    {
        Store s;
        CHECK_EQ(run(s, {"ZADD", "lb", "100", "alice", "90", "bob", "110",
                         "carol"}),
                 resp::encodeInteger(3));
        CHECK_EQ(run(s, {"ZCARD", "lb"}), resp::encodeInteger(3));
        CHECK_EQ(run(s, {"ZSCORE", "lb", "alice"}),
                 resp::encodeBulkString("100"));
        CHECK_EQ(run(s, {"ZSCORE", "lb", "missing"}), resp::encodeNil());

        // updating an existing member returns 0 new and moves its rank
        CHECK_EQ(run(s, {"ZADD", "lb", "95", "bob"}), resp::encodeInteger(0));
        CHECK_EQ(run(s, {"ZSCORE", "lb", "bob"}), resp::encodeBulkString("95"));

        // ascending order is now bob(95), alice(100), carol(110)
        CHECK_EQ(run(s, {"ZRANK", "lb", "bob"}), resp::encodeInteger(0));
        CHECK_EQ(run(s, {"ZRANK", "lb", "carol"}), resp::encodeInteger(2));
        CHECK_EQ(run(s, {"ZRANGE", "lb", "0", "-1"}),
                 resp::encodeArray({"bob", "alice", "carol"}));
        CHECK_EQ(run(s, {"ZRANGE", "lb", "0", "-1", "WITHSCORES"}),
                 resp::encodeArray(
                     {"bob", "95", "alice", "100", "carol", "110"}));

        // fractional scores format cleanly
        CHECK_EQ(run(s, {"ZADD", "lb", "3.5", "dave"}), resp::encodeInteger(1));
        CHECK_EQ(run(s, {"ZSCORE", "lb", "dave"}),
                 resp::encodeBulkString("3.5"));

        CHECK_EQ(run(s, {"ZREM", "lb", "bob"}), resp::encodeInteger(1));
        CHECK_EQ(run(s, {"ZCARD", "lb"}), resp::encodeInteger(3));
        CHECK_EQ(run(s, {"TYPE", "lb"}), resp::encodeSimpleString("zset"));

        // wrong type
        run(s, {"SET", "str", "x"});
        CHECK(isWrongType(run(s, {"ZADD", "str", "1", "m"})));
    }
}

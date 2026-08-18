#include "store.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

#include "framework.hpp"

using namespace std::chrono_literals;

void test_persistence() {
    namespace fs = std::filesystem;
    const std::string path = "test_snapshot.mrdb";
    fs::remove(path);

    // --- roundtrip: values and TTLs survive save/load ---
    {
        Store a;
        a.set("plain", "value");
        a.set("withttl", "temp", 100000ms);  // ~100s
        a.set("binary", std::string("a\0b\r\n c", 8));  // arbitrary bytes
        CHECK(a.saveSnapshot(path));

        Store b;
        CHECK(b.loadSnapshot(path));
        CHECK_EQ(b.get("plain").value_or(""), std::string("value"));
        CHECK_EQ(b.get("binary").value_or(""), std::string("a\0b\r\n c", 8));
        CHECK(b.pttlMillis("plain") == -1);       // no TTL
        long long ms = b.pttlMillis("withttl");
        CHECK(ms > 0 && ms <= 100000);            // TTL preserved
        CHECK(b.size() == 3);
    }

    // --- expired keys are not persisted ---
    {
        Store a;
        a.set("live", "v");
        a.set("dying", "v", 5ms);
        std::this_thread::sleep_for(30ms);
        CHECK(a.saveSnapshot(path));

        Store b;
        CHECK(b.loadSnapshot(path));
        CHECK(b.get("live").has_value());
        CHECK(!b.get("dying").has_value());
        CHECK(b.size() == 1);
    }

    // --- lists and hashes survive a save/load roundtrip ---
    {
        Store a;
        a.set("s", "text");
        a.rpush("l", {"x", "y", "z"});
        a.hset("h", {{"f1", "v1"}, {"f2", "v2"}});
        CHECK(a.saveSnapshot(path));

        Store b;
        CHECK(b.loadSnapshot(path));
        CHECK_EQ(b.typeName("s"), std::string("string"));
        CHECK_EQ(b.typeName("l"), std::string("list"));
        CHECK_EQ(b.typeName("h"), std::string("hash"));

        auto range = b.lrange("l", 0, -1);
        CHECK(!range.wrongType);
        CHECK(range.items.size() == 3);
        CHECK(range.items[0] == "x" && range.items[2] == "z");

        auto hg = b.hget("h", "f2");
        CHECK_EQ(hg.value.value_or(""), std::string("v2"));
        CHECK(b.hlen("h").value_or(-1) == 2);
    }

    // --- sorted sets survive a roundtrip, order intact ---
    {
        Store a;
        a.zadd("z", {{100.0, "alice"}, {90.5, "bob"}, {110.0, "carol"}});
        CHECK(a.saveSnapshot(path));

        Store b;
        CHECK(b.loadSnapshot(path));
        CHECK_EQ(b.typeName("z"), std::string("zset"));
        CHECK(b.zcard("z").value_or(-1) == 3);
        auto sc = b.zscore("z", "bob");
        CHECK(!sc.wrongType && sc.score.has_value());
        CHECK(sc.score.value_or(0) == 90.5);
        // ascending by score: bob(90.5), alice(100), carol(110)
        auto range = b.zrange("z", 0, -1);
        CHECK(range.items.size() == 3);
        CHECK(range.items[0].first == "bob");
        CHECK(range.items[2].first == "carol");
    }

    // --- loading a missing file yields an empty store, not an error ---
    {
        fs::remove(path);
        Store b;
        CHECK(b.loadSnapshot(path));  // true = ok
        CHECK(b.size() == 0);
    }

    // --- a corrupt file is reported as an error ---
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << "not a real snapshot";
        out.close();
        Store b;
        CHECK(b.loadSnapshot(path) == false);
    }

    // --- dirty tracking: set marks dirty, save/consume clears it ---
    {
        Store a;
        CHECK(a.consumeDirty() == false);   // fresh store, nothing changed
        a.set("k", "v");
        CHECK(a.consumeDirty() == true);    // the set dirtied it
        CHECK(a.consumeDirty() == false);   // consumed
    }

    fs::remove(path);
}

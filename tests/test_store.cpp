#include "store.hpp"

#include <chrono>
#include <thread>

#include "framework.hpp"

using namespace std::chrono_literals;

void test_store() {
    // --- basic set / get / overwrite ---
    {
        Store s;
        CHECK(!s.get("missing").has_value());

        s.set("name", "Ada");
        auto v = s.get("name");
        CHECK(v.has_value());
        CHECK_EQ(v.value_or(""), std::string("Ada"));

        s.set("name", "Grace");  // overwrite
        CHECK_EQ(s.get("name").value_or(""), std::string("Grace"));
    }

    // --- del / exists counting ---
    {
        Store s;
        s.set("a", "1");
        s.set("b", "2");
        CHECK(s.exists({"a", "b", "c"}) == 2);
        CHECK(s.del({"a", "c"}) == 1);   // only "a" existed
        CHECK(s.exists({"a"}) == 0);
        CHECK(s.size() == 1);            // "b" remains
    }

    // --- TTL: set with expiry, pttl conventions ---
    {
        Store s;
        s.set("perm", "v");
        CHECK(s.pttlMillis("perm") == -1);       // exists, no expiry
        CHECK(s.pttlMillis("nope") == -2);       // missing

        s.set("temp", "v", 10000ms);
        long long ms = s.pttlMillis("temp");
        CHECK(ms > 0 && ms <= 10000);
    }

    // --- expire / persist ---
    {
        Store s;
        s.set("k", "v");
        CHECK(s.expire("k", 5000ms) == true);
        CHECK(s.pttlMillis("k") > 0);
        CHECK(s.expire("missing", 5000ms) == false);

        CHECK(s.persist("k") == true);           // had a TTL -> removed
        CHECK(s.pttlMillis("k") == -1);
        CHECK(s.persist("k") == false);          // nothing left to remove
    }

    // --- passive + active expiration (short, sleep-based) ---
    {
        Store s;
        s.set("gone", "v", 5ms);
        std::this_thread::sleep_for(30ms);
        CHECK(!s.get("gone").has_value());       // passive: gone on access

        s.set("gone2", "v", 5ms);
        std::this_thread::sleep_for(30ms);
        CHECK(s.purgeExpired() >= 1);            // active: swept
        CHECK(s.size() == 0);
    }

    // --- SET clears an existing TTL ---
    {
        Store s;
        s.set("k", "v", 5000ms);
        CHECK(s.pttlMillis("k") > 0);
        s.set("k", "v2");                        // plain SET drops the TTL
        CHECK(s.pttlMillis("k") == -1);
    }
}

#include "resp.hpp"

#include <string>
#include <vector>

#include "framework.hpp"

void test_resp() {
    // --- parse a complete command ---
    {
        std::string buf = "*3\r\n$3\r\nSET\r\n$4\r\nname\r\n$3\r\nAda\r\n";
        size_t pos = 0;
        std::vector<std::string> args;
        auto status = resp::parseCommand(buf, pos, args);
        CHECK(status == resp::ParseStatus::Ok);
        CHECK(args.size() == 3);
        CHECK_EQ(args[0], std::string("SET"));
        CHECK_EQ(args[1], std::string("name"));
        CHECK_EQ(args[2], std::string("Ada"));
        CHECK(pos == buf.size());  // consumed everything
    }

    // --- two commands back to back: parse advances pos across both ---
    {
        std::string buf =
            "*1\r\n$4\r\nPING\r\n*2\r\n$3\r\nGET\r\n$1\r\nx\r\n";
        size_t pos = 0;
        std::vector<std::string> a, b;
        CHECK(resp::parseCommand(buf, pos, a) == resp::ParseStatus::Ok);
        CHECK(a.size() == 1 && a[0] == "PING");
        CHECK(resp::parseCommand(buf, pos, b) == resp::ParseStatus::Ok);
        CHECK(b.size() == 2 && b[0] == "GET" && b[1] == "x");
        CHECK(pos == buf.size());
    }

    // --- incomplete input: half a command must not parse ---
    {
        std::string buf = "*2\r\n$3\r\nGET\r\n$4\r\nna";  // value truncated
        size_t pos = 0;
        std::vector<std::string> args;
        CHECK(resp::parseCommand(buf, pos, args) ==
              resp::ParseStatus::Incomplete);
        CHECK(pos == 0);  // pos untouched so caller can retry after more bytes
    }

    // --- malformed input: not a RESP array ---
    {
        std::string buf = "hello\r\n";
        size_t pos = 0;
        std::vector<std::string> args;
        CHECK(resp::parseCommand(buf, pos, args) == resp::ParseStatus::Error);
    }

    // --- encoders ---
    CHECK_EQ(resp::encodeSimpleString("OK"), std::string("+OK\r\n"));
    CHECK_EQ(resp::encodeError("ERR bad"), std::string("-ERR bad\r\n"));
    CHECK_EQ(resp::encodeInteger(42), std::string(":42\r\n"));
    CHECK_EQ(resp::encodeInteger(-2), std::string(":-2\r\n"));
    CHECK_EQ(resp::encodeBulkString("hola"), std::string("$4\r\nhola\r\n"));
    CHECK_EQ(resp::encodeBulkString(""), std::string("$0\r\n\r\n"));
    CHECK_EQ(resp::encodeNil(), std::string("$-1\r\n"));
}

#include "resp.hpp"

namespace resp {
namespace {

// Finds the next CRLF ("\r\n") at or after `from`. Returns its index, or
// std::string::npos if no complete line terminator is present yet.
size_t findCRLF(const std::string& buf, size_t from) {
    for (size_t i = from; i + 1 < buf.size(); ++i) {
        if (buf[i] == '\r' && buf[i + 1] == '\n') {
            return i;
        }
    }
    return std::string::npos;
}

// Parses a base-10 integer out of buf[start, end). Returns false on any
// non-digit (an optional leading '-' is allowed).
bool parseInt(const std::string& buf, size_t start, size_t end, long long& out) {
    if (start >= end) {
        return false;
    }
    bool negative = false;
    size_t i = start;
    if (buf[i] == '-') {
        negative = true;
        ++i;
        if (i >= end) {
            return false;
        }
    }
    long long value = 0;
    for (; i < end; ++i) {
        char c = buf[i];
        if (c < '0' || c > '9') {
            return false;
        }
        value = value * 10 + (c - '0');
    }
    out = negative ? -value : value;
    return true;
}

}  // namespace

ParseStatus parseCommand(const std::string& buf, size_t& pos,
                         std::vector<std::string>& out) {
    size_t p = pos;
    if (p >= buf.size()) {
        return ParseStatus::Incomplete;
    }

    // A well-formed client command is always a RESP array of bulk strings.
    if (buf[p] != '*') {
        return ParseStatus::Error;
    }

    size_t lineEnd = findCRLF(buf, p);
    if (lineEnd == std::string::npos) {
        return ParseStatus::Incomplete;
    }

    long long numElements = 0;
    if (!parseInt(buf, p + 1, lineEnd, numElements) || numElements < 0) {
        return ParseStatus::Error;
    }

    std::vector<std::string> parsed;
    parsed.reserve(static_cast<size_t>(numElements));
    p = lineEnd + 2;  // skip past "\r\n"

    for (long long e = 0; e < numElements; ++e) {
        if (p >= buf.size()) {
            return ParseStatus::Incomplete;
        }
        if (buf[p] != '$') {
            return ParseStatus::Error;
        }

        size_t headEnd = findCRLF(buf, p);
        if (headEnd == std::string::npos) {
            return ParseStatus::Incomplete;
        }

        long long len = 0;
        if (!parseInt(buf, p + 1, headEnd, len) || len < 0) {
            return ParseStatus::Error;
        }

        size_t dataStart = headEnd + 2;
        size_t dataEnd = dataStart + static_cast<size_t>(len);
        // Need the data plus its trailing CRLF.
        if (dataEnd + 2 > buf.size()) {
            return ParseStatus::Incomplete;
        }
        if (buf[dataEnd] != '\r' || buf[dataEnd + 1] != '\n') {
            return ParseStatus::Error;
        }

        parsed.emplace_back(buf, dataStart, static_cast<size_t>(len));
        p = dataEnd + 2;
    }

    out = std::move(parsed);
    pos = p;
    return ParseStatus::Ok;
}

std::string encodeSimpleString(const std::string& s) {
    return "+" + s + "\r\n";
}

std::string encodeError(const std::string& msg) {
    return "-" + msg + "\r\n";
}

std::string encodeInteger(long long n) {
    return ":" + std::to_string(n) + "\r\n";
}

std::string encodeBulkString(const std::string& s) {
    return "$" + std::to_string(s.size()) + "\r\n" + s + "\r\n";
}

std::string encodeNil() {
    return "$-1\r\n";
}

std::string encodeNilArray() {
    return "*-1\r\n";
}

std::string encodeArray(const std::vector<std::string>& items) {
    std::string out = "*" + std::to_string(items.size()) + "\r\n";
    for (const auto& item : items) {
        out += encodeBulkString(item);
    }
    return out;
}

}  // namespace resp

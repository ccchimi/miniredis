#pragma once

#include <string>
#include <vector>

// RESP: the Redis Serialization Protocol (RESP2).
//
// A client sends every command as an array of "bulk strings". For example
// `SET name Ada` arrives on the wire as:
//
//     *3\r\n$3\r\nSET\r\n$4\r\nname\r\n$3\r\nAda\r\n
//
// where `*3` means "array of 3 elements" and each `$N` announces a string of
// N bytes followed by those bytes. We reply with one of a few typed forms:
// simple strings (+OK), errors (-ERR ...), integers (:5), bulk strings, or a
// nil bulk string for "no value".
//
// Because TCP is a byte stream, a single recv() may deliver half a command or
// several commands at once. parseCommand() is therefore incremental: it tells
// the caller whether a full command is available yet.
namespace resp {

enum class ParseStatus {
    Ok,          // A full command was parsed; `pos` was advanced past it.
    Incomplete,  // Not enough bytes yet; the caller should read more and retry.
    Error,       // The bytes are not valid RESP; the connection should close.
};

// Attempts to parse one command starting at `buf[pos]`.
// On Ok, `out` holds the command and its arguments and `pos` is advanced to
// the first unparsed byte. On Incomplete/Error, `pos` and `out` are unchanged.
ParseStatus parseCommand(const std::string& buf, size_t& pos,
                         std::vector<std::string>& out);

// Reply encoders. Each returns a ready-to-send RESP fragment.
std::string encodeSimpleString(const std::string& s);  // +s\r\n
std::string encodeError(const std::string& msg);       // -msg\r\n
std::string encodeInteger(long long n);                // :n\r\n
std::string encodeBulkString(const std::string& s);    // $len\r\ns\r\n
std::string encodeNil();                                // $-1\r\n

}  // namespace resp

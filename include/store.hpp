#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

// Store: an in-memory, thread-safe data store with three value types (strings,
// lists, hashes), optional per-key expiry, and snapshot persistence.
//
// Concurrency: the keyspace is split into a fixed number of independently-locked
// shards. A key is routed to a shard by its hash, so operations on different
// keys usually take different locks and run in parallel; only keys that land in
// the same shard contend.
//
// Types: each key holds exactly one type. A command that targets the wrong type
// (e.g. LPUSH on a string) is rejected; the affected methods signal this with a
// `wrongType` flag or a nullopt result, which the command layer turns into a
// RESP WRONGTYPE error.
//
// Expiry: passive (evicted on access) plus active (purgeExpired() from a
// background thread). Durability: saveSnapshot()/loadSnapshot().
class Store {
public:
    // Result of a range/getall query. `items` is empty for a missing key.
    struct SeqResult {
        bool wrongType = false;
        std::vector<std::string> items;
    };
    // Result of a single-value lookup (e.g. HGET).
    struct ValueResult {
        bool wrongType = false;
        std::optional<std::string> value;  // nullopt = missing
    };
    // Result of a pop. `values` is nullopt when the key does not exist.
    struct PopResult {
        bool wrongType = false;
        std::optional<std::vector<std::string>> values;
    };

    // --- Strings --------------------------------------------------------
    // Sets key=value as a string, replacing any previous value or type. Any
    // existing expiry is cleared unless `ttl` is supplied.
    void set(const std::string& key, const std::string& value,
             std::optional<std::chrono::milliseconds> ttl = std::nullopt);
    // Returns the string value, or nullopt if missing, expired, or not a string.
    std::optional<std::string> get(const std::string& key);

    // --- Generic (any type) --------------------------------------------
    int del(const std::vector<std::string>& keys);
    int exists(const std::vector<std::string>& keys);
    bool expire(const std::string& key, std::chrono::milliseconds ttl);
    bool persist(const std::string& key);
    long long pttlMillis(const std::string& key);
    size_t size();
    size_t purgeExpired();
    // "string", "list", "hash", or "none" if the key is missing/expired.
    std::string typeName(const std::string& key);

    // --- Lists ----------------------------------------------------------
    // Push each value to the head (lpush) or tail (rpush). Creates the list if
    // needed. Returns the new length, or nullopt if the key holds another type.
    std::optional<long long> lpush(const std::string& key,
                                   const std::vector<std::string>& values);
    std::optional<long long> rpush(const std::string& key,
                                   const std::vector<std::string>& values);
    // List length (0 if missing); nullopt if the key holds another type.
    std::optional<long long> llen(const std::string& key);
    // Elements in [start, stop], Redis-style (negative = from the end).
    SeqResult lrange(const std::string& key, long long start, long long stop);
    // Pop up to `count` elements from the head (lpop) or tail (rpop). The key is
    // removed when its last element is popped.
    PopResult lpop(const std::string& key, long long count);
    PopResult rpop(const std::string& key, long long count);

    // --- Hashes ---------------------------------------------------------
    // Sets each field=value, creating the hash if needed. Returns the number of
    // fields that were newly added; nullopt if the key holds another type.
    std::optional<long long> hset(
        const std::string& key,
        const std::vector<std::pair<std::string, std::string>>& fieldValues);
    ValueResult hget(const std::string& key, const std::string& field);
    // Removes fields; returns how many were removed (key deleted if it empties);
    // nullopt if the key holds another type.
    std::optional<long long> hdel(const std::string& key,
                                  const std::vector<std::string>& fields);
    // 1 if the field exists, 0 if not; nullopt if the key holds another type.
    std::optional<long long> hexists(const std::string& key,
                                     const std::string& field);
    // Field count (0 if missing); nullopt if the key holds another type.
    std::optional<long long> hlen(const std::string& key);
    // Flattened [field0, value0, field1, value1, ...]; empty if missing.
    SeqResult hgetall(const std::string& key);

    // --- Persistence (snapshot / RDB-style) -----------------------------
    bool saveSnapshot(const std::string& path);
    bool loadSnapshot(const std::string& path);
    bool consumeDirty();

private:
    using ListType = std::deque<std::string>;
    using HashType = std::unordered_map<std::string, std::string>;

    struct Entry {
        std::variant<std::string, ListType, HashType> value;
        bool hasExpiry = false;
        std::chrono::steady_clock::time_point expireAt;
    };

    using Map = std::unordered_map<std::string, Entry>;

    struct Shard {
        Map data;
        std::mutex mtx;
    };

    static constexpr size_t kShardCount = 16;  // power of two

    Shard& shardFor(const std::string& key) {
        return shards_[std::hash<std::string>{}(key) & (kShardCount - 1)];
    }

    // All helpers below must be called with the owning shard's mutex held.
    bool isExpired(const Entry& e) const;
    // Looks up `key`, transparently dropping it if expired. Returns end() when
    // the key is missing or was just expired.
    Map::iterator findLive(Map& map, const std::string& key);

    std::array<Shard, kShardCount> shards_;
    std::atomic<bool> dirty_{false};
};

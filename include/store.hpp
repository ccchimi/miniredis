#pragma once

#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

// Store: an in-memory, thread-safe key-value map with optional per-key expiry
// and snapshot persistence.
//
// Every public method takes a single lock, so it is safe to call from many
// client threads at once. This is the simplest correct design; in a later
// phase we shard the map across several locks to reduce contention.
//
// Expiry is handled two ways, mirroring Redis:
//   - passively: a key found to be expired on access is deleted and reported
//     as missing;
//   - actively: a background thread periodically calls purgeExpired().
//
// Durability is provided by snapshots: saveSnapshot() writes the whole
// keyspace to a file and loadSnapshot() restores it on startup.
class Store {
public:
    // Inserts or overwrites the value for `key`. Any existing expiry is
    // cleared unless `ttl` is supplied, in which case the key expires that far
    // from now.
    void set(const std::string& key, const std::string& value,
             std::optional<std::chrono::milliseconds> ttl = std::nullopt);

    // Returns the value for `key`, or std::nullopt if it is missing or expired.
    std::optional<std::string> get(const std::string& key);

    // Removes the given keys. Returns how many actually existed (expired keys
    // count as already gone) and were removed.
    int del(const std::vector<std::string>& keys);

    // Returns how many of the given keys currently exist and are not expired.
    int exists(const std::vector<std::string>& keys);

    // Sets a TTL on an existing key. Returns true if the key existed; false if
    // it was missing or already expired.
    bool expire(const std::string& key, std::chrono::milliseconds ttl);

    // Removes any TTL from `key` so it no longer expires. Returns true only if
    // the key existed and actually had a TTL to remove.
    bool persist(const std::string& key);

    // Remaining time to live for `key`, in milliseconds:
    //   >= 0 : milliseconds left before it expires
    //     -1 : the key exists but has no expiry
    //     -2 : the key does not exist (or is already expired)
    long long pttlMillis(const std::string& key);

    // Number of keys currently stored (may briefly include expired-but-not-yet-
    // purged keys until the next purgeExpired() sweep).
    size_t size();

    // Removes every currently-expired key. Returns how many were removed.
    // Intended to be called periodically by a background thread.
    size_t purgeExpired();

    // --- Persistence (snapshot / RDB-style) -----------------------------
    // Atomically writes the whole keyspace to `path` (temp file + rename).
    // Already-expired keys are skipped; live keys record their *remaining* TTL
    // so it is preserved across a restart. Returns false on an I/O error.
    bool saveSnapshot(const std::string& path);

    // Replaces the keyspace with the contents of `path`. A missing file is
    // treated as an empty database and returns true. Returns false only if the
    // file exists but is unreadable or corrupt.
    bool loadSnapshot(const std::string& path);

    // Returns whether any key changed since the previous call, clearing the
    // flag. A background saver uses this to skip rewriting an unchanged dataset.
    bool consumeDirty();

private:
    struct Entry {
        std::string value;
        bool hasExpiry = false;
        std::chrono::steady_clock::time_point expireAt;
    };

    using Map = std::unordered_map<std::string, Entry>;

    // Both helpers must be called with mtx_ already held.
    bool isExpired(const Entry& e) const;
    // If the entry at `it` is expired, erases it and returns true (leaving `it`
    // invalidated). Otherwise returns false and leaves `it` untouched.
    bool eraseIfExpired(Map::iterator it);

    Map data_;
    std::mutex mtx_;
    std::atomic<bool> dirty_{false};
};

#pragma once

#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

// Store: an in-memory, thread-safe key-value map.
//
// Every public method takes a single lock, so it is safe to call from many
// client threads at once. This is the simplest correct design; in a later
// phase we shard the map across several locks to reduce contention.
class Store {
public:
    // Inserts or overwrites the value for `key`.
    void set(const std::string& key, const std::string& value);

    // Returns the value for `key`, or std::nullopt if it does not exist.
    std::optional<std::string> get(const std::string& key);

    // Removes the given keys. Returns how many actually existed and were removed.
    int del(const std::vector<std::string>& keys);

    // Returns how many of the given keys currently exist.
    int exists(const std::vector<std::string>& keys);

    // Number of keys currently stored.
    size_t size();

private:
    std::unordered_map<std::string, std::string> data_;
    std::mutex mtx_;
};

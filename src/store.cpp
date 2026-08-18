#include "store.hpp"

using std::chrono::steady_clock;

bool Store::isExpired(const Entry& e) const {
    return e.hasExpiry && e.expireAt <= steady_clock::now();
}

bool Store::eraseIfExpired(Map::iterator it) {
    if (it != data_.end() && isExpired(it->second)) {
        data_.erase(it);
        return true;
    }
    return false;
}

void Store::set(const std::string& key, const std::string& value,
                std::optional<std::chrono::milliseconds> ttl) {
    std::lock_guard<std::mutex> lock(mtx_);
    Entry entry;
    entry.value = value;
    if (ttl) {
        entry.hasExpiry = true;
        entry.expireAt = steady_clock::now() + *ttl;
    }
    data_[key] = std::move(entry);
}

std::optional<std::string> Store::get(const std::string& key) {
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = data_.find(key);
    if (it == data_.end()) {
        return std::nullopt;
    }
    if (eraseIfExpired(it)) {
        return std::nullopt;
    }
    return it->second.value;
}

int Store::del(const std::vector<std::string>& keys) {
    std::lock_guard<std::mutex> lock(mtx_);
    int removed = 0;
    for (const auto& key : keys) {
        auto it = data_.find(key);
        if (it == data_.end()) {
            continue;
        }
        // An already-expired key counts as "not there": drop it but do not
        // count it as a deletion.
        if (eraseIfExpired(it)) {
            continue;
        }
        data_.erase(it);
        ++removed;
    }
    return removed;
}

int Store::exists(const std::vector<std::string>& keys) {
    std::lock_guard<std::mutex> lock(mtx_);
    int count = 0;
    for (const auto& key : keys) {
        auto it = data_.find(key);
        if (it == data_.end()) {
            continue;
        }
        if (eraseIfExpired(it)) {
            continue;
        }
        ++count;
    }
    return count;
}

bool Store::expire(const std::string& key, std::chrono::milliseconds ttl) {
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = data_.find(key);
    if (it == data_.end() || eraseIfExpired(it)) {
        return false;
    }
    it->second.hasExpiry = true;
    it->second.expireAt = steady_clock::now() + ttl;
    return true;
}

bool Store::persist(const std::string& key) {
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = data_.find(key);
    if (it == data_.end() || eraseIfExpired(it)) {
        return false;
    }
    if (!it->second.hasExpiry) {
        return false;
    }
    it->second.hasExpiry = false;
    return true;
}

long long Store::pttlMillis(const std::string& key) {
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = data_.find(key);
    if (it == data_.end() || eraseIfExpired(it)) {
        return -2;
    }
    if (!it->second.hasExpiry) {
        return -1;
    }
    auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                         it->second.expireAt - steady_clock::now())
                         .count();
    return remaining < 0 ? 0 : remaining;
}

size_t Store::size() {
    std::lock_guard<std::mutex> lock(mtx_);
    return data_.size();
}

size_t Store::purgeExpired() {
    std::lock_guard<std::mutex> lock(mtx_);
    auto now = steady_clock::now();
    size_t removed = 0;
    // A full scan is O(n). Redis instead samples random keys to bound the work
    // per tick; a full sweep is simpler and fine at this scale. See ROADMAP.
    for (auto it = data_.begin(); it != data_.end();) {
        if (it->second.hasExpiry && it->second.expireAt <= now) {
            it = data_.erase(it);
            ++removed;
        } else {
            ++it;
        }
    }
    return removed;
}

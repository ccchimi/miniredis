#include "store.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <utility>

using std::chrono::steady_clock;

namespace {

// Snapshot binary format (little-endian, length-prefixed so keys and values
// may contain any bytes, including spaces and newlines):
//
//   "MINIRDS1"            8-byte magic
//   uint32   count        number of entries
//   repeated count times:
//     uint32  keyLen,  key bytes
//     uint32  valLen,  value bytes
//     int64   ttlMs    -1 for no expiry, else remaining milliseconds
constexpr char kMagic[8] = {'M', 'I', 'N', 'I', 'R', 'D', 'S', '1'};

void putU32(std::string& out, uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
    }
}

void putI64(std::string& out, int64_t v) {
    uint64_t u = static_cast<uint64_t>(v);
    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<char>((u >> (8 * i)) & 0xFF));
    }
}

bool getU32(const std::string& s, size_t& p, uint32_t& v) {
    if (p + 4 > s.size()) {
        return false;
    }
    v = 0;
    for (int i = 0; i < 4; ++i) {
        v |= static_cast<uint32_t>(static_cast<unsigned char>(s[p + i]))
             << (8 * i);
    }
    p += 4;
    return true;
}

bool getI64(const std::string& s, size_t& p, int64_t& v) {
    if (p + 8 > s.size()) {
        return false;
    }
    uint64_t u = 0;
    for (int i = 0; i < 8; ++i) {
        u |= static_cast<uint64_t>(static_cast<unsigned char>(s[p + i]))
             << (8 * i);
    }
    v = static_cast<int64_t>(u);
    p += 8;
    return true;
}

}  // namespace

bool Store::isExpired(const Entry& e) const {
    return e.hasExpiry && e.expireAt <= steady_clock::now();
}

bool Store::eraseIfExpired(Map& map, Map::iterator it) {
    if (it != map.end() && isExpired(it->second)) {
        map.erase(it);
        return true;
    }
    return false;
}

void Store::set(const std::string& key, const std::string& value,
                std::optional<std::chrono::milliseconds> ttl) {
    Shard& shard = shardFor(key);
    std::lock_guard<std::mutex> lock(shard.mtx);
    Entry entry;
    entry.value = value;
    if (ttl) {
        entry.hasExpiry = true;
        entry.expireAt = steady_clock::now() + *ttl;
    }
    shard.data[key] = std::move(entry);
    dirty_.store(true, std::memory_order_relaxed);
}

std::optional<std::string> Store::get(const std::string& key) {
    Shard& shard = shardFor(key);
    std::lock_guard<std::mutex> lock(shard.mtx);
    auto it = shard.data.find(key);
    if (it == shard.data.end()) {
        return std::nullopt;
    }
    if (eraseIfExpired(shard.data, it)) {
        return std::nullopt;
    }
    return it->second.value;
}

int Store::del(const std::vector<std::string>& keys) {
    int removed = 0;
    for (const auto& key : keys) {
        Shard& shard = shardFor(key);
        std::lock_guard<std::mutex> lock(shard.mtx);
        auto it = shard.data.find(key);
        if (it == shard.data.end()) {
            continue;
        }
        // An already-expired key counts as "not there": drop it but do not
        // count it as a deletion.
        if (eraseIfExpired(shard.data, it)) {
            continue;
        }
        shard.data.erase(it);
        ++removed;
    }
    if (removed > 0) {
        dirty_.store(true, std::memory_order_relaxed);
    }
    return removed;
}

int Store::exists(const std::vector<std::string>& keys) {
    int count = 0;
    for (const auto& key : keys) {
        Shard& shard = shardFor(key);
        std::lock_guard<std::mutex> lock(shard.mtx);
        auto it = shard.data.find(key);
        if (it == shard.data.end()) {
            continue;
        }
        if (eraseIfExpired(shard.data, it)) {
            continue;
        }
        ++count;
    }
    return count;
}

bool Store::expire(const std::string& key, std::chrono::milliseconds ttl) {
    Shard& shard = shardFor(key);
    std::lock_guard<std::mutex> lock(shard.mtx);
    auto it = shard.data.find(key);
    if (it == shard.data.end() || eraseIfExpired(shard.data, it)) {
        return false;
    }
    it->second.hasExpiry = true;
    it->second.expireAt = steady_clock::now() + ttl;
    dirty_.store(true, std::memory_order_relaxed);
    return true;
}

bool Store::persist(const std::string& key) {
    Shard& shard = shardFor(key);
    std::lock_guard<std::mutex> lock(shard.mtx);
    auto it = shard.data.find(key);
    if (it == shard.data.end() || eraseIfExpired(shard.data, it)) {
        return false;
    }
    if (!it->second.hasExpiry) {
        return false;
    }
    it->second.hasExpiry = false;
    dirty_.store(true, std::memory_order_relaxed);
    return true;
}

long long Store::pttlMillis(const std::string& key) {
    Shard& shard = shardFor(key);
    std::lock_guard<std::mutex> lock(shard.mtx);
    auto it = shard.data.find(key);
    if (it == shard.data.end() || eraseIfExpired(shard.data, it)) {
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
    size_t total = 0;
    for (auto& shard : shards_) {
        std::lock_guard<std::mutex> lock(shard.mtx);
        total += shard.data.size();
    }
    return total;
}

size_t Store::purgeExpired() {
    size_t removed = 0;
    for (auto& shard : shards_) {
        std::lock_guard<std::mutex> lock(shard.mtx);
        auto now = steady_clock::now();
        // A full scan per shard is O(n). Redis instead samples random keys to
        // bound the work per tick; a full sweep is simpler and fine at this
        // scale. See the ROADMAP note.
        for (auto it = shard.data.begin(); it != shard.data.end();) {
            if (it->second.hasExpiry && it->second.expireAt <= now) {
                it = shard.data.erase(it);
                ++removed;
            } else {
                ++it;
            }
        }
    }
    if (removed > 0) {
        dirty_.store(true, std::memory_order_relaxed);
    }
    return removed;
}

bool Store::consumeDirty() {
    return dirty_.exchange(false, std::memory_order_relaxed);
}

bool Store::saveSnapshot(const std::string& path) {
    // Build the whole snapshot into a buffer, locking one shard at a time, then
    // do the actual (slow) disk I/O with no locks held. Snapshotting shard by
    // shard keeps each key internally consistent; there is no cross-shard
    // invariant to preserve.
    std::string buf;
    buf.append(kMagic, sizeof(kMagic));
    const size_t countPos = buf.size();
    putU32(buf, 0);  // placeholder, patched once we know the live count

    uint32_t count = 0;
    for (auto& shard : shards_) {
        std::lock_guard<std::mutex> lock(shard.mtx);
        auto now = steady_clock::now();
        for (const auto& [key, entry] : shard.data) {
            int64_t ttlMs = -1;
            if (entry.hasExpiry) {
                auto rem = std::chrono::duration_cast<std::chrono::milliseconds>(
                               entry.expireAt - now)
                               .count();
                if (rem <= 0) {
                    continue;  // already expired; do not persist it
                }
                ttlMs = rem;
            }
            putU32(buf, static_cast<uint32_t>(key.size()));
            buf.append(key);
            putU32(buf, static_cast<uint32_t>(entry.value.size()));
            buf.append(entry.value);
            putI64(buf, ttlMs);
            ++count;
        }
    }
    for (int i = 0; i < 4; ++i) {
        buf[countPos + i] = static_cast<char>((count >> (8 * i)) & 0xFF);
    }

    namespace fs = std::filesystem;
    const std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            return false;
        }
        out.write(buf.data(), static_cast<std::streamsize>(buf.size()));
        if (!out) {
            return false;
        }
    }

    // Rename is atomic on POSIX. On Windows it fails if the target exists, so
    // fall back to remove-then-rename there.
    std::error_code ec;
    fs::rename(tmp, path, ec);
    if (ec) {
        fs::remove(path, ec);
        fs::rename(tmp, path, ec);
        if (ec) {
            return false;
        }
    }
    return true;
}

bool Store::loadSnapshot(const std::string& path) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::exists(path, ec)) {
        return true;  // no snapshot yet: start empty
    }

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    std::string buf((std::istreambuf_iterator<char>(in)),
                    std::istreambuf_iterator<char>());

    if (buf.size() < sizeof(kMagic) ||
        std::string(buf, 0, sizeof(kMagic)) !=
            std::string(kMagic, sizeof(kMagic))) {
        return false;  // not our format
    }

    size_t p = sizeof(kMagic);
    uint32_t count = 0;
    if (!getU32(buf, p, count)) {
        return false;
    }

    // Parse everything into a temporary list first, so a corrupt file leaves
    // the live store untouched instead of half-loaded.
    std::vector<std::pair<std::string, Entry>> loaded;
    loaded.reserve(count);
    auto now = steady_clock::now();
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t keyLen = 0;
        if (!getU32(buf, p, keyLen) || p + keyLen > buf.size()) {
            return false;
        }
        std::string key = buf.substr(p, keyLen);
        p += keyLen;

        uint32_t valLen = 0;
        if (!getU32(buf, p, valLen) || p + valLen > buf.size()) {
            return false;
        }
        std::string value = buf.substr(p, valLen);
        p += valLen;

        int64_t ttlMs = 0;
        if (!getI64(buf, p, ttlMs)) {
            return false;
        }

        Entry entry;
        entry.value = std::move(value);
        if (ttlMs >= 0) {
            entry.hasExpiry = true;
            entry.expireAt = now + std::chrono::milliseconds(ttlMs);
        }
        loaded.emplace_back(std::move(key), std::move(entry));
    }

    // Replace the keyspace: clear every shard, then route each entry home.
    for (auto& shard : shards_) {
        std::lock_guard<std::mutex> lock(shard.mtx);
        shard.data.clear();
    }
    for (auto& [key, entry] : loaded) {
        Shard& shard = shardFor(key);
        std::lock_guard<std::mutex> lock(shard.mtx);
        shard.data[key] = std::move(entry);
    }
    dirty_.store(false, std::memory_order_relaxed);
    return true;
}

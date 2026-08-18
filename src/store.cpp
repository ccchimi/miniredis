#include "store.hpp"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <utility>

using std::chrono::steady_clock;

namespace {

// Snapshot binary format v2 (little-endian, length-prefixed so all bytes are
// safe). Header: "MINIRDS2" magic, then uint32 entry count. Each entry:
//   uint32 keyLen, key bytes
//   uint8  type            0 = string, 1 = list, 2 = hash
//   payload:
//     string: uint32 len, bytes
//     list:   uint32 count, then count x (uint32 len, bytes)
//     hash:   uint32 count, then count x (uint32 fLen, field, uint32 vLen, val)
//     zset:   uint32 count, then count x (uint32 mLen, member, double score)
//   int64  ttlMs           -1 = no expiry, else remaining milliseconds
constexpr char kMagic[8] = {'M', 'I', 'N', 'I', 'R', 'D', 'S', '3'};
constexpr uint8_t kTypeString = 0;
constexpr uint8_t kTypeList = 1;
constexpr uint8_t kTypeHash = 2;
constexpr uint8_t kTypeZSet = 3;

void putU8(std::string& out, uint8_t v) { out.push_back(static_cast<char>(v)); }

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

// Appends a length-prefixed blob.
void putBlob(std::string& out, const std::string& s) {
    putU32(out, static_cast<uint32_t>(s.size()));
    out.append(s);
}

// Doubles are stored as their raw 8-byte IEEE-754 bit pattern.
void putDouble(std::string& out, double d) {
    uint64_t bits;
    std::memcpy(&bits, &d, sizeof(bits));
    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<char>((bits >> (8 * i)) & 0xFF));
    }
}

bool getU8(const std::string& s, size_t& p, uint8_t& v) {
    if (p + 1 > s.size()) {
        return false;
    }
    v = static_cast<uint8_t>(s[p]);
    p += 1;
    return true;
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

// Reads a length-prefixed blob into `out`.
bool getBlob(const std::string& s, size_t& p, std::string& out) {
    uint32_t len = 0;
    if (!getU32(s, p, len) || p + len > s.size()) {
        return false;
    }
    out.assign(s, p, len);
    p += len;
    return true;
}

bool getDouble(const std::string& s, size_t& p, double& out) {
    if (p + 8 > s.size()) {
        return false;
    }
    uint64_t bits = 0;
    for (int i = 0; i < 8; ++i) {
        bits |= static_cast<uint64_t>(static_cast<unsigned char>(s[p + i]))
                << (8 * i);
    }
    std::memcpy(&out, &bits, sizeof(out));
    p += 8;
    return true;
}

}  // namespace

bool Store::isExpired(const Entry& e) const {
    return e.hasExpiry && e.expireAt <= steady_clock::now();
}

Store::Map::iterator Store::findLive(Map& map, const std::string& key) {
    auto it = map.find(key);
    if (it != map.end() && isExpired(it->second)) {
        map.erase(it);
        return map.end();
    }
    return it;
}

// --- Strings ------------------------------------------------------------

void Store::set(const std::string& key, const std::string& value,
                std::optional<std::chrono::milliseconds> ttl) {
    Shard& shard = shardFor(key);
    std::lock_guard<std::mutex> lock(shard.mtx);
    Entry entry;
    entry.value = value;  // string alternative
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
    auto it = findLive(shard.data, key);
    if (it == shard.data.end()) {
        return std::nullopt;
    }
    if (auto* s = std::get_if<std::string>(&it->second.value)) {
        return *s;
    }
    return std::nullopt;  // wrong type: GET treats it as absent
}

// --- Generic ------------------------------------------------------------

int Store::del(const std::vector<std::string>& keys) {
    int removed = 0;
    for (const auto& key : keys) {
        Shard& shard = shardFor(key);
        std::lock_guard<std::mutex> lock(shard.mtx);
        auto it = findLive(shard.data, key);
        if (it == shard.data.end()) {
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
        if (findLive(shard.data, key) != shard.data.end()) {
            ++count;
        }
    }
    return count;
}

bool Store::expire(const std::string& key, std::chrono::milliseconds ttl) {
    Shard& shard = shardFor(key);
    std::lock_guard<std::mutex> lock(shard.mtx);
    auto it = findLive(shard.data, key);
    if (it == shard.data.end()) {
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
    auto it = findLive(shard.data, key);
    if (it == shard.data.end() || !it->second.hasExpiry) {
        return false;
    }
    it->second.hasExpiry = false;
    dirty_.store(true, std::memory_order_relaxed);
    return true;
}

long long Store::pttlMillis(const std::string& key) {
    Shard& shard = shardFor(key);
    std::lock_guard<std::mutex> lock(shard.mtx);
    auto it = findLive(shard.data, key);
    if (it == shard.data.end()) {
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

std::string Store::typeName(const std::string& key) {
    Shard& shard = shardFor(key);
    std::lock_guard<std::mutex> lock(shard.mtx);
    auto it = findLive(shard.data, key);
    if (it == shard.data.end()) {
        return "none";
    }
    if (std::holds_alternative<std::string>(it->second.value)) {
        return "string";
    }
    if (std::holds_alternative<ListType>(it->second.value)) {
        return "list";
    }
    if (std::holds_alternative<HashType>(it->second.value)) {
        return "hash";
    }
    return "zset";
}

// --- Lists --------------------------------------------------------------

namespace {

// Normalizes a possibly-negative Redis index against a length.
long long normalizeIndex(long long i, long long len) {
    if (i < 0) {
        i += len;
    }
    return i;
}

}  // namespace

std::optional<long long> Store::lpush(const std::string& key,
                                      const std::vector<std::string>& values) {
    Shard& shard = shardFor(key);
    std::lock_guard<std::mutex> lock(shard.mtx);
    auto it = findLive(shard.data, key);
    if (it == shard.data.end()) {
        it = shard.data.emplace(key, Entry{ListType{}, false, {}}).first;
    }
    auto* list = std::get_if<ListType>(&it->second.value);
    if (!list) {
        return std::nullopt;  // WRONGTYPE
    }
    for (const auto& v : values) {
        list->push_front(v);
    }
    dirty_.store(true, std::memory_order_relaxed);
    return static_cast<long long>(list->size());
}

std::optional<long long> Store::rpush(const std::string& key,
                                      const std::vector<std::string>& values) {
    Shard& shard = shardFor(key);
    std::lock_guard<std::mutex> lock(shard.mtx);
    auto it = findLive(shard.data, key);
    if (it == shard.data.end()) {
        it = shard.data.emplace(key, Entry{ListType{}, false, {}}).first;
    }
    auto* list = std::get_if<ListType>(&it->second.value);
    if (!list) {
        return std::nullopt;  // WRONGTYPE
    }
    for (const auto& v : values) {
        list->push_back(v);
    }
    dirty_.store(true, std::memory_order_relaxed);
    return static_cast<long long>(list->size());
}

std::optional<long long> Store::llen(const std::string& key) {
    Shard& shard = shardFor(key);
    std::lock_guard<std::mutex> lock(shard.mtx);
    auto it = findLive(shard.data, key);
    if (it == shard.data.end()) {
        return 0;
    }
    auto* list = std::get_if<ListType>(&it->second.value);
    if (!list) {
        return std::nullopt;  // WRONGTYPE
    }
    return static_cast<long long>(list->size());
}

Store::SeqResult Store::lrange(const std::string& key, long long start,
                               long long stop) {
    Shard& shard = shardFor(key);
    std::lock_guard<std::mutex> lock(shard.mtx);
    auto it = findLive(shard.data, key);
    if (it == shard.data.end()) {
        return {};  // empty list
    }
    auto* list = std::get_if<ListType>(&it->second.value);
    if (!list) {
        return {true, {}};  // WRONGTYPE
    }

    const long long len = static_cast<long long>(list->size());
    start = normalizeIndex(start, len);
    stop = normalizeIndex(stop, len);
    if (start < 0) {
        start = 0;
    }
    if (stop >= len) {
        stop = len - 1;
    }

    SeqResult out;
    for (long long i = start; i <= stop && i < len; ++i) {
        if (i < 0) {
            continue;
        }
        out.items.push_back((*list)[static_cast<size_t>(i)]);
    }
    return out;
}

Store::PopResult Store::lpop(const std::string& key, long long count) {
    Shard& shard = shardFor(key);
    std::lock_guard<std::mutex> lock(shard.mtx);
    auto it = findLive(shard.data, key);
    if (it == shard.data.end()) {
        return {};  // key missing -> values stays nullopt
    }
    auto* list = std::get_if<ListType>(&it->second.value);
    if (!list) {
        return {true, std::nullopt};  // WRONGTYPE
    }
    std::vector<std::string> popped;
    long long n = std::min<long long>(count, static_cast<long long>(list->size()));
    for (long long i = 0; i < n; ++i) {
        popped.push_back(std::move(list->front()));
        list->pop_front();
    }
    if (list->empty()) {
        shard.data.erase(it);
    }
    dirty_.store(true, std::memory_order_relaxed);
    return {false, std::move(popped)};
}

Store::PopResult Store::rpop(const std::string& key, long long count) {
    Shard& shard = shardFor(key);
    std::lock_guard<std::mutex> lock(shard.mtx);
    auto it = findLive(shard.data, key);
    if (it == shard.data.end()) {
        return {};
    }
    auto* list = std::get_if<ListType>(&it->second.value);
    if (!list) {
        return {true, std::nullopt};  // WRONGTYPE
    }
    std::vector<std::string> popped;
    long long n = std::min<long long>(count, static_cast<long long>(list->size()));
    for (long long i = 0; i < n; ++i) {
        popped.push_back(std::move(list->back()));
        list->pop_back();
    }
    if (list->empty()) {
        shard.data.erase(it);
    }
    dirty_.store(true, std::memory_order_relaxed);
    return {false, std::move(popped)};
}

// --- Hashes -------------------------------------------------------------

std::optional<long long> Store::hset(
    const std::string& key,
    const std::vector<std::pair<std::string, std::string>>& fieldValues) {
    Shard& shard = shardFor(key);
    std::lock_guard<std::mutex> lock(shard.mtx);
    auto it = findLive(shard.data, key);
    if (it == shard.data.end()) {
        it = shard.data.emplace(key, Entry{HashType{}, false, {}}).first;
    }
    auto* hash = std::get_if<HashType>(&it->second.value);
    if (!hash) {
        return std::nullopt;  // WRONGTYPE
    }
    long long added = 0;
    for (const auto& [field, value] : fieldValues) {
        auto [pos, inserted] = hash->insert_or_assign(field, value);
        (void)pos;
        if (inserted) {
            ++added;
        }
    }
    dirty_.store(true, std::memory_order_relaxed);
    return added;
}

Store::ValueResult Store::hget(const std::string& key,
                               const std::string& field) {
    Shard& shard = shardFor(key);
    std::lock_guard<std::mutex> lock(shard.mtx);
    auto it = findLive(shard.data, key);
    if (it == shard.data.end()) {
        return {};  // missing
    }
    auto* hash = std::get_if<HashType>(&it->second.value);
    if (!hash) {
        return {true, std::nullopt};  // WRONGTYPE
    }
    auto f = hash->find(field);
    if (f == hash->end()) {
        return {};
    }
    return {false, f->second};
}

std::optional<long long> Store::hdel(const std::string& key,
                                     const std::vector<std::string>& fields) {
    Shard& shard = shardFor(key);
    std::lock_guard<std::mutex> lock(shard.mtx);
    auto it = findLive(shard.data, key);
    if (it == shard.data.end()) {
        return 0;
    }
    auto* hash = std::get_if<HashType>(&it->second.value);
    if (!hash) {
        return std::nullopt;  // WRONGTYPE
    }
    long long removed = 0;
    for (const auto& field : fields) {
        removed += static_cast<long long>(hash->erase(field));
    }
    if (hash->empty()) {
        shard.data.erase(it);
    }
    if (removed > 0) {
        dirty_.store(true, std::memory_order_relaxed);
    }
    return removed;
}

std::optional<long long> Store::hexists(const std::string& key,
                                        const std::string& field) {
    Shard& shard = shardFor(key);
    std::lock_guard<std::mutex> lock(shard.mtx);
    auto it = findLive(shard.data, key);
    if (it == shard.data.end()) {
        return 0;
    }
    auto* hash = std::get_if<HashType>(&it->second.value);
    if (!hash) {
        return std::nullopt;  // WRONGTYPE
    }
    return hash->count(field) ? 1 : 0;
}

std::optional<long long> Store::hlen(const std::string& key) {
    Shard& shard = shardFor(key);
    std::lock_guard<std::mutex> lock(shard.mtx);
    auto it = findLive(shard.data, key);
    if (it == shard.data.end()) {
        return 0;
    }
    auto* hash = std::get_if<HashType>(&it->second.value);
    if (!hash) {
        return std::nullopt;  // WRONGTYPE
    }
    return static_cast<long long>(hash->size());
}

Store::SeqResult Store::hgetall(const std::string& key) {
    Shard& shard = shardFor(key);
    std::lock_guard<std::mutex> lock(shard.mtx);
    auto it = findLive(shard.data, key);
    if (it == shard.data.end()) {
        return {};
    }
    auto* hash = std::get_if<HashType>(&it->second.value);
    if (!hash) {
        return {true, {}};  // WRONGTYPE
    }
    SeqResult out;
    out.items.reserve(hash->size() * 2);
    for (const auto& [field, value] : *hash) {
        out.items.push_back(field);
        out.items.push_back(value);
    }
    return out;
}

// --- Sorted sets --------------------------------------------------------

std::optional<long long> Store::zadd(
    const std::string& key,
    const std::vector<std::pair<double, std::string>>& members) {
    Shard& shard = shardFor(key);
    std::lock_guard<std::mutex> lock(shard.mtx);
    auto it = findLive(shard.data, key);
    if (it == shard.data.end()) {
        it = shard.data.emplace(key, Entry{ZSetType{}, false, {}}).first;
    }
    auto* zset = std::get_if<ZSetType>(&it->second.value);
    if (!zset) {
        return std::nullopt;  // WRONGTYPE
    }
    long long added = 0;
    for (const auto& [score, member] : members) {
        auto found = zset->scores.find(member);
        if (found != zset->scores.end()) {
            // Update: move the member to its new position in the ordered index.
            zset->sorted.erase({found->second, member});
            found->second = score;
            zset->sorted.insert({score, member});
        } else {
            zset->scores.emplace(member, score);
            zset->sorted.insert({score, member});
            ++added;
        }
    }
    dirty_.store(true, std::memory_order_relaxed);
    return added;
}

Store::ScoreResult Store::zscore(const std::string& key,
                                 const std::string& member) {
    Shard& shard = shardFor(key);
    std::lock_guard<std::mutex> lock(shard.mtx);
    auto it = findLive(shard.data, key);
    if (it == shard.data.end()) {
        return {};
    }
    auto* zset = std::get_if<ZSetType>(&it->second.value);
    if (!zset) {
        return {true, std::nullopt};  // WRONGTYPE
    }
    auto found = zset->scores.find(member);
    if (found == zset->scores.end()) {
        return {};
    }
    return {false, found->second};
}

std::optional<long long> Store::zcard(const std::string& key) {
    Shard& shard = shardFor(key);
    std::lock_guard<std::mutex> lock(shard.mtx);
    auto it = findLive(shard.data, key);
    if (it == shard.data.end()) {
        return 0;
    }
    auto* zset = std::get_if<ZSetType>(&it->second.value);
    if (!zset) {
        return std::nullopt;  // WRONGTYPE
    }
    return static_cast<long long>(zset->scores.size());
}

std::optional<long long> Store::zrem(const std::string& key,
                                     const std::vector<std::string>& members) {
    Shard& shard = shardFor(key);
    std::lock_guard<std::mutex> lock(shard.mtx);
    auto it = findLive(shard.data, key);
    if (it == shard.data.end()) {
        return 0;
    }
    auto* zset = std::get_if<ZSetType>(&it->second.value);
    if (!zset) {
        return std::nullopt;  // WRONGTYPE
    }
    long long removed = 0;
    for (const auto& member : members) {
        auto found = zset->scores.find(member);
        if (found != zset->scores.end()) {
            zset->sorted.erase({found->second, member});
            zset->scores.erase(found);
            ++removed;
        }
    }
    if (zset->scores.empty()) {
        shard.data.erase(it);
    }
    if (removed > 0) {
        dirty_.store(true, std::memory_order_relaxed);
    }
    return removed;
}

Store::RankResult Store::zrank(const std::string& key,
                               const std::string& member) {
    Shard& shard = shardFor(key);
    std::lock_guard<std::mutex> lock(shard.mtx);
    auto it = findLive(shard.data, key);
    if (it == shard.data.end()) {
        return {};
    }
    auto* zset = std::get_if<ZSetType>(&it->second.value);
    if (!zset) {
        return {true, std::nullopt};  // WRONGTYPE
    }
    auto found = zset->scores.find(member);
    if (found == zset->scores.end()) {
        return {};
    }
    // Rank = number of members ordered before (score, member).
    auto pos = zset->sorted.find({found->second, member});
    long long rank = std::distance(zset->sorted.begin(), pos);
    return {false, rank};
}

Store::RangeResult Store::zrange(const std::string& key, long long start,
                                 long long stop) {
    Shard& shard = shardFor(key);
    std::lock_guard<std::mutex> lock(shard.mtx);
    auto it = findLive(shard.data, key);
    if (it == shard.data.end()) {
        return {};
    }
    auto* zset = std::get_if<ZSetType>(&it->second.value);
    if (!zset) {
        return {true, {}};  // WRONGTYPE
    }

    const long long len = static_cast<long long>(zset->sorted.size());
    if (start < 0) {
        start += len;
    }
    if (stop < 0) {
        stop += len;
    }
    if (start < 0) {
        start = 0;
    }
    if (stop >= len) {
        stop = len - 1;
    }

    RangeResult out;
    if (start > stop) {
        return out;
    }
    // std::set iterates in ascending (score, member) order.
    long long i = 0;
    for (const auto& [score, member] : zset->sorted) {
        if (i > stop) {
            break;
        }
        if (i >= start) {
            out.items.emplace_back(member, score);
        }
        ++i;
    }
    return out;
}

// --- Persistence --------------------------------------------------------

bool Store::consumeDirty() {
    return dirty_.exchange(false, std::memory_order_relaxed);
}

bool Store::saveSnapshot(const std::string& path) {
    std::string buf;
    buf.append(kMagic, sizeof(kMagic));
    const size_t countPos = buf.size();
    putU32(buf, 0);  // placeholder

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
                    continue;  // already expired
                }
                ttlMs = rem;
            }

            putU32(buf, static_cast<uint32_t>(key.size()));
            buf.append(key);

            if (const auto* s = std::get_if<std::string>(&entry.value)) {
                putU8(buf, kTypeString);
                putBlob(buf, *s);
            } else if (const auto* l = std::get_if<ListType>(&entry.value)) {
                putU8(buf, kTypeList);
                putU32(buf, static_cast<uint32_t>(l->size()));
                for (const auto& item : *l) {
                    putBlob(buf, item);
                }
            } else if (const auto* h = std::get_if<HashType>(&entry.value)) {
                putU8(buf, kTypeHash);
                putU32(buf, static_cast<uint32_t>(h->size()));
                for (const auto& [field, value] : *h) {
                    putBlob(buf, field);
                    putBlob(buf, value);
                }
            } else {
                const auto& z = std::get<ZSetType>(entry.value);
                putU8(buf, kTypeZSet);
                putU32(buf, static_cast<uint32_t>(z.sorted.size()));
                for (const auto& [score, member] : z.sorted) {
                    putBlob(buf, member);
                    putDouble(buf, score);
                }
            }

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
        return true;  // no snapshot yet
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
        return false;
    }

    size_t p = sizeof(kMagic);
    uint32_t count = 0;
    if (!getU32(buf, p, count)) {
        return false;
    }

    // Parse into a temporary list first so a corrupt file leaves the live
    // store untouched instead of half-loaded.
    std::vector<std::pair<std::string, Entry>> loaded;
    loaded.reserve(count);
    auto now = steady_clock::now();
    for (uint32_t i = 0; i < count; ++i) {
        std::string key;
        if (!getBlob(buf, p, key)) {
            return false;
        }
        uint8_t type = 0;
        if (!getU8(buf, p, type)) {
            return false;
        }

        Entry entry;
        if (type == kTypeString) {
            std::string value;
            if (!getBlob(buf, p, value)) {
                return false;
            }
            entry.value = std::move(value);
        } else if (type == kTypeList) {
            uint32_t n = 0;
            if (!getU32(buf, p, n)) {
                return false;
            }
            ListType list;
            for (uint32_t j = 0; j < n; ++j) {
                std::string item;
                if (!getBlob(buf, p, item)) {
                    return false;
                }
                list.push_back(std::move(item));
            }
            entry.value = std::move(list);
        } else if (type == kTypeHash) {
            uint32_t n = 0;
            if (!getU32(buf, p, n)) {
                return false;
            }
            HashType hash;
            for (uint32_t j = 0; j < n; ++j) {
                std::string field, value;
                if (!getBlob(buf, p, field) || !getBlob(buf, p, value)) {
                    return false;
                }
                hash.emplace(std::move(field), std::move(value));
            }
            entry.value = std::move(hash);
        } else if (type == kTypeZSet) {
            uint32_t n = 0;
            if (!getU32(buf, p, n)) {
                return false;
            }
            ZSetType zset;
            for (uint32_t j = 0; j < n; ++j) {
                std::string member;
                double score = 0;
                if (!getBlob(buf, p, member) || !getDouble(buf, p, score)) {
                    return false;
                }
                zset.scores.emplace(member, score);
                zset.sorted.insert({score, member});
            }
            entry.value = std::move(zset);
        } else {
            return false;  // unknown type tag
        }

        int64_t ttlMs = 0;
        if (!getI64(buf, p, ttlMs)) {
            return false;
        }
        if (ttlMs >= 0) {
            entry.hasExpiry = true;
            entry.expireAt = now + std::chrono::milliseconds(ttlMs);
        }
        loaded.emplace_back(std::move(key), std::move(entry));
    }

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

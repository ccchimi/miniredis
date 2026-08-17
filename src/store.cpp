#include "store.hpp"

void Store::set(const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> lock(mtx_);
    data_[key] = value;
}

std::optional<std::string> Store::get(const std::string& key) {
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = data_.find(key);
    if (it == data_.end()) {
        return std::nullopt;
    }
    return it->second;
}

int Store::del(const std::vector<std::string>& keys) {
    std::lock_guard<std::mutex> lock(mtx_);
    int removed = 0;
    for (const auto& key : keys) {
        removed += static_cast<int>(data_.erase(key));
    }
    return removed;
}

int Store::exists(const std::vector<std::string>& keys) {
    std::lock_guard<std::mutex> lock(mtx_);
    int count = 0;
    for (const auto& key : keys) {
        if (data_.find(key) != data_.end()) {
            ++count;
        }
    }
    return count;
}

size_t Store::size() {
    std::lock_guard<std::mutex> lock(mtx_);
    return data_.size();
}

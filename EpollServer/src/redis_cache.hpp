#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

struct redisContext;

namespace chat {

// Owned by Database; every call is serialized by Database::mutex_.
class RedisFriendCache {
public:
    struct Stats {
        std::uint64_t hits = 0, misses = 0, errors = 0, writes = 0;
    };
    RedisFriendCache();
    ~RedisFriendCache();
    RedisFriendCache(const RedisFriendCache &) = delete;
    RedisFriendCache &operator=(const RedisFriendCache &) = delete;
    std::optional<std::vector<std::string>> get(const std::string &user);
    void put(const std::string &user, const std::vector<std::string> &friends);
    void invalidate(const std::string &user, const std::string &other);
    Stats stats() const { return stats_; }

private:
    bool connect();
    void failed();
    std::string key(const std::string &user) const;
    std::string host_, password_, namespace_;
    int port_ = 6379, timeoutMs_ = 20, ttlSeconds_ = 30;
    redisContext *context_ = nullptr;
    std::uint64_t epoch_ = 0;
    std::chrono::steady_clock::time_point retryAt_{};
    Stats stats_;
};

} // namespace chat

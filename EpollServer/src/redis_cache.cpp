#include "redis_cache.hpp"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <random>
#include <stdexcept>

#ifdef CHAT_WITH_REDIS
#include <arpa/inet.h>
#include <hiredis/hiredis.h>
#endif

namespace chat {
namespace {
std::string env(const char *name)
{
    const char *value = std::getenv(name);
    return value ? value : "";
}
#ifdef CHAT_WITH_REDIS
constexpr std::size_t kMaxCacheBytes = 1024U * 1024U;
int setting(const char *name, int fallback, int maximum)
{
    const auto value = env(name);
    if (value.empty()) return fallback;
    std::size_t consumed = 0;
    const int result = std::stoi(value, &consumed);
    if (consumed != value.size() || result < 1 || result > maximum)
        throw std::invalid_argument(std::string("invalid ") + name);
    return result;
}
using Reply = std::unique_ptr<redisReply, decltype(&freeReplyObject)>;

// Length-prefix encoding preserves empty lists and names with delimiters.
std::optional<std::vector<std::string>> decode(const std::string &bytes)
{
    if (bytes.size() < 3 || bytes.size() > kMaxCacheBytes
            || bytes.compare(0, 3, "FC1") != 0) return std::nullopt;
    std::vector<std::string> names;
    std::size_t offset = 3;
    while (offset < bytes.size()) {
        if (bytes.size() - offset < 4) return std::nullopt;
        std::uint32_t size = 0;
        for (int i = 0; i < 4; ++i)
            size = (size << 8U) | static_cast<unsigned char>(bytes[offset++]);
        if (size > bytes.size() - offset) return std::nullopt;
        names.emplace_back(bytes.data() + offset, size);
        offset += size;
    }
    return names;
}
std::optional<std::string> encode(const std::vector<std::string> &names)
{
    std::string bytes = "FC1";
    for (const auto &name : names) {
        if (name.size() > kMaxCacheBytes - 4
                || bytes.size() > kMaxCacheBytes - 4 - name.size())
            return std::nullopt;
        const auto size = static_cast<std::uint32_t>(name.size());
        for (int shift = 24; shift >= 0; shift -= 8)
            bytes.push_back(static_cast<char>((size >> shift) & 0xffU));
        bytes += name;
    }
    return bytes;
}
#endif
} // namespace

RedisFriendCache::RedisFriendCache() : host_(env("CHAT_REDIS_HOST"))
{
    if (host_.empty()) return;
#ifdef CHAT_WITH_REDIS
    unsigned char address[sizeof(in6_addr)];
    if (inet_pton(AF_INET, host_.c_str(), address) != 1
            && inet_pton(AF_INET6, host_.c_str(), address) != 1)
        throw std::invalid_argument("CHAT_REDIS_HOST must be a numeric IP (no blocking DNS)");
    port_ = setting("CHAT_REDIS_PORT", 6379, 65535);
    timeoutMs_ = setting("CHAT_REDIS_TIMEOUT_MS", 20, 1000);
    ttlSeconds_ = setting("CHAT_REDIS_TTL_SECONDS", 30, 3600);
    password_ = env("CHAT_REDIS_PASSWORD");
    // Separate each Database instance, including after a server restart.
    std::random_device random;
    namespace_ = "qtchat:friends:v1:";
    for (int i = 0; i < 4; ++i) namespace_ += std::to_string(random()) + ":";
#else
    throw std::invalid_argument("CHAT_REDIS_HOST is set but built with CHAT_WITH_REDIS=OFF");
#endif
}
RedisFriendCache::~RedisFriendCache()
{
#ifdef CHAT_WITH_REDIS
    if (context_) redisFree(context_);
#endif
}
std::string RedisFriendCache::key(const std::string &user) const
{
    return namespace_ + std::to_string(epoch_) + ":" + user;
}
void RedisFriendCache::failed()
{
#ifdef CHAT_WITH_REDIS
    if (context_) redisFree(context_);
#endif
    context_ = nullptr;
    ++stats_.errors;
    // A failed DEL after a commit must never expose old data on reconnect.
    // Rotate the local namespace; abandoned entries expire by TTL.
    ++epoch_;
    retryAt_ = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    std::cerr << "Redis cache unavailable; using SQLite (retry in 1s)\n";
}
bool RedisFriendCache::connect()
{
#ifdef CHAT_WITH_REDIS
    if (host_.empty() || std::chrono::steady_clock::now() < retryAt_) return false;
    if (context_) return true;
    const timeval timeout{timeoutMs_ / 1000, (timeoutMs_ % 1000) * 1000};
    context_ = redisConnectWithTimeout(host_.c_str(), port_, timeout);
    if (!context_ || context_->err || redisSetTimeout(context_, timeout) != REDIS_OK) {
        failed();
        return false;
    }
    if (!password_.empty()) {
        Reply reply(static_cast<redisReply *>(redisCommand(context_, "AUTH %b",
            password_.data(), password_.size())), freeReplyObject);
        if (!reply || reply->type != REDIS_REPLY_STATUS
                || std::string(reply->str, reply->len) != "OK") {
            failed();
            return false;
        }
    }
    return true;
#else
    return false;
#endif
}
std::optional<std::vector<std::string>> RedisFriendCache::get(const std::string &user)
{
#ifdef CHAT_WITH_REDIS
    if (!connect()) return std::nullopt;
    const auto cacheKey = key(user);
    Reply reply(static_cast<redisReply *>(redisCommand(context_, "GET %b",
        cacheKey.data(), cacheKey.size())), freeReplyObject);
    if (!reply || (reply->type != REDIS_REPLY_STRING && reply->type != REDIS_REPLY_NIL)) {
        failed();
        return std::nullopt;
    }
    if (reply->type == REDIS_REPLY_STRING) {
        auto value = decode(std::string(reply->str, reply->len));
        if (value) {
            ++stats_.hits;
            return value;
        }
    }
    ++stats_.misses;
#else
    (void)user;
#endif
    return std::nullopt;
}
void RedisFriendCache::put(const std::string &user, const std::vector<std::string> &friends)
{
#ifdef CHAT_WITH_REDIS
    if (!connect()) return;
    const auto bytes = encode(friends);
    if (!bytes) return;
    const auto cacheKey = key(user);
    Reply reply(static_cast<redisReply *>(redisCommand(context_, "SET %b %b EX %d",
        cacheKey.data(), cacheKey.size(), bytes->data(), bytes->size(), ttlSeconds_)),
        freeReplyObject);
    if (!reply || reply->type != REDIS_REPLY_STATUS
            || std::string(reply->str, reply->len) != "OK") {
        failed();
        return;
    }
    ++stats_.writes;
#else
    (void)user;
    (void)friends;
#endif
}
void RedisFriendCache::invalidate(const std::string &user, const std::string &other)
{
#ifdef CHAT_WITH_REDIS
    if (!connect()) return;
    const auto first = key(user), second = key(other);
    Reply reply(static_cast<redisReply *>(redisCommand(context_, "DEL %b %b",
        first.data(), first.size(), second.data(), second.size())), freeReplyObject);
    if (!reply || reply->type != REDIS_REPLY_INTEGER) failed();
#else
    (void)user;
    (void)other;
#endif
}
} // namespace chat

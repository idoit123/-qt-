#pragma once

#include "redis_cache.hpp"

#include <mutex>
#include <string>
#include <vector>

struct sqlite3;

namespace chat {

class Database {
public:
    enum class FriendRequestResult { AlreadyFriends, Online, Offline, Missing, Error };

    explicit Database(std::string path);
    ~Database();
    Database(const Database &) = delete;
    Database &operator=(const Database &) = delete;

    bool initialize();
    bool registerUser(const std::string &name, const std::string &password);
    bool login(const std::string &name, const std::string &password);
    void offline(const std::string &name);
    std::vector<std::string> onlineUsers();
    int searchUser(const std::string &name);
    FriendRequestResult friendRequest(const std::string &user,
                                      const std::string &friendName);
    bool addFriend(const std::string &user, const std::string &friendName);
    bool deleteFriend(const std::string &user, const std::string &friendName);
    std::vector<std::string> friends(const std::string &user);
    struct CacheStats {
        RedisFriendCache::Stats redis;
        std::uint64_t friendQueries;
    };
    CacheStats cacheStats();

private:
    bool execute(const char *sql);

    std::string path_;
    sqlite3 *db_ = nullptr;
    std::mutex mutex_;
    RedisFriendCache friendCache_;
    std::uint64_t friendQueries_ = 0;
};

} // namespace chat

#include "database.hpp"

#include <sqlite3.h>

#include <iostream>
#include <utility>

namespace chat {
namespace {

class Statement {
public:
    Statement(sqlite3 *db, const char *sql)
    {
        if (sqlite3_prepare_v2(db, sql, -1, &statement_, nullptr) != SQLITE_OK)
            statement_ = nullptr;
    }
    ~Statement() { sqlite3_finalize(statement_); }
    sqlite3_stmt *get() const { return statement_; }
    explicit operator bool() const { return statement_ != nullptr; }

private:
    sqlite3_stmt *statement_ = nullptr;
};

void bindText(sqlite3_stmt *statement, int index, const std::string &value)
{
    sqlite3_bind_text(statement, index, value.c_str(),
                      static_cast<int>(value.size()), SQLITE_TRANSIENT);
}

} // namespace

Database::Database(std::string path) : path_(std::move(path)) {}

Database::~Database()
{
    const auto stats = friendCache_.stats();
    if (friendQueries_ || stats.hits || stats.errors)
        std::clog << "friend cache: hits=" << stats.hits
                  << " misses=" << stats.misses << " errors=" << stats.errors
                  << " fills=" << stats.writes << " sqlite_queries=" << friendQueries_ << '\n';
    if (db_) sqlite3_close(db_);
}

bool Database::execute(const char *sql)
{
    char *error = nullptr;
    const int result = sqlite3_exec(db_, sql, nullptr, nullptr, &error);
    if (result != SQLITE_OK) {
        std::cerr << "database error: " << (error ? error : "unknown") << '\n';
        sqlite3_free(error);
        return false;
    }
    return true;
}

bool Database::initialize()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (sqlite3_open_v2(path_.c_str(), &db_,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE
                        | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK) {
        std::cerr << "cannot open database: " << path_ << '\n';
        return false;
    }
    sqlite3_busy_timeout(db_, 5000);
    return execute("PRAGMA foreign_keys = ON")
            && execute("CREATE TABLE IF NOT EXISTS usrInfo ("
                       "id INTEGER PRIMARY KEY, name TEXT UNIQUE NOT NULL, "
                       "pwd TEXT NOT NULL, online INTEGER NOT NULL DEFAULT 0)")
            && execute("CREATE TABLE IF NOT EXISTS friend ("
                       "id INTEGER NOT NULL, friendId INTEGER NOT NULL)")
            && execute("CREATE INDEX IF NOT EXISTS idx_friend_id ON friend(id)")
            && execute("CREATE INDEX IF NOT EXISTS idx_friend_friend_id ON friend(friendId)")
            && execute("UPDATE usrInfo SET online = 0");
}

bool Database::registerUser(const std::string &name, const std::string &password)
{
    std::lock_guard<std::mutex> lock(mutex_);
    Statement statement(db_, "INSERT INTO usrInfo(name, pwd) VALUES(?, ?)");
    if (!statement) return false;
    bindText(statement.get(), 1, name);
    bindText(statement.get(), 2, password);
    return sqlite3_step(statement.get()) == SQLITE_DONE;
}

bool Database::login(const std::string &name, const std::string &password)
{
    std::lock_guard<std::mutex> lock(mutex_);
    Statement statement(db_, "UPDATE usrInfo SET online = 1 "
                             "WHERE name = ? AND pwd = ? AND online = 0");
    if (!statement) return false;
    bindText(statement.get(), 1, name);
    bindText(statement.get(), 2, password);
    return sqlite3_step(statement.get()) == SQLITE_DONE
            && sqlite3_changes(db_) == 1;
}

void Database::offline(const std::string &name)
{
    std::lock_guard<std::mutex> lock(mutex_);
    Statement statement(db_, "UPDATE usrInfo SET online = 0 WHERE name = ?");
    if (!statement) return;
    bindText(statement.get(), 1, name);
    sqlite3_step(statement.get());
}

std::vector<std::string> Database::onlineUsers()
{
    std::lock_guard<std::mutex> lock(mutex_);
    Statement statement(db_, "SELECT name FROM usrInfo WHERE online = 1");
    std::vector<std::string> result;
    if (!statement) return result;
    while (sqlite3_step(statement.get()) == SQLITE_ROW) {
        const auto *text = sqlite3_column_text(statement.get(), 0);
        if (text) result.emplace_back(reinterpret_cast<const char *>(text));
    }
    return result;
}

int Database::searchUser(const std::string &name)
{
    std::lock_guard<std::mutex> lock(mutex_);
    Statement statement(db_, "SELECT online FROM usrInfo WHERE name = ?");
    if (!statement) return -1;
    bindText(statement.get(), 1, name);
    if (sqlite3_step(statement.get()) != SQLITE_ROW) return -1;
    return sqlite3_column_int(statement.get(), 0) == 1 ? 1 : 0;
}

Database::FriendRequestResult Database::friendRequest(
        const std::string &user, const std::string &friendName)
{
    std::lock_guard<std::mutex> lock(mutex_);
    Statement target(db_, "SELECT online FROM usrInfo WHERE name = ?");
    if (!target) return FriendRequestResult::Error;
    bindText(target.get(), 1, friendName);
    if (sqlite3_step(target.get()) != SQLITE_ROW)
        return FriendRequestResult::Missing;
    const bool online = sqlite3_column_int(target.get(), 0) == 1;

    Statement relation(db_,
        "SELECT 1 FROM friend WHERE "
        "(id = (SELECT id FROM usrInfo WHERE name = ?) "
        "AND friendId = (SELECT id FROM usrInfo WHERE name = ?)) OR "
        "(id = (SELECT id FROM usrInfo WHERE name = ?) "
        "AND friendId = (SELECT id FROM usrInfo WHERE name = ?)) LIMIT 1");
    if (!relation) return FriendRequestResult::Error;
    bindText(relation.get(), 1, user);
    bindText(relation.get(), 2, friendName);
    bindText(relation.get(), 3, friendName);
    bindText(relation.get(), 4, user);
    if (sqlite3_step(relation.get()) == SQLITE_ROW)
        return FriendRequestResult::AlreadyFriends;
    return online ? FriendRequestResult::Online : FriendRequestResult::Offline;
}

bool Database::addFriend(const std::string &user, const std::string &friendName)
{
    std::lock_guard<std::mutex> lock(mutex_);
    Statement statement(db_,
        "INSERT INTO friend(id, friendId) VALUES("
        "(SELECT id FROM usrInfo WHERE name = ?), "
        "(SELECT id FROM usrInfo WHERE name = ?))");
    if (!statement) return false;
    bindText(statement.get(), 1, user);
    bindText(statement.get(), 2, friendName);
    const bool ok = sqlite3_step(statement.get()) == SQLITE_DONE;
    if (ok) friendCache_.invalidate(user, friendName);
    return ok;
}

bool Database::deleteFriend(const std::string &user,
                            const std::string &friendName)
{
    std::lock_guard<std::mutex> lock(mutex_);
    Statement statement(db_,
        "DELETE FROM friend WHERE "
        "(id = (SELECT id FROM usrInfo WHERE name = ?) "
        "AND friendId = (SELECT id FROM usrInfo WHERE name = ?)) OR "
        "(id = (SELECT id FROM usrInfo WHERE name = ?) "
        "AND friendId = (SELECT id FROM usrInfo WHERE name = ?))");
    if (!statement) return false;
    bindText(statement.get(), 1, user);
    bindText(statement.get(), 2, friendName);
    bindText(statement.get(), 3, friendName);
    bindText(statement.get(), 4, user);
    const bool ok = sqlite3_step(statement.get()) == SQLITE_DONE
            && sqlite3_changes(db_) > 0;
    if (ok) friendCache_.invalidate(user, friendName);
    return ok;
}

std::vector<std::string> Database::friends(const std::string &user)
{
    std::lock_guard<std::mutex> lock(mutex_);
    // Order read/fill and write/invalidate under the same lock. A concurrent
    // cache miss cannot overwrite a committed friendship change with old data.
    if (auto cached = friendCache_.get(user)) return std::move(*cached);
    ++friendQueries_;
    Statement statement(db_,
        "SELECT u.name FROM friend f JOIN usrInfo u ON f.friendId = u.id "
        "WHERE f.id = (SELECT id FROM usrInfo WHERE name = ?) UNION "
        "SELECT u.name FROM friend f JOIN usrInfo u ON f.id = u.id "
        "WHERE f.friendId = (SELECT id FROM usrInfo WHERE name = ?)");
    std::vector<std::string> result;
    if (!statement) return result;
    bindText(statement.get(), 1, user);
    bindText(statement.get(), 2, user);
    int status = SQLITE_OK;
    while ((status = sqlite3_step(statement.get())) == SQLITE_ROW) {
        const auto *text = sqlite3_column_text(statement.get(), 0);
        if (text) result.emplace_back(reinterpret_cast<const char *>(text));
    }
    if (status == SQLITE_DONE) friendCache_.put(user, result);
    return result;
}

Database::CacheStats Database::cacheStats()
{
    std::lock_guard<std::mutex> lock(mutex_);
    return {friendCache_.stats(), friendQueries_};
}

} // namespace chat

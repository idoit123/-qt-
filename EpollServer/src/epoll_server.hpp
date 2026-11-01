#pragma once

#include "database.hpp"
#include "flow_control.hpp"
#include "protocol.hpp"
#include "file_store.hpp"

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace chat {

class EpollServer;
class Reactor;
#ifdef CHAT_WITH_VIDEO
class VideoProcessor;
struct MotionState;
#endif

class Connection : public std::enable_shared_from_this<Connection> {
public:
    Connection(int fd, std::uint32_t generation, Reactor &reactor,
               EpollServer &server);

    int fd() const { return fd_; }
    std::uint32_t generation() const { return generation_; }
    std::uint64_t eventToken() const;
    const std::string &userName() const { return userName_; }
    bool authenticated() const { return !userName_.empty(); }
    void authenticate(std::string userName) { userName_ = std::move(userName); }
    Reactor &reactor() const { return reactor_; }

    bool handleEvents(std::uint32_t events);
    bool queue(Frame frame);
    FileSession &files() { return files_; }
    bool videoPending = false; // Only touched by the owning Reactor.
    bool active = true;
#ifdef CHAT_WITH_VIDEO
    std::shared_ptr<MotionState> motion;
#endif

private:
    bool readAvailable();
    bool flushWrites();

    int fd_;
    std::uint32_t generation_;
    Reactor &reactor_;
    EpollServer &server_;
    FrameDecoder decoder_;
    TokenBucket rateLimiter_;
    OutboundBudget outboundBudget_;
    std::deque<Frame> output_;
    std::size_t outputOffset_ = 0;
    std::size_t pendingBytes_ = 0;
    std::string userName_;
    FileSession files_;
};

class Reactor {
public:
    explicit Reactor(EpollServer &server);
    ~Reactor();
    Reactor(const Reactor &) = delete;
    Reactor &operator=(const Reactor &) = delete;

    void start();
    void stop();
    void addConnection(int fd);
    void post(std::function<void()> task);
    void updateInterest(const Connection &connection, bool wantsWrite);
    void closeConnection(int fd, std::uint32_t generation);

private:
    void run();
    void drainTasks();

    EpollServer &server_;
    int epollFd_ = -1;
    int wakeFd_ = -1;
    std::atomic<bool> running_{false};
    std::thread thread_;
    std::mutex taskMutex_;
    std::deque<std::function<void()>> tasks_;
    std::unordered_map<int, std::shared_ptr<Connection>> connections_;
    std::uint32_t nextGeneration_ = 1;
};

class EpollServer {
public:
    EpollServer(std::uint16_t port, std::string databasePath,
                std::size_t ioThreads);
    ~EpollServer();
    EpollServer(const EpollServer &) = delete;
    EpollServer &operator=(const EpollServer &) = delete;

    bool run();
    bool handleFrame(const std::shared_ptr<Connection> &connection, Frame frame);
    void connectionClosed(const std::shared_ptr<Connection> &connection);
    void route(const std::string &userName, Frame frame);

private:
    bool registerRoute(const std::string &userName,
                       const std::shared_ptr<Connection> &connection);
    void unregisterRoute(const std::string &userName, const Connection *connection);
    bool send(const std::shared_ptr<Connection> &connection, Frame frame);
    bool validateIdentity(const Connection &connection, const Frame &frame) const;

    std::uint16_t port_;
    Database database_;
    FileStore files_;
    std::size_t ioThreadCount_;
    std::vector<std::unique_ptr<Reactor>> reactors_;
    std::mutex routesMutex_;
    std::unordered_map<std::string, std::weak_ptr<Connection>> routes_;
#ifdef CHAT_WITH_VIDEO
    std::unique_ptr<VideoProcessor> video_;
#endif
};

} // namespace chat

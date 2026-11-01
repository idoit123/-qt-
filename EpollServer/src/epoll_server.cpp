#include "epoll_server.hpp"
#ifdef CHAT_WITH_VIDEO
#include "video_processor.hpp"
#endif

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <utility>

namespace chat {
namespace {

constexpr std::uint64_t kWakeToken = std::numeric_limits<std::uint64_t>::max();
constexpr int kMaxEvents = 256;

bool needsFirstField(MessageType type)
{
    return type == MessageType::RegisterRequest
            || type == MessageType::LoginRequest
            || type == MessageType::SearchRequest
            || type == MessageType::AddFriendRequest
            || type == MessageType::AddFriendAgree
            || type == MessageType::AddFriendRefuse
            || type == MessageType::FlushFriendRequest
            || type == MessageType::DeleteFriendRequest
            || type == MessageType::PrivateChatRequest
            || type == MessageType::GroupChatRequest;
}

bool needsSecondField(MessageType type)
{
    return type == MessageType::RegisterRequest
            || type == MessageType::LoginRequest
            || type == MessageType::AddFriendRequest
            || type == MessageType::AddFriendAgree
            || type == MessageType::AddFriendRefuse
            || type == MessageType::DeleteFriendRequest
            || type == MessageType::PrivateChatRequest;
}

bool identityInFirstField(MessageType type)
{
    return type == MessageType::AddFriendRequest
            || type == MessageType::FlushFriendRequest
            || type == MessageType::DeleteFriendRequest
            || type == MessageType::PrivateChatRequest
            || type == MessageType::GroupChatRequest;
}

bool identityInSecondField(MessageType type)
{
    return type == MessageType::AddFriendAgree
            || type == MessageType::AddFriendRefuse;
}

bool authenticationRequest(MessageType type)
{
    return type == MessageType::RegisterRequest
            || type == MessageType::LoginRequest;
}

Frame textResponse(MessageType type, std::string_view text)
{
    return makeFrame(type, text);
}

void closeFd(int &fd)
{
    if (fd >= 0) {
        ::close(fd);
        fd = -1;
    }
}

} // namespace

Connection::Connection(int fd, std::uint32_t generation, Reactor &reactor,
                       EpollServer &server)
    : fd_(fd), generation_(generation), reactor_(reactor), server_(server)
{
}

std::uint64_t Connection::eventToken() const
{
    return (static_cast<std::uint64_t>(generation_) << 32U)
            | static_cast<std::uint32_t>(fd_);
}

bool Connection::handleEvents(std::uint32_t events)
{
    if ((events & (EPOLLERR | EPOLLHUP)) != 0) return false;
    if ((events & EPOLLIN) != 0 && !readAvailable()) return false;
    if ((events & EPOLLOUT) != 0 && !flushWrites()) return false;
    if ((events & EPOLLRDHUP) != 0) return false;
    return true;
}

bool Connection::readAvailable()
{
    std::array<std::uint8_t, 8192> buffer{};
    while (true) {
        const ssize_t received = ::recv(fd_, buffer.data(), buffer.size(), 0);
        if (received > 0) {
            decoder_.append(buffer.data(), static_cast<std::size_t>(received));
            while (true) {
                Frame frame;
                const auto result = decoder_.next(frame);
                if (result == FrameDecoder::Result::NeedMore) break;
                if (result == FrameDecoder::Result::Invalid) {
                    std::cerr << "invalid frame on fd=" << fd_ << '\n';
                    return false;
                }
                if (!rateLimiter_.consume()) {
                    std::cerr << "rate limit exceeded user=" << userName_ << '\n';
                    return false;
                }
                if (!server_.handleFrame(shared_from_this(), std::move(frame)))
                    return false;
            }
            continue;
        }
        if (received == 0) return false;
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return true;
        return false;
    }
}

bool Connection::queue(Frame frame)
{
    if (!outboundBudget_.canQueue(pendingBytes_, frame.size())) {
        std::cerr << "outbound high-water exceeded user=" << userName_
                  << " pending=" << pendingBytes_
                  << " payload=" << frame.size() << '\n';
        return false;
    }
    pendingBytes_ += frame.size();
    output_.push_back(std::move(frame));
    reactor_.updateInterest(*this, true);
    return flushWrites();
}

bool Connection::flushWrites()
{
    while (!output_.empty()) {
        Frame &front = output_.front();
        const std::size_t remaining = front.size() - outputOffset_;
        const ssize_t written = ::send(fd_, front.data() + outputOffset_, remaining,
                                       MSG_NOSIGNAL);
        if (written > 0) {
            outputOffset_ += static_cast<std::size_t>(written);
            pendingBytes_ -= static_cast<std::size_t>(written);
            if (outputOffset_ == front.size()) {
                output_.pop_front();
                outputOffset_ = 0;
            }
            continue;
        }
        if (written < 0 && errno == EINTR) continue;
        if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            reactor_.updateInterest(*this, true);
            return true;
        }
        return false;
    }
    reactor_.updateInterest(*this, false);
    return true;
}

Reactor::Reactor(EpollServer &server) : server_(server)
{
    epollFd_ = epoll_create1(EPOLL_CLOEXEC);
    wakeFd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (epollFd_ < 0 || wakeFd_ < 0) {
        closeFd(wakeFd_);
        closeFd(epollFd_);
        throw std::runtime_error("cannot initialize epoll reactor");
    }

    epoll_event event{};
    event.events = EPOLLIN;
    event.data.u64 = kWakeToken;
    if (epoll_ctl(epollFd_, EPOLL_CTL_ADD, wakeFd_, &event) != 0) {
        closeFd(wakeFd_);
        closeFd(epollFd_);
    }
    if (epollFd_ < 0 || wakeFd_ < 0)
        throw std::runtime_error("cannot initialize epoll reactor");
}

Reactor::~Reactor()
{
    stop();
    closeFd(wakeFd_);
    closeFd(epollFd_);
}

void Reactor::start()
{
    if (epollFd_ < 0 || wakeFd_ < 0) return;
    running_ = true;
    thread_ = std::thread(&Reactor::run, this);
}

void Reactor::stop()
{
    if (!running_.exchange(false)) {
        if (thread_.joinable()) thread_.join();
        return;
    }
    const std::uint64_t value = 1;
    const ssize_t ignored = ::write(wakeFd_, &value, sizeof(value));
    (void)ignored;
    if (thread_.joinable()) thread_.join();
}

void Reactor::post(std::function<void()> task)
{
    {
        std::lock_guard<std::mutex> lock(taskMutex_);
        tasks_.push_back(std::move(task));
    }
    const std::uint64_t value = 1;
    const ssize_t ignored = ::write(wakeFd_, &value, sizeof(value));
    (void)ignored;
}

void Reactor::addConnection(int fd)
{
    post([this, fd]() {
        const std::uint32_t generation = nextGeneration_++;
        auto connection = std::make_shared<Connection>(fd, generation, *this, server_);
        epoll_event event{};
        event.events = EPOLLIN | EPOLLRDHUP | EPOLLET;
        event.data.u64 = connection->eventToken();
        if (epoll_ctl(epollFd_, EPOLL_CTL_ADD, fd, &event) != 0) {
            ::close(fd);
            return;
        }
        connections_[fd] = std::move(connection);
    });
}

void Reactor::updateInterest(const Connection &connection, bool wantsWrite)
{
    epoll_event event{};
    event.events = EPOLLIN | EPOLLRDHUP | EPOLLET
            | (wantsWrite ? EPOLLOUT : 0U);
    event.data.u64 = connection.eventToken();
    if (epoll_ctl(epollFd_, EPOLL_CTL_MOD, connection.fd(), &event) != 0)
        std::cerr << "epoll MOD failed fd=" << connection.fd() << '\n';
}

void Reactor::closeConnection(int fd, std::uint32_t generation)
{
    const auto it = connections_.find(fd);
    if (it == connections_.end() || it->second->generation() != generation) return;
    const auto connection = it->second;
    connection->active = false;
    epoll_ctl(epollFd_, EPOLL_CTL_DEL, fd, nullptr);
    server_.connectionClosed(connection);
    ::close(fd);
    connections_.erase(it);
}

void Reactor::drainTasks()
{
    std::uint64_t value = 0;
    while (::read(wakeFd_, &value, sizeof(value)) > 0) {}

    std::deque<std::function<void()>> pending;
    {
        std::lock_guard<std::mutex> lock(taskMutex_);
        pending.swap(tasks_);
    }
    for (auto &task : pending) task();
}

void Reactor::run()
{
    std::array<epoll_event, kMaxEvents> events{};
    while (running_) {
        const int count = epoll_wait(epollFd_, events.data(), events.size(), -1);
        if (count < 0) {
            if (errno == EINTR) continue;
            break;
        }
        for (int i = 0; i < count; ++i) {
            if (events[i].data.u64 == kWakeToken) {
                drainTasks();
                continue;
            }
            const int fd = static_cast<int>(events[i].data.u64 & 0xffffffffU);
            const auto generation = static_cast<std::uint32_t>(events[i].data.u64 >> 32U);
            const auto it = connections_.find(fd);
            if (it == connections_.end() || it->second->generation() != generation)
                continue;
            if (!it->second->handleEvents(events[i].events))
                closeConnection(fd, generation);
        }
    }

    drainTasks();
    while (!connections_.empty()) {
        const auto connection = connections_.begin()->second;
        closeConnection(connection->fd(), connection->generation());
    }
}

EpollServer::EpollServer(std::uint16_t port, std::string databasePath,
                         std::size_t ioThreads)
    : port_(port), database_(std::move(databasePath)),
      files_(std::getenv("CHAT_FILE_ROOT") ? std::getenv("CHAT_FILE_ROOT") : "files"),
      ioThreadCount_(ioThreads == 0 ? 1 : ioThreads)
{
}

EpollServer::~EpollServer()
{
#ifdef CHAT_WITH_VIDEO
    if (video_) video_->stop();
#endif
    for (auto &reactor : reactors_) reactor->stop();
}

bool EpollServer::registerRoute(
        const std::string &userName,
        const std::shared_ptr<Connection> &connection)
{
    std::lock_guard<std::mutex> lock(routesMutex_);
    const auto it = routes_.find(userName);
    if (it != routes_.end() && !it->second.expired()) return false;
    routes_[userName] = connection;
    return true;
}

void EpollServer::unregisterRoute(const std::string &userName,
                                  const Connection *connection)
{
    std::lock_guard<std::mutex> lock(routesMutex_);
    const auto it = routes_.find(userName);
    if (it == routes_.end()) return;
    const auto current = it->second.lock();
    if (!current || current.get() == connection) routes_.erase(it);
}

void EpollServer::connectionClosed(const std::shared_ptr<Connection> &connection)
{
    connection->files().cancel();
    if (!connection->authenticated()) return;
    unregisterRoute(connection->userName(), connection.get());
    database_.offline(connection->userName());
}

bool EpollServer::send(const std::shared_ptr<Connection> &connection, Frame frame)
{
    if (connection->queue(std::move(frame))) return true;
    connection->reactor().closeConnection(connection->fd(), connection->generation());
    return false;
}

void EpollServer::route(const std::string &userName, Frame frame)
{
    std::weak_ptr<Connection> target;
    {
        std::lock_guard<std::mutex> lock(routesMutex_);
        const auto it = routes_.find(userName);
        if (it == routes_.end()) return;
        target = it->second;
    }
    const auto targetConnection = target.lock();
    if (!targetConnection) return;
    auto payload = std::make_shared<Frame>(std::move(frame));
    Reactor &reactor = targetConnection->reactor();
    reactor.post([this, target, payload]() {
        const auto connection = target.lock();
        if (!connection) return;
        send(connection, *payload);
    });
}

bool EpollServer::validateIdentity(const Connection &connection,
                                   const Frame &frame) const
{
    const auto type = messageType(frame);
    const auto &value = *header(frame);
    if (needsFirstField(type) && !hasTerminatedField(value, 0)) return false;
    if (needsSecondField(type) && !hasTerminatedField(value, 32)) return false;

    if (connection.authenticated() == authenticationRequest(type)) return false;
    if (identityInFirstField(type) && field(value, 0) != connection.userName())
        return false;
    if (identityInSecondField(type) && field(value, 32) != connection.userName())
        return false;
    if ((type == MessageType::PrivateChatRequest
            || type == MessageType::GroupChatRequest)
            && value.messageLength > kMaxChatMessageLength)
        return false;
    return true;
}

bool EpollServer::handleFrame(const std::shared_ptr<Connection> &connection,
                              Frame frame)
{
    if (!validateIdentity(*connection, frame)) {
        std::cerr << "authentication/protocol validation failed fd="
                  << connection->fd() << " user=" << connection->userName() << '\n';
        return false;
    }

    const auto type = messageType(frame);
    const auto &value = *header(frame);
    const std::string first = needsFirstField(type) ? field(value, 0) : std::string{};
    const std::string second = needsSecondField(type) ? field(value, 32) : std::string{};

    switch (type) {
    case MessageType::VideoRequest: {
        const auto videoFields = value.data;
        const auto replyError = [this, connection, videoFields](const char *message) {
            Frame response = makeFrame(MessageType::VideoError, {}, {}, message);
            header(response)->data = videoFields;
            return send(connection, std::move(response));
        };
        if (value.messageLength == 0 || value.messageLength > 512U * 1024U
                || frame.size() != sizeof(LegacyHeader) + value.messageLength)
            return replyError("Video payload must be 1..524288 bytes");
#ifdef CHAT_WITH_VIDEO
        // A sender may not pipeline frames. This bounds memory and latency,
        // including callbacks posted to the Reactor's task queue.
        if (connection->videoPending) return false;
        if (!connection->motion) connection->motion = std::make_shared<MotionState>();
        connection->videoPending = true;
        std::weak_ptr<Connection> weak = connection;
        Reactor *reactor = &connection->reactor();
        auto done = [this, weak, reactor](Frame response) {
            reactor->post([this, weak, response = std::move(response)]() mutable {
                const auto target = weak.lock();
                if (!target || !target->active) return;
                target->videoPending = false;
                send(target, std::move(response));
            });
        };
        if (!video_->submit(std::move(frame), connection->motion, std::move(done))) {
            connection->videoPending = false;
            return replyError("Video worker busy");
        }
        return true;
#else
        return replyError("Rebuild server with -DCHAT_WITH_VIDEO=ON");
#endif
    }
    case MessageType::FileRequest:
        return send(connection, files_.handle(connection->userName(), connection->files(), frame));
    case MessageType::RegisterRequest: {
        const bool ok = database_.registerUser(first, second);
        return send(connection, textResponse(MessageType::RegisterResponse,
            ok ? "regtist ok" : "regist failed : name existed"));
    }
    case MessageType::LoginRequest: {
        const bool ok = database_.login(first, second);
        if (ok && !registerRoute(first, connection)) {
            database_.offline(first);
            return send(connection, textResponse(MessageType::LoginResponse,
                "login failed : name failed or password failed or relogin"));
        }
        if (ok) connection->authenticate(first);
        return send(connection, textResponse(MessageType::LoginResponse,
            ok ? "login ok"
               : "login failed : name failed or password failed or relogin"));
    }
    case MessageType::AllOnlineRequest: {
        const auto users = database_.onlineUsers();
        Frame response = makeFrame(MessageType::AllOnlineResponse, {}, {}, nullptr,
                                   users.size() * kNameFieldLength);
        std::size_t offset = sizeof(LegacyHeader);
        for (const auto &user : users) {
            const std::size_t size = std::min(user.size(), kNameFieldLength - 1);
            std::memcpy(response.data() + offset, user.data(), size);
            offset += kNameFieldLength;
        }
        return send(connection, std::move(response));
    }
    case MessageType::SearchRequest: {
        const int status = database_.searchUser(first);
        const char *text = status < 0 ? "no such people"
                : (status == 1 ? "online" : "offline");
        return send(connection, textResponse(MessageType::SearchResponse, text));
    }
    case MessageType::AddFriendRequest: {
        switch (database_.friendRequest(first, second)) {
        case Database::FriendRequestResult::AlreadyFriends:
            return send(connection, textResponse(MessageType::AddFriendResponse,
                                                  "friend exist"));
        case Database::FriendRequestResult::Online:
            route(second, std::move(frame));
            return true;
        case Database::FriendRequestResult::Offline:
            return send(connection, textResponse(MessageType::AddFriendResponse,
                                                  "usr offline"));
        case Database::FriendRequestResult::Missing:
            return send(connection, textResponse(MessageType::AddFriendResponse,
                                                  "usr not exit"));
        case Database::FriendRequestResult::Error:
            return send(connection, textResponse(MessageType::AddFriendResponse,
                                                  "unknow error"));
        }
        return false;
    }
    case MessageType::AddFriendAgree: {
        if (!database_.addFriend(first, second)) return false;
        route(first, makeFrame(MessageType::AddFriendAgree, second));
        return true;
    }
    case MessageType::AddFriendRefuse:
        route(first, makeFrame(MessageType::AddFriendRefuse, second));
        return true;
    case MessageType::FlushFriendRequest: {
        const auto names = database_.friends(connection->userName());
        Frame response = makeFrame(MessageType::FlushFriendResponse, {}, {}, nullptr,
                                   names.size() * kNameFieldLength);
        std::size_t offset = sizeof(LegacyHeader);
        for (const auto &name : names) {
            const std::size_t size = std::min(name.size(), kNameFieldLength - 1);
            std::memcpy(response.data() + offset, name.data(), size);
            offset += kNameFieldLength;
        }
        return send(connection, std::move(response));
    }
    case MessageType::DeleteFriendRequest: {
        const bool ok = database_.deleteFriend(first, second);
        return send(connection, makeFrame(
            ok ? MessageType::DeleteFriendResponseOk
               : MessageType::DeleteFriendResponseFailed,
            first, second));
    }
    case MessageType::PrivateChatRequest:
        route(second, std::move(frame));
        return true;
    case MessageType::GroupChatRequest: {
        header(frame)->messageType = static_cast<std::uint32_t>(
            MessageType::GroupChatResponse);
        for (const auto &friendName : database_.friends(connection->userName()))
            route(friendName, frame);
        return true;
    }
    default:
        std::cerr << "unsupported client message type="
                  << static_cast<std::uint32_t>(type) << '\n';
        return false;
    }
}

bool EpollServer::run()
{
    if (!database_.initialize()) return false;

    sigset_t signalMask;
    sigemptyset(&signalMask);
    sigaddset(&signalMask, SIGINT);
    sigaddset(&signalMask, SIGTERM);
    if (pthread_sigmask(SIG_BLOCK, &signalMask, nullptr) != 0) return false;//屏蔽信号常规发送
#ifdef CHAT_WITH_VIDEO
    video_ = std::make_unique<VideoProcessor>();
#endif

    reactors_.reserve(ioThreadCount_);
    for (std::size_t i = 0; i < ioThreadCount_; ++i) {
        reactors_.push_back(std::make_unique<Reactor>(*this));
        reactors_.back()->start();
    }

    int listenFd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (listenFd < 0) return false;
    int reuse = 1;
    setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port_);
    if (bind(listenFd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0
            || listen(listenFd, SOMAXCONN) != 0) {
        closeFd(listenFd);
        return false;
    }

    int signalFd = signalfd(-1, &signalMask, SFD_NONBLOCK | SFD_CLOEXEC);
    int acceptEpoll = epoll_create1(EPOLL_CLOEXEC);
    if (signalFd < 0 || acceptEpoll < 0) {
        closeFd(signalFd);
        closeFd(acceptEpoll);
        closeFd(listenFd);
        return false;
    }

    epoll_event listenEvent{};
    listenEvent.events = EPOLLIN | EPOLLET;
    listenEvent.data.u64 = 1;
    if (epoll_ctl(acceptEpoll, EPOLL_CTL_ADD, listenFd, &listenEvent) != 0) {
        closeFd(acceptEpoll);
        closeFd(signalFd);
        closeFd(listenFd);
        return false;
    }
    epoll_event signalEvent{};
    signalEvent.events = EPOLLIN;
    signalEvent.data.u64 = 2;
    if (epoll_ctl(acceptEpoll, EPOLL_CTL_ADD, signalFd, &signalEvent) != 0) {
        closeFd(acceptEpoll);
        closeFd(signalFd);
        closeFd(listenFd);
        return false;
    }

    bool running = true;
    std::size_t nextReactor = 0;
    std::array<epoll_event, 16> events{};
    while (running) {
        const int count = epoll_wait(acceptEpoll, events.data(), events.size(), -1);
        if (count < 0) {
            if (errno == EINTR) continue;
            break;
        }
        for (int i = 0; i < count; ++i) {
            if (events[i].data.u64 == 2) {
                signalfd_siginfo info{};
                const ssize_t ignored = ::read(signalFd, &info, sizeof(info));
                (void)ignored;
                running = false;
                continue;
            }
            while (true) {
                int clientFd = accept4(listenFd, nullptr, nullptr,
                                       SOCK_NONBLOCK | SOCK_CLOEXEC);
                if (clientFd >= 0) {
                    int noDelay = 1;
                    setsockopt(clientFd, IPPROTO_TCP, TCP_NODELAY,
                               &noDelay, sizeof(noDelay));
                    reactors_[nextReactor++ % reactors_.size()]->addConnection(clientFd);
                    continue;
                }
                if (errno == EINTR) continue;
                if (errno != EAGAIN && errno != EWOULDBLOCK)
                    std::cerr << "accept4 failed: " << std::strerror(errno) << '\n';
                break;
            }
        }
    }

    closeFd(acceptEpoll);
    closeFd(signalFd);
    closeFd(listenFd);
#ifdef CHAT_WITH_VIDEO
    if (video_) video_->stop();
#endif
    for (auto &reactor : reactors_) reactor->stop();
    return true;
}

} // namespace chat

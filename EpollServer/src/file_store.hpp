#pragma once
#include "protocol.hpp"
#include <cstdint>
#include <mutex>
#include <string>
namespace chat {
constexpr std::size_t kFileChunkSize = 64U * 1024U;
constexpr std::uint64_t kMaxFileSize = 256ULL * 1024ULL * 1024ULL;
struct FileSession {
    FileSession() = default;
    ~FileSession();
    FileSession(const FileSession &) = delete;
    FileSession &operator=(const FileSession &) = delete;
    void cancel();
    int fd = -1, parent = -1;
    std::string temporary, destination;
    std::uint64_t total = 0, transferred = 0;
    bool uploading = false;
};
class FileStore {
public:
    explicit FileStore(const std::string &path);
    ~FileStore();
    Frame handle(const std::string &user, FileSession &session, const Frame &request);
private:
    int root_ = -1;
    std::mutex mutex_;
};
}

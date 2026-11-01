#include "file_store.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <filesystem>
#include <linux/fs.h>
#include <memory>
#include <stdexcept>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
namespace chat {
namespace {
struct Fd {
    int value;
    explicit Fd(int fd) : value(fd) { if (fd < 0) throw std::runtime_error(std::strerror(errno)); }
    ~Fd() { if (value >= 0) ::close(value); }
    Fd(const Fd &) = delete;
    Fd &operator=(const Fd &) = delete;
    int release() { int result = value; value = -1; return result; }
};
void require(bool condition, const char *message) { if (!condition) throw std::runtime_error(message); }
void checked(int result) { if (result < 0) throw std::runtime_error(std::strerror(errno)); }
Frame response(std::string_view op, std::string_view status, std::string_view body = {}) {
    return makeFrame(MessageType::FileResponse, op, status,
        reinterpret_cast<const std::uint8_t *>(body.data()), body.size());
}
std::vector<std::string> paths(std::string_view body, std::size_t count) {
    require(body.size() <= 8192, "Path is too long");
    std::vector<std::string> result;
    std::size_t offset = 0;
    while (offset < body.size()) {
        auto end = body.find('\0', offset);
        require(end != std::string_view::npos, "Path must be NUL terminated");
        result.emplace_back(body.substr(offset, end - offset)); offset = end + 1;
    }
    require(result.size() == count, "Invalid path payload");
    return result;
}
std::vector<std::string> components(const std::string &path) {
    require(path.size() <= 4096 && (path.empty() || path.front() != '/'), "Use a relative path");
    std::vector<std::string> result;
    for (std::size_t start = 0; start < path.size();) {
        auto end = path.find('/', start);
        if (end == std::string::npos) end = path.size();
        std::string name = path.substr(start, end - start);
        require(!name.empty() && name != "." && name != ".." && name.size() <= 255
            && name.find('\\') == std::string::npos && name.rfind(".upload-", 0) != 0, "Invalid path component");
        for (unsigned char ch : name) require(ch >= 32 && ch != 127, "Control character in path");
        result.push_back(std::move(name)); start = end + 1;
    }
    require(path.empty() || path.back() != '/', "Invalid trailing slash");
    return result;
}
int directory(int root, const std::vector<std::string> &parts, std::size_t count) {
    Fd current(::openat(root, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    for (std::size_t i = 0; i < count; ++i) {
        Fd next(::openat(current.value, parts[i].c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        ::close(current.value); current.value = next.release();
    }
    return current.release();
}
std::string userDirectory(const std::string &name) {
    constexpr char hex[] = "0123456789abcdef";
    std::string result = "u-";
    for (unsigned char ch : name) { result += hex[ch >> 4]; result += hex[ch & 15]; }
    return result;
}
struct stat entryStat(int parent, const std::string &name) {
    struct stat info{};
    checked(::fstatat(parent, name.c_str(), &info, AT_SYMLINK_NOFOLLOW));
    require(S_ISREG(info.st_mode) || S_ISDIR(info.st_mode), "Only regular files and directories are supported");
    return info;
}
std::uint64_t sizeValue(const std::string &text) {
    std::uint64_t size = 0;
    auto parsed = std::from_chars(text.data(), text.data() + text.size(), size);
    require(!text.empty() && parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size()
        && size <= kMaxFileSize, "File exceeds the 256 MiB limit or has an invalid size");
    return size;
}
}
FileSession::~FileSession() { cancel(); }
void FileSession::cancel() {
    if (fd >= 0) ::close(fd);
    if (parent >= 0) {
        if (!temporary.empty()) ::unlinkat(parent, temporary.c_str(), 0);
        ::close(parent);
    }
    fd = parent = -1; temporary.clear(); destination.clear();
    total = transferred = 0; uploading = false;
}
FileStore::FileStore(const std::string &path) {
    std::filesystem::create_directories(path);
    root_ = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (root_ < 0) throw std::runtime_error("Cannot open file storage root");
}
FileStore::~FileStore() { if (root_ >= 0) ::close(root_); }
Frame FileStore::handle(const std::string &user, FileSession &session, const Frame &request) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto &head = *header(request);
    const std::string op = field(head, 0);
    try {
        require(!user.empty() && hasTerminatedField(head, 0) && hasTerminatedField(head, 32), "Invalid file request");
        require(head.messageLength == request.size() - sizeof(LegacyHeader)
            && head.messageLength <= kFileChunkSize, "Invalid file payload length");
        const std::string_view body(reinterpret_cast<const char *>(request.data() + sizeof(LegacyHeader)), head.messageLength);
        if (op == "cancel") { require(body.empty(), "Unexpected payload"); session.cancel(); return response(op, "ok"); }
        if (op == "chunk") {
            require(session.fd >= 0 && session.uploading, "No upload is active");
            require(!body.empty() && body.size() <= session.total - session.transferred, "Upload length mismatch");
            for (std::size_t offset = 0; offset < body.size();) {
                ssize_t n = ::write(session.fd, body.data() + offset, body.size() - offset);
                if (n < 0 && errno == EINTR) continue;
                require(n > 0, "Cannot write upload (check disk space)"); offset += static_cast<std::size_t>(n);
            }
            session.transferred += body.size(); return response(op, "ok");
        }
        if (op == "commit") {
            require(body.empty() && session.fd >= 0 && session.uploading, "No upload is active");
            require(session.transferred == session.total, "Upload is incomplete");
            checked(::fsync(session.fd));
            // Atomic publication with no overwrite; incomplete files stay hidden.
            checked(::linkat(session.parent, session.temporary.c_str(), session.parent, session.destination.c_str(), 0));
            session.cancel(); return response(op, "ok");
        }
        if (op == "read") {
            require(body.empty() && session.fd >= 0 && !session.uploading, "No download is active");
            if (session.transferred == session.total) { session.cancel(); return response(op, "done"); }
            std::array<char, kFileChunkSize> buffer{};
            auto amount = static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), session.total - session.transferred));
            ssize_t n;
            do { n = ::read(session.fd, buffer.data(), amount); } while (n < 0 && errno == EINTR);
            require(n > 0, "File changed or cannot be read");
            session.transferred += static_cast<std::uint64_t>(n);
            return response(op, "more", std::string_view(buffer.data(), static_cast<std::size_t>(n)));
        }
        require(session.fd < 0, "Finish or cancel the current transfer first");
        const std::string folder = userDirectory(user);
        if (::mkdirat(root_, folder.c_str(), 0700) < 0 && errno != EEXIST) checked(-1);
        Fd userRoot(::openat(root_, folder.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        auto args = paths(body, op == "rename" ? 2 : 1);
        auto parts = components(args[0]);
        if (op == "list") {
            Fd dirFd(directory(userRoot.value, parts, parts.size()));
            DIR *dir = ::fdopendir(dirFd.value);
            require(dir != nullptr, "Cannot list directory"); dirFd.release();
            struct DirCloser { void operator()(DIR *p) const { ::closedir(p); } };
            std::unique_ptr<DIR, DirCloser> owner(dir);
            std::vector<std::pair<std::string, char>> entries;
            std::size_t bytes = 0;
            errno = 0;
            while (auto *entry = ::readdir(dir)) {
                std::string name = entry->d_name;
                if (name == "." || name == ".." || name.rfind(".upload-", 0) == 0) continue;
                struct stat info{};
                checked(::fstatat(::dirfd(dir), name.c_str(), &info, AT_SYMLINK_NOFOLLOW));
                if (!S_ISREG(info.st_mode) && !S_ISDIR(info.st_mode)) continue;
                bytes += name.size() + 2; require(bytes <= 256U * 1024U, "Directory listing is too large");
                entries.emplace_back(std::move(name), S_ISDIR(info.st_mode) ? 'D' : 'F'); errno = 0;
            }
            checked(errno ? -1 : 0);
            std::sort(entries.begin(), entries.end());
            std::string listing;
            for (const auto &entry : entries) { listing += entry.second; listing += entry.first; listing += '\0'; }
            return response(op, "ok", listing);
        }
        require(!parts.empty(), "Cannot modify the user root directory");
        Fd parent(directory(userRoot.value, parts, parts.size() - 1));
        const std::string &name = parts.back();
        if (op == "mkdir") checked(::mkdirat(parent.value, name.c_str(), 0700));
        else if (op == "rmdir" || op == "remove") {
            auto info = entryStat(parent.value, name);
            require(op == "rmdir" ? S_ISDIR(info.st_mode) : S_ISREG(info.st_mode), "Entry type mismatch");
            checked(::unlinkat(parent.value, name.c_str(), op == "rmdir" ? AT_REMOVEDIR : 0));
        } else if (op == "rename") {
            entryStat(parent.value, name);
            auto target = components(args[1]); require(!target.empty(), "Cannot replace the root directory");
            Fd targetParent(directory(userRoot.value, target, target.size() - 1));
            checked(static_cast<int>(::syscall(SYS_renameat2, parent.value, name.c_str(), targetParent.value,
                target.back().c_str(), RENAME_NOREPLACE)));
        } else if (op == "upload") {
            auto total = sizeValue(field(head, 32));
            struct stat existing{};
            if (::fstatat(parent.value, name.c_str(), &existing, AT_SYMLINK_NOFOLLOW) == 0)
                throw std::runtime_error("Destination already exists");
            require(errno == ENOENT, "Cannot access destination");
            static std::atomic<std::uint64_t> sequence{0};
            int upload = -1; std::string temp;
            for (int attempt = 0; attempt < 100; ++attempt) {
                temp = ".upload-" + std::to_string(::getpid()) + "-" + std::to_string(++sequence);
                upload = ::openat(parent.value, temp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
                if (upload >= 0 || errno != EEXIST) break;
            }
            Fd file(upload);
            session.temporary = temp; session.destination = name;
            session.parent = parent.release(); session.fd = file.release();
            session.uploading = true; session.total = total; session.transferred = 0;
        } else if (op == "download") {
            Fd file(::openat(parent.value, name.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
            struct stat info{}; checked(::fstat(file.value, &info));
            require(S_ISREG(info.st_mode) && info.st_size >= 0
                && static_cast<std::uint64_t>(info.st_size) <= kMaxFileSize, "Unsupported file or file too large");
            session.fd = file.release(); session.total = static_cast<std::uint64_t>(info.st_size);
            session.transferred = 0; session.uploading = false;
            return response(op, "ok", std::to_string(session.total));
        } else throw std::runtime_error("Unsupported file operation");
        return response(op, "ok");
    } catch (const std::exception &error) {
        session.cancel(); return response(op, "error", error.what());
    }
}
}

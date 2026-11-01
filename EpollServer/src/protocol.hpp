#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace chat {

constexpr std::uint32_t kMaxFrameLength = 16U * 1024U * 1024U;
constexpr std::uint32_t kMaxChatMessageLength = 64U * 1024U;
constexpr std::size_t kNameFieldLength = 32;

enum class MessageType : std::uint32_t {//协议类型
    RegisterRequest = 1,
    RegisterResponse,
    LoginRequest,
    LoginResponse,
    AllOnlineRequest,
    AllOnlineResponse,
    SearchRequest,
    SearchResponse,
    AddFriendRequest,
    AddFriendResponse,
    AddFriendAgree,
    AddFriendRefuse,
    FlushFriendRequest,
    FlushFriendResponse,
    DeleteFriendRequest,
    DeleteFriendResponseOk,
    DeleteFriendResponseFailed,
    PrivateChatRequest,
    PrivateChatResponse,
    GroupChatRequest,
    GroupChatResponse,
    FileRequest = 100,
    FileResponse = 101,
    VideoRequest = 200,
    VideoResponse = 201,
    VideoError = 202,
};

// Compatibility wire header for the existing Qt client. Validated on
// little-endian x86_64 and ARMv7 with the fixed-width layout below.
// A versioned network-byte-order codec is planned
// as the next protocol migration and should replace this legacy layout.
struct LegacyHeader {
    std::uint32_t totalLength = 0;//总长度
    std::uint32_t messageType = 0;//消息类型
    std::array<char, 64> data{};
    std::uint32_t messageLength = 0;//消息体长度
};
static_assert(sizeof(LegacyHeader) == 76, "legacy protocol layout changed");

using Frame = std::vector<std::uint8_t>;

inline LegacyHeader *header(Frame &frame)
{
    return reinterpret_cast<LegacyHeader *>(frame.data());
}

inline const LegacyHeader *header(const Frame &frame)
{
    return reinterpret_cast<const LegacyHeader *>(frame.data());
}

inline MessageType messageType(const Frame &frame)
{
    return static_cast<MessageType>(header(frame)->messageType);
}

inline std::string field(const LegacyHeader &value, std::size_t offset)
{
    const char *begin = value.data.data() + offset;
    const void *end = std::memchr(begin, '\0', kNameFieldLength);
    if (!end) return {};
    return std::string(begin, static_cast<const char *>(end));
}

inline void setField(LegacyHeader &value, std::size_t offset,
                     std::string_view text)
{
    const std::size_t size = std::min(text.size(), kNameFieldLength - 1);
    std::memcpy(value.data.data() + offset, text.data(), size);
}

inline Frame makeFrame(MessageType type, std::string_view first = {},
                       std::string_view second = {},
                       const std::uint8_t *payload = nullptr,
                       std::size_t payloadSize = 0)
{
    Frame result(sizeof(LegacyHeader) + payloadSize, 0);
    auto &value = *header(result);
    value.totalLength = static_cast<std::uint32_t>(result.size());
    value.messageType = static_cast<std::uint32_t>(type);
    value.messageLength = static_cast<std::uint32_t>(payloadSize);
    setField(value, 0, first);
    setField(value, kNameFieldLength, second);
    if (payloadSize != 0)
        std::memcpy(result.data() + sizeof(LegacyHeader), payload, payloadSize);
    return result;
}

inline Frame makeFrame(MessageType type, std::string_view first,
                       std::string_view second, std::string_view payload)
{
    return makeFrame(type, first, second,
                     reinterpret_cast<const std::uint8_t *>(payload.data()),
                     payload.size());
}

class FrameDecoder {
public:
    enum class Result { NeedMore, FrameReady, Invalid };

    void append(const std::uint8_t *data, std::size_t size)
    {
        buffer_.insert(buffer_.end(), data, data + size);
    }

    Result next(Frame &result)
    {
        if (buffer_.size() < sizeof(std::uint32_t)) return Result::NeedMore;
        std::uint32_t length = 0;
        std::memcpy(&length, buffer_.data(), sizeof(length));
        if (length < sizeof(LegacyHeader) || length > kMaxFrameLength)
            return Result::Invalid;
        if (buffer_.size() < length) return Result::NeedMore;

        result.assign(buffer_.begin(), buffer_.begin() + length);
        buffer_.erase(buffer_.begin(), buffer_.begin() + length);
        const auto &value = *header(result);
        if (value.totalLength != length
                || value.messageLength > length - sizeof(LegacyHeader))
            return Result::Invalid;
        return Result::FrameReady;
    }

    std::size_t bufferedBytes() const { return buffer_.size(); }

private:
    std::vector<std::uint8_t> buffer_;
};

inline bool hasTerminatedField(const LegacyHeader &value, std::size_t offset)
{
    return std::memchr(value.data.data() + offset, '\0', kNameFieldLength) != nullptr;
}

} // namespace chat

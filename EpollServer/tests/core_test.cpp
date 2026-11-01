#include "flow_control.hpp"
#include "protocol.hpp"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>

using namespace chat;

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            std::cerr << "check failed: " #condition << " at " << __FILE__   \
                      << ':' << __LINE__ << '\n';                              \
            return EXIT_FAILURE;                                               \
        }                                                                      \
    } while (false)

int main()
{
    const std::string body("hello\0world", 11);
    const Frame packet = makeFrame(MessageType::PrivateChatRequest,
                                   "alice", "bob", body);
    FrameDecoder decoder;
    Frame decoded;
    for (std::size_t i = 0; i < packet.size(); ++i) {
        const std::uint8_t byte = packet[i];
        decoder.append(&byte, 1);
        const auto result = decoder.next(decoded);
        if (i + 1 != packet.size())
            CHECK(result == FrameDecoder::Result::NeedMore);
        else
            CHECK(result == FrameDecoder::Result::FrameReady);
    }
    CHECK(decoded == packet);
    CHECK(field(*header(decoded), 0) == "alice");
    CHECK(field(*header(decoded), 32) == "bob");

    FrameDecoder combined;
    Frame twoPackets = packet;
    twoPackets.insert(twoPackets.end(), packet.begin(), packet.end());
    combined.append(twoPackets.data(), twoPackets.size());
    CHECK(combined.next(decoded) == FrameDecoder::Result::FrameReady);
    CHECK(combined.next(decoded) == FrameDecoder::Result::FrameReady);
    CHECK(combined.next(decoded) == FrameDecoder::Result::NeedMore);

    std::uint32_t invalidLength = 1;
    FrameDecoder invalid;
    invalid.append(reinterpret_cast<const std::uint8_t *>(&invalidLength),
                   sizeof(invalidLength));
    CHECK(invalid.next(decoded) == FrameDecoder::Result::Invalid);

    TokenBucket bucket(10.0, 2.0);
    const auto start = TokenBucket::Clock::now();
    CHECK(bucket.consume(start));
    CHECK(bucket.consume(start));
    CHECK(!bucket.consume(start));
    CHECK(bucket.consume(start + std::chrono::milliseconds(100)));

    OutboundBudget budget(1024);
    CHECK(budget.canQueue(0, 1024));
    CHECK(budget.canQueue(100, 924));
    CHECK(!budget.canQueue(100, 925));
    CHECK(!budget.canQueue(0, 0));

    std::cout << "epoll core tests passed\n";
    return 0;
}

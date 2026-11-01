#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>

namespace chat {

class TokenBucket {
public:
    using Clock = std::chrono::steady_clock;

    TokenBucket(double ratePerSecond = 100.0, double burst = 200.0)
        : ratePerSecond_(ratePerSecond), burst_(burst), tokens_(burst),
          lastRefill_(Clock::now())
    {
    }

    bool consume(Clock::time_point now = Clock::now())
    {
        const std::chrono::duration<double> elapsed = now - lastRefill_;
        tokens_ = std::min(burst_, tokens_ + elapsed.count() * ratePerSecond_);
        lastRefill_ = now;
        if (tokens_ < 1.0) return false;
        tokens_ -= 1.0;
        return true;
    }

private:
    double ratePerSecond_;
    double burst_;
    double tokens_;
    Clock::time_point lastRefill_;
};

class OutboundBudget {
public:
    explicit OutboundBudget(std::size_t highWater = 1024U * 1024U)
        : highWater_(highWater)
    {
    }

    bool canQueue(std::size_t pending, std::size_t payload) const
    {
        return payload != 0 && payload <= highWater_
                && pending <= highWater_ - payload;
    }

    std::size_t highWater() const { return highWater_; }

private:
    std::size_t highWater_;
};

} // namespace chat

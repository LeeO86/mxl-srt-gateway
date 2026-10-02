#pragma once

#include "config/config.hpp"
#include "srt/access.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

namespace srtgw
{
struct SrtStats
{
    double rttMs = 0;
    std::uint64_t pktLoss = 0;
    std::uint64_t pktRetrans = 0;
    std::uint64_t pktDrop = 0;
    double bitrateBps = 0;
    double bufferMs = 0;
    bool connected = false;
    std::string peer;
    std::string streamId;
};

void interruptSrt();
bool srtInterrupted();

class SrtLink
{
public:
    SrtLink();
    ~SrtLink();

    SrtLink(SrtLink const&) = delete;
    SrtLink& operator=(SrtLink const&) = delete;

    void configure(SrtEndpointConfig config, std::string channelId);
    bool establish(std::atomic<bool> const& stop, AttemptLimiter& limiter);
    void closeData();
    void closeAll();
    int recv(std::uint8_t* data, int size);
    int send(std::uint8_t const* data, int size);
    void rejectPending(AttemptLimiter& limiter);
    [[nodiscard]] SrtStats stats() const;
    [[nodiscard]] bool connected() const;

private:
    bool openSocket(bool listener);
    bool acceptOne(std::atomic<bool> const& stop, AttemptLimiter& limiter);
    void applyOptions(int fd, bool caller) const;
    void logReject(std::string const& peer, std::string const& reason) const;

    SrtEndpointConfig config_;
    std::string channelId_;
    int listenFd_ = -1;
    int dataFd_ = -1;
    mutable std::mutex mutex_;
    SrtStats stats_;
};
} // namespace srtgw

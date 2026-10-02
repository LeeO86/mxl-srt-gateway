#pragma once

#include "config/config.hpp"
#include "mxl/domain.hpp"
#include "ops/metrics.hpp"
#include "srt/socket.hpp"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace srtgw
{
struct TrackStatus
{
    int pid = 0;
    std::string codec;
    std::string layout;
    std::string language;
    bool missing = false;
};

struct PipelineStatus
{
    std::string state = "idle";
    std::string error;
    std::string peer;
    SrtStats srt;
    std::string sourceFormat;
    std::string targetFormat;
    std::string decoder;
    std::string encoder;
    std::uint64_t frames = 0;
    std::uint64_t repeats = 0;
    std::uint64_t drops = 0;
    double driftPpm = 0;
    double fifoMs = 0;
    std::uint64_t decodeErrors = 0;
    double decodeFps = 0;
    double encodeFps = 0;
    bool failover = false;
    std::string timecode;
    std::vector<double> meters;
    std::vector<TrackStatus> tracks;
    std::vector<std::string> alarms;
    std::vector<std::uint8_t> jpeg;
    std::uint64_t encodeLatencyCount = 0;
    double encodeLatencySum = 0;
    std::vector<std::uint64_t> latencyBuckets = std::vector<std::uint64_t>(8, 0);
    std::string request;
};

struct FlowRoute
{
    bool active = false;
    std::string domainId;
    std::string flowId;
    bool mirror = false;
};

class IngestPipeline
{
public:
    IngestPipeline(ChannelConfig config, std::shared_ptr<MxlDomain> domain, std::string videoFlow, std::vector<std::string> audioFlows, std::string decoder);
    ~IngestPipeline();
    void start();
    void stop();
    [[nodiscard]] PipelineStatus status() const;

private:
    void runIo();
    void runClock();
    void runLink(bool backup);
    void publish(PipelineStatus const& status);
    friend int readActive(void* opaque, std::uint8_t* buf, int bufSize);

    ChannelConfig config_;
    std::shared_ptr<MxlDomain> domain_;
    std::string videoFlow_;
    std::vector<std::string> audioFlows_;
    std::string decoderPref_;
    std::atomic<bool> stop_{false};
    mutable std::mutex statusMu_;
    PipelineStatus status_;
    std::mutex mediaMu_;
    struct Shared;
    std::unique_ptr<Shared> shared_;
    std::thread io_;
    std::thread clock_;
    std::thread mainLink_;
    std::thread backupLink_;
};

class EgressPipeline
{
public:
    using DomainFn = std::function<std::shared_ptr<MxlDomain>(std::string const& domainId, bool* mirror)>;
    using RouteFn = std::function<FlowRoute(bool video)>;

    EgressPipeline(ChannelConfig config, std::string encoder, DomainFn domains, RouteFn routes);
    ~EgressPipeline();
    void start();
    void stop();
    [[nodiscard]] PipelineStatus status() const;

private:
    void run();

    ChannelConfig config_;
    std::string encoderPref_;
    DomainFn domains_;
    RouteFn routes_;
    std::atomic<bool> stop_{false};
    mutable std::mutex statusMu_;
    PipelineStatus status_;
    std::thread thread_;
    class SrtLink* livePrimary_ = nullptr;
    class SrtLink* liveCopy_ = nullptr;
    friend int writeEgress(void* opaque, std::uint8_t* buf, int size);
    friend int writeEgress(void* opaque, std::uint8_t const* buf, int size);
};
} // namespace srtgw

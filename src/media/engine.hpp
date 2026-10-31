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
    // Egress: readers opened again because their MXL flow was re-created, or its head stalled.
    std::uint64_t flowReopensRecreated = 0;
    std::uint64_t flowReopensStalled = 0;
    // Egress: output frames that repeated the last good source frame (its grain missing, invalid
    // or late), frames sent as the slate while routed (no good grain for longer than hold_ms, or
    // none yet), and grains whose audio the open flow did not have in time (sent as silence).
    std::uint64_t repeatedFrames = 0;
    std::uint64_t slateFrames = 0;
    std::uint64_t audioSilentGrains = 0;
    // Ingest: re-anchors of the video mapping (a step of its PTS offset) and of the audio
    // (a step against the video, or a hold beyond av_resync_ms), and the last one's size (ms,
    // + later, - earlier).
    std::uint64_t avResyncsVideo = 0;
    std::uint64_t avResyncsAudio = 0;
    double avResyncJumpVideoMs = 0;
    double avResyncJumpAudioMs = 0;
    double driftPpm = 0;
    double fifoMs = 0;
    std::uint64_t decodeErrors = 0;
    double decodeFps = 0;
    // Video codec delay (ms), smoothed: ingest from a packet's arrival to its picture, egress from a
    // frame into the encoder to its packet.
    double codecDelayMs = 0;
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
    // Counts activations: a new one opens the flow again even with the same flow id.
    std::uint64_t generation = 0;
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
